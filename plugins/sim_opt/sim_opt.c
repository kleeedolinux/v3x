/* sim_opt: simulation-tick diagnostics and (opt-in) task changes. Default: only the read-only diag_trades / diag_journal probes are on.
 *
 * Background (docs/re/simulation.md, victoria3.exe 1.15.1): RunTask (exe+0x146d010) runs a tick
 * task as a chain of STAGES. For each stage it zero-fills a Step on its stack (rsp+0x50) and calls
 * the task's vtable slot 6 ("Execute", `call [rax+0x30]`, rcx = task, rdx = Step*). Execute fills:
 *   Step+0x00 u32 flags   bit0 = run the range in parallel (RunRangeExecutionParallel 0x146c960),
 *                         bit1 = run the whole range in one call, neither = serial per-item loop
 *                         (with RenderFrameIfNeeded between items)
 *   Step+0x10 std::function "single" body, impl ptr at Step+0x48
 *   Step+0x50 std::function range body,     impl ptr at Step+0x88
 *   Step+0x90/0x94 i32 range begin/end; Step+0x98/0x9c batch mode; Step+0xa0..0xb0 cost heuristic
 * If both impl pointers are still NULL after Execute, RunTask ends the task (no work at all).
 * 69 classes use the shared slot-6 wrapper 0x84fb00, which only stores "call slot 7" as the
 * single body, so wrapping slot 6 covers every class the same way.
 *
 * We replace slot 6 in each CTickTask_* vtable (8-byte pointer write in .rdata via api->patch)
 * with one shared detour. This chains with tick_profiler, which inline-hooks RunTask itself.
 *
 * Features (sim_opt.ini, section [sim_opt]):
 *   diag=1      per task: runs, Execute time, stage kind of the FIRST stage (parallel / whole /
 *               serial-loop / single / empty), range size, thread (main or worker). Read-only.
 *               Report: console "simopt", load_transition Game->Empty, v3x/sim_opt_diag.csv.
 *   throttle=1  for tasks listed in throttle_tasks=Name:N,... skip 1 of every N runs (N 2..64):
 *               the detour returns without calling Execute, so the Step stays empty and RunTask
 *               ends the task. CHANGES THE SIMULATION; single-player only (the skip counter is
 *               local to this process: MP clients would desync). Never two skips in a row.
 *               Tasks in the deny list (modifier recalculation, object removal, paired
 *               prepare/flush, validation, ...) are refused. Console: "simopt throttle on|off".
 *   diag_trades=1 (DEFAULT ON, read-only): UpdateStateTrades probe -- iterations per run, records
 *               per iteration, run / parallel-phase / longest-iteration time (see the block below).
 *   diag_journal=1 (DEFAULT ON, read-only): journal pulse cost per journal-entry type (ExecuteOnPulse)
 *               and of the per-country stage, top 20 (see the block below).
 * Disable completely: enabled=0 or env V3X_NO_SIMOPT=1.
 *
 * Fails closed (no patch, non-zero return): RTTI index fails or <50 CTickTask_ vtables; RunTask
 * anchor ("Context %d" single xref) missing; any of the three RunTask byte shapes below missing
 * (step zero-init, Execute dispatch with Step at rsp+0x50, empty-step exit); slots 1..5 of a class
 * differ from the common pattern or slot 6 is outside .text (that class is skipped). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v3x.h"
#include "v3x_rtti.h"

V3X_EXPORT const char *v3x_plugin_name = "sim_opt";

#define TAG "sim_opt"
#define MAXT 512
#define NAMELEN 64

/* Step layout (see header) */
#define STEP_FLAGS 0x00
#define STEP_SINGLE_IMPL 0x48
#define STEP_RANGE_IMPL 0x88
#define STEP_BEGIN 0x90
#define STEP_END 0x94

enum { K_EMPTY, K_SINGLE, K_SERIAL, K_WHOLE, K_PARALLEL, K_N };
static const char *KIND_NAMES[K_N] = { "empty", "single", "serial-loop", "whole", "parallel" };

typedef void (*exec_fn)(void *task, void *step);
/* Detour rule (crash 2026-11-13, see diag_trades): every pass-through detour takes and forwards 8
 * integer-class args (4 registers + 4 stack slots [rsp+0x28..0x40]), so a target with up to 8 args
 * receives them unchanged; reading unused slots only reads the caller's own stack frame. Targets
 * with float/vector args in xmm0-3 are not hooked by this plugin (none of ours take any). */
#define PASS8_ARGS void *a0_, void *a1_, void *a2_, void *a3_, void *a4_, void *a5_, void *a6_, void *a7_
#define PASS8 a0_, a1_, a2_, a3_, a4_, a5_, a6_, a7_
typedef uint64_t (*pass8_fn)(PASS8_ARGS);

typedef struct {
    void *vt;                 /* primary vtable */
    exec_fn orig;             /* original slot 6 */
    char name[NAMELEN];
    int every;                /* throttle: skip 1 of every N runs; 0 = never */
    volatile LONG64 runs, skipped, exec_qpc, kind[K_N], range_sum, range_max, on_main;
    volatile LONG since_skip; /* runs since the last skip */
} Task;

static const V3X_API *api;
static int has_v2, has_v3;
static uintptr_t img_lo, img_hi, text_lo, text_hi;
static double qpc_freq;
static DWORD g_main_tid;

static Task g_t[MAXT];
static int g_nt;
static Task *volatile g_map[1024];   /* vtable -> task, open addressing, filled before patching */

static struct {
    int enabled, diag, throttle, diag_trades, diag_journal;
    char throttle_tasks[2048];
} cfg;
static volatile LONG g_throttle_on;  /* runtime switch (console) */

static unsigned vt_hash(void *vt)
{
    return (unsigned)((((uint64_t)(uintptr_t)vt >> 3) * 0x9E3779B97F4A7C15ull) >> 54) & 1023;
}

static Task *task_of(void *vt)
{
    unsigned h = vt_hash(vt);
    for (unsigned i = 0; i < 1024; i++, h = (h + 1) & 1023) {
        Task *t = g_map[h];
        if (!t) return 0;
        if (t->vt == vt) return t;
    }
    return 0;
}

