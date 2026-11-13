/* End-to-end test for plugins/devtools without the game.
 * Links build/version.dll like victoria3.exe, runs two named busy threads in distinct noinline
 * functions (each referencing its own string literal), pumps PeekMessageW so on_frame fires,
 * then parses v3x/devtools.log and checks threads, hot functions + hints, memory and GPU sections.
 * Then drives the API v2 console command "profile off|report|reset|on" through exec_command, and
 * stress-tests 200 short-lived threads with a report every 0.2 s (must not crash). With API v3 it
 * also emits fake "load_transition" events and checks the per-phase CSVs and the caller-edge CSV.
 * Build with -Iinclude. Run with V3X_PROFILE=1 and v3x/devtools.ini containing interval_sec=2.
 * Exit code 0 = ALL OK. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v3x.h"

static const char ALPHA_MSG[] = "devtools_test alpha: float spin";
static const char BETA_MSG[] = "devtools_test beta: integer hash";
static volatile LONG g_stop;
static volatile double g_sink_d;
static volatile unsigned long long g_sink_u;
static const char *volatile g_sink_s;

__attribute__((noinline)) void busy_alpha(void)
{
    double x = 1.0;
    while (!g_stop) {
        g_sink_s = ALPHA_MSG;                   /* rip-relative lea to the literal */
        for (int i = 0; i < 20000; i++) x = x * 1.0000001 + 0.5 / (x + 1.0);
        g_sink_d = x;
    }
}

__attribute__((noinline)) void busy_beta(void)
{
    unsigned long long h = 1469598103934665603ull;
    while (!g_stop) {
        g_sink_s = BETA_MSG;
        for (int i = 0; i < 20000; i++) { h ^= (unsigned)i; h *= 1099511628211ull; h = (h << 7) | (h >> 57); }
        g_sink_u = h;
    }
}

typedef HRESULT(WINAPI *SetThreadDescription_t)(HANDLE, PCWSTR);
static SetThreadDescription_t pSTD;

static DWORD WINAPI thread_alpha(void *p) { (void)p; if (pSTD) pSTD(GetCurrentThread(), L"busy_alpha"); busy_alpha(); return 0; }
static DWORD WINAPI thread_beta(void *p) { (void)p; if (pSTD) pSTD(GetCurrentThread(), L"busy_beta"); busy_beta(); return 0; }

/* stress: short-lived threads that spin 2-20 ms (busy enough to become sampler targets) and exit,
 * freeing TEB + stack while devtools may be reading them */
/* unwinder hardening: a thread spinning in VirtualAlloc'd code (no module, like JIT) and a thread
 * loading/unloading DLLs, so samples land in non-module code and in modules that go away */
static volatile LONG64 g_jit_stop;
static DWORD WINAPI jit_thread(void *code)
{
    ((void (*)(volatile LONG64 *))code)(&g_jit_stop);
    return 0;
}
static volatile LONG g_dll_cycles;
static DWORD WINAPI dll_churn_thread(void *p)
{
    (void)p;
    static const char *dlls[] = {"dbghelp.dll", "xmllite.dll", "wtsapi32.dll", "winhttp.dll"};
    for (unsigned i = 0; !g_jit_stop; i++) {
        HMODULE h = LoadLibraryA(dlls[i % 4]);
        if (h) { FreeLibrary(h); InterlockedIncrement(&g_dll_cycles); }
    }
    return 0;
}

static volatile LONG g_churn_done;
static DWORD WINAPI churn_thread(void *p)
{
    DWORD ms = 2 + (DWORD)(ULONG_PTR)p % 19, t0 = GetTickCount();
    volatile unsigned long long h = (ULONG_PTR)p;
    while (GetTickCount() - t0 < ms) h = h * 6364136223846793005ull + 1;
    InterlockedIncrement(&g_churn_done);
    return 0;
}

