/* tick_profiler: per-CTickTask wall-time profile of the game simulation.
 *
 * Hook point: RunTask (victoria3.exe 1.15.1: exe+0x146d010), the function that runs ONE tick task
 * (Prepare = vtable slot 6, then the range/context workers it set up). Signature, from its
 * disassembly: void RunTask(void *runner_ctx /rcx/, CTickTask *task /rdx/); it keeps rdx in r14
 * and dispatches `mov rax,[r14]; mov rcx,r14; call [rax+0x30]`. Hooking slot 6 per class would
 * be wrong: 69 classes share the slot-6 wrapper 0x84fb00, and slot 6 is only Prepare for
 * range tasks (see docs/re/simulation.md).
 * Anchor: the only xref to the string "Context %d" lies inside RunTask (-> .pdata start);
 * we also check the entry saves rdx in r14 and contains the call [rax+0x30].
 *
 * Task identity = the task's vtable (*(void**)task). Names come from MSVC RTTI (vtable[-1] ->
 * CompleteObjectLocator -> TypeDescriptor), pre-filled at load for every CTickTask_* class via
 * include/v3x_rtti.h; unknown vtables are added lock-free on first sight into the same fixed
 * table. The detour does no allocation and takes no locks (Interlocked* only).
 *
 * Output: top-25 table (task, calls, total ms, avg us, max ms, % of top-level task time) every
 * 60 s wall clock if tasks ran since the last report, on load_transition Game->Empty (API v3), and
 * on console "ticks"; "ticks reset" clears. Each report also rewrites v3x/tick_profile.csv
 * (all tasks); on the core's "process_exit" event (exiting thread, all threads alive) only the CSV
 * and one log line. Nothing at DLL_PROCESS_DETACH (other threads are dead, it must not lock or log).
 * Disable: env V3X_NO_TICKPROF=1. Fails closed (non-zero, no hook) if <50 CTickTask_ vtables
 * resolve or the anchor does not match. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v3x.h"
#include "v3x_rtti.h"

V3X_EXPORT const char *v3x_plugin_name = "tick_profiler";
V3X_PLUGIN_MANIFEST("tick_profiler", "0.5.0", "v3x", "per-task wall-time profile of the simulation tick", "release/1.15.1");

#define TAG "tick_profiler"
#define TBL 1024                  /* power of two, > 4x the ~270 task classes */
#define OTHER TBL                 /* bucket for a full table / NULL task */
#define REPORT_SEC 60
#define TOPN 25

static const V3X_API *api;
static int has_v2;
static uintptr_t img_lo, img_hi;
static double qpc_freq;

static void *volatile g_key[TBL];         /* vtable */
static const char *volatile g_name[TBL];  /* mangled RTTI name (in the exe) or NULL */
static volatile LONG64 g_calls[TBL + 1], g_total[TBL + 1], g_max[TBL + 1];
static volatile LONG64 g_top_total, g_all_calls;
static __thread int t_depth;

typedef uint64_t (*run_fn)(void *, void *, void *, void *);
static run_fn orig_run;

static const char *rtti_name_of(void *vt)
{
    uintptr_t v = (uintptr_t)vt;
    if (v < img_lo + 8 || v >= img_hi || (v & 7)) return 0;
    uintptr_t col = *(const uintptr_t *)(v - 8);
    if (col < img_lo || col + 24 > img_hi || (col & 3)) return 0;
    const uint32_t *c = (const uint32_t *)col;
    if (c[0] != 1 || c[5] != col - img_lo) return 0;
    uintptr_t td = img_lo + c[3];
    if (td + 20 > img_hi) return 0;
    const char *n = (const char *)(td + 16);
    return (n[0] == '.' && n[1] == '?' && n[2] == 'A') ? n : 0;
}

static unsigned slot_hash(void *vt)
{
    return (unsigned)((((uint64_t)(uintptr_t)vt >> 3) * 0x9E3779B97F4A7C15ull) >> 54) & (TBL - 1);
}

/* find or insert (lock-free) the slot of a vtable */
static int slot_of(void *vt)
{
    unsigned h = slot_hash(vt);
    for (unsigned i = 0; i < TBL; i++, h = (h + 1) & (TBL - 1)) {
        void *k = g_key[h];
        if (k == vt) return (int)h;
        if (!k) {
            void *prev = InterlockedCompareExchangePointer((void *volatile *)&g_key[h], vt, 0);
            if (!prev) { g_name[h] = rtti_name_of(vt); return (int)h; }
            if (prev == vt) return (int)h;
        }
    }
    return OTHER;
}

static void atomic_max(volatile LONG64 *p, LONG64 v)
{
    LONG64 cur = *p;
    while (v > cur) {
        LONG64 seen = InterlockedCompareExchange64(p, v, cur);
        if (seen == cur) break;
        cur = seen;
    }
}

static volatile LONG g_first_seen;