static int map_insert(Task *t)
{
    unsigned h = vt_hash(t->vt);
    for (unsigned i = 0; i < 1024; i++, h = (h + 1) & 1023)
        if (!g_map[h]) { g_map[h] = t; return 0; }
    return -1;
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

static int step_kind(const unsigned char *step)
{
    uint32_t fl = *(const uint32_t *)(step + STEP_FLAGS);
    int has_range = *(void *const *)(step + STEP_RANGE_IMPL) != 0;
    int has_single = *(void *const *)(step + STEP_SINGLE_IMPL) != 0;
    if (has_range) return (fl & 1) ? K_PARALLEL : (fl & 2) ? K_WHOLE : K_SERIAL;
    return has_single ? K_SINGLE : K_EMPTY;
}

/* Should this run be skipped? Lock-free; never two skips in a row. */
static int throttle_skip(Task *t)
{
    if (!g_throttle_on || t->every < 2) return 0;
    for (;;) {   /* CAS so concurrent runs of one task (never seen in-game) cannot double-skip */
        LONG n = t->since_skip;
        int skip = n + 1 >= t->every;
        if (InterlockedCompareExchange(&t->since_skip, skip ? 0 : n + 1, n) == n) return skip;
    }
}

static void call_orig(Task *t, PASS8_ARGS)
{
    ((pass8_fn)t->orig)(PASS8);
}

/* the shared slot-6 detour: rcx = task, rdx = Step* */
static void ex_detour(PASS8_ARGS)
{
    void *task = a0_, *step = a1_;   /* RunTask calls Execute(task, Step*) via call [rax+0x30] */
    Task *t = task ? task_of(*(void **)task) : 0;
    if (!t) {   /* cannot happen (we only patch vtables we registered); fail safe */
        return;
    }
    if (throttle_skip(t)) { InterlockedIncrement64(&t->skipped); return; }
    if (!cfg.diag) { call_orig(t, PASS8); return; }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    call_orig(t, PASS8);
    QueryPerformanceCounter(&t1);
    InterlockedIncrement64(&t->runs);
    InterlockedAdd64(&t->exec_qpc, t1.QuadPart - t0.QuadPart);
    int k = step_kind((const unsigned char *)step);
    InterlockedIncrement64(&t->kind[k]);
    if (k >= K_SERIAL) {
        int32_t b = *(const int32_t *)((const char *)step + STEP_BEGIN), e = *(const int32_t *)((const char *)step + STEP_END);
        LONG64 n = e > b ? (LONG64)e - b : 0;
        InterlockedAdd64(&t->range_sum, n);
        atomic_max(&t->range_max, n);
    }
    if (GetCurrentThreadId() == g_main_tid) InterlockedIncrement64(&t->on_main);
}

/* ---------------------------------------------------------------- config */

static const char *DENY[] = {   /* never throttled: correctness or paired state */
    "RecalculateModifierNodes", "RemoveDeadObjects", "RemoveNonexistantCountries", "PrepareNavyAIData",
    "FlushNavyAIData", "ClearLogisticsData", "ClearOutlinerData", "MoveArmies", "MoveFleets", "MoveFormations",
    "UpdateActiveBattles", "UpdateActiveNavalBattles", "ProcessAttritionCasualties", "UpdateRichPresence",
    "MonthlyPulseJournalEntries", "YearlyPulseJournalEntries", "UpdateJournalEntries", 0 };
static const char *DENY_PREFIX[] = { "Remove", "Clear", "Flush", "Validate", "Prepare", "Recalculate", "Invalidate", 0 };

static int denied(const char *n)
{
    for (int i = 0; DENY[i]; i++) if (!strcmp(n, DENY[i])) return 1;
    for (int i = 0; DENY_PREFIX[i]; i++) if (!strncmp(n, DENY_PREFIX[i], strlen(DENY_PREFIX[i]))) return 1;
    return 0;
}

/* plain task name without "CTickTask_" / template prefix */
static const char *short_name(const char *n)
{
    const char *p = strstr(n, "CTickTask_");
    return p ? p + 10 : n;
}

static void load_cfg(void)
{
    char ini[MAX_PATH];
    snprintf(ini, sizeof ini, "%ssim_opt.ini", api->v3x_dir());
    cfg.enabled = GetPrivateProfileIntA("sim_opt", "enabled", 1, ini);
    cfg.diag = GetPrivateProfileIntA("sim_opt", "diag", 0, ini);
    cfg.throttle = GetPrivateProfileIntA("sim_opt", "throttle", 0, ini);
    cfg.diag_trades = GetPrivateProfileIntA("sim_opt", "diag_trades", 1, ini);
    cfg.diag_journal = GetPrivateProfileIntA("sim_opt", "diag_journal", 1, ini);
    GetPrivateProfileStringA("sim_opt", "throttle_tasks", "", cfg.throttle_tasks, sizeof cfg.throttle_tasks, ini);
}

/* apply "Name:N,Name:N" to the task table; returns the number of throttled tasks */
static int apply_throttle_list(const char *list)
{
    int n = 0;
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", list);
    for (char *tok = strtok(buf, ", \t"); tok; tok = strtok(0, ", \t")) {
        char *c = strchr(tok, ':');
        int every = c ? atoi(c + 1) : 0;
        if (c) *c = 0;
        if (every < 2 || every > 64) { api->log(TAG, "throttle '%s': N=%d out of range 2..64, ignored", tok, every); continue; }
        if (denied(tok)) { api->log(TAG, "throttle '%s': refused (deny list)", tok); continue; }
        Task *hit = 0;
        for (int i = 0; i < g_nt; i++) if (!strcmp(short_name(g_t[i].name), tok)) { hit = &g_t[i]; break; }
        if (!hit) { api->log(TAG, "throttle '%s': no such patched task, ignored", tok); continue; }
        hit->every = every;
        n++;
        api->log(TAG, "throttle %s: skip 1 of every %d runs%s", tok, every,
                 every % 2 == 0 ? " (WARNING: even N can alias with 4 ticks/day)" : "");
    }
    return n;
}

/* ---------------------------------------------------------------- report */

static void outf(const char *fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (has_v2) api->print("%s", buf); else api->log(TAG, "%s", buf);
}


/* ---------------------------------------------------------------- diag_trades (read-only)
 * UpdateStateTrades (StateManager 0xdd1650, statemanager.cpp) is an iterative solver: per
 * iteration it compacts its candidate list, evaluates every record in parallel through the
 * parallel-for 0xe02c90 (params rcx: +0xc begin, +0x10 end, +0x14 max jobs, +0x18 batch), applies
 * them serially in order, and repeats until nothing changes (docs/re/simulation.md). We hook the
 * entry of both (MinHook, nothing else hooks them) and record per run: iterations (= parallel-for
 * calls), records per iteration, run time, parallel-phase time and the longest iteration (from one
 * parallel-for entry to the next, or to the end of the run).
 * Anchor: of the functions referencing "UpdateStateTrades", exactly one contains the call site
 * `lea rdx,[rbp+0x270]; lea rcx,[rbp-0x70]; vzeroupper; call rel32` (exactly once); its target must
 * have the 1.15.1 prologue and read begin/end from +0xc/+0x10. Otherwise the probe stays off. */
#define TR_CALL_PAT "48 8D 95 70 02 00 00 48 8D 4D 90 C5 F8 77 E8"
#define TR_PFOR_PROLOGUE "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 81 EC"
#define TR_PFOR_RANGE "8B 59 0C 8B 71 10"
#define TR_SCAN 0x1200
#define TR_SLOW_MS 1000.0

static pass8_fn orig_trades, orig_pfor;
static void *g_tr_fn, *g_tr_pfor;
static int g_tr_on;
static volatile LONG g_tr_lock;
static __thread int t_tr_in;
static __thread LONG64 t_tr_t0, t_tr_last, t_tr_par, t_tr_itmax, t_tr_recs;
static __thread int t_tr_iters, t_tr_n0, t_tr_nlast, t_tr_nmax;
#define TR_HB 6   /* iteration histogram: 0, 1, 2-3, 4-7, 8-15, 16+ */
static struct {
    LONG64 runs, iters, iters_max, recs, n_max, par_qpc, run_qpc, run_qpc_max, iter_qpc_max, other_calls, slow_runs;
    LONG64 hist[TR_HB];
    int last_iters, last_n0, last_nlast; double last_ms;
} tr;

static int tr_bucket(int it) { return it <= 0 ? 0 : it == 1 ? 1 : it < 4 ? 2 : it < 8 ? 3 : it < 16 ? 4 : 5; }
static const char *TR_HNAMES[TR_HB] = { "0", "1", "2-3", "4-7", "8-15", "16+" };

/* 0xe02c90 takes 5 args: (params, captures, r8, r9, [rsp+0x28] = source-location record; the caller
 * stores it at 0xdd22fb). */
static uint64_t hk_pfor(PASS8_ARGS)
{
    void *p = a0_;
    if (!t_tr_in) { InterlockedIncrement64(&tr.other_calls); return orig_pfor(PASS8); }
    LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
    if (t_tr_iters && t0.QuadPart - t_tr_last > t_tr_itmax) t_tr_itmax = t0.QuadPart - t_tr_last;
    t_tr_last = t0.QuadPart;
    int n = p ? *(const int32_t *)((const char *)p + 0x10) - *(const int32_t *)((const char *)p + 0xc) : 0;
    if (n < 0) n = 0;
    if (!t_tr_iters) t_tr_n0 = n;
    t_tr_nlast = n; if (n > t_tr_nmax) t_tr_nmax = n;
    t_tr_iters++; t_tr_recs += n;
    uint64_t r = orig_pfor(PASS8);
    QueryPerformanceCounter(&t1);
    t_tr_par += t1.QuadPart - t0.QuadPart;
    return r;
}

/* 0xdd1650 takes 1 arg (rcx = StateManager); it only uses its 4 home slots as scratch. */
static uint64_t hk_trades(PASS8_ARGS)
{
    if (t_tr_in) return orig_trades(PASS8);   /* re-entry: count only the outer run */
    t_tr_in = 1; t_tr_iters = 0; t_tr_recs = 0; t_tr_par = 0; t_tr_itmax = 0; t_tr_n0 = t_tr_nlast = t_tr_nmax = 0;
    LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
    t_tr_t0 = t_tr_last = t0.QuadPart;
    uint64_t r = orig_trades(PASS8);
    QueryPerformanceCounter(&t1);
    t_tr_in = 0;
    LONG64 run = t1.QuadPart - t0.QuadPart;
    if (t_tr_iters && t1.QuadPart - t_tr_last > t_tr_itmax) t_tr_itmax = t1.QuadPart - t_tr_last;
    while (InterlockedCompareExchange(&g_tr_lock, 1, 0)) YieldProcessor();
    tr.runs++; tr.iters += t_tr_iters; if (t_tr_iters > tr.iters_max) tr.iters_max = t_tr_iters;
    tr.recs += t_tr_recs; if (t_tr_nmax > tr.n_max) tr.n_max = t_tr_nmax;
    tr.par_qpc += t_tr_par; tr.run_qpc += run; if (run > tr.run_qpc_max) tr.run_qpc_max = run;
    if (t_tr_itmax > tr.iter_qpc_max) tr.iter_qpc_max = t_tr_itmax;
    tr.hist[tr_bucket(t_tr_iters)]++;
    tr.last_iters = t_tr_iters; tr.last_n0 = t_tr_n0; tr.last_nlast = t_tr_nlast; tr.last_ms = run * 1000.0 / qpc_freq;
    int slow = tr.last_ms >= TR_SLOW_MS;
    if (slow) tr.slow_runs++;
    InterlockedExchange(&g_tr_lock, 0);
    if (slow)
        api->log(TAG, "diag_trades: slow UpdateStateTrades %.0f ms: %d iterations, records %d -> %d (max %d), parallel %.0f ms, "
                 "longest iteration %.0f ms", tr.last_ms, t_tr_iters, t_tr_n0, t_tr_nlast, t_tr_nmax, t_tr_par * 1000.0 / qpc_freq,
                 t_tr_itmax * 1000.0 / qpc_freq);
    return r;
}

static void tr_reset(void)
{
    while (InterlockedCompareExchange(&g_tr_lock, 1, 0)) YieldProcessor();
    memset(&tr, 0, sizeof tr);
    InterlockedExchange(&g_tr_lock, 0);
}

static void tr_report(void)
{
    if (!g_tr_on) return;
    double q = 1000.0 / qpc_freq;
    outf("diag_trades: %lld UpdateStateTrades runs, %.1f ms avg / %.0f ms max; iterations avg %.1f max %lld; records/iteration "
         "avg %.0f max %lld; parallel phase %.0f%% of run time; longest iteration %.0f ms; slow (>=1 s) runs %lld; "
         "last run %.0f ms, %d iterations, records %d -> %d",
         (long long)tr.runs, tr.runs ? tr.run_qpc * q / tr.runs : 0.0, tr.run_qpc_max * q, tr.runs ? (double)tr.iters / tr.runs : 0.0,
         (long long)tr.iters_max, tr.iters ? (double)tr.recs / tr.iters : 0.0, (long long)tr.n_max,
         tr.run_qpc ? 100.0 * tr.par_qpc / tr.run_qpc : 0.0, tr.iter_qpc_max * q, (long long)tr.slow_runs, tr.last_ms,
         tr.last_iters, tr.last_n0, tr.last_nlast);
    char h[160]; int k = 0;
    for (int i = 0; i < TR_HB; i++) k += snprintf(h + k, sizeof h - k, "%s%s:%lld", i ? " " : "", TR_HNAMES[i], (long long)tr.hist[i]);
    outf("diag_trades: iterations per run histogram %s%s", h, tr.other_calls ? " (parallel-for also called outside the task!)" : "");
}

/* rows appended to sim_opt_diag.csv */
static void tr_csv(HANDLE h)
{
    if (!g_tr_on) return;
    char line[512]; DWORD w; double q = 1000.0 / qpc_freq;
    int k = snprintf(line, sizeof line, "\r\ntrades_metric,value\r\nruns,%lld\r\nrun_ms_total,%.3f\r\nrun_ms_max,%.3f\r\n"
                     "iterations_total,%lld\r\niterations_max,%lld\r\nrecords_total,%lld\r\nrecords_max,%lld\r\nparallel_ms_total,%.3f\r\n"
                     "iteration_ms_max,%.3f\r\nslow_runs,%lld\r\n",
                     (long long)tr.runs, tr.run_qpc * q, tr.run_qpc_max * q, (long long)tr.iters, (long long)tr.iters_max,
                     (long long)tr.recs, (long long)tr.n_max, tr.par_qpc * q, tr.iter_qpc_max * q, (long long)tr.slow_runs);
    if (k > 0 && k < (int)sizeof line) WriteFile(h, line, (DWORD)k, &w, 0);
    for (int i = 0; i < TR_HB; i++) {
        k = snprintf(line, sizeof line, "iterations_%s,%lld\r\n", TR_HNAMES[i], (long long)tr.hist[i]);
        WriteFile(h, line, (DWORD)k, &w, 0);
    }
}

/* ---------------------------------------------------------------- diag_journal (read-only)
 * Journal pulses (docs/re/simulation.md): 0x10162a0(step, pulse) runs stage 1 = ExecuteOnPulse
 * 0x101c770(entry, pulse) for every active journal entry, then stage 2 = the per-country functor
 * whose Do_call 0x106d210 calls 0x10195c0(journal_mgr, country_id, pulse), which again calls
 * ExecuteOnPulse for that country's entries. pulse 0/1/2 = daily/monthly/yearly effect slot
 * (type+0x728/+0x738/+0x748). We time ExecuteOnPulse per journal-entry TYPE (key entry+0x18) and
 * 0x10195c0 per pulse and per country. Both take <= 4 args (tools/re_simopt_argcheck.py); the
 * detours forward 8 anyway (PASS8). 0xedcd00 (5 args) is deliberately NOT hooked.
 * Type name: std::string at type+0x18 (the validation 0x17ffc30 formats `type+0x18` with the
 * std::string formatter 0x807570), read with ReadProcessMemory at report time only. */
#define JR_EOP_PROLOGUE "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8D AC 24 70 FF FF FF " \
                        "48 81 EC 90 01 00 00 48 8B D9 80 B9 08 02 00 00 00 0F 84"
#define JR_DOCALL "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 41 8B 18 48 8B F1 41 8B 78 04 3B DF 74 ?? " \
                  "0F 1F 00 48 8B 46 08 44 8B 46 18 48 63 D3 48 8B 08 48 8B 14 D1 48 8B 4E 10 8B 52 18 E8"
#define JR_DOCALL_LEN 58         /* bytes in JR_DOCALL (ends with E8); rel32 follows */
#define JR_S2_PROLOGUE "40 53 41 55 41 57 48 83 EC 50 4C 8B 99 88 00 00 00 4C 8B D1 48 63 99 94 00 00 00 45 8B F8"
#define JR_TBL 2048
#define JR_CTBL 1024
#define JR_TOP 20

static pass8_fn orig_eop, orig_s2;
static void *g_jr_eop, *g_jr_s2;
static int g_jr_on;
static __thread int t_jr_depth;
typedef struct { void *volatile key; volatile LONG64 calls[3], qpc[3], max[3]; } JrType;
static JrType g_jr[JR_TBL + 1];                 /* [JR_TBL] = other (table full / bad pulse) */
typedef struct { volatile LONG key; volatile LONG used; volatile LONG64 qpc, calls; } JrCountry;
static JrCountry g_jrc[JR_CTBL];
static volatile LONG64 g_jr_s2_calls[3], g_jr_s2_qpc[3], g_jr_s2_max[3];

static JrType *jr_slot(void *type)
{
    unsigned h = (unsigned)((((uint64_t)(uintptr_t)type >> 4) * 0x9E3779B97F4A7C15ull) >> 53) & (JR_TBL - 1);
    for (unsigned i = 0; i < JR_TBL; i++, h = (h + 1) & (JR_TBL - 1)) {
        void *k = g_jr[h].key;
        if (k == type) return &g_jr[h];
        if (!k) {
            void *prev = InterlockedCompareExchangePointer((void *volatile *)&g_jr[h].key, type, 0);
            if (!prev || prev == type) return &g_jr[h];
        }
    }
    return &g_jr[JR_TBL];
}

static void jr_country_add(int32_t id, LONG64 dt)
{
    unsigned h = ((uint32_t)id * 2654435761u) & (JR_CTBL - 1);
    for (unsigned i = 0; i < JR_CTBL; i++, h = (h + 1) & (JR_CTBL - 1)) {
        JrCountry *c = &g_jrc[h];
        if (c->used && c->key == id) { InterlockedAdd64(&c->qpc, dt); InterlockedIncrement64(&c->calls); return; }
        if (!c->used && !InterlockedCompareExchange(&c->used, 2, 0)) {   /* 2 = being filled */
            c->key = id; InterlockedAdd64(&c->qpc, dt); InterlockedIncrement64(&c->calls);
            InterlockedExchange(&c->used, 1);
            return;
        }
        while (c->used == 2) YieldProcessor();
        if (c->key == id) { InterlockedAdd64(&c->qpc, dt); InterlockedIncrement64(&c->calls); return; }
    }
}

/* ExecuteOnPulse(entry, pulse) */
static uint64_t hk_eop(PASS8_ARGS)
{
    if (t_jr_depth++) { uint64_t r = orig_eop(PASS8); t_jr_depth--; return r; }   /* nested: outer one counts */
    void *type = a0_ ? *(void *const *)((const char *)a0_ + 0x18) : 0;
    unsigned pulse = (unsigned)(uintptr_t)a1_ & 0xffffffffu;
    LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
    uint64_t r = orig_eop(PASS8);
    QueryPerformanceCounter(&t1);
    t_jr_depth--;
    LONG64 dt = t1.QuadPart - t0.QuadPart;
    JrType *s = pulse < 3 && type ? jr_slot(type) : &g_jr[JR_TBL];
    if (pulse > 2) pulse = 0;
    InterlockedIncrement64(&s->calls[pulse]); InterlockedAdd64(&s->qpc[pulse], dt); atomic_max(&s->max[pulse], dt);
    return r;
}

/* 0x10195c0(journal_mgr, country_id, pulse) */
static uint64_t hk_s2(PASS8_ARGS)
{
    int32_t id = (int32_t)(uintptr_t)a1_;
    unsigned pulse = (unsigned)(uintptr_t)a2_ & 0xffffffffu;
    LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
    uint64_t r = orig_s2(PASS8);
    QueryPerformanceCounter(&t1);
    LONG64 dt = t1.QuadPart - t0.QuadPart;
    if (pulse > 2) pulse = 0;
    InterlockedIncrement64(&g_jr_s2_calls[pulse]); InterlockedAdd64(&g_jr_s2_qpc[pulse], dt); atomic_max(&g_jr_s2_max[pulse], dt);
    if (pulse == 1) jr_country_add(id, dt);
    return r;
}

/* fault-proof read of game memory (report time only) */
static int jr_read(const void *addr, void *out, size_t n)
{
    SIZE_T got = 0;
    return addr && ReadProcessMemory(GetCurrentProcess(), addr, out, n, &got) && got == n;
}

/* name of a journal-entry type: std::string at type+0x18 {buf/ptr[16], size, cap} */
static void jr_name(void *type, char *out, size_t n)
{
    unsigned char s[32];
    if (type && jr_read((const char *)type + 0x18, s, sizeof s)) {
        uint64_t size = *(uint64_t *)(s + 16), cap = *(uint64_t *)(s + 24);
        char buf[97];
        const void *src = cap > 15 ? *(const void **)s : (const void *)s;
        if (size > 0 && size < sizeof buf && cap >= size && (cap <= 15 ? 1 : jr_read(src, buf, (size_t)size))) {
            if (cap <= 15) memcpy(buf, s, (size_t)size);
            buf[size] = 0;
            int ok = 1;
            for (uint64_t i = 0; i < size && ok; i++)
                ok = (buf[i] >= 'a' && buf[i] <= 'z') || (buf[i] >= 'A' && buf[i] <= 'Z') || (buf[i] >= '0' && buf[i] <= '9') ||
                     buf[i] == '_' || buf[i] == '.' || buf[i] == '-';
            if (ok) { snprintf(out, n, "%s", buf); return; }
        }
    }
    snprintf(out, n, "type@%p", type);
}

typedef struct { JrType *t; LONG64 total; } JrRow;
static int jr_cmp(const void *a, const void *b)
{
    LONG64 x = ((const JrRow *)a)->total, y = ((const JrRow *)b)->total;
    return x < y ? 1 : x > y ? -1 : 0;
}
static JrRow g_jr_rows[JR_TBL + 1];
static int jr_rows(void)
{
    int n = 0;
    for (int i = 0; i <= JR_TBL; i++) {
        JrType *t = &g_jr[i];
        LONG64 tot = t->qpc[0] + t->qpc[1] + t->qpc[2];
        if (t->calls[0] + t->calls[1] + t->calls[2]) { g_jr_rows[n].t = t; g_jr_rows[n].total = tot; n++; }
    }
    qsort(g_jr_rows, n, sizeof g_jr_rows[0], jr_cmp);
    return n;
}

static void jr_report(void)
{
    if (!g_jr_on) return;
    double q = 1000.0 / qpc_freq;
    int n = jr_rows();
    outf("diag_journal: ExecuteOnPulse by journal-entry type (%d types), top %d by total time", n, JR_TOP);
    outf("  %-48s %8s %10s %8s %10s %10s", "type", "monthly", "month ms", "max ms", "daily ms", "yearly ms");
    char nm[100];
    for (int i = 0; i < n && i < JR_TOP; i++) {
        JrType *t = g_jr_rows[i].t;
        if (t == &g_jr[JR_TBL]) snprintf(nm, sizeof nm, "(other)"); else jr_name(t->key, nm, sizeof nm);
        outf("  %-48.48s %8lld %10.1f %8.1f %10.1f %10.1f", nm, (long long)t->calls[1], t->qpc[1] * q, t->max[1] * q,
             t->qpc[0] * q, t->qpc[2] * q);
    }
    if (g_jr_s2) {
        outf("diag_journal: per-country stage (0x10195c0): monthly %lld calls %.1f ms (max %.1f ms), daily %lld calls %.1f ms, "
             "yearly %lld calls %.1f ms", (long long)g_jr_s2_calls[1], g_jr_s2_qpc[1] * q, g_jr_s2_max[1] * q,
             (long long)g_jr_s2_calls[0], g_jr_s2_qpc[0] * q, (long long)g_jr_s2_calls[2], g_jr_s2_qpc[2] * q);
        int best[5] = { -1, -1, -1, -1, -1 };
        for (int i = 0; i < JR_CTBL; i++) {
            if (g_jrc[i].used != 1) continue;
            for (int k = 0; k < 5; k++)
                if (best[k] < 0 || g_jrc[i].qpc > g_jrc[best[k]].qpc) {
                    for (int m = 4; m > k; m--) best[m] = best[m - 1];
                    best[k] = i; break;
                }
        }
        char line[256]; int k = 0;
        for (int i = 0; i < 5 && best[i] >= 0; i++)
            k += snprintf(line + k, sizeof line - k, "%s#%ld %.1f ms", i ? ", " : "", (long)g_jrc[best[i]].key, g_jrc[best[i]].qpc * q);
        if (k) outf("diag_journal: costliest countries in the monthly per-country stage (country id): %s", line);
    }
}

static void jr_csv(HANDLE h)
{
    if (!g_jr_on) return;
    char line[400], nm[100]; DWORD w; double q = 1000.0 / qpc_freq;
    int k = snprintf(line, sizeof line, "\r\njournal_type,monthly_calls,monthly_ms,monthly_max_ms,daily_calls,daily_ms,yearly_calls,yearly_ms\r\n");
    WriteFile(h, line, (DWORD)k, &w, 0);
    int n = jr_rows();
    for (int i = 0; i < n; i++) {
        JrType *t = g_jr_rows[i].t;
        if (t == &g_jr[JR_TBL]) snprintf(nm, sizeof nm, "(other)"); else jr_name(t->key, nm, sizeof nm);
        k = snprintf(line, sizeof line, "%s,%lld,%.3f,%.3f,%lld,%.3f,%lld,%.3f\r\n", nm, (long long)t->calls[1], t->qpc[1] * q,
                     t->max[1] * q, (long long)t->calls[0], t->qpc[0] * q, (long long)t->calls[2], t->qpc[2] * q);
        if (k > 0 && k < (int)sizeof line) WriteFile(h, line, (DWORD)k, &w, 0);
    }
    if (g_jr_s2) {
        k = snprintf(line, sizeof line, "\r\njournal_country_stage,pulse,calls,ms,max_ms\r\n");
        WriteFile(h, line, (DWORD)k, &w, 0);
        static const char *P[3] = { "daily", "monthly", "yearly" };
        for (int p = 0; p < 3; p++) {
            k = snprintf(line, sizeof line, "all,%s,%lld,%.3f,%.3f\r\n", P[p], (long long)g_jr_s2_calls[p], g_jr_s2_qpc[p] * q, g_jr_s2_max[p] * q);
            WriteFile(h, line, (DWORD)k, &w, 0);
        }
    }
}

static void jr_reset(void)
{
    for (int i = 0; i <= JR_TBL; i++)
        for (int p = 0; p < 3; p++) g_jr[i].calls[p] = g_jr[i].qpc[p] = g_jr[i].max[p] = 0;   /* keys stay */
    for (int i = 0; i < JR_CTBL; i++) { g_jrc[i].qpc = 0; g_jrc[i].calls = 0; }
    for (int p = 0; p < 3; p++) g_jr_s2_calls[p] = g_jr_s2_qpc[p] = g_jr_s2_max[p] = 0;
}

static int dominant_kind(const Task *t)
{
    int best = 0;
    for (int k = 1; k < K_N; k++) if (t->kind[k] > t->kind[best]) best = k;
    return best;
}

static int cmp_exec(const void *a, const void *b)
{
    const Task *x = *(Task *const *)a, *y = *(Task *const *)b;
    return x->exec_qpc < y->exec_qpc ? 1 : x->exec_qpc > y->exec_qpc ? -1 : 0;
}

static void write_csv(Task **v, int n)
{
    char path[MAX_PATH], line[400];
    snprintf(path, sizeof path, "%ssim_opt_diag.csv", api->v3x_dir());
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { api->log(TAG, "cannot write %s", path); return; }
    DWORD w;
    int k = snprintf(line, sizeof line, "task,runs,skipped,exec_ms,first_stage,empty,single,serial_loop,whole,parallel,avg_range,max_range,on_main\r\n");
    WriteFile(h, line, (DWORD)k, &w, 0);
    for (int i = 0; i < n; i++) {
        const Task *t = v[i];
        k = snprintf(line, sizeof line, "%s,%lld,%lld,%.3f,%s,%lld,%lld,%lld,%lld,%lld,%.1f,%lld,%lld\r\n",
                     short_name(t->name), (long long)t->runs, (long long)t->skipped, t->exec_qpc * 1000.0 / qpc_freq,
                     KIND_NAMES[dominant_kind(t)], (long long)t->kind[0], (long long)t->kind[1], (long long)t->kind[2],
                     (long long)t->kind[3], (long long)t->kind[4],
                     t->runs ? (double)t->range_sum / t->runs : 0.0, (long long)t->range_max, (long long)t->on_main);
        if (k > 0 && k < (int)sizeof line) WriteFile(h, line, (DWORD)k, &w, 0);
    }
    tr_csv(h);
    jr_csv(h);
    CloseHandle(h);
}

static void report(const char *why)
{
    static Task *v[MAXT];
    int n = 0;
    for (int i = 0; i < g_nt; i++) if (g_t[i].runs || g_t[i].skipped) v[n++] = &g_t[i];
    qsort(v, n, sizeof v[0], cmp_exec);
    if (cfg.diag || g_tr_on || g_jr_on) write_csv(v, n);
    api->log(TAG, "report (%s): %d tasks ran, diag %s, diag_trades %s, throttle %s", why, n, cfg.diag ? "on" : "off",
             g_tr_on ? "on" : "off", g_throttle_on ? "on" : "off");
    tr_report();
    jr_report();
    if (!cfg.diag) return;
    outf("%-44s %8s %7s %9s %-11s %9s %6s", "task (sorted by Execute ms)", "runs", "skipped", "exec ms", "1st stage", "avg range", "main%");
    for (int i = 0; i < n && i < 30; i++) {
        const Task *t = v[i];
        outf("%-44.44s %8lld %7lld %9.1f %-11s %9.1f %6.1f", short_name(t->name), (long long)t->runs, (long long)t->skipped,
             t->exec_qpc * 1000.0 / qpc_freq, KIND_NAMES[dominant_kind(t)], t->runs ? (double)t->range_sum / t->runs : 0.0,
             t->runs ? 100.0 * t->on_main / t->runs : 0.0);
    }
}

static void reset_all(void)
{
    for (int i = 0; i < g_nt; i++) {
        Task *t = &g_t[i];
        t->runs = t->skipped = t->exec_qpc = t->range_sum = t->range_max = t->on_main = 0;
        for (int k = 0; k < K_N; k++) t->kind[k] = 0;
    }
}

static void cmd_simopt(const char *args, void *user)
{
    (void)user;
    if (!strncmp(args, "reset", 5)) { reset_all(); tr_reset(); jr_reset(); outf("sim_opt: counters reset"); return; }
    if (!strncmp(args, "throttle", 8)) {
        const char *a = args + 8; while (*a == ' ') a++;
        if (!strcmp(a, "on")) {
            if (!cfg.throttle) { outf("sim_opt: throttle=0 in sim_opt.ini; not enabling"); return; }
            InterlockedExchange(&g_throttle_on, 1);
        } else if (!strcmp(a, "off")) InterlockedExchange(&g_throttle_on, 0);
        outf("sim_opt: throttle %s", g_throttle_on ? "on" : "off");
        return;
    }
    report("console");
}

static void on_transition(const char *name, void *data, void *user)
{
    (void)name; (void)user;
    const V3X_LoadTransition *t = (const V3X_LoadTransition *)data;
    if (t && t->from && t->to && !strcmp(t->from, "Game") && !strcmp(t->to, "Empty")) report("Game->Empty");
}

/* ---------------------------------------------------------------- anchors */

static int find_bytes(const unsigned char *p, size_t n, const char *pat)
{
    /* pat: hex bytes separated by spaces, "??" wildcard; returns offset or -1 */
    unsigned char b[64]; int wild[64], m = 0;
    for (const char *s = pat; *s && m < 64;) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (s[0] == '?') { wild[m] = 1; b[m++] = 0; s += 2; continue; }
        wild[m] = 0; b[m++] = (unsigned char)strtoul(s, 0, 16); s += 2;
    }
    for (size_t i = 0; i + m <= n; i++) {
        int ok = 1;
        for (int j = 0; j < m && ok; j++) ok = wild[j] || p[i + j] == b[j];
        if (ok) return (int)i;
    }
    return -1;
}

