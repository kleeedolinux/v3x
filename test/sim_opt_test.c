/* Offline test of plugins/sim_opt without the game: includes the plugin source, fakes the V3X API,
 * a RunTask byte region (anchor shapes), a few CTickTask vtables and Execute bodies that fill the
 * Step like the real ones (parallel / serial / single / multi), then checks anchors, patching,
 * fail-closed paths, diag classification, throttle (1 of N, deny list, never 2 skips in a row),
 * the console report + CSV, diag_trades (anchors + fail-closed cases, per-run iteration stats, report,
 * CSV), diag_journal (anchors, per-type timing, type-name reads incl. invalid pointers, report, CSV),
 * and 6-argument pass-through through every detour (5th/6th args on the stack).
 * Build+run (from the repo root; T = scratch dir, e.g. $V3X_TMP/sim):
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -o $T/sim_opt_test.exe test/sim_opt_test.c && $T/sim_opt_test.exe
 * Exit code 0 = ALL OK. */
#include "../plugins/sim_opt/sim_opt.c"

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
static int f_patch(void *a, const void *b, size_t n) { memcpy(a, b, n); return 0; }
typedef void (*ev_fn)(const char *, void *, void *);
static struct { char name[64]; ev_fn fn; void *user; } evs[8]; static int nev;
static int f_on_event(const char *name, ev_fn fn, void *user)
{
    if (nev >= 8) return -1;
    snprintf(evs[nev].name, 64, "%s", name); evs[nev].fn = fn; evs[nev].user = user; nev++; return 0;
}
static int f_emit_event(const char *name, void *data)
{
    int c = 0;
    for (int i = 0; i < nev; i++) if (!strcmp(evs[i].name, name)) { evs[i].fn(name, data, evs[i].user); c++; }
    return c;
}


/* fake RunTask: the three shapes in order, inside one "function" */
static unsigned char runtask[0x200];
static const char CTX[] = "Context %d";
static int g_xrefs = 1;
static const char TRS[] = "UpdateStateTrades";
static void *tr_xref(int i);
static unsigned char trcode[0x4000];
static const char EOPS_[] = "ExecuteOnPulse";
static void *jr_xref(int i);
static unsigned char jrcode[0x3000];
static void *f_find_string(const char *s)
{
    return !strcmp(s, CTX) ? (void *)CTX : !strcmp(s, TRS) ? (void *)TRS : !strcmp(s, EOPS_) ? (void *)EOPS_ : 0;
}
static void *f_find_xref(const void *t, int i)
{
    if (t == TRS) return tr_xref(i);
    if (t == EOPS_) return jr_xref(i);
    return i < g_xrefs ? runtask + 0x150 : 0;
}
static void *f_function_start(const void *a)
{
    const unsigned char *p = a;
    if (p >= trcode && p < trcode + sizeof trcode) return trcode + ((p - trcode) & ~0xfff);
    if (p >= jrcode && p < jrcode + sizeof jrcode) return jrcode + 0x100;
    return runtask;
}
static void put(int off, const char *hex)
{
    for (const char *s = hex; *s;) {
        while (*s == ' ') s++;
        if (!*s) break;
        runtask[off++] = s[0] == '?' ? 0x75 : (unsigned char)strtoul(s, 0, 16);
        s += 2;
    }
}

/* fake Execute bodies (fill the Step like the game) */
static volatile LONG n_par, n_ser, n_single;
static void ex_parallel(void *task, void *step)
{
    (void)task; unsigned char *s = step; InterlockedIncrement(&n_par);
    *(uint32_t *)s |= 3; *(int32_t *)(s + STEP_BEGIN) = 0; *(int32_t *)(s + STEP_END) = 100;
    *(void **)(s + STEP_RANGE_IMPL) = (void *)1;
}
static void ex_serial(void *task, void *step)
{
    (void)task; unsigned char *s = step; InterlockedIncrement(&n_ser);
    *(int32_t *)(s + STEP_BEGIN) = 5; *(int32_t *)(s + STEP_END) = 15;
    *(void **)(s + STEP_RANGE_IMPL) = (void *)1;
}
static void ex_single(void *task, void *step)
{
    (void)task; unsigned char *s = step; InterlockedIncrement(&n_single);
    *(void **)(s + STEP_SINGLE_IMPL) = (void *)1;
}
static void slot_common(void) {}
static void slot_other(void) {}

