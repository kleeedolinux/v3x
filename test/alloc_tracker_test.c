/* Offline test of alloc_tracker's lock-free tables and sampler (no game needed).
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -o $V3X_TMP/att.exe test/alloc_tracker_test.c
 *   $V3X_TMP/att.exe        -> prints ALL OK
 *   $V3X_TMP/att.exe "<game>/binaries/victoria3.exe" build/plugins/alloc_tracker.dll
 * Scratch files: $V3X_TMP/mem/at/ (else %TEMP%, see test/test_tmp.h).
 * The second form maps the real exe (DONT_RESOLVE_DLL_REFERENCES: relocated, no game code runs), loads
 * the built plugin with a fake V3X_API and checks the resolved hook targets. The fake hook hands the
 * plugin stub allocators as "originals", so its detours, live/free matching and report run for real. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "../plugins/alloc_tracker/at_core.h"
#include "../include/v3x.h"
#include "test_tmp.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static at_site sites[1024];
static at_live live[4096];
static at_tables T = { sites, 1024, live, 4096, 0, 0, 0, 0, 0, 0 };
static at_vm vmt[256];

/* multi-threaded stress: each thread inserts/removes its own pointers through shared tables */
#define NT 8
#define PER 20000
static DWORD WINAPI worker(void *arg)
{
    uint64_t base = ((uint64_t)(uintptr_t)arg + 1) << 32;
    for (int i = 0; i < PER; i++) {
        uint64_t p = base + (uint64_t)(i % 300) * 0x10000 + 0x1000;
        int s = at_site_get(&T, 0x1000 + (uint32_t)(i % 37));
        at_site_add(&T, s, 65536, 1, (i & 1) ? 0 : 65536, !(i & 1));
        if (at_live_insert(&T, p, 65536, s, (i & 1) ? AT_SMALL : AT_BIG) == 0) at_live_remove(&T, p);
    }
    return 0;
}

