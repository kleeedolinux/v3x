/* tick_par: fork/join analysis of the simulation tick (victoria3.exe 1.15.1). Read-only.
 *
 * How the engine schedules (docs/re/tick_par.md, static RE):
 *   tick-task runner 0x14736c0 ("Tick Tasks for {}") calls the due-list builder 0x146bce0 once per tick,
 *   which filters the GLOBAL ORDER returned by 0x2fd5e70 by rate (type+0x28) and maps each type to its
 *   instance; then the runner calls RunTask 0x146d010 for each item, strictly one after another
 *   (call site 0x1473c78, rcx = runner ctx, rdx = task instance). The global order is computed once:
 *   stable sort by (priority type+0x2c, name type+0x08), then a DFS that emits each task's
 *   dependencies first (0x2fd5ca0 over type+0x30 = array of 0x20-byte dependency NAME strings, count
 *   +0x3c). Parallelism exists only inside a task (range stages).
 *
 * mode=measure (default): hooks the builder (MinHook) and redirects the runner's RunTask call site
 *   through a near jump stub (so it chains with tick_profiler, which inline-hooks RunTask itself).
 *   Per tick: due list, per-task time, then it simulates the fork/join policy of tick_par_sched.h
 *   (one extra lane, engine-declared independence only, join before any dependent task and at the
 *   end) with the measured times:
 *     all    = every task may be offloaded / co-run: an UPPER BOUND, ignores undeclared data sharing
 *     vetted = only tasks vetted as concurrency-safe (none in this build, see below)
 *     crit   = unlimited lanes, longest declared dependency chain (lower bound of the tick)
 *   Report: console "tickpar", on Game->Empty, binaries/v3x/tick_par_report.csv; the runtime
 *   dependency graph read from the live type objects goes to binaries/v3x/tick_par_graph.csv once.
 * mode=on: REFUSED (runs as measure). The engine's dependencies are ordering constraints for a
 *   serial runner, not a thread-safety contract. Every RunTask writes unsynchronised globals
 *   (profiler scope 0x5436898 at 0x146d0d7, RenderFrameIfNeeded guard 0x1468a60/0x1468be0 global
 *   strings and frame rendering), the script RNG has ONE global owner (*0x540ae80, thread id at +0x10,
 *   non-atomic counter at 0x3787398) and a parallel range swaps that owner out globally (0x146ca14).
 *   So no pair of tasks can be run concurrently through RunTask without races; the vetted list is
 *   empty and the concurrent executor is not shipped (fail closed).
 * mode=off or enabled=0 or env V3X_NO_TICKPAR=1: nothing is installed.
 *
 * Fails closed (nothing installed, logged): any anchor below missing, call-site target is not RunTask,
 * near stub allocation fails. At runtime the measurement stops (pass-through only) if the registry
 * cannot be read, a dependency cycle is found, or the due list is not in topological order. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "v3x.h"
#include "tick_par_sched.h"

V3X_EXPORT const char *v3x_plugin_name = "tick_par";

#define TAG "tick_par"
#define NAMELEN 64

/* anchors (1.15.1) */
#define S_RUNNER   "Tick Tasks for {}"
#define S_SORTED   "Two tick tasks with the same name, not allowed!"
#define S_RUNTASK  "Context %d"
#define P_RUNNER_PRO  "48 8B C4 48 89 50 10 55 53 56 57"
#define P_BUILD_CALL  "4C 8B C3 48 8D 55 F0 49 8B CD E8"          /* mov r8,rbx; lea rdx,[rbp-0x10]; mov rcx,r13; call builder */
#define P_RUN_CALL    "49 8B D7 49 8B CD E8"                      /* mov rdx,r15; mov rcx,r13; call RunTask */
#define P_INST_TYPE   "49 8B 57 08 48 8B 4D 18 E8"                /* mov rdx,[r15+8] (instance -> type) */
#define P_BUILD_PRO   "48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 55 41 56 48 83 EC 30"
#define P_BUILD_SORT  "49 8B F0 48 8B FA 4C 8B E9 E8"             /* ... call 0x2fd5e70 (sorted registry) */
#define P_BUILD_RATE  "8B 43 28 83 F8 01"                         /* mov eax,[rbx+0x28] (rate); cmp eax,1 */
#define P_TOPO_VISIT  "4C 89 44 24 18 48 89 4C 24 08 53 56 57 41 57 48 83 EC 38 48 8B 71 30 4D 8B F8 48 63 79 3C"
#define RUNNER_SCAN 0xd00
#define BUILD_SCAN  0x80

