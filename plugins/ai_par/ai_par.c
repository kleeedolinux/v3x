/* ai_par: where the AI / economy tick tasks spend serial vs parallel time (victoria3.exe 1.15.1).
 *
 * Static RE: docs/re/ai_par.md (offline check: tools/re_aipar_check.py, ALL OK). Short version:
 *   - Every range-parallel tick stage goes through RunRangeExecutionParallel (RRP, exe+0x146c960, only
 *     caller RunTask). It swaps the script-RNG owner out for the whole range (0x146ca14), computes
 *     T = [sched+0x648] + 1 threads (NORMAL workers + caller), grain from Step+0x98 (if Step+0x9c, else
 *     mode 1 = n/T/3), chunks = min(ceil(n/grain), T), and submits the chunks as tasks that pull grains
 *     from an atomic counter (lock xadd, 0x14d3a6b): dynamic self-scheduling. With a cost array
 *     (Step+0xa0/+0xb0) the heuristic 0x146c010 first splits items into Expensive (cost > mean+2 sigma,
 *     grain 1) and Affordable (grain n/T/3).
 *   - HandleAutonomousInvestment: stage 1 = RRP over all countries, item = eval 0x921910 (result into
 *     its own slot); stage 2 (0x9b2930) = SERIAL loop over countries: build 0x8dcea0, then re-eval
 *     0x921910 for the next build, RenderFrameIfNeeded between builds.
 *   - UpdateAI: stage 1 = serial country loop inside Execute 0x1647910 (period query); stage 2 = RRP over
 *     the selected AI subsystems, item 0x1604cc0 (local seed per item); stage 3 = SERIAL per-item apply
 *     0x1647ab0 that executes game commands (0x1085b90).
 *   - UpdateStateTrades (Execute 0xdcedd0 -> 0xdd1650): iterative solver; per iteration a parallel-for
 *     (0xe02c90) evaluates the records (0xdd26e0; inline when 1 chunk), then a SERIAL ordered apply
 *     (0xd96a90). Most iterations hold 1-4 records (sim_opt diag_trades), i.e. are inherently serial.
 *   - UpdatePopGrowth: 0xe5bbd0 UpdatePopGrowthParallel (own job), then SERIAL apply 0xe5c260.
 *
 * mode=measure (default; read-only hook_safe probes, nothing in the game changes):
 *   per range-parallel task: runs, wall, range size, threads, engine grain/chunks, heuristic used
 *     (hook result vs our replica), and for HAI / UpdateAI the per-item sum, longest item, the
 *     lower bound max(longest, sum/T) per run and what finer chunking could recover at most;
 *   per candidate loop: serial vs parallel time and the per-item cost distribution (p50/p90/p99/max);
 *   RenderFrameIfNeeded (guard dtor 0x1468be0, renders after > 10 ms) time per call site.
 *   Report: console `aipar` (`aipar reset`), on Game->Empty, on process exit -> binaries/v3x/ai_par_report.txt
 * mode=on: REFUSED (runs as measure, logged). No serial loop of these tasks is proven safe to run in
 *   parallel: every one writes shared game state in an order-dependent way or executes game commands
 *   (docs/re/ai_par.md sec. 3).
 * fine_chunks=1 (separate opt-in lever, default 0): for the listed tasks (fine_chunks_tasks) whose stage
 *   did not choose a batch mode itself, RRP gets mode 2 (grain 1: every item is pulled individually from
 *   the engine's own atomic counter); fine_chunks_heuristic=0 additionally disengages the cost heuristic
 *   (Step+0xb0) for that call. Restored right after RRP returns. Safe by construction: it selects a
 *   schedule the engine itself runs (grain 1 is what the engine uses for Expensive items, and what mode 1
 *   yields when n < 6T, i.e. on machines with many workers); item-to-thread assignment is already
 *   timing-dependent (atomic counter) and the range runs without a script-RNG owner. Does not change what
 *   is computed, only which thread takes which item when. Needs anchors RRP + heuristic.
 * Disable: enabled=0, mode=off, or env V3X_NO_AIPAR=1.
 *
 * Fails closed: RRP anchor (or its Step-offset / scheduler checks) missing -> plugin inactive; any other
 * group (HAI, UpdateAI, trades, pop growth, guard, heuristic) missing -> only that group is off. Every
 * hook is hook_safe (observe only; the lever writes 3 Step fields in pre and restores them in post). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "v3x.h"
#include "v3x_scan.h"
#include "v3x_anchor.h"
#include "v3x_hook.h"
#include "v3x_ini.h"
#include "v3x_mem.h"
#include "ai_par_core.h"

V3X_EXPORT const char *v3x_plugin_name = "ai_par";
V3X_PLUGIN_MANIFEST_EX("ai_par", "0.1.0", "v3x", "AI/economy tick tasks: serial vs parallel time probe; opt-in range chunking lever",
                       "release/1.15.1", 0u, 4u);

#define TAG "ai_par"

/* ---------------------------------------------------------------- anchors (1.15.1; same as tools/re_aipar_check.py) */
#define S_RRP   "RunRangeExecutionParallel"
#define S_HEUR  "MaybeRunRangeExecutionParallelWithHeuristic"
#define S_PGPAR "UpdatePopGrowthParallel"
#define P_RRP_PRO     "4C 89 44 24 18 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 08 FE FF FF 48 81 EC F8 02 00 00"
#define P_RRP_SCHED   "48 8B 05 ?? ?? ?? ?? 45 8B B5 94 00 00 00 45 8B A5 90 00 00 00"   /* mov rax,[sched]; Step end/begin */
#define P_RRP_MODE    "41 80 BD 9C 00 00 00 00 74 09 45 8B BD 98 00 00 00"                 /* Step+0x9c ? Step+0x98 */
#define P_RRP_WORKERS "8B 88 48 06 00 00"                                                  /* mov ecx,[rax+0x648] */
#define P_HEUR_PRO    "48 8B C4 48 89 58 10 4C 89 48 20 48 89 48 08 55 56 57 41 54 41 55 41 56 41 57"
#define P_HEUR_FLAG   "40 38 BB B0 00 00 00"                                               /* cmp [Step+0xb0],dil */
#define P_GUARD_DTOR  "48 83 EC 28 80 39 00 48 89 5C 24 38 48 8B D9 0F 84 ?? ?? ?? ?? 48 8B 41 08"
#define P_GUARD_10MS  "48 83 F8 0A"
#define P_HAI_PSITE   "4A 8B 0C 36 E8 ?? ?? ?? ?? 48 8B 4D 10 FF C3 48 89 04 0E"
#define P_HAI_SSITE   "FF C6 FF CB 85 DB 7E ?? 48 8B CD E8 ?? ?? ?? ?? 48 8B F8 48 8B 08 48 8B 51 30"
#define P_HAI_EVAL_PRO "48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 48 81 EC 70 01 00 00"
#define P_HAI_S2_PRO  "4C 8B DC 4D 89 43 18 49 89 4B 08 56 41 54 48 81 EC E8 00 00 00"
#define P_HAI_BSITE   "4C 8B C7 49 8B D6 48 8B CD E8 ?? ?? ?? ?? 84 C0 74 ?? FF C6 FF CB"
#define P_AI_S1SITE   "4C 8D 46 28 45 8B CF 48 8D 56 10 E8"
#define P_AI_EXEC_PRO "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 60"
#define P_AI_SETRNG   "83 4D 00 03"                                                        /* or [Step],3 */
#define P_AI_ISITE    "49 8B 52 10 48 81 C2 90 02 00 00 E8"
#define P_AI_ITEM_PRO "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 48 89 48 08"
#define P_AI_ASITE    "45 0F B6 09 48 8B 49 08 E9"
#define P_AI_APPLY_PRO "4C 8B DC 45 88 4B 20 49 89 53 10 55 41 55 48 81 EC D8 00 00 00"
#define P_TR_ESITE    "48 8D 4F 08 E8 ?? ?? ?? ?? FF C3 3B DE 75 E0"
#define P_TR_EVAL_PRO "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 81 EC 10 03 00 00"
#define P_TR_ASITE    "48 8D 85 90 00 00 00 48 89 44 24 20 4C 8D 8D E0 00 00 00 E8"
#define P_TR_PFOR_CALL "48 8D 95 70 02 00 00 48 8D 4D 90 C5 F8 77 E8"
#define P_TR_APPLY_PRO "4C 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10 53 55 56 57 41 54 41 55 41 56 41 57"
#define P_TR_EXEC     "48 8B 42 08 48 8B 88 20 01 00 00 48 81 C1 00 1C 00 00 E9"
#define P_PG_PAR_PRO  "40 55 53 56 57 41 54 41 57 48 8D 6C 24 D1 48 81 EC B8 00 00 00"
#define P_PG_SEQ      "48 8D 4D D7 E8 ?? ?? ?? ?? 48 8D 4D D7 E8 ?? ?? ?? ?? 48 8D 4D D7 E8"
#define P_PG_APPLY_PRO "4C 8B DC 55 56 41 54 41 57 49 8D 6B A1 48 81 EC D8 00 00 00"