typedef struct { uintptr_t col; uintptr_t slot[8]; } FakeVt;
static FakeVt vts[60];
static struct { void *vt; } objs[60];

static void run_task(int i, unsigned char *step)
{
    memset(step, 0, 0xc0);
    ((exec_fn)((uintptr_t *)objs[i].vt)[6])(&objs[i], step);   /* what RunTask does: call [rax+0x30] */
}


/* ======================================================================== diag_trades */
/* trcode: fnA @0, fnB @0x1000, fnC @0x2000, pfor @0x3000 */
static int tr_xref_mode;                          /* 0: A,B,B  1: A,B,C(with pattern too) */
static void *tr_xref(int i)
{
    static const int base[2][3] = { { 0x0, 0x1000, 0x1000 }, { 0x0, 0x1000, 0x2000 } };
    return i < 3 ? trcode + base[tr_xref_mode][i] + 0x10 + i : 0;
}
static int tr_sleep_par = 5, tr_sleep_apply = 10, tr_iters = 3, tr_ns[8] = { 100, 60, 10 };
static int tr_fail_hook_trades, tr_unhooked;
static uint64_t fake_pfor(void *p, void *b, void *c, void *d) { (void)p; (void)b; (void)c; (void)d; Sleep(tr_sleep_par); return 7; }
static uint64_t fake_trades(void *a, void *b, void *c, void *d)
{
    (void)a; (void)b; (void)c; (void)d;
    for (int i = 0; i < tr_iters; i++) {
        int32_t params[8] = { 0, 0, 0, 0, tr_ns[i], 4, 10, 1 };   /* +0xc begin = 0, +0x10 end = n */
        params[3] = 0;
        hk_pfor(params, 0, 0, 0, 0, 0, 0, 0);
        Sleep(tr_sleep_apply);
    }
    return 99;
}

/* ======================================================================== diag_journal */
typedef struct { char pad[0x18]; union { char buf[16]; char *ptr; } u; uint64_t size, cap; char rest[0x800]; } FakeJeType;
typedef struct { char pad[0x18]; FakeJeType *type; } FakeJe;
typedef uint64_t (*six_fn_j)(void *, void *, void *, void *, void *, void *);
static FakeJeType je_short, je_long, je_bad, je_weird;
static char je_long_name[] = "je_a_rather_long_journal_entry_type_name";
static int eop_sleep_ms = 2, eop_nest;
static void *eop_seen[6], *s2_seen[6];
static uint64_t fake_eop(void *a, void *b, void *c, void *d, void *e, void *f)
{
    eop_seen[0] = a; eop_seen[1] = b; eop_seen[2] = c; eop_seen[3] = d; eop_seen[4] = e; eop_seen[5] = f;
    if (eop_nest) { eop_nest = 0; hk_eop(a, b, c, d, e, f, 0, 0); }   /* nested ExecuteOnPulse */
    if (eop_sleep_ms) Sleep(eop_sleep_ms);
    return 0x1234;
}
static uint64_t fake_s2(void *a, void *b, void *c, void *d, void *e, void *f)
{
    s2_seen[0] = a; s2_seen[1] = b; s2_seen[2] = c; s2_seen[3] = d; s2_seen[4] = e; s2_seen[5] = f;
    Sleep((int)(uintptr_t)b == 7 ? 120 : 1);
    return 0x5678;
}
/* jrcode: EOP fn @0x100, docall @0x1000 (target @0x2000), decoy docall @0x1800 */
static int jr_fake_xrefs = 1;
static void *jr_xref(int i) { return i < jr_fake_xrefs ? jrcode + 0x150 : 0; }
static void jr_put(unsigned char *p, const char *hex)
{
    for (const char *q = hex; *q;) {
        while (*q == ' ') q++;
        if (!*q) break;
        *p++ = q[0] == '?' ? 0x27 : (unsigned char)strtoul(q, 0, 16);
        q += 2;
    }
}
static void set_name(FakeJeType *t, const char *n)
{
    size_t k = strlen(n);
    if (k <= 15) { memcpy(t->u.buf, n, k + 1); t->cap = 15; } else { t->u.ptr = (char *)n; t->cap = k + 8; }
    t->size = k;
}