/* type layout (CJominiTickTaskType) */
#define T_NAME 0x08
#define T_RATE 0x28
#define T_PRIO 0x2c
#define T_DEPS 0x30
#define T_NDEPS 0x3c
#define I_TYPE 0x08     /* task instance -> type */

#define PASS8_ARGS void *a0_, void *a1_, void *a2_, void *a3_, void *a4_, void *a5_, void *a6_, void *a7_
#define PASS8 a0_, a1_, a2_, a3_, a4_, a5_, a6_, a7_
typedef uint64_t (*pass8_fn)(PASS8_ARGS);
typedef void *(*sorted_fn)(void);

static const V3X_API *api;
static int has_v2, has_v3;
static uintptr_t img_lo, img_hi;
static double qpc_freq;

static struct { int enabled; char mode[16]; } cfg;
enum { M_OFF, M_MEASURE, M_ON };
static int g_mode;

static pass8_fn orig_builder, orig_run;
static sorted_fn g_sorted;
static unsigned char *g_callsite;   /* address of the E8 of the RunTask call in the runner */
static unsigned char *g_stub;

/* ---------------------------------------------------------------- registry / graph */
typedef struct {
    void *type;
    char name[NAMELEN];
    int rate, prio, ndeps, unresolved;
    LONG64 runs, qpc, offloads, overlap_qpc;
    int partner;                 /* registry id of the last main-lane task that co-ran (all policy) */
} Node;

static Node g_node[TP_MAXN];
static int g_nn;
static TpGraph g_graph;
static unsigned char g_cand_all[TP_MAXN], g_corun_all[TP_MAXN], g_cand_vet[TP_MAXN], g_corun_vet[TP_MAXN];
static int g_graph_state;           /* 0 not built, 1 ok, -1 failed (measurement off) */
static int g_edges;
static int g_from[TP_MAXN * 16], g_to[TP_MAXN * 16];   /* direct edges: g_from[e] depends on g_to[e] */

static void *g_hmap_k[1024]; static int g_hmap_v[1024];
static unsigned hidx(void *p) { return (unsigned)((((uint64_t)(uintptr_t)p >> 3) * 0x9E3779B97F4A7C15ull) >> 54) & 1023; }
static int node_of(void *type)
{
    unsigned h = hidx(type);
    for (int i = 0; i < 1024; i++, h = (h + 1) & 1023) {
        if (!g_hmap_k[h]) return -1;
        if (g_hmap_k[h] == type) return g_hmap_v[h];
    }
    return -1;
}
static void node_put(void *type, int v)
{
    unsigned h = hidx(type);
    for (int i = 0; i < 1024; i++, h = (h + 1) & 1023)
        if (!g_hmap_k[h]) { g_hmap_k[h] = type; g_hmap_v[h] = v; return; }
}

/* fault-proof read (graph build / validation only) */
static int rd(const void *addr, void *out, size_t n)
{
    SIZE_T got = 0;
    return addr && ReadProcessMemory(GetCurrentProcess(), addr, out, n, &got) && got == n;
}