/* engine layouts */
#define STEP_FLAGS 0x00
#define STEP_BEGIN 0x90
#define STEP_END 0x94
#define STEP_MODE 0x98
#define STEP_MODE_SET 0x9c
#define STEP_COST_B 0xa0
#define STEP_COST_E 0xa8
#define STEP_COST_SET 0xb0
#define TASK_TYPE 0x08
#define TYPE_NAME 0x08     /* MSVC std::string: 16-byte SSO buffer / pointer, size +0x10, capacity +0x18 */
#define SCHED_WORKERS 0x648

enum { G_RRP, G_HEUR, G_GUARD, G_HAI, G_AI, G_TR, G_PG, G_N };
static const char *G_NAME[G_N] = { "range-runner", "heuristic", "render-guard", "HandleAutonomousInvestment",
                                   "UpdateAI", "UpdateStateTrades", "UpdatePopGrowth" };

typedef struct apar_res {
    uint8_t *rrp, *heur, *guard;
    void **sched_global;
    uint8_t *hai_eval, *hai_pret, *hai_sret, *hai_s2, *hai_build;
    uint8_t *ai_exec, *ai_item, *ai_iret, *ai_apply;
    uint8_t *tr_exec, *tr_fn, *tr_eval, *tr_eret, *tr_apply;
    uint8_t *pg_par, *pg_apply;
    int ok[G_N];
    char err[G_N][120];
} apar_res;

static const V3X_API *api;
static apar_res R;
static uintptr_t g_base;

static int at(const uint8_t *p, const char *pat)
{
    V3xPattern c;
    return p && v3x_pattern_compile(pat, &c) && v3x_pattern_match_at(&c, p);
}
static uint8_t *near_(const uint8_t *from, size_t n, const char *pat) { return (uint8_t *)v3x_scan_near(from, n, pat); }
static uint8_t *call_tgt(const uint8_t *e8) { return (uint8_t *)v3x_rel32_target(e8); }
static uint8_t *fstart(const void *p) { return p ? (uint8_t *)api->function_start(p) : NULL; }

#define GFAIL(g, ...) do { snprintf(R.err[g], sizeof R.err[g], __VA_ARGS__); R.ok[g] = 0; goto next_##g; } while (0)

enum { SP_GUARD, SP_HAI_P, SP_HAI_S, SP_HAI_B, SP_AI_S1, SP_AI_I, SP_AI_A, SP_TR_A, SP_TR_X, SP_PG_SEQ, SP_N };