/* RunTask shapes, all inside the function found via "Context %d" */
static const char *SHAPE_ZERO = "4C 89 65 98 4C 89 65 D8";                         /* single/range impl = 0 */
static const char *SHAPE_EXEC = "49 8B 06 48 8D 54 24 50 49 8B CE FF 50 30";       /* Execute(task, rsp+0x50) */
static const char *SHAPE_EMPTY = "48 8B 4D D8 4C 8B 45 98 4D 85 C0 75 ?? 48 85 C9 0F 84"; /* both NULL -> exit */

static int check_runtask(void)
{
    void *s = api->find_string("Context %d");
    void *x = s ? api->find_xref(s, 0) : 0;
    if (!x || api->find_xref(s, 1)) { api->log(TAG, "anchor 'Context %%d': %s", x ? ">1 xref" : "not found"); return -1; }
    const unsigned char *f = (const unsigned char *)api->function_start(x);
    if (!f) { api->log(TAG, "RunTask start not found"); return -1; }
    int z = find_bytes(f, 0x200, SHAPE_ZERO), e = find_bytes(f, 0x200, SHAPE_EXEC), m = find_bytes(f, 0x200, SHAPE_EMPTY);
    if (z < 0 || e < 0 || m < 0 || !(z < e && e < m)) {
        api->log(TAG, "RunTask shape mismatch (zero=%d exec=%d empty=%d), not patching", z, e, m);
        return -1;
    }
    api->log(TAG, "RunTask at exe+0x%llx: step zero-init +0x%x, Execute dispatch +0x%x, empty exit +0x%x",
             (unsigned long long)((uintptr_t)f - img_lo), z, e, m);
    return 0;
}

