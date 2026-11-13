/* par_load: measure (and later parallelize) the serial game-init steps of Victoria 3 1.15.1.
 * RE notes: docs/re/par_load.md. Offline test: test/par_load_test.c.
 *
 * Game init (gameapplication.cpp "Initializing map and databases...", exe+0x6aff80 in 1.15.1) runs
 * on ONE task worker and calls, in this order (all direct calls, resolved below without
 * hardcoded addresses, every anchor fail-closed = that hook is simply not installed):
 *   tex_lookup   "gfx/models" (pdxassetutil.cpp ThreadedInitTextureLookup)          0x3ad4300
 *   gui_load     the game-init callee that calls GUI asset loading
 *                ("Loading of GUI Asset \"{}\"", 0x3924b10) - every .gui file       0x39247e0
 *   gui_validate the game-init callee that calls the "gui_validation" function     0x38a3b20
 *   asset_load   .asset files (entities, pdxmesh types, blend shapes), called twice 0x3794de0
 *   db_init      "occupation_cost" (game databases: two ready-loop lists)           0x6b21a0
 *   achievements "gui/achievements/popup.gui"                                       0x2dd29c0
 *   render_ls    "RenderLoadingScreen" (6 calls from game init)                     0x1021ff0
 * plus, anywhere inside: db_loop "Failed to initialize some databases!" 0x3dbb980 (the serial
 * database ready-loop; every database callback is timed with a generated thunk), vfs_open
 * "VFSOpen Error: " 0x3e1c4f0, lexer_file (the asset_load callee that calls the lexer
 * "Could not create lexer from file" 0x3e1e070) 0x3e1e490, blendshape (sole caller of the
 * blend-shape loader "Blendshape file {} not found..." 0x3ab47d0) 0x3ab67c0, asset_read (sole
 * caller of the pdx_asset reader core "Asset not saved as BINARY: {}" 0x3dd1260) 0x3dd1190 - also
 * counts distinct vs total paths it reads on the init thread (duplicate file reads).
 *
 * mode=off      nothing.
 * mode=measure  (default) hook_safe timing only; no behaviour change. Report:
 *               binaries/v3x/par_load_report.txt after every load transition, on exit and with the
 *               console command `parload`.
 * mode=on       reserved for proven-safe parallel steps. In this build NO step is proven (see
 *               docs/re/par_load.md section 2), so `on` logs that and runs as measure. pl_forjoin
 *               (ordered fork/join, offline determinism test) is the building block. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "v3x.h"
#include "v3x_scan.h"
#include "v3x_anchor.h"
#include "v3x_hook.h"
#include "v3x_ini.h"
#include "v3x_mem.h"
#include "pl_core.h"

V3X_EXPORT const char *v3x_plugin_name = "par_load";
V3X_PLUGIN_MANIFEST_EX("par_load", "0.1.0", "v3x", "game-init step timing (parallel load groundwork)",
                       "release/1.15.1", 0u, 4u);

static const V3X_API *api;
static pl_stats S;
static pl_dbtab T;
static pl_pathset P;                       /* paths read by asset_read on the init thread */
static int g_mode = PL_MODE_MEASURE, g_db_times = 1, g_top_db = 40, g_bs_cache = 0, g_window_mb = 64;
static pl_pf R;                            /* recording of this run's blend-step reads */
static pl_pf F;                            /* prefetch list of the previous run (blendshape_cache=1) */
static int g_pf_loaded, g_rec_saved, g_rec_resolved = -1;
static void *g_rd_ret;                     /* return address of the blend loader's getRealDir call */
static volatile long g_in_blend;           /* init thread inside the blend-shape step */
static volatile long g_bs_ord;             /* blend-step reads started by the engine (init thread) */
static HANDLE g_pf_thread;
static SRWLOCK g_rep_lock = SRWLOCK_INIT;

/* ------------------------------------------------------------------ resolution */