static uint64_t hk_run(void *ctx, void *task, void *c, void *d)
{
    int s = task ? slot_of(*(void **)task) : OTHER;
    if (!g_first_seen && !InterlockedExchange(&g_first_seen, 1)) {
        const char *n = s != OTHER ? g_name[s] : 0;
        api->log(TAG, "first RunTask call seen (task %s, thread %lu)", n ? n : "?", GetCurrentThreadId());
    }
    int top = t_depth++ == 0;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    uint64_t r = orig_run(ctx, task, c, d);
    QueryPerformanceCounter(&t1);
    t_depth--;
    LONG64 dt = t1.QuadPart - t0.QuadPart;
    InterlockedIncrement64(&g_calls[s]);
    InterlockedAdd64(&g_total[s], dt);
    atomic_max(&g_max[s], dt);
    InterlockedIncrement64(&g_all_calls);
    if (top) InterlockedAdd64(&g_top_total, dt);
    return r;
}

/* ---- reporting ---- */

static void outf(const char *fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (has_v2) api->print("%s", buf); else api->log(TAG, "%s", buf);
}

static void plain_name(int s, char *out, size_t n)
{
    if (s == OTHER) { snprintf(out, n, "(other)"); return; }
    const char *m = g_name[s];
    if (!m) m = g_name[s] = rtti_name_of(g_key[s]);
    if (!m) { snprintf(out, n, "vtable exe+0x%llx", (unsigned long long)((uintptr_t)g_key[s] - img_lo)); return; }
    m += 4;   /* ".?AV" */
    size_t k = 0;
    while (m[k] && m[k] != '@' && k < n - 1) { out[k] = m[k]; k++; }
    out[k] = 0;
}

typedef struct { int s; LONG64 calls, total, max; } Row;
static Row rows[TBL + 1];                       /* only used by report (main/console thread) */
static volatile LONG g_reporting;

static int cmp_row(const void *a, const void *b)
{
    LONG64 x = ((const Row *)a)->total, y = ((const Row *)b)->total;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* CSV via Win32 file calls. Returns rows written or -1. */
static int write_csv(int n, double all, double ms)
{
    char path[MAX_PATH], line[256], nm[128];
    snprintf(path, sizeof path, "%stick_profile.csv", api->v3x_dir());
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { api->log(TAG, "cannot write %s (err %lu)", path, GetLastError()); return -1; }
    DWORD w;
    int k = snprintf(line, sizeof line, "task,calls,total_ms,avg_us,max_ms,pct\r\n");
    WriteFile(h, line, (DWORD)k, &w, 0);
    for (int i = 0; i < n; i++) {
        plain_name(rows[i].s, nm, sizeof nm);
        k = snprintf(line, sizeof line, "%s,%lld,%.3f,%.3f,%.3f,%.3f\r\n", nm, (long long)rows[i].calls,
                     rows[i].total * ms, rows[i].total * ms * 1000.0 / rows[i].calls, rows[i].max * ms,
                     all > 0 ? 100.0 * rows[i].total / all : 0.0);
        if (k > 0) WriteFile(h, line, (DWORD)(k < (int)sizeof line ? k : (int)sizeof line - 1), &w, 0);
    }
    CloseHandle(h);
    return n;
}

/* quiet: no console table (DLL unload), only the CSV and one log line */
static void report(const char *why, int quiet)
{
    if (InterlockedCompareExchange(&g_reporting, 1, 0)) return;   /* one report at a time */
    int n = 0;
    for (int s = 0; s <= TBL; s++) {
        LONG64 c = g_calls[s];
        if (!c) continue;
        rows[n].s = s; rows[n].calls = c; rows[n].total = g_total[s]; rows[n].max = g_max[s]; n++;
    }
    qsort(rows, n, sizeof rows[0], cmp_row);
    double all = (double)g_top_total;
    double ms = 1000.0 / qpc_freq;
    int wrote = write_csv(n, all, ms);
    api->log(TAG, "report (%s): %d tasks, %lld runs, %.1f ms top-level task time, csv %s", why, n,
             (long long)g_all_calls, all * ms, wrote >= 0 ? "written" : "FAILED");
    if (quiet) { InterlockedExchange(&g_reporting, 0); return; }
    outf("%-48s %9s %11s %10s %9s %6s", "task", "calls", "total ms", "avg us", "max ms", "%");
    char nm[128];
    for (int i = 0; i < n && i < TOPN; i++) {
        plain_name(rows[i].s, nm, sizeof nm);
        outf("%-48.48s %9lld %11.1f %10.1f %9.2f %6.2f", nm, (long long)rows[i].calls,
             rows[i].total * ms, rows[i].total * ms * 1000.0 / rows[i].calls, rows[i].max * ms,
             all > 0 ? 100.0 * rows[i].total / all : 0.0);
    }
    InterlockedExchange(&g_reporting, 0);
}

static void reset_all(void)
{
    for (int s = 0; s <= TBL; s++) {
        InterlockedExchange64(&g_calls[s], 0);
        InterlockedExchange64(&g_total[s], 0);
        InterlockedExchange64(&g_max[s], 0);
    }
    InterlockedExchange64(&g_top_total, 0);
    InterlockedExchange64(&g_all_calls, 0);
}

static void cmd_ticks(const char *args, void *user)
{
    (void)user;
    if (!strncmp(args, "reset", 5)) { reset_all(); outf("tick_profiler: counters reset"); return; }
    report("console", 0);
}

/* every frame: report every REPORT_SEC of wall time if any task ran since the last report */
static LONG64 g_last_report_calls;
static void on_frame(void *user)
{
    (void)user;
    static LARGE_INTEGER last;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!last.QuadPart) { last = now; return; }
    if ((now.QuadPart - last.QuadPart) / qpc_freq < REPORT_SEC) return;
    last = now;
    LONG64 c = g_all_calls;
    if (c != g_last_report_calls) { g_last_report_calls = c; report("periodic", 0); }
}