/* Resolves every group; returns the number of groups that resolved. Never hooks. */
static int apar_resolve(const V3X_API *a)
{
    api = a;
    memset(&R, 0, sizeof R);
    g_base = api->image_base();
    V3xSection text;
    if (!v3x_section(g_base, ".text", &text)) {
        for (int g = 0; g < G_N; g++) snprintf(R.err[g], sizeof R.err[g], "no .text");
        return 0;
    }
    apar_scanpat sp[SP_N] = {
        [SP_GUARD] = {.pattern = P_GUARD_DTOR}, [SP_HAI_P] = {.pattern = P_HAI_PSITE}, [SP_HAI_S] = {.pattern = P_HAI_SSITE}, [SP_HAI_B] = {.pattern = P_HAI_BSITE},
        [SP_AI_S1] = {.pattern = P_AI_S1SITE}, [SP_AI_I] = {.pattern = P_AI_ISITE}, [SP_AI_A] = {.pattern = P_AI_ASITE}, [SP_TR_A] = {.pattern = P_TR_ASITE},
        [SP_TR_X] = {.pattern = P_TR_EXEC}, [SP_PG_SEQ] = {.pattern = P_PG_SEQ},
    };
    apar_multi_scan(text.ptr, text.size, sp, SP_N);

    /* range runner + scheduler global (required by everything) */
    {
        V3xAnchor A = {.name = "RunRangeExecutionParallel", .string = S_RRP, .pattern = P_RRP_PRO, .xref = V3X_ANCHOR_ALL_SAME};
        if (v3x_anchor_resolve(api, &A)) GFAIL(G_RRP, "%s", A.err);
        R.rrp = (uint8_t *)A.result;
        uint8_t *s = near_(R.rrp, 0x300, P_RRP_SCHED);
        if (!s || near_(s + 1, 0x300 - (size_t)(s + 1 - R.rrp), P_RRP_SCHED)) GFAIL(G_RRP, "scheduler load not found once");
        if (!near_(s, 0x40, P_RRP_WORKERS)) GFAIL(G_RRP, "worker count read [sched+0x648] not found");
        if (!near_(R.rrp, 0x300, P_RRP_MODE)) GFAIL(G_RRP, "Step+0x98/+0x9c mode read not found");
        R.sched_global = (void **)v3x_rip_target(s, 3, 7);
        R.ok[G_RRP] = 1;
    }
next_G_RRP:
    if (!R.ok[G_RRP]) return 0;
    {
        V3xAnchor A = {.name = "heuristic", .string = S_HEUR, .pattern = P_HEUR_PRO, .xref = V3X_ANCHOR_ALL_SAME};
        if (v3x_anchor_resolve(api, &A)) GFAIL(G_HEUR, "%s", A.err);
        R.heur = (uint8_t *)A.result;
        if (!near_(R.heur, 0x100, P_HEUR_FLAG)) GFAIL(G_HEUR, "Step+0xb0 test not found");
        int called = 0;
        for (uint8_t *p = R.rrp; p < R.rrp + 0x400; p++)
            if (*p == 0xE8 && call_tgt(p) == R.heur) called++;
        if (called != 1) GFAIL(G_HEUR, "RRP calls it %d times (want 1)", called);
        R.ok[G_HEUR] = 1;
    }
next_G_HEUR:
    {
        if (sp[SP_GUARD].n != 1) GFAIL(G_GUARD, "dtor shape matches %d times (want 1)", sp[SP_GUARD].n);
        R.guard = sp[SP_GUARD].hit[0];
        if (!near_(R.guard, 0x40, P_GUARD_10MS)) GFAIL(G_GUARD, "10 ms threshold not found");
        R.ok[G_GUARD] = 1;
    }
next_G_GUARD:
    {
        if (sp[SP_HAI_S].n != 1) GFAIL(G_HAI, "serial re-eval site matches %d times", sp[SP_HAI_S].n);
        uint8_t *ss = sp[SP_HAI_S].hit[0];
        R.hai_eval = call_tgt(ss + 11);
        R.hai_sret = ss + 16;
        if (!at(R.hai_eval, P_HAI_EVAL_PRO)) GFAIL(G_HAI, "eval prologue mismatch");
        int np = 0;
        for (int i = 0; i < sp[SP_HAI_P].n && i < APAR_SCAN_MAXHITS; i++)
            if (call_tgt(sp[SP_HAI_P].hit[i] + 4) == R.hai_eval) { np++; R.hai_pret = sp[SP_HAI_P].hit[i] + 9; }
        if (np != 1) GFAIL(G_HAI, "parallel item site: %d of %d shapes call the eval", np, sp[SP_HAI_P].n);
        R.hai_s2 = fstart(ss);
        if (!at(R.hai_s2, P_HAI_S2_PRO)) GFAIL(G_HAI, "stage-2 prologue mismatch");
        if (sp[SP_HAI_B].n != 1 || fstart(sp[SP_HAI_B].hit[0]) != R.hai_s2) GFAIL(G_HAI, "build site not in stage 2");
        R.hai_build = call_tgt(sp[SP_HAI_B].hit[0] + 9);
        R.ok[G_HAI] = 1;
    }
next_G_HAI:
    {
        if (sp[SP_AI_S1].n != 1) GFAIL(G_AI, "stage-1 site matches %d times", sp[SP_AI_S1].n);
        R.ai_exec = fstart(sp[SP_AI_S1].hit[0]);
        if (!at(R.ai_exec, P_AI_EXEC_PRO) || !near_(R.ai_exec, 0x120, P_AI_SETRNG)) GFAIL(G_AI, "Execute shape mismatch");
        if (sp[SP_AI_I].n != 1) GFAIL(G_AI, "item site matches %d times", sp[SP_AI_I].n);
        R.ai_item = call_tgt(sp[SP_AI_I].hit[0] + 11);
        R.ai_iret = sp[SP_AI_I].hit[0] + 16;
        if (!at(R.ai_item, P_AI_ITEM_PRO)) GFAIL(G_AI, "item prologue mismatch");
        int na = 0;
        for (int i = 0; i < sp[SP_AI_A].n && i < APAR_SCAN_MAXHITS; i++) {
            uint8_t *t = call_tgt(sp[SP_AI_A].hit[i] + 8);
            if (at(t, P_AI_APPLY_PRO)) { na++; R.ai_apply = t; }
        }
        if (na != 1) GFAIL(G_AI, "stage-3 apply: %d candidates", na);
        R.ok[G_AI] = 1;
    }
next_G_AI:
    {
        if (sp[SP_TR_A].n != 1) GFAIL(G_TR, "apply site matches %d times", sp[SP_TR_A].n);
        uint8_t *as = sp[SP_TR_A].hit[0];
        R.tr_apply = call_tgt(as + 19);
        if (!at(R.tr_apply, P_TR_APPLY_PRO)) GFAIL(G_TR, "apply prologue mismatch");
        R.tr_fn = fstart(as);
        uint8_t *pc = R.tr_fn ? near_(R.tr_fn, 0x1200, P_TR_PFOR_CALL) : NULL;
        if (!pc || near_(pc + 1, 0x1200 - (size_t)(pc + 1 - R.tr_fn), P_TR_PFOR_CALL)) GFAIL(G_TR, "parallel-for call not found once");
        uint8_t *pf = call_tgt(pc + 14);
        uint8_t *es = near_(pf, 0x200, P_TR_ESITE);
        if (!es || near_(es + 1, 0x200 - (size_t)(es + 1 - pf), P_TR_ESITE)) GFAIL(G_TR, "inline eval site not found once");
        R.tr_eval = call_tgt(es + 4);
        R.tr_eret = es + 9;
        if (!at(R.tr_eval, P_TR_EVAL_PRO)) GFAIL(G_TR, "eval prologue mismatch");
        if (sp[SP_TR_X].n != 1 || (uint8_t *)v3x_rel32_target(sp[SP_TR_X].hit[0] + 18) != R.tr_fn)
            GFAIL(G_TR, "Execute tail-jump to UpdateStateTrades not found");
        R.tr_exec = sp[SP_TR_X].hit[0];
        R.ok[G_TR] = 1;
    }
next_G_TR:
    {
        V3xAnchor A = {.name = "UpdatePopGrowthParallel", .string = S_PGPAR, .pattern = P_PG_PAR_PRO, .xref = V3X_ANCHOR_ALL_SAME};
        if (v3x_anchor_resolve(api, &A)) GFAIL(G_PG, "%s", A.err);
        R.pg_par = (uint8_t *)A.result;
        if (sp[SP_PG_SEQ].n != 1 || call_tgt(sp[SP_PG_SEQ].hit[0] + 13) != R.pg_par) GFAIL(G_PG, "stage sequence not found");
        R.pg_apply = call_tgt(sp[SP_PG_SEQ].hit[0] + 22);
        if (!at(R.pg_apply, P_PG_APPLY_PRO)) GFAIL(G_PG, "apply prologue mismatch");
        R.ok[G_PG] = 1;
    }
next_G_PG:;
    int n = 0;
    for (int g = 0; g < G_N; g++) n += R.ok[g];
    return n;
}