enum { A_INIT, A_TEX, A_GUI, A_GUIVAL, A_ASSET, A_DBINIT, A_ACH, A_RLS, A_DBLOOP, A_VFS, A_LEX, A_BLEND, A_AREAD, A_REALDIR, A_N };
static const char *A_NAME[A_N] = {"game_init", "tex_lookup", "gui_load", "gui_validate", "asset_load", "db_init",
                                  "achievements", "render_ls", "db_loop", "vfs_open", "lexer_file", "blendshape", "asset_read", "realdir"};
static void *g_fn[A_N];
static char g_err[A_N][120];

/* [start, end) of the .pdata function that starts exactly at fn (0 if fn is not a function start) */
static uint8_t *fn_end(const void *fn)
{
    uint8_t *base = (uint8_t *)api->image_base();
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *ex = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    IMAGE_RUNTIME_FUNCTION_ENTRY *rf = (IMAGE_RUNTIME_FUNCTION_ENTRY *)(base + ex->VirtualAddress);
    size_t lo = 0, hi = ex->Size / sizeof *rf;
    DWORD rva = (DWORD)((const uint8_t *)fn - base);
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (rf[m].BeginAddress < rva) lo = m + 1;
        else if (rf[m].BeginAddress > rva) hi = m;
        else return base + rf[m].EndAddress;
    }
    return NULL;
}

static int body(const void *fn, V3xSection *b)
{
    uint8_t *e = fn ? fn_end(fn) : NULL;
    if (!e || e <= (uint8_t *)fn || e - (uint8_t *)fn > 0x40000) return 0;
    b->ptr = (uint8_t *)fn; b->size = (size_t)(e - (uint8_t *)fn);
    return 1;
}

/* distinct direct-call targets of fn that are .pdata function starts */
static int callees(const void *fn, void **out, int max, int *count_of /* per out: call sites */)
{
    V3xSection b, text;
    if (!body(fn, &b) || !v3x_section(api->image_base(), ".text", &text)) return 0;
    int n = 0;
    for (size_t i = 0; i + 5 <= b.size; i++) {
        if (b.ptr[i] != 0xE8) continue;
        uint8_t *t = v3x_rel32_target(b.ptr + i);
        if (t < text.ptr || t >= text.ptr + text.size || api->function_start(t) != t || !fn_end(t)) continue;
        int k = 0;
        while (k < n && out[k] != t) k++;
        if (k == n) { if (n == max) continue; out[n] = t; if (count_of) count_of[n] = 0; n++; }
        if (count_of) count_of[k]++;
    }
    return n;
}

static int calls(const void *fn, const void *target)
{
    V3xSection b;
    return target && body(fn, &b) && v3x_callers(&b, target, NULL, 0) > 0;
}

static int refs_addr(const void *fn, const void *a)
{
    V3xSection b;
    return a && body(fn, &b) && v3x_xrefs(&b, a, NULL, 0) > 0;
}

static int has_prologue(const void *fn, const char *pat)
{
    V3xPattern p;
    return fn && v3x_pattern_compile(pat, &p) && v3x_pattern_match_at(&p, (const uint8_t *)fn);
}

/* function anchored by a string that only one function references, with a prologue check */
static void *by_string(int id, const char *s, const char *pro)
{
    V3xAnchor a = {A_NAME[id], s, pro, V3X_ANCHOR_ALL_SAME, 0, NULL, NULL, NULL, {0}};
    if (v3x_anchor_resolve(api, &a)) { snprintf(g_err[id], sizeof g_err[id], "%s", a.err); return NULL; }
    return a.result;
}

static void *fail(int id, const char *why) { snprintf(g_err[id], sizeof g_err[id], "%s", why); return NULL; }