/* "load_transition" (API v3, from fast_load): report when leaving a game (Game -> Empty) */
static void on_transition(const char *name, void *data, void *user)
{
    (void)name; (void)user;
    const V3X_LoadTransition *t = (const V3X_LoadTransition *)data;
    if (t && t->from && t->to && !strcmp(t->from, "Game") && !strcmp(t->to, "Empty") && g_all_calls) {
        g_last_report_calls = g_all_calls;
        report("Game->Empty", 0);
    }
}

static volatile LONG g_hooked;

/* "process_exit" (core API v3): ExitProcess was called, every thread still alive */
static void on_process_exit(const char *name, void *data, void *user)
{
    (void)name; (void)data; (void)user;
    if (g_hooked && g_all_calls) report("exit", 1);
}

/* Process exit (reserved != NULL): every other thread is already gone and may have died holding
 * the core log lock, a CRT/heap lock or the console, so do nothing here (the exit CSV was written on
 * "process_exit"). Only a FreeLibrary-style unload (reserved == NULL, threads alive) writes one. */
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)h;
    if (reason == DLL_PROCESS_DETACH && !reserved && g_hooked && g_all_calls) report("unload", 1);
    return TRUE;
}

/* ---- load ---- */

static int g_class_count;
static void prefill(const char *name, void *vt, void *user)
{
    (void)name; (void)user;
    slot_of(vt);
    g_class_count++;
}

static void *find_runtask(void)
{
    void *s = api->find_string("Context %d");
    void *x = s ? api->find_xref(s, 0) : 0;
    if (!x) { api->log(TAG, "anchor string 'Context %%d' or its xref not found"); return 0; }
    if (api->find_xref(s, 1)) { api->log(TAG, "anchor 'Context %%d' has >1 xref, unsupported version"); return 0; }
    unsigned char *f = (unsigned char *)api->function_start(x);
    if (!f) { api->log(TAG, "RunTask start not found"); return 0; }
    /* entry must save rdx in r14 (mov r14, rdx = 4C 8B F2) and dispatch call [rax+0x30] (FF 50 30)
     * with mov rcx, r14 (49 8B CE) before it */
    int sav = 0, call = 0;
    for (int i = 0; i < 0x40 && !sav; i++) sav = f[i] == 0x4C && f[i + 1] == 0x8B && f[i + 2] == 0xF2;
    for (int i = 0; i < 0x180 && !call; i++)
        call = f[i] == 0x49 && f[i + 1] == 0x8B && f[i + 2] == 0xCE && f[i + 3] == 0xFF && f[i + 4] == 0x50 && f[i + 5] == 0x30;
    if (!sav || !call) { api->log(TAG, "RunTask shape mismatch (save=%d call=%d)", sav, call); return 0; }
    return f;
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    const char *off = getenv("V3X_NO_TICKPROF");
    if (off && off[0] == '1') { api->log(TAG, "disabled by V3X_NO_TICKPROF=1"); return 0; }
    has_v2 = api->api_version >= 2 && api->struct_size >= offsetof(V3X_API, exec_command) + sizeof(void *);
    img_lo = api->image_base(); img_hi = img_lo + api->image_size();
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); qpc_freq = (double)fq.QuadPart;

    V3xRtti r;
    int rc = v3x_rtti_build(&r, img_lo, api->image_size());
    if (rc) { api->log(TAG, "RTTI index failed (%d)", rc); return 1; }
    v3x_rtti_enum(&r, "CTickTask_", prefill, 0);
    api->log(TAG, "RTTI: %u COLs, %d CTickTask_ vtables", r.n, g_class_count);
    v3x_rtti_free(&r);
    if (g_class_count < 50) { api->log(TAG, "only %d tick tasks resolved (<50), not hooking", g_class_count); return 2; }

    void *fn = find_runtask();
    if (!fn) return 3;
    rc = api->hook(fn, (void *)hk_run, (void **)&orig_run);
    api->log(TAG, "RunTask at exe+0x%llx, hook %s", (unsigned long long)((uintptr_t)fn - img_lo),
             rc == 0 ? "installed" : "FAILED");
    if (rc) return 4;
    g_hooked = 1;
    api->on_frame(on_frame, 0);
    if (api->api_version >= 3 && api->struct_size >= offsetof(V3X_API, on_event) + sizeof(void *))
    {
        api->on_event("load_transition", on_transition, 0);
        api->on_event("process_exit", on_process_exit, 0);
    }
    if (has_v2) api->register_command("ticks", "tick task profile; 'ticks reset' clears it", cmd_ticks, 0);
    return 0;
}
