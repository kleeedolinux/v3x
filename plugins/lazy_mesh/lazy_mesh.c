/* lazy_mesh: load a pdxmesh type (CMeshType) only when something is about to use it.
 * See docs/re/lazy_mesh.md for the RE behind every hook. RVAs are for 1.15.1 and are only comments:
 * every target is found by a string / RTTI anchor and byte-checked, any mismatch = fail closed.
 *
 * Engine flow (verified statically):
 *   0x6aff80 -> 3D type pass 0x3ab3f10:
 *       IterateTypesThreaded 0x3ae4d80 -> CMeshType::Load 0x3adcfd0 (vtable slot 6) for every type, 8 workers
 *       then, serially, the animation import 0x3ae1400 for every type (consumes the +0x148 import list)
 *   entity types are parsed AFTER the pass and may read loaded data (0x3ad7860, 0x37a9520)
 *   everything renderable is a CMeshObject, built only by the ctor 0x3ad80a0(this, const CMeshType *)
 *
 * Hooks (all MinHook through api->hook):
 *   pass   0x3ab3f10  brackets the startup pass (g_pass_active)
 *   load   0x3adcfd0  in the pass: defer the type (lazy) or load it and time it
 *   anim   0x3ae1400  skip the import for a deferred type (keeps its import list for later)
 *   ctor   0x3ad80a0  choke point: load the type on demand before the mesh object reads it
 *   rdata  0x3ad7860  entity render-data copy from the type (entities with mesh overrides)
 *   blend  0x37a9520  entity blend-shape attribute lookup (registry filled by Load)
 * On-demand load = original Load + original anim import, once per type (lm_ensure, lm_core.c).
 *
 * Modes (v3x/lazy_mesh.ini): off (no hooks), measure (no behaviour change: records which types are
 * requested at the choke points, writes v3x/lazy_mesh_used.txt), lazy (defers types chosen by the rules,
 * loads them on demand). Console: lazymesh [status|dump|load <name>]. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "v3x.h"
#include "v3x_rtti.h"
#include "lm_core.h"

V3X_EXPORT const char *v3x_plugin_name = "lazy_mesh";
V3X_PLUGIN_MANIFEST("lazy_mesh", "0.5.0", "v3x", "loads a mesh type only when it is about to be used", "release/1.15.1");

typedef uintptr_t (*fn4)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
typedef void (*fn1)(void *);

static const V3X_API *api;
static lm_config g_cfg;
static lm_nameset g_eager;
static lm_table g_tab;
static LARGE_INTEGER g_freq, g_qpc0;
static volatile LONG g_pass_active, g_armed, g_dirty, g_deferred, g_lazy_loads, g_log_budget = 400, g_timeouts;
static double g_pass_t0, g_pass_end_ms;
static char g_report_path[MAX_PATH];
static ULONGLONG g_next_write;
#define MAX_PHASES 32
static char g_phase_names[MAX_PHASES][48];
static const char *g_phase_ptr[MAX_PHASES];
static volatile LONG g_phase = -1;
static lm_prev g_prev;                  /* previous session's lazy_mesh_used.txt (read before it is rewritten) */
static lm_nameset g_prevstart;          /* ... its types first requested by entity parsing */
static lm_stats g_stats; static SRWLOCK g_stats_lock = SRWLOCK_INIT;
static DWORD g_main_tid;                /* v3x_plugin_load runs on the game's main thread */
static volatile LONG g_prefetch_started, g_prefetch_stop;   /* stop: the game left the Game state */

/* ------------------------------------------------------------------ anchors (mirrored in tools/re_lazymesh_check.py) */

#define STR_LOAD  "gfx/models/mesh_not_found_fallback_object.mesh"
#define STR_ANIM  "Could not find animation set {} declared as import in mesh type {}"
#define STR_BLEND "Could not find blend shape \"{}\" for attribute \"{}\" in entity \"{}\""
#define RTTI_MESHTYPE   ".?AVCMeshType@NPdxMesh@@"
#define RTTI_MESHOBJECT ".?AVCMeshObject@NPdxMesh@@"
#define LOAD_VSLOT 6                                   /* +0x30 */

