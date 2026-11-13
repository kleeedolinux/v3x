/* Offline test of plugins/tick_par without the game: includes the plugin source, fakes the V3X API,
 * the runner / builder byte shapes (anchors), a task-type registry with dependency NAME arrays laid out
 * like CJominiTickTaskType, task instances, and a fake RunTask. Checks:
 *   - scheduling logic (tick_par_sched.h): closure incl. transitive deps through non-due tasks, cycles,
 *     bad edges; policy decisions (join before dependents / non-co-runnable tasks / at the end, no
 *     offload of the last item or of a task whose successor depends on it); simulated makespans and
 *     overlap credit; critical path; topological-order check;
 *   - anchors + fail-closed install paths (missing string, changed prologue, call site already redirected);
 *   - the near jump stub + call-site patch, 8-argument pass-through (5th/6th args on the stack);
 *   - runtime graph read (names, deps, unresolved names), graph CSV, per-tick measurement, report + CSV,
 *     mismatched instance -> tick not measured, mode=on refused (still measure), mode=off installs nothing.
 * Build+run (from the repo root; T = scratch dir):
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -o $T/tpar.exe test/tick_par_test.c && $T/tpar.exe
 * Exit code 0 = ALL OK. */
#include "../plugins/tick_par/tick_par.c"

static int fails;
#define CHECK(c, ...) do { int ok_ = !!(c); printf("%s: ", ok_ ? "OK  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); fails += !ok_; } while (0)

static char logbuf[1 << 17]; static size_t logn;
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
static char g_dir[MAX_PATH];
static const char *f_dir(void) { return g_dir; }
static int f_patch(void *a, const void *b, size_t n) { DWORD o; VirtualProtect(a, n, PAGE_EXECUTE_READWRITE, &o); memcpy(a, b, n); return 0; }
typedef void (*ev_fn)(const char *, void *, void *);
static ev_fn g_ev; static int g_nev;
static int f_on_event(const char *name, ev_fn fn, void *user) { (void)name; (void)user; g_ev = fn; g_nev++; return 0; }
static v3x_command_fn g_cmd; static int g_ncmd;
static int f_register(const char *n, const char *h, v3x_command_fn fn, void *u) { (void)n; (void)h; (void)u; g_cmd = fn; g_ncmd++; return 0; }
static uintptr_t f_base(void) { return (uintptr_t)GetModuleHandleA(0); }
static size_t f_size(void)
{
    unsigned char *b = (unsigned char *)GetModuleHandleA(0);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    return nt->OptionalHeader.SizeOfImage;
}

/* ---- fake code: runner, builder, DFS shape; real C functions for RunTask and the sorted registry */
static unsigned char runner_buf[0xd00] __attribute__((aligned(16)));
static unsigned char builder_buf[0x100] __attribute__((aligned(16)));
static unsigned char topo_buf[64];
static const char S1[] = S_RUNNER, S2[] = S_SORTED, S3[] = S_RUNTASK;
#define OFF_BCALL 0x100
#define OFF_RCALL 0x200
#define OFF_INST 0x300
static int g_no_sorted_string, g_topo_missing, g_xref_runner_twice;

static void put(unsigned char *at, const char *pat)
{
    unsigned char b[64]; int w[64];
    int m = parse_pat(pat, b, w, 64);
    memcpy(at, b, (size_t)m);
}
static void put_rel(unsigned char *e8, void *target) { e8[0] = 0xE8; *(int32_t *)(e8 + 1) = (int32_t)((intptr_t)target - (intptr_t)(e8 + 5)); }

/* registry */
typedef struct { void *vt; unsigned char name[32]; int32_t rate, prio; void *deps; int32_t cap, ndeps; void *alloc; unsigned char flag[8]; int64_t order; } FType;
typedef struct { unsigned char s[32]; } FStr;
#define NT 8
static FType g_ft[NT];
static FStr g_fdeps[NT][4];
static char g_longname[40] = "AVeryLongTickTaskNameOver15";
static struct { void **data; int32_t cap, count; } g_reg;
static void *g_regdata[NT];
static void *sorted_impl(void) { return &g_reg; }