static void collect(const char *name, void *vt, void *user)
{
    (void)user;
    if (g_nt >= MAXT) return;
    Task *t = &g_t[g_nt];
    t->vt = vt;
    snprintf(t->name, sizeof t->name, "%s", name);
    g_nt++;
}

static int in_text(uintptr_t p) { return p >= text_lo && p < text_hi; }

static void find_text(void)
{
    const IMAGE_DOS_HEADER *d = (const IMAGE_DOS_HEADER *)img_lo;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(img_lo + d->e_lfanew);
    const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(s[i].Name, ".text", 6)) { text_lo = img_lo + s[i].VirtualAddress; text_hi = text_lo + s[i].Misc.VirtualSize; }
}

/* common slots 1..5 = the most frequent value per slot across all task vtables */
static uintptr_t common_slot(int k)
{
    uintptr_t best = 0; int bestc = 0;
    for (int i = 0; i < g_nt; i++) {
        uintptr_t v = ((uintptr_t *)g_t[i].vt)[k];
        int c = 0;
        for (int j = 0; j < g_nt; j++) c += ((uintptr_t *)g_t[j].vt)[k] == v;
        if (c > bestc) { bestc = c; best = v; }
    }
    return best;
}

/* Validate and register every class; returns the number patched. Patching only after all checks. */
static int patch_all(void)
{
    uintptr_t common[6];
    for (int k = 1; k <= 5; k++) common[k] = common_slot(k);
    int ok = 0, bad = 0;
    static Task tmp[MAXT]; int nt = 0;
    for (int i = 0; i < g_nt; i++) {
        uintptr_t *vt = (uintptr_t *)g_t[i].vt;
        int good = in_text(vt[6]);
        for (int k = 1; k <= 5 && good; k++) good = vt[k] == common[k];
        if (!good) { bad++; continue; }
        tmp[nt] = g_t[i];
        tmp[nt].orig = (exec_fn)vt[6];
        nt++;
    }
    memcpy(g_t, tmp, sizeof(Task) * nt);
    g_nt = nt;
    for (int i = 0; i < g_nt; i++) if (map_insert(&g_t[i])) return -1;
    for (int i = 0; i < g_nt; i++) {
        void *det = (void *)ex_detour;
        if (api->patch((uintptr_t *)g_t[i].vt + 6, &det, sizeof det)) { api->log(TAG, "patch failed for %s", g_t[i].name); continue; }
        ok++;
    }
    api->log(TAG, "patched slot 6 of %d task vtables (%d skipped: slot shape differs)", ok, bad);
    return ok;
}


