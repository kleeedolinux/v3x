/* Offline test of plugins/tick_profiler without the game: includes the plugin source, fakes the
 * V3X API and a tiny MSVC RTTI region, drives the RunTask detour with a fake original, and checks
 * the first-call log, report, CSV (written next to the exe), Game->Empty, process-exit (no-op), unload and reset paths.
 * Build+run (from the repo root; T = any scratch dir):
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -o $T/tick_profiler_test.exe test/tick_profiler_test.c && $T/tick_profiler_test.exe
 * Exit code 0 = ALL OK. */
#include "../plugins/tick_profiler/tick_profiler.c"

static int fails;
#define CHECK(c, ...) do { int ok_ = !!(c); printf("%s: ", ok_ ? "OK  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); fails += !ok_; } while (0)

static char logbuf[65536]; static size_t logn;
static void f_log(const char *tag, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    logn += snprintf(logbuf + logn, sizeof logbuf - logn, "[%s] ", tag);
    logn += vsnprintf(logbuf + logn, sizeof logbuf - logn, fmt, ap);
    logn += snprintf(logbuf + logn, sizeof logbuf - logn, "\n");
    va_end(ap);
}
static int f_print(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(logbuf + logn, sizeof logbuf - logn, fmt, ap); logn += k;
    logn += snprintf(logbuf + logn, sizeof logbuf - logn, "\n");
    va_end(ap); return k;
}
static char g_dir[MAX_PATH], g_csv[MAX_PATH];
static const char *f_dir(void) { return g_dir; }

static volatile int body_calls;
static uint64_t fake_run(void *ctx, void *task, void *c, void *d)
{
    (void)ctx; (void)c; (void)d; body_calls++;
    Sleep(*(int *)((char *)task + 8));
    return 42;
}

int main(void)
{
    GetModuleFileNameA(0, g_dir, MAX_PATH);
    char *sl = strrchr(g_dir, '\\'); if (!sl) sl = strrchr(g_dir, '/'); if (sl) sl[1] = 0;
    snprintf(g_csv, sizeof g_csv, "%stick_profile.csv", g_dir);
    unsigned char *im = VirtualAlloc(0, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    strcpy((char *)im + 0x110, ".?AVCTickTask_Slow@?A0x1234abcd@@");
    strcpy((char *)im + 0x210, ".?AVCTickTask_Fast@@");
    uint32_t *c1 = (uint32_t *)(im + 0x1000), *c2 = (uint32_t *)(im + 0x1018);
    c1[0] = 1; c1[3] = 0x100; c1[5] = 0x1000;
    c2[0] = 1; c2[3] = 0x200; c2[5] = 0x1018;
    *(uint64_t *)(im + 0x2000) = (uint64_t)(uintptr_t)(im + 0x1000);
    *(uint64_t *)(im + 0x2100) = (uint64_t)(uintptr_t)(im + 0x1018);
    struct { void *vt; int ms; } slow = { im + 0x2008, 30 }, fast = { im + 0x2108, 0 };

    static V3X_API A;
    A.api_version = 2; A.struct_size = sizeof A; A.log = f_log; A.print = f_print; A.v3x_dir = f_dir;
    api = &A; has_v2 = 1; img_lo = (uintptr_t)im; img_hi = img_lo + 0x10000;
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); qpc_freq = (double)fq.QuadPart;
    orig_run = fake_run; g_hooked = 1;

    uint64_t r = hk_run(0, &slow, 0, 0);
    CHECK(r == 42 && body_calls == 1, "detour passes through return value");
    CHECK(strstr(logbuf, "first RunTask call seen (task .?AVCTickTask_Slow") != 0, "first-call log line");
    hk_run(0, &slow, 0, 0);
    for (int i = 0; i < 100; i++) hk_run(0, &fast, 0, 0);
    int nfirst = 0; for (char *p = logbuf; (p = strstr(p, "first RunTask")); p++) nfirst++;
    CHECK(nfirst == 1, "first-call logged once (%d)", nfirst);
    CHECK(g_all_calls == 102, "calls counted %lld", (long long)g_all_calls);

    cmd_ticks("", 0);
    CHECK(strstr(logbuf, "CTickTask_Slow") && strstr(logbuf, "csv written"), "console report + csv");
    FILE *f = fopen(g_csv, "r");
    char csv[2048] = {0}; if (f) { fread(csv, 1, sizeof csv - 1, f); fclose(f); }
    CHECK(!strncmp(csv, "task,calls", 10) && strstr(csv, "\nCTickTask_Slow,2,") && strstr(csv, "\nCTickTask_Fast,100,"),
          "csv content:\n%s", csv);

    size_t before = logn;
    V3X_LoadTransition t = { "Game", "Empty", 1.0 };
    on_transition("load_transition", &t, 0);
    CHECK(strstr(logbuf + before, "report (Game->Empty)") != 0, "report on Game->Empty");
    before = logn;
    V3X_LoadTransition t2 = { "Empty", "Game", 1.0 };
    on_transition("load_transition", &t2, 0);
    CHECK(logn == before, "no report on Empty->Game");

    DeleteFileA(g_csv);
    before = logn;
    DllMain(0, DLL_PROCESS_DETACH, (void *)1);   /* process exit: other threads are dead, must do nothing */
    CHECK(GetFileAttributesA(g_csv) == INVALID_FILE_ATTRIBUTES && logn == before,
          "process exit detach: no csv, no log (lock-free)");
    DllMain(0, DLL_PROCESS_DETACH, NULL);        /* FreeLibrary unload: threads alive, csv + one log line */
    CHECK(GetFileAttributesA(g_csv) != INVALID_FILE_ATTRIBUTES &&
          strstr(logbuf + before, "report (unload)"), "csv written at FreeLibrary unload");
    DeleteFileA(g_csv);
    long code = 0;
    on_process_exit("process_exit", &code, 0);   /* core event: ExitProcess called, threads alive */
    CHECK(GetFileAttributesA(g_csv) != INVALID_FILE_ATTRIBUTES &&
          strstr(logbuf + before, "report (exit)"), "csv written on process_exit");

    on_frame(0); on_frame(0);   /* first call arms the timer; <60 s -> no report */
    CHECK(!strstr(logbuf + before, "periodic"), "no periodic report before 60 s");

    reset_all();
    CHECK(g_all_calls == 0 && g_calls[slot_of(fast.vt)] == 0, "ticks reset");
    printf("--- log ---\n%s", logbuf);
    printf("%s\n", fails ? "FAILED" : "ALL OK");
    return fails;
}