static void set_str(unsigned char *s, const char *v)
{
    memset(s, 0, 32);
    uint64_t n = strlen(v);
    if (n <= 15) { memcpy(s, v, n); *(uint64_t *)(s + 24) = 15; }
    else { *(const char **)s = v; *(uint64_t *)(s + 24) = n; }
    *(uint64_t *)(s + 16) = n;
}

/* engine order (topological): 0 A, 1 B, 2 X(dep A), 3 C(dep X), 4 D, 5 E(dep D, "Missing"), 6 L(long name, dep B), 7 F */
static const char *NAMES[NT] = { "A", "B", "X", "C", "D", "E", 0, "F" };
static void build_registry(int with_cycle)
{
    for (int i = 0; i < NT; i++) {
        FType *t = &g_ft[i];
        memset(t, 0, sizeof *t);
        set_str(t->name, NAMES[i] ? NAMES[i] : g_longname);
        t->rate = i == 7 ? 2 : i == 4 ? 1 : 0; t->prio = 1; t->deps = g_fdeps[i]; t->ndeps = 0;
        g_regdata[i] = t;
    }
#define DEP(i, nm) set_str(g_fdeps[i][g_ft[i].ndeps++].s, nm)
    DEP(2, "A"); DEP(3, "X"); DEP(5, "D"); DEP(5, "Missing"); DEP(6, "B");
    if (with_cycle) DEP(0, "C");
    g_reg.data = g_regdata; g_reg.cap = NT; g_reg.count = NT;
}

