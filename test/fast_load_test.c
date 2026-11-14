/* Offline test of plugins/fast_load (no game needed).
 *   M=third_party/minhook; python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -I$M/include \
 *     -o $V3X_TMP/flt.exe test/fast_load_test.c $M/src/buffer.c $M/src/hook.c $M/src/trampoline.c $M/src/hde/hde64.c
 *   $V3X_TMP/flt.exe            -> prints ALL OK  (run from v3x/: parses plugins/fast_load/fast_load.ini.example)
 *   $V3X_TMP/flt.exe <victoria3.exe>   also maps the real exe (no code runs) and resolves every
 *                                             ANCHORS[] entry + the mesh thunk with the plugin's own C code
 * Covers fl_core (config, pattern match, timeline, thread stats, string readers) and the
 * mesh_item entry-only stub: a byte-identical copy of the engine thunk (mov rax,[rcx]; jmp [rax+0x30])
 * is hooked with MinHook and must forward 6 integer + float args and stack args unchanged. */
#include "../plugins/fast_load/fl_core.c"
#include "../plugins/fast_load/fast_load.c"
#include "MinHook.h"
#include <stdio.h>
#include <stdarg.h>
#include "test_tmp.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int all_readable(const void *p, size_t n) { (void)p; (void)n; return 1; }

/* vtable slot 6 (+0x30) target with stack args and a float */
typedef struct { void **vt; } obj_t;
static long long target6(obj_t *o, long long b, long long c, double d, long long e, long long f)
{
    (void)o;
    return b * 1 + c * 10 + (long long)(d * 100) + e * 1000 + f * 10000;
}
static void *VT[8] = { 0, 0, 0, 0, 0, 0, (void *)target6, 0 };

/* ---- fake V3X_API over an image mapped with LoadLibraryEx(DONT_RESOLVE_DLL_REFERENCES) */
static uint8_t *X_base, *X_text; static size_t X_size, X_text_n;
static struct { uint8_t *p; size_t n; } X_data[8]; static int X_nd;
static void x_log(const char *tag, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); printf("[%s] ", tag); vprintf(fmt, ap); putchar('\n'); va_end(ap); }
static uintptr_t x_base(void) { return (uintptr_t)X_base; }
static size_t x_size(void) { return X_size; }
static void *x_find_string(const char *s)
{
    size_t n = strlen(s) + 1;
    for (int r = 0; r < X_nd; r++)
        for (size_t i = 0; i + n <= X_data[r].n; i++)
            if (X_data[r].p[i] == (uint8_t)s[0] && !memcmp(X_data[r].p + i, s, n) && (i == 0 || X_data[r].p[i - 1] == 0))
                return X_data[r].p + i;
    return NULL;
}
static void *x_find_xref(const void *t, int index)
{
    for (size_t i = 0; i + 7 <= X_text_n; i++) {
        uint8_t *q = X_text + i;
        if ((q[0] != 0x48 && q[0] != 0x4C) || (q[1] != 0x8D && q[1] != 0x8B) || (q[2] & 0xC7) != 5) continue;
        int32_t d; memcpy(&d, q + 3, 4);
        if (q + 7 + d == (const uint8_t *)t && index-- == 0) return q;
    }
    return NULL;
}
static void *x_function_start(const void *addr)  /* .pdata binary search (+ chained unwind) */
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(X_base + ((IMAGE_DOS_HEADER *)X_base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    RUNTIME_FUNCTION *rf = (RUNTIME_FUNCTION *)(X_base + dd.VirtualAddress);
    size_t lo = 0, hi = dd.Size / sizeof *rf; DWORD r = (DWORD)((const uint8_t *)addr - X_base);
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (r < rf[m].BeginAddress) hi = m; else if (r >= rf[m].EndAddress) lo = m + 1;
        else {
            RUNTIME_FUNCTION *f = &rf[m];
            for (int g = 0; g < 32; g++) {
                uint8_t *ui = X_base + f->UnwindData;
                if (!((ui[0] >> 3) & UNW_FLAG_CHAININFO)) break;
                f = (RUNTIME_FUNCTION *)(ui + 4 + ((ui[2] + 1) & ~1u) * 2);
            }
            return X_base + f->BeginAddress;
        }
    }
    return NULL;
}
static int x_hook(void *t, void *d, void **o) { (void)t; (void)d; *o = (void *)1; return 0; }  /* resolve only */
static void *x_pattern_scan(const char *pat)      /* first match in .text, ?? = wildcard (as core.c) */
{
    for (size_t i = 0; i + 64 <= X_text_n; i++)
        if (X_text[i] == 0x48 && fl_match(X_text + i, X_text_n - i, 0, pat)) return X_text + i;
    return NULL;
}

