/* Offline test of plugins/ai_par without starting the game. Includes the plugin source. Checks:
 *   - logic (ai_par_core.h): histogram buckets / percentiles, the engine chunking replicas (grain per batch
 *     mode, chunk count, cost heuristic incl. the float threshold and its preconditions), lower bound,
 *     list / mode parsing, the fine_chunks decision, the one-pass multi-pattern scanner vs v3x_scan_count;
 *   - the RRP hook on a fake task / Step / scheduler: lever writes mode 2 (and optionally clears the
 *     heuristic flag) only for listed tasks with no explicit mode, restores every byte afterwards; per-run
 *     item accounting, lower bound, heuristic hook vs replica; return-address classification; the report;
 *   - with argv[1] = victoria3.exe: every anchor group resolves against the real exe at the RVAs of
 *     docs/re/ai_par.md, v3x_plugin_load installs the expected hooks, mode=on is refused (runs as measure),
 *     mode=off installs nothing, a missing range-runner string fails closed (no hooks).
 * Build+run (from the repo root; T = scratch dir):
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -Isdk -o $T/aipar.exe test/ai_par_test.c
 *   $T/aipar.exe ../binaries/victoria3.exe $T/
 * Exit code 0 = ALL OK. */
#include "v3x_testkit.h"
#include "../plugins/ai_par/ai_par.c"

#define CHECK V3X_TEST_CHECK