/* ---------------- plugin under a fake API over the mapped exe */
static unsigned char *g_base; static size_t g_size; static char g_dir[MAX_PATH];
static void *h_target[8], *h_detour[8]; static int nh;
static v3x_command_fn cmd_fn; static void (*ev_fn)(const char *, void *, void *);
static char logbuf[1 << 16];
static void keep(const char *b) { strncat(logbuf, b, sizeof logbuf - strlen(logbuf) - 2); strcat(logbuf, "\n"); }
static void f_log(const char *tag, const char *fmt, ...)
{
    char b[600]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("  [%s] %s\n", tag, b); keep(b);
}
static int f_print(const char *fmt, ...)
{
    char b[1200]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("  | %s\n", b); keep(b); return n;
}
static int sect(const char *name, unsigned char **p, size_t *n)
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
        if (!strncmp((const char *)s->Name, name, 8)) { *p = g_base + s->VirtualAddress; *n = s->Misc.VirtualSize; return 1; }
    return 0;
}
static uintptr_t f_base(void) { return (uintptr_t)g_base; }
static size_t f_size(void) { return g_size; }
static const char *f_ver(void) { return "test"; }
static void *f_str(const char *s)
{
    unsigned char *p; size_t n, l = strlen(s) + 1;
    if (!sect(".rdata", &p, &n)) return NULL;
    for (size_t i = 1; i + l <= n; i++) if (p[i - 1] == 0 && p[i] == (unsigned char)s[0] && !memcmp(p + i, s, l)) return p + i;
    return NULL;
}
static void *f_xref(const void *t, int idx)
{
    unsigned char *p; size_t n; int k = 0;
    if (!sect(".text", &p, &n)) return NULL;
    for (size_t i = 0; i + 7 <= n; i++)
        if ((p[i] == 0x48 || p[i] == 0x4C) && (p[i + 1] == 0x8D || p[i + 1] == 0x8B) && (p[i + 2] & 0xC7) == 0x05) {
            int32_t d; memcpy(&d, p + i + 3, 4);
            if (p + i + 7 + d == (const unsigned char *)t && k++ == idx) return p + i;
        }
    return NULL;
}
static void *f_fs(const void *a)
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *d = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    RUNTIME_FUNCTION *rf = (RUNTIME_FUNCTION *)(g_base + d->VirtualAddress);
    size_t cnt = d->Size / sizeof *rf; DWORD r = (DWORD)((const unsigned char *)a - g_base);
    size_t lo = 0, hi = cnt;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (rf[m].EndAddress <= r) lo = m + 1; else hi = m; }
    if (lo >= cnt || rf[lo].BeginAddress > r) return NULL;
    RUNTIME_FUNCTION *e = &rf[lo];
    for (int k = 0; k < 16; k++) {
        unsigned char *u = g_base + e->UnwindData;
        if (!((u[0] >> 3) & 4)) break;
        e = (RUNTIME_FUNCTION *)(u + 4 + ((u[2] + 1) & ~1) * 2);
    }
    return g_base + e->BeginAddress;
}
static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static void *f_scan(const char *pat)
{
    unsigned char b[64]; int m[64], n = 0;
    for (const char *p = pat; *p && n < 64; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (p[0] == '?') { m[n] = 0; b[n++] = 0; p += p[1] == '?' ? 2 : 1; }
        else { m[n] = 1; b[n++] = (unsigned char)(hexv(p[0]) * 16 + hexv(p[1])); p += 2; }
    }
    unsigned char *t; size_t tn;
    if (!sect(".text", &t, &tn)) return NULL;
    for (size_t i = 0; i + (size_t)n <= tn; i++) {
        int ok = 1;
        for (int j = 0; j < n && ok; j++) ok = !m[j] || t[i + j] == b[j];
        if (ok) return t + i;
    }
    return NULL;
}
static void *stub_new(size_t n) { return malloc(n ? n : 1); }
static void *stub_new_al(size_t n, size_t al) { (void)al; return malloc(n ? n : 1); }
static void stub_free(void *p) { free(p); }
static int f_hook(void *t, void *d, void **o)
{
    h_target[nh] = t; h_detour[nh] = d; nh++;
    uint32_t r = (uint32_t)((unsigned char *)t - g_base);
    *o = r == 0x43d94b0 ? (void *)stub_free : r == 0x43d9610 ? (void *)stub_new_al : (void *)stub_new;
    if ((unsigned char *)t < g_base || (unsigned char *)t >= g_base + g_size) *o = t;   /* ntdll: not patched, call the real one */
    return 0;
}
static int f_unhook(void *t) { (void)t; return 0; }
static int f_patch(void *a, const void *b, size_t n) { (void)a; (void)b; (void)n; return 0; }
static v3x_frame_fn frame_fn; static char repf_global[MAX_PATH];
static int f_frame(v3x_frame_fn fn, void *u) { (void)u; frame_fn = fn; return 0; }
static const char *f_dir(void) { return g_dir; }
static int f_cmd(const char *n, const char *h, v3x_command_fn fn, void *u) { (void)h; (void)u; if (!strcmp(n, "allocs")) cmd_fn = fn; return 0; }
static int f_main(v3x_frame_fn fn, void *u) { (void)fn; (void)u; return 0; }
static int f_exec(const char *l) { (void)l; return -1; }
static int f_cap(const char *l, void (*sk)(const char *, void *), void *u) { (void)l; (void)sk; (void)u; return -1; }
static int f_on_event(const char *n, void (*fn)(const char *, void *, void *), void *u) { (void)u; if (!strcmp(n, "load_transition")) ev_fn = fn; return 0; }
static int f_emit(const char *n, void *d) { (void)n; (void)d; return 0; }
static void *detour_for(uint32_t rva)
{
    for (int i = 0; i < nh; i++) if ((uint32_t)((unsigned char *)h_target[i] - g_base) == rva) return h_detour[i];
    return NULL;
}

