/* mt_mi.h: read the engine's statically linked mimalloc 2.0.x from the outside (victoria3 1.15.1).
 * Shared by mem_tuner (census / memstat) and alloc_tracker (alloc_report.txt). docs/re/memory.md section 11.
 *
 * 1. OS stats: `_mi_stats_main` (.data). os.c updates committed/reserved there directly and atomically
 *    (lea rcx,[stats+0x60] at 17 commit/decommit sites), so committed.current is mimalloc's exact commit.
 * 2. Segment walk: every 32 MiB-aligned private allocation whose header passes the checks (cookie =
 *    segment ^ global key, slice counts, kind) is a mimalloc segment. Its commit mask, decommit mask and
 *    slice/page array give: bytes in use (page.used * block size), slack inside pages, empty pages,
 *    committed free spans and pending (delayed) decommits. Header copied with ReadProcessMemory, so a
 *    segment freed meanwhile cannot fault; a torn copy fails the checks and is counted as unparsed.
 *
 * Layout [V] (1.15.1 disassembly): mi_free 0x43d94b0 (thread_id +0x100, slices +0x108, slice 0x50 bytes,
 * slice_offset +4 in bytes), segment init 0x43daf80 (allow_decommit +0x20, decommit_expire +0x28,
 * decommit_mask +0x30, commit_mask +0x70, cookie +0xd8, segment_slices +0xe0, info_slices +0xe8,
 * kind +0xf0, slice_entries +0xf8), page extend 0x43ddf5b (capacity +0xa, reserved +0xc),
 * mi_free (used +0x18), xblock_size +0x1c. Stats size 0x240 (mi_stats_reset 0x43e15f0). */
#ifndef MT_MI_H
#define MT_MI_H
#include <stdint.h>
#include <string.h>
#include <windows.h>
#include "v3x_safe.h"

#define MI_SEGSZ        (32ull << 20)
#define MI_SLICESZ      (64ull << 10)
#define MI_NSLICES      512
#define MI_O_DMASK      0x30
#define MI_O_CMASK      0x70
#define MI_O_COOKIE     0xd8
#define MI_O_SEGSLICES  0xe0
#define MI_O_INFOSLICES 0xe8
#define MI_O_KIND       0xf0
#define MI_O_ENTRIES    0xf8
#define MI_O_THREAD     0x100
#define MI_O_SLICES     0x108
#define MI_SLICE        0x50
#define MI_P_COUNT      0x00
#define MI_P_USED       0x18
#define MI_P_BSIZE      0x1c
#define MI_HDR_BYTES    (MI_O_SLICES + MI_SLICE * (MI_NSLICES + 1))
#define MI_HUGE_BSIZE   0x80000000u

/* _mi_stats_main offsets: mi_stat_count_t {allocated, freed, peak, current} = 0x20 bytes */
#define MI_S_RESERVED   0x40
#define MI_S_COMMITTED  0x60
#define MI_S_THREADS    0x100
#define MI_S_COMMITCALL 0x1e0   /* mi_stat_counter_t {total, count} [I: 3rd counter after pages_extended, mmap_calls] */
#define MI_S_SIZE       0x240

#define MT_MI_HIST    12
#define MT_MI_THREADS 48
static const char *const MT_MI_HIST_NAME[MT_MI_HIST] = { "<=16", "<=32", "<=64", "<=128", "<=256", "<=512", "<=1K",
                                                         "<=4K", "<=16K", "<=64K", "<=1M", ">1M" };

typedef struct { uint64_t tid, committed, used, free; uint32_t segs; } mt_mi_thr;
typedef struct {
    uint64_t segs, bad, huge, mismatch;        /* parsed normal segments, unparsed, huge, mask vs VirtualQuery mismatch */
    uint64_t committed, vq_committed;          /* normal segments: commit-mask bytes; VirtualQuery committed (cross-check) */
    uint64_t bad_committed, huge_committed;    /* VirtualQuery committed of unparsed / huge segments */
    uint64_t meta, used, slack, empty, free_committed, pending;
    uint64_t pages, pages_empty, abandoned_segs, abandoned_committed;
    uint64_t hist_used[MT_MI_HIST], hist_slack[MT_MI_HIST], hist_pages[MT_MI_HIST];
    mt_mi_thr thr[MT_MI_THREADS]; int nthr; uint64_t thr_other_committed;
} mt_mi_sum;

