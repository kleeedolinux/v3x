/* ai_par_core.h - pure logic of the ai_par plugin (no engine access), unit-tested by test/ai_par_test.c.
 *
 *  - latency histogram (log2 microsecond buckets, lock-free adds from any thread) + percentiles
 *  - exact replicas of the engine's range-chunking rules (RunRangeExecutionParallel exe+0x146c960 and the
 *    cost heuristic exe+0x146c010, docs/re/ai_par.md sec. 1) used to explain the measured ranges
 *  - lower bound of a range's wall time on T threads: max(longest item, sum / T)
 *  - the opt-in chunking lever decision (fine_chunks) and config parsing
 *  - a one-pass multi-pattern scanner (the anchors need ~10 whole-.text scans; one pass is ~10x faster) */
#ifndef AI_PAR_CORE_H
#define AI_PAR_CORE_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "v3x_scan.h"

/* ------------------------------------------------------------------ histogram */
#define APAR_HB 26   /* bucket 0: < 1 us; bucket k (1..25): [2^(k-1), 2^k) us; last bucket open-ended (> 16 s) */

typedef struct apar_hist {
    volatile LONG64 n, sum, max;   /* sum / max in QPC ticks */
    volatile LONG64 b[APAR_HB];
} apar_hist;

static int apar_bucket_us(double us)
{
    if (!(us >= 1.0)) return 0;   /* also NaN */
    int k = 1;
    double lim = 2.0;
    while (us >= lim && k < APAR_HB - 1) { lim *= 2.0; k++; }
    return k;
}

/* upper edge of bucket k in microseconds (bucket 0 -> 1 us) */
static double apar_bucket_hi_us(int k) { return k <= 0 ? 1.0 : ldexp(1.0, k); }

static void apar_atomic_max(volatile LONG64 *p, LONG64 v)
{
    LONG64 cur = *p;
    while (v > cur) {
        LONG64 seen = InterlockedCompareExchange64(p, v, cur);
        if (seen == cur) break;
        cur = seen;
    }
}

static void apar_hist_add(apar_hist *h, LONG64 dt, double ticks_per_us)
{
    if (dt < 0) dt = 0;
    InterlockedIncrement64(&h->n);
    InterlockedAdd64(&h->sum, dt);
    apar_atomic_max(&h->max, dt);
    InterlockedIncrement64(&h->b[apar_bucket_us(ticks_per_us > 0 ? (double)dt / ticks_per_us : 0.0)]);
}

/* p in (0,1]: upper bucket edge (us) below which at least p of the samples lie; 0 if empty */
static double apar_hist_pct_us(const apar_hist *h, double p)
{
    LONG64 n = h->n, acc = 0;
    if (n <= 0) return 0.0;
    double need = p * (double)n;
    for (int k = 0; k < APAR_HB; k++) {
        acc += h->b[k];
        if ((double)acc >= need) return apar_bucket_hi_us(k);
    }
    return apar_bucket_hi_us(APAR_HB - 1);
}

/* ------------------------------------------------------------------ engine chunking replicas */
/* RunRangeExecutionParallel (exe+0x146cae7..0x146cb8a, 0x146ccd5..0x146ccfa):
 *   T = [sched+0x648] + 1 (NORMAL workers + the calling thread); mode = Step+0x98 if Step+0x9c else 1
 *   mode 0: grain = max(n/T, 1); mode 1: grain = max(n/T/3, 1); mode 2 (and >= 4): grain = 1;
 *   mode 3: the whole range in one inline call (returns -1)
 *   chunks = min(ceil(n/grain), T); 1 chunk -> inline call on the calling thread. */
static int apar_engine_grain(int n, int T, int mode_engaged, int mode)
{
    int m = mode_engaged ? mode : 1;
    if (T < 1) T = 1;
    if (m == 3) return -1;
    if (m == 0) { int g = n / T; return g > 1 ? g : 1; }
    if (m == 1) { int g = n / T / 3; return g > 1 ? g : 1; }
    return 1;
}

static int apar_engine_chunks(int n, int T, int grain)
{
    if (n <= 0) return 0;
    if (grain < 0) return 1;
    if (grain < 1) grain = 1;
    if (T < 1) T = 1;
    int c = (n + grain - 1) / grain;
    return c < T ? c : T;
}

/* MaybeRunRangeExecutionParallelWithHeuristic (exe+0x146c07e..0x146c55a): only if the cost optional
 * (Step+0xb0) is engaged and n > 2 * workers and max > min. Threshold t = (int)(2*sigma + mean) (float
 * math, population variance); used only if t < max. Items with cost > t are "Expensive" (own job, grain 1),
 * the rest "Affordable" (grain max(n_aff/T/3, 1)). */