/* Resolves every anchor (idempotent). Returns the number resolved; game_init missing = 0 hooks. */
static int pl_resolve(void)
{
    memset(g_fn, 0, sizeof g_fn);
    memset(g_err, 0, sizeof g_err);
    void *ini = g_fn[A_INIT] = by_string(A_INIT, "Initializing map and databases...", "40 55 53 56 57 41 54 41 55 41 56 41 57");
    void *kid[256]; int ncall[256];
    int nk = ini ? callees(ini, kid, 256, ncall) : 0;
    if (ini && nk < 20) { g_fn[A_INIT] = NULL; fail(A_INIT, "game init has too few callees"); ini = NULL; nk = 0; }

    /* direct children of game init, string-anchored */
    struct { int id; const char *s, *pro; } K[] = {
        {A_TEX, "gfx/models", "48 89 5C 24 10 48 89 74 24 18 55 57 41 56"},
        {A_DBINIT, "occupation_cost", "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57"},
        {A_ACH, "gui/achievements/popup.gui", "48 89 4C 24 08 53 55 56 57 41 56"},
        {A_RLS, "RenderLoadingScreen", "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 60"},
    };
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++) {
        void *f = by_string(K[i].id, K[i].s, K[i].pro);
        if (f && !(ini && calls(ini, f))) f = fail(K[i].id, "not called by game init");
        g_fn[K[i].id] = f;
    }
    /* gui_load: the game-init callee that calls the GUI asset loader and references ".gui" */
    void *gui_asset = by_string(A_GUI, "Loading of GUI Asset \"{}\"", "48 8B C4 48 89 58 08");
    void *val_fn = NULL;
    {
        V3xAnchor a = {"gui_validation", "gui_validation", NULL, V3X_ANCHOR_SHAPE, 0, NULL, NULL, NULL, {0}};
        a.pattern = "48 8B C4 48 89 58 18 88 50 10";      /* 2 functions reference the string; select by shape */
        if (!v3x_anchor_resolve(api, &a)) val_fn = a.result;
    }
    void *lexer_core = by_string(A_LEX, "Could not create lexer from file", "48 89 5C 24 18 48 89 4C 24 08");
    void *gui_str = api->find_string(".gui");
    int ngui = 0, nval = 0, nasset = 0;
    void *gui = NULL, *val = NULL, *asset = NULL;
    for (int i = 0; i < nk; i++) {
        if (gui_asset && calls(kid[i], gui_asset) && refs_addr(kid[i], gui_str)) { gui = kid[i]; ngui++; }
        if (val_fn && calls(kid[i], val_fn)) { val = kid[i]; nval++; }
        if (ncall[i] >= 2 && has_prologue(kid[i], "48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 88 54 24 10 55")) {
            void *gk[64]; int ng = callees(kid[i], gk, 64, NULL), hit = 0;
            for (int j = 0; j < ng; j++) hit += lexer_core && calls(gk[j], lexer_core);
            if (hit == 1) { asset = kid[i]; nasset++; }
        }
    }
    g_fn[A_GUI] = ngui == 1 ? gui : fail(A_GUI, gui_asset ? "need exactly 1 game-init callee calling the GUI loader" : g_err[A_GUI]);
    g_fn[A_GUIVAL] = nval == 1 ? val : fail(A_GUIVAL, "need exactly 1 game-init callee calling gui_validation");
    g_fn[A_ASSET] = nasset == 1 ? asset : fail(A_ASSET, "need exactly 1 asset loader (2 calls, prologue, lexer callee)");
    /* lexer_file: the asset loader's callee that calls the lexer core */
    if (g_fn[A_ASSET] && lexer_core) {
        void *gk[64]; int ng = callees(g_fn[A_ASSET], gk, 64, NULL), hit = 0; void *lf = NULL;
        for (int j = 0; j < ng; j++) if (calls(gk[j], lexer_core)) { lf = gk[j]; hit++; }
        g_fn[A_LEX] = hit == 1 ? lf : fail(A_LEX, "lexer-from-file callee not unique");
    } else if (!g_err[A_LEX][0]) fail(A_LEX, "needs asset_load");
    /* db_loop + its entry layout (0x38-byte entries: +0 name, +8/+0x20/+0x28 callbacks, +0x10 ready) */
    void *dl = by_string(A_DBLOOP, "Failed to initialize some databases!", "48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57");
    if (dl) {
        static const char *SHAPE[] = {"4C 63 79 0C", "4D 6B C7 38", "48 8B 44 33 10", "FF 54 33 08 FF 54 33 20",
                                      "48 8B 43 28 48 85 C0", "48 8B 0C 07"};
        V3xSection b;
        if (!body(dl, &b)) dl = fail(A_DBLOOP, "no .pdata range");
        for (size_t i = 0; dl && i < sizeof SHAPE / sizeof SHAPE[0]; i++)
            if (!v3x_scan_near(b.ptr, b.size, SHAPE[i])) dl = fail(A_DBLOOP, "entry layout changed (shape check)");
        if (dl) {   /* called twice by one helper (0x6b2de0 in 1.15.1, reached from db_init indirectly) */
            V3xSection text; void *site[4];
            v3x_section(api->image_base(), ".text", &text);
            int n = v3x_callers(&text, dl, site, 4);
            if (n != 2 || api->function_start(site[0]) != api->function_start(site[1]))
                dl = fail(A_DBLOOP, "expected exactly 2 call sites in one helper");
        }
    }
    g_fn[A_DBLOOP] = dl;
    g_fn[A_VFS] = by_string(A_VFS, "VFSOpen Error: ", "48 8B C4 48 89 58 08 48 89 70 18");
    /* blendshape: sole direct caller of the blend-shape loader */
    void *bl = by_string(A_BLEND, "Blendshape file {} not found. Relevant mesh file: {}", "48 8B C4 4C 89 40 18");
    if (bl) {
        V3xSection text; void *site[4];
        v3x_section(api->image_base(), ".text", &text);
        int n = v3x_callers(&text, bl, site, 4);
        void *f = n == 1 ? api->function_start(site[0]) : NULL;
        g_fn[A_BLEND] = f ? f : fail(A_BLEND, "blend-shape loader must have exactly 1 caller");
    }
    /* realdir: PHYSFS_getRealDir = the one function both the blend loader and VFSOpen call; the blend
     * loader calls it exactly once (its existence check of the blend-shape file, 0x3ab4848 in 1.15.1) */
    g_rd_ret = NULL;
    if (bl && g_fn[A_VFS]) {
        void *a[256], *b[256]; int na = callees(bl, a, 256, NULL), nb = callees(g_fn[A_VFS], b, 256, NULL), hit = 0;
        void *rd = NULL;
        for (int i = 0; i < na; i++)
            for (int j = 0; j < nb; j++)
                if (a[i] == b[j] && has_prologue(a[i], "40 55 56 57 41 54 41 55 41 56 41 57")) { rd = a[i]; hit++; }
        V3xSection bb; void *site[4];
        int ns = rd && body(bl, &bb) ? v3x_callers(&bb, rd, site, 4) : 0;
        if (hit == 1 && ns == 1) { g_fn[A_REALDIR] = rd; g_rd_ret = (uint8_t *)site[0] + 5; }
        else fail(A_REALDIR, "need 1 shared getRealDir callee with 1 call site in the blend loader");
    } else fail(A_REALDIR, "needs blend loader + vfs_open");
    /* asset_read: sole caller of the pdx_asset binary reader core ("Asset not saved as BINARY: {}") */
    void *ar = by_string(A_AREAD, "Asset not saved as BINARY: {}", "48 89 5C 24 08 48 89 74 24 18");
    if (ar) {
        V3xSection text; void *site[4];
        v3x_section(api->image_base(), ".text", &text);
        int n = v3x_callers(&text, ar, site, 4);
        void *f = n == 1 ? api->function_start(site[0]) : NULL;
        if (f && g_fn[A_BLEND] && !calls(g_fn[A_BLEND], f)) f = NULL;   /* the blend-shape step reads through it */
        g_fn[A_AREAD] = f ? f : fail(A_AREAD, "reader core must have exactly 1 caller, called by blendshape");
    }
    int ok = 0;
    for (int i = 0; i < A_N; i++) ok += g_fn[i] != NULL;
    return ok;
}