/* ---------------------------------------------------------------- config + state */
static struct {
    int enabled, mode, probe_items, fine_chunks, fine_heur;
    char fine_tasks[512];
} cfg;
static double g_tpus;            /* QPC ticks per microsecond */
static int g_lever_ok;           /* lever usable (anchors) and enabled */

enum { PR_HAI_EVAL_PAR, PR_HAI_EVAL_SER, PR_HAI_S2, PR_HAI_BUILD, PR_AI_S1, PR_AI_ITEM, PR_AI_APPLY, PR_TR_RUN,
       PR_TR_EVAL_INLINE, PR_TR_EVAL_FORK, PR_TR_APPLY, PR_PG_PAR, PR_PG_APPLY, PR_GUARD, PR_N };
static const char *PR_NAME[PR_N] = {
    "HAI eval/country (parallel stage 1)", "HAI re-eval (serial stage 2)", "HAI stage 2 total (serial)",
    "HAI build call (serial)", "UpdateAI stage 1 Execute (serial)", "UpdateAI subsystem item (parallel)",
    "UpdateAI stage 3 apply item (serial)", "UpdateStateTrades run", "trades eval/record inline (1 chunk)",
    "trades eval/record forked", "trades apply/record (serial)", "PopGrowth parallel phase",
    "PopGrowth apply (serial)", "RenderFrameIfNeeded guard (all)" };
static apar_hist g_pr[PR_N];

/* per range-parallel task (RRP) */
#define MAXRT 160
typedef struct apar_rtask {
    const void *type;
    char name[64];
    LONG64 runs, wall, wall_max, n_sum, n_max, threads_sum, chunks_sum, grain1_runs;
    LONG64 heur_used, heur_replica_used, heur_mismatch, heur_seen, exp_sum, lever;
    LONG64 item_runs, item_wall, item_threads, items, item_sum, item_max_sum, ideal_sum, item_max;
} apar_rtask;
static apar_rtask g_rt[MAXRT];
static volatile LONG g_nrt;
static SRWLOCK g_rt_lock = SRWLOCK_INIT;

/* the range in flight (RRP is only called from RunTask, i.e. never concurrently with itself) */
static struct {
    volatile LONG active;
    volatile LONG64 sum, max, n;
    int rt, heur_ret;
} g_run;
static volatile LONG g_depth;

/* RenderFrameIfNeeded guard dtor time per return address */
#define MAXG 48
typedef struct { void *volatile ret; volatile LONG64 n, sum, max, slow; } apar_gsite;
static apar_gsite g_gs[MAXG];