typedef struct { int off; const char *pat; } shape_t;
enum { A_LOAD, A_ANIM, A_PASS, A_CTOR, A_RDATA, A_BLEND, A_COUNT };
typedef struct { const char *name; shape_t shape[4]; uint8_t *fn; } anchor_t;
static anchor_t A[A_COUNT] = {
    { "load",  { { 0x00, "40 53 56 57 41 54 41 55 41 56 41 57" },
                 { 0x46, "4C 8B E9" },                          /* mov r13, rcx (this) */
                 { 0x72, "41 39 5D 4C 0F 85" } } },             /* cmp [r13+0x4c], ebx; jne -> already loaded */
    { "anim",  { { 0x00, "48 89 4C 24 08 55 53 56 57 41 54" },
                 { 0x2D, "48 8D B1 48 01 00 00" },              /* lea rsi, [rcx+0x148] import list */
                 { 0x44, "4C 39 61 38 0F 84" } } },             /* cmp [rcx+0x38], r12; je -> no mesh data */
    { "pass",  { { 0x00, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B DA E8" },
                 { 0x20, "E8" },                                /* call IterateTypesThreaded */
                 { 0x60, "48 8B 4B 28 E8" } } },                /* mov rcx,[bucket+0x28]; call anim import */
    { "ctor",  { { 0x00, "48 89 5C 24 20 48 89 4C 24 08 55 56 57 41 54 41" },
                 { 0x62, "48 89 51 50" },                       /* mov [this+0x50], type */
                 { 0x71, "48 8D 05 ?? ?? ?? ?? 48 89 01" } } }, /* lea rax, CMeshObject vftable; mov [rcx], rax */
    { "rdata", { { 0x00, "48 89 54 24 10 48 89 4C 24 08 53 41 56 41 57" },
                 { 0x13, "48 8B 1A 4C 8B FA 4C 8B F1" } } },    /* mov rbx,[rdx]; mov r15,rdx; mov r14,rcx(type) */
    { "blend", { { 0x00, "48 89 5C 24 18 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57" },
                 { 0x21, "4D 8B E9 49 8B F0 48 8B DA 4C 8B F1" } } },  /* mov rsi, r8 (type) */
};
#define SHAPE_SPAN 0x100
#define LOAD_RDATA_CALL "49 8D 95 D0 01 00 00 49 8B CD E8"   /* lea rdx,[r13+0x1d0]; mov rcx,r13; call rdata */
#define LOAD_BODY_SPAN 0x1C00

static uint8_t *g_text; static size_t g_text_n;

static int in_image(const void *p, size_t n)
{
    uintptr_t b = api->image_base(), a = (uintptr_t)p;
    return a >= b && a + n <= b + api->image_size();
}

static uint32_t rva(const void *p) { return (uint32_t)((uintptr_t)p - api->image_base()); }

static int shape_ok(const uint8_t *fn, const shape_t *sh)
{
    if (!fn || !in_image(fn, SHAPE_SPAN)) return 0;
    for (int i = 0; i < 4 && sh[i].pat; i++)
        if (!lm_match(fn, SHAPE_SPAN, (size_t)sh[i].off, sh[i].pat)) return 0;
    return 1;
}

static uint8_t *rel32(const uint8_t *ins, int len) { int32_t d; memcpy(&d, ins + len - 4, 4); return (uint8_t *)ins + len + d; }

static int find_text(void)
{
    uint8_t *b = (uint8_t *)api->image_base();
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)b;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return -1;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(b + dos->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(s[i].Name, ".text", 6)) { g_text = b + s[i].VirtualAddress; g_text_n = s[i].Misc.VirtualSize; return 0; }
    return -1;
}

static uint8_t *by_string(const char *str, const shape_t *sh)
{
    void *s = api->find_string(str);
    if (!s) return NULL;
    for (int i = 0; i < 16; i++) {
        void *x = api->find_xref(s, i);
        if (!x) break;
        uint8_t *f = api->function_start(x);
        if (shape_ok(f, sh)) return f;
    }
    return NULL;
}

