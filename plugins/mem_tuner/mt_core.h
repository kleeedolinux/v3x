/* mem_tuner pure logic (no Windows calls), shared by mem_tuner.c and test/mem_tuner_test.c.
 * Mirrored in tools/re_mem_check.py - keep the byte patterns in sync.
 *
 * mimalloc 2.0.x option table (victoria3.exe 1.15.1: .data 0x5407bb0, 26 entries x 0x20):
 *   struct { int32 value; int32 init; int32 option; int32 pad; const char *name; const char *legacy; }
 *   init: 0 UNINIT, 1 DEFAULTED, 2 INITIALIZED  (we write 2 so mimalloc never reads env for it) */
#ifndef MT_CORE_H
#define MT_CORE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MT_NOPT 26
#define MT_ENTRY 0x20
#define MT_INIT_INITIALIZED 2

static const char *const MT_NAMES[MT_NOPT] = {
    "show_errors", "show_stats", "verbose", "eager_commit", "deprecated_eager_region_commit",
    "deprecated_reset_decommits", "large_os_pages", "reserve_huge_os_pages", "reserve_huge_os_pages_at",
    "reserve_os_memory", "deprecated_segment_cache", "page_reset", "abandoned_page_decommit",
    "deprecated_segment_reset", "eager_commit_delay", "decommit_delay", "use_numa_nodes", "limit_os_alloc",
    "os_tag", "max_errors", "max_warnings", "max_segment_reclaim", "allow_decommit",
    "segment_decommit_delay", "decommit_extend_delay", "destroy_on_exit",
};
enum { MT_EAGER_COMMIT = 3, MT_LARGE_OS_PAGES = 6, MT_ABANDONED_PAGE_DECOMMIT = 12,
       MT_DECOMMIT_DELAY = 15, MT_SEGMENT_DECOMMIT_DELAY = 23, MT_DECOMMIT_EXTEND_DELAY = 24 };

typedef struct { int32_t value, init, option, pad; uint64_t name, legacy; } mt_entry;

/* Resolve a VA to a NUL-terminated string inside the image, or NULL. */
typedef const char *(*mt_str_fn)(void *ctx, uint64_t va);

/* Validate MT_NOPT entries at `t`: option index == i and name == MT_NAMES[i]. 0 = ok, else 1+bad index. */
static int mt_validate(const unsigned char *t, mt_str_fn str, void *ctx)
{
    for (int i = 0; i < MT_NOPT; i++) {
        mt_entry e; memcpy(&e, t + (size_t)i * MT_ENTRY, sizeof e);
        const char *s = str(ctx, e.name);
        if (e.option != i || !s || strcmp(s, MT_NAMES[i])) return i + 1;
    }
    return 0;
}

/* Find the table in a data section [data, data+n) whose bytes live at virtual address data_va.
 * name0_va = VA of the string "show_errors". Requires exactly one valid match (fail closed).
 * Returns byte offset of entry 0 in data, or -1 (none) / -2 (ambiguous). */
static ptrdiff_t mt_find_table(const unsigned char *data, size_t n, uint64_t name0_va, mt_str_fn str, void *ctx)
{
    ptrdiff_t found = -1;
    if (n < (size_t)MT_NOPT * MT_ENTRY) return -1;
    for (size_t off = 0x10; off + 8 <= n; off += 8) {
        uint64_t q; memcpy(&q, data + off, 8);
        if (q != name0_va) continue;
        size_t t = off - 0x10;
        if (t + (size_t)MT_NOPT * MT_ENTRY > n) continue;
        if (mt_validate(data + t, str, ctx)) continue;
        if (found >= 0) return -2;
        found = (ptrdiff_t)t;
    }
    return found;
}

/* Byte-pattern helpers on code. `code` = bytes of a function, `va` = its address. */
static int32_t mt_rd32(const unsigned char *p) { int32_t v; memcpy(&v, p, 4); return v; }

/* First `mov cl,1 ; call rel32` (B1 01 E8) in [code, code+n): returns call target or 0. (mi_process_done -> mi_collect) */
static uint64_t mt_find_movcl1_call(const unsigned char *code, size_t n, uint64_t va)
{
    for (size_t i = 0; i + 7 <= n; i++)
        if (code[i] == 0xB1 && code[i + 1] == 0x01 && code[i + 2] == 0xE8)
            return va + i + 7 + (int64_t)mt_rd32(code + i + 3);
    return 0;
}

/* First call rel32 (E8) in [code, code+n) whose target bytes start with `want` (checked by `peek`). */
typedef const unsigned char *(*mt_peek_fn)(void *ctx, uint64_t va, size_t len);
static uint64_t mt_find_call_to(const unsigned char *code, size_t n, uint64_t va,
                                const unsigned char *want, size_t wn, mt_peek_fn peek, void *ctx)
{
    for (size_t i = 0; i + 5 <= n; i++) {
        if (code[i] != 0xE8) continue;
        uint64_t t = va + i + 5 + (int64_t)mt_rd32(code + i + 1);
        const unsigned char *p = peek(ctx, t, wn);
        if (p && !memcmp(p, want, wn)) return t;
    }
    return 0;
}