static LONG64 qpc(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

static int read_threads(void)
{
    if (!R.sched_global) return 0;
    uintptr_t s = (uintptr_t)*R.sched_global;
    if (!s) return 1;
    return *(const int32_t *)(s + SCHED_WORKERS) + 1;
}

static void name_of(const void *task, char *out, size_t cap, const void **type_out)
{
    const uint8_t *type = *(const uint8_t *const *)((const uint8_t *)task + TASK_TYPE);
    *type_out = type;
    out[0] = 0;
    if (!type) return;
    uint64_t size = *(const uint64_t *)(type + TYPE_NAME + 0x10), capy = *(const uint64_t *)(type + TYPE_NAME + 0x18);
    const char *s = capy > 15 ? *(const char *const *)(type + TYPE_NAME) : (const char *)(type + TYPE_NAME);
    if (!s || size >= cap) { snprintf(out, cap, "?"); return; }
    memcpy(out, s, (size_t)size);
    out[size] = 0;
}

static int rt_slot(const void *task)
{
    const void *type = *(const void *const *)((const uint8_t *)task + TASK_TYPE);
    LONG n = g_nrt;
    for (int i = 0; i < n; i++) if (g_rt[i].type == type) return i;
    if (n >= MAXRT) return -1;
    AcquireSRWLockExclusive(&g_rt_lock);
    int i = g_nrt;
    if (i < MAXRT) {
        memset(&g_rt[i], 0, sizeof g_rt[i]);
        name_of(task, g_rt[i].name, sizeof g_rt[i].name, &g_rt[i].type);
        InterlockedExchange(&g_nrt, i + 1);
    } else i = -1;
    ReleaseSRWLockExclusive(&g_rt_lock);
    return i;
}

/* ---------------------------------------------------------------- hooks */
enum { H_RRP, H_HEUR, H_GUARD, H_HAI_EVAL, H_HAI_S2, H_HAI_BUILD, H_AI_EXEC, H_AI_ITEM, H_AI_APPLY, H_TR_EXEC,
       H_TR_EVAL, H_TR_APPLY, H_PG_PAR, H_PG_APPLY, H_N };
static const char *H_NAME[H_N] = { "RRP", "heuristic", "guard", "hai_eval", "hai_stage2", "hai_build", "ai_exec",
                                   "ai_item", "ai_apply", "trades_exec", "trades_eval", "trades_apply", "pg_par", "pg_apply" };
static void *g_hooked[H_N];

static void t_pre(V3X_HookCall *c) { c->scratch[0] = (uint64_t)qpc(); }

static void run_item(LONG64 dt)
{
    if (!g_run.active) return;
    InterlockedAdd64(&g_run.sum, dt);
    InterlockedIncrement64(&g_run.n);
    apar_atomic_max(&g_run.max, dt);
}

static void guard_add(void *ret, LONG64 dt)
{
    unsigned h = (unsigned)(((uintptr_t)ret >> 2) * 2654435761u) % MAXG;
    for (int i = 0; i < MAXG; i++, h = (h + 1) % MAXG) {
        apar_gsite *s = &g_gs[h];
        void *cur = s->ret;
        if (!cur) {
            cur = InterlockedCompareExchangePointer((void *volatile *)&s->ret, ret, NULL);
            if (!cur) cur = ret;
        }
        if (cur != ret) continue;
        InterlockedIncrement64(&s->n);
        InterlockedAdd64(&s->sum, dt);
        apar_atomic_max(&s->max, dt);
        if (dt > (LONG64)(1000.0 * g_tpus)) InterlockedIncrement64(&s->slow);
        return;
    }
}

static void t_post(V3X_HookCall *c)
{
    LONG64 dt = qpc() - (LONG64)c->scratch[0];
    int h = (int)(intptr_t)c->user;
    void *ret = c->return_address;
    switch (h) {
    case H_GUARD: apar_hist_add(&g_pr[PR_GUARD], dt, g_tpus); guard_add(ret, dt); break;
    case H_HAI_EVAL:
        if (ret == R.hai_pret) { apar_hist_add(&g_pr[PR_HAI_EVAL_PAR], dt, g_tpus); run_item(dt); }
        else if (ret == R.hai_sret) apar_hist_add(&g_pr[PR_HAI_EVAL_SER], dt, g_tpus);
        break;
    case H_HAI_S2: apar_hist_add(&g_pr[PR_HAI_S2], dt, g_tpus); break;
    case H_HAI_BUILD: apar_hist_add(&g_pr[PR_HAI_BUILD], dt, g_tpus); break;
    case H_AI_EXEC: apar_hist_add(&g_pr[PR_AI_S1], dt, g_tpus); break;
    case H_AI_ITEM: if (ret == R.ai_iret) { apar_hist_add(&g_pr[PR_AI_ITEM], dt, g_tpus); run_item(dt); } break;
    case H_AI_APPLY: apar_hist_add(&g_pr[PR_AI_APPLY], dt, g_tpus); break;
    case H_TR_EXEC: apar_hist_add(&g_pr[PR_TR_RUN], dt, g_tpus); break;
    case H_TR_EVAL: apar_hist_add(&g_pr[ret == R.tr_eret ? PR_TR_EVAL_INLINE : PR_TR_EVAL_FORK], dt, g_tpus); break;
    case H_TR_APPLY: apar_hist_add(&g_pr[PR_TR_APPLY], dt, g_tpus); break;
    case H_PG_PAR: apar_hist_add(&g_pr[PR_PG_PAR], dt, g_tpus); break;
    case H_PG_APPLY: apar_hist_add(&g_pr[PR_PG_APPLY], dt, g_tpus); break;
    }
}

/* heuristic post: al = 1 if it ran the range itself */
static void heur_post(V3X_HookCall *c)
{
    if (g_depth == 1) g_run.heur_ret = (int)(c->ret & 0xff);
}

/* scratch[0] = t0, scratch[1] = lever save: bit63 applied | mode u32 (bits 0..31) | mode_set (32..39) | cost_set (40..47) */
static void rrp_pre(V3X_HookCall *c)
{
    c->scratch[0] = 0; c->scratch[1] = 0;
    if (InterlockedIncrement(&g_depth) != 1) return;   /* nested range: not ours to account */
    uint8_t *step = (uint8_t *)(uintptr_t)c->args[1];
    const void *task = (const void *)(uintptr_t)c->args[0];
    int rt = task && step ? rt_slot(task) : -1;
    g_run.rt = rt; g_run.heur_ret = -1;
    g_run.sum = 0; g_run.max = 0; g_run.n = 0;
    if (rt >= 0 && g_lever_ok &&
        apar_lever_should(1, cfg.fine_tasks, g_rt[rt].name, *(uint32_t *)(step + STEP_FLAGS), step[STEP_MODE_SET])) {
        uint64_t save = (uint64_t)*(uint32_t *)(step + STEP_MODE) | ((uint64_t)step[STEP_MODE_SET] << 32) |
                        ((uint64_t)step[STEP_COST_SET] << 40) | (1ull << 63);
        c->scratch[1] = save;
        *(uint32_t *)(step + STEP_MODE) = 2;
        step[STEP_MODE_SET] = 1;
        if (!cfg.fine_heur) step[STEP_COST_SET] = 0;
    }
    InterlockedExchange(&g_run.active, 1);
    c->scratch[0] = (uint64_t)qpc();
}

static void rrp_post(V3X_HookCall *c)
{
    if (InterlockedDecrement(&g_depth) != 0 || !c->scratch[0]) return;
    LONG64 wall = qpc() - (LONG64)c->scratch[0];
    InterlockedExchange(&g_run.active, 0);
    uint8_t *step = (uint8_t *)(uintptr_t)c->args[1];
    uint64_t save = c->scratch[1];
    int rt = g_run.rt;
    if (save) {   /* restore exactly what the task had set */
        *(uint32_t *)(step + STEP_MODE) = (uint32_t)save;
        step[STEP_MODE_SET] = (uint8_t)(save >> 32);
        step[STEP_COST_SET] = (uint8_t)(save >> 40);
    }
    if (rt < 0) return;
    int b = *(int32_t *)(step + STEP_BEGIN), e = *(int32_t *)(step + STEP_END), n = e - b;
    if (n < 0) n = 0;
    int T = read_threads();
    int mode_set = save ? 1 : step[STEP_MODE_SET], mode = save ? 2 : (int)*(uint32_t *)(step + STEP_MODE);
    int grain = apar_engine_grain(n, T, mode_set, mode), chunks = apar_engine_chunks(n, T, grain);
    int cost_set = save && !cfg.fine_heur ? 0 : step[STEP_COST_SET];
    apar_heur hr = {0, 0, 0};
    const int32_t *cb = *(const int32_t *const *)(step + STEP_COST_B), *ce = *(const int32_t *const *)(step + STEP_COST_E);
    if (cost_set && cb && ce && ce - cb >= e && b >= 0) hr = apar_engine_heuristic(cb + b, n, T - 1, 1);
    apar_rtask *r = &g_rt[rt];
    AcquireSRWLockExclusive(&g_rt_lock);
    r->runs++; r->wall += wall; if (wall > r->wall_max) r->wall_max = wall;
    r->n_sum += n; if (n > r->n_max) r->n_max = n;
    r->threads_sum += T; r->chunks_sum += chunks; r->grain1_runs += grain == 1;
    r->lever += save != 0;
    if (g_run.heur_ret >= 0) {
        r->heur_seen++;
        r->heur_used += g_run.heur_ret == 1;
        r->heur_mismatch += (g_run.heur_ret == 1) != (hr.used != 0);
    }
    r->heur_replica_used += hr.used != 0; r->exp_sum += hr.n_exp;
    if (g_run.n) {
        double sum = (double)g_run.sum, mx = (double)g_run.max;
        r->item_runs++; r->item_wall += wall; r->item_threads += T; r->items += g_run.n; r->item_sum += g_run.sum; r->item_max_sum += g_run.max;
        if (g_run.max > r->item_max) r->item_max = g_run.max;
        r->ideal_sum += (LONG64)apar_ideal(sum, mx, T);
    }
    ReleaseSRWLockExclusive(&g_rt_lock);
}

/* ---------------------------------------------------------------- report */
typedef struct { char *p; size_t n, cap; } sbuf;
static V3X_PRINTF(2, 3) void sb(sbuf *b, const char *fmt, ...)
{
    if (b->n + 1 >= b->cap) return;
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (k > 0) b->n += (size_t)k < b->cap - b->n ? (size_t)k : b->cap - b->n - 1;
}

static double ms(LONG64 t) { return g_tpus > 0 ? (double)t / (g_tpus * 1000.0) : 0.0; }

static void hist_line(sbuf *b, int pr)
{
    const apar_hist *h = &g_pr[pr];
    if (!h->n) { sb(b, "  %-40s -\r\n", PR_NAME[pr]); return; }
    sb(b, "  %-40s n=%lld total %.1f ms, avg %.1f us, p50<=%.0f p90<=%.0f p99<=%.0f us, max %.2f ms\r\n", PR_NAME[pr],
       (long long)h->n, ms(h->sum), ms(h->sum) * 1000.0 / (double)h->n, apar_hist_pct_us(h, 0.5), apar_hist_pct_us(h, 0.9),
       apar_hist_pct_us(h, 0.99), ms(h->max));
}

static const apar_rtask *rt_find(const char *name)
{
    for (int i = 0; i < g_nrt; i++) if (!strcmp(g_rt[i].name, name)) return &g_rt[i];
    return NULL;
}

static const char *site_owner(void *ret)
{
    uint8_t *f = fstart(ret);
    if (!f) return "?";
    if (f == R.tr_fn) return "UpdateStateTrades iteration";
    if (f == R.hai_s2) return "HAI stage 2 (between builds)";
    if (f == R.ai_apply) return "UpdateAI stage 3 item";
    if (f == R.pg_apply) return "PopGrowth apply";
    return "";
}

static size_t apar_format_report(char *out, size_t cap, const char *why)
{
    sbuf b = {out, 0, cap};
    out[0] = 0;
    sb(&b, "ai_par report (%s), mode=%s, fine_chunks=%s, threads per range now T=%d\r\n", why,
       cfg.mode == APAR_OFF ? "off" : "measure", g_lever_ok ? (cfg.fine_heur ? "on (heuristic kept)" : "on (heuristic off)") : "off",
       read_threads());
    sb(&b, "groups:");
    for (int g = 0; g < G_N; g++) sb(&b, " %s=%s", G_NAME[g], R.ok[g] ? "ok" : "OFF");
    sb(&b, "\r\n\r\n");
    /* candidate split */
    const apar_rtask *hai = rt_find("HandleAutonomousInvestment"), *ai = rt_find("UpdateAI");
    double hai_par = hai ? ms(hai->wall) : 0, hai_ser = ms(g_pr[PR_HAI_S2].sum);
    double ai_par = ai ? ms(ai->wall) : 0, ai_ser = ms(g_pr[PR_AI_S1].sum) + ms(g_pr[PR_AI_APPLY].sum);
    double tr_run = ms(g_pr[PR_TR_RUN].sum), tr_app = ms(g_pr[PR_TR_APPLY].sum), tr_inl = ms(g_pr[PR_TR_EVAL_INLINE].sum);
    sb(&b, "serial vs parallel (ms):\r\n");
    sb(&b, "  HandleAutonomousInvestment  parallel range %.0f | serial stage 2 %.0f (re-eval %.0f, builds %.0f)\r\n", hai_par,
       hai_ser, ms(g_pr[PR_HAI_EVAL_SER].sum), ms(g_pr[PR_HAI_BUILD].sum));
    sb(&b, "  UpdateAI                    parallel range %.0f | serial stage 1 %.0f + stage 3 %.0f\r\n", ai_par,
       ms(g_pr[PR_AI_S1].sum), ms(g_pr[PR_AI_APPLY].sum));
    (void)ai_ser;
    sb(&b, "  UpdateStateTrades           runs %lld, total %.0f | serial apply %.0f, inline (1-chunk) eval %.0f, forked eval work %.0f, rest %.0f\r\n",
       (long long)g_pr[PR_TR_RUN].n, tr_run, tr_app, tr_inl, ms(g_pr[PR_TR_EVAL_FORK].sum),
       tr_run - tr_app - tr_inl > 0 ? tr_run - tr_app - tr_inl : 0.0);
    sb(&b, "  UpdatePopGrowth             parallel phase %.0f | serial apply %.0f\r\n", ms(g_pr[PR_PG_PAR].sum),
       ms(g_pr[PR_PG_APPLY].sum));
    sb(&b, "\r\nper-item cost:\r\n");
    for (int i = 0; i < PR_N; i++) hist_line(&b, i);
    AcquireSRWLockShared(&g_rt_lock);
    sb(&b, "\r\nrange-parallel stages (RunRangeExecutionParallel), by wall time:\r\n");
    sb(&b, "  %-44s %7s %10s %8s %6s %5s %6s %6s %6s %5s\r\n", "task", "runs", "wall ms", "max ms", "avg n", "T", "chunks",
       "heur%", "exp/run", "lever");
    int idx[MAXRT], n = g_nrt;
    for (int i = 0; i < n; i++) idx[i] = i;
    for (int i = 1; i < n; i++) { int v = idx[i], j = i; while (j > 0 && g_rt[idx[j - 1]].wall < g_rt[v].wall) { idx[j] = idx[j - 1]; j--; } idx[j] = v; }
    for (int k = 0; k < n && k < 40; k++) {
        const apar_rtask *r = &g_rt[idx[k]];
        if (!r->runs) continue;
        sb(&b, "  %-44s %7lld %10.1f %8.1f %6.0f %5.1f %6.1f %6.0f %6.1f %5lld%s\r\n", r->name, (long long)r->runs, ms(r->wall),
           ms(r->wall_max), (double)r->n_sum / r->runs, (double)r->threads_sum / r->runs, (double)r->chunks_sum / r->runs,
           r->heur_seen ? 100.0 * r->heur_used / r->heur_seen : 0.0, (double)r->exp_sum / r->runs, (long long)r->lever,
           r->heur_mismatch ? " (heuristic replica mismatch!)" : "");
    }
    sb(&b, "\r\nrange efficiency (item-timed ranges): items, item work, longest item per run, lower bound max(longest, work/T):\r\n");
    for (int k = 0; k < n; k++) {
        const apar_rtask *r = &g_rt[idx[k]];
        if (!r->item_runs) continue;
        double wall = ms(r->item_wall), ideal = ms(r->ideal_sum), work = ms(r->item_sum);
        double T = (double)r->item_threads / r->item_runs;
        sb(&b, "  %-28s runs %lld, wall %.0f ms, item work %.0f ms (efficiency %.0f%% of %.1f threads), longest item %.2f ms "
               "(sum of per-run longest %.0f ms), lower bound %.0f ms -> finer chunking can recover at most %.0f ms (%.0f%%)\r\n",
           r->name, (long long)r->item_runs, wall, work, wall > 0 ? 100.0 * work / (wall * T) : 0.0, T, ms(r->item_max),
           ms(r->item_max_sum), ideal, wall - ideal > 0 ? wall - ideal : 0.0, wall > 0 ? 100.0 * (wall - ideal) / wall : 0.0);
    }
    ReleaseSRWLockShared(&g_rt_lock);
    sb(&b, "\r\nRenderFrameIfNeeded guard time by call site (calls > 1 ms counted as renders):\r\n");
    apar_gsite gs[MAXG]; int ng = 0;
    for (int i = 0; i < MAXG; i++) if (g_gs[i].ret) gs[ng++] = g_gs[i];
    for (int i = 1; i < ng; i++) { apar_gsite v = gs[i]; int j = i; while (j > 0 && gs[j - 1].sum < v.sum) { gs[j] = gs[j - 1]; j--; } gs[j] = v; }
    for (int i = 0; i < ng && i < 12; i++)
        sb(&b, "  ret exe+0x%08llx (fn exe+0x%08llx %s): calls %lld, %.1f ms, renders %lld, max %.1f ms\r\n",
           (unsigned long long)((uintptr_t)gs[i].ret - g_base), (unsigned long long)(fstart(gs[i].ret) ? (uintptr_t)fstart(gs[i].ret) - g_base : 0),
           site_owner(gs[i].ret), (long long)gs[i].n, ms(gs[i].sum), (long long)gs[i].slow, ms(gs[i].max));
    return b.n;
}

static void write_report(const char *why, int to_console)
{
    static char buf[48 * 1024];
    size_t n = apar_format_report(buf, sizeof buf, why);
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%sai_par_report.txt", api->v3x_dir());
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, buf, (DWORD)n, &w, NULL); CloseHandle(h); }
    if (to_console && api->print) {
        /* api->print truncates at 8 KB: print line by line */
        char *p = buf;
        while (*p) {
            char *e = strstr(p, "\r\n");
            if (e) *e = 0;
            api->print("%s", p);
            if (!e) break;
            *e = '\r';
            p = e + 2;
        }
    }
    api->log(TAG, "report (%s) written to ai_par_report.txt", why);
}