/* MSVC std::string {buf/ptr[16], size, cap}; names are [A-Za-z0-9_] */
static int read_str(const void *s, char *out, size_t n)
{
    unsigned char b[32];
    if (!rd(s, b, 32)) return 0;
    uint64_t size = *(uint64_t *)(b + 16), cap = *(uint64_t *)(b + 24);
    if (!size || size >= n || cap < size) return 0;
    if (cap > 15) { if (!rd(*(void **)b, out, (size_t)size)) return 0; }
    else memcpy(out, b, (size_t)size);
    out[size] = 0;
    for (uint64_t i = 0; i < size; i++) {
        char c = out[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return 0;
    }
    return 1;
}

static void outf(const char *fmt, ...)
{
    char buf[600];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (has_v2) api->print("%s", buf); else api->log(TAG, "%s", buf);
}

static const char *RATE_N[] = { "TICK", "DAY", "WEEK", "MONTH", "QUARTER", "YEAR" };
static const char *PRIO_N[] = { "EARLY", "DEFAULT", "LATE" };

static void write_graph_csv(void)
{
    char path[MAX_PATH], line[2048];
    snprintf(path, sizeof path, "%stick_par_graph.csv", api->v3x_dir());
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { api->log(TAG, "cannot write %s", path); return; }
    DWORD w;
    int k = snprintf(line, sizeof line, "order,task,rate,prio,deps,unresolved_deps\r\n");
    WriteFile(h, line, (DWORD)k, &w, 0);
    for (int i = 0; i < g_nn; i++) {
        const Node *nd = &g_node[i];
        k = snprintf(line, sizeof line, "%d,%s,%s,%s,", i, nd->name, nd->rate >= 0 && nd->rate < 6 ? RATE_N[nd->rate] : "?",
                     nd->prio >= 0 && nd->prio < 3 ? PRIO_N[nd->prio] : "?");
        int first = 1;
        for (int e = 0; e < g_edges && k < (int)sizeof line - 80; e++)
            if (g_from[e] == i) { k += snprintf(line + k, sizeof line - k, "%s%s", first ? "" : " ", g_node[g_to[e]].name); first = 0; }
        k += snprintf(line + k, sizeof line - k, ",%d\r\n", nd->unresolved);
        if (k > 0 && k < (int)sizeof line) WriteFile(h, line, (DWORD)k, &w, 0);
    }
    CloseHandle(h);
    api->log(TAG, "runtime dependency graph (%d tasks, %d declared edges, deps = direct) -> %s", g_nn, g_edges, path);
}

/* the vetted list: tasks proven safe to co-run. Empty in this build (see header). */
static const char *const VETTED[] = { 0 };

/* Build the registry snapshot from the engine's own sorted list (called on the tick thread). */
static int build_graph(void)
{
    unsigned char arr[16];
    void *reg = g_sorted ? g_sorted() : 0;
    if (!reg || !rd(reg, arr, 16)) { api->log(TAG, "registry: sorted list unreadable"); return -1; }
    void **data = *(void ***)arr; int cnt = *(int32_t *)(arr + 12);
    if (cnt <= 0 || cnt > TP_MAXN) { api->log(TAG, "registry: %d tasks (limit %d)", cnt, TP_MAXN); return -1; }
    static void *types[TP_MAXN];
    if (!rd(data, types, sizeof(void *) * (size_t)cnt)) { api->log(TAG, "registry: array unreadable"); return -1; }
    g_nn = 0;
    for (int i = 0; i < cnt; i++) {
        Node *nd = &g_node[i];
        memset(nd, 0, sizeof *nd);
        nd->type = types[i]; nd->partner = -1;
        unsigned char hdr[0x40];
        if ((uintptr_t)types[i] < img_lo || (uintptr_t)types[i] >= img_hi || !rd(types[i], hdr, sizeof hdr) ||
            !read_str((char *)types[i] + T_NAME, nd->name, NAMELEN)) {
            api->log(TAG, "registry: type %d at %p unreadable", i, types[i]); return -1;
        }
        nd->rate = *(int32_t *)(hdr + T_RATE); nd->prio = *(int32_t *)(hdr + T_PRIO);
        nd->ndeps = *(int32_t *)(hdr + T_NDEPS);
        if (nd->ndeps < 0 || nd->ndeps > 64) { api->log(TAG, "registry: %s has %d deps", nd->name, nd->ndeps); return -1; }
        node_put(types[i], i);
    }
    g_nn = cnt;
    int *from = g_from, *to = g_to;
    int ne = 0;
    for (int i = 0; i < cnt; i++) {
        void *dp = 0;
        rd((char *)types[i] + T_DEPS, &dp, sizeof dp);
        for (int d = 0; d < g_node[i].ndeps; d++) {
            char dn[NAMELEN];
            if (!read_str((char *)dp + 0x20 * d, dn, sizeof dn)) { api->log(TAG, "registry: dep %d of %s unreadable", d, g_node[i].name); return -1; }
            int j = -1;
            for (int k = 0; k < cnt; k++) if (!strcmp(g_node[k].name, dn)) { j = k; break; }
            if (j < 0 || j == i) { g_node[i].unresolved++; continue; }   /* the engine ignores unknown names too */
            if (ne >= TP_MAXN * 16) return -1;
            from[ne] = i; to[ne] = j; ne++;
        }
    }
    g_edges = ne;
    int rc = tp_closure(&g_graph, cnt, from, to, ne);
    if (rc) { api->log(TAG, "registry: dependency closure failed (%d%s)", rc, rc == -2 ? ", cycle" : ""); return -1; }
    for (int i = 0; i < cnt; i++) {
        g_cand_all[i] = g_corun_all[i] = 1;
        g_cand_vet[i] = g_corun_vet[i] = 0;
        for (int v = 0; VETTED[v]; v++) if (!strcmp(VETTED[v], g_node[i].name)) g_cand_vet[i] = g_corun_vet[i] = 1;
    }
    write_graph_csv();
    return 1;
}

/* ---------------------------------------------------------------- per tick */
#define NB 4   /* due-set class: highest rate due = TICK, DAY, WEEK, MONTH+ */
static const char *BUCKET_N[NB] = { "TICK", "DAY", "WEEK", "MONTH+" };
static struct {
    LONG64 ticks, serial, all2, vet2, crit, serial_max;
} g_b[NB];
static LONG64 g_ticks_bad, g_order_bad, g_hook_ticks;
static SRWLOCK g_lock = SRWLOCK_INIT;

static struct {
    int n, pos, valid;
    void *inst[TP_MAXN];
    int reg[TP_MAXN];
    LONG64 q[TP_MAXN];
} T;
static double g_dur[TP_MAXN], g_saved[TP_MAXN], g_scr[TP_MAXN];

static void finish_tick(void)
{
    int n = T.n;
    if (!T.valid || n <= 0) return;
    int top = 0; LONG64 ser = 0;
    for (int k = 0; k < n; k++) {
        g_dur[k] = (double)T.q[k];
        ser += T.q[k];
        int r = g_node[T.reg[k]].rate;
        if (r > top) top = r;
    }
    if (top >= NB) top = NB - 1;
    double all2 = tp_simulate(&g_graph, T.reg, g_dur, n, g_cand_all, g_corun_all, g_saved);
    double vet2 = tp_simulate(&g_graph, T.reg, g_dur, n, g_cand_vet, g_corun_vet, 0);
    double crit = tp_critical_path(&g_graph, T.reg, g_dur, n, g_scr);
    AcquireSRWLockExclusive(&g_lock);
    g_b[top].ticks++; g_b[top].serial += ser; g_b[top].all2 += (LONG64)all2; g_b[top].vet2 += (LONG64)vet2;
    g_b[top].crit += (LONG64)crit;
    if (ser > g_b[top].serial_max) g_b[top].serial_max = ser;
    for (int k = 0; k < n; k++) {
        Node *nd = &g_node[T.reg[k]];
        nd->runs++; nd->qpc += T.q[k];
        if (g_saved[k] > 0) {
            nd->offloads++; nd->overlap_qpc += (LONG64)g_saved[k];
            if (k + 1 < n) nd->partner = T.reg[k + 1];
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/* due-list builder detour: rcx = runner ctx, rdx = out CPdxArray<instance*> {data, cap, count@+0xc} */
static uint64_t hk_builder(PASS8_ARGS)
{
    uint64_t r = orig_builder(PASS8);
    if (g_graph_state < 0) return r;
    if (g_graph_state == 0) g_graph_state = build_graph();
    if (g_graph_state < 0) { api->log(TAG, "measurement off (pass-through only)"); return r; }
    g_hook_ticks++;
    if (T.n && T.pos != T.n) g_ticks_bad++;           /* previous tick did not finish through our call site */
    T.n = 0; T.pos = 0; T.valid = 0;
    const unsigned char *out = (const unsigned char *)a1_;
    void **data = *(void ***)out; int n = *(const int32_t *)(out + 12);
    if (n <= 0) return r;
    if (n > TP_MAXN) { g_ticks_bad++; return r; }
    for (int k = 0; k < n; k++) {
        void *inst = data[k];
        void *type = inst ? *(void **)((char *)inst + I_TYPE) : 0;   /* the runner dereferences both right away */
        int id = type ? node_of(type) : -1;
        if (id < 0) { g_ticks_bad++; return r; }
        T.inst[k] = inst; T.reg[k] = id; T.q[k] = 0;
    }
    if (!tp_order_ok(&g_graph, T.reg, n)) {   /* never expected: the engine order is topological */
        g_order_bad++; g_ticks_bad++;
        return r;
    }
    T.n = n; T.valid = 1;
    return r;
}

/* RunTask call-site detour: rcx = runner ctx, rdx = task instance (2 args; PASS8 forwards 8) */
static uint64_t hk_run(PASS8_ARGS)
{
    if (!T.valid || T.pos >= T.n || T.inst[T.pos] != a1_) {
        if (T.valid) { T.valid = 0; g_ticks_bad++; }
        return orig_run(PASS8);
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    uint64_t r = orig_run(PASS8);
    QueryPerformanceCounter(&t1);
    T.q[T.pos++] = t1.QuadPart - t0.QuadPart;
    if (T.pos == T.n) finish_tick();
    return r;
}

/* ---------------------------------------------------------------- report */
typedef struct { int id; LONG64 v; } Row;
static int cmp_row(const void *a, const void *b)
{
    LONG64 x = ((const Row *)a)->v, y = ((const Row *)b)->v;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void report(const char *why)
{
    double ms = 1000.0 / qpc_freq;
    static Row rows[TP_MAXN];
    int nr = 0;
    AcquireSRWLockShared(&g_lock);
    outf("tick_par (%s): mode=%s (on is refused: no concurrency-safe task pairs, see docs/re/tick_par.md); graph %s, %d tasks, %d edges; "
         "%lld ticks hooked, %lld not measured, %lld order violations",
         why, g_mode == M_ON ? "on->measure" : "measure", g_graph_state > 0 ? "ok" : g_graph_state < 0 ? "FAILED" : "not built",
         g_nn, g_edges, (long long)g_hook_ticks, (long long)g_ticks_bad, (long long)g_order_bad);
    outf("  %-7s %7s %10s %10s %10s %10s %9s %9s", "dueset", "ticks", "serial ms", "all-2lane", "vetted", "critpath", "save all", "max ms");
    for (int b = 0; b < NB; b++) {
        if (!g_b[b].ticks) continue;
        double t = (double)g_b[b].ticks;
        outf("  %-7s %7lld %10.2f %10.2f %10.2f %10.2f %8.1f%% %9.1f", BUCKET_N[b], (long long)g_b[b].ticks, g_b[b].serial * ms / t,
             g_b[b].all2 * ms / t, g_b[b].vet2 * ms / t, g_b[b].crit * ms / t,
             g_b[b].serial ? 100.0 * (g_b[b].serial - g_b[b].all2) / g_b[b].serial : 0.0, g_b[b].serial_max * ms);
    }
    for (int i = 0; i < g_nn; i++) if (g_node[i].overlap_qpc) { rows[nr].id = i; rows[nr].v = g_node[i].overlap_qpc; nr++; }
    qsort(rows, nr, sizeof rows[0], cmp_row);
    outf("  upper bound (all tasks treated as safe): tasks that would run on the 2nd lane, by overlapped time");
    outf("  %-44s %8s %10s %10s  %s", "task", "offloads", "overlap ms", "task ms", "co-runs with (last)");
    for (int i = 0; i < nr && i < 15; i++) {
        const Node *nd = &g_node[rows[i].id];
        outf("  %-44.44s %8lld %10.1f %10.1f  %s", nd->name, (long long)nd->offloads, nd->overlap_qpc * ms, nd->qpc * ms,
             nd->partner >= 0 ? g_node[nd->partner].name : "-");
    }
    ReleaseSRWLockShared(&g_lock);

    char path[MAX_PATH], line[512];
    snprintf(path, sizeof path, "%stick_par_report.csv", api->v3x_dir());
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { api->log(TAG, "cannot write %s", path); return; }
    DWORD w; int k;
    AcquireSRWLockShared(&g_lock);
    k = snprintf(line, sizeof line, "dueset,ticks,serial_ms_avg,all_2lane_ms_avg,vetted_ms_avg,critpath_ms_avg,serial_ms_max\r\n");
    WriteFile(h, line, (DWORD)k, &w, 0);
    for (int b = 0; b < NB; b++) {
        if (!g_b[b].ticks) continue;
        double t = (double)g_b[b].ticks;
        k = snprintf(line, sizeof line, "%s,%lld,%.3f,%.3f,%.3f,%.3f,%.3f\r\n", BUCKET_N[b], (long long)g_b[b].ticks, g_b[b].serial * ms / t,
                     g_b[b].all2 * ms / t, g_b[b].vet2 * ms / t, g_b[b].crit * ms / t, g_b[b].serial_max * ms);
        WriteFile(h, line, (DWORD)k, &w, 0);
    }
    k = snprintf(line, sizeof line, "\r\ntask,rate,prio,runs,total_ms,offloads_all,overlap_ms_all,last_partner\r\n");
    WriteFile(h, line, (DWORD)k, &w, 0);
    for (int i = 0; i < g_nn; i++) {
        const Node *nd = &g_node[i];
        if (!nd->runs) continue;
        k = snprintf(line, sizeof line, "%s,%s,%s,%lld,%.3f,%lld,%.3f,%s\r\n", nd->name, nd->rate >= 0 && nd->rate < 6 ? RATE_N[nd->rate] : "?",
                     nd->prio >= 0 && nd->prio < 3 ? PRIO_N[nd->prio] : "?", (long long)nd->runs, nd->qpc * ms,
                     (long long)nd->offloads, nd->overlap_qpc * ms, nd->partner >= 0 ? g_node[nd->partner].name : "");
        if (k > 0 && k < (int)sizeof line) WriteFile(h, line, (DWORD)k, &w, 0);
    }
    ReleaseSRWLockShared(&g_lock);
    CloseHandle(h);
}

static void reset_all(void)
{
    AcquireSRWLockExclusive(&g_lock);
    memset(g_b, 0, sizeof g_b);
    g_ticks_bad = g_order_bad = g_hook_ticks = 0;
    for (int i = 0; i < g_nn; i++) { Node *nd = &g_node[i]; nd->runs = nd->qpc = nd->offloads = nd->overlap_qpc = 0; nd->partner = -1; }
    ReleaseSRWLockExclusive(&g_lock);
}

static void cmd_tickpar(const char *args, void *user)
{
    (void)user;
    if (!strncmp(args, "reset", 5)) { reset_all(); outf("tick_par: counters reset"); return; }
    report("console");
}

static void on_transition(const char *name, void *data, void *user)
{
    (void)name; (void)user;
    const V3X_LoadTransition *t = (const V3X_LoadTransition *)data;
    if (t && t->from && t->to && !strcmp(t->from, "Game") && !strcmp(t->to, "Empty")) report("Game->Empty");
}

/* ---------------------------------------------------------------- anchors */
static int parse_pat(const char *pat, unsigned char *b, int *wild, int max)
{
    int m = 0;
    for (const char *p = pat; *p && m < max;) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (p[0] == '?') { wild[m] = 1; b[m++] = 0; p += 2; continue; }
        wild[m] = 0; b[m++] = (unsigned char)strtoul(p, 0, 16); p += 2;
    }
    return m;
}

/* offset of the only match of pat in [p, p+n), -1 none, -2 more than one */
static long find_once(const unsigned char *p, size_t n, const char *pat)
{
    unsigned char b[64]; int wild[64];
    int m = parse_pat(pat, b, wild, 64);
    long hit = -1;
    for (size_t i = 0; i + (size_t)m <= n; i++) {
        int ok = 1;
        for (int j = 0; j < m && ok; j++) ok = wild[j] || p[i + j] == b[j];
        if (ok) { if (hit >= 0) return -2; hit = (long)i; }
    }
    return hit;
}

static int match_at(const unsigned char *p, const char *pat)
{
    unsigned char b[64]; int wild[64];
    int m = parse_pat(pat, b, wild, 64);
    for (int j = 0; j < m; j++) if (!wild[j] && p[j] != b[j]) return 0;
    return 1;
}

static unsigned char *rel_target(unsigned char *e8) { return e8 + 5 + *(int32_t *)(e8 + 1); }

static unsigned char *fn_of_string(const char *s)
{
    void *str = api->find_string(s);
    if (!str) return 0;
    void *x = api->find_xref(str, 0);
    if (!x || api->find_xref(str, 1)) return 0;     /* exactly one reference */
    return (unsigned char *)api->function_start(x);
}

/* jump stub within +-2 GB of `site` (rel32 reach), 14-byte absolute jmp to `to` */
static unsigned char *alloc_stub(unsigned char *site, void *to)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t g = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    uintptr_t base = (uintptr_t)site & ~(g - 1);
    for (uintptr_t d = g; d < 0x70000000; d += g) {
        for (int s = 0; s < 2; s++) {
            uintptr_t a = s ? base + d : base - d;
            if (!s && base < d) continue;
            unsigned char *p = (unsigned char *)VirtualAlloc((void *)a, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!p) continue;
            int64_t dist = (int64_t)((uintptr_t)p - (uintptr_t)site);
            if (dist > 0x7fff0000ll || dist < -0x7fff0000ll) { VirtualFree(p, 0, MEM_RELEASE); continue; }
            p[0] = 0xFF; p[1] = 0x25; memset(p + 2, 0, 4);
            memcpy(p + 6, &to, 8);
            DWORD old;
            if (!VirtualProtect(p, 4096, PAGE_EXECUTE_READ, &old)) { VirtualFree(p, 0, MEM_RELEASE); return 0; }
            FlushInstructionCache(GetCurrentProcess(), p, 16);
            return p;
        }
    }
    return 0;
}

/* returns 0 or a fail-closed code; on success the hooks are live */
static int install(void)
{
    unsigned char *runner = fn_of_string(S_RUNNER);
    if (!runner || !match_at(runner, P_RUNNER_PRO)) { api->log(TAG, "anchor: runner (\"%s\") not found or prologue differs", S_RUNNER); return 2; }
    unsigned char *runtask = (unsigned char *)api->function_start(api->find_xref(api->find_string(S_RUNTASK), 0));
    if (!runtask) { api->log(TAG, "anchor: RunTask (\"%s\") not found", S_RUNTASK); return 3; }
    long ob = find_once(runner, RUNNER_SCAN, P_BUILD_CALL);
    long orr = find_once(runner, RUNNER_SCAN, P_RUN_CALL);
    long oi = find_once(runner, RUNNER_SCAN, P_INST_TYPE);
    if (ob < 0 || orr < 0 || oi < 0) { api->log(TAG, "anchor: runner shapes builder=%ld run=%ld inst=%ld", ob, orr, oi); return 4; }
    unsigned char *bcall = runner + ob + 10, *rcall = runner + orr + 6;
    unsigned char *builder = rel_target(bcall);
    if (rel_target(rcall) != runtask) { api->log(TAG, "anchor: runner's call does not target RunTask (already redirected?)"); return 5; }
    if (!match_at(builder, P_BUILD_PRO)) { api->log(TAG, "anchor: builder prologue differs"); return 6; }
    long os = find_once(builder, BUILD_SCAN, P_BUILD_SORT);
    long ort = find_once(builder, 0x100, P_BUILD_RATE);
    if (os < 0 || ort < 0) { api->log(TAG, "anchor: builder shapes sort=%ld rate=%ld", os, ort); return 7; }
    unsigned char *sorted = rel_target(builder + os + 9);
    if (fn_of_string(S_SORTED) != sorted) { api->log(TAG, "anchor: sorted-registry function does not own \"%s\"", S_SORTED); return 8; }
    if (!api->pattern_scan(P_TOPO_VISIT)) { api->log(TAG, "anchor: dependency DFS (type+0x30/+0x3c) not found"); return 9; }
    g_sorted = (sorted_fn)(void *)sorted;

    g_stub = alloc_stub(rcall, (void *)hk_run);
    if (!g_stub) { api->log(TAG, "no jump stub within 2 GB of the call site"); return 10; }
    orig_run = (pass8_fn)(void *)runtask;
    if (api->hook(builder, (void *)hk_builder, (void **)&orig_builder)) { api->log(TAG, "hook builder failed"); return 11; }
    int32_t rel = (int32_t)((intptr_t)g_stub - (intptr_t)(rcall + 5));
    if (api->patch(rcall + 1, &rel, 4)) { api->unhook(builder); api->log(TAG, "call-site patch failed"); return 12; }
    g_callsite = rcall;
    api->log(TAG, "installed: runner +0x%llx, builder +0x%llx, sorted +0x%llx, RunTask call site +0x%llx -> stub %p",
             (unsigned long long)((uintptr_t)runner - img_lo), (unsigned long long)((uintptr_t)builder - img_lo),
             (unsigned long long)((uintptr_t)sorted - img_lo), (unsigned long long)((uintptr_t)rcall - img_lo), (void *)g_stub);
    return 0;
}

static void load_cfg(void)
{
    char ini[MAX_PATH];
    snprintf(ini, sizeof ini, "%stick_par.ini", api->v3x_dir());
    cfg.enabled = GetPrivateProfileIntA("tick_par", "enabled", 1, ini);
    GetPrivateProfileStringA("tick_par", "mode", "measure", cfg.mode, sizeof cfg.mode, ini);
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    const char *off = getenv("V3X_NO_TICKPAR");
    if (off && off[0] == '1') { api->log(TAG, "disabled by V3X_NO_TICKPAR=1"); return 0; }
    load_cfg();
    g_mode = !_stricmp(cfg.mode, "off") ? M_OFF : !_stricmp(cfg.mode, "on") ? M_ON : !_stricmp(cfg.mode, "measure") ? M_MEASURE : -1;
    if (g_mode < 0) { api->log(TAG, "mode=%s unknown (off|measure|on), using measure", cfg.mode); g_mode = M_MEASURE; }
    if (!cfg.enabled || g_mode == M_OFF) { api->log(TAG, "inactive (enabled=%d mode=%s)", cfg.enabled, cfg.mode); return 0; }
    if (g_mode == M_ON)
        api->log(TAG, "mode=on REFUSED: engine dependencies are ordering-only and RunTask/RNG/render state is global "
                      "(docs/re/tick_par.md); no vetted task pairs in this build -> running measure (read-only)");
    has_v2 = api->api_version >= 2 && api->struct_size >= offsetof(V3X_API, exec_command) + sizeof(void *);
    has_v3 = api->api_version >= 3 && api->struct_size >= offsetof(V3X_API, on_event) + sizeof(void *);
    img_lo = api->image_base(); img_hi = img_lo + api->image_size();
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); qpc_freq = (double)fq.QuadPart;
    int rc = install();
    if (rc) { api->log(TAG, "not installed (fail closed, code %d)", rc); return rc; }
    if (has_v3) api->on_event("load_transition", on_transition, 0);
    if (has_v2) api->register_command("tickpar", "tick_par fork/join estimate; 'tickpar reset'", cmd_tickpar, 0);
    return 0;
}