static int tr_setup(void)
{
    void *s = api->find_string("UpdateStateTrades");
    if (!s) { api->log(TAG, "diag_trades: string not found, off"); return -1; }
    const unsigned char *fn = 0, *hit = 0, *seen[32];
    int nseen = 0, nfound = 0;
    for (int i = 0; i < 32; i++) {
        void *x = api->find_xref(s, i);
        if (!x) break;
        const unsigned char *f = (const unsigned char *)api->function_start(x);
        if (!f) continue;
        int dup = 0;
        for (int j = 0; j < nseen; j++) dup |= seen[j] == f;
        if (dup || nseen >= 32) continue;
        seen[nseen++] = f;
        size_t span = (uintptr_t)f + TR_SCAN <= text_hi ? TR_SCAN : (size_t)(text_hi - (uintptr_t)f);
        int off = find_bytes(f, span, TR_CALL_PAT);
        if (off < 0) continue;
        if (find_bytes(f + off + 1, span - off - 1, TR_CALL_PAT) >= 0) { api->log(TAG, "diag_trades: call site not unique, off"); return -2; }
        nfound++; fn = f; hit = f + off;
    }
    if (nfound != 1) { api->log(TAG, "diag_trades: %d candidate functions (want 1), off", nfound); return -3; }
    const unsigned char *pf = hit + 19 + *(const int32_t *)(hit + 15);
    if (!in_text((uintptr_t)pf) || find_bytes(pf, 25, TR_PFOR_PROLOGUE) != 0 || find_bytes(pf, 0x40, TR_PFOR_RANGE) < 0) {
        api->log(TAG, "diag_trades: parallel-for shape mismatch, off");
        return -4;
    }
    if (api->hook((void *)pf, (void *)hk_pfor, (void **)&orig_pfor)) { api->log(TAG, "diag_trades: hook parallel-for failed, off"); return -5; }
    if (api->hook((void *)fn, (void *)hk_trades, (void **)&orig_trades)) {
        api->unhook((void *)pf);
        api->log(TAG, "diag_trades: hook UpdateStateTrades failed, off");
        return -6;
    }
    g_tr_fn = (void *)fn; g_tr_pfor = (void *)pf; g_tr_on = 1;
    api->log(TAG, "diag_trades: on (UpdateStateTrades exe+0x%llx, parallel-for exe+0x%llx)",
             (unsigned long long)((uintptr_t)fn - img_lo), (unsigned long long)((uintptr_t)pf - img_lo));
    return 0;
}