/* Resolve every anchor; returns the number that failed. Logs each one. */
static int resolve_all(void)
{
    int bad = 0;
    for (int i = 0; i < A_COUNT; i++) A[i].fn = NULL;
    if (find_text()) { api->log("lazy_mesh", "FAIL-CLOSED: no .text section"); return A_COUNT; }

    /* load: string xref + shape + it must be slot 6 of the CMeshType vtable */
    uint8_t *f = by_string(STR_LOAD, A[A_LOAD].shape);
    V3xRtti r;
    void **vt_type = NULL, **vt_obj = NULL;
    /* absolute pointers are relative to the actual base in the running (relocated) game, and to the
     * header's ImageBase in an unrelocated offline mapping: try both */
    uintptr_t base = api->image_base();
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    uint64_t ptr_bases[2] = { base, nt->OptionalHeader.ImageBase };
    for (int pb = 0; pb < 2 && !vt_type; pb++) {
        if (pb == 1 && ptr_bases[1] == ptr_bases[0]) break;
        if (v3x_rtti_build_ex(&r, base, api->image_size(), ptr_bases[pb]) == 0) {
            vt_type = v3x_rtti_find_vtable(&r, RTTI_MESHTYPE);
            vt_obj = v3x_rtti_find_vtable(&r, RTTI_MESHOBJECT);
            /* absolute pointers in the image are relative to r.ptr_base; convert slot 6 to this mapping */
            if (vt_type && f) {
                uint64_t slot = (uint64_t)(uintptr_t)vt_type[LOAD_VSLOT];
                if ((uintptr_t)(slot - r.ptr_base + base) != (uintptr_t)f) f = NULL;
            }
            v3x_rtti_free(&r);
        }
    }
    if (!vt_type) f = NULL;
    A[A_LOAD].fn = f;

    /* anim import: string xref (two functions use it: the import and CMeshType::Reload) + shape */
    A[A_ANIM].fn = by_string(STR_ANIM, A[A_ANIM].shape);

    /* pass: the direct caller of the anim import, whose +0x64 call is that import */
    if (A[A_ANIM].fn && g_text) {
        for (size_t i = 0; i + 5 <= g_text_n; i++) {
            if (g_text[i] != 0xE8 || rel32(g_text + i, 5) != A[A_ANIM].fn) continue;
            uint8_t *fn = api->function_start(g_text + i);
            if (fn && g_text + i == fn + 0x64 && shape_ok(fn, A[A_PASS].shape)) { A[A_PASS].fn = fn; break; }
        }
    }

    /* ctor: the function that stores the CMeshObject vftable and checks the shape */
    for (int i = 0; vt_obj && i < 8 && !A[A_CTOR].fn; i++) {
        uint8_t *x = api->find_xref(vt_obj, i);
        if (!x) break;
        uint8_t *fn = api->function_start(x);
        if (fn && x == fn + 0x71 && shape_ok(fn, A[A_CTOR].shape) && rel32(x, 7) == (uint8_t *)vt_obj) A[A_CTOR].fn = fn;
    }

    /* rdata: called from Load as rdata(this, &this->render_data) */
    if (A[A_LOAD].fn && in_image(A[A_LOAD].fn, LOAD_BODY_SPAN)) {
        for (size_t o = 0; o + 16 < LOAD_BODY_SPAN; o++) {
            if (!lm_match(A[A_LOAD].fn, LOAD_BODY_SPAN, o, LOAD_RDATA_CALL)) continue;
            uint8_t *t = rel32(A[A_LOAD].fn + o + 10, 5);
            if (shape_ok(t, A[A_RDATA].shape)) A[A_RDATA].fn = t;
            break;
        }
    }

    A[A_BLEND].fn = by_string(STR_BLEND, A[A_BLEND].shape);

    for (int i = 0; i < A_COUNT; i++) {
        if (A[i].fn) api->log("lazy_mesh", "anchor %-5s OK at %08x", A[i].name, rva(A[i].fn));
        else { api->log("lazy_mesh", "FAIL-CLOSED anchor %s: not found or shape mismatch", A[i].name); bad++; }
    }
    return bad;
}

/* ------------------------------------------------------------------ helpers */

static int readable(const void *p, size_t n)
{
    uintptr_t a = (uintptr_t)p, end = a + n;
    if (!p || end < a) return 0;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)a, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
        if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
        if (!(mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return 0;
        a = (uintptr_t)mi.BaseAddress + mi.RegionSize;
    }
    return 1;
}

