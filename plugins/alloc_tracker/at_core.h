/* alloc_tracker pure logic (no Windows calls except Interlocked*), shared by alloc_tracker.c and
 * test/alloc_tracker_test.c. All tables are fixed-size, preallocated by the caller, lock-free.
 *
 * Sites:  open addressing keyed by caller RVA (uint32, 0 = empty). Insert claims a slot with CAS.
 * Live:   open addressing keyed by block pointer (0 = empty, 1 = tombstone). Blocks >= the big
 *         threshold (kind 0, exact size) and SAMPLED small blocks (kind 1, size = sample weight) are
 *         inserted, so mi_free does one lookup per call. Tombstones are reused.
 * Sample: per-thread byte countdown; every `period` bytes of small allocations one event is
 *         recorded with weight = period (unbiased estimate of small-allocation bytes per site).
 *         The sampled block stays in the live table until mi_free: sum of weights of still-live
 *         samples = unbiased estimate of LIVE small bytes per site (size-biased sampling).
 * VM:     NtAllocateVirtualMemory(MEM_RESERVE) owners keyed by allocation base (diagnostic for the
 *         non-mimalloc private commit). */
#ifndef AT_CORE_H
#define AT_CORE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>

#define AT_FRAMES 6

typedef struct {
    volatile LONG key;                 /* caller RVA, 0 = empty */
    volatile LONG64 total_bytes;       /* big: exact; small: sampled estimate */
    volatile LONG64 total_count;
    volatile LONG64 live_bytes;        /* big blocks only */
    volatile LONG64 live_count;
    volatile LONG64 live_small_bytes;  /* sampled small blocks still live, sum of weights (estimate) */
    volatile LONG64 live_small_count;  /* live samples */
    volatile LONG64 big_count;
    volatile LONG64 max_size;
    volatile LONG frames_set;          /* 0 -> 1 when `frames` was filled (first >= stack threshold) */
    uint32_t frames[AT_FRAMES];        /* RVAs, 0 = none */
} at_site;

enum { AT_BIG = 0, AT_SMALL = 1 };
typedef struct { volatile LONG64 key; volatile LONG64 size; volatile LONG site; volatile LONG kind; } at_live;

typedef struct {
    at_site *sites; uint32_t nsites;   /* power of two */
    at_live *live;  uint32_t nlive;    /* power of two */
    volatile LONG64 dropped_sites, dropped_live, live_bytes, live_blocks;   /* live_blocks counts both kinds */
    volatile LONG64 live_small_bytes, live_small_blocks;
} at_tables;

static inline uint32_t at_hash32(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }
static inline uint64_t at_hash64(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return x; }

/* Find or create the site for `rva` (non-zero). Returns index or -1 when the table is full. */
static int at_site_get(at_tables *t, uint32_t rva)
{
    if (!rva) rva = 1;
    uint32_t m = t->nsites - 1, h = at_hash32(rva) & m;
    for (uint32_t i = 0; i < 64; i++, h = (h + 1) & m) {
        LONG k = t->sites[h].key;
        if (k == (LONG)rva) return (int)h;
        if (k == 0) {
            LONG prev = InterlockedCompareExchange(&t->sites[h].key, (LONG)rva, 0);
            if (prev == 0 || prev == (LONG)rva) return (int)h;
        }
    }
    InterlockedIncrement64(&t->dropped_sites);
    return -1;
}

static void at_site_add(at_tables *t, int s, int64_t bytes, int64_t count, int64_t size, int big)
{
    if (s < 0) return;
    at_site *e = &t->sites[s];
    InterlockedAdd64(&e->total_bytes, bytes);
    InterlockedAdd64(&e->total_count, count);
    if (big) {
        InterlockedAdd64(&e->live_bytes, size);
        InterlockedIncrement64(&e->live_count);
        InterlockedIncrement64(&e->big_count);
        LONG64 cur = e->max_size;
        while (size > cur) { LONG64 p = InterlockedCompareExchange64(&e->max_size, size, cur); if (p == cur) break; cur = p; }
    }
}

/* Record a live block (kind AT_BIG: exact size; AT_SMALL: sample weight). Returns 0, or -1 when no
 * slot was found within the probe limit. */
static int at_live_insert(at_tables *t, uint64_t p, int64_t size, int site, int kind)
{
    uint32_t m = t->nlive - 1, h = (uint32_t)at_hash64(p) & m;
    for (uint32_t i = 0; i < 128; i++, h = (h + 1) & m) {
        LONG64 k = t->live[h].key;
        if (k == 0 || k == 1) {
            if (InterlockedCompareExchange64(&t->live[h].key, (LONG64)p, k) == k) {
                t->live[h].size = size; t->live[h].site = site; t->live[h].kind = kind;
                InterlockedIncrement64(&t->live_blocks);
                if (kind == AT_SMALL) {
                    InterlockedAdd64(&t->live_small_bytes, size); InterlockedIncrement64(&t->live_small_blocks);
                    if (site >= 0) { InterlockedAdd64(&t->sites[site].live_small_bytes, size); InterlockedIncrement64(&t->sites[site].live_small_count); }
                } else InterlockedAdd64(&t->live_bytes, size);
                return 0;
            }
        }
    }
    InterlockedIncrement64(&t->dropped_live);
    return -1;
}

