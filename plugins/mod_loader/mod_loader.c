/* mod_loader: measures and (opt-in) removes the per-file cost mods add to Victoria 3's virtual file
 * system, and writes a playset / override / error report (v3x/mod_report.txt).
 *
 * Why (docs/re/modloading.md): the engine's VFS is PhysFS 3. Every mod and every DLC folder is a
 * separate search-path root, mounted with PREPEND (last mod = searched first). VFSOpen does
 * PHYSFS_getRealDir (DIR_stat = GetFileAttributesExW per root until found) and then PHYSFS_openRead
 * (DIR_openRead = CreateFileW per root until found). With 22 mods + 12 DLC every vanilla file costs
 * ~34 failed syscall pairs; in the lead's profiles those two loops are ~59 s of sampled thread time per
 * Empty->Game load (13.7 % of all samples).
 *
 * How: hooks the PhysFS DIR archiver's stat and openRead (found via its archiver struct: the
 * .rdata struct whose description is "Non-archive, direct filesystem I/O", slots checked). For roots
 * under steamapps/workshop/content/529340/ and <game>/game/dlc/ it builds a file index once (thread
 * pool, a few ms per root). Then:
 *   mode=measure (default)  every probe still goes to the engine; the index only PREDICTS the result,
 *                           and every prediction is checked against the real one (mismatches are
 *                           reported). Counts probes, misses and time in misses per root.
 *   mode=on                 a probe the index says is a miss returns "not found" immediately (PhysFS
 *                           error code NOT_FOUND set exactly like the real miss); every verify_every-th
 *                           skip is re-checked with the real call and one mismatch turns the cache off.
 *   mode=off                nothing is hooked.
 * Names Windows could alias (8.3 '~', trailing dots, non-ASCII, devices, ...) are never answered from
 * the index (ml_norm). Any index build error = that root is not indexed.
 *
 * Optional (default off): skip_mods=<ids> / skip_fatal=1 hide a mod's root (stat/openRead answer
 * "not found", as if the mod had no files). skip_fatal uses the previous run's logs/error.log
 * (parse errors attributed to the mod that provides the file). Never touches DLC roots, the mod list,
 * the launcher database or any ownership/entitlement code.
 *
 * Report (report=1): v3x/mod_report.txt after every load transition and on the `modload` command. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <intrin.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "v3x.h"
#include "v3x_safe.h"
#include "ml_core.h"

V3X_EXPORT const char *v3x_plugin_name = "mod_loader";
V3X_PLUGIN_MANIFEST("mod_loader", "0.5.0", "v3x", "measures and removes the per-file cost of mods", "release/1.15.1");

#define TAG "mod_loader"
#define MAX_ROOTS 128
#define MAX_PLAY 128
#define PHYSFS_ERR_NOT_FOUND 0xb

#define PASS8_ARGS void *a0_, void *a1_, void *a2_, void *a3_, void *a4_, void *a5_, void *a6_, void *a7_
#define PASS8 a0_, a1_, a2_, a3_, a4_, a5_, a6_, a7_
typedef uintptr_t (*pass8_fn)(PASS8_ARGS);

enum { IX_NONE = 0, IX_PENDING, IX_READY, IX_FAILED };

typedef struct {
    void *opaque;
    char path[600];          /* opaque string as the engine gave it */
    char norm[600];
    int cls, play, hidden;
    volatile LONG state;
    ml_hv hv;                /* compact lookup index: sorted 64-bit key hashes */
    ml_dirs dirs;            /* directories + mtimes = persisted signature */
    int from_cache;
    double build_ms;
    volatile LONG64 st_n, st_miss, op_n, op_miss, miss_qpc, hit_qpc, would_n, would_qpc, skipped, verified,
        mismatch, fp, hidden_n, unpred_n;
} root_t;

typedef struct {
    char path[600], norm[600], leaf[128];
    int errs, parse, dup, loc, hidden;
    char sample[2][200];
} play_t;

static const V3X_API *api;

/* L10 (docs/security-audit.md): log lines carry mod / file / engine text; control characters,
 * C1 controls and bidi overrides are replaced (v3x_sanitize_line) so a crafted name cannot forge
 * v3x.log lines (parsed by doctor and the installer). */
static void slog(const char *fmt, ...) V3X_PRINTF(1, 2);
static void slog(const char *fmt, ...)
{
    if (!api) return;
    char raw[1536], clean[1536];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(raw, sizeof raw, fmt, ap);
    va_end(ap);
    v3x_sanitize_line(clean, sizeof clean, raw);
    api->log(TAG, "%s", clean);
}
static ml_config cfg;
static pass8_fn o_stat, o_open;
static void *g_stat_fn, *g_open_fn, *g_archive_fn;
static int32_t g_err_off = -1;
static SRWLOCK g_lock = SRWLOCK_INIT;
static root_t g_roots[MAX_ROOTS];
static volatile LONG g_nroots;
static play_t g_play[MAX_PLAY];
static int g_nplay;
static volatile LONG g_disabled;           /* mode=on cache turned off after a verify mismatch */
static int g_active;                       /* hooks installed */
static char g_game_norm[600], g_docs_norm[600], g_game_native[600], g_docs_native[600];
static char g_mismatch[16][300];
static char g_unpred[1024][200];          /* measure: misses the index could not predict (why + root + name) */
static volatile LONG g_nunpred;
static volatile LONG g_nmismatch;
static LARGE_INTEGER g_freq;
static volatile LONG g_reports, g_errlog_done;
static char g_last_event[96];