static double now_ms(void)
{
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    return (double)(q.QuadPart - g_qpc0.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
}

/* CMeshType: +0x08 name (std::string), +0x198 mesh file path (std::string) */
#define MT_NAME 0x08
#define MT_PATH 0x198

static void fill_names(lm_entry *e, const void *type)
{
    if (lm_msvc_str((const uint8_t *)type + MT_NAME, e->name, sizeof e->name, readable) < 0) strcpy(e->name, "?");
    if (lm_msvc_str((const uint8_t *)type + MT_PATH, e->path, sizeof e->path, readable) < 0) e->path[0] = 0;
}

/* ------------------------------------------------------------------ hooks */

static fn4 o_pass, o_ctor, o_rdata, o_blend;
static fn1 o_load, o_anim;

static void write_report(void);
static DWORD WINAPI prefetch_main(void *all);
static void start_prefetch(int all, const char *why);

static uintptr_t h_pass(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    g_pass_t0 = now_ms();
    InterlockedExchange(&g_pass_active, 1);
    uintptr_t r = o_pass(a, b, c, d);
    InterlockedExchange(&g_pass_active, 0);
    g_pass_end_ms = now_ms();
    double sum = 0; int eager = 0;
    for (uint32_t i = 0; i < g_tab.cap; i++)
        if (g_tab.e[i].key && g_tab.e[i].state == LM_ST_DONE) { eager++; sum += g_tab.e[i].pass_ms; }
    api->log("lazy_mesh", "startup pass: %.0f ms wall, %d types, %d loaded (sum %.0f ms thread time), %ld deferred, table full %d",
             g_pass_end_ms - g_pass_t0, (int)g_tab.n, eager, sum, g_deferred, (int)g_tab.full);
    InterlockedExchange(&g_dirty, 1);
    write_report();
    if (g_cfg.prefetch_after_s > 0) start_prefetch(1, "prefetch_after_s");
    return r;
}

static void timed_load(lm_entry *e, void *type, float *slot)
{
    double t0 = now_ms();
    o_load(type);
    if (e && slot) *slot = (float)(now_ms() - t0);
}

static void h_load(void *type)
{
    int32_t tid = (int32_t)GetCurrentThreadId();
    if (g_pass_active) {
        int created;
        lm_entry *e = lm_table_insert(&g_tab, type, &created);
        if (!e) { o_load(type); return; }                       /* table full: eager */
        if (created) {
            fill_names(e, type);
            lm_config as_lazy = g_cfg; as_lazy.mode = LM_MODE_LAZY;   /* measure: record what lazy would do */
            e->decision = lm_decide(&as_lazy, &g_eager, &g_prevstart, e->name, e->path);
            if (g_armed && LM_IS_DEFER(e->decision)) {
                InterlockedExchange((volatile LONG *)&e->state, LM_ST_DEFERRED);
                InterlockedIncrement(&g_deferred);
                return;
            }
            if (!g_armed && g_cfg.mode == LM_MODE_LAZY) e->decision = LM_EAGER_MODE;
            InterlockedExchange((volatile LONG *)&e->owner, tid);
            InterlockedExchange((volatile LONG *)&e->state, LM_ST_LOADING);
            timed_load(e, type, &e->pass_ms);
            InterlockedExchange((volatile LONG *)&e->owner, 0);
            InterlockedExchange((volatile LONG *)&e->state, LM_ST_DONE);
            return;
        }
        if (e->state == LM_ST_DEFERRED) return;                 /* repeated call in the pass: still deferred */
        o_load(type);
        return;
    }
    /* outside the pass: CMeshType::Reload (file watcher) or assignment. If we deferred this type,
     * the engine is loading it now: claim it so lm_ensure won't load it a second time. */
    lm_entry *e = lm_table_find(&g_tab, type);
    if (e && e->state == LM_ST_DEFERRED &&
        InterlockedCompareExchange((volatile LONG *)&e->state, LM_ST_LOADING, LM_ST_DEFERRED) == LM_ST_DEFERRED) {
        InterlockedExchange((volatile LONG *)&e->owner, tid);
        o_load(type);
        InterlockedExchange((volatile LONG *)&e->owner, 0);
        InterlockedExchange((volatile LONG *)&e->state, LM_ST_DONE);
        return;
    }
    o_load(type);
}

static void h_anim(void *type)
{
    if (g_armed) {
        lm_entry *e = lm_table_find(&g_tab, type);
        if (e && e->state == LM_ST_DEFERRED) return;   /* keep +0x148 for the on-demand load */
    }
    o_anim(type);
}

/* lm_ensure callback: exactly what the startup pass does for one type */
static void do_load(void *type, void *site)
{
    lm_entry *e = lm_table_find(&g_tab, type);
    double t0 = now_ms();
    o_load(type);
    o_anim(type);
    float ms = (float)(now_ms() - t0);
    if (e) e->lazy_ms = ms;
    InterlockedIncrement(&g_lazy_loads);
    int on_main = GetCurrentThreadId() == g_main_tid;
    AcquireSRWLockExclusive(&g_stats_lock);
    lm_stats_add(&g_stats, (int)(intptr_t)site, ms, on_main && (intptr_t)site != LM_SITE_PREFETCH, e ? e->name : "?");
    ReleaseSRWLockExclusive(&g_stats_lock);
    InterlockedExchange(&g_dirty, 1);
    if (g_cfg.log_loads && InterlockedDecrement(&g_log_budget) >= 0)
        api->log("lazy_mesh", "on-demand load %s (%s) %.0f ms, thread %lu%s, via %s", e ? e->name : "?", e ? e->path : "",
                 ms, GetCurrentThreadId(), on_main ? " (main)" : "", lm_site_name((int)(intptr_t)site));
}

static void request(void *type, int site)
{
    lm_entry *e = lm_table_find(&g_tab, type);
    if (!e) return;
    /* Load itself calls rdata(this): a call from the thread that is loading this type is not a use */
    if (e->state == LM_ST_LOADING && e->owner == (int32_t)GetCurrentThreadId()) return;
    if (!e->uses) InterlockedExchange(&g_dirty, 1);
    lm_note_use(e, site, now_ms(), (int)g_phase);
    if (!g_armed || e->state == LM_ST_DONE) return;
    int r = lm_ensure(&g_tab, type, (int32_t)GetCurrentThreadId(), do_load, (void *)(intptr_t)site,
                      (unsigned)g_cfg.wait_timeout_ms, NULL);
    if (r == LM_R_TIMEOUT && InterlockedIncrement(&g_timeouts) <= 20)
        api->log("lazy_mesh", "WARNING: waited %d ms for %s (loading on another thread); continuing", g_cfg.wait_timeout_ms, e->name);
}

static uintptr_t h_ctor(uintptr_t self, uintptr_t type, uintptr_t c, uintptr_t d) { request((void *)type, LM_SITE_CTOR); return o_ctor(self, type, c, d); }
static uintptr_t h_rdata(uintptr_t type, uintptr_t out, uintptr_t c, uintptr_t d) { request((void *)type, LM_SITE_RDATA); return o_rdata(type, out, c, d); }
static uintptr_t h_blend(uintptr_t out, uintptr_t attr, uintptr_t type, uintptr_t name) { request((void *)type, LM_SITE_BLEND); return o_blend(out, attr, type, name); }

/* Background prefetch: one low-priority thread loads deferred types one at a time (lm_ensure, so a
 * request for the same type from the game just waits for it). Order: most used in the previous session
 * first; types never used before only if `all`. */
typedef struct { lm_entry *e; int prio; } pf_item;
static int pf_cmp(const void *a, const void *b) { return ((const pf_item *)b)->prio - ((const pf_item *)a)->prio; }

static DWORD WINAPI prefetch_main(void *all_p)
{
    int all = (int)(intptr_t)all_p;
    if (all && g_cfg.prefetch_after_s > 0) Sleep((DWORD)g_cfg.prefetch_after_s * 1000);
    pf_item *v = malloc(sizeof *v * (g_tab.n + 1));
    if (!v) return 0;
    int n = 0;
    for (uint32_t i = 0; i < g_tab.cap; i++) {
        lm_entry *e = &g_tab.e[i];
        if (!e->key || e->state != LM_ST_DEFERRED) continue;
        const lm_prev_item *p = lm_prev_find(&g_prev, e->name);
        if (!p && !all && !g_cfg.prefetch_all) continue;
        v[n].e = e; v[n].prio = p ? p->uses : 0; n++;
    }
    qsort(v, (size_t)n, sizeof *v, pf_cmp);
    double t0 = now_ms(); int loaded = 0;
    for (int i = 0; i < n; i++) {
        if (g_prefetch_stop) { api->log("lazy_mesh", "prefetch stopped (left the Game state)"); break; }
        if (v[i].e->state != LM_ST_DEFERRED) continue;   /* the game got there first */
        if (lm_ensure(&g_tab, (void *)(uintptr_t)v[i].e->key, (int32_t)GetCurrentThreadId(), do_load, (void *)(intptr_t)LM_SITE_PREFETCH,
                      (unsigned)g_cfg.wait_timeout_ms, NULL) == LM_R_LOADED) loaded++;
        if (g_cfg.prefetch_gap_ms) Sleep((DWORD)g_cfg.prefetch_gap_ms); else SwitchToThread();
    }
    api->log("lazy_mesh", "prefetch done: %d of %d queued types loaded in the background in %.1f s", loaded, n, (now_ms() - t0) / 1000.0);
    free(v);
    InterlockedExchange(&g_dirty, 1);
    return 0;
}

static void start_prefetch(int all, const char *why)
{
    if (!g_armed || g_deferred <= 0 || InterlockedExchange(&g_prefetch_started, 1)) return;
    HANDLE h = CreateThread(NULL, 0, prefetch_main, (void *)(intptr_t)all, 0, NULL);
    if (h) { SetThreadPriority(h, THREAD_PRIORITY_LOWEST); CloseHandle(h); }
    api->log("lazy_mesh", "prefetch started (%s)%s", why, h ? "" : " FAILED");
}

static void log_stats(const char *when)
{
    char b[512];
    AcquireSRWLockShared(&g_stats_lock);
    lm_stats_format(&g_stats, b, sizeof b);
    ReleaseSRWLockShared(&g_stats_lock);
    api->log("lazy_mesh", "session summary (%s): %s", when, b);
}

/* ------------------------------------------------------------------ report / console / events */

static SRWLOCK g_write_lock = SRWLOCK_INIT;

static void write_report(void)
{
    if (!g_tab.e || !InterlockedExchange(&g_dirty, 0)) return;
    AcquireSRWLockExclusive(&g_write_lock);
    size_t cap = ((size_t)g_tab.n + 16) * 420 + 4096;
    char *buf = malloc(cap);
    if (buf) {
        lm_report_info info = { g_cfg.mode, api->game_version(), g_pass_end_ms, g_phase_ptr, (int)(g_phase + 1),
                                g_cfg.merge_report ? &g_prev : NULL };
        size_t n = lm_format_report(&g_tab, &info, buf, cap);
        char tmp[MAX_PATH]; snprintf(tmp, sizeof tmp, "%s.tmp", g_report_path);
        HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w = 0; BOOL ok = WriteFile(h, buf, (DWORD)n, &w, NULL); CloseHandle(h);
            if (!ok || w != n || !MoveFileExA(tmp, g_report_path, MOVEFILE_REPLACE_EXISTING))
                api->log("lazy_mesh", "report write failed (%lu)", GetLastError());
        }
        free(buf);
    }
    ReleaseSRWLockExclusive(&g_write_lock);
}