/* Name the main thread the old way (exception 0x406D1388); swallow it ourselves (we're last in the VEH chain). */
static LONG CALLBACK swallow_name(PEXCEPTION_POINTERS ep)
{
    return ep->ExceptionRecord->ExceptionCode == 0x406D1388 ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
static void name_thread_legacy(const char *name)
{
    ULONG_PTR info[4] = {0x1000, (ULONG_PTR)name, (ULONG_PTR)-1, 0};
    RaiseException(0x406D1388, 0, 4, info);
}

static int fails;
static void check(int ok, const char *what) { printf("%-60s %s\n", what, ok ? "OK" : "FAIL"); fails += !ok; }

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    n = (long)fread(b, 1, n, f); b[n] = 0;
    fclose(f);
    return b;
}

static double thread_cpu_in(const char *rep, const char *name)
{
    const char *sec = strstr(rep, "[threads]");
    if (!sec) return -1;
    const char *end = strstr(sec, "[profiler]");
    for (const char *l = strchr(sec, '\n'); l && (!end || l < end); l = strchr(l + 1, '\n')) {
        const char *nl = strchr(l + 1, '\n');
        const char *hit = strstr(l + 1, name);
        if (hit && (!nl || hit < nl)) {
            unsigned long tid; double cpu;
            if (sscanf(l + 1, "%lu %lf", &tid, &cpu) == 2) return cpu;
        }
    }
    return -1;
}

/* rank (1-based) of `addr` in the self-time hot list, and whether its line contains `hint` */
static int hot_rank(const char *rep, const char *addr, const char *hint, int *hint_ok)
{
    *hint_ok = 0;
    const char *sec = strstr(rep, "hottest functions (self time)");
    if (!sec) return 0;
    const char *end = strstr(sec, "top inclusive");
    const char *l = strchr(sec, '\n');
    l = l ? strchr(l + 1, '\n') : NULL;   /* skip column header */
    for (; l && (!end || l < end); l = strchr(l + 1, '\n')) {
        int rank; unsigned s; double a, b; char fn[128];
        if (sscanf(l + 1, "%d %u %lf %lf %127s", &rank, &s, &a, &b, fn) != 5) continue;
        if (!strcmp(fn, addr)) {
            const char *nl = strchr(l + 1, '\n');
            const char *h = strstr(l + 1, hint);
            *hint_ok = h && (!nl || h < nl);
            return rank;
        }
    }
    return 0;
}