typedef struct apar_heur { int used, n_exp, threshold; } apar_heur;

static apar_heur apar_engine_heuristic(const int32_t *cost, int n, int workers, int engaged)
{
    apar_heur r = {0, 0, 0};
    if (!engaged || !cost || n <= 2 * workers || n <= 0) return r;
    int32_t mn = 0x7fffffff, mx = (int32_t)0x80000000;
    for (int i = 0; i < n; i++) { if (cost[i] < mn) mn = cost[i]; if (cost[i] > mx) mx = cost[i]; }
    if (mx <= mn) return r;
    float s = 0.0f;
    for (int i = 0; i < n; i++) s += (float)cost[i];
    float mean = s / (float)n, v = 0.0f;
    for (int i = 0; i < n; i++) { float d = (float)cost[i] - mean; v += d * d; }
    v /= (float)n;
    float sd = sqrtf(v);
    int t = (int)(sd + sd + mean);
    r.threshold = t;
    if (t >= mx) return r;
    for (int i = 0; i < n; i++) r.n_exp += cost[i] > t;
    r.used = r.n_exp > 0;
    return r;
}

/* Lower bound of the wall time of a range whose items take `sum` in total, the longest `max`, on T threads
 * (any schedule, any chunking). */
static double apar_ideal(double sum, double max, int T)
{
    if (T < 1) T = 1;
    double even = sum / (double)T;
    return max > even ? max : even;
}

/* ------------------------------------------------------------------ config */
enum { APAR_OFF = 0, APAR_MEASURE = 1, APAR_ON = 2 };

static int apar_mode_parse(const char *s)
{
    if (!s || !*s) return APAR_MEASURE;
    if (!_stricmp(s, "off")) return APAR_OFF;
    if (!_stricmp(s, "measure")) return APAR_MEASURE;
    if (!_stricmp(s, "on")) return APAR_ON;
    return -1;
}

/* 1 if `name` is an element of the comma/space separated list `csv` (exact, case-sensitive) */
static int apar_list_has(const char *csv, const char *name)
{
    if (!csv || !name || !*name) return 0;
    size_t n = strlen(name);
    const char *p = csv;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        const char *e = p;
        while (*e && *e != ',' && *e != ' ' && *e != '\t') e++;
        if ((size_t)(e - p) == n && !memcmp(p, name, n)) return 1;
        p = e;
    }
    return 0;
}

/* fine_chunks lever: apply only when enabled, the task is listed, the stage really is a parallel range
 * (Step flags bit0) and the task did NOT choose a batch mode itself (Step+0x9c optional empty: the engine
 * default mode 1 would apply). We never override an explicit mode (mode 3 = "must run as one call"). */
static int apar_lever_should(int enabled, const char *csv, const char *task, uint32_t flags, uint8_t mode_engaged)
{
    return enabled && (flags & 1u) && !mode_engaged && apar_list_has(csv, task);
}

/* ------------------------------------------------------------------ one-pass multi-pattern scan */
#define APAR_SCAN_MAXHITS 16
typedef struct apar_scanpat {
    const char *pattern;
    V3xPattern p;
    int ok;                      /* compiled */
    int n;                       /* total matches */
    uint8_t *hit[APAR_SCAN_MAXHITS];
} apar_scanpat;

static void apar_multi_scan(const uint8_t *base, size_t size, apar_scanpat *ps, int np)
{
    uint32_t first[256];   /* bit k = pattern k starts with this byte (np <= 32) */
    memset(first, 0, sizeof first);
    if (np > 32) np = 32;
    for (int k = 0; k < np; k++) {
        ps[k].n = 0;
        ps[k].ok = v3x_pattern_compile(ps[k].pattern, &ps[k].p) > 0 && ps[k].p.mask[0];
        if (ps[k].ok) first[ps[k].p.bytes[0]] |= 1u << k;
    }
    for (size_t i = 0; i < size; i++) {
        uint32_t m = first[base[i]];
        while (m) {
            int k = 0;
            while (!(m & (1u << k))) k++;
            m &= ~(1u << k);
            apar_scanpat *s = &ps[k];
            if (i + (size_t)s->p.n > size || !v3x_pattern_match_at(&s->p, base + i)) continue;
            if (s->n < APAR_SCAN_MAXHITS) s->hit[s->n] = (uint8_t *)base + i;
            s->n++;
        }
    }
}

#endif