static void journal_tests(void)
{
    /* anchors on fake code */
    memset(jrcode, 0x90, sizeof jrcode);
    jr_put(jrcode + 0x100, JR_EOP_PROLOGUE);
    unsigned char *dc = jrcode + 0x1000, *tgt = jrcode + 0x2000, *decoy = jrcode + 0x1800;
    jr_put(dc, JR_DOCALL); *(int32_t *)(dc + JR_DOCALL_LEN) = (int32_t)(tgt - (dc + JR_DOCALL_LEN + 4));
    jr_put(decoy, JR_DOCALL); *(int32_t *)(decoy + JR_DOCALL_LEN) = (int32_t)((jrcode + 0x2800) - (decoy + JR_DOCALL_LEN + 4));
    jr_put(tgt, JR_S2_PROLOGUE);
    uintptr_t slo = text_lo, shi = text_hi;
    text_lo = (uintptr_t)jrcode; text_hi = (uintptr_t)jrcode + sizeof jrcode;
    jr_fake_xrefs = 2;
    CHECK(jr_setup() == -1 && !g_jr_on, "diag_journal: 2 xrefs to 'ExecuteOnPulse' -> off");
    jr_fake_xrefs = 1; jrcode[0x100] = 0x90;
    CHECK(jr_setup() == -2 && !g_jr_on, "diag_journal: ExecuteOnPulse prologue differs -> off");
    jrcode[0x100] = 0x48;
    CHECK(jr_setup() == 0 && g_jr_on && g_jr_eop == jrcode + 0x100 && g_jr_s2 == tgt,
          "diag_journal: ExecuteOnPulse + per-country stage found (decoy Do_call ignored)");
    text_lo = slo; text_hi = shi;

    /* timing per type */
    set_name(&je_short, "je_short"); set_name(&je_long, je_long_name);
    je_bad.cap = 64; je_bad.size = 10; je_bad.u.ptr = (char *)(uintptr_t)0x10;   /* unreadable */
    set_name(&je_weird, "bad name!");
    FakeJe e1 = { {0}, &je_short }, e2 = { {0}, &je_long }, e3 = { {0}, &je_bad }, e4 = { {0}, &je_weird };
    jr_reset();
    for (int i = 0; i < 3; i++) hk_eop(&e1, (void *)1, 0, 0, 0, 0, 0, 0);
    eop_sleep_ms = 150; hk_eop(&e2, (void *)1, 0, 0, 0, 0, 0, 0); eop_sleep_ms = 2;
    hk_eop(&e2, (void *)0, 0, 0, 0, 0, 0, 0);
    hk_eop(&e3, (void *)2, 0, 0, 0, 0, 0, 0);
    hk_eop(&e4, (void *)1, 0, 0, 0, 0, 0, 0);
    eop_nest = 1; hk_eop(&e1, (void *)1, 0, 0, 0, 0, 0, 0);
    JrType *ts = jr_slot(&je_short), *tl = jr_slot(&je_long), *tb = jr_slot(&je_bad);
    CHECK(ts->calls[1] == 4 && tl->calls[1] == 1 && tl->calls[0] == 1 && tb->calls[2] == 1,
          "calls per type and pulse (nested call counted once): short %lld, long m%lld d%lld, bad y%lld",
          (long long)ts->calls[1], (long long)tl->calls[1], (long long)tl->calls[0], (long long)tb->calls[2]);
    CHECK(tl->max[1] * 1000.0 / qpc_freq >= 10, "max time per type recorded");
    char nm[100];
    jr_name(&je_short, nm, sizeof nm); int ok1 = !strcmp(nm, "je_short");
    jr_name(&je_long, nm, sizeof nm); int ok2 = !strcmp(nm, je_long_name);
    jr_name(&je_bad, nm, sizeof nm); int ok3 = !strncmp(nm, "type@", 5);
    jr_name(&je_weird, nm, sizeof nm); int ok4 = !strncmp(nm, "type@", 5);
    jr_name((void *)(uintptr_t)0x20, nm, sizeof nm); int ok5 = !strncmp(nm, "type@", 5);
    CHECK(ok1 && ok2 && ok3 && ok4 && ok5, "type names: SSO, heap, unreadable/invalid/non-printable -> pointer fallback (%d%d%d%d%d)",
          ok1, ok2, ok3, ok4, ok5);

    /* per-country stage */
    for (int i = 0; i < 3; i++) hk_s2((void *)0x99, (void *)(uintptr_t)(i + 5), (void *)1, 0, 0, 0, 0, 0);
    hk_s2((void *)0x99, (void *)7, (void *)2, 0, 0, 0, 0, 0);
    CHECK(g_jr_s2_calls[1] == 3 && g_jr_s2_calls[2] == 1, "per-country stage counted per pulse");

    /* 6-argument pass-through through both detours */
    pass8_fn se = orig_eop, s2o = orig_s2;
    orig_eop = (pass8_fn)fake_eop; orig_s2 = (pass8_fn)fake_s2;
    eop_sleep_ms = 0;
    uint64_t r = ((six_fn_j)hk_eop)(&e1, (void *)1, (void *)0x3333, (void *)0x4444, (void *)0x5555, (void *)0x6666);
    CHECK(r == 0x1234 && eop_seen[0] == &e1 && eop_seen[1] == (void *)1 && eop_seen[2] == (void *)0x3333 &&
          eop_seen[3] == (void *)0x4444 && eop_seen[4] == (void *)0x5555 && eop_seen[5] == (void *)0x6666,
          "hk_eop forwards 6 args (2 on the stack) + return value");
    r = ((six_fn_j)hk_s2)((void *)0x1111, (void *)0x2222, (void *)1, (void *)0x4444, (void *)0x5555, (void *)0x6666);
    CHECK(r == 0x5678 && s2_seen[0] == (void *)0x1111 && s2_seen[1] == (void *)0x2222 && s2_seen[2] == (void *)1 &&
          s2_seen[3] == (void *)0x4444 && s2_seen[4] == (void *)0x5555 && s2_seen[5] == (void *)0x6666,
          "hk_s2 forwards 6 args (2 on the stack) + return value");
    orig_eop = se; orig_s2 = s2o; eop_sleep_ms = 2;

    /* report, csv, Game->Empty, reset */
    size_t before = logn;
    cmd_simopt("", 0);
    char *rep = logbuf + before;
    char *pl = strstr(rep, je_long_name), *ps = strstr(rep, "je_short");
    CHECK(strstr(rep, "diag_journal: ExecuteOnPulse by journal-entry type") && pl && ps && pl < ps,
          "console report lists types by total time (long before short)");
    CHECK(strstr(rep, "costliest countries") && strstr(rep, "(country id): #7 ") != 0, "costliest countries line (#7 first)");
    char csvp[MAX_PATH]; snprintf(csvp, sizeof csvp, "%ssim_opt_diag.csv", g_dir);
    FILE *f = fopen(csvp, "r"); static char csv[8192]; memset(csv, 0, sizeof csv);
    if (f) { fread(csv, 1, sizeof csv - 1, f); fclose(f); }
    CHECK(strstr(csv, "journal_type,monthly_calls") && strstr(csv, "\nje_short,5,") && strstr(csv, "journal_country_stage,pulse") &&
          strstr(csv, "\nall,monthly,4,"), "csv journal rows");
    DeleteFileA(csvp);
    before = logn;
    V3X_LoadTransition t = { "Game", "Empty", 1.0 };
    on_transition("load_transition", &t, 0);
    CHECK(strstr(logbuf + before, "diag_journal: ExecuteOnPulse by journal-entry type") != 0, "report on Game->Empty");
    DeleteFileA(csvp);
    cmd_simopt("reset", 0);
    CHECK(jr_slot(&je_short)->calls[1] == 0 && g_jr_s2_calls[1] == 0, "reset clears diag_journal");
}

