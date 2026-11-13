/* tick_par scheduling logic (pure C, no Windows, no game memory): shared by the plugin and
 * test/tick_par_test.c.
 *
 * Model (docs/re/tick_par.md): the engine runs the due tasks of a tick strictly one after another in
 * its own order (a DFS topological order of the declared dependency graph, computed once by
 * 0x2fd5e70). A task's declared dependencies are NAMES of other tasks; dependencies are transitive
 * through tasks that are not due this tick.
 *
 * The fork/join policy evaluated here (one extra lane, the main lane keeps the engine order):
 *   before due item k:  JOIN if an offloaded task is in flight and it has already finished, or
 *                         k transitively depends on it, or k is not allowed to co-run (at most one
 *                         task in flight);
 *   item k:             OFFLOAD if k is a candidate, nothing is in flight, k is not the last item and
 *                         item k+1 does not depend on k and may co-run (otherwise offloading only adds
 *                         a join); else run inline on the main lane (also while something is in flight);
 *   after the last item: JOIN (nothing may still run when the runner leaves its loop).
 * Because the engine order is topological, a task's dependencies always come before it, so this policy
 * never runs a task before (or concurrently with) anything it declares a dependency on. */
#ifndef TICK_PAR_SCHED_H
#define TICK_PAR_SCHED_H
#include <stdint.h>
#include <string.h>

#define TP_MAXN 512                       /* registry size limit (1.15.1 has 272) */
#define TP_W ((TP_MAXN + 63) / 64)

typedef struct { uint64_t b[TP_W]; } TpSet;

static inline void tp_set(TpSet *s, int i) { s->b[i >> 6] |= 1ull << (i & 63); }
static inline int tp_has(const TpSet *s, int i) { return (int)((s->b[i >> 6] >> (i & 63)) & 1); }

/* Transitive ancestors. n nodes, ne edges "from depends on to" (dep_from[e] needs dep_to[e] first).
 * Returns 0, -1 bad args, -2 dependency cycle (then the graph must not be used). */
typedef struct { int n; TpSet anc[TP_MAXN]; } TpGraph;

static int tp_closure(TpGraph *g, int n, const int *dep_from, const int *dep_to, int ne)
{
    if (!g || n < 0 || n > TP_MAXN || ne < 0) return -1;
    memset(g, 0, sizeof *g);
    g->n = n;
    static TpSet direct[TP_MAXN];
    static unsigned char state[TP_MAXN];     /* 0 new, 1 on stack, 2 done */
    static int stack[TP_MAXN], it[TP_MAXN];
    memset(direct, 0, sizeof(TpSet) * (size_t)n);
    memset(state, 0, (size_t)n);
    for (int e = 0; e < ne; e++) {
        int a = dep_from[e], b = dep_to[e];
        if (a < 0 || a >= n || b < 0 || b >= n) return -1;
        if (a == b) return -2;
        tp_set(&direct[a], b);
    }
    for (int root = 0; root < n; root++) {
        if (state[root]) continue;
        int sp = 0; stack[sp] = root; it[sp] = 0; state[root] = 1;
        while (sp >= 0) {
            int v = stack[sp], j = it[sp];
            for (; j < n; j++) if (tp_has(&direct[v], j)) break;
            if (j < n) {
                it[sp] = j + 1;
                if (state[j] == 1) return -2;
                if (state[j] == 0) { state[j] = 1; sp++; stack[sp] = j; it[sp] = 0; }
                continue;
            }
            /* all children done: anc(v) = direct(v) | anc(children) */
            for (int w = 0; w < TP_W; w++) g->anc[v].b[w] |= direct[v].b[w];
            for (int c = 0; c < n; c++)
                if (tp_has(&direct[v], c))
                    for (int w = 0; w < TP_W; w++) g->anc[v].b[w] |= g->anc[c].b[w];
            state[v] = 2; sp--;
        }
    }
    return 0;
}

/* does registry node a (transitively) depend on b */
static inline int tp_dep(const TpGraph *g, int a, int b) { return tp_has(&g->anc[a], b); }

enum { TP_JOIN_BEFORE = 1, TP_OFFLOAD = 2 };

typedef struct { int inflight; } TpState;   /* due index in flight or -1 */

static inline void tp_begin(TpState *s) { s->inflight = -1; }

/* Decision for due item k of n (reg[] = registry ids in engine order). cand / corun are per registry id;
 * inflight_done = the in-flight task (if any) has already finished (a join would not wait).
 * Returns TP_JOIN_BEFORE and/or TP_OFFLOAD; updates s. */
static int tp_decide(TpState *s, const TpGraph *g, const int *reg, int n, int k,
                     const unsigned char *cand, const unsigned char *corun, int inflight_done)
{
    int act = 0, r = reg[k];
    if (s->inflight >= 0) {
        int f = reg[s->inflight];
        if (inflight_done || tp_dep(g, r, f) || !corun[r]) { act |= TP_JOIN_BEFORE; s->inflight = -1; }
    }
    if (cand[r] && corun[r] && s->inflight < 0 && k + 1 < n) {
        int nx = reg[k + 1];
        if (!tp_dep(g, nx, r) && corun[nx]) { act |= TP_OFFLOAD; s->inflight = k; }
    }
    return act;
}

/* Simulate the policy with measured durations (any unit). Returns the makespan; *saved_by (per due
 * index, may be NULL) receives the overlapped time credited to each offloaded item. The final join
 * after the last item is included. */
static double tp_simulate(const TpGraph *g, const int *reg, const double *dur, int n,
                          const unsigned char *cand, const unsigned char *corun, double *saved_by)
{
    TpState s; tp_begin(&s);
    double main_t = 0, lane_end = 0, lane_start = 0; int lane_k = -1;
    if (saved_by) for (int k = 0; k < n; k++) saved_by[k] = 0;
    for (int k = 0; k < n; k++) {
        int act = tp_decide(&s, g, reg, n, k, cand, corun, lane_k >= 0 && lane_end <= main_t);
        if ((act & TP_JOIN_BEFORE) && lane_k >= 0) {
            double ov = (main_t < lane_end ? main_t : lane_end) - lane_start;
            if (saved_by && ov > 0) saved_by[lane_k] += ov;
            if (lane_end > main_t) main_t = lane_end;
            lane_k = -1;
        }
        if (act & TP_OFFLOAD) { lane_start = main_t; lane_end = main_t + dur[k]; lane_k = k; }
        else main_t += dur[k];
    }
    if (lane_k >= 0) {
        double ov = (main_t < lane_end ? main_t : lane_end) - lane_start;
        if (saved_by && ov > 0) saved_by[lane_k] += ov;
        if (lane_end > main_t) main_t = lane_end;
    }
    return main_t;
}

/* Lower bound with unlimited lanes: longest dependency chain among the due items (DAG critical path). */
static double tp_critical_path(const TpGraph *g, const int *reg, const double *dur, int n, double *scratch)
{
    double best = 0;
    for (int k = 0; k < n; k++) {
        double st = 0;
        for (int j = 0; j < k; j++)
            if (scratch[j] > st && tp_dep(g, reg[k], reg[j])) st = scratch[j];
        scratch[k] = st + dur[k];
        if (scratch[k] > best) best = scratch[k];
    }
    return best;
}

/* Is the due list a valid topological order (no item depends on a later one)? 1 yes, 0 no. */
static int tp_order_ok(const TpGraph *g, const int *reg, int n)
{
    for (int k = 0; k < n; k++)
        for (int j = k + 1; j < n; j++)
            if (tp_dep(g, reg[k], reg[j])) return 0;
    return 1;
}
#endif