static void set_notfound_tls(void)
{
    char **tls = (char **)__readgsqword(0x58);
    *(int32_t *)(tls[0] + g_err_off) = PHYSFS_ERR_NOT_FOUND;
}
static void (*g_set_err)(void) = set_notfound_tls;   /* replaced by the offline test */

static double qms(LONG64 t) { return g_freq.QuadPart ? (double)t * 1000.0 / (double)g_freq.QuadPart : 0; }

/* ---- index build / persisted cache -------------------------------------------------------------- */

/* walk(): calls sink(u, key, len, is_dir) for every entry under the root (keys lower-case ASCII, '/'),
 * and dir(u, rel, mtime) for the root ("") and every directory with its mtime read BEFORE it is listed. */
typedef struct {
    int (*sink)(void *u, const char *key, size_t len, int is_dir);
    int (*dir)(void *u, const char *rel, uint64_t mtime);
    void *u;
} walk_t;

static int dir_mtime(const wchar_t *path, uint64_t *mt)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a) || !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    *mt = ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    return 1;
}

static int walk(const walk_t *w, wchar_t *path, size_t plen, char *key, size_t klen, int depth)
{
    if (depth > 24 || plen + 4 >= 1024) return 0;
    uint64_t mt;
    if (w->dir && (!dir_mtime(path, &mt) || w->dir(w->u, key, mt))) return 0;
    memcpy(path + plen, L"\\*", 3 * sizeof(wchar_t));      /* plen + 4 < 1024 checked above */
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(path, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    path[plen] = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = 1;
    do {
        const wchar_t *n = fd.cFileName;
        if (n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2]))) continue;
        char u[MAX_PATH * 4];
        int ul = WideCharToMultiByte(CP_UTF8, 0, n, -1, u, sizeof u, NULL, NULL);
        if (ul <= 1 || klen + (size_t)ul + 2 >= 1024) { ok = 0; break; }
        size_t kl = klen;
        if (kl) key[kl++] = '/';
        for (int i = 0; i < ul - 1; i++) key[kl++] = (char)(u[i] >= 'A' && u[i] <= 'Z' ? u[i] + 32 : u[i]);
        key[kl] = 0;
        int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (w->sink(w->u, key, kl, isdir)) { ok = 0; break; }
        if (isdir) {
            size_t nl = wcslen(n);
            if (plen + 1 + nl + 4 >= 1024) { ok = 0; break; }
            path[plen] = L'\\';
            memcpy(path + plen + 1, n, (nl + 1) * sizeof(wchar_t));
            ok = walk(w, path, plen + 1 + nl, key, kl, depth + 1);
            path[plen] = 0;
            if (!ok) break;
        }
    } while (FindNextFileW(h, &fd));
    if (ok && GetLastError() != ERROR_NO_MORE_FILES) ok = 0;
    FindClose(h);
    key[klen] = 0;
    return ok;
}

static size_t native_root(const char *path, wchar_t *w)   /* opaque -> "D:\x\y" (no trailing '\'), 0 = bad */
{
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, w, 1000) <= 0) return 0;
    size_t n = wcslen(w);
    while (n && (w[n - 1] == L'\\' || w[n - 1] == L'/')) w[--n] = 0;
    for (size_t i = 0; i < n; i++) if (w[i] == L'/') w[i] = L'\\';
    return n;
}

static int hv_sink(void *u, const char *k, size_t n, int d) { (void)d; return ml_hv_push(&((root_t *)u)->hv, ml_hash(k, n)); }
static int dirs_sink(void *u, const char *rel, uint64_t mt) { return ml_dirs_add(&((root_t *)u)->dirs, rel, mt); }
static int set_sink(void *u, const char *k, size_t n, int d) { return ml_set_add((ml_set *)u, k, n, d) < 0; }

/* keys of a root as a string set (report only: overrides / BOM need the names) */
static int list_keys(const root_t *r, ml_set *s)
{
    wchar_t w[1024];
    char key[1024] = "";
    size_t n = native_root(r->path, w);
    walk_t wk = {set_sink, NULL, s};
    return n && walk(&wk, w, n, key, 0, 0);
}

/* persisted index: loaded once at plugin load, consumed by build_index, rewritten when roots changed */
static ml_ixroot *g_cache;
static int g_ncache;
static volatile LONG *g_cache_taken;
static volatile LONG g_dirty;
static char g_cache_note[160];