/* diag_journal anchors: ExecuteOnPulse = the only function referencing "ExecuteOnPulse", with the
 * 1.15.1 prologue (incl. `cmp byte [rcx+0x208],0`). Stage 2 = the call target of the per-country
 * Do_call shape JR_DOCALL (4 such functors exist) whose target has the JR_S2_PROLOGUE (exactly one).
 * Stage 2 failing only disables the stage-2 timing. */
static int jr_setup(void)
{
    void *s = api->find_string("ExecuteOnPulse");
    void *x = s ? api->find_xref(s, 0) : 0;
    if (!x || api->find_xref(s, 1)) { api->log(TAG, "diag_journal: 'ExecuteOnPulse' %s, off", x ? "has >1 xref" : "not found"); return -1; }
    const unsigned char *eop = (const unsigned char *)api->function_start(x);
    if (!eop || find_bytes(eop, 64, JR_EOP_PROLOGUE) != 0) { api->log(TAG, "diag_journal: ExecuteOnPulse shape mismatch, off"); return -2; }
    const unsigned char *s2 = 0; int ns2 = 0;
    for (uintptr_t p = text_lo; p + JR_DOCALL_LEN + 4 < text_hi;) {
        int off = find_bytes((const unsigned char *)p, (size_t)(text_hi - p), JR_DOCALL);
        if (off < 0) break;
        const unsigned char *h = (const unsigned char *)p + off;
        const unsigned char *t = h + JR_DOCALL_LEN + 4 + *(const int32_t *)(h + JR_DOCALL_LEN);
        if (in_text((uintptr_t)t) && (uintptr_t)t + 32 < text_hi && find_bytes(t, 32, JR_S2_PROLOGUE) == 0) {
            if (t != s2) ns2++;
            s2 = t;
        }
        p = (uintptr_t)h + 1;
    }
    if (api->hook((void *)eop, (void *)hk_eop, (void **)&orig_eop)) { api->log(TAG, "diag_journal: hook ExecuteOnPulse failed, off"); return -3; }
    g_jr_eop = (void *)eop; g_jr_on = 1;
    if (ns2 == 1 && !api->hook((void *)s2, (void *)hk_s2, (void **)&orig_s2)) g_jr_s2 = (void *)s2;
    else api->log(TAG, "diag_journal: per-country stage not hooked (%d candidates)", ns2);
    api->log(TAG, "diag_journal: on (ExecuteOnPulse exe+0x%llx, per-country stage %s exe+0x%llx)",
             (unsigned long long)((uintptr_t)eop - img_lo), g_jr_s2 ? "on" : "off",
             (unsigned long long)(g_jr_s2 ? (uintptr_t)g_jr_s2 - img_lo : 0));
    return 0;
}