/* instances + fake builder (orig) */
typedef struct { void *vt; void *type; } FInst;
static FInst g_inst[NT];
static int g_due[NT], g_ndue;
static void *g_due_arr[NT];
static uint64_t fake_builder(PASS8_ARGS)
{
    (void)a0_; (void)a2_;
    struct { void **data; int32_t cap, count; } *out = a1_;
    for (int k = 0; k < g_ndue; k++) { g_inst[g_due[k]].type = &g_ft[g_due[k]]; g_due_arr[k] = &g_inst[g_due[k]]; }
    out->data = g_due_arr; out->cap = NT; out->count = g_ndue;
    return 0x1234;
}
static volatile int g_ran[NT * 4], g_nran; static void *g_a4, *g_a5;
static int g_spin_us[NT];
static uint64_t fake_runtask(void *ctx, void *inst, void *a2, void *a3, void *a4, void *a5)
{
    (void)ctx; (void)a2; (void)a3;
    int id = (int)((FInst *)inst - g_inst);
    g_ran[g_nran++] = id; g_a4 = a4; g_a5 = a5;
    LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    do QueryPerformanceCounter(&t); while ((t.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart < g_spin_us[id]);
    return 0x77 + id;
}

static void *f_find_string(const char *s)
{
    if (!strcmp(s, S1)) return (void *)S1;
    if (!strcmp(s, S2)) return g_no_sorted_string ? 0 : (void *)S2;
    if (!strcmp(s, S3)) return (void *)S3;
    return 0;
}
static void *f_find_xref(const void *t, int i)
{
    if (t == S1) return i == 0 ? runner_buf + 0x20 : (i == 1 && g_xref_runner_twice) ? runner_buf + 0x40 : 0;
    if (t == S2) return i == 0 ? (void *)((uintptr_t)sorted_impl + 1) : 0;
    if (t == S3) return i == 0 ? (void *)((uintptr_t)fake_runtask + 1) : 0;
    return 0;
}
static void *f_function_start(const void *a)
{
    uintptr_t x = (uintptr_t)a;
    if (x >= (uintptr_t)runner_buf && x < (uintptr_t)runner_buf + sizeof runner_buf) return runner_buf;
    if (x == (uintptr_t)sorted_impl + 1) return (void *)sorted_impl;
    if (x == (uintptr_t)fake_runtask + 1) return (void *)fake_runtask;
    return 0;
}
static void *f_pattern_scan(const char *p) { return !g_topo_missing && find_once(topo_buf, sizeof topo_buf, p) >= 0 ? topo_buf : 0; }
static void *g_hooked_target, *g_detour; static int g_nhook, g_nunhook;
static int f_hook(void *target, void *detour, void **orig)
{
    g_hooked_target = target; g_detour = detour; g_nhook++;
    *orig = (void *)fake_builder;
    return 0;
}
static int f_unhook(void *t) { (void)t; g_nunhook++; return 0; }

static V3X_API A;
static void setup_api(void)
{
    memset(&A, 0, sizeof A);
    A.api_version = 3; A.struct_size = sizeof A;
    A.log = f_log; A.print = f_print; A.image_base = f_base; A.image_size = f_size;
    A.pattern_scan = f_pattern_scan; A.find_string = f_find_string; A.find_xref = f_find_xref; A.function_start = f_function_start;
    A.hook = f_hook; A.unhook = f_unhook; A.patch = f_patch; A.v3x_dir = f_dir; A.game_dir = f_dir;
    A.register_command = f_register; A.on_event = f_on_event;
}

static void build_code(void)
{
    memset(runner_buf, 0xCC, sizeof runner_buf); memset(builder_buf, 0xCC, sizeof builder_buf); memset(topo_buf, 0xCC, sizeof topo_buf);
    put(runner_buf, P_RUNNER_PRO);
    put(runner_buf + OFF_BCALL, P_BUILD_CALL); put_rel(runner_buf + OFF_BCALL + 10, builder_buf);
    put(runner_buf + OFF_RCALL, P_RUN_CALL); put_rel(runner_buf + OFF_RCALL + 6, (void *)fake_runtask);
    put(runner_buf + OFF_INST, P_INST_TYPE);
    put(builder_buf, P_BUILD_PRO);
    put(builder_buf + 0x16, P_BUILD_SORT); put_rel(builder_buf + 0x16 + 9, (void *)sorted_impl);
    put(builder_buf + 0x40, P_BUILD_RATE);
    put(topo_buf + 4, P_TOPO_VISIT);
    DWORD o; VirtualProtect(runner_buf, sizeof runner_buf, PAGE_EXECUTE_READWRITE, &o);
}

static void reset_plugin(void)
{
    orig_builder = orig_run = 0; g_sorted = 0; g_callsite = g_stub = 0;
    g_graph_state = 0; g_nn = 0; g_edges = 0;
    memset(g_hmap_k, 0, sizeof g_hmap_k);
    memset(&T, 0, sizeof T);
    memset(g_b, 0, sizeof g_b); g_ticks_bad = g_order_bad = g_hook_ticks = 0;
    g_nhook = g_nunhook = 0; g_ncmd = g_nev = 0;
    g_no_sorted_string = g_topo_missing = g_xref_runner_twice = 0;
    logn = 0; logbuf[0] = 0;
}

/* run one "tick" like the engine runner: builder, then each due task through the patched call site */
typedef uint64_t (*call6)(void *, void *, void *, void *, void *, void *);
static int run_tick(const int *due, int n)
{
    memcpy(g_due, due, sizeof(int) * (size_t)n); g_ndue = n;
    struct { void **data; int32_t cap, count; } out = { 0, 0, 0 };
    ((pass8_fn)g_detour)((void *)0x1000, &out, (void *)0x2000, 0, 0, 0, 0, 0);
    unsigned char *e8 = runner_buf + OFF_RCALL + 6;
    call6 f = (call6)(void *)rel_target(e8);
    int ok = 1;
    for (int k = 0; k < out.count; k++) {
        uint64_t r = f((void *)0x1000, out.data[k], (void *)3, (void *)4, (void *)0x55555, (void *)0x66666);
        ok &= r == (uint64_t)(0x77 + due[k]);
    }
    return ok;
}

static void test_sched(void)
{
    static TpGraph g;
    /* nodes: 0 A, 1 B, 2 X(A), 3 C(X), 4 D, 5 E(D) */
    int from[] = { 2, 3, 5 }, to[] = { 0, 2, 4 };
    CHECK(tp_closure(&g, 6, from, to, 3) == 0, "closure ok");
    CHECK(tp_dep(&g, 3, 0) && tp_dep(&g, 3, 2) && !tp_dep(&g, 3, 1) && !tp_dep(&g, 0, 3), "C depends on X and (transitively) A, not on B");
    int cf[] = { 0, 1 }, ct[] = { 1, 0 };
    static TpGraph g2;
    CHECK(tp_closure(&g2, 2, cf, ct, 2) == -2, "cycle detected");
    int sf[] = { 1 }, st[] = { 1 };
    CHECK(tp_closure(&g2, 2, sf, st, 1) == -2, "self dependency = cycle");
    int bf[] = { 0 }, bt[] = { 9 };
    CHECK(tp_closure(&g2, 2, bf, bt, 1) == -1, "edge out of range rejected");

    unsigned char all[8] = { 1, 1, 1, 1, 1, 1, 1, 1 }, none[8] = { 0 };
    /* independent pair A,B */
    { int reg[] = { 0, 1 }; double d[] = { 10, 10 }, sv[2];
      double m = tp_simulate(&g, reg, d, 2, all, all, sv);
      CHECK(m == 10 && sv[0] == 10, "A || B: makespan 10 (A offloaded, joined at the end), A credited 10 (got %.1f, %.1f)", m, sv[0]);
      CHECK(tp_simulate(&g, reg, d, 2, none, none, 0) == 20, "nothing vetted: serial 20"); }
    /* chain A -> X: no offload */
    { int reg[] = { 0, 2 }; double d[] = { 10, 10 };
      CHECK(tp_simulate(&g, reg, d, 2, all, all, 0) == 20, "X depends on A: not offloaded (20)"); }
    /* A, B, C(dep A via X not due): join before C */
    { int reg[] = { 0, 1, 3 }; double d[] = { 10, 4, 3 }, sv[3];
      TpState s; tp_begin(&s);
      int a0 = tp_decide(&s, &g, reg, 3, 0, all, all, 0), a1 = tp_decide(&s, &g, reg, 3, 1, all, all, 0), a2 = tp_decide(&s, &g, reg, 3, 2, all, all, 0);
      CHECK(a0 == TP_OFFLOAD && a1 == 0 && a2 == TP_JOIN_BEFORE, "decisions: offload A, B inline, join before C (dep via non-due X): %d %d %d", a0, a1, a2);
      double m = tp_simulate(&g, reg, d, 3, all, all, sv);
      CHECK(m == 13 && sv[0] == 4, "makespan 13 (B 0-4, wait A until 10, C 10-13), overlap 4 (got %.1f, %.1f)", m, sv[0]); }
    /* B may not co-run: no offload of A */
    { int reg[] = { 0, 1 }; double d[] = { 10, 10 }; unsigned char cr[8] = { 1, 0, 1, 1, 1, 1 };
      CHECK(tp_simulate(&g, reg, d, 2, all, cr, 0) == 20, "successor not co-runnable: serial"); }
    /* last item never offloaded, in-flight finished early is joined and the lane reused */
    { int reg[] = { 0, 1, 4, 5 }; double d[] = { 1, 5, 6, 2 }, sv[4];
      TpState s; tp_begin(&s);
      int a3; tp_decide(&s, &g, reg, 4, 0, all, all, 0); tp_decide(&s, &g, reg, 4, 1, all, all, 0);
      int a2 = tp_decide(&s, &g, reg, 4, 2, all, all, 1);
      CHECK(a2 == (TP_JOIN_BEFORE), "D: finished A joined; D not offloaded because E depends on D (%d)", a2);
      a3 = tp_decide(&s, &g, reg, 4, 3, all, all, 0);
      CHECK(a3 == 0, "last item E runs inline (%d)", a3);
      double m = tp_simulate(&g, reg, d, 4, all, all, sv);
      CHECK(m == 13 && sv[0] == 1, "makespan 13 = 5+6+2 with A hidden under B (got %.1f)", m); }
    /* critical path + order check */
    { int reg[] = { 0, 1, 2, 3 }; double d[] = { 5, 7, 1, 1 }, scr[4];
      CHECK(tp_critical_path(&g, reg, d, 4, scr) == 7, "critical path = max(A+X+C=7, B=7) = 7");
      int bad[] = { 3, 0 };
      CHECK(tp_order_ok(&g, reg, 4) && !tp_order_ok(&g, bad, 2), "topological-order check"); }
}

int main(void)
{
    GetTempPathA(sizeof g_dir, g_dir);
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); qpc_freq = (double)fq.QuadPart;
    test_sched();

    setup_api(); build_code(); build_registry(0);
    /* ---- fail-closed installs */
    struct { const char *what; int code; } fc[] = { { "sorted string missing", 8 }, { "DFS shape missing", 9 }, { "runner string xref twice", 2 },
                                                     { "runner prologue changed", 2 }, { "call site already redirected", 5 } };
    for (int c = 0; c < 5; c++) {
        reset_plugin(); build_code();
        if (c == 0) g_no_sorted_string = 1;
        if (c == 1) g_topo_missing = 1;
        if (c == 2) g_xref_runner_twice = 1;
        if (c == 3) runner_buf[0] = 0x90;
        if (c == 4) put_rel(runner_buf + OFF_RCALL + 6, (void *)sorted_impl);
        unsigned char before[11]; memcpy(before, runner_buf + OFF_RCALL, 11);
        api = &A; img_lo = A.image_base(); img_hi = img_lo + A.image_size();
        int rc = install();
        CHECK(rc == fc[c].code && g_nhook == 0 && !memcmp(before, runner_buf + OFF_RCALL, 11),
              "fail closed (%s): code %d, nothing hooked or patched", fc[c].what, rc);
    }

    /* ---- mode=off: nothing */
    reset_plugin(); build_code();
    char ini[MAX_PATH]; snprintf(ini, sizeof ini, "%stick_par.ini", g_dir);
    WritePrivateProfileStringA("tick_par", "mode", "off", ini);
    CHECK(v3x_plugin_load(&A) == 0 && g_nhook == 0 && g_ncmd == 0, "mode=off: nothing installed");

    /* ---- mode=on: refused, runs as measure */
    reset_plugin(); build_code();
    WritePrivateProfileStringA("tick_par", "mode", "on", ini);
    int rc = v3x_plugin_load(&A);
    CHECK(rc == 0 && g_nhook == 1 && g_hooked_target == builder_buf && strstr(logbuf, "mode=on REFUSED"), "mode=on: refused (logged), measure installed");
    CHECK(g_mode == M_ON && g_ncmd == 1 && g_nev == 1, "console command + load_transition subscribed");
    unsigned char *tgt = rel_target(runner_buf + OFF_RCALL + 6);
    CHECK(tgt == g_stub && tgt[0] == 0xFF && tgt[1] == 0x25 && *(void **)(tgt + 6) == (void *)hk_run, "call site -> near stub -> hk_run");
    CHECK(orig_run == (pass8_fn)(void *)fake_runtask, "orig RunTask = the call's original target");

    /* ---- default (no ini key): measure */
    WritePrivateProfileStringA("tick_par", "mode", 0, ini);

    /* ---- ticks */
    int due1[] = { 0, 1, 2, 3, 4, 5, 6 };          /* DAY tick (D rate DAY) */
    g_spin_us[0] = 3000; g_spin_us[1] = 3000; g_spin_us[2] = 200; g_spin_us[3] = 200; g_spin_us[4] = 500; g_spin_us[5] = 500; g_spin_us[6] = 100;
    g_nran = 0;
    int ok = run_tick(due1, 7);
    CHECK(ok && g_nran == 7 && g_a4 == (void *)0x55555 && g_a5 == (void *)0x66666, "tick 1: all 7 tasks ran in order with 5th/6th args intact, return values passed");
    int inorder = 1; for (int k = 0; k < 7; k++) inorder &= g_ran[k] == due1[k];
    CHECK(inorder, "engine order kept (read-only)");
    CHECK(g_graph_state == 1 && g_nn == NT && g_edges == 4, "runtime graph: %d tasks, %d edges (unknown dep name skipped)", g_nn, g_edges);
    CHECK(!strcmp(g_node[6].name, g_longname) && g_node[5].unresolved == 1, "long (heap) name read; unresolved dep counted");
    CHECK(tp_dep(&g_graph, 3, 0) && tp_dep(&g_graph, 6, 1) && !tp_dep(&g_graph, 6, 0), "transitive deps from the live type objects");
    CHECK(g_b[1].ticks == 1 && g_b[1].all2 < g_b[1].serial && g_b[1].vet2 == g_b[1].serial && g_b[1].crit <= g_b[1].all2,
          "DAY bucket: all-2lane < serial, vetted == serial (nothing vetted), critpath <= 2-lane");
    int due2[] = { 0, 1, 7 };                        /* WEEK tick */
    run_tick(due2, 3);
    CHECK(g_b[2].ticks == 1, "WEEK tick bucketed by highest due rate");
    /* builder reports an instance, runner calls another -> tick not measured, still forwarded */
    memcpy(g_due, due2, sizeof due2); g_ndue = 3;
    struct { void **data; int32_t cap, count; } out = { 0, 0, 0 };
    ((pass8_fn)g_detour)((void *)0x1000, &out, 0, 0, 0, 0, 0, 0);
    call6 f = (call6)(void *)rel_target(runner_buf + OFF_RCALL + 6);
    LONG64 bad0 = g_ticks_bad;
    uint64_t r1 = f((void *)0x1000, &g_inst[4], 0, 0, 0, 0);
    CHECK(r1 == 0x77 + 4 && g_ticks_bad == bad0 + 1 && !T.valid, "unexpected instance: forwarded, tick dropped from the stats");
    f((void *)0x1000, &g_inst[1], 0, 0, 0, 0); f((void *)0x1000, &g_inst[7], 0, 0, 0, 0);
    CHECK(g_b[2].ticks == 1, "dropped tick not counted");

    logn = 0;
    g_cmd("", 0);
    CHECK(strstr(logbuf, "DAY") && strstr(logbuf, "WEEK") && strstr(logbuf, "co-runs with"), "console report has buckets + overlap table");
    char p2[MAX_PATH]; snprintf(p2, sizeof p2, "%stick_par_report.csv", g_dir);
    FILE *fp = fopen(p2, "rb"); char buf[8192] = { 0 }; if (fp) { fread(buf, 1, sizeof buf - 1, fp); fclose(fp); }
    CHECK(strstr(buf, "dueset,ticks") && strstr(buf, "\r\nA,TICK,DEFAULT,2,"), "report CSV written");
    snprintf(p2, sizeof p2, "%stick_par_graph.csv", g_dir);
    fp = fopen(p2, "rb"); memset(buf, 0, sizeof buf); if (fp) { fread(buf, 1, sizeof buf - 1, fp); fclose(fp); }
    CHECK(strstr(buf, "3,C,TICK,DEFAULT,X,0") && strstr(buf, "5,E,TICK,DEFAULT,D,1"), "graph CSV: direct deps + unresolved count");
    V3X_LoadTransition lt = { "Game", "Empty", 1.0 };
    logn = 0; g_ev("load_transition", &lt, 0);
    CHECK(strstr(logbuf, "Game->Empty") != 0, "report on Game->Empty");
    g_cmd("reset", 0);
    CHECK(g_b[1].ticks == 0 && g_node[0].runs == 0, "tickpar reset");

    /* ---- registry with a cycle: measurement off, pass-through */
    reset_plugin(); build_code(); build_registry(1);
    rc = v3x_plugin_load(&A);
    g_nran = 0;
    ok = run_tick(due1, 7);
    CHECK(rc == 0 && ok && g_nran == 7 && g_graph_state == -1 && g_b[1].ticks == 0 && strstr(logbuf, "cycle"),
          "dependency cycle: measurement off, tasks still run");
    DeleteFileA(ini);
    printf("%s\n", fails ? "FAILED" : "ALL OK");
    return fails;
}