/* `lea rdx,[rip+X] ; mov ecx,0Ch ; call rel32` where X == str_va: returns call target (_mi_error_message) or 0. */
static uint64_t mt_find_errmsg_call(const unsigned char *code, size_t n, uint64_t va, uint64_t str_va)
{
    for (size_t i = 0; i + 17 <= n; i++) {
        if (code[i] != 0x48 || code[i + 1] != 0x8D || code[i + 2] != 0x15) continue;
        if (va + i + 7 + (int64_t)mt_rd32(code + i + 3) != str_va) continue;
        if (code[i + 7] != 0xB9 || mt_rd32(code + i + 8) != 12 || code[i + 12] != 0xE8) continue;
        return va + i + 17 + (int64_t)mt_rd32(code + i + 13);
    }
    return 0;
}

/* In _mi_error_message: `mov rax,[rip+H] ; mov ecx,edi ; mov rdx,[rip+A] ; (nop)? ; call rax` with A == H+8.
 * Returns VA of the handler global H (arg global is H+8), or 0. */
static uint64_t mt_find_error_handler_global(const unsigned char *code, size_t n, uint64_t va)
{
    for (size_t i = 0; i + 18 <= n; i++) {
        const unsigned char *c = code + i;
        if (c[0] != 0x48 || c[1] != 0x8B || c[2] != 0x05) continue;
        if (c[7] != 0x8B || c[8] != 0xCF || c[9] != 0x48 || c[10] != 0x8B || c[11] != 0x15) continue;
        uint64_t h = va + i + 7 + (int64_t)mt_rd32(c + 3);
        uint64_t a = va + i + 16 + (int64_t)mt_rd32(c + 12);
        size_t j = 16;
        if (c[j] == 0x90) j++;
        if (i + j + 2 > n || c[j] != 0xFF || c[j + 1] != 0xD0) continue;
        if (a != h + 8) continue;
        return h;
    }
    return 0;
}

/* ---------------------------------------------------------------- periodic collect trigger
 * Called every check with the current process commit. `base_mb` follows the lowest commit seen
 * since the last collect (so a transient spike that was freed again also counts as growth: that is
 * exactly when mimalloc holds free-but-committed pages). Due when commit >= base + grow_mb and at
 * least min_interval_ms passed since the last collect. After a collect call mt_periodic_done. */
typedef struct { long long grow_mb; uint64_t min_interval_ms, last_ms; long long base_mb; int started; } mt_periodic;

static int mt_periodic_due(mt_periodic *p, uint64_t now_ms, long long commit_mb)
{
    if (!p->started) { p->started = 1; p->base_mb = commit_mb; p->last_ms = now_ms; return 0; }
    if (commit_mb < p->base_mb) p->base_mb = commit_mb;
    return commit_mb - p->base_mb >= p->grow_mb && now_ms - p->last_ms >= p->min_interval_ms;
}
static void mt_periodic_done(mt_periodic *p, uint64_t now_ms, long long commit_after_mb)
{
    p->started = 1; p->last_ms = now_ms; p->base_mb = commit_after_mb;
}

/* ---------------------------------------------------------------- commit census
 * Feed every non-free VirtualQuery region in ascending address order (allocation = run of regions
 * with the same AllocationBase). mimalloc 2.0.x reserves each segment separately, 32 MiB aligned
 * (mi_free masks pointers with ~(32 MiB-1)): normal segment = exactly 32 MiB, huge segment (object
 * > 16 MiB) = one aligned reservation > 16 MiB. Anything else private is not mimalloc (Win32 heaps,
 * driver/D3D resources, the OOM reserve). A non-mimalloc reservation is 32 MiB aligned by chance
 * with p = 1/512 (64 KiB granularity), so the split is a close estimate, not exact. */
#define MT_SEG (32ull << 20)
enum { MT_PRIVATE = 1, MT_MAPPED = 2, MT_IMAGE = 3 };
#define MT_NBUCKET 4   /* other private by committed size: < 1 MiB, < 16 MiB, < 64 MiB, >= 64 MiB */
typedef struct {
    uint64_t seg_n, seg_full_n, seg_commit;               /* mimalloc normal 32 MiB segments */
    uint64_t huge_n, huge_commit, mi_reserved_only;       /* huge segments; mimalloc reserved-not-committed */
    uint64_t other_n, other_commit, bn[MT_NBUCKET], bc[MT_NBUCKET];
    uint64_t mapped_commit, image_commit, total_private_commit;
    uint64_t cur_base, cur_res, cur_com; int cur_type;    /* allocation being accumulated */
} mt_census;