/* ------------------------------------------------------------------ logic */
static void test_logic(void)
{
    CHECK(apar_bucket_us(0.2) == 0 && apar_bucket_us(1.0) == 1 && apar_bucket_us(1.99) == 1 && apar_bucket_us(2.0) == 2 &&
          apar_bucket_us(1000.0) == 10 && apar_bucket_us(1e12) == APAR_HB - 1, "histogram buckets (log2 us)");
    apar_hist h; memset(&h, 0, sizeof h);
    for (int i = 0; i < 90; i++) apar_hist_add(&h, 5, 1.0);      /* 5 us */
    for (int i = 0; i < 10; i++) apar_hist_add(&h, 3000, 1.0);   /* 3 ms */
    CHECK(h.n == 100 && h.sum == 90 * 5 + 10 * 3000 && h.max == 3000, "histogram n/sum/max");
    CHECK(apar_hist_pct_us(&h, 0.5) == 8.0 && apar_hist_pct_us(&h, 0.9) == 8.0 && apar_hist_pct_us(&h, 0.99) == 4096.0,
          "percentiles = upper bucket edge (p50 %.0f p99 %.0f)", apar_hist_pct_us(&h, 0.5), apar_hist_pct_us(&h, 0.99));

    /* RRP grain: mode0 n/T, mode1 n/T/3 (default when Step+0x9c is empty), mode2 1, mode3 whole */
    CHECK(apar_engine_grain(267, 8, 0, 0) == 11 && apar_engine_chunks(267, 8, 11) == 8, "HAI-like range, default mode 1: grain 11, 8 chunks");
    CHECK(apar_engine_grain(267, 8, 1, 0) == 33 && apar_engine_chunks(267, 8, 33) == 8, "mode 0: grain n/T = 33");
    CHECK(apar_engine_grain(267, 8, 1, 2) == 1 && apar_engine_chunks(267, 8, 1) == 8, "mode 2: grain 1, chunks capped at T");
    CHECK(apar_engine_grain(267, 8, 1, 3) == -1 && apar_engine_chunks(267, 8, -1) == 1, "mode 3: one inline call");
    CHECK(apar_engine_grain(4, 8, 0, 0) == 1 && apar_engine_chunks(4, 8, 1) == 4, "small range: grain 1, n chunks");
    CHECK(apar_engine_chunks(1, 8, 1) == 1 && apar_engine_chunks(0, 8, 1) == 0, "1 item -> inline, 0 items -> nothing");
    CHECK(apar_engine_grain(1068, 50, 0, 0) == 7 && apar_engine_grain(267, 50, 0, 0) == 1,
          "with 49 workers the engine itself runs a 267-item range with grain 1");

    int32_t flat[40], one[40];
    for (int i = 0; i < 40; i++) { flat[i] = 10; one[i] = 10 + (i % 3); }
    one[17] = 500;
    apar_heur r = apar_engine_heuristic(one, 40, 7, 1);
    /* mean 23.4, sigma ~76.4 -> t = 176: only item 17 is expensive */
    CHECK(r.used && r.n_exp == 1 && r.threshold > 100 && r.threshold < 500, "heuristic: one outlier is Expensive (t=%d, n_exp=%d)", r.threshold, r.n_exp);
    CHECK(!apar_engine_heuristic(flat, 40, 7, 1).used, "heuristic declines flat costs (max == min)");
    CHECK(!apar_engine_heuristic(one, 14, 7, 1).used, "heuristic needs n > 2 * workers");
    CHECK(!apar_engine_heuristic(one, 40, 7, 0).used, "heuristic needs the Step+0xb0 optional engaged");
    int32_t two[40];
    for (int i = 0; i < 40; i++) two[i] = i < 20 ? 0 : 100;   /* mean 50, sigma 50 -> t = 150 >= max 100 */
    apar_heur r2 = apar_engine_heuristic(two, 40, 7, 1);
    CHECK(!r2.used && r2.threshold == 150, "heuristic declines when mean+2 sigma >= max (t=%d)", r2.threshold);

    CHECK(apar_ideal(800, 10, 8) == 100 && apar_ideal(800, 300, 8) == 300 && apar_ideal(5, 1, 0) == 5, "lower bound max(longest, sum/T)");
    CHECK(apar_list_has("HandleAutonomousInvestment,UpdateAI", "UpdateAI") && apar_list_has(" A , B ", "B") &&
          !apar_list_has("UpdateAI2", "UpdateAI") && !apar_list_has("UpdateAI", "Update") && !apar_list_has(NULL, "x"),
          "task list matching is exact");
    CHECK(apar_mode_parse("measure") == APAR_MEASURE && apar_mode_parse("ON") == APAR_ON && apar_mode_parse("off") == APAR_OFF &&
          apar_mode_parse("") == APAR_MEASURE && apar_mode_parse("fast") == -1, "mode parsing");
    CHECK(apar_lever_should(1, "UpdateAI", "UpdateAI", 3, 0) && !apar_lever_should(0, "UpdateAI", "UpdateAI", 3, 0) &&
          !apar_lever_should(1, "UpdateAI", "UpdateAI", 2, 0) && !apar_lever_should(1, "UpdateAI", "UpdateAI", 3, 1) &&
          !apar_lever_should(1, "UpdateAI", "UpdatePopGrowth", 3, 0),
          "lever: only enabled + listed + parallel stage + no explicit batch mode");

    /* multi scan == single scans */
    static uint8_t buf[4096];
    for (int i = 0; i < (int)sizeof buf; i++) buf[i] = (uint8_t)(i * 7 + 3);
    memcpy(buf + 100, "\x48\x8B\x05\x11\x22\x33\x44\x90", 8);
    memcpy(buf + 2000, "\x48\x8B\x05\x55\x66\x77\x88\x90", 8);
    memcpy(buf + 3000, "\xE8\x01\x02\x03\x04\xCC", 6);
    apar_scanpat ps[3] = {{"48 8B 05 ?? ?? ?? ?? 90"}, {"E8 ?? ?? ?? ?? CC"}, {"?? 8B"}};
    apar_multi_scan(buf, sizeof buf, ps, 3);
    V3xSection sec = {buf, sizeof buf};
    CHECK(ps[0].n == 2 && ps[0].hit[0] == buf + 100 && ps[0].hit[1] == buf + 2000 &&
          ps[0].n == v3x_scan_count(&sec, ps[0].pattern, NULL), "multi scan finds every match like v3x_scan_count");
    CHECK(ps[1].n == v3x_scan_count(&sec, ps[1].pattern, NULL) && ps[1].n >= 1, "multi scan, second pattern (%d)", ps[1].n);
    CHECK(!ps[2].ok && ps[2].n == 0, "patterns starting with a wildcard are rejected (would be slow and ambiguous)");
}

/* ------------------------------------------------------------------ RRP hook on fake engine objects */
static uint8_t f_sched[0x700];
static void *f_sched_ptr = f_sched;
static uint8_t f_types[8][0x40], f_task[0x20];
static char f_names[8][64];
static uint8_t f_step[0xc0];
static int32_t f_cost[64];