static const char *t_ver(void) { return "test"; }
static char g_tdir[MAX_PATH];   /* <scratch>/load/, set in main */
static const char *t_dir(void) { return g_tdir; }
static int t_emits, t_begins; static char t_from[32], t_to[32], t_bfrom[32]; static double t_secs;
static int t_emit(const char *name, void *data)
{
    if (!strcmp(name, "load_transition_begin")) { V3X_LoadTransition *b = data; t_begins++; snprintf(t_bfrom, sizeof t_bfrom, "%s", b && b->from ? b->from : "(null)"); return 1; }
    if (strcmp(name, "load_transition")) return 0;
    V3X_LoadTransition *i = data; t_emits++;
    snprintf(t_from, sizeof t_from, "%s", i->from ? i->from : "(null)");
    snprintf(t_to, sizeof t_to, "%s", i->to ? i->to : "(null)");
    t_secs = i->seconds; return 1;
}
static uintptr_t t_orig(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) { (void)a; return b + c + d + 33; }

static uint8_t m_settings[0x1000]; static int32_t m_seen;
static uintptr_t m_orig(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{ memcpy(&m_seen, m_settings + 0x648, 4); return a + b + c + d - 3; }

/* fake 03dfa270: returns ws_ret; publishes &ws_task once ws_avail is set */
static int ws_task; static volatile LONG ws_avail; static int ws_ret = 1; static int ws_hold_ms; static LARGE_INTEGER ws_pub;
static uintptr_t ws_fake(uintptr_t w, uintptr_t out, uintptr_t c, uintptr_t d)
{ (void)w; (void)c; (void)d; *(void **)out = ws_avail ? (void *)&ws_task : NULL; return (uintptr_t)ws_ret; }
static DWORD WINAPI ws_producer(void *u)
{ (void)u; Sleep(ws_hold_ms); QueryPerformanceCounter(&ws_pub); InterlockedExchange(&ws_avail, 1); return 0; }

static int real_exe(const char *path)
{
    X_base = (uint8_t *)LoadLibraryExA(path, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!X_base) { printf("cannot map %s (%lu)\n", path, GetLastError()); return 1; }
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(X_base + ((IMAGE_DOS_HEADER *)X_base)->e_lfanew);
    X_size = nt->OptionalHeader.SizeOfImage;
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (!memcmp(sec->Name, ".text", 5)) { X_text = X_base + sec->VirtualAddress; X_text_n = sec->Misc.VirtualSize; }
        else if ((!memcmp(sec->Name, ".rdata", 6) || !memcmp(sec->Name, ".data", 5)) && X_nd < 8)
            { X_data[X_nd].p = X_base + sec->VirtualAddress; X_data[X_nd++].n = sec->Misc.VirtualSize; }
    }
    static V3X_API fake;
    fake.log = x_log; fake.image_base = x_base; fake.image_size = x_size;
    fake.find_string = x_find_string; fake.find_xref = x_find_xref; fake.function_start = x_function_start;
    fake.hook = x_hook; fake.pattern_scan = x_pattern_scan;
    api = &fake;
    int bad = 0;
    for (int i = 0; i < A_COUNT; i++) {
        ANCHORS[i].fn = NULL;
        uint8_t *f = resolve(&ANCHORS[i]);
        printf("%-10s %s %08x\n", ANCHORS[i].name, f ? "OK  " : "FAIL", f ? rva(f) : 0);
        bad += !f;
    }
    if (ANCHORS[A_MESH].fn) { g_item_orig = NULL; install_item_thunk(); bad += g_item_orig == NULL; }
    o_pump_step = NULL; install_pump(); bad += o_pump_step == NULL;
    g_cfg.worker_spin = 1; o_worker_get = NULL; install_worker_spin(); bad += o_worker_get == NULL; g_cfg.worker_spin = 0;
    printf("real exe: %s\n", bad ? "ANCHORS FAILED" : "ALL ANCHORS OK");
    return bad;
}