static int f_hook(void *target, void *detour, void **orig)
{
    (void)detour;
    if (target == trcode + 0x3000) { *orig = (void *)fake_pfor; return 0; }
    if (target == trcode + 0x1000) { if (tr_fail_hook_trades) return 5; *orig = (void *)fake_trades; return 0; }
    if (target == jrcode + 0x100) { *orig = (void *)fake_eop; return 0; }
    if (target == jrcode + 0x2000) { *orig = (void *)fake_s2; return 0; }
    return 9;
}
static int f_unhook(void *t) { (void)t; tr_unhooked++; return 0; }
static void tr_put(unsigned char *p, const char *hex)
{
    for (const char *q = hex; *q;) { while (*q == ' ') q++; if (!*q) break; *p++ = (unsigned char)strtoul(q, 0, 16); q += 2; }
}

/* A target that reads args 5 and 6 from the stack, like 0xe02c90 reads its source-location record. */
typedef uint64_t (*six_fn)(void *, void *, void *, void *, void *, void *);
static void *seen6[6]; static int six_calls;
static uint64_t fake6(void *a, void *b, void *c, void *d, void *e, void *f)
{
    seen6[0] = a; seen6[1] = b; seen6[2] = c; seen6[3] = d; seen6[4] = e; seen6[5] = f; six_calls++;
    return (uint64_t)(uintptr_t)e ^ 0xABCDEF;
}
static void fake6_exec(void *a, void *b, void *c, void *d, void *e, void *f) { fake6(a, b, c, d, e, f); }
static int six_ok(void)
{
    return seen6[0] == (void *)0x1111 && seen6[1] == (void *)0x2222 && seen6[2] == (void *)0x3333 &&
           seen6[3] == (void *)0x4444 && seen6[4] == (void *)0x5555 && seen6[5] == (void *)0x6666;
}
static void passthrough_tests(void)
{
    /* hk_pfor / hk_trades outside a run and inside a run */
    pass8_fn sp = orig_pfor, st = orig_trades;
    orig_pfor = (pass8_fn)fake6; orig_trades = (pass8_fn)fake6;
    int32_t params[8] = { 0, 0, 0, 0, 9, 0, 0, 0 };
    memset(seen6, 0, sizeof seen6);
    uint64_t r = ((six_fn)hk_pfor)((void *)0x1111, (void *)0x2222, (void *)0x3333, (void *)0x4444, (void *)0x5555, (void *)0x6666);
    CHECK(r == (0x5555 ^ 0xABCDEF) && six_ok(), "hk_pfor (outside run) forwards 6 args incl. 2 stack args + return value");
    t_tr_in = 1;
    memset(seen6, 0, sizeof seen6);
    r = ((six_fn)hk_pfor)(params, (void *)0x2222, (void *)0x3333, (void *)0x4444, (void *)0x5555, (void *)0x6666);
    seen6[0] = seen6[0] == params ? (void *)0x1111 : 0;
    CHECK(r == (0x5555 ^ 0xABCDEF) && six_ok(), "hk_pfor (inside run) forwards the 5th stack arg (the crash case)");
    t_tr_in = 0;
    memset(seen6, 0, sizeof seen6);
    r = ((six_fn)hk_trades)((void *)0x1111, (void *)0x2222, (void *)0x3333, (void *)0x4444, (void *)0x5555, (void *)0x6666);
    CHECK(r == (0x5555 ^ 0xABCDEF) && six_ok(), "hk_trades forwards 6 args + return value");
    orig_pfor = sp; orig_trades = st;
    /* slot-6 detour: a task whose Execute takes 6 args */
    Task *t = &g_t[10];
    exec_fn so = t->orig; t->orig = (exec_fn)fake6_exec;
    int sd = cfg.diag; LONG st_on = g_throttle_on; g_throttle_on = 0;
    for (int dg = 0; dg < 2; dg++) {
        cfg.diag = dg;
        static unsigned char stp[0xc0];
        memset(seen6, 0, sizeof seen6); memset(stp, 0, sizeof stp);
        ((six_fn)vts[10].slot[6])(&objs[10], stp, (void *)0x3333, (void *)0x4444, (void *)0x5555, (void *)0x6666);
        seen6[0] = seen6[0] == &objs[10] ? (void *)0x1111 : 0;
        seen6[1] = seen6[1] == stp ? (void *)0x2222 : 0;
        CHECK(six_ok(), "ex_detour (diag=%d) forwards 6 args to Execute", dg);
    }
    cfg.diag = sd; g_throttle_on = st_on; t->orig = so;
}