/* ------------------------------------------------------------------ hooks */

static int g_step_of[A_N];
static LARGE_INTEGER g_freq;
static long long now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

static DWORD WINAPI pf_main(LPVOID u)
{
    (void)u;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    pl_pf_run(&F);
    return 0;
}
static void pf_start(void)
{
    if (!g_bs_cache || !g_pf_loaded || g_pf_thread) return;
    F.window = (long long)g_window_mb << 20;
    g_pf_thread = CreateThread(NULL, 256 * 1024, pf_main, NULL, 0, NULL);
    api->log("par_load", "blendshape_cache: prefetching %d files (window %d MB)%s", F.n, g_window_mb,
             g_pf_thread ? "" : " FAILED to start");
}

static void step_pre(V3X_HookCall *c)
{
    int id = (int)(intptr_t)c->user;
    uint32_t tid = GetCurrentThreadId();
    if (id == A_INIT) { S.init_tid = tid; S.depth = 0; pf_start(); }
    c->scratch[0] = (uint64_t)now();
    pl_step_enter(&S, g_step_of[id], tid);
}
static void step_post(V3X_HookCall *c)
{
    int id = (int)(intptr_t)c->user;
    pl_step_leave(&S, g_step_of[id], GetCurrentThreadId(), now() - (long long)c->scratch[0]);
    if (id == A_ASSET || id == A_INIT) F.stop = 1;   /* the blend-shape reads happen in the first asset pass */
}
static void inner_pre(V3X_HookCall *c)
{
    c->scratch[0] = (uint64_t)now();
    /* asset_read(out, const {char *ptr; int64 len} *path): count distinct paths read by the init thread */
    if ((int)(intptr_t)c->user == A_AREAD && GetCurrentThreadId() == S.init_tid) {
        uint64_t sv[2]; char buf[512];
        if (v3x_mem_read((uintptr_t)c->args[1], sv, sizeof sv)) {
            size_t n = (size_t)(uint32_t)sv[1];
            if (n > 0 && n < sizeof buf && v3x_mem_read((uintptr_t)sv[0], buf, n)) {
                uint64_t h = pl_hash_path(buf, n);
                pl_pathset_add(&P, h);
                if (g_in_blend > 0) {             /* a blend-step read: record it, tell the prefetcher */
                    long k = g_bs_ord++;
                    pl_pf_rec_read(&R, buf, n);
                    if (g_pf_thread) pl_pf_engine_read(&F, k, h);
                }
            }
        }
    }
    if ((int)(intptr_t)c->user == A_BLEND && GetCurrentThreadId() == S.init_tid) g_in_blend++;
}
/* getRealDir(const {char *ptr; int len; ...} *path) -> the PhysFS DirHandle that has the file (this build
 * returns the handle, not dirName: 0x3dffeed `mov r12, rbx` with rbx = the search-path walk; next at +0x30,
 * funcs at +0x28 as in docs/re/modloading.md), so dirName = [ret+8]. Only the blend loader's call. */