int main(void)
{
    pSTD = (SetThreadDescription_t)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadDescription");
    AddVectoredExceptionHandler(0, swallow_name);
    name_thread_legacy("test_main");

    /* forwarded export still works */
    DWORD h, sz = GetFileVersionInfoSizeA("C:/Windows/System32/kernel32.dll", &h);
    check(sz != 0, "version.dll forwarding");

    HANDLE ta = CreateThread(NULL, 0, thread_alpha, NULL, 0, NULL);
    HANDLE tb = CreateThread(NULL, 0, thread_beta, NULL, 0, NULL);
    DWORD t0 = GetTickCount();
    MSG msg;
    while (GetTickCount() - t0 < 6600) {     /* reports at ~2, 4, 6 s */
        PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE);
        Sleep(8);
    }
    /* API v3: fake fast_load transition while the busy threads still run: begin -> snapshot #1
     * ("before", ~6.6 s), end 400 ms later -> snapshot #2 (only the transition); after the threads
     * stop, an end with NULL data -> snapshot #3 ("none_to_none") */
    v3x_api_fn get_api = (v3x_api_fn)(void *)GetProcAddress(GetModuleHandleA("version.dll"), "v3x_api");
    const V3X_API *api = get_api ? get_api() : NULL;
    int v3 = api && api->api_version >= 3 && api->struct_size >= sizeof(V3X_API) && api->emit_event;
    int subs0 = -9, subs1 = -9, subs2 = -9;
    if (v3) {
        V3X_LoadTransition lt = {"Loading Screen", "main/menu", 6.6};
        subs0 = api->emit_event("load_transition_begin", &lt);
        Sleep(400);
        lt.seconds = 0.4;
        subs1 = api->emit_event("load_transition", &lt);
        Sleep(300);
    }
    g_stop = 1;
    WaitForSingleObject(ta, INFINITE); WaitForSingleObject(tb, INFINITE);
    if (v3) { subs2 = api->emit_event("load_transition", NULL); Sleep(300); }

    char dir[MAX_PATH], path[MAX_PATH + 32], exe[64];
    GetModuleFileNameA(NULL, dir, MAX_PATH);
    char *bs = strrchr(dir, '\\');
    snprintf(exe, sizeof exe, "%s", bs + 1);
    bs[1] = 0;
    snprintf(path, sizeof path, "%sv3x\\devtools.log", dir);
    char *log = read_file(path);
    check(log != NULL, "devtools.log exists");
    if (!log) return 1;
    int nrep = 0; char *last = NULL;
    for (char *p = strstr(log, "===== devtools report"); p; p = strstr(p + 1, "===== devtools report")) { nrep++; last = p; }
    printf("reports found: %d\n", nrep);
    check(nrep >= 2, "at least 2 reports in ~6 s at interval 2 s");
    if (!last) return 1;
    /* the first report may cover the startup period before the sampler picked targets; use the last one */
    double ca = thread_cpu_in(last, "busy_alpha"), cb = thread_cpu_in(last, "busy_beta");
    printf("busy_alpha cpu %.1f%%, busy_beta cpu %.1f%%\n", ca, cb);
    check(ca >= 50 && ca <= 110, "thread busy_alpha named, CPU% plausible");
    check(cb >= 50 && cb <= 110, "thread busy_beta named, CPU% plausible");
    check(strstr(last, "test_main") != NULL, "main thread named via exception 0x406D1388");
    check(strstr(last, "[profiler] ON") != NULL, "profiler ON");

    char aa[96], ab[96];
    uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    snprintf(aa, sizeof aa, "%s+0x%llX", exe, (unsigned long long)((uintptr_t)busy_alpha - base));
    snprintf(ab, sizeof ab, "%s+0x%llX", exe, (unsigned long long)((uintptr_t)busy_beta - base));
    int ha, hb;
    int ra = hot_rank(last, aa, ALPHA_MSG + 14, &ha), rb = hot_rank(last, ab, BETA_MSG + 14, &hb);
    printf("busy_alpha %s rank %d, busy_beta %s rank %d\n", aa, ra, ab, rb);
    check(ra >= 1 && ra <= 2, "busy_alpha in top 2 hot functions");
    check(rb >= 1 && rb <= 2, "busy_beta in top 2 hot functions");
    check(ha, "busy_alpha hint shows its string");
    check(hb, "busy_beta hint shows its string");
    check(strstr(last, "[frames]") && !strstr(last, "no frames this interval"), "frame stats present");
    check(strstr(last, "[memory] private") && strstr(last, "largest committed allocations"), "memory section");
    check(strstr(last, "heaps:") != NULL, "heap summary");
    check(strstr(last, "[gpu]") != NULL, "gpu section");

    snprintf(path, sizeof path, "%sv3x\\profile_latest.csv", dir);
    char *csv = read_file(path);
    char needle[64];
    snprintf(needle, sizeof needle, "0x%llX,", (unsigned long long)((uintptr_t)busy_alpha - base));
    check(csv && strstr(csv, "rva,samples") && strstr(csv, needle), "profile_latest.csv has busy_alpha rva");

    /* phase snapshots + caller edges (thread_alpha -> busy_alpha) */
    check(v3 && subs0 >= 1 && subs1 >= 1 && subs2 >= 1, "core API v3: devtools subscribed to load_transition(_begin)");
    if (v3) {
        snprintf(path, sizeof path, "%sv3x\\profile_Loading_Screen_before_main_menu_1.csv", dir);
        char *ph1 = read_file(path);
        check(ph1 && strstr(ph1, "rva,samples") && strstr(ph1, needle), "begin snapshot 1 (before) csv has busy_alpha rva");
        snprintf(path, sizeof path, "%sv3x\\profile_Loading_Screen_to_main_menu_2.csv", dir);
        char *pht = read_file(path);
        snprintf(path, sizeof path, "%sv3x\\profile_none_to_none_3.csv", dir);
        char *ph2 = read_file(path);
        unsigned long long s1 = 0, st = 0, s2 = 0;
        const char *q;
        if (ph1 && (q = strstr(ph1, needle))) sscanf(q + strlen(needle), "%llu", &s1);
        if (pht && (q = strstr(pht, needle))) sscanf(q + strlen(needle), "%llu", &st);
        if (ph2 && (q = strstr(ph2, needle))) sscanf(q + strlen(needle), "%llu", &s2);
        printf("busy_alpha self samples: before %llu, transition %llu, after stop %llu\n", s1, st, s2);
        check(pht && st > 0 && st < s1 / 4, "end snapshot 2 covers only the transition (~0.4 s)");
        check(ph2 && strstr(ph2, "rva,samples") && s1 > 100 && s2 < s1 / 4, "snapshot 3 csv written, tables were reset");
        char edge[96];
        snprintf(edge, sizeof edge, "0x%llX,0x%llX,", (unsigned long long)((uintptr_t)thread_alpha - base),
                 (unsigned long long)((uintptr_t)busy_alpha - base));
        snprintf(path, sizeof path, "%sv3x\\profile_edges_Loading_Screen_before_main_menu_1.csv", dir);
        char *ed = read_file(path);
        unsigned long long es = 0;
        if (ed && (q = strstr(ed, edge))) sscanf(q + strlen(edge), "%llu", &es);
        printf("edge thread_alpha -> busy_alpha (%s): %llu samples\n", edge, es);
        check(ed && strstr(ed, "caller_rva,callee_rva,samples") && es > 100, "edge csv has thread_alpha -> busy_alpha");
        snprintf(path, sizeof path, "%sv3x\\profile_edges.csv", dir);
        char *el = read_file(path);
        check(el && strstr(el, "caller_rva,callee_rva,samples"), "profile_edges.csv written");
    }

    /* API v2 console command via exec_command (works without a console window) */
    int v2 = api && api->api_version >= 2 && api->struct_size >= sizeof(V3X_API) && api->exec_command;
    check(v2, "core API v2 (v3x_api export)");
    if (v2) {
        size_t old = strlen(log);
        int rc_off = api->exec_command("profile off");     Sleep(400);
        int rc_rep = api->exec_command("profile report");  Sleep(800);
        int rc_rst = api->exec_command("profile reset");   Sleep(400);
        int rc_on = api->exec_command("PROFILE on");       Sleep(400);
        int rc_bad = api->exec_command("profile bogus");
        int rc_unk = api->exec_command("no_such_command_xyz");
        snprintf(path, sizeof path, "%sv3x\\devtools.log", dir);
        char *log2 = read_file(path);
        const char *tail = log2 && strlen(log2) > old ? log2 + old : "";
        const char *p_off = strstr(tail, "profiler OFF"), *p_req = strstr(tail, "(request, interval");
        const char *p_rst = strstr(tail, "profiler samples reset"), *p_on = strstr(tail, "profiler ON");
        printf("exec rc: off %d report %d reset %d on %d bogus %d unknown %d\n",
               rc_off, rc_rep, rc_rst, rc_on, rc_bad, rc_unk);
        check(!rc_off && !rc_rep && !rc_rst && !rc_on && !rc_bad, "'profile' command registered and runs");
        check(rc_unk == -1, "unknown command rejected");
        check(p_off != NULL, "'profile off' stops profiler");
        check(p_off && p_req > p_off && strstr(p_req, "[profiler] OFF"),
              "'profile report' writes a report while stopped");
        check(p_req && p_rst > p_req, "'profile reset' clears samples");
        check(p_rst && p_on > p_rst, "'profile on' (case-insensitive) restarts profiler");

        /* stress: 200 threads start and exit over 3 s while a report is requested every 0.2 s;
         * profiler ON for the first half (sampler targets die), OFF for the second (stale thread
         * list: stack_root reads TEBs/stacks of exited threads). Surviving = no fault in devtools. */
        size_t old2 = log2 ? strlen(log2) : 0;
        int spawned = 0, reqs = 0;
        DWORD s0 = GetTickCount(), next_req = s0 + 200, switched = 0;
        while (GetTickCount() - s0 < 3000) {
            DWORD now = GetTickCount();
            if (!switched && now - s0 >= 1500) { api->exec_command("profile off"); switched = 1; Sleep(30); }
            if ((LONG)(now - next_req) >= 0) { api->exec_command("profile report"); reqs++; next_req += 200; }
            while (spawned < 200 && (int)((now - s0) / 14) >= spawned) {   /* catch up: Sleep(1) may take 15 ms */
                HANDLE h = CreateThread(NULL, 64 * 1024, churn_thread, (void *)(ULONG_PTR)(spawned + 1), 0, NULL);
                if (h) { CloseHandle(h); spawned++; }
            }
            PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE);
            Sleep(1);
        }
        for (int i = 0; i < 200 && g_churn_done < spawned; i++) Sleep(10);
        Sleep(500);   /* let the last requested report finish */
        char *log3 = read_file(path);
        int nreq = 0;
        if (log3 && strlen(log3) > old2)
            for (const char *q = strstr(log3 + old2, "(request, interval"); q; q = strstr(q + 1, "(request, interval")) nreq++;
        printf("stress: %d threads spawned, %ld exited, %d report requests, %d requested reports written\n",
               spawned, g_churn_done, reqs, nreq);
        check(spawned == 200 && g_churn_done == 200, "stress: 200 short-lived threads ran and exited");
        check(nreq >= 10, "stress: >= 10 reports written during thread churn, no crash");

        /* profiler ON for 2 s with a JIT-like spinner and DLL load/unload churn */
        static const unsigned char loop[] = {0x48, 0x8B, 0x01, 0x48, 0x85, 0xC0, 0x74, 0xF8, 0xC3};  /* while(!*p); */
        unsigned char *code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        HANDLE tj = NULL, td = NULL;
        if (code) {
            memcpy(code, loop, sizeof loop);
            FlushInstructionCache(GetCurrentProcess(), code, sizeof loop);
            api->exec_command("profile on"); Sleep(100);
            tj = CreateThread(NULL, 0, jit_thread, code, 0, NULL);
            td = CreateThread(NULL, 0, dll_churn_thread, NULL, 0, NULL);
            for (DWORD u0 = GetTickCount(); GetTickCount() - u0 < 2000;) { PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE); Sleep(8); }
            api->exec_command("profile report"); Sleep(600);
            g_jit_stop = 1;
            if (tj) WaitForSingleObject(tj, INFINITE);
            if (td) WaitForSingleObject(td, INFINITE);
        }
        snprintf(path, sizeof path, "%sv3x\\crash.log", dir);
        char *cl = read_file(path);
        printf("unwind hardening: %ld dll load/unload cycles, crash.log %s\n", g_dll_cycles, cl ? "present" : "absent");
        check(code && tj && td && g_dll_cycles > 10, "unwind stress: JIT spinner + DLL churn ran");
        check(!cl || !strstr(cl, "EXCEPTION"), "unwind stress: no exception logged by crash_logger");
    }

    printf("%s\n", fails ? "FAILED" : "ALL OK");
    return fails;
}