int main(int argc, char **argv)
{
    test_tmp_dir(g_tdir, sizeof g_tdir, "load");   /* scratch: $V3X_TMP or %TEMP% (test/test_tmp.h) */
    /* config */
    fl_config c; char err[160];
    fl_config_defaults(&c);
    CHECK(c.timeline == 1 && c.checksum_cache == 0 && c.cache_verify == 1 && c.db_nodes == 1 && c.mesh_threads == 0);
    CHECK(fl_config_parse(&c, "# comment\r\n timeline = 0 \r\n;x\n\nchecksum_cache=1\ncache_verify = 0\ndb_nodes=0", err, sizeof err) == 0);
    CHECK(c.timeline == 0 && c.checksum_cache == 1 && c.cache_verify == 0 && c.db_nodes == 0);
    fl_config_defaults(&c);
    CHECK(fl_config_parse(&c, "timeline = 2\nfoo = 1\nnoeq\ntimeline=x\n", err, sizeof err) == 4);
    CHECK(!strncmp(err, "line 1:", 7));
    CHECK(c.timeline == 1);
    CHECK(fl_config_parse(&c, "mesh_threads = 8\n", err, sizeof err) == 0 && c.mesh_threads == 8);
    CHECK(fl_config_parse(&c, "mesh_threads=1\nmesh_threads=16\n", err, sizeof err) == 2 && c.mesh_threads == 8);
    CHECK(fl_config_parse(&c, "mesh_threads=0\n", err, sizeof err) == 0 && c.mesh_threads == 0);
    CHECK(fl_config_parse(&c, "mesh_threads = 6   # six\r\ndb_nodes=0;off\t \n", err, sizeof err) == 0 && c.mesh_threads == 6 && c.db_nodes == 0);
    CHECK(fl_config_parse(&c, "mesh_threads = # nothing\n", err, sizeof err) == 1);
    {   /* the shipped example must parse cleanly and equal the defaults */
        FILE *ef = fopen("plugins/fast_load/fast_load.ini.example", "rb");
        char eb[4096] = {0}; CHECK(ef != NULL);
        if (ef) { fread(eb, 1, sizeof eb - 1, ef); fclose(ef); }
        fl_config ex, def; fl_config_defaults(&ex); fl_config_defaults(&def);
        CHECK(fl_config_parse(&ex, eb, err, sizeof err) == 0);
        if (err[0]) printf("example: %s\n", err);
        CHECK(!memcmp(&ex, &def, sizeof ex));
    }

    /* pattern match */
    const uint8_t code[] = { 0x48, 0x8B, 0x01, 0x48, 0xFF, 0x60, 0x30, 0xCC };
    CHECK(fl_match(code, sizeof code, 0, "48 8B 01 48 FF 60 30"));
    CHECK(fl_match(code, sizeof code, 3, "48 ?? 60"));
    CHECK(!fl_match(code, sizeof code, 0, "48 8B 02"));
    CHECK(!fl_match(code, sizeof code, 6, "30 CC 00"));         /* would read past avail */
    CHECK(!fl_match(code, sizeof code, 0, "4"));                /* malformed */

    /* timeline */
    fl_event evbuf[3]; fl_timeline tl = { evbuf, 0, 3, 0 };
    fl_tl_add(&tl, 20.0, 7, "b %d", 2);
    fl_tl_add(&tl, 10.0, 7, "a");
    fl_tl_add(&tl, 20.0, 8, "c");
    fl_tl_add(&tl, 30.0, 8, "dropped");
    char out[512];
    size_t n = fl_tl_format(&tl, "# hdr", out, sizeof out);
    CHECK(n == strlen(out));
    CHECK(tl.dropped == 1);
    const char *pa = strstr(out, "  a\n"), *pb = strstr(out, "  b 2\n"), *pc = strstr(out, "  c\n");
    CHECK(pa && pb && pc && pa < pb && pb < pc);                 /* sorted, stable */
    CHECK(strstr(out, "(1 events dropped") != NULL);
    char tiny[8]; CHECK(fl_tl_format(&tl, "# hdr", tiny, sizeof tiny) == 7 && strlen(tiny) == 7);

    /* thread stats */
    fl_thrset s; memset(&s, 0, sizeof s);
    fl_thr_hit(&s, 1, 0); fl_thr_hit(&s, 2, 5); fl_thr_hit(&s, 1, 100); fl_thr_hit(&s, 2, 105);
    fl_thr_summary(&s, 100, out, sizeof out);
    CHECK(s.n == 2 && s.calls == 4 && s.t[0].calls == 2);
    CHECK(strstr(out, "threads=2 items=4 span_sum=200ms wall=100ms est_parallelism=2.00") != NULL);

    /* strings */
    struct { union { char buf[16]; char *ptr; } u; uint64_t size, cap; } ms;
    memset(&ms, 0, sizeof ms); strcpy(ms.u.buf, "Empty->Game"); ms.size = 11; ms.cap = 15;
    CHECK(fl_msvc_str(&ms, out, sizeof out, all_readable) == 11 && !strcmp(out, "Empty->Game"));
    char heap[] = "a_long_transition_name_over_16";
    ms.u.ptr = heap; ms.size = strlen(heap); ms.cap = 31;
    CHECK(fl_msvc_str(&ms, out, sizeof out, all_readable) == (int)strlen(heap) && !strcmp(out, heap));
    CHECK(fl_msvc_str(&ms, out, 5, all_readable) == 4 && !strcmp(out, "a_lo"));
    ms.size = 40;                                                   /* size > cap: corrupt */
    CHECK(fl_msvc_str(&ms, out, sizeof out, all_readable) == -1);
    struct { const char *p; int64_t n; } view = { "technologies_xyz", 12 };
    CHECK(fl_view_str(&view, out, sizeof out, all_readable) == 12 && !strcmp(out, "technologies"));
    view.n = -1; CHECK(fl_view_str(&view, out, sizeof out, all_readable) == -1);
    CHECK(readable(&view, sizeof view) && !readable(NULL, 4) && !readable((void *)16, 4));

    /* mesh_item stub through a MinHook'd copy of the engine thunk */
    uint8_t *thunk = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(thunk, code, sizeof code); memset(thunk + 8, 0xCC, 24);
    CHECK(MH_Initialize() == MH_OK);
    CHECK(MH_CreateHook(thunk, (void *)mesh_item_stub, &g_item_orig) == MH_OK);
    CHECK(MH_EnableHook(thunk) == MH_OK);
    QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_qpc0);
    memset(&g_thr, 0, sizeof g_thr);
    obj_t o = { VT };
    long long (*volatile call)(obj_t *, long long, long long, double, long long, long long) = (void *)thunk;
    g_mesh_active = 0;
    CHECK(call(&o, 1, 2, 0.03, 4, 5) == 1 + 20 + 3 + 4000 + 50000);
    CHECK(g_thr.calls == 0);                                         /* inactive: not counted */
    g_mesh_active = 1;
    for (int i = 0; i < 3; i++) CHECK(call(&o, 9, 8, 0.07, 6, 5) == 9 + 80 + 7 + 6000 + 50000);
    CHECK(g_thr.calls == 3 && g_thr.n == 1 && g_thr.t[0].tid == GetCurrentThreadId());

    /* transition detour end-to-end: fake task {+0x150 -> state(+0x128 float secs), +0x168 std::string} */
    {
        static V3X_API f3;
        f3.api_version = 3; f3.struct_size = sizeof f3;
        f3.log = x_log; f3.game_version = t_ver; f3.v3x_dir = t_dir; f3.emit_event = t_emit;
        api = &f3;
        g_tl.cap = 64; g_tl.n = 0; g_tl.ev = calloc(64, sizeof(fl_event));
        snprintf(g_out_path, sizeof g_out_path, "%sload_timeline_test.txt", t_dir());
        static uint8_t task[0x200], state[0x200];
        float secs = 171.5f; memcpy(state + 0x128, &secs, 4);
        uint64_t sp = (uint64_t)(uintptr_t)state; memcpy(task + 0x150, &sp, 8);
        memset(&ms, 0, sizeof ms); strcpy(ms.u.buf, "Empty->Game"); ms.size = 11; ms.cap = 15;
        memcpy(task + 0x168, &ms, sizeof ms);
        void *taskp = task;
        o_transition = t_orig;
        CHECK(h_transition((uintptr_t)&taskp, 2, 3, 4) == 42);
        CHECK(t_begins == 1 && !strcmp(t_bfrom, "Empty"));
        CHECK(t_emits == 1 && !strcmp(t_from, "Empty") && !strcmp(t_to, "Game") && t_secs > 171.4 && t_secs < 171.6);
        FILE *tf = fopen(g_out_path, "rb"); char tb[2048] = {0};
        if (tf) { fread(tb, 1, sizeof tb - 1, tf); fclose(tf); }
        CHECK(strstr(tb, "TRANSITION Empty->Game took 171.50 s (engine timer") != NULL);
        f3.api_version = 2;                                           /* old core: no emit */
        CHECK(h_transition((uintptr_t)&taskp, 2, 3, 4) == 42 && t_emits == 1);
    }

    /* mesh hook: mesh_threads override is visible inside the loader and restored afterwards */
    {
        static uint8_t fakefn[0x80]; static uint64_t gptr;
        memset(m_settings, 0, sizeof m_settings);
        int32_t engine = 4; memcpy(m_settings + 0x648, &engine, 4);
        gptr = (uint64_t)(uintptr_t)m_settings;
        const uint8_t ins[] = { 0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x44, 0x8B, 0xA0, 0x48, 0x06, 0x00, 0x00 };
        memcpy(fakefn + 0x41, ins, sizeof ins);
        int32_t disp = (int32_t)((intptr_t)&gptr - (intptr_t)(fakefn + 0x41 + 7));
        memcpy(fakefn + 0x41 + 3, &disp, 4);
        ANCHORS[A_MESH].fn = fakefn; o_mesh = m_orig; g_item_orig = NULL;
        g_cfg.mesh_threads = 12;
        CHECK(h_mesh(1, 2, 3, 4) == 7 && m_seen == 12 && g_mesh_setting == 4);
        int32_t after; memcpy(&after, m_settings + 0x648, 4); CHECK(after == 4);
        g_cfg.mesh_threads = 0;
        CHECK(h_mesh(1, 2, 3, 4) == 7 && m_seen == 4);
    }

    /* new keys */
    fl_config_defaults(&c);
    CHECK(c.counters == 1 && c.pump_yield == 0);
    CHECK(fl_config_parse(&c, "counters = 0\npump_yield = 2 # sleep\n", err, sizeof err) == 0 && c.counters == 0 && c.pump_yield == 2);
    CHECK(fl_config_parse(&c, "pump_yield = 3\n", err, sizeof err) == 1 && c.pump_yield == 2);

    /* counters: the SYSTEM_PERFORMANCE_INFORMATION layout check must pass on this machine */
    {
        ntqsi_fn q = (ntqsi_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation");
        int ok = 2; ctr_t c1, c2;
        ctr_read(&c1, q, &ok);
        CHECK(ok == 1 && c1.sys_ok);
        volatile double x = 0; for (int i = 0; i < 20000000; i++) x += i;   /* burn some CPU */
        ctr_read(&c2, q, &ok);
        CHECK(c2.pcpu > c1.pcpu && c2.stot >= c1.stot && c2.sio >= c1.sio && c2.pread >= c1.pread);
    }

    /* pump step hook: counts steps and time, forwards args and return value */
    {
        o_pump_step = t_orig; g_pump_iters = 0; g_pump_ticks = 0;
        g_cfg.pump_yield = 0; CHECK(h_pump_step(0, 1, 2, 3) == 39);
        g_cfg.pump_yield = 1; CHECK(h_pump_step(0, 1, 2, 3) == 39);
        g_cfg.pump_yield = 2; CHECK(h_pump_step(0, 1, 2, 3) == 39);
        CHECK(g_pump_iters == 3 && g_pump_ticks > 0);
        g_cfg.pump_yield = 0;
    }

    /* worker_spin: semantics + task pickup latency per mode (a producer publishes a task after the
     * consumer has been polling empty for 30 ms; latency = publish -> consumer got it) */
    {
        CHECK(fl_config_parse(&c, "worker_spin = 3\n", err, sizeof err) == 0 && c.worker_spin == 3);
        CHECK(fl_config_parse(&c, "worker_spin = 4\n", err, sizeof err) == 1);
        g_ws_tls = TlsAlloc(); CHECK(g_ws_tls != TLS_OUT_OF_INDEXES);
        o_worker_get = ws_fake;
        void *task = NULL;
        g_cfg.worker_spin = 1; ws_avail = 0;
        CHECK(h_worker_get(0, (uintptr_t)&task, 0, 0) == 1 && task == NULL && (uintptr_t)TlsGetValue(g_ws_tls) == 1);
        ws_avail = 1;
        CHECK(h_worker_get(0, (uintptr_t)&task, 0, 0) == 1 && task == &ws_task && TlsGetValue(g_ws_tls) == NULL);
        ws_ret = 0; task = NULL;                                       /* returns 0 = no backoff, no count */
        LONG64 e0 = g_ws_empty;
        CHECK(h_worker_get(0, (uintptr_t)&task, 0, 0) == 0 && g_ws_empty == e0);
        ws_ret = 1;
        for (int mode = 0; mode <= 3; mode++) {
            double worst = 0;
            for (int rep = 0; rep < 5; rep++) {
                g_cfg.worker_spin = mode; ws_avail = 0; TlsSetValue(g_ws_tls, NULL);
                ws_hold_ms = mode == 3 ? 400 : 30;                     /* mode 3 sleeps only after 20000 empty polls */
                HANDLE th = CreateThread(NULL, 0, ws_producer, NULL, 0, NULL);
                LARGE_INTEGER got; void *tk = NULL;
                do { tk = NULL; h_worker_get(0, (uintptr_t)&tk, 0, 0); } while (!tk);
                QueryPerformanceCounter(&got);
                WaitForSingleObject(th, INFINITE); CloseHandle(th);
                double us = (double)(got.QuadPart - ws_pub.QuadPart) * 1e6 / (double)g_freq.QuadPart;
                if (us > worst) worst = us;
            }
            printf("worker_spin=%d: worst task pickup latency %.1f us (5 runs)\n", mode, worst);
            if (mode <= 2) CHECK(worst < 2000);        /* pause/yield only: well under 2 ms even on a busy box */
            else CHECK(worst < 40000);                 /* Sleep(1): bounded by the timer resolution */
        }
        g_cfg.worker_spin = 0;
    }

    printf("%s\n", fails ? "FAILED" : "ALL OK");
    MH_Uninitialize();
    if (argc > 1) fails += real_exe(argv[1]);
    return fails;
}