/* one type object per task name (the engine has one CJominiTickTaskType per task) */
static void fake_task(const char *name)
{
    int k = 0;
    while (k < 7 && f_names[k][0] && strcmp(f_names[k], name)) k++;
    uint8_t *f_type = f_types[k];
    char *f_name = f_names[k];
    snprintf(f_name, 64, "%s", name);
    memset(f_type, 0, sizeof f_types[k]);
    size_t n = strlen(f_name);
    if (n > 15) { *(char **)(f_type + TYPE_NAME) = f_name; *(uint64_t *)(f_type + TYPE_NAME + 0x18) = 31; }
    else { memcpy(f_type + TYPE_NAME, f_name, n + 1); *(uint64_t *)(f_type + TYPE_NAME + 0x18) = 15; }
    *(uint64_t *)(f_type + TYPE_NAME + 0x10) = n;
    *(uint8_t **)(f_task + TASK_TYPE) = f_type;
}

static void fake_step(int n, uint32_t flags, int mode_set, uint32_t mode, int cost_set)
{
    memset(f_step, 0, sizeof f_step);
    *(uint32_t *)(f_step + STEP_FLAGS) = flags;
    *(int32_t *)(f_step + STEP_BEGIN) = 0; *(int32_t *)(f_step + STEP_END) = n;
    *(uint32_t *)(f_step + STEP_MODE) = mode; f_step[STEP_MODE_SET] = (uint8_t)mode_set;
    *(int32_t **)(f_step + STEP_COST_B) = f_cost; *(int32_t **)(f_step + STEP_COST_E) = f_cost + n;
    f_step[STEP_COST_SET] = (uint8_t)cost_set;
}

static uint8_t seen_mode_set, seen_cost_set; static uint32_t seen_mode;
static void run_range(int heur_ret, int nitems, LONG64 item_ticks, LONG64 longest)
{
    V3X_HookCall c; memset(&c, 0, sizeof c);
    c.args[0] = (uint64_t)(uintptr_t)f_task; c.args[1] = (uint64_t)(uintptr_t)f_step;
    rrp_pre(&c);
    seen_mode = *(uint32_t *)(f_step + STEP_MODE); seen_mode_set = f_step[STEP_MODE_SET]; seen_cost_set = f_step[STEP_COST_SET];
    if (heur_ret >= 0) { V3X_HookCall hc; memset(&hc, 0, sizeof hc); hc.ret = (uint64_t)heur_ret; heur_post(&hc); }
    for (int i = 0; i < nitems; i++) {   /* items arrive through the HAI eval hook from the parallel site */
        V3X_HookCall ic; memset(&ic, 0, sizeof ic);
        ic.user = (void *)(intptr_t)H_HAI_EVAL; ic.return_address = R.hai_pret;
        ic.scratch[0] = (uint64_t)(qpc() - (i == 0 ? longest : item_ticks));
        t_post(&ic);
    }
    c.scratch[0] -= (uint64_t)(g_tpus * 1000.0 * 50);   /* pretend the range took >= 50 ms */
    rrp_post(&c);
}

