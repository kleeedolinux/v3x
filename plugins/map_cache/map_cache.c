/* map_cache: measure (and optionally stop) repeated texture disk reads behind the lag on map-mode
 * switches and GUI views such as decrees. See docs/re/mapmode.md. RVAs are 1.15.1 comments only:
 * every target is found by a string anchor + byte shape; any mismatch = fail closed (no hooks).
 *
 * Engine (static RE):
 *   GetOrLoadTexture 0x3c5b4f0(out, path, flags): find in the global texture cache 0x5421820 (0x3c7f100),
 *       else CreateTexture + insert. The cache is WEAK: find treats an entry whose refcount [tex+8] is 0
 *       as a miss and erases it, so a texture is destroyed as soon as the last user drops it and the next
 *       request reads the file from disk again.
 *   CreateTexture 0x3c5af20(out, path, ?, flags): opens the file (vfs 0x3e1c4f0), size = file vslot +0x20,
 *       reserves a byte array of that size (0x847fe0 at +0x20f, returns to +0x214), reads, parses the DDS
 *       (0x3c5e0d0), creates the GPU texture.
 *   0x847fe0 = generic CPdxArray<uint8>::Reserve (1219 callers) = alloc site 0x84800b in alloc_report.
 *
 * Hooks: GetOrLoadTexture (miss/hit + pin decision), CreateTexture (time, path), Reserve (file size when
 * called from CreateTexture+0x214; attribution of big reserves >= big_reserve_mb from every caller).
 * mode=measure: no behaviour change, logs slow loads, main-thread stalls (spikes) with the loads inside
 *   them, and a periodic summary (v3x/map_cache_report.txt).
 * mode=on: also keeps textures that were read from disk >= repeat_min times alive with one extra
 *   reference (LRU, budget_mb of file bytes, files <= max_texture_mb), so the weak cache hits.
 *   Pins are released (engine's own release sequence) on the main thread: over budget, on leaving the
 *   Game state, and on "mapcache flush". */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "v3x.h"
#include "mc_core.h"

V3X_EXPORT const char *v3x_plugin_name = "map_cache";

typedef uintptr_t (*fn4)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

static const V3X_API *api;
static mc_state g_st;
static LARGE_INTEGER g_freq;
static DWORD g_tls_gol = TLS_OUT_OF_INDEXES, g_tls_ct = TLS_OUT_OF_INDEXES;
static volatile LONG g_log_left, g_armed_pin;
static uint8_t *g_res_ret;                      /* CreateTexture + 0x214: return address of its Reserve call */
static char g_report_path[MAX_PATH];

/* ------------------------------------------------------------------ anchors (mirrored in tools/re_map_check.py) */

#define STR_CT "Could not find texture"
typedef struct { int off; const char *pat; } shape_t;
enum { A_CT, A_GOL, A_RES, A_COUNT };
typedef struct { const char *name; int span; shape_t shape[4]; uint8_t *fn; } anchor_t;
static anchor_t A[A_COUNT] = {
    { "create", 0x240, { { 0x000, "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 48 89 4C 24 08 55 41 54 41 55 41 56 41 57" },
                         { 0x02C, "41 8B F9 45 8B F0 4C 8B E2 4C 8B F9" },        /* edi=flags r14d=a3 r12=path r15=out */
                         { 0x204, "3B 5D DC 7E 0F 8B D3 48 8D 4D D0 E8" } } },    /* if size>cap: Reserve(&buf, size) */
    { "getload", 0x100, { { 0x000, "48 89 4C 24 08 53 56 57 48 83 EC 40 41 8B D8 48 8B F2 48 8B F9" },
                          { 0x035, "48 8D 0D ?? ?? ?? ?? E8" },                    /* lea rcx, texture cache; call find */
                          { 0x051, "44 8B CB 48 8B D6 48 8D 4C 24 78 E8" },        /* CreateTexture(tmp, path, ?, flags) */
                          /* refcount layout we rely on: lock xadd [tex+8],-1; ==1 -> owner [tex+0x10] vslot1(owner, tex), tex vslot1 */
                          { 0x086, "F0 0F C1 43 08 83 F8 01 75 1C 48 8B 4B 10 48 85 C9 74 09 48 8B 01 48 8B D3 FF 50 08 48 8B 03 48 8B CB FF 50 08" } } },
    { "reserve", 0x40, { { 0x000, "48 89 5C 24 10 57 48 83 EC 20 48 8B D9 8B FA 3B 51 08 7E" } } },
};
#define CT_RES_CALL 0x20F
#define GOL_CT_CALL 0x05C