static void reset_stats(void)
{
    AcquireSRWLockExclusive(&g_rt_lock);
    for (int i = 0; i < g_nrt; i++) {
        const void *t = g_rt[i].type; char nm[64]; memcpy(nm, g_rt[i].name, sizeof nm);
        memset(&g_rt[i], 0, sizeof g_rt[i]);
        g_rt[i].type = t; memcpy(g_rt[i].name, nm, sizeof nm);
    }
    ReleaseSRWLockExclusive(&g_rt_lock);
    for (int i = 0; i < PR_N; i++) memset((void *)&g_pr[i], 0, sizeof g_pr[i]);
    for (int i = 0; i < MAXG; i++) { g_gs[i].n = g_gs[i].sum = g_gs[i].max = g_gs[i].slow = 0; }
}

static void cmd_aipar(const char *args, void *user)
{
    (void)user;
    if (args && !_strnicmp(args, "reset", 5)) { reset_stats(); api->print("ai_par: statistics reset"); return; }
    write_report("console", 1);
}

static void on_transition(const char *name, void *data, void *user)
{
    (void)name; (void)user;
    const V3X_LoadTransition *t = (const V3X_LoadTransition *)data;
    if (t && t->from && t->to && !strcmp(t->from, "Game") && !strcmp(t->to, "Empty")) write_report("Game->Empty", 0);
}