static inline uint64_t mt_rd64(const unsigned char *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline uint32_t mt_rdu32(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline int mt_popc64(uint64_t x) { int n = 0; while (x) { x &= x - 1; n++; } return n; }

static int mt_mi_bin(uint32_t bs)
{
    static const uint32_t lim[MT_MI_HIST - 1] = { 16, 32, 64, 128, 256, 512, 1024, 4096, 16384, 65536, 1u << 20 };
    for (int i = 0; i < MT_MI_HIST - 1; i++) if (bs <= lim[i]) return i;
    return MT_MI_HIST - 1;
}

/* committed slices in [a, b) of a 512-bit mask */
static uint64_t mt_mi_mask_range(const unsigned char *mask, uint32_t a, uint32_t b)
{
    uint64_t n = 0;
    for (uint32_t i = a; i < b && i < MI_NSLICES; i++) n += (mask[i >> 3] >> (i & 7)) & 1;
    return n * MI_SLICESZ;
}

static mt_mi_thr *mt_mi_thread(mt_mi_sum *s, uint64_t tid)
{
    for (int i = 0; i < s->nthr; i++) if (s->thr[i].tid == tid) return &s->thr[i];
    if (s->nthr >= MT_MI_THREADS) return NULL;
    mt_mi_thr *t = &s->thr[s->nthr++]; memset(t, 0, sizeof *t); t->tid = tid; return t;
}

/* Parse one segment header copy `h` (MI_HDR_BYTES) at address `va`. key = global cookie key (0 = unknown:
 * cookie not checked). vq = VirtualQuery committed bytes of the allocation. Returns 0 normal, 1 huge, -1 not
 * a (consistent) segment. Pure: no Windows calls. */
static int mt_mi_parse(mt_mi_sum *s, const unsigned char *h, uint64_t va, uint64_t key, uint64_t vq)
{
    uint64_t nsl = mt_rd64(h + MI_O_SEGSLICES), info = mt_rd64(h + MI_O_INFOSLICES), ent = mt_rd64(h + MI_O_ENTRIES);
    uint32_t kind = mt_rdu32(h + MI_O_KIND);
    uint64_t tid = mt_rd64(h + MI_O_THREAD);
    if ((va & (MI_SEGSZ - 1)) || (key && (mt_rd64(h + MI_O_COOKIE) ^ va) != key)) goto bad;
    if (kind == 1) {
        if (nsl < 1 || info < 1 || info > 64 || ent != (nsl < MI_NSLICES ? nsl : MI_NSLICES)) goto bad;
        s->huge++; s->huge_committed += vq;
        return 1;
    }
    if (kind != 0 || nsl != MI_NSLICES || ent != MI_NSLICES || info < 1 || info > 64) goto bad;
    {
        const unsigned char *cm = h + MI_O_CMASK, *dm = h + MI_O_DMASK;
        uint64_t committed = 0, pending = 0;
        for (int i = 0; i < 8; i++) {
            uint64_t c = mt_rd64(cm + i * 8), d = mt_rd64(dm + i * 8);
            committed += (uint64_t)mt_popc64(c) * MI_SLICESZ; pending += (uint64_t)mt_popc64(c & d) * MI_SLICESZ;
        }
        /* walk spans; collect into locals first so a torn copy adds nothing */
        uint64_t meta = 0, used = 0, slack = 0, empty = 0, freec = 0, pages = 0, pempty = 0;
        uint64_t hu[MT_MI_HIST] = { 0 }, hs[MT_MI_HIST] = { 0 }, hp[MT_MI_HIST] = { 0 };
        uint32_t i = 0;
        while (i < MI_NSLICES) {
            const unsigned char *sl = h + MI_O_SLICES + (size_t)i * MI_SLICE;
            uint32_t cnt = mt_rdu32(sl + MI_P_COUNT), bs = mt_rdu32(sl + MI_P_BSIZE), u = mt_rdu32(sl + MI_P_USED);
            if (cnt == 0 || i + cnt > MI_NSLICES) goto bad;
            uint64_t c = mt_mi_mask_range(cm, i, i + cnt);
            if (i < info) meta += c;
            else if (bs == 0) freec += c;
            else {
                pages++;
                uint64_t ub = bs >= MI_HUGE_BSIZE ? c : (uint64_t)u * bs;
                if (ub > (uint64_t)cnt * MI_SLICESZ) goto bad;
                if (ub > c) ub = c;                       /* page span partly uncommitted: count what is there */
                int b = mt_mi_bin(bs);
                if (u == 0) { empty += c; pempty++; hs[b] += c; }
                else { used += ub; slack += c - ub; hu[b] += ub; hs[b] += c - ub; }
                hp[b]++;
            }
            i += cnt;
        }
        s->segs++; s->committed += committed; s->vq_committed += vq; s->pending += pending;
        if ((committed > vq ? committed - vq : vq - committed) > (1ull << 20)) s->mismatch++;
        s->meta += meta; s->used += used; s->slack += slack; s->empty += empty; s->free_committed += freec;
        s->pages += pages; s->pages_empty += pempty;
        for (int b = 0; b < MT_MI_HIST; b++) { s->hist_used[b] += hu[b]; s->hist_slack[b] += hs[b]; s->hist_pages[b] += hp[b]; }
        if (!tid) { s->abandoned_segs++; s->abandoned_committed += committed; }
        mt_mi_thr *t = mt_mi_thread(s, tid);
        if (t) { t->segs++; t->committed += committed; t->used += used; t->free += committed - used - meta; }
        else s->thr_other_committed += committed;
        return 0;
    }
bad:
    s->bad++; s->bad_committed += vq;
    return -1;
}

/* ---- anchors inside the mimalloc code range (pure; `code` maps va `va0`) ---- */
typedef struct { uint64_t stats, cookie_key, deferred_fn, deferred_arg; } mt_mi_anchors;

static int mt_mi_match(const unsigned char *p, const char *pat)     /* "48 8D 3D ?? .." */
{
    for (; *pat; ) {
        while (*pat == ' ') pat++;
        if (!*pat) break;
        if (pat[0] == '?') { pat += 2; p++; continue; }
        unsigned v = 0;
        for (int k = 0; k < 2; k++) { char c = pat[k]; v = v * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10); }
        if (*p++ != (unsigned char)v) return 0;
        pat += 2;
    }
    return 1;
}
static size_t mt_mi_patlen(const char *pat) { size_t n = 0; for (; *pat; pat++) if (*pat != ' ') n++; return n / 2; }
/* unique match offset, -1 none, -2 ambiguous */
static ptrdiff_t mt_mi_find(const unsigned char *code, size_t n, const char *pat)
{
    size_t pl = mt_mi_patlen(pat); ptrdiff_t hit = -1;
    for (size_t i = 0; i + pl <= n; i++)
        if (mt_mi_match(code + i, pat)) { if (hit >= 0) return -2; hit = (ptrdiff_t)i; }
    return hit;
}
static uint64_t mt_mi_rip(const unsigned char *code, uint64_t va0, size_t at, size_t disp_off, size_t ins_len)
{
    int32_t d; memcpy(&d, code + at + disp_off, 4);
    return va0 + at + ins_len + (int64_t)d;
}

/* mi_stats_print: lea rdi,[_mi_stats_main]; mov r10,[rax]; add r10,0x3c0 (tld->stats); cmp r10,rdi */
static const char MT_MI_PAT_STATS[] = "48 8D 3D ?? ?? ?? ?? 4C 8B 10 49 81 C2 C0 03 00 00 4C 3B D7";
/* mi_segment_init: xchg [rbx+100h],rax (thread id); ...; xor rax,[key]; mov [rbx+0D8h],rax (cookie) */
static const char MT_MI_PAT_COOKIE[] = "48 87 83 00 01 00 00 48 8B 93 E8 00 00 00 B9 00 02 00 00 48 8B C3 48 33 05 ?? ?? ?? ?? 48 89 83 D8 00 00 00";
/* _mi_malloc_generic, inlined _mi_deferred_free: if (deferred_free && !tld->recurse) { recurse=1; fn(false, heartbeat, arg); } */
static const char MT_MI_PAT_DEFERRED[] = "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B 07 80 78 08 00 75 ?? C6 40 08 01 48 8B 05 ?? ?? ?? ?? 4C 8B 05 ?? ?? ?? ?? 48 8B 17 48 8B 12 33 C9 FF D0";

/* Returns a bit mask of what was found: 1 stats, 2 cookie key address, 4 deferred-free globals. */
static int mt_mi_anchor(const unsigned char *code, size_t n, uint64_t va0, mt_mi_anchors *a)
{
    int got = 0; ptrdiff_t o;
    memset(a, 0, sizeof *a);
    if ((o = mt_mi_find(code, n, MT_MI_PAT_STATS)) >= 0) { a->stats = mt_mi_rip(code, va0, (size_t)o, 3, 7); got |= 1; }
    if ((o = mt_mi_find(code, n, MT_MI_PAT_COOKIE)) >= 0) { a->cookie_key = mt_mi_rip(code, va0, (size_t)o + 22, 3, 7); got |= 2; }
    if ((o = mt_mi_find(code, n, MT_MI_PAT_DEFERRED)) >= 0) {
        uint64_t f1 = mt_mi_rip(code, va0, (size_t)o, 3, 7), f2 = mt_mi_rip(code, va0, (size_t)o + 25, 3, 7);
        uint64_t ar = mt_mi_rip(code, va0, (size_t)o + 32, 3, 7);
        if (f1 == f2 && ar == f1 + 8) { a->deferred_fn = f1; a->deferred_arg = ar; got |= 4; }
    }
    return got;
}

/* ---- live walk (Windows) ---- */
/* VirtualQuery the whole address space; parse each 32 MiB-aligned private allocation except the one
 * based at `skip` (0 = none; a caller's own non-mimalloc allocation, e.g. mem_tuner's OOM reserve,
 * which is 32 MiB aligned by chance often enough to make the counts nondeterministic). `buf` must hold
 * MI_HDR_BYTES. Returns the number of candidate allocations. Runs in a few ms for ~300 segments. */
static int mt_mi_walk_skip(mt_mi_sum *s, uint64_t key, unsigned char *buf, uint64_t skip)
{
    memset(s, 0, sizeof *s);
    SYSTEM_INFO si; GetSystemInfo(&si);
    unsigned char *p = (unsigned char *)si.lpMinimumApplicationAddress, *end = (unsigned char *)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mb;
    uint64_t cur = 0, vq = 0; int cur_ok = 0, cand = 0;
    for (;;) {
        int more = p < end && VirtualQuery(p, &mb, sizeof mb) == sizeof mb;
        uint64_t ab = more ? (uint64_t)(uintptr_t)mb.AllocationBase : 0;
        if (!more || mb.State == MEM_FREE || ab != cur) {
            if (cur_ok) {
                SIZE_T got = 0; cand++;
                if (ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)cur, buf, MI_HDR_BYTES, &got) && got == MI_HDR_BYTES)
                    mt_mi_parse(s, buf, cur, key, vq);
                else { s->bad++; s->bad_committed += vq; }
            }
            cur = 0; cur_ok = 0; vq = 0;
            if (more && mb.State != MEM_FREE) {
                cur = ab;
                cur_ok = mb.Type == MEM_PRIVATE && !(ab & (MI_SEGSZ - 1)) && mb.BaseAddress == mb.AllocationBase && mb.State == MEM_COMMIT
                         && (!skip || ab != skip);
            }
        }
        if (!more) break;
        if (mb.State == MEM_COMMIT && cur_ok) vq += mb.RegionSize;
        unsigned char *nx = (unsigned char *)mb.BaseAddress + mb.RegionSize;
        if (nx <= p) break;
        p = nx;
    }
    return cand;
}
static int mt_mi_walk(mt_mi_sum *s, uint64_t key, unsigned char *buf) { return mt_mi_walk_skip(s, key, buf, 0); }