static int in_image(const void *p, size_t n)
{
    uintptr_t b = api->image_base(), a = (uintptr_t)p;
    return a >= b && a + n <= b + api->image_size();
}
static uint32_t rva(const void *p) { return (uint32_t)((uintptr_t)p - api->image_base()); }
static uint8_t *rel32(const uint8_t *ins) { int32_t d; memcpy(&d, ins + 1, 4); return (uint8_t *)ins + 5 + d; }

static int match(const uint8_t *p, const char *pat)
{
    for (const char *s = pat; *s;) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (s[0] == '?') { s += s[1] == '?' ? 2 : 1; p++; continue; }
        unsigned v; if (sscanf(s, "%2x", &v) != 1 || *p != (uint8_t)v) return 0;
        s += 2; p++;
    }
    return 1;
}

static int shape_ok(const uint8_t *fn, const anchor_t *a)
{
    if (!fn || !in_image(fn, (size_t)a->span)) return 0;
    for (int i = 0; i < 4 && a->shape[i].pat; i++)
        if (!match(fn + a->shape[i].off, a->shape[i].pat)) return 0;
    return 1;
}

static uint8_t *g_text; static size_t g_text_n;
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

/* Returns the number of anchors that failed; logs each. */
static int resolve_all(void)
{
    for (int i = 0; i < A_COUNT; i++) A[i].fn = NULL;
    g_res_ret = NULL;
    if (find_text()) { api->log("map_cache", "FAIL-CLOSED: no .text section"); return A_COUNT; }
    void *s = api->find_string(STR_CT);
    for (int i = 0; s && i < 16 && !A[A_CT].fn; i++) {
        void *x = api->find_xref(s, i);
        if (!x) break;
        uint8_t *f = api->function_start(x);
        if (shape_ok(f, &A[A_CT])) A[A_CT].fn = f;
    }
    if (A[A_CT].fn) {
        uint8_t *ct = A[A_CT].fn;
        /* GetOrLoadTexture: the direct caller of CreateTexture whose call sits at +0x5c */
        for (size_t i = 0; g_text && i + 5 <= g_text_n && !A[A_GOL].fn; i++) {
            if (g_text[i] != 0xE8 || rel32(g_text + i) != ct) continue;
            uint8_t *f = api->function_start(g_text + i);
            if (f && g_text + i == f + GOL_CT_CALL && shape_ok(f, &A[A_GOL])) A[A_GOL].fn = f;
        }
        uint8_t *r = rel32(ct + CT_RES_CALL);
        if (shape_ok(r, &A[A_RES])) { A[A_RES].fn = r; g_res_ret = ct + CT_RES_CALL + 5; }
    }
    int bad = 0;
    for (int i = 0; i < A_COUNT; i++) {
        if (A[i].fn) api->log("map_cache", "anchor %-7s OK at %08x", A[i].name, rva(A[i].fn));
        else { api->log("map_cache", "FAIL-CLOSED anchor %s: not found or shape mismatch", A[i].name); bad++; }
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
    return (double)q.QuadPart * 1000.0 / (double)g_freq.QuadPart;
}
static uint64_t now_tick(void) { return GetTickCount64(); }

/* texture refcount: int at +8; final release = owner [tex+0x10] vslot1(owner, tex), then tex vslot1(tex)
 * (exactly the sequence at GetOrLoadTexture+0x86, which the anchor shape checks) */
typedef void (*rel2_fn)(void *, void *);
static void tex_addref(void *t) { InterlockedIncrement((volatile LONG *)((uint8_t *)t + 8)); }
static void tex_release(void *t)
{
    if (InterlockedDecrement((volatile LONG *)((uint8_t *)t + 8)) != 0) return;
    void *owner = *(void **)((uint8_t *)t + 0x10);
    if (owner) ((rel2_fn)(*(void ***)owner)[1])(owner, t);
    ((rel2_fn)(*(void ***)t)[1])(t, t);
}

static void release_list(void **v, int n) { for (int i = 0; i < n; i++) if (v[i]) tex_release(v[i]); }

/* ------------------------------------------------------------------ hooks */

static fn4 o_gol, o_ct, o_res;
#ifdef MC_TEST
static void *g_test_last_ret;
#endif

typedef struct { int ct; uint64_t h, bytes; uint32_t loads; } gol_ctx;
typedef struct { uint64_t bytes; } ct_ctx;

static uintptr_t h_res(uintptr_t arr, uintptr_t n, uintptr_t c, uintptr_t d)
{
    void *ret = __builtin_return_address(0);
#ifdef MC_TEST
    g_test_last_ret = ret;
#endif
    int32_t want = (int32_t)(uint32_t)n;
    if (want > 0) {
        if ((uint8_t *)ret == g_res_ret) {
            ct_ctx *x = TlsGetValue(g_tls_ct);
            if (x) x->bytes = (uint64_t)want;
        }
        if ((uint64_t)want >= ((uint64_t)g_st.cfg.big_reserve_mb << 20))
            mc_big_record(&g_st, in_image(ret, 1) ? rva(ret) : 0, (uint64_t)want);
    }
    return o_res(arr, n, c, d);
}

static uintptr_t h_ct(uintptr_t out, uintptr_t path, uintptr_t a3, uintptr_t flags)
{
    ct_ctx x = { 0 };
    void *prev = TlsGetValue(g_tls_ct);
    TlsSetValue(g_tls_ct, &x);
    double t0 = now_ms();
    uintptr_t r = o_ct(out, path, a3, flags);
    double ms = now_ms() - t0;
    TlsSetValue(g_tls_ct, prev);

    char full[512];
    int n = mc_msvc_str((const void *)path, full, sizeof full, readable);
    if (n < 0) { strcpy(full, "?"); n = 1; }
    uint64_t h = mc_hash(full, (size_t)n);
    gol_ctx *g = TlsGetValue(g_tls_gol);
    uint32_t loads = mc_record_load(&g_st, h, full, (size_t)n, x.bytes, ms, g != NULL);
    if (g) { g->ct = 1; g->h = h; g->bytes = x.bytes; g->loads = loads; }
    if ((ms >= g_st.cfg.load_log_ms || (g_st.cfg.load_log_mb && x.bytes >= ((uint64_t)g_st.cfg.load_log_mb << 20)))
        && InterlockedDecrement(&g_log_left) >= 0)
        api->log("map_cache", "load %.1f ms %.2f MB #%u%s %s tid=%lu '%s'", ms, (double)x.bytes / 1048576.0, loads,
                 loads > 1 ? " (re-read)" : "", g ? "cache-miss" : "direct", GetCurrentThreadId(), full);
    return r;
}

static uintptr_t h_gol(uintptr_t out, uintptr_t path, uintptr_t flags, uintptr_t d)
{
    gol_ctx g = { 0 };
    void *prev = TlsGetValue(g_tls_gol);
    TlsSetValue(g_tls_gol, &g);
    uintptr_t r = o_gol(out, path, flags, d);
    TlsSetValue(g_tls_gol, prev);
    mc_record_gol(&g_st, g.ct);
    if (!g_armed_pin || !out) return r;
    void *tex = *(void **)out;
    if (!tex) return r;
    if (!g.ct) { mc_pin_touch(&g_st, tex, now_tick()); return r; }
    if (!mc_should_pin(&g_st, g.loads, g.bytes)) return r;
    void *replaced = NULL;
    /* the caller's `out` holds a reference, so tex is alive here: adding one is safe */
    if (mc_pin_add(&g_st, tex, g.h, g.bytes, now_tick(), &replaced)) tex_addref(tex);
    if (replaced) tex_release(replaced);
    return r;
}

/* ------------------------------------------------------------------ frame / report / console */

static double g_last_frame;
static double g_next_summary;

static void write_summary(const char *why)
{
    static char buf[64 * 1024];
    int n = mc_format_summary(&g_st, buf, sizeof buf, 25);
    char tmp[MAX_PATH + 8]; snprintf(tmp, sizeof tmp, "%s.tmp", g_report_path);
    FILE *f = fopen(tmp, "wb");
    if (f) {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(f, "map_cache report (%s) %02d:%02d:%02d   RVAs relative to victoria3.exe\n", why, t.wHour, t.wMinute, t.wSecond);
        fwrite(buf, 1, (size_t)n, f);
        fclose(f);
        if (!MoveFileExA(tmp, g_report_path, MOVEFILE_REPLACE_EXISTING))
            api->log("map_cache", "report rename failed (%lu)", GetLastError());
    }
    char *nl = strchr(buf, '\n'); if (nl) nl = strchr(nl + 1, '\n');
    if (nl) *nl = 0;
    api->log("map_cache", "summary (%s): %s", why, buf);
}

static void flush_pins(const char *why)
{
    static void *v[MC_MAX_PINS];
    int n = mc_pin_take_all(&g_st, v, MC_MAX_PINS);
    release_list(v, n);
    if (n) api->log("map_cache", "released %d pinned textures (%s)", n, why);
}

static void on_frame(void *u)
{
    (void)u;
    double t = now_ms();
    double gap = g_last_frame > 0 ? t - g_last_frame : 0;
    g_last_frame = t;
    mc_frame_snap fs;
    mc_frame_take(&g_st, &fs);
    if (gap >= g_st.cfg.spike_ms && InterlockedDecrement(&g_log_left) >= 0)
        api->log("map_cache", "spike %.0f ms | texture disk reads in it (all threads): %d (%d re-reads), %.1f MB, %.0f ms%s%s%s",
                 gap, fs.loads, fs.reloads, fs.mb, fs.ms, fs.slow[0] ? " | slowest: '" : "", fs.slow, fs.slow[0] ? "'" : "");
    if (g_armed_pin) {
        void *v[64];
        int n = mc_pin_collect(&g_st, v, 64);
        release_list(v, n);
    }
    if (g_st.cfg.summary_sec > 0 && t >= g_next_summary) {
        if (g_next_summary > 0) write_summary("periodic");
        g_next_summary = t + g_st.cfg.summary_sec * 1000.0;
    }
}

static void main_flush(void *u) { (void)u; flush_pins("console"); }

static void cmd_mapcache(const char *args, void *u)
{
    (void)u;
    if (!_strnicmp(args, "flush", 5)) {
        if (api->run_on_main(main_flush, NULL)) api->print("map_cache: run_on_main queue full");
        else api->print("map_cache: pins will be released on the next frame");
        return;
    }
    if (!_strnicmp(args, "report", 6)) { write_summary("console"); api->print("map_cache: wrote %s", g_report_path); return; }
    static char buf[16 * 1024];
    mc_format_summary(&g_st, buf, sizeof buf, 10);
    api->print("map_cache: armed_pin=%ld\n%s", g_armed_pin, buf);
}

static void on_transition(const char *name, void *data, void *u)
{
    (void)name; (void)u;
    const V3X_LoadTransition *t = data;
    if (!t || !t->from || _stricmp(t->from, "Game")) return;
    flush_pins("left the Game state");
    write_summary("left the Game state");
}

/* ------------------------------------------------------------------ load */

static int install(int id, void *detour, fn4 *orig)
{
    int s = api->hook(A[id].fn, detour, (void **)orig);
    api->log("map_cache", "%s %s at %08x (status %d)", s ? "FAIL" : "hooked", A[id].name, rva(A[id].fn), s);
    if (s) *orig = NULL;
    return s == 0;
}

static char *load_ini(void)
{
    char path[MAX_PATH]; snprintf(path, sizeof path, "%smap_cache.ini", api->v3x_dir());
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = n >= 0 && n < (1 << 20) ? malloc((size_t)n + 1) : NULL;
    if (b) { size_t r = fread(b, 1, (size_t)n, f); b[r] = 0; }
    fclose(f);
    return b;
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    mc_config cfg; mc_config_defaults(&cfg);
    char *ini = load_ini();
    if (ini) {
        char err[160];
        int bad = mc_config_parse(&cfg, ini, err, sizeof err);
        if (bad) api->log("map_cache", "map_cache.ini: %d bad line(s), first: %s", bad, err);
        free(ini);
    }
    mc_init(&g_st, &cfg);
    g_log_left = cfg.log_budget;
    if (cfg.mode == MC_OFF) { api->log("map_cache", "mode=off: no hooks installed"); return 0; }
    QueryPerformanceFrequency(&g_freq);
    snprintf(g_report_path, sizeof g_report_path, "%smap_cache_report.txt", api->v3x_dir());
    g_tls_gol = TlsAlloc(); g_tls_ct = TlsAlloc();
    if (g_tls_gol == TLS_OUT_OF_INDEXES || g_tls_ct == TLS_OUT_OF_INDEXES) { api->log("map_cache", "FAIL-CLOSED: no TLS slot"); return 0; }
    if (resolve_all()) { api->log("map_cache", "FAIL-CLOSED: anchors missing, nothing installed"); return 0; }
    /* order: leaf first, so no detour can see a half-installed chain */
    if (!install(A_RES, (void *)h_res, &o_res)) { api->log("map_cache", "FAIL-CLOSED: reserve hook failed"); return 0; }
    if (!install(A_CT, (void *)h_ct, &o_ct)) { api->unhook(A[A_RES].fn); api->log("map_cache", "FAIL-CLOSED: create hook failed"); return 0; }
    if (!install(A_GOL, (void *)h_gol, &o_gol)) {
        api->unhook(A[A_CT].fn); api->unhook(A[A_RES].fn);
        api->log("map_cache", "FAIL-CLOSED: getload hook failed"); return 0;
    }
    if (cfg.mode == MC_ON) InterlockedExchange(&g_armed_pin, 1);
    api->log("map_cache", "mode=%s spike_ms=%d load_log_ms=%d load_log_mb=%d big_reserve_mb=%d%s",
             mc_mode_name(cfg.mode), cfg.spike_ms, cfg.load_log_ms, cfg.load_log_mb, cfg.big_reserve_mb,
             cfg.mode == MC_ON ? "" : " (no behaviour change)");
    if (cfg.mode == MC_ON)
        api->log("map_cache", "keep-alive armed: budget_mb=%d max_texture_mb=%d repeat_min=%d max_pins=%d",
                 cfg.budget_mb, cfg.max_texture_mb, cfg.repeat_min, g_st.cfg.max_pins);
    if (api->api_version >= 2 && api->struct_size >= offsetof(V3X_API, register_command) + sizeof(void *) && api->register_command)
        api->register_command("mapcache", "map_cache: status | report (write v3x/map_cache_report.txt) | flush (release pins)", cmd_mapcache, NULL);
    if (api->api_version >= 3 && api->struct_size >= offsetof(V3X_API, on_event) + sizeof(void *) && api->on_event)
        api->on_event("load_transition", on_transition, NULL);
    api->on_frame(on_frame, NULL);
    return 0;
}