static void trades_tests(void)
{
    memset(trcode, 0x90, sizeof trcode);
    unsigned char *b = trcode + 0x1000 + 0x400, *pf = trcode + 0x3000;
    tr_put(b, TR_CALL_PAT); *(int32_t *)(b + 15) = (int32_t)(pf - (b + 19));
    tr_put(pf, TR_PFOR_PROLOGUE); tr_put(pf + 0x30, TR_PFOR_RANGE);
    uintptr_t slo = text_lo, shi = text_hi;
    text_lo = (uintptr_t)trcode; text_hi = (uintptr_t)trcode + sizeof trcode;
    img_lo = 0;

    tr_xref_mode = 1;
    unsigned char *c = trcode + 0x2000 + 0x300; tr_put(c, TR_CALL_PAT);
    CHECK(tr_setup() == -3 && !g_tr_on, "diag_trades: two candidate functions -> off");
    memset(c, 0x90, 16); tr_xref_mode = 0;
    tr_put(b + 0x200, TR_CALL_PAT);
    CHECK(tr_setup() == -2 && !g_tr_on, "diag_trades: call site twice -> off");
    memset(b + 0x200, 0x90, 16);
    pf[0] = 0x90;
    CHECK(tr_setup() == -4 && !g_tr_on, "diag_trades: parallel-for prologue differs -> off");
    pf[0] = 0x48;
    tr_fail_hook_trades = 1;
    CHECK(tr_setup() == -6 && !g_tr_on && tr_unhooked == 1, "diag_trades: second hook fails -> first unhooked, off");
    tr_fail_hook_trades = 0;
    CHECK(tr_setup() == 0 && g_tr_on && g_tr_fn == trcode + 0x1000 && g_tr_pfor == pf, "diag_trades: anchors found, hooks on");
    text_lo = slo; text_hi = shi;

    tr_reset();
    hk_pfor((int32_t[8]){ 0, 0, 0, 0, 5, 0, 0, 0 }, 0, 0, 0, 0, 0, 0, 0);
    CHECK(tr.other_calls == 1 && tr.runs == 0, "parallel-for outside the task counted separately");
    CHECK(hk_trades(0, 0, 0, 0, 0, 0, 0, 0) == 99, "UpdateStateTrades detour passes the return value");
    double q = 1000.0 / qpc_freq;
    CHECK(tr.runs == 1 && tr.iters == 3 && tr.recs == 170 && tr.n_max == 100 && tr.hist[2] == 1 &&
          tr.last_n0 == 100 && tr.last_nlast == 10, "run: 3 iterations, 170 records, 100 -> 10");
    CHECK(tr.par_qpc * q >= 14 && tr.iter_qpc_max * q >= 14 && tr.run_qpc_max * q >= 44, "timings: parallel %.1f ms, longest it %.1f ms, run %.1f ms",
          tr.par_qpc * q, tr.iter_qpc_max * q, tr.run_qpc_max * q);
    tr_iters = 0; hk_trades(0, 0, 0, 0, 0, 0, 0, 0);
    CHECK(tr.runs == 2 && tr.hist[0] == 1 && tr.iters_max == 3, "zero-iteration run -> histogram bucket 0");
    tr_iters = 1; tr_ns[0] = 5; tr_sleep_apply = 1010;
    hk_trades(0, 0, 0, 0, 0, 0, 0, 0);
    CHECK(tr.slow_runs == 1 && strstr(logbuf, "slow UpdateStateTrades") != 0, "slow run (>= 1 s) logged");
    tr_sleep_apply = 10;

    cfg.diag = 0;
    size_t before = logn;
    cmd_simopt("", 0);
    CHECK(strstr(logbuf + before, "diag_trades: 3 UpdateStateTrades runs") && strstr(logbuf + before, "histogram 0:1 1:1 2-3:1") &&
          strstr(logbuf + before, "outside the task"), "console report");
    CHECK(!strstr(logbuf + before, "task (sorted by Execute ms)"), "no empty task table when only diag_trades is on");
    char csvp[MAX_PATH]; snprintf(csvp, sizeof csvp, "%ssim_opt_diag.csv", g_dir);
    FILE *f = fopen(csvp, "r"); char csv[4096] = {0}; if (f) { fread(csv, 1, sizeof csv - 1, f); fclose(f); }
    CHECK(strstr(csv, "trades_metric,value") && strstr(csv, "\nruns,3") && strstr(csv, "\niterations_total,4") &&
          strstr(csv, "\niterations_2-3,1"), "csv trades rows:\n%s", csv);
    DeleteFileA(csvp);
    before = logn;
    V3X_LoadTransition t = { "Game", "Empty", 1.0 };
    on_transition("load_transition", &t, 0);
    CHECK(strstr(logbuf + before, "diag_trades: 3 UpdateStateTrades runs") != 0, "report on Game->Empty");
    DeleteFileA(csvp);
    cmd_simopt("reset", 0);
    CHECK(tr.runs == 0 && tr.iters == 0, "reset clears diag_trades");
}