static void on_exit_ev(const char *name, void *data, void *user)
{
    (void)name; (void)data; (void)user;
    write_report("process exit", 0);
}

/* ---------------------------------------------------------------- install */
static void load_cfg(void)
{
    V3xIni ini;
    v3x_ini_load_plugin(&ini, api, "ai_par");
    char mode[32];
    cfg.enabled = v3x_ini_int(&ini, "ai_par", "enabled", 1);
    v3x_ini_str(&ini, "ai_par", "mode", "measure", mode, sizeof mode);
    cfg.mode = apar_mode_parse(mode);
    if (cfg.mode < 0) { api->log(TAG, "mode=%s unknown, using measure", mode); cfg.mode = APAR_MEASURE; }
    cfg.probe_items = v3x_ini_int(&ini, "ai_par", "probe_items", 1);
    cfg.fine_chunks = v3x_ini_int(&ini, "ai_par", "fine_chunks", 0);
    cfg.fine_heur = v3x_ini_int(&ini, "ai_par", "fine_chunks_heuristic", 1);
    v3x_ini_str(&ini, "ai_par", "fine_chunks_tasks", "HandleAutonomousInvestment,UpdateAI", cfg.fine_tasks, sizeof cfg.fine_tasks);
    v3x_ini_free(&ini);
}