static void mt_census_close(mt_census *c)
{
    if (!c->cur_res) return;
    if (c->cur_type == MT_MAPPED) c->mapped_commit += c->cur_com;
    else if (c->cur_type == MT_IMAGE) c->image_commit += c->cur_com;
    else {
        c->total_private_commit += c->cur_com;
        if (c->cur_base % MT_SEG == 0 && c->cur_res > (16ull << 20)) {
            if (c->cur_res == MT_SEG) { c->seg_n++; c->seg_commit += c->cur_com; c->seg_full_n += c->cur_com == MT_SEG; }
            else { c->huge_n++; c->huge_commit += c->cur_com; }
            c->mi_reserved_only += c->cur_res - c->cur_com;
        } else if (c->cur_com) {
            int b = c->cur_com < (1ull << 20) ? 0 : c->cur_com < (16ull << 20) ? 1 : c->cur_com < (64ull << 20) ? 2 : 3;
            c->other_n++; c->other_commit += c->cur_com; c->bn[b]++; c->bc[b] += c->cur_com;
        }
    }
    c->cur_res = c->cur_com = 0;
}
static void mt_census_region(mt_census *c, uint64_t alloc_base, uint64_t size, int committed, int type)
{
    if (c->cur_res && alloc_base != c->cur_base) mt_census_close(c);
    if (!c->cur_res) { c->cur_base = alloc_base; c->cur_type = type; }
    c->cur_res += size;
    if (committed) c->cur_com += size;
}
static void mt_census_finish(mt_census *c) { mt_census_close(c); }

/* Census schedule: returns 1 when due and moves *next forward. every_min <= 0 = never. */
static int mt_census_due(uint64_t now_ms, uint64_t *next_ms, int every_min)
{
    if (every_min <= 0) return 0;
    if (!*next_ms) { *next_ms = now_ms + (uint64_t)every_min * 60000; return 0; }
    if (now_ms < *next_ms) return 0;
    *next_ms = now_ms + (uint64_t)every_min * 60000;
    return 1;
}

/* ---------------------------------------------------------------- GUI animation-state pool (diag)
 * PdxGuiAnimation slot pool insert (1.15.1 0x39304d0, docs/re/memory.md section 10):
 * pool(rcx) = { +0 elem*, +8 int cap, +0xc int used, +0x10 allocator*, +0x18 mutex,
 *               +0x68 int* free_idx, +0x74 int free_n }, element 0x308 bytes, array grows x1.5.
 * Anchor: unique bytes at fn+0x58 (pop free index ; imul rbx,rbp,0x308), '?' = wildcard. */
#define MT_POOL_ELEM 0x308
#define MT_POOL_PAT_OFF 0x58
static const unsigned char MT_POOL_PAT[] = {
    0x41,0x8B,0x56,0x74, 0x85,0xD2, 0x7E,0x00, 0x48,0x63,0xCA, 0x49,0x8B,0x46,0x68, 0x48,0x63,0x6C,0x88,0xFC,
    0x8D,0x42,0xFF, 0x41,0x89,0x46,0x74, 0x48,0x69,0xDD,0x08,0x03,0x00,0x00 };
static const char MT_POOL_MASK[] = "xxxxxxx?xxxxxxxxxxxxxxxxxxxxxxxxxx";
static const unsigned char MT_SIG_POOL_PROLOGUE[] = { 0x48,0x89,0x5C,0x24,0x08, 0x48,0x89,0x6C,0x24,0x10, 0x48,0x89,0x74,0x24,0x18,
                                                      0x48,0x89,0x7C,0x24,0x20, 0x41,0x55, 0x41,0x56, 0x41,0x57 };
static int mt_match(const unsigned char *p, const unsigned char *pat, const char *mask, size_t n)
{
    for (size_t i = 0; i < n; i++) if (mask[i] == 'x' && p[i] != pat[i]) return 0;
    return 1;
}
/* Returns offset of the single match in [code, code+n), -1 none, -2 ambiguous. */
static ptrdiff_t mt_find_unique(const unsigned char *code, size_t n, const unsigned char *pat, const char *mask, size_t pn)
{
    ptrdiff_t f = -1;
    for (size_t i = 0; i + pn <= n; i++)
        if (code[i] == pat[0] && mt_match(code + i, pat, mask, pn)) { if (f >= 0) return -2; f = (ptrdiff_t)i; }
    return f;
}
typedef struct { uint64_t data; int32_t cap, used; } mt_pool_hdr;
/* Sanity-check a pool snapshot; fills live (used - free) and array bytes. 0 = plausible. */
static int mt_pool_check(const unsigned char hdr[0x78], long long *live, long long *array_bytes, long long *cap_out)
{
    uint64_t data; int32_t cap, used, freen;
    memcpy(&data, hdr, 8); memcpy(&cap, hdr + 8, 4); memcpy(&used, hdr + 0xc, 4); memcpy(&freen, hdr + 0x74, 4);
    if (!data || cap <= 0 || used < 0 || used > cap || freen < 0 || freen > used || cap > (1 << 26)) return 1;
    *live = used - freen; *array_bytes = (long long)cap * MT_POOL_ELEM; *cap_out = cap;
    return 0;
}

/* Signatures used for validation (verified on 1.15.1, see docs/re/memory.md). */
static const unsigned char MT_SIG_GET_NEW_HANDLER[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8D, 0x0D };
static const unsigned char MT_SIG_MI_COLLECT[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0xB6, 0xF9 };

#endif