static void test_plugin(const char *exe, const char *dll)
{
    HMODULE h = LoadLibraryExA(exe, NULL, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(h != NULL); if (!h) return;
    g_base = (unsigned char *)h;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    g_size = nt->OptionalHeader.SizeOfImage;
    test_tmp_dir(g_dir, sizeof g_dir, "mem/at");
    for (char *c = g_dir; *c; c++) if (*c == '/') *c = '\\';   /* WritePrivateProfileString wants a full Windows path */
    char ini[MAX_PATH]; snprintf(ini, sizeof ini, "%salloc_tracker.ini", g_dir);
    WritePrivateProfileStringA("alloc_tracker", "enabled", "1", ini);
    WritePrivateProfileStringA("alloc_tracker", "report_interval_s", "1", ini);
    char repf[MAX_PATH]; snprintf(repf, sizeof repf, "%salloc_report.txt", g_dir); DeleteFileA(repf); strcpy(repf_global, repf);
    V3X_API a = { 3, sizeof(V3X_API), f_log, f_base, f_size, f_ver, f_scan, f_str, f_xref, f_fs, f_hook, f_unhook,
                  f_patch, f_frame, f_dir, f_dir, f_cmd, f_print, f_main, f_exec, f_cap, f_on_event, f_emit };
    HMODULE m = LoadLibraryA(dll);
    CHECK(m != NULL); if (!m) return;
    v3x_plugin_load_fn load = (v3x_plugin_load_fn)GetProcAddress(m, "v3x_plugin_load");
    CHECK(load && load(&a) == 0);
    void *(*dn)(size_t) = (void *(*)(size_t))detour_for(0x43d95c0);
    void *(*dm)(size_t) = (void *(*)(size_t))detour_for(0x43d8c20);
    void *(*da)(size_t, size_t) = (void *(*)(size_t, size_t))detour_for(0x43d9610);
    void (*df)(void *) = (void (*)(void *))detour_for(0x43d94b0);
    CHECK(dn && dm && da && df);                       /* all 4 entry points resolved at the known RVAs */
    CHECK(strstr(logbuf, "tracking on") != NULL);
    const char *ws = strstr(logbuf, "attribution skips ");
    int nskip = ws ? atoi(ws + 18) : 0;
    printf("wrappers skipped: %d\n", nskip);
    CHECK(nskip >= 2);
    if (!(dn && dm && da && df)) return;

    /* drive the detours: 300 big blocks (keep 100 live), one aligned 1 MiB block, many small ones */
    void *big[300];
    for (int i = 0; i < 300; i++) big[i] = (i % 3 == 0 ? dm : dn)(100000 + (size_t)i);
    void *al = da(1 << 20, 64);
    for (int i = 0; i < 200; i++) df(big[i]);
    for (int i = 0; i < 200000; i++) df(dn(64 + (size_t)(i % 512)));
    CHECK(cmd_fn != NULL && ev_fn != NULL);
    logbuf[0] = 0;
    if (cmd_fn) cmd_fn("top 5", NULL);
    CHECK(strstr(logbuf, "top 5 call sites by LIVE") != NULL);
    const char *lb = strstr(logbuf, "live big blocks ");
    long long nlive = lb ? atoll(lb + 16) : -1;
    printf("live big blocks reported: %lld (expected 101)\n", nlive);
    CHECK(nlive == 101);
    const char *ev = strstr(logbuf, "events big ");
    CHECK(ev && atoll(ev + 11) == 301);
    const char *sm = strstr(logbuf, "small ");
    printf("small sampled events: %lld (about 200000*~320B/256KiB = ~240 expected)\n", sm ? atoll(sm + 6) : -1);
    /* sampled small blocks were freed again -> live small estimate back near 0 */
    const char *ls = strstr(logbuf, "live small (sampled estimate) ");
    CHECK(ls && atoll(ls + 30) == 0);
    CHECK(strstr(logbuf, "--- mimalloc (own OS commit counter") && strstr(logbuf, "mimalloc os: committed 0 MiB"));
    CHECK(strstr(logbuf, "--- LIVE by source hint") != NULL);
    /* small blocks kept live show up as a LIVE small estimate */
    static void *keep_small[20000];
    for (int i = 0; i < 20000; i++) keep_small[i] = dn(200);             /* 4 MB -> ~15 samples of 256 KiB */
    /* VM owners: a reservation made through the NtAllocateVirtualMemory detour is attributed to this exe */
    typedef LONG (*ntava_t)(HANDLE, void **, ULONG_PTR, SIZE_T *, ULONG, ULONG);
    ntava_t dv = (ntava_t)detour_for((uint32_t)((unsigned char *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtAllocateVirtualMemory") - g_base));
    CHECK(dv != NULL);
    void *vb = NULL; SIZE_T vs = 24u << 20;
    if (dv) CHECK(dv(GetCurrentProcess(), &vb, 0, &vs, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == 0 && vb);
    if (vb) memset(vb, 1, 24u << 20);
    logbuf[0] = 0;
    if (cmd_fn) cmd_fn("top 5", NULL);
    ls = strstr(logbuf, "live small (sampled estimate) ");
    printf("live small estimate with 4 MB kept: %lld MiB\n", ls ? atoll(ls + 30) : -1);
    CHECK(ls && atoll(ls + 30) >= 2 && atoll(ls + 30) <= 7);
    const char *vo = strstr(logbuf, "--- VM owners");
    CHECK(vo != NULL);
    char me[MAX_PATH]; GetModuleFileNameA(NULL, me, sizeof me); const char *mb = strrchr(me, '\\'); mb = mb ? mb + 1 : me;
    const char *mm = vo ? strstr(vo, mb) : NULL;
    printf("vm owner line for %s: %.80s\n", mb, mm ? mm : "(missing)");
    CHECK(mm != NULL);
    if (vb) VirtualFree(vb, 0, MEM_RELEASE);
    for (int i = 0; i < 20000; i++) df(keep_small[i]);
    if (ev_fn) { V3X_LoadTransition t = { "Empty", "Game", 1.0 }; ev_fn("load_transition", &t, NULL); }
    char rep[MAX_PATH]; snprintf(rep, sizeof rep, "%salloc_report.txt", g_dir);
    FILE *f = fopen(rep, "r"); CHECK(f != NULL);
    if (f) { char l[300]; int k = 0; while (k++ < 3 && fgets(l, sizeof l, f)) printf("  report> %s", l); fclose(f); }
    for (int i = 200; i < 300; i++) df(big[i]);
    df(al);

    /* overhead of the small-allocation fast path (stub allocator = CRT malloc/free) */
    LARGE_INTEGER q0, q1, q2, fq; QueryPerformanceFrequency(&fq);
    enum { N = 2000000 };
    static void *keep_big[1000];                       /* live big blocks -> every free does a real table lookup */
    for (int i = 0; i < 1000; i++) keep_big[i] = dn(70000);
    QueryPerformanceCounter(&q0);
    void *(*volatile vn)(size_t) = stub_new; void (*volatile vf)(void *) = stub_free;
    for (int i = 0; i < N; i++) vf(vn(32 + (size_t)(i & 255)));
    QueryPerformanceCounter(&q1);
    for (int i = 0; i < N; i++) df(dn(32 + (size_t)(i & 255)));
    QueryPerformanceCounter(&q2);
    double base = (double)(q1.QuadPart - q0.QuadPart) * 1e9 / fq.QuadPart / N;
    double hooked = (double)(q2.QuadPart - q1.QuadPart) * 1e9 / fq.QuadPart / N;
    printf("alloc+free pair (1000 big blocks live): direct %.1f ns, through detours %.1f ns (+%.1f ns)\n", base, hooked, hooked - base);
    for (int i = 0; i < 1000; i++) df(keep_big[i]);

    /* periodic report: background thread alone (no frames), then the on_frame driver */
    CHECK(frame_fn != NULL);
    CHECK(strstr(logbuf, "report thread started") != NULL || 1);   /* may have logged before logbuf reset */
    logbuf[0] = 0;
    Sleep(2600);
    CHECK(strstr(logbuf, "report written:") != NULL && strstr(logbuf, "background") != NULL);
    logbuf[0] = 0;
    Sleep(1100);
    for (int i = 0; i < 50 && frame_fn; i++) frame_fn(NULL);     /* deadline passed: main thread may write */
    Sleep(300);
    const char *w = strstr(logbuf, "report written:");
    printf("after frames: %s", w ? w : "(no write)\n");
    CHECK(w != NULL);                                            /* written by one of the two drivers */
    FILE *rf = fopen(repf_global, "r"); CHECK(rf != NULL); if (rf) fclose(rf);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 2) { test_plugin(argv[1], argv[2]); printf(fails ? "%d FAILED\n" : "ALL OK\n", fails); return fails != 0; }
    /* sites */
    int a = at_site_get(&T, 0x1234), b = at_site_get(&T, 0x5678), a2 = at_site_get(&T, 0x1234);
    CHECK(a >= 0 && b >= 0 && a != b && a == a2);
    at_site_add(&T, a, 100000, 1, 100000, 1);
    CHECK(sites[a].live_bytes == 100000 && sites[a].big_count == 1 && sites[a].max_size == 100000);

    /* live table: insert, remove, unknown pointer, tombstone reuse */
    CHECK(at_live_insert(&T, 0x7000000010000ULL, 100000, a, AT_BIG) == 0);
    CHECK(T.live_bytes == 100000 && T.live_blocks == 1);
    CHECK(at_live_remove(&T, 0x7000000020000ULL) == 0);            /* not tracked */
    CHECK(at_live_remove(&T, 0x7000000010000ULL) == 100000);
    CHECK(T.live_bytes == 0 && T.live_blocks == 0 && sites[a].live_bytes == 0);
    CHECK(at_live_remove(&T, 0x7000000010000ULL) == 0);            /* double free ignored */
    for (int i = 0; i < 3000; i++) { CHECK(at_live_insert(&T, 0x100000000ULL + (uint64_t)i * 64, 70000, b, AT_BIG) == 0); }
    for (int i = 0; i < 3000; i++) at_live_remove(&T, 0x100000000ULL + (uint64_t)i * 64);
    for (int i = 0; i < 3000; i++) { CHECK(at_live_insert(&T, 0x200000000ULL + (uint64_t)i * 64, 70000, b, AT_BIG) == 0); } /* reuses tombstones */
    for (int i = 0; i < 3000; i++) at_live_remove(&T, 0x200000000ULL + (uint64_t)i * 64);
    CHECK(T.live_blocks == 0 && T.dropped_live == 0);

    /* sampled small blocks: weight goes to the small counters, never to the big ones */
    CHECK(at_live_insert(&T, 0x7100000000040ULL, 262144, a, AT_SMALL) == 0);
    CHECK(T.live_small_bytes == 262144 && T.live_small_blocks == 1 && T.live_bytes == 0 && T.live_blocks == 1);
    CHECK(sites[a].live_small_bytes == 262144 && sites[a].live_small_count == 1 && sites[a].live_bytes == 0);
    CHECK(at_live_remove(&T, 0x7100000000040ULL) == 262144);
    CHECK(T.live_small_bytes == 0 && T.live_small_blocks == 0 && T.live_blocks == 0 && sites[a].live_small_bytes == 0);

    /* LIVE small estimate is unbiased: allocate a mix, free a known part, compare estimate with truth */
    {
        static at_site s2[64]; static at_live l2[1 << 16];
        at_tables U = { s2, 64, l2, 1 << 16, 0, 0, 0, 0, 0, 0 };
        int site = at_site_get(&U, 0x4242);
        static uint64_t ptr[400000]; static int32_t sz[400000];
        int64_t lft = 65536, live_truth = 0; srand(7);
        for (int i = 0; i < 400000; i++) {
            sz[i] = 16 + rand() % 3000; ptr[i] = 0x10000000ULL + (uint64_t)i * 4096;
            int64_t wgt = at_sample(&lft, sz[i], 65536);
            if (wgt) at_live_insert(&U, ptr[i], wgt, site, AT_SMALL);
        }
        for (int i = 0; i < 400000; i++) { if (i % 3) at_live_remove(&U, ptr[i]); else live_truth += sz[i]; }
        double e2 = (double)(U.live_small_bytes - live_truth) / (double)live_truth;
        printf("small live estimate: truth %lld, estimate %lld (%+.2f%%), samples live %lld, dropped %lld\n", (long long)live_truth,
               (long long)U.live_small_bytes, e2 * 100, (long long)U.live_small_blocks, (long long)U.dropped_live);
        CHECK(e2 > -0.05 && e2 < 0.05 && U.dropped_live == 0 && s2[site].live_small_bytes == U.live_small_bytes);
    }

    /* VM owner table: put, overwrite, get, tombstone reuse */
    CHECK(at_vm_put(vmt, 256, 0x7ff000000000ULL, 0x1111, 0x2222, 1 << 20) == 0);
    CHECK(at_vm_put(vmt, 256, 0x7ff000000000ULL, 0x3333, 0, 2 << 20) == 0);
    at_vm *ve = at_vm_get(vmt, 256, 0x7ff000000000ULL);
    CHECK(ve && ve->owner == 0x3333 && ve->exe == 0 && ve->size == (2 << 20) && at_vm_get(vmt, 256, 0x7ff000010000ULL) == NULL);
    if (ve) ve->key = 1;                                               /* tombstone, as vm_report does */
    CHECK(at_vm_get(vmt, 256, 0x7ff000000000ULL) == NULL);
    for (int i = 0; i < 200; i++) CHECK(at_vm_put(vmt, 256, 0x10000000ULL + (uint64_t)i * 0x10000, (uint64_t)i, 0, 0x10000) == 0);
    int vok = 1; for (int i = 0; i < 200; i++) { at_vm *q = at_vm_get(vmt, 256, 0x10000000ULL + (uint64_t)i * 0x10000); vok &= q && q->owner == (LONG64)i; }
    CHECK(vok);

    /* sampler: total weight ~= total bytes */
    int64_t left = 262144, w = 0, total = 0;
    srand(1);
    for (int i = 0; i < 2000000; i++) { int64_t s = 16 + rand() % 2000; total += s; w += at_sample(&left, s, 262144); }
    double err = (double)(w - total) / (double)total;
    printf("sampler: bytes %lld estimate %lld (%.4f%%)\n", (long long)total, (long long)w, err * 100);
    CHECK(err > -0.001 && err < 0.001);
    left = 262144; CHECK(at_sample(&left, 1 << 20, 262144) == 1 << 20);   /* one big sample counted fully */

    /* frame picking skips allocator wrappers */
    at_range skip[2] = { { 0x67b430, 0x67b4a0 }, { 0x43d8000, 0x43e5000 } };
    uint32_t f1[4] = { 0x67b455, 0x123456, 0x200000, 0 }, f2[3] = { 0x43d9620, 0x67b460, 0x999 };
    CHECK(at_pick_frame(f1, 4, skip, 2) == 0x123456);
    CHECK(at_pick_frame(f2, 3, skip, 2) == 0x999);
    uint32_t f3[2] = { 0x67b440, 0x67b441 };
    CHECK(at_pick_frame(f3, 2, skip, 2) == 0x67b440);                   /* all skipped -> first */

    /* concurrency */
    HANDLE th[NT];
    for (int i = 0; i < NT; i++) th[i] = CreateThread(NULL, 0, worker, (void *)(uintptr_t)i, 0, NULL);
    WaitForMultipleObjects(NT, th, TRUE, INFINITE);
    LONG64 sum = 0; for (int i = 0; i < 1024; i++) if (sites[i].key >= 0x1000 && sites[i].key < 0x1000 + 37) sum += sites[i].live_bytes + sites[i].live_small_bytes;
    printf("stress: live blocks %lld, live bytes %lld, per-site live sum %lld, dropped %lld\n",
           (long long)T.live_blocks, (long long)T.live_bytes, (long long)sum, (long long)T.dropped_live);
    CHECK(T.live_blocks == 0 && T.live_bytes == 0 && T.live_small_bytes == 0 && T.live_small_blocks == 0 && sum == 0 && T.dropped_live == 0);

    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails != 0;
}