/* Remove a live block if present. Returns its size (or weight), 0 when the pointer is not tracked. */
static int64_t at_live_remove(at_tables *t, uint64_t p)
{
    uint32_t m = t->nlive - 1, h = (uint32_t)at_hash64(p) & m;
    for (uint32_t i = 0; i < 128; i++, h = (h + 1) & m) {
        LONG64 k = t->live[h].key;
        if (k == 0) return 0;
        if (k == (LONG64)p) {
            int64_t size = t->live[h].size; int site = t->live[h].site, kind = t->live[h].kind;
            if (InterlockedCompareExchange64(&t->live[h].key, 1, k) != k) return 0;   /* lost a race: already freed */
            InterlockedDecrement64(&t->live_blocks);
            if (kind == AT_SMALL) {
                InterlockedAdd64(&t->live_small_bytes, -size); InterlockedDecrement64(&t->live_small_blocks);
                if (site >= 0) { InterlockedAdd64(&t->sites[site].live_small_bytes, -size); InterlockedDecrement64(&t->sites[site].live_small_count); }
            } else {
                InterlockedAdd64(&t->live_bytes, -size);
                if (site >= 0) { InterlockedAdd64(&t->sites[site].live_bytes, -size); InterlockedDecrement64(&t->sites[site].live_count); }
            }
            return size;
        }
    }
    return 0;
}

/* Size-weighted sampler step. `*left` is the per-thread countdown. Returns the event weight in
 * bytes (0 = not sampled). */
static inline int64_t at_sample(int64_t *left, int64_t size, int64_t period)
{
    *left -= size;
    if (*left > 0) return 0;
    int64_t w = period, over = -*left;
    *left = period - (over % period);
    return w + (over / period) * period;   /* sizes larger than one period count proportionally */
}

/* Pick the attribution frame: first frame whose RVA is not inside a skip range.
 * `skip` must be sorted by begin and non-overlapping (binary search). */
typedef struct { uint32_t begin, end; } at_range;
static int at_in_ranges(uint32_t r, const at_range *skip, int nskip)
{
    int lo = 0, hi = nskip;
    while (lo < hi) { int m = (lo + hi) / 2; if (skip[m].end <= r) lo = m + 1; else hi = m; }
    return lo < nskip && skip[lo].begin <= r && r < skip[lo].end;
}
static uint32_t at_pick_frame(const uint32_t *f, int n, const at_range *skip, int nskip)
{
    for (int i = 0; i < n; i++) if (f[i] && !at_in_ranges(f[i], skip, nskip)) return f[i];
    return n > 0 ? f[0] : 0;
}
static int at_range_cmp(const void *a, const void *b)
{
    uint32_t x = ((const at_range *)a)->begin, y = ((const at_range *)b)->begin;
    return x < y ? -1 : x > y;
}

/* ---- VM owners: allocation base -> first non-system return address (+ first exe frame) ----
 * key 0 = empty, 1 = tombstone. put overwrites an existing key (a base is only handed out again
 * after it was freed). */
typedef struct { volatile LONG64 key; volatile LONG64 owner, exe, size; } at_vm;
static int at_vm_put(at_vm *v, uint32_t n, uint64_t base, uint64_t owner, uint64_t exe, uint64_t size)
{
    uint32_t m = n - 1, h0 = (uint32_t)at_hash64(base) & m, h = h0;
    for (uint32_t i = 0; i < 64; i++, h = (h + 1) & m) {
        LONG64 k = v[h].key;
        if (k == (LONG64)base) { v[h].owner = (LONG64)owner; v[h].exe = (LONG64)exe; v[h].size = (LONG64)size; return 0; }
        if (k == 0) break;
    }
    h = h0;
    for (uint32_t i = 0; i < 64; i++, h = (h + 1) & m) {
        LONG64 k = v[h].key;
        if ((k == 0 || k == 1) && InterlockedCompareExchange64(&v[h].key, (LONG64)base, k) == k) {
            v[h].owner = (LONG64)owner; v[h].exe = (LONG64)exe; v[h].size = (LONG64)size; return 0;
        }
    }
    return -1;
}
static at_vm *at_vm_get(at_vm *v, uint32_t n, uint64_t base)
{
    uint32_t m = n - 1, h = (uint32_t)at_hash64(base) & m;
    for (uint32_t i = 0; i < 64; i++, h = (h + 1) & m) {
        LONG64 k = v[h].key;
        if (k == (LONG64)base) return &v[h];
        if (k == 0) return NULL;
    }
    return NULL;
}

#endif