int main(void)
{
    GetModuleFileNameA(0, g_dir, MAX_PATH);
    char *sl = strrchr(g_dir, '\\'); if (!sl) sl = strrchr(g_dir, '/'); if (sl) sl[1] = 0;
    static V3X_API A;
    A.api_version = 3; A.struct_size = sizeof A; A.log = f_log; A.print = f_print; A.v3x_dir = f_dir;
    A.find_string = f_find_string; A.find_xref = f_find_xref; A.function_start = f_function_start; A.patch = f_patch;
    A.on_event = f_on_event; A.emit_event = f_emit_event;
    A.hook = f_hook; A.unhook = f_unhook;
    api = &A; has_v2 = 1;
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); qpc_freq = (double)fq.QuadPart;
    g_main_tid = GetCurrentThreadId();

    /* ---- anchors */
    CHECK(check_runtask() != 0, "fails closed on an empty RunTask");
    put(0x76, SHAPE_ZERO); put(0xd3, SHAPE_EXEC);
    CHECK(check_runtask() != 0, "fails closed when the empty-step exit is missing");
    put(0x103, SHAPE_EMPTY);
    CHECK(check_runtask() == 0, "anchor shapes accepted");
    g_xrefs = 2;
    CHECK(check_runtask() != 0, "fails closed on >1 xref to 'Context %%d'");
    g_xrefs = 1;

    /* ---- vtables: 0..55 normal, 56 bad slot 6 (outside text), 57 bad slot 3 */
    static const char *names[] = { "CTickTask_HandleAutonomousInvestment", "CTickTask_UpdateStateTrades",
                                   "CTickTask_UpdateAI", "CTickTask_RecalculateModifierNodes" };
    exec_fn bodies[] = { ex_parallel, ex_serial, ex_single, ex_single };
    text_lo = (uintptr_t)ex_parallel < (uintptr_t)slot_common ? (uintptr_t)ex_parallel : (uintptr_t)slot_common;
    text_lo -= 0x100000; text_hi = text_lo + 0x10000000;
    for (int i = 0; i < 58; i++) {
        for (int k = 0; k < 8; k++) vts[i].slot[k] = (uintptr_t)slot_common;
        vts[i].slot[6] = (uintptr_t)(i < 4 ? bodies[i] : ex_single);
        objs[i].vt = vts[i].slot;
        char nm[64];
        if (i < 4) snprintf(nm, sizeof nm, "%s", names[i]); else snprintf(nm, sizeof nm, "CTickTask_Filler%d", i);
        collect(nm, vts[i].slot, 0);
    }
    vts[56].slot[6] = 16;                       /* not code */
    vts[57].slot[3] = (uintptr_t)slot_other;    /* foreign layout */
    int np = patch_all();
    CHECK(np == 56 && g_nt == 56, "patched 56, skipped the 2 malformed vtables (%d, nt %d)", np, g_nt);
    CHECK(vts[0].slot[6] == (uintptr_t)ex_detour && vts[56].slot[6] == 16 && vts[57].slot[6] == (uintptr_t)ex_single,
          "only valid vtables got the detour");

    /* ---- diag */
    cfg.diag = 1;
    unsigned char step[0xc0];
    run_task(0, step);
    CHECK(n_par == 1 && (*(uint32_t *)step & 1) && *(int32_t *)(step + STEP_END) == 100, "detour calls the original Execute");
    run_task(1, step); run_task(2, step); run_task(2, step);
    Task *hai = task_of(vts[0].slot), *str = task_of(vts[1].slot), *ai = task_of(vts[2].slot);
    CHECK(hai && hai->kind[K_PARALLEL] == 1 && hai->range_sum == 100, "HAI classified parallel, range 100");
    CHECK(str && str->kind[K_SERIAL] == 1 && str->range_max == 10, "StateTrades classified serial-loop, range 10");
    CHECK(ai && ai->kind[K_SINGLE] == 2 && ai->on_main == 2, "UpdateAI classified single, on main thread");

    /* ---- throttle */
    cfg.throttle = 1;
    int nthr = apply_throttle_list("HandleAutonomousInvestment:3, RecalculateModifierNodes:3,UpdateAI:1,Nope:5,UpdateStateTrades:4");
    CHECK(nthr == 2, "2 tasks throttled (deny list, bad N and unknown name refused): %d", nthr);
    CHECK(strstr(logbuf, "RecalculateModifierNodes': refused") && strstr(logbuf, "UpdateAI': N=1") &&
          strstr(logbuf, "'Nope': no such") && strstr(logbuf, "even N"), "refusal/warning log lines");
    g_throttle_on = 1;
    n_par = 0;
    int skipped_pattern[9], consecutive = 0, prev = 0;
    for (int i = 0; i < 9; i++) {
        LONG before = n_par;
        run_task(0, step);
        int skipped = n_par == before;
        skipped_pattern[i] = skipped;
        if (skipped) CHECK(*(void **)(step + STEP_RANGE_IMPL) == 0 && *(void **)(step + STEP_SINGLE_IMPL) == 0 && *(uint32_t *)step == 0,
                           "skipped run leaves the Step empty (run %d)", i);
        if (skipped && prev) consecutive = 1;
        prev = skipped;
    }
    CHECK(n_par == 6 && hai->skipped == 3 && !consecutive, "1 of 3 skipped (ran %ld, skipped %lld)", n_par, (long long)hai->skipped);
    CHECK(skipped_pattern[2] && skipped_pattern[5] && skipped_pattern[8], "skip on every 3rd run");
    n_single = 0;
    for (int i = 0; i < 5; i++) run_task(3, step);
    CHECK(n_single == 5, "denied task never skipped");
    cmd_simopt("throttle off", 0);
    LONG before = n_par;
    for (int i = 0; i < 6; i++) run_task(0, step);
    CHECK(n_par - before == 6 && !g_throttle_on, "console 'throttle off' stops skipping");

    /* ---- multithreaded hammering: counters stay consistent */
    cmd_simopt("throttle on", 0);
    hai->runs = hai->skipped = 0;
    HANDLE th[4];
    DWORD WINAPI worker(LPVOID p);
    for (int i = 0; i < 4; i++) th[i] = CreateThread(0, 0, worker, 0, 0, 0);
    WaitForMultipleObjects(4, th, TRUE, INFINITE);
    CHECK(hai->runs + hai->skipped == 40000, "threaded: runs+skipped = %lld", (long long)(hai->runs + hai->skipped));
    CHECK(hai->skipped >= 13000 && hai->skipped <= 13400, "threaded: ~1/3 skipped (%lld)", (long long)hai->skipped);

    /* ---- report + csv */
    cmd_simopt("", 0);
    char csvp[MAX_PATH]; snprintf(csvp, sizeof csvp, "%ssim_opt_diag.csv", g_dir);
    FILE *f = fopen(csvp, "r"); char csv[4096] = {0}; if (f) { fread(csv, 1, sizeof csv - 1, f); fclose(f); }
    CHECK(strstr(csv, "task,runs,skipped") && strstr(csv, "\nHandleAutonomousInvestment,") && strstr(csv, ",parallel,") &&
          strstr(csv, "\nUpdateStateTrades,1,0,") && strstr(csv, "serial-loop"), "csv content:\n%s", csv);
    CHECK(strstr(logbuf, "task (sorted by Execute ms)") != 0, "console table");
    DeleteFileA(csvp);
    cmd_simopt("reset", 0);
    CHECK(hai->runs == 0 && str->kind[K_SERIAL] == 0, "reset");
    trades_tests();
    journal_tests();
    passthrough_tests();
    printf("--- log ---\n%s", logbuf);
    printf("%s\n", fails ? "FAILED" : "ALL OK");
    return fails;
}

DWORD WINAPI worker(LPVOID p)
{
    (void)p;
    unsigned char step[0xc0];
    for (int i = 0; i < 10000; i++) run_task(0, step);
    return 0;
}