static void cache_load(void)
{
    char p[700];
    ml_ixfile_free(g_cache, g_ncache);           /* (re)load: drop any previous cache */
    free((void *)g_cache_taken);
    g_cache = NULL; g_ncache = 0; g_cache_taken = NULL;
    snprintf(p, sizeof p, "%smod_loader.index", api->v3x_dir());
    size_t n = 0;
    FILE *f = fopen(p, "rb");
    if (!f) { snprintf(g_cache_note, sizeof g_cache_note, "no index file"); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = sz > 0 && sz < (256L << 20) ? malloc((size_t)sz) : NULL;
    if (b) n = fread(b, 1, (size_t)sz, f);
    fclose(f);
    int k = b && n == (size_t)sz ? ml_ixfile_read(b, n, &g_cache) : -1;
    free(b);
    if (k < 0) { g_cache = NULL; snprintf(g_cache_note, sizeof g_cache_note, "index file corrupt/old -> ignored, full rebuild"); return; }
    g_ncache = k;
    g_cache_taken = calloc(k ? k : 1, sizeof *g_cache_taken);
    snprintf(g_cache_note, sizeof g_cache_note, "index file: %d root(s), %ld bytes", k, sz);
}

/* adopts the cached index of r if every recorded directory still has its mtime. 1 = adopted */
static int cache_try(root_t *r, const wchar_t *w, size_t wn)
{
    for (int i = 0; i < g_ncache; i++) {
        if (strcmp(g_cache[i].root, r->norm) || InterlockedExchange(&g_cache_taken[i], 1)) continue;
        ml_ixroot *c = &g_cache[i];
        wchar_t p[1100];
        int ok = c->dirs.n > 0 && !strcmp(ml_dirs_name(&c->dirs, 0), "");
        for (uint32_t k = 0; ok && k < c->dirs.n; k++) {
            const char *rel = ml_dirs_name(&c->dirs, k);
            uint64_t mt;
            /* the index cache is a file under binaries/v3x: a recorded dir must be a clean relative path */
            if (wn >= 1024 || (*rel && v3x_path_rel_ok(rel) != V3X_R_OK)) { ok = 0; break; }
            memcpy(p, w, (wn + 1) * sizeof(wchar_t));
            size_t pl = wn;
            if (*rel) {
                p[pl++] = L'\\';
                int m = MultiByteToWideChar(CP_UTF8, 0, rel, -1, p + pl, (int)(1100 - pl));
                if (m <= 0) { ok = 0; break; }
                for (wchar_t *q = p + pl; *q; q++) if (*q == L'/') *q = L'\\';
            }
            ok = dir_mtime(p, &mt) && mt == c->dirs.mt[k];
        }
        if (!ok) return 0;
        r->hv = c->hv; memset(&c->hv, 0, sizeof c->hv);         /* steal */
        r->dirs = c->dirs; memset(&c->dirs, 0, sizeof c->dirs);
        return 1;
    }
    return 0;
}

static void build_index(root_t *r)
{
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    wchar_t w[1024];
    char key[1024] = "";
    size_t n = native_root(r->path, w);
    int ok = 0;
    if (n && cfg.index_cache && cache_try(r, w, n)) { ok = 1; r->from_cache = 1; }
    else if (n) {
        walk_t wk = {hv_sink, dirs_sink, r};
        ok = walk(&wk, w, n, key, 0, 0);
        if (ok) { ml_hv_finish(&r->hv); InterlockedExchange(&g_dirty, 1); }
    }
    QueryPerformanceCounter(&t1);
    r->build_ms = qms(t1.QuadPart - t0.QuadPart);
    if (!ok) { ml_hv_free(&r->hv); ml_dirs_free(&r->dirs); }
    InterlockedExchange(&r->state, ok ? IX_READY : IX_FAILED);
    if (api) slog("index %s: %s, %u keys %u dirs, %.2f ms", r->norm,
                      !ok ? "FAILED (root not indexed)" : r->from_cache ? "reused (signature unchanged)" : "built",
                      r->hv.n, r->dirs.n, r->build_ms);
}

/* writes every READY root to mod_loader.index (tmp + rename). Returns bytes or 0. */
static size_t cache_save(void)
{
    const ml_ixroot *rs[MAX_ROOTS];
    static ml_ixroot tmp[MAX_ROOTS];
    int n = 0;
    for (LONG k = 0; k < g_nroots; k++) {
        root_t *r = &g_roots[k];
        if (r->state != IX_READY) continue;
        snprintf(tmp[n].root, sizeof tmp[n].root, "%s", r->norm);
        tmp[n].dirs = r->dirs; tmp[n].hv = r->hv;               /* shallow views, not freed */
        rs[n] = &tmp[n]; n++;
    }
    size_t sz = ml_ixfile_write(rs, n, NULL, 0);
    uint8_t *b = malloc(sz);
    if (!b || ml_ixfile_write(rs, n, b, sz) != sz) { free(b); return 0; }
    char p[700], pt[720];
    snprintf(p, sizeof p, "%smod_loader.index", api->v3x_dir());
    snprintf(pt, sizeof pt, "%s.tmp", p);
    FILE *f = fopen(pt, "wb");
    size_t wr = f ? fwrite(b, 1, sz, f) : 0;
    if (f) fclose(f);
    free(b);
    if (wr != sz || !MoveFileExA(pt, p, MOVEFILE_REPLACE_EXISTING)) { remove(pt); return 0; }
    InterlockedExchange(&g_dirty, 0);
    return sz;
}

static void CALLBACK build_cb(PTP_CALLBACK_INSTANCE inst, void *ctx) { (void)inst; build_index((root_t *)ctx); }

/* ---- roots --------------------------------------------------------------------------------------- */

static int indexable(int cls)
{
    return (cls == ML_ROOT_WORKSHOP && cfg.index_workshop) || (cls == ML_ROOT_DLC && cfg.index_dlc) ||
           (cls == ML_ROOT_LOCAL && cfg.index_local) || (cls == ML_ROOT_GAME && cfg.index_game) ||
           (cls == ML_ROOT_PACK && cfg.index_packs);
}

static root_t *root_find(void *opaque)
{
    LONG n = g_nroots;
    MemoryBarrier();
    for (LONG i = 0; i < n; i++)
        if (g_roots[i].opaque == opaque && !strcmp(g_roots[i].path, (const char *)opaque)) return &g_roots[i];
    return NULL;
}

static root_t *root_get(void *opaque)
{
    if (!opaque) return NULL;
    root_t *r = root_find(opaque);
    if (r) return r;
    if (strlen((const char *)opaque) >= sizeof r->path) return NULL;
    AcquireSRWLockExclusive(&g_lock);
    r = root_find(opaque);
    if (!r && g_nroots < MAX_ROOTS) {
        r = &g_roots[g_nroots];
        memset(r, 0, sizeof *r);
        r->opaque = opaque;
        snprintf(r->path, sizeof r->path, "%s", (const char *)opaque);
        ml_norm_root(r->path, r->norm, sizeof r->norm);
        r->cls = ml_classify(r->norm, g_game_norm, g_docs_norm);
        r->play = -1;
        for (int i = g_nplay - 1; i >= 0; i--)
            if (!strcmp(g_play[i].norm, r->norm)) { r->play = i; break; }
        if (r->play >= 0 && g_play[r->play].hidden && r->cls != ML_ROOT_DLC && r->cls != ML_ROOT_GAME && g_err_off >= 0)
            r->hidden = 1;
        int want = indexable(r->cls) && !r->hidden;
        r->state = want ? IX_PENDING : IX_NONE;
        MemoryBarrier();
        InterlockedIncrement(&g_nroots);
        if (want && !TrySubmitThreadpoolCallback(build_cb, r, NULL)) InterlockedExchange(&r->state, IX_FAILED);
        if (api) slog("root %s (%s%s%s)", r->norm, ml_class_name(r->cls), r->play >= 0 ? ", playset" : "",
                          r->hidden ? ", HIDDEN" : "");
    }
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

static void note_mismatch(root_t *r, const char *name, int is_open)
{
    LONG k = InterlockedIncrement(&g_nmismatch) - 1;
    if (k < 16) snprintf(g_mismatch[k], sizeof g_mismatch[k], "%s %s: %s", ml_root_leaf(r->norm), is_open ? "open" : "stat", name);
    if (api && k < 16) slog("INDEX MISMATCH %s %s: '%s' exists but the index says no", r->norm, is_open ? "open" : "stat", name);
}

/* one PhysFS DIR probe (stat or openRead) on root r */
static uintptr_t probe(root_t *r, int is_open, pass8_fn orig, PASS8_ARGS)
{
    int pred = -1;
    if (r->state == IX_READY) {
        MemoryBarrier();
        uint64_t h;
        if (ml_key((const char *)a1_, &h) >= 0) pred = ml_hv_has(&r->hv, h);
    }
    if (cfg.mode == ML_ON && pred == 0 && !g_disabled) {
        LONG64 k = InterlockedIncrement64(&r->skipped);
        if (!(cfg.verify_every && k % cfg.verify_every == 0)) {
            g_set_err();
            return 0;
        }
        InterlockedIncrement64(&r->verified);
        uintptr_t res = orig(PASS8);
        int hit = is_open ? res != 0 : (int)res != 0;
        if (hit) {
            InterlockedIncrement64(&r->mismatch);
            note_mismatch(r, (const char *)a1_, is_open);
            if (!InterlockedExchange(&g_disabled, 1) && api)
                slog("verify mismatch -> lookup cache OFF for this session (every probe goes to the engine)");
        }
        return res;
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    uintptr_t res = orig(PASS8);
    QueryPerformanceCounter(&t1);
    LONG64 dt = t1.QuadPart - t0.QuadPart;
    int hit = is_open ? res != 0 : (int)res != 0;
    InterlockedIncrement64(is_open ? &r->op_n : &r->st_n);
    if (!hit) {
        InterlockedIncrement64(is_open ? &r->op_miss : &r->st_miss);
        InterlockedAdd64(&r->miss_qpc, dt);
    } else
        InterlockedAdd64(&r->hit_qpc, dt);
    if (pred == 0 && hit) {
        InterlockedIncrement64(&r->mismatch);
        note_mismatch(r, (const char *)a1_, is_open);
        if (cfg.mode == ML_ON) InterlockedExchange(&g_disabled, 1);
    } else if (pred == 1 && !hit)
        InterlockedIncrement64(&r->fp);
    else if (pred == 0) {
        InterlockedIncrement64(&r->would_n);
        InterlockedAdd64(&r->would_qpc, dt);
    } else if (pred < 0 && !hit && !r->hidden) {
        InterlockedIncrement64(&r->unpred_n);
        if (g_nunpred < cfg.log_misses) {
            LONG k = InterlockedIncrement(&g_nunpred) - 1;
            if (k < cfg.log_misses && k < 1024)
                snprintf(g_unpred[k], sizeof g_unpred[k], "%s %s %s: %s",
                         r->state == IX_READY ? "unsafe-name" : r->state == IX_NONE ? "root-not-indexed" :
                         r->state == IX_PENDING ? "index-building" : "index-failed",
                         is_open ? "open" : "stat", ml_root_leaf(r->norm), (const char *)a1_);
        }
    }
    return res;
}

/* PhysFS 3 DIR archiver: int stat(void *opaque, const char *name, PHYSFS_Stat *st) */
static uintptr_t stat_detour(PASS8_ARGS)
{
    root_t *r = root_get(a0_);
    if (!r) return o_stat(PASS8);
    if (r->hidden) { InterlockedIncrement64(&r->hidden_n); g_set_err(); return 0; }
    return probe(r, 0, o_stat, PASS8);
}

/* PHYSFS_Io *openRead(void *opaque, const char *name) */
static uintptr_t open_detour(PASS8_ARGS)
{
    root_t *r = root_get(a0_);
    if (!r) return o_open(PASS8);
    if (r->hidden) { InterlockedIncrement64(&r->hidden_n); g_set_err(); return 0; }
    return probe(r, 1, o_open, PASS8);
}

/* ---- resolve ------------------------------------------------------------------------------------- */

static int in_text(const void *p)
{
    uintptr_t b = api->image_base();
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
        if ((s->Characteristics & IMAGE_SCN_MEM_EXECUTE) && (uintptr_t)p >= b + s->VirtualAddress &&
            (uintptr_t)p < b + s->VirtualAddress + s->Misc.VirtualSize)
            return 1;
    return 0;
}

static int in_image(const void *p, size_t n)
{
    uintptr_t b = api->image_base();
    return (uintptr_t)p >= b && (uintptr_t)p + n <= b + api->image_size();
}

static int match(const uint8_t *p, const char *pat)   /* IDA-style, ?? = any */
{
    for (int i = 0; *pat; i++) {
        while (*pat == ' ') pat++;
        if (!*pat) break;
        if (pat[0] == '?') { pat += pat[1] == '?' ? 2 : 1; continue; }
        unsigned v;
        if (sscanf(pat, "%2x", &v) != 1 || p[i] != (uint8_t)v) return 0;
        pat += 2;
    }
    return 1;
}

#define DIR_DESC "Non-archive, direct filesystem I/O"
#define DIR_AUTHOR "Ryan C. Gordon <icculus@icculus.org>"
#define OPEN_THUNK "41 B8 72 00 00 00 E9"                 /* mov r8d,'r' ; jmp doOpen */
#define STAT_PROLOGUE "40 55 41 56 41 57 48 83 EC 20"
#define ERR_WRITE "65 48 8B 04 25 58 00 00 00 BA ?? ?? ?? ?? 48 8B 08 C7 04 0A"

/* Returns NULL on success, else the reason. Fills g_stat_fn, g_open_fn, g_archive_fn, g_err_off. */
static const char *resolve(void)
{
    const char *desc = api->find_string(DIR_DESC);
    if (!desc) return "description string not found";
    uintptr_t b = api->image_base(), want = (uintptr_t)desc;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    uint8_t *hit = NULL;
    int nhit = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (s->Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        uint8_t *p = (uint8_t *)(b + s->VirtualAddress);
        for (size_t o = 0; o + 8 <= s->Misc.VirtualSize; o += 8)
            if (*(uintptr_t *)(p + o) == want) { hit = p + o; nhit++; }
    }
    if (nhit != 1) return "expected exactly one pointer to the DIR archiver description";
    uint8_t *st = hit - 0x10;
    uintptr_t *q = (uintptr_t *)(st + 8);
    if (!in_image(st, 0x80) || *(uint32_t *)st != 0) return "archiver version is not 0";
    if (!in_image((void *)q[0], 1) || *(const char *)q[0] != 0) return "extension is not \"\"";
    if (!in_image((void *)q[2], sizeof DIR_AUTHOR) || strcmp((const char *)q[2], DIR_AUTHOR)) return "author mismatch";
    if (*(int32_t *)(st + 0x28) != 1) return "supportsSymlinks is not 1";
    for (int k = 0; k < 9; k++)
        if (!in_text((void *)q[5 + k])) return "archiver slot outside .text";
    g_archive_fn = (void *)q[5];
    g_open_fn = (void *)q[5 + 2];
    g_stat_fn = (void *)q[5 + 7];
    if (!match(g_open_fn, OPEN_THUNK)) return "openRead is not the mov r8d,'r'; jmp thunk";
    if (!match(g_stat_fn, STAT_PROLOGUE)) return "stat prologue mismatch";
    g_err_off = -1;
    for (int o = 0; o < 0x100; o++)
        if (match((uint8_t *)g_archive_fn + o, ERR_WRITE)) {
            int32_t v;
            memcpy(&v, (uint8_t *)g_archive_fn + o + 10, 4);
            if (v > 0 && v < 0x100000) g_err_off = v;
            break;
        }
    return NULL;
}

/* ---- playset, previous error.log ---------------------------------------------------------------- */

static void add_play(const char *path, void *u)
{
    (void)u;
    if (g_nplay >= MAX_PLAY) return;
    play_t *p = &g_play[g_nplay++];
    memset(p, 0, sizeof *p);
    snprintf(p->path, sizeof p->path, "%s", path);
    ml_norm_root(path, p->norm, sizeof p->norm);
    snprintf(p->leaf, sizeof p->leaf, "%s", ml_root_leaf(p->norm));
    p->hidden = ml_in_list(cfg.skip_mods, p->leaf);
}

static char *read_file(const char *path, size_t max, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = malloc(max + 1);
    size_t n = b ? fread(b, 1, max, f) : 0;
    fclose(f);
    if (!b) return NULL;
    b[n] = 0;
    if (len) *len = n;
    return b;
}

static int file_exists_u8(const char *dir, const char *rel)
{
    char p[1400];
    wchar_t w[1400];
    snprintf(p, sizeof p, "%s/%s", dir, rel);
    for (char *c = p; *c; c++) if (*c == '/') *c = '\\';
    if (MultiByteToWideChar(CP_UTF8, 0, p, -1, w, 1400) <= 0) return 0;
    return GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
}

/* attributes each error.log entry with a file path to the highest-priority playset mod providing it */
static void scan_errlog(void)
{
    char path[700];
    snprintf(path, sizeof path, "%s/logs/error.log", g_docs_native);
    size_t n = 0, pos = 0;
    char *t = read_file(path, 8u << 20, &n);
    if (t) {
        char rel[512];
        int kind;
        while (ml_errlog_next(t, &pos, rel, sizeof rel, &kind)) {
            if (!rel[0]) continue;
            for (int i = g_nplay - 1; i >= 0; i--)
                if (file_exists_u8(g_play[i].path, rel)) {
                    play_t *p = &g_play[i];
                    if (p->errs < 2) {
                        snprintf(p->sample[p->errs], sizeof p->sample[0], "[%s] %s",
                                 kind == ML_ERR_PARSE ? "parse" : kind == ML_ERR_DUPKEY ? "dup" : kind == ML_ERR_LOC ? "loc" : "other", rel);
                    }
                    p->errs++;
                    p->parse += kind == ML_ERR_PARSE;
                    p->dup += kind == ML_ERR_DUPKEY;
                    p->loc += kind == ML_ERR_LOC;
                    break;
                }
        }
        free(t);
    }
    if (cfg.skip_fatal)
        for (int i = 0; i < g_nplay; i++)
            if (g_play[i].parse >= cfg.fatal_min && !g_play[i].hidden) {
                g_play[i].hidden = 1;
                if (api) slog("skip_fatal: hiding mod %s (%d parse errors last run)", g_play[i].leaf, g_play[i].parse);
            }
    InterlockedExchange(&g_errlog_done, 1);
}

static DWORD WINAPI errlog_thread(void *u) { (void)u; scan_errlog(); return 0; }

/* ---- report -------------------------------------------------------------------------------------- */

static root_t *root_of_play(int i)
{
    for (LONG k = 0; k < g_nroots; k++)
        if (g_roots[k].play == i) return &g_roots[k];
    return NULL;
}

static void totals(LONG64 *t)   /* st_n st_miss op_n op_miss miss_qpc would_n would_qpc skipped mismatch fp verified */
{
    memset(t, 0, sizeof(LONG64) * 11);
    for (LONG k = 0; k < g_nroots; k++) {
        root_t *r = &g_roots[k];
        t[0] += r->st_n; t[1] += r->st_miss; t[2] += r->op_n; t[3] += r->op_miss; t[4] += r->miss_qpc;
        t[5] += r->would_n; t[6] += r->would_qpc; t[7] += r->skipped; t[8] += r->mismatch; t[9] += r->fp;
        t[10] += r->verified;
    }
}

static void summary_line(char *out, size_t n)
{
    LONG64 t[11];
    totals(t);
    int ix = 0;
    for (LONG k = 0; k < g_nroots; k++) ix += g_roots[k].state == IX_READY;
    snprintf(out, n,
             "mode=%s%s roots %ld (indexed %d) | stat %lld (miss %lld) open %lld (miss %lld) | time in misses %.0f ms | "
             "index-predicted misses %lld (%.0f ms) | skipped %lld (verified %lld) | coverage %.1f%% of misses | "
             "mismatches %lld, false hits %lld",
             cfg.mode == ML_ON ? "on" : cfg.mode == ML_MEASURE ? "measure" : "off", g_disabled ? " (cache DISABLED)" : "",
             g_nroots, ix, t[0], t[1], t[2], t[3], qms(t[4]), t[5], qms(t[6]), t[7], t[10],
             100.0 * (double)(t[5] + t[7] - t[10]) / (double)(t[1] + t[3] + t[7] - t[10] > 0 ? t[1] + t[3] + t[7] - t[10] : 1),
             t[8], t[9]);
}

static int has_higher(int play, const char *key, size_t kl, int *who)
{
    for (int j = g_nplay - 1; j > play; j--) {
        root_t *h = root_of_play(j);
        if (h && h->state == IX_READY && ml_hv_has(&h->hv, ml_hash(key, kl))) { *who = j; return 1; }
    }
    return 0;
}

static void write_report(void)
{
    char path[700], line[700];
    snprintf(path, sizeof path, "%smod_report.txt", api->v3x_dir());
    FILE *f = fopen(path, "w");
    if (!f) return;
    SYSTEMTIME tm;
    GetLocalTime(&tm);
    fprintf(f, "# V3X mod_loader report (%s) %04d-%02d-%02d %02d:%02d:%02d, after: %s\n", api->game_version(),
            tm.wYear, tm.wMonth, tm.wDay, tm.wHour, tm.wMinute, tm.wSecond, g_last_event[0] ? g_last_event : "-");
    fprintf(f, "# PhysFS DIR stat=%p openRead=%p err_off=0x%x | docs %s\n", g_stat_fn, g_open_fn, (unsigned)g_err_off, g_docs_native);
    summary_line(line, sizeof line);
    fprintf(f, "%s\n", line);
    {
        int nc = 0, nb = 0; size_t mem = 0; double bms = 0;
        for (LONG k = 0; k < g_nroots; k++) {
            root_t *r = &g_roots[k];
            if (r->state != IX_READY) continue;
            if (r->from_cache) nc++; else nb++;
            bms += r->build_ms;
            mem += (size_t)r->hv.cap * 8 + r->dirs.ncap + (size_t)r->dirs.cap * 12;
        }
        fprintf(f, "# index: %d root(s) reused from mod_loader.index, %d built, %.1f ms total, %zu KB in memory | %s\n",
                nc, nb, bms, mem / 1024, g_cache_note);
    }

    fprintf(f, "\n## playset (content_load.json, %d mods; LAST = searched first = wins file conflicts)\n", g_nplay);
    for (int i = 0; i < g_nplay; i++) {
        play_t *p = &g_play[i];
        root_t *r = root_of_play(i);
        fprintf(f, "  %2d %-14s %-9s %s | prev-run errors %d (parse %d, dup %d, loc %d)%s%s%s\n", i, p->leaf,
                r ? ml_class_name(r->cls) : "-", r ? (r->hidden ? "HIDDEN" : "mounted") : "NOT MOUNTED (no VFS root seen)",
                p->errs, p->parse, p->dup, p->loc, p->sample[0][0] ? "  e.g. " : "", p->sample[0], p->hidden && !r ? " (skip requested)" : "");
    }
    for (int i = 0; i < g_nplay; i++)
        for (int j = 0; j < i; j++)
            if (!strcmp(g_play[i].norm, g_play[j].norm)) fprintf(f, "  !! %s is listed twice\n", g_play[i].leaf);

    fprintf(f, "\n## search-path roots seen (%ld)\n", g_nroots);
    fprintf(f, "  %-24s %-8s %-14s %9s %9s %9s %9s %10s %10s %9s %5s\n", "root", "class", "index", "stat", "stat_miss",
            "open", "open_miss", "miss_ms", "predict_ms", "skipped", "mism");
    for (LONG k = 0; k < g_nroots; k++) {
        root_t *r = &g_roots[k];
        char ix[40];
        if (r->state == IX_READY) snprintf(ix, sizeof ix, "%uk/%.1fms%s", r->hv.n, r->build_ms, r->from_cache ? "/c" : "");
        else snprintf(ix, sizeof ix, "%s", r->hidden ? "hidden" : r->state == IX_PENDING ? "building" : r->state == IX_FAILED ? "FAILED" : "-");
        fprintf(f, "  %-24.24s %-8s %-14s %9lld %9lld %9lld %9lld %10.1f %10.1f %9lld %5lld\n", ml_root_leaf(r->norm),
                ml_class_name(r->cls), ix, r->st_n, r->st_miss, r->op_n, r->op_miss, qms(r->miss_qpc), qms(r->would_qpc),
                r->skipped, r->mismatch);
    }
    LONG nu = g_nunpred;
    LONG64 un = 0;
    for (LONG k = 0; k < g_nroots; k++) un += g_roots[k].unpred_n;
    fprintf(f, "\n## misses the index could not predict: %lld (first %ld)\n", un, nu < cfg.log_misses ? nu : cfg.log_misses);
    for (LONG i = 0; i < nu && i < cfg.log_misses && i < 1024; i++) fprintf(f, "  %s\n", g_unpred[i]);
    LONG nm = g_nmismatch;
    fprintf(f, "\n## index mismatches (must be 0; any = cache unsafe): %ld\n", nm);
    for (LONG i = 0; i < nm && i < 16; i++) fprintf(f, "  %s\n", g_mismatch[i]);

    /* overrides between playset mods, and mod files that replace vanilla / DLC files */
    fprintf(f, "\n## overrides (same path in several roots, from the indexes; winner first)\n");
    int listed = 0, mm = 0;
    for (int i = 0; i < g_nplay; i++) {
        root_t *r = root_of_play(i);
        if (!r || r->state != IX_READY) continue;
        int over_game = 0, over_dlc = 0, nobom = 0, idx = 0, flags;
        const char *key;
        ml_set ks = {0};
        if (!list_keys(r, &ks)) { ml_set_free(&ks); fprintf(f, "  %s: cannot list files\n", g_play[i].leaf); continue; }
        while ((idx = ml_set_next(&ks, idx, &key, &flags)) >= 0) {
            idx++;
            if (flags & ML_SET_DIR) continue;
            size_t kl = strlen(key);
            if (!strncmp(key, ".metadata/", 10) || !strncmp(key, ".git/", 5) || !strchr(key, '/')) continue;
            int who;
            if (has_higher(i, key, kl, &who)) {
                mm++;
                if (listed++ < 60) fprintf(f, "  mod>mod  %s: %s > %s\n", key, g_play[who].leaf, g_play[i].leaf);
                continue;
            }
            for (LONG k = 0; k < g_nroots; k++)
                if (g_roots[k].cls == ML_ROOT_DLC && g_roots[k].state == IX_READY && ml_hv_has(&g_roots[k].hv, ml_hash(key, kl))) { over_dlc++; break; }
            char gdir[700];
            snprintf(gdir, sizeof gdir, "%s/game", g_game_native);
            if (file_exists_u8(gdir, key)) over_game++;
            if (!strncmp(key, "localization/", 13) && kl > 4 && !strcmp(key + kl - 4, ".yml")) {
                char full[1400];
                snprintf(full, sizeof full, "%s/%s", r->norm, key);
                wchar_t w[1400];
                unsigned char bom[3] = {0};
                if (MultiByteToWideChar(CP_UTF8, 0, full, -1, w, 1400) > 0) {
                    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
                    DWORD got = 0;
                    if (h != INVALID_HANDLE_VALUE) { ReadFile(h, bom, 3, &got, NULL); CloseHandle(h); }
                    if (got == 3 && !(bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF)) {
                        nobom++;
                        fprintf(f, "  !! %s: %s has no UTF-8 BOM (the engine needs it for localization)\n", g_play[i].leaf, key);
                    }
                }
            }
        }
        ml_set_free(&ks);
        if (over_game || over_dlc || nobom)
            fprintf(f, "  %-14s replaces %d vanilla file(s), %d DLC file(s)%s\n", g_play[i].leaf, over_game, over_dlc,
                    nobom ? " | localization without BOM listed above" : "");
    }
    fprintf(f, "  mod>mod total %d%s\n", mm, listed > 60 ? " (first 60 listed)" : "");
    fprintf(f, "\nDuplicate database keys / loc keys / encoding details: python tools/re_modload_conflicts.py\n");
    fclose(f);
    InterlockedIncrement(&g_reports);
}

static void CALLBACK report_cb(PTP_CALLBACK_INSTANCE inst, void *ctx)
{
    (void)inst; (void)ctx;
    AcquireSRWLockExclusive(&g_lock);   /* serializes writers; root_get() waits only on new roots */
    if (cfg.index_cache && g_dirty) {
        size_t b = cache_save();
        if (api) slog("index file %s (%zu bytes)", b ? "written" : "NOT written", b);
    }
    if (cfg.report) write_report();
    ReleaseSRWLockExclusive(&g_lock);
}

static void on_transition(const char *name, void *data, void *user)
{
    (void)name; (void)user;
    const V3X_LoadTransition *t = data;
    snprintf(g_last_event, sizeof g_last_event, "transition %s->%s", t && t->from ? t->from : "?", t && t->to ? t->to : "?");
    char line[700];
    summary_line(line, sizeof line);
    slog("%s: %s", g_last_event, line);
    if (cfg.report || cfg.index_cache) TrySubmitThreadpoolCallback(report_cb, NULL, NULL);
}

static void cmd_modload(const char *args, void *user)
{
    (void)args; (void)user;
    char line[700];
    summary_line(line, sizeof line);
    api->print("mod_loader: %s", line);
    snprintf(g_last_event, sizeof g_last_event, "console command");
    report_cb(NULL, NULL);
    api->print("report: %smod_report.txt", api->v3x_dir());
}

/* ---- load ---------------------------------------------------------------------------------------- */

static void paths_init(void)
{
    char g[700];
    snprintf(g, sizeof g, "%s", api->game_dir());               /* .../Victoria 3/binaries/ */
    size_t n = strlen(g);
    while (n && (g[n - 1] == '\\' || g[n - 1] == '/')) g[--n] = 0;
    char *s = strrchr(g, '\\'), *s2 = strrchr(g, '/');
    if (s2 > s) s = s2;
    if (s) *s = 0;
    snprintf(g_game_native, sizeof g_game_native, "%s", g);
    ml_norm_root(g, g_game_norm, sizeof g_game_norm);
    wchar_t docs[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, docs) == S_OK) {   /* Documents */
        char d[600];
        WideCharToMultiByte(CP_UTF8, 0, docs, -1, d, sizeof d, NULL, NULL);
        snprintf(g_docs_native, sizeof g_docs_native, "%s/Paradox Interactive/Victoria 3", d);
        ml_norm_root(g_docs_native, g_docs_norm, sizeof g_docs_norm);
    }
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    QueryPerformanceFrequency(&g_freq);
    ml_config_defaults(&cfg);
    char p[700], err[160];
    snprintf(p, sizeof p, "%smod_loader.ini", api->v3x_dir());
    char *ini = read_file(p, 64 * 1024, NULL);
    if (ini) {
        int bad = ml_config_parse(&cfg, ini, err, sizeof err);
        if (bad) slog("mod_loader.ini: %d bad line(s), first: %s", bad, err);
        free(ini);
    }
    if (cfg.mode == ML_OFF) { slog("mode=off: nothing hooked"); return 0; }
    paths_init();
    if (cfg.index_cache) { cache_load(); slog("%s", g_cache_note); }
    snprintf(p, sizeof p, "%s\\content_load.json", g_docs_native);
    char *js = read_file(p, 1u << 20, NULL);
    int np = js ? ml_parse_playset(js, add_play, NULL) : -1;
    free(js);

    const char *why = resolve();
    uintptr_t b = api->image_base();
    if (why) {
        slog("PhysFS DIR archiver not resolved (%s) - mod_loader inactive", why);
        return 0;
    }
    slog("PhysFS DIR archiver: stat rva %llx, openRead rva %llx, error slot +0x%x%s", (unsigned long long)((uintptr_t)g_stat_fn - b),
             (unsigned long long)((uintptr_t)g_open_fn - b), (unsigned)g_err_off, g_err_off < 0 ? " NOT FOUND" : "");
    if (g_err_off < 0 && cfg.mode == ML_ON) {
        cfg.mode = ML_MEASURE;
        slog("mode=on REFUSED: cannot set the PhysFS error code exactly like a real miss -> measure");
    }
    int want_hide = cfg.skip_fatal || cfg.skip_mods[0];
    if (want_hide && g_err_off < 0) slog("skip_mods/skip_fatal REFUSED (no PhysFS error slot)");
    if (cfg.skip_fatal) scan_errlog();                 /* must be done before the first mount */
    else if (cfg.report) { HANDLE th = CreateThread(NULL, 0, errlog_thread, NULL, 0, NULL); if (th) CloseHandle(th); }
    int s1 = api->hook(g_stat_fn, (void *)stat_detour, (void **)&o_stat);
    int s2 = s1 ? -1 : api->hook(g_open_fn, (void *)open_detour, (void **)&o_open);
    if (s1 || s2) {
        slog("hook failed (stat %d, openRead %d) - inactive", s1, s2);
        if (!s1) api->unhook(g_stat_fn);
        return 0;
    }
    g_active = 1;
    if (api->api_version >= 3 && api->struct_size >= sizeof(V3X_API)) {
        api->on_event("load_transition", on_transition, NULL);
        api->register_command("modload", "mod_loader: VFS probe stats + write v3x/mod_report.txt", cmd_modload, NULL);
    }
    slog("ready: mode=%s, playset %d mod(s), index workshop=%d dlc=%d local=%d game=%d packs=%d, verify_every=%d, skip_fatal=%d skip_mods='%s'",
             cfg.mode == ML_ON ? "on" : "measure", np, cfg.index_workshop, cfg.index_dlc, cfg.index_local,
             cfg.index_game, cfg.index_packs, cfg.verify_every,
             cfg.skip_fatal, cfg.skip_mods);
    return 0;
}