static void test_hooks(void)
{
    static V3X_API fa;
    v3x_testkit_api(&fa);
    api = &fa;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_tpus = (double)f.QuadPart / 1e6;
    memset(&R, 0, sizeof R);
    R.sched_global = &f_sched_ptr;
    *(int32_t *)(f_sched + SCHED_WORKERS) = 7;
    static uint8_t dummy[64];
    R.hai_pret = dummy + 1; R.hai_sret = dummy + 2; R.tr_eret = dummy + 3; R.ai_iret = dummy + 4;
    R.ok[G_RRP] = R.ok[G_HEUR] = 1;
    CHECK(read_threads() == 8, "threads = [sched+0x648] + 1");
    f_sched_ptr = NULL;
    CHECK(read_threads() == 1, "no scheduler yet -> 1 thread (engine does the same)");
    f_sched_ptr = f_sched;

    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.fine_tasks, sizeof cfg.fine_tasks, "HandleAutonomousInvestment,UpdateAI");
    cfg.fine_heur = 1;
    for (int i = 0; i < 64; i++) f_cost[i] = 10 + (i % 3);
    f_cost[5] = 900;

    /* measure only: nothing written */
    g_lever_ok = 0;
    fake_task("HandleAutonomousInvestment");
    fake_step(40, 3, 0, 0, 1);
    uint8_t before[0xc0]; memcpy(before, f_step, sizeof before);
    LONG64 tick = (LONG64)(g_tpus * 1000.0);   /* 1 ms */
    run_range(1, 40, tick, 30 * tick);
    CHECK(seen_mode == 0 && seen_mode_set == 0 && seen_cost_set == 1 && !memcmp(before, f_step, sizeof before), "measure: Step untouched");
    const apar_rtask *t = rt_find("HandleAutonomousInvestment");
    CHECK(t && t->runs == 1 && t->n_sum == 40 && t->threads_sum == 8 && t->chunks_sum == 8 && t->lever == 0,
          "range recorded: name from the type's std::string (heap form), n=40, T=8, 8 chunks");
    CHECK(t && t->heur_seen == 1 && t->heur_used == 1 && t->heur_replica_used == 1 && t->heur_mismatch == 0 && t->exp_sum == 1,
          "heuristic hook result agrees with the replica (1 expensive item)");
    double work = t ? ms(t->item_sum) : 0, ideal = t ? ms(t->ideal_sum) : 0;
    CHECK(t && t->items == 40 && work > 68.0 && work < 75.0 && ms(t->item_max) >= 29.9 && ideal >= 29.9 && ideal < 31.0,
          "item accounting: 40 items, work %.1f ms, longest %.1f ms, lower bound %.1f ms (= longest)", work, t ? ms(t->item_max) : 0, ideal);

    /* lever on, heuristic kept */
    g_lever_ok = 1;
    fake_step(40, 3, 0, 0, 1);
    memcpy(before, f_step, sizeof before);
    run_range(1, 0, 0, 0);
    CHECK(seen_mode == 2 && seen_mode_set == 1 && seen_cost_set == 1, "lever: mode 2 (grain 1) during the call, heuristic kept");
    CHECK(!memcmp(before, f_step, sizeof before), "lever: Step restored byte for byte after the call");
    CHECK(t->lever == 1 && t->grain1_runs == 2 && t->heur_mismatch == 0, "lever counted; grain 1 (n=40 on 8 threads is grain 1 even in mode 1)");
    /* heuristic off */
    cfg.fine_heur = 0;
    fake_step(40, 3, 0, 0, 1);
    memcpy(before, f_step, sizeof before);
    run_range(0, 0, 0, 0);
    CHECK(seen_mode == 2 && seen_mode_set == 1 && seen_cost_set == 0 && !memcmp(before, f_step, sizeof before),
          "lever with fine_chunks_heuristic=0: cost optional disengaged during the call, restored after");
    CHECK(t->heur_mismatch == 0, "replica sees the disengaged heuristic (no false mismatch)");
    cfg.fine_heur = 1;
    /* explicit mode chosen by the task: never overridden */
    fake_step(40, 3, 1, 3, 0);
    run_range(-1, 0, 0, 0);
    CHECK(seen_mode == 3 && seen_mode_set == 1, "lever never overrides a task's own batch mode (mode 3 = one call)");
    /* unlisted task */
    fake_task("UpdatePopGrowthX");
    fake_step(40, 3, 0, 0, 0);
    run_range(-1, 0, 0, 0);
    CHECK(seen_mode_set == 0, "lever ignores unlisted tasks");
    /* short (SSO) name */
    fake_task("UpdateAI");
    fake_step(1068, 3, 0, 0, 0);
    run_range(-1, 0, 0, 0);
    const apar_rtask *u = rt_find("UpdateAI");
    CHECK(u && u->runs == 1 && seen_mode == 2, "SSO task name read; listed UpdateAI gets the lever");
    /* replica mismatch is reported */
    fake_task("HandleAutonomousInvestment");
    fake_step(40, 3, 0, 0, 1);
    g_lever_ok = 0;
    run_range(0, 0, 0, 0);
    CHECK(t->heur_mismatch == 1, "heuristic hook result vs replica mismatch counted");

    /* return-address classification */
    memset((void *)g_pr, 0, sizeof g_pr);
    V3X_HookCall ic; memset(&ic, 0, sizeof ic);
    ic.user = (void *)(intptr_t)H_HAI_EVAL; ic.return_address = R.hai_sret; ic.scratch[0] = (uint64_t)qpc(); t_post(&ic);
    ic.user = (void *)(intptr_t)H_TR_EVAL; ic.return_address = R.tr_eret; t_post(&ic);
    ic.return_address = dummy + 9; t_post(&ic); t_post(&ic);
    ic.user = (void *)(intptr_t)H_AI_ITEM; ic.return_address = dummy + 9; t_post(&ic);   /* other caller of 0x1604cc0 */
    ic.return_address = R.ai_iret; t_post(&ic);
    CHECK(g_pr[PR_HAI_EVAL_SER].n == 1 && g_pr[PR_HAI_EVAL_PAR].n == 0, "HAI eval from stage 2 counted as serial re-eval");
    CHECK(g_pr[PR_TR_EVAL_INLINE].n == 1 && g_pr[PR_TR_EVAL_FORK].n == 2, "trades eval: inline vs forked by return address");
    CHECK(g_pr[PR_AI_ITEM].n == 1, "UpdateAI item: only calls from the UpdateAI range functor count");
    ic.user = (void *)(intptr_t)H_GUARD;
    for (int i = 0; i < 3; i++) { ic.return_address = dummy + 20; ic.scratch[0] = (uint64_t)(qpc() - 2 * tick); t_post(&ic); }
    ic.return_address = dummy + 21; ic.scratch[0] = (uint64_t)qpc(); t_post(&ic);
    int sites = 0; LONG64 slow = 0;
    for (int i = 0; i < MAXG; i++) if (g_gs[i].ret) { sites++; slow += g_gs[i].slow; }
    CHECK(sites == 2 && slow == 3 && g_pr[PR_GUARD].n == 4, "guard time per call site, > 1 ms counted as renders");

    static char rep[48 * 1024];
    size_t n = apar_format_report(rep, sizeof rep, "test");
    CHECK(n > 500 && strstr(rep, "HandleAutonomousInvestment") && strstr(rep, "finer chunking can recover at most") &&
          strstr(rep, "UpdateStateTrades") && strstr(rep, "RenderFrameIfNeeded"), "report text (%zu bytes)", n);
    char *p = strstr(rep, "range efficiency");
    if (p) { char *e = strstr(p, "\r\n\r\n"); if (e) *e = 0; printf("%s\n", p); }
    reset_stats();
    CHECK(rt_find("HandleAutonomousInvestment")->runs == 0 && g_pr[PR_GUARD].n == 0 && g_gs[0].n == 0, "reset clears statistics, keeps task names");
}