/* Read the OS stats block (our own process memory; plain reads of .data). */
typedef struct { int64_t committed, committed_peak, reserved, reserved_peak, threads, commit_calls; } mt_mi_os;
static int mt_mi_os_read(uint64_t stats, mt_mi_os *o)
{
    memset(o, 0, sizeof *o);
    if (!stats) return -1;
    const volatile int64_t *q = (const volatile int64_t *)(uintptr_t)stats;
    o->committed = q[(MI_S_COMMITTED + 0x18) / 8]; o->committed_peak = q[(MI_S_COMMITTED + 0x10) / 8];
    o->reserved = q[(MI_S_RESERVED + 0x18) / 8]; o->reserved_peak = q[(MI_S_RESERVED + 0x10) / 8];
    o->threads = q[(MI_S_THREADS + 0x18) / 8]; o->commit_calls = q[(MI_S_COMMITCALL + 8) / 8];
    if (o->committed < 0 || o->reserved < o->committed || o->committed > (1ll << 40)) return -1;   /* not the stats block */
    return 0;
}

#define MT_MIB(x) ((unsigned long long)((x) >> 20))
/* One-line summary (and optionally the size histogram / per-thread lines through `out`). */
static void mt_mi_format(const mt_mi_sum *s, const mt_mi_os *os, int have_os, char *b, size_t n)
{
    V3xStr sb; v3x_str_init(&sb, b, n);
    if (have_os)
        v3x_str_appendf(&sb, "mimalloc os: committed %llu MiB (peak %llu), reserved %llu MiB, threads %lld, commit calls %lld | ",
                              MT_MIB(os->committed), MT_MIB(os->committed_peak), MT_MIB(os->reserved), (long long)os->threads,
                              (long long)os->commit_calls);
    v3x_str_appendf(&sb, "segments %llu (+%llu huge %llu MiB, %llu unparsed %llu MiB, mask!=VQ %llu): committed %llu MiB = "
             "IN USE %llu + page slack %llu + empty pages %llu + free spans %llu (pending decommit %llu) + meta %llu MiB; "
             "pages %llu (%llu empty); abandoned segs %llu (%llu MiB)",
             (unsigned long long)s->segs, (unsigned long long)s->huge, MT_MIB(s->huge_committed), (unsigned long long)s->bad,
             MT_MIB(s->bad_committed), (unsigned long long)s->mismatch, MT_MIB(s->committed), MT_MIB(s->used), MT_MIB(s->slack),
             MT_MIB(s->empty), MT_MIB(s->free_committed), MT_MIB(s->pending), MT_MIB(s->meta), (unsigned long long)s->pages,
             (unsigned long long)s->pages_empty, (unsigned long long)s->abandoned_segs, MT_MIB(s->abandoned_committed));
}
static void mt_mi_format_hist(const mt_mi_sum *s, char *b, size_t n)
{
    V3xStr sb; v3x_str_init(&sb, b, n);
    v3x_str_append(&sb, "mimalloc by block size (in use / slack MiB, pages):");
    for (int i = 0; i < MT_MI_HIST; i++)
        if (s->hist_pages[i])
            v3x_str_appendf(&sb, " %s %llu/%llu (%llu)", MT_MI_HIST_NAME[i], MT_MIB(s->hist_used[i]),
                                  MT_MIB(s->hist_slack[i]), (unsigned long long)s->hist_pages[i]);
}
#endif