static void realdir_pre(V3X_HookCall *c)
{
    c->scratch[1] = c->return_address == g_rd_ret && g_in_blend > 0 && GetCurrentThreadId() == S.init_tid;
}
static void realdir_post(V3X_HookCall *c)
{
    char root[520];
    uintptr_t dn = 0;
    if (!c->scratch[1] || !c->ret || !v3x_mem_read_ptr((uintptr_t)c->ret + 8, &dn) || !dn) return;
    /* only absolute native roots (a mounted archive gives no usable path: those files are not prefetched) */
    if (v3x_mem_read_cstr(dn, root, sizeof root) > 3 && root[1] == ':') pl_pf_rec_root(&R, root);
}
static void inner_post(V3X_HookCall *c)
{
    int id = (int)(intptr_t)c->user;
    int kind = id == A_VFS ? PL_IN_VFS : id == A_LEX ? PL_IN_LEXER : id == A_AREAD ? PL_IN_AREAD : PL_IN_BLEND;
    if (id == A_BLEND && GetCurrentThreadId() == S.init_tid && g_in_blend > 0) g_in_blend--;
    pl_inner_add(&S, kind, GetCurrentThreadId(), now() - (long long)c->scratch[0]);
}

/* db_loop: point each entry's callbacks at a timing thunk for the duration of the call */
typedef struct { uintptr_t slot_addr; uint64_t orig; } patch_t;
static patch_t g_patch[PL_DB_MAX];
static int g_npatch, g_db_active;