/* vtable part (diag / throttle). Returns 0 or the fail-closed code. */
static int load_tasks(void)
{
    if (check_runtask()) return 2;
    V3xRtti r;
    if (v3x_rtti_build(&r, img_lo, api->image_size())) { api->log(TAG, "RTTI index failed"); return 1; }
    v3x_rtti_enum(&r, "CTickTask_", collect, 0);
    v3x_rtti_free(&r);
    if (g_nt < 50) { api->log(TAG, "only %d CTickTask_ vtables (<50), not patching", g_nt); return 3; }
    int nthr = 0;
    if (patch_all() <= 0) return 4;
    if (cfg.throttle) {
        nthr = apply_throttle_list(cfg.throttle_tasks);
        g_throttle_on = nthr > 0;
    }
    api->log(TAG, "active: diag=%d throttle=%d (%d tasks) -- throttle changes the simulation, single-player only",
             cfg.diag, g_throttle_on, nthr);
    return 0;
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    const char *off = getenv("V3X_NO_SIMOPT");
    if (off && off[0] == '1') { api->log(TAG, "disabled by V3X_NO_SIMOPT=1"); return 0; }
    load_cfg();
    if (!cfg.enabled || (!cfg.diag && !cfg.throttle && !cfg.diag_trades && !cfg.diag_journal)) {
        api->log(TAG, "inactive (enabled=%d diag=%d throttle=%d diag_trades=%d diag_journal=%d in sim_opt.ini)", cfg.enabled,
                 cfg.diag, cfg.throttle, cfg.diag_trades, cfg.diag_journal);
        return 0;
    }
    has_v2 = api->api_version >= 2 && api->struct_size >= offsetof(V3X_API, exec_command) + sizeof(void *);
    has_v3 = api->api_version >= 3 && api->struct_size >= offsetof(V3X_API, on_event) + sizeof(void *);
    img_lo = api->image_base(); img_hi = img_lo + api->image_size();
    find_text();
    g_main_tid = GetCurrentThreadId();   /* load runs on the game's main thread */
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); qpc_freq = (double)fq.QuadPart;

    if (cfg.diag_trades) tr_setup();   /* independent of the task patching; read-only */
    if (cfg.diag_journal) jr_setup();
    int rc = cfg.diag || cfg.throttle ? load_tasks() : 0;
    if (rc && !g_tr_on && !g_jr_on) return rc;      /* nothing active */
    if (has_v3) api->on_event("load_transition", on_transition, 0);
    if (has_v2) api->register_command("simopt", "sim_opt report; 'simopt reset'; 'simopt throttle on|off'", cmd_simopt, 0);
    return 0;
}