/* ------------------------------------------------------------------ against the real exe */
static int g_hide_rrp;
static void *f_find_string_hide(const char *s) { return g_hide_rrp && !strcmp(s, S_RRP) ? NULL : v3x_tk_find_string(s); }

static void write_ini(const char *dir, const char *text)
{
    char p[MAX_PATH]; snprintf(p, sizeof p, "%sai_par.ini", dir);
    FILE *f = fopen(p, "wb"); if (f) { fputs(text, f); fclose(f); }
}

static void test_exe(const char *exe, const char *dir)
{
    static V3X_API ea;
    if (v3x_testkit_map_exe_dir(exe, dir, &ea)) { CHECK(0, "map %s", exe); return; }
    LARGE_INTEGER t0, t1, fq; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
    int ng = apar_resolve(&ea);
    QueryPerformanceCounter(&t1);
    {   /* where the resolve time goes: the one-pass scan alone */
        V3xSection tx; v3x_section(ea.image_base(), ".text", &tx);
        apar_scanpat sp[2] = {{P_GUARD_DTOR}, {P_HAI_SSITE}};
        LARGE_INTEGER a, b2; QueryPerformanceCounter(&a); apar_multi_scan(tx.ptr, tx.size, sp, 2); QueryPerformanceCounter(&b2);
        printf("(one-pass scan of %.0f MB .text: %.0f ms)\n", tx.size / 1048576.0, (b2.QuadPart - a.QuadPart) * 1000.0 / fq.QuadPart);
    }
    for (int g = 0; g < G_N; g++) CHECK(R.ok[g], "group %s resolves%s%s", G_NAME[g], R.err[g][0] ? ": " : "", R.err[g]);
    CHECK(ng == G_N, "all %d groups (%.0f ms)", G_N, (t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart);
    struct { const char *n; void *p; unsigned long long rva; } E[] = {
        {"RunRangeExecutionParallel", R.rrp, 0x146c960}, {"heuristic", R.heur, 0x146c010}, {"guard dtor", R.guard, 0x1468be0},
        {"scheduler global", R.sched_global, 0x5c726e8}, {"HAI eval", R.hai_eval, 0x921910}, {"HAI parallel ret", R.hai_pret, 0x9f3465},
        {"HAI serial ret", R.hai_sret, 0x9b2c32}, {"HAI stage 2", R.hai_s2, 0x9b2930}, {"HAI build", R.hai_build, 0x8dcea0},
        {"UpdateAI Execute", R.ai_exec, 0x1647910}, {"UpdateAI item", R.ai_item, 0x1604cc0}, {"UpdateAI item ret", R.ai_iret, 0x16cb8f8},
        {"UpdateAI stage 3", R.ai_apply, 0x1647ab0}, {"trades Execute", R.tr_exec, 0xdcedd0}, {"UpdateStateTrades", R.tr_fn, 0xdd1650},
        {"trades eval", R.tr_eval, 0xdd26e0}, {"trades inline ret", R.tr_eret, 0xe02e1a}, {"trades apply", R.tr_apply, 0xd96a90},
        {"PopGrowth parallel", R.pg_par, 0xe5bbd0}, {"PopGrowth apply", R.pg_apply, 0xe5c260},
    };
    for (size_t i = 0; i < sizeof E / sizeof E[0]; i++)
        CHECK(v3x_testkit_rva(E[i].p) == E[i].rva, "%s at exe+0x%llx (expect 0x%llx)", E[i].n, v3x_testkit_rva(E[i].p), E[i].rva);

    /* full plugin load: defaults (no ini) */
    write_ini(dir, "");
    int h0 = v3x_tk.hooks;
    int rc = v3x_plugin_load(&ea);
    CHECK(rc == 0 && v3x_tk.hooks - h0 == 14 && !g_lever_ok, "load (measure): rc %d, %d hook_safe, lever off", rc, v3x_tk.hooks - h0);
    /* mode=on refused */
    write_ini(dir, "[ai_par]\nmode = on\n");
    h0 = v3x_tk.hooks;
    rc = v3x_plugin_load(&ea);
    CHECK(rc == 0 && cfg.mode == APAR_MEASURE && v3x_tk.hooks - h0 == 14, "mode=on refused -> measure (%d hooks)", v3x_tk.hooks - h0);
    /* lever opt-in */
    write_ini(dir, "[ai_par]\nfine_chunks = 1\nprobe_items = 0\n");
    h0 = v3x_tk.hooks;
    rc = v3x_plugin_load(&ea);
    CHECK(rc == 0 && g_lever_ok && v3x_tk.hooks - h0 == 9, "fine_chunks=1 -> lever on; probe_items=0 -> %d hooks", v3x_tk.hooks - h0);
    /* off */
    write_ini(dir, "[ai_par]\nmode = off\n");
    h0 = v3x_tk.hooks;
    rc = v3x_plugin_load(&ea);
    CHECK(rc == 0 && v3x_tk.hooks == h0, "mode=off installs nothing");
    /* fail closed */
    write_ini(dir, "");
    ea.find_string = f_find_string_hide; g_hide_rrp = 1;
    h0 = v3x_tk.hooks;
    rc = v3x_plugin_load(&ea);
    CHECK(rc != 0 && v3x_tk.hooks == h0 && !R.ok[G_RRP], "range-runner string missing -> fail closed (rc %d, no hooks)", rc);
    ea.find_string = v3x_tk_find_string; g_hide_rrp = 0;
    char p[MAX_PATH]; snprintf(p, sizeof p, "%sai_par.ini", dir); DeleteFileA(p);
}

int main(int argc, char **argv)
{
    test_logic();
    test_hooks();
    if (argc > 1) test_exe(argv[1], argc > 2 ? argv[2] : ".\\");
    else printf("(no exe given: anchor checks skipped)\n");
    return v3x_testkit_done();
}