static void db_pre(V3X_HookCall *c)
{
    step_pre(c);
    c->scratch[1] = 0;
    if (!g_db_times || g_db_active || !pl_thunk_pool_size()) return;
    uintptr_t vec = (uintptr_t)c->args[0], data = 0;
    uint32_t cnt = 0;
    if (!v3x_mem_read_ptr(vec, &data) || !v3x_mem_read_u32(vec + 0xc, &cnt) || cnt == 0 || cnt > 512) return;
    if (!v3x_mem_readable(data, (size_t)cnt * 0x38)) return;
    int list = ++T.nlists;
    if (list <= 16) T.list_entries[list - 1] = (int)cnt;
    g_db_active = 1; g_npatch = 0; c->scratch[1] = 1;
    uintptr_t tb = api->image_base(); size_t ts = api->image_size();
    for (uint32_t i = 0; i < cnt; i++) {
        uintptr_t e = data + (uintptr_t)i * 0x38, name = *(uintptr_t *)e;
        for (int s = 0; s < PL_DB_NSLOTS; s++) {
            uintptr_t sa = e + (uintptr_t)PL_DB_SLOT_OFF[s];
            uint64_t f = *(uint64_t *)sa;
            if (!f || !v3x_mem_in_module(tb, ts, (uintptr_t)f, 1) || !v3x_mem_is_code((uintptr_t)f)) continue;
            long r = T.nrec;
            void *th = pl_thunk_at((int)r);
            if (!th || r >= PL_DB_MAX || g_npatch >= PL_DB_MAX) continue;
            pl_dbrec *d = &T.rec[r];
            memset(d, 0, sizeof *d);
            if (!name || v3x_mem_read_cstr(name, d->name, sizeof d->name) <= 0) snprintf(d->name, sizeof d->name, "<entry %u>", i);
            d->list = list; d->idx = (int)i; d->slot = s; d->orig = (void *)(uintptr_t)f;
            T.nrec = r + 1;
            g_patch[g_npatch].slot_addr = sa; g_patch[g_npatch].orig = f; g_npatch++;
            *(uint64_t *)sa = (uint64_t)(uintptr_t)th;
        }
    }
}
static void db_post(V3X_HookCall *c)
{
    long long dt = now() - (long long)c->scratch[0];
    if (c->scratch[1]) {
        /* the vector belongs to the caller and is still alive: put the engine's pointers back */
        for (int i = 0; i < g_npatch; i++) *(uint64_t *)g_patch[i].slot_addr = g_patch[i].orig;
        g_npatch = 0; g_db_active = 0;
        if (T.nlists >= 1 && T.nlists <= 16) T.list_ticks[T.nlists - 1] += dt;
    }
    pl_step_leave(&S, g_step_of[A_DBLOOP], GetCurrentThreadId(), dt);
}

/* ------------------------------------------------------------------ report */