static void on_frame(void *u)
{
    (void)u;
    ULONGLONG t = GetTickCount64();
    if (t < g_next_write) return;
    g_next_write = t + 30000;
    write_report();
}

static void on_transition(const char *name, void *data, void *user)
{
    (void)name; (void)user;
    const V3X_LoadTransition *i = data;
    LONG k = g_phase + 1;
    if (k >= MAX_PHASES) return;
    snprintf(g_phase_names[k], sizeof g_phase_names[k], "%s", i && i->to ? i->to : "?");
    g_phase_ptr[k] = g_phase_names[k];
    InterlockedExchange(&g_phase, k);
    InterlockedExchange(&g_dirty, 1);
    char when[112]; snprintf(when, sizeof when, "%s->%s", i && i->from ? i->from : "?", g_phase_names[k]);
    log_stats(when);
    InterlockedExchange(&g_prefetch_stop, strcmp(g_phase_names[k], "Game") != 0 && g_prefetch_started);
    if (g_cfg.prefetch_on_game && !strcmp(g_phase_names[k], "Game")) start_prefetch(0, when);
}

static void cmd_lazymesh(const char *args, void *user)
{
    (void)user;
    if (!strncmp(args, "dump", 4)) { InterlockedExchange(&g_dirty, 1); write_report(); api->print("lazy_mesh: wrote %s", g_report_path); return; }
    if (!strncmp(args, "load ", 5)) {
        const char *nm = args + 5;
        for (uint32_t i = 0; i < g_tab.cap; i++) {
            lm_entry *e = &g_tab.e[i];
            if (!e->key || strcmp(e->name, nm)) continue;
            int r = g_armed ? lm_ensure(&g_tab, (void *)(uintptr_t)e->key, (int32_t)GetCurrentThreadId(), do_load,
                                        (void *)(intptr_t)LM_SITE_CONSOLE, (unsigned)g_cfg.wait_timeout_ms, NULL) : LM_R_READY;
            api->print("lazy_mesh: %s -> %s (%.0f ms)", nm, r == LM_R_LOADED ? "loaded now" : r == LM_R_TIMEOUT ? "timeout" : "already loaded", e->lazy_ms);
            return;
        }
        api->print("lazy_mesh: no mesh type named '%s'", nm);
        return;
    }
    static const char *modes[] = { "off", "measure", "lazy" };
    int pending = 0, used = 0;
    for (uint32_t i = 0; i < g_tab.cap; i++) {
        if (!g_tab.e[i].key) continue;
        pending += g_tab.e[i].state == LM_ST_DEFERRED;
        used += g_tab.e[i].uses > 0;
    }
    api->print("lazy_mesh: mode=%s armed=%ld types=%d used=%d deferred=%ld still_unloaded=%d on_demand=%ld timeouts=%ld report=%s",
               modes[g_cfg.mode], g_armed, (int)g_tab.n, used, g_deferred, pending, g_lazy_loads, g_timeouts, g_report_path);
    char b[512];
    AcquireSRWLockShared(&g_stats_lock); lm_stats_format(&g_stats, b, sizeof b); ReleaseSRWLockShared(&g_stats_lock);
    api->print("lazy_mesh: %s", b);
}