static int hook1(int h, void *target, v3x_hook_fn pre, v3x_hook_fn post)
{
    if (!target) return -1;
    int rc = api->hook_safe(target, pre, post, (void *)(intptr_t)h);
    if (rc) { api->log(TAG, "hook_safe %s at exe+0x%llx failed (%d)", H_NAME[h], (unsigned long long)((uintptr_t)target - g_base), rc); return rc; }
    g_hooked[h] = target;
    return 0;
}

/* group = all its hooks or none */
static int hook_group(const int *hs, void *const *targets, int n, v3x_hook_fn *pres, v3x_hook_fn *posts)
{
    for (int i = 0; i < n; i++) {
        if (hook1(hs[i], targets[i], pres[i], posts[i])) {
            for (int k = 0; k < i; k++) { api->unhook(targets[k]); g_hooked[hs[k]] = NULL; }
            return -1;
        }
    }
    return 0;
}

static int install(void)
{
    int nh = 0;
    { int hs[] = {H_RRP}; void *t[] = {R.rrp}; v3x_hook_fn pr[] = {rrp_pre}, po[] = {rrp_post};
      if (hook_group(hs, t, 1, pr, po)) { api->log(TAG, "range-runner hook failed: inactive"); return -1; } nh++; }
    if (R.ok[G_HEUR]) { int hs[] = {H_HEUR}; void *t[] = {R.heur}; v3x_hook_fn pr[] = {NULL}, po[] = {heur_post};
      if (hook_group(hs, t, 1, pr, po)) R.ok[G_HEUR] = 0; else nh++; }
    if (R.ok[G_GUARD]) { int hs[] = {H_GUARD}; void *t[] = {R.guard}; v3x_hook_fn pr[] = {t_pre}, po[] = {t_post};
      if (hook_group(hs, t, 1, pr, po)) R.ok[G_GUARD] = 0; else nh++; }
    if (R.ok[G_HAI]) {
        int hs[] = {H_HAI_S2, H_HAI_BUILD, H_HAI_EVAL}; void *t[] = {R.hai_s2, R.hai_build, R.hai_eval};
        v3x_hook_fn pr[] = {t_pre, t_pre, t_pre}, po[] = {t_post, t_post, t_post};
        int k = cfg.probe_items ? 3 : 1;
        if (hook_group(hs, t, k, pr, po)) R.ok[G_HAI] = 0; else nh += k;
    }
    if (R.ok[G_AI]) {
        int hs[] = {H_AI_EXEC, H_AI_APPLY, H_AI_ITEM}; void *t[] = {R.ai_exec, R.ai_apply, R.ai_item};
        v3x_hook_fn pr[] = {t_pre, t_pre, t_pre}, po[] = {t_post, t_post, t_post};
        int k = cfg.probe_items ? 3 : 2;
        if (hook_group(hs, t, k, pr, po)) R.ok[G_AI] = 0; else nh += k;
    }
    if (R.ok[G_TR]) {
        int hs[] = {H_TR_EXEC, H_TR_APPLY, H_TR_EVAL}; void *t[] = {R.tr_exec, R.tr_apply, R.tr_eval};
        v3x_hook_fn pr[] = {t_pre, t_pre, t_pre}, po[] = {t_post, t_post, t_post};
        int k = cfg.probe_items ? 3 : 1;
        if (hook_group(hs, t, k, pr, po)) R.ok[G_TR] = 0; else nh += k;
    }
    if (R.ok[G_PG]) {
        int hs[] = {H_PG_PAR, H_PG_APPLY}; void *t[] = {R.pg_par, R.pg_apply};
        v3x_hook_fn pr[] = {t_pre, t_pre}, po[] = {t_post, t_post};
        if (hook_group(hs, t, 2, pr, po)) R.ok[G_PG] = 0; else nh += 2;
    }
    return nh;
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    const char *off = getenv("V3X_NO_AIPAR");
    if (off && off[0] == '1') { api->log(TAG, "disabled by V3X_NO_AIPAR=1"); return 0; }
    if (!V3X_HAS(api, hook_safe)) { api->log(TAG, "needs API v4 hook_safe; inactive"); return 1; }
    load_cfg();
    if (!cfg.enabled || cfg.mode == APAR_OFF) { api->log(TAG, "inactive (enabled=%d mode=%s)", cfg.enabled, cfg.mode == APAR_OFF ? "off" : "measure"); return 0; }
    if (cfg.mode == APAR_ON) {
        api->log(TAG, "mode=on REFUSED: no serial loop of HandleAutonomousInvestment / UpdateAI / UpdateStateTrades / "
                      "UpdatePopGrowth / RecalculateModifierNodes is proven safe to run in parallel (docs/re/ai_par.md sec. 3); running as measure");
        cfg.mode = APAR_MEASURE;
    }
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_tpus = (double)f.QuadPart / 1e6;
    int ng = apar_resolve(api);
    for (int g = 0; g < G_N; g++)
        if (!R.ok[g]) api->log(TAG, "group %s OFF: %s", G_NAME[g], R.err[g][0] ? R.err[g] : "not resolved");
    if (!R.ok[G_RRP]) { api->log(TAG, "range-runner anchor failed: inactive (fail closed)"); return 2; }
    g_lever_ok = cfg.fine_chunks && R.ok[G_HEUR];
    if (cfg.fine_chunks && !g_lever_ok) api->log(TAG, "fine_chunks requested but the heuristic anchor failed: lever OFF");
    int nh = install();
    if (nh < 0) { g_lever_ok = 0; return 3; }
    api->on_event("load_transition", on_transition, NULL);
    api->on_event("process_exit", on_exit_ev, NULL);
    api->register_command("aipar", "ai_par: serial vs parallel report of the AI/economy tasks; 'aipar reset'", cmd_aipar, NULL);
    api->log(TAG, "mode=measure: %d/%d groups, %d hooks (RRP exe+0x%llx, sched global exe+0x%llx); fine_chunks=%s%s%s", ng, G_N, nh,
             (unsigned long long)((uintptr_t)R.rrp - g_base), (unsigned long long)((uintptr_t)R.sched_global - g_base),
             g_lever_ok ? "ON for " : "off", g_lever_ok ? cfg.fine_tasks : "", g_lever_ok && !cfg.fine_heur ? " (heuristic off)" : "");
    return 0;
}