static void write_report(const char *why)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%spar_load_report.txt", api->v3x_dir());
    AcquireSRWLockExclusive(&g_rep_lock);
    FILE *f = fopen(path, "w");
    if (f) {
        char hdr[160];
        snprintf(hdr, sizeof hdr, "mode=%s, %s", pl_mode_name(g_mode), why);
        pl_report(f, &S, &T, &P, hdr, g_top_db);
        fprintf(f, "\n# blend-shape reads (init thread, blend step): %d recorded; prefetch list from the last run: %s\n", R.n,
                g_pf_loaded ? "loaded" : "none");
        if (g_rec_resolved >= 0) fprintf(f, "  list written: %d entries, %d native paths exist\n", R.n, g_rec_resolved);
        if (g_pf_thread)
            fprintf(f, "  prefetch: %lld files / %.1f MB read, %lld failed; engine reads: %lld hit, %lld in flight, %lld miss, "
                       "%lld path mismatch, %lld beyond the list\n", F.files_read, F.bytes_read / 1048576.0, F.files_failed, F.hit,
                    F.inflight, F.miss, F.mismatch, F.beyond);
        fprintf(f, "\n# anchors\n");
        for (int i = 0; i < A_N; i++)
            fprintf(f, "  %-13s %s%s\n", A_NAME[i], g_fn[i] ? "ok" : "FAILED: ", g_fn[i] ? "" : g_err[i]);
        fclose(f);
    }
    ReleaseSRWLockExclusive(&g_rep_lock);
    if (S.nsteps)
        api->log("par_load", "report (%s): game init %.1f s, asset_load %.1f s (vfs %.1f s), gui %.1f s, db %.1f s -> %s", why,
                 S.steps[0].ticks / (double)S.freq, S.steps[g_step_of[A_ASSET]].ticks / (double)S.freq,
                 S.steps[g_step_of[A_ASSET]].in_ticks[PL_IN_VFS] / (double)S.freq,
                 S.steps[g_step_of[A_GUI]].ticks / (double)S.freq, S.steps[g_step_of[A_DBINIT]].ticks / (double)S.freq, path);
}

static void on_transition(const char *name, void *data, void *user)
{
    (void)name; (void)user;
    const V3X_LoadTransition *t = data;
    if (!g_rec_saved && R.n > 0) {          /* first load with blend-step reads: keep the list for the next launch */
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%spar_load_prefetch.txt", api->v3x_dir());
        int ok = 0;
        if (pl_pf_save(&R, path, &ok) > 0) { g_rec_saved = 1; g_rec_resolved = ok; }
    }
    char why[96];
    snprintf(why, sizeof why, "after %s->%s", t && t->from ? t->from : "?", t && t->to ? t->to : "?");
    write_report(why);
}
static void on_exit_ev(const char *name, void *data, void *user) { (void)name; (void)data; (void)user; write_report("process exit"); }
static void cmd_parload(const char *args, void *user) { (void)args; (void)user; write_report("console"); api->print("par_load: report written"); }

/* ------------------------------------------------------------------ load */

static void setup_steps(void)
{
    pl_stats_init(&S, g_freq.QuadPart);
    static const struct { int id; const char *what; int top; } ST[] = {
        {A_INIT, "game init (gameapplication.cpp), one task worker", 0},
        {A_TEX, "texture path lookup (gfx/models)", 1},
        {A_GUI, "all .gui files: parse + register templates/types", 1},
        {A_GUIVAL, "gui validation", 1},
        {A_ASSET, ".asset files (2 passes): entities, pdxmesh types, blend shapes", 1},
        {A_DBINIT, "game databases (2 ready-loop lists) + history", 1},
        {A_ACH, "achievements", 1},
        {A_RLS, "RenderLoadingScreen (6 calls, waits for the frame)", 1},
        {A_DBLOOP, "database ready-loop (inside db_init)", 0},
    };
    for (int i = 0; i < A_N; i++) g_step_of[i] = -1;
    for (size_t i = 0; i < sizeof ST / sizeof ST[0]; i++) g_step_of[ST[i].id] = pl_step_add(&S, A_NAME[ST[i].id], ST[i].what, ST[i].top);
    memset(&T, 0, sizeof T);
    pl_db_bind(&T);
}