/* ------------------------------------------------------------------ load */

static void load_text_file(const char *name, char **out)
{
    *out = NULL;
    char path[MAX_PATH]; snprintf(path, sizeof path, "%s%s", api->v3x_dir(), name);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (32 << 20)) { fclose(f); return; }
    char *b = malloc((size_t)n + 1);
    if (b) { size_t r = fread(b, 1, (size_t)n, f); b[r] = 0; }
    fclose(f);
    *out = b;
}

static int install(int id, void *detour, void **orig)
{
    int s = api->hook(A[id].fn, detour, orig);
    api->log("lazy_mesh", "%s %s at %08x (status %d)", s ? "FAIL" : "hooked", A[id].name, rva(A[id].fn), s);
    if (s) *orig = NULL;
    return s == 0;
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    lm_config_defaults(&g_cfg);
    char *ini; load_text_file("lazy_mesh.ini", &ini);
    if (ini) {
        char err[160];
        int bad = lm_config_parse(&g_cfg, ini, err, sizeof err);
        if (bad) api->log("lazy_mesh", "lazy_mesh.ini: %d bad line(s), first: %s", bad, err);
        free(ini);
    }
    if (g_cfg.mode == LM_MODE_OFF) { api->log("lazy_mesh", "mode=off: no hooks installed"); return 0; }
    QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_qpc0);
    snprintf(g_report_path, sizeof g_report_path, "%slazy_mesh_used.txt", api->v3x_dir());
    char *eager; load_text_file(g_cfg.eager_file, &eager);
    if (eager) { int n = lm_nameset_parse(&g_eager, eager); free(eager); api->log("lazy_mesh", "eager list %s: %d names", g_cfg.eager_file, n); }
    g_main_tid = GetCurrentThreadId();
    if (g_cfg.merge_report || (g_cfg.mode == LM_MODE_LAZY && (g_cfg.startup_from_report || g_cfg.prefetch_on_game))) {
        char *prev; load_text_file("lazy_mesh_used.txt", &prev);   /* previous session; rewritten after the pass */
        if (prev) {
            lm_prev_parse(&g_prev, prev); free(prev);
            int ns = g_cfg.mode == LM_MODE_LAZY && g_cfg.startup_from_report ? lm_prev_startup_set(&g_prev, &g_prevstart) : 0;
            api->log("lazy_mesh", "previous report: %d used types, %d first requested by entity parsing (loaded in the pass)", g_prev.n, ns);
        }
    }
    if (lm_table_init(&g_tab, 1u << 14)) { api->log("lazy_mesh", "FAIL-CLOSED: out of memory"); return 1; }

    resolve_all();
    /* measure needs load + pass + ctor; lazy needs all six. Deferral is only ever enabled (g_armed) after
     * every hook it depends on is installed, so a partial failure can never leave a type unloaded. */
    int ok_measure = A[A_LOAD].fn && A[A_PASS].fn && A[A_CTOR].fn;
    int ok_lazy = ok_measure && A[A_ANIM].fn && A[A_RDATA].fn && A[A_BLEND].fn;
    if (!ok_measure) { api->log("lazy_mesh", "FAIL-CLOSED: required anchors missing, nothing installed"); return 0; }
    if (g_cfg.mode == LM_MODE_LAZY && !ok_lazy)
        api->log("lazy_mesh", "FAIL-CLOSED: lazy needs all 6 anchors; running as measure (no type is deferred)");

    int hooked = 1;
    hooked &= install(A_CTOR, (void *)h_ctor, (void **)&o_ctor);
    if (A[A_RDATA].fn) hooked &= install(A_RDATA, (void *)h_rdata, (void **)&o_rdata) || !ok_lazy;
    if (A[A_BLEND].fn) hooked &= install(A_BLEND, (void *)h_blend, (void **)&o_blend) || !ok_lazy;
    if (A[A_ANIM].fn) hooked &= install(A_ANIM, (void *)h_anim, (void **)&o_anim) || !ok_lazy;
    if (!install(A_LOAD, (void *)h_load, (void **)&o_load)) { api->log("lazy_mesh", "FAIL-CLOSED: load hook failed"); return 0; }
    if (!install(A_PASS, (void *)h_pass, (void **)&o_pass)) { api->log("lazy_mesh", "FAIL-CLOSED: pass hook failed (measure/lazy inactive)"); return 0; }
    if (g_cfg.mode == LM_MODE_LAZY && ok_lazy && hooked && o_anim && o_rdata && o_blend && o_ctor) {
        InterlockedExchange(&g_armed, 1);
        api->log("lazy_mesh", "lazy armed: skip_debug=%d defer_all=%d defer_dirs=%d eager_contains=%d eager names=%u prev_startup=%u "
                 "prefetch_on_game=%d prefetch_all=%d prefetch_after_s=%d",
                 g_cfg.skip_debug, g_cfg.defer_all, g_cfg.n_defer_dirs, g_cfg.n_eager_contains, g_eager.n, g_prevstart.n,
                 g_cfg.prefetch_on_game, g_cfg.prefetch_all, g_cfg.prefetch_after_s);
    } else if (g_cfg.mode == LM_MODE_LAZY) {
        api->log("lazy_mesh", "FAIL-CLOSED: lazy not armed (a hook failed); measuring only");
    }
    if (api->api_version >= 2 && api->struct_size >= offsetof(V3X_API, register_command) + sizeof(void *) && api->register_command)
        api->register_command("lazymesh", "lazy_mesh: status | dump (write v3x/lazy_mesh_used.txt) | load <type name>", cmd_lazymesh, NULL);
    if (api->api_version >= 3 && api->struct_size >= offsetof(V3X_API, on_event) + sizeof(void *) && api->on_event)
        api->on_event("load_transition", on_transition, NULL);
    api->on_frame(on_frame, NULL);
    return 0;
}