static int install(void)
{
    int n = 0;
    for (int i = 0; i < A_N; i++) {
        if (!g_fn[i]) { api->log("par_load", "anchor %s FAILED: %s (not hooked)", A_NAME[i], g_err[i]); continue; }
        int inner = i == A_VFS || i == A_LEX || i == A_BLEND || i == A_AREAD;
        v3x_hook_fn pre = i == A_DBLOOP ? db_pre : i == A_REALDIR ? realdir_pre : inner ? inner_pre : step_pre;
        v3x_hook_fn post = i == A_DBLOOP ? db_post : i == A_REALDIR ? realdir_post : inner ? inner_post : step_post;
        int rc = v3x_hook_observe(api, g_fn[i], pre, post, (void *)(intptr_t)i);
        api->log("par_load", "hooked %s at exe+0x%llx (status %d)", A_NAME[i],
                 (unsigned long long)((uintptr_t)g_fn[i] - api->image_base()), rc);
        n += rc == 0;
    }
    return n;
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    if (!V3X_HAS(api, hook_safe)) { api->log("par_load", "needs API v4 hook_safe; inactive"); return 1; }
    QueryPerformanceFrequency(&g_freq);
    V3xIni ini;
    v3x_ini_load_plugin(&ini, api, "par_load");
    char mode[32];
    v3x_ini_str(&ini, "par_load", "mode", "measure", mode, sizeof mode);
    g_db_times = v3x_ini_int(&ini, "par_load", "db_times", 1);
    g_top_db = v3x_ini_int(&ini, "par_load", "top_db", 40);
    g_bs_cache = v3x_ini_int(&ini, "par_load", "blendshape_cache", 0);
    g_window_mb = v3x_ini_int(&ini, "par_load", "prefetch_window_mb", 64);
    if (g_window_mb < 4) g_window_mb = 4;
    if (g_window_mb > 1024) g_window_mb = 1024;
    v3x_ini_free(&ini);
    g_mode = pl_mode_parse(mode);
    if (g_mode < 0) { api->log("par_load", "mode=%s unknown, using measure", mode); g_mode = PL_MODE_MEASURE; }
    if (g_mode == PL_MODE_OFF) { api->log("par_load", "mode=off"); return 0; }
    if (g_mode == PL_MODE_ON)
        api->log("par_load", "mode=on: no parallel step is proven safe in this build (docs/re/par_load.md sec. 2); running as measure");
    setup_steps();
    pl_pf_reset(&R); pl_pf_reset(&F);
    int ok = pl_resolve();
    if (g_bs_cache) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%spar_load_prefetch.txt", api->v3x_dir());
        if (!g_fn[A_AREAD] || !g_fn[A_BLEND] || !g_fn[A_ASSET] || !g_fn[A_INIT])
            api->log("par_load", "blendshape_cache: anchors missing, off");
        else if (!(g_pf_loaded = pl_pf_load(&F, path) > 0))
            api->log("par_load", "blendshape_cache: no list yet (this run records it)");
    }
    if (!g_fn[A_INIT]) { api->log("par_load", "game init anchor failed (%s): inactive", g_err[A_INIT]); return 1; }
    if (g_db_times && g_fn[A_DBLOOP] && !pl_thunk_pool_create(PL_DB_MAX, (const void *)pl_db_dispatch)) {
        api->log("par_load", "thunk pool allocation failed: per-database times off");
        g_db_times = 0;
    }
    int n = install();
    api->on_event("load_transition", on_transition, NULL);
    api->on_event("process_exit", on_exit_ev, NULL);
    api->register_command("parload", "par_load: write binaries/v3x/par_load_report.txt", cmd_parload, NULL);
    api->log("par_load", "mode=%s: %d/%d anchors, %d hooks, db_times=%d", pl_mode_name(g_mode), ok, A_N, n, g_db_times);
    return 0;
}
