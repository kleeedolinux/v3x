/* Offline test of mem_tuner's table walk and byte patterns (no game run).
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -o D:/v3x_tmp/mem/mtt.exe test/mem_tuner_test.c
 *   D:/v3x_tmp/mem/mtt.exe ["<game>/binaries/victoria3.exe"]     -> prints ALL OK
 * With the exe path it also maps the real exe (DONT_RESOLVE_DLL_REFERENCES: relocated, no code runs)
 * and resolves every anchor exactly the way the plugin does.
 *   D:/v3x_tmp/mem/mtt.exe "<exe>" D:/v3x_tmp/mem/mem_tuner.dll [api_version]
 * also loads the built plugin and runs v3x_plugin_load against a fake V3X_API over the mapped exe
 * (hooks are recorded, not installed; no game code runs): checks option writes, hook targets and
 * the API v3 "load_transition" subscription (api_version 3, default) vs own hook (api_version 2). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include "../plugins/mem_tuner/mt_core.h"
#include "../plugins/mem_tuner/mt_mi.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* ---------------- fake image: strings + data in one buffer, VA = buffer address */
static unsigned char img[0x4000];
static const char *fake_str(void *ctx, uint64_t va)
{
    (void)ctx;
    if (va < (uint64_t)img || va >= (uint64_t)img + sizeof img) return NULL;
    return (const char *)va;
}
static uint64_t put_str(size_t *at, const char *s) { size_t n = strlen(s) + 1; memcpy(img + *at, s, n); uint64_t va = (uint64_t)(img + *at); *at += (n + 7) & ~7u; return va; }
static void build_table(size_t toff, uint64_t *names)
{
    for (int i = 0; i < MT_NOPT; i++) {
        mt_entry e = { i == 3 ? 1 : 0, 0, i, 0, names[i], 0 };
        memcpy(img + 0x1000 + toff + (size_t)i * MT_ENTRY, &e, sizeof e);   /* data section = img + 0x1000 */
    }
}

static void test_fake(void)
{
    size_t at = 0; uint64_t names[MT_NOPT];
    for (int i = 0; i < MT_NOPT; i++) names[i] = put_str(&at, MT_NAMES[i]);
    unsigned char *data = img + 0x1000; size_t dn = 0x2000;
    memset(data, 0, dn);
    build_table(0x100, names);
    ptrdiff_t off = mt_find_table(data, dn, names[0], fake_str, NULL);
    CHECK(off == 0x100);
    /* decoy pointer to "show_errors" elsewhere (not a table) is ignored */
    memcpy(data + 0x808, &names[0], 8);
    CHECK(mt_find_table(data, dn, names[0], fake_str, NULL) == 0x100);
    /* a second valid table -> ambiguous -> fail closed */
    build_table(0xA00, names);
    CHECK(mt_find_table(data, dn, names[0], fake_str, NULL) == -2);
    memset(data + 0xA00, 0, (size_t)MT_NOPT * MT_ENTRY);
    /* wrong name (different mimalloc version) -> not found */
    uint64_t other = put_str(&at, "purge_delay");
    memcpy(data + 0x100 + 15 * MT_ENTRY + 0x10, &other, 8);
    CHECK(mt_validate(data + 0x100, fake_str, NULL) == 16);
    CHECK(mt_find_table(data, dn, names[0], fake_str, NULL) == -1);
    memcpy(data + 0x100 + 15 * MT_ENTRY + 0x10, &names[15], 8);
    /* wrong option index -> not found */
    int32_t bad = 7; memcpy(data + 0x100 + 6 * MT_ENTRY + 8, &bad, 4);
    CHECK(mt_find_table(data, dn, names[0], fake_str, NULL) == -1);
    bad = 6; memcpy(data + 0x100 + 6 * MT_ENTRY + 8, &bad, 4);
    /* truncated section -> not found */
    CHECK(mt_find_table(data, 0x100 + 25 * MT_ENTRY, names[0], fake_str, NULL) == -1);
    CHECK(mt_find_table(data, dn, names[0], fake_str, NULL) == 0x100);

    /* byte patterns, copied from victoria3.exe 1.15.1 */
    static const unsigned char done[] = { 0x48,0x83,0xEC,0x28, 0xE8,0xBA,0x0C,0x00,0x00, 0xB1,0x01, 0xE8,0x03,0x59,0x00,0x00 };
    /* at va 0x43deda1: call; at 0x43deda6: mov cl,1; call 0x43e46b0 */
    CHECK(mt_find_movcl1_call(done + 4, sizeof done - 4, 0x43deda1) == 0x43e46b0);
    static const unsigned char em[] = { 0x48,0x8D,0x15,0xB7,0x51,0x26,0x00, 0xB9,0x0C,0x00,0x00,0x00, 0xE8,0xFD,0x05,0x00,0x00 };
    CHECK(mt_find_errmsg_call(em, sizeof em, 0x43d9402, 0x463e5c0) == 0x43d9a10);
    CHECK(mt_find_errmsg_call(em, sizeof em, 0x43d9402, 0x463e5c8) == 0);
    static const unsigned char eh[] = { 0x48,0x8B,0x05,0xE2,0xF9,0x7D,0x01, 0x8B,0xCF, 0x48,0x8B,0x15,0xE1,0xF9,0x7D,0x01, 0x90, 0xFF,0xD0 };
    CHECK(mt_find_error_handler_global(eh, sizeof eh, 0x43d9b37) == 0x5bb9520);
    unsigned char eh2[sizeof eh]; memcpy(eh2, eh, sizeof eh); eh2[12] ^= 0x10;   /* arg global not handler+8 */
    CHECK(mt_find_error_handler_global(eh2, sizeof eh2, 0x43d9b37) == 0);
}

/* ---------------- periodic trigger + census (pure logic) */
static void test_periodic_census(void)
{
    mt_periodic p; memset(&p, 0, sizeof p); p.grow_mb = 256; p.min_interval_ms = 300000;
    CHECK(!mt_periodic_due(&p, 1000, 16000));                 /* first sample: baseline */
    CHECK(!mt_periodic_due(&p, 400000, 16200));               /* +200 < 256 */
    p.last_ms = 1000;
    CHECK(mt_periodic_due(&p, 400000, 16300));                /* +300 after 6.6 min */
    CHECK(!mt_periodic_due(&p, 100000, 16300));               /* interval not reached (last 1000 -> 99 s) */
    mt_periodic_done(&p, 400000, 16100);
    CHECK(!mt_periodic_due(&p, 800000, 16300));               /* +200 over new base */
    CHECK(!mt_periodic_due(&p, 800000, 15900));               /* trough lowers the base ... */
    CHECK(mt_periodic_due(&p, 800000, 16200));                /* ... so a spike over it counts: +300 */
    CHECK(!mt_periodic_due(&p, 600000, 16200));               /* < 5 min after the collect at 400000 */

    mt_census c; memset(&c, 0, sizeof c);
    const uint64_t M = 1ull << 20, S = 32 * M;
    uint64_t a = 0x5AF60000000ull;                            /* 32 MiB aligned */
    /* normal segment, fully committed (2 regions) */
    mt_census_region(&c, a, 8 * M, 1, MT_PRIVATE); mt_census_region(&c, a, 24 * M, 1, MT_PRIVATE);
    /* normal segment, partially committed */
    mt_census_region(&c, a + S, 4 * M, 1, MT_PRIVATE); mt_census_region(&c, a + S, 28 * M, 0, MT_PRIVATE);
    /* huge segment 120 MiB */
    mt_census_region(&c, a + 2 * S, 120 * M, 1, MT_PRIVATE);
    /* unaligned 32 MiB fully committed (driver-like) + small heap-like + mapped + image */
    mt_census_region(&c, 0x26AAFAB0000ull, 32 * M, 1, MT_PRIVATE);
    mt_census_region(&c, 0x26A94FC0000ull, 64 * 1024, 1, MT_PRIVATE); mt_census_region(&c, 0x26A94FC0000ull, 1 * M, 0, MT_PRIVATE);
    mt_census_region(&c, 0x26A99A10000ull, 128 * M, 1, MT_PRIVATE);
    mt_census_region(&c, 0x7FF600000000ull, 2 * M, 1, MT_MAPPED);
    mt_census_region(&c, 0x7FF6A3280000ull, 97 * M, 1, MT_IMAGE);
    /* aligned but small (16 MiB) -> not mimalloc */
    mt_census_region(&c, 0x7FF700000000ull, 16 * M, 1, MT_PRIVATE);
    mt_census_finish(&c);
    CHECK(c.seg_n == 2 && c.seg_full_n == 1 && c.seg_commit == 36 * M);
    CHECK(c.huge_n == 1 && c.huge_commit == 120 * M && c.mi_reserved_only == 28 * M);
    CHECK(c.other_n == 4 && c.other_commit == 32 * M + 64 * 1024 + 128 * M + 16 * M);
    CHECK(c.bn[0] == 1 && c.bn[1] == 0 && c.bn[2] == 2 && c.bn[3] == 1 && c.bc[3] == 128 * M);
    CHECK(c.mapped_commit == 2 * M && c.image_commit == 97 * M);
    CHECK(c.total_private_commit == c.seg_commit + c.huge_commit + c.other_commit);

    /* census schedule: every 10 min, first one 10 min after start */
    uint64_t next = 0;
    CHECK(!mt_census_due(1000, &next, 10) && next == 601000);
    CHECK(!mt_census_due(600999, &next, 10));
    CHECK(mt_census_due(601000, &next, 10) && next == 1201000);
    CHECK(!mt_census_due(900000, &next, 10));
    CHECK(mt_census_due(1230000, &next, 10) && next == 1830000);
    uint64_t n0 = 0; CHECK(!mt_census_due(5, &n0, 0) && !mt_census_due(99999999, &n0, 0));   /* disabled */

    /* pool pattern + header check */
    unsigned char code[0x200]; memset(code, 0xCC, sizeof code);
    memcpy(code + 0x40, MT_POOL_PAT, sizeof MT_POOL_PAT); code[0x47] = 0x77;   /* wildcard byte differs */
    CHECK(mt_find_unique(code, sizeof code, MT_POOL_PAT, MT_POOL_MASK, sizeof MT_POOL_PAT) == 0x40);
    memcpy(code + 0x100, MT_POOL_PAT, sizeof MT_POOL_PAT);
    CHECK(mt_find_unique(code, sizeof code, MT_POOL_PAT, MT_POOL_MASK, sizeof MT_POOL_PAT) == -2);
    code[0x100 + 30] = 0x04;                                  /* element size 0x408 -> no match */
    CHECK(mt_find_unique(code, sizeof code, MT_POOL_PAT, MT_POOL_MASK, sizeof MT_POOL_PAT) == 0x40);
    CHECK(sizeof MT_POOL_PAT == strlen(MT_POOL_MASK));
    unsigned char h[0x78]; memset(h, 0, sizeof h); long long live, bytes, cap;
    uint64_t d = 0x5BAE8000000ull; int32_t v;
    memcpy(h, &d, 8); v = 708000; memcpy(h + 8, &v, 4); v = 700000; memcpy(h + 0xc, &v, 4); v = 1000; memcpy(h + 0x74, &v, 4);
    CHECK(mt_pool_check(h, &live, &bytes, &cap) == 0 && live == 699000 && cap == 708000 && bytes == 708000LL * 0x308);
    v = 800000; memcpy(h + 0xc, &v, 4);                       /* used > cap -> garbage */
    CHECK(mt_pool_check(h, &live, &bytes, &cap) != 0);
}

/* ---------------- mimalloc segment parser (synthetic 2.0.x header) */
static unsigned char seghdr[MI_HDR_BYTES];
static void put64(unsigned char *p, uint64_t v) { memcpy(p, &v, 8); }
static void put32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }
static void slice(unsigned char *h, int i, uint32_t cnt, uint32_t bs, uint32_t used)
{
    unsigned char *s = h + MI_O_SLICES + (size_t)i * MI_SLICE;
    put32(s + MI_P_COUNT, cnt); put32(s + MI_P_BSIZE, bs); put32(s + MI_P_USED, used);
}
static void build_seg(unsigned char *h, uint64_t va, uint64_t key)
{
    memset(h, 0, MI_HDR_BYTES);
    put64(h + MI_O_COOKIE, va ^ key); put64(h + MI_O_SEGSLICES, 512); put64(h + MI_O_INFOSLICES, 1);
    put32(h + MI_O_KIND, 0); put64(h + MI_O_ENTRIES, 512); put64(h + MI_O_THREAD, 0x7000);
    put64(h + MI_O_CMASK, 0x3ff);                              /* slices 0..9 committed */
    h[MI_O_CMASK + 300 / 8] |= 1 << (300 % 8);                 /* + slice 300 (inside the free span) */
    h[MI_O_DMASK + 300 / 8] |= 1 << (300 % 8);                 /* ... scheduled for decommit */
    slice(h, 0, 1, 65536, 1);                                  /* segment info */
    slice(h, 1, 2, 64, 1000);                                  /* 64 B blocks: 64000 in use of 128 KiB */
    slice(h, 3, 1, 1024, 0);                                   /* empty page */
    slice(h, 4, 6, 393216, 1);                                 /* one 384 KiB block */
    slice(h, 10, 502, 0, 0);                                   /* free span */
}
static void test_mi_parse(void)
{
    const uint64_t K = 1024, va = 0x5AF60000000ull, key = 0x1234567;
    mt_mi_sum s; memset(&s, 0, sizeof s);
    build_seg(seghdr, va, key);
    CHECK(mt_mi_parse(&s, seghdr, va, key, 704 * K) == 0);
    CHECK(s.segs == 1 && s.bad == 0 && s.mismatch == 0 && s.committed == 704 * K && s.vq_committed == 704 * K);
    CHECK(s.meta == 64 * K && s.used == 64000 + 384 * K && s.slack == 128 * K - 64000 && s.empty == 64 * K);
    CHECK(s.free_committed == 64 * K && s.pending == 64 * K && s.pages == 3 && s.pages_empty == 1);
    CHECK(s.meta + s.used + s.slack + s.empty + s.free_committed == s.committed);
    CHECK(s.hist_used[mt_mi_bin(64)] == 64000 && s.hist_slack[mt_mi_bin(1024)] == 64 * K && s.hist_pages[mt_mi_bin(393216)] == 1);
    CHECK(s.nthr == 1 && s.thr[0].tid == 0x7000 && s.thr[0].committed == 704 * K && s.thr[0].used == 64000 + 384 * K);
    CHECK(mt_mi_parse(&s, seghdr, va, key, 32 << 20) == 0 && s.mismatch == 1);       /* VirtualQuery disagrees */
    CHECK(mt_mi_parse(&s, seghdr, va, key + 1, 704 * K) == -1 && s.bad == 1);        /* wrong cookie */
    CHECK(mt_mi_parse(&s, seghdr, va + 0x10000, 0, 704 * K) == -1);                 /* unaligned */
    slice(seghdr, 4, 0, 0, 0);                                                        /* torn: zero count */
    CHECK(mt_mi_parse(&s, seghdr, va, key, 704 * K) == -1);
    build_seg(seghdr, va, key); slice(seghdr, 1, 2, 64, 5000);                       /* used*bs > span */
    CHECK(mt_mi_parse(&s, seghdr, va, key, 704 * K) == -1);
    build_seg(seghdr, va, key); put32(seghdr + MI_O_KIND, 1); put64(seghdr + MI_O_SEGSLICES, 600);
    CHECK(mt_mi_parse(&s, seghdr, va, key, 40 << 20) == 1 && s.huge == 1 && s.huge_committed == (40 << 20));
    build_seg(seghdr, va, key); put64(seghdr + MI_O_THREAD, 0);
    uint64_t ab = s.abandoned_segs;
    CHECK(mt_mi_parse(&s, seghdr, va, 0, 704 * K) == 0 && s.abandoned_segs == ab + 1);   /* key 0 = unchecked */
    char b[1200]; mt_mi_os os = { 9000ll << 20, 9100ll << 20, 12000ll << 20, 12100ll << 20, 40, 123456 };
    mt_mi_format(&s, &os, 1, b, sizeof b);
    printf("format: %s\n", b);
    CHECK(strstr(b, "mimalloc os: committed 9000 MiB (peak 9100), reserved 12000 MiB") && strstr(b, "IN USE "));
    mt_mi_format_hist(&s, b, sizeof b);
    CHECK(strstr(b, "<=64 ") != NULL);
    /* anchors: patterns in a fake code buffer */
    static unsigned char code[0x400]; memset(code, 0xCC, sizeof code);
    const unsigned char st[] = { 0x48,0x8D,0x3D,0x10,0x00,0x00,0x00, 0x4C,0x8B,0x10, 0x49,0x81,0xC2,0xC0,0x03,0x00,0x00, 0x4C,0x3B,0xD7 };
    memcpy(code + 0x20, st, sizeof st);
    mt_mi_anchors A; int got = mt_mi_anchor(code, sizeof code, 0x1000, &A);
    CHECK(got == 1 && A.stats == 0x1000 + 0x20 + 7 + 0x10);
    memcpy(code + 0x100, st, sizeof st);
    CHECK(mt_mi_anchor(code, sizeof code, 0x1000, &A) == 0);                          /* ambiguous -> not found */
    os.committed = -1; CHECK(mt_mi_os_read(0, &os) == -1);
}

/* ---------------- real exe */
static unsigned char *g_base; static size_t g_size;
static const char *real_str(void *ctx, uint64_t va)
{
    (void)ctx;
    if (va < (uint64_t)g_base || va >= (uint64_t)g_base + g_size) return NULL;
    return (const char *)va;
}
static const unsigned char *real_peek(void *ctx, uint64_t va, size_t n) { (void)ctx; return va >= (uint64_t)g_base && va + n <= (uint64_t)g_base + g_size ? (const unsigned char *)va : NULL; }
static int sect(const char *name, unsigned char **p, size_t *n)
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
        if (!strncmp((const char *)s->Name, name, 8)) { *p = g_base + s->VirtualAddress; *n = s->Misc.VirtualSize; return 1; }
    return 0;
}
static unsigned char *find_str(const char *s)   /* exact NUL-terminated string in .rdata */
{
    unsigned char *p; size_t n, l = strlen(s) + 1;
    if (!sect(".rdata", &p, &n)) return NULL;
    for (size_t i = 1; i + l <= n; i++) if (p[i - 1] == 0 && p[i] == (unsigned char)s[0] && !memcmp(p + i, s, l)) return p + i;
    return NULL;
}
static unsigned char *find_xref(const void *t)    /* first RIP-relative lea/mov to t in .text */
{
    unsigned char *p; size_t n;
    if (!sect(".text", &p, &n)) return NULL;
    for (size_t i = 0; i + 7 <= n; i++)
        if ((p[i] == 0x48 || p[i] == 0x4C) && (p[i + 1] == 0x8D || p[i + 1] == 0x8B) && (p[i + 2] & 0xC7) == 0x05 &&
            p + i + 7 + mt_rd32(p + i + 3) == (const unsigned char *)t) return p + i;
    return NULL;
}
static unsigned char *fstart(const void *a)       /* .pdata lookup incl. chained entries */
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *d = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    RUNTIME_FUNCTION *rf = (RUNTIME_FUNCTION *)(g_base + d->VirtualAddress);
    size_t cnt = d->Size / sizeof *rf; DWORD r = (DWORD)((const unsigned char *)a - g_base);
    size_t lo = 0, hi = cnt;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (rf[m].EndAddress <= r) lo = m + 1; else hi = m; }
    if (lo >= cnt || rf[lo].BeginAddress > r) return NULL;
    RUNTIME_FUNCTION *e = &rf[lo];
    for (int k = 0; k < 16; k++) {
        unsigned char *u = g_base + e->UnwindData;
        if (!((u[0] >> 3) & 4)) break;
        e = (RUNTIME_FUNCTION *)(u + 4 + ((u[2] + 1) & ~1) * 2);
    }
    return g_base + e->BeginAddress;
}
static unsigned char *g_pool_fn;
static uint64_t g_mi_stats; static void **g_mi_slot;
static unsigned char *g_tf; static void *g_gnh, *g_col; static mt_entry *g_tab; static void **g_eh;   /* for test_plugin */
#define RVA(p) ((unsigned long long)((const unsigned char *)(p) - g_base))

static void test_real(const char *exe)
{
    HMODULE h = LoadLibraryExA(exe, NULL, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(h != NULL); if (!h) return;
    g_base = (unsigned char *)h;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    g_size = nt->OptionalHeader.SizeOfImage;
    printf("mapped at %p (preferred %llx)\n", (void *)g_base, (unsigned long long)nt->OptionalHeader.ImageBase);

    unsigned char *data; size_t dn;
    unsigned char *s0 = find_str("show_errors");
    CHECK(s0 && sect(".data", &data, &dn));
    ptrdiff_t off = (s0 && data) ? mt_find_table(data, dn, (uint64_t)s0, real_str, NULL) : -1;
    CHECK(off >= 0);
    if (off >= 0) {
        mt_entry *t = (mt_entry *)(data + off);
        printf("option table rva %llx: eager_commit=%d decommit_delay=%d segment_decommit_delay=%d init0=%d\n",
               RVA(t), t[MT_EAGER_COMMIT].value, t[MT_DECOMMIT_DELAY].value, t[MT_SEGMENT_DECOMMIT_DELAY].value, t[0].init);
        CHECK(t[MT_EAGER_COMMIT].value == 1 && t[MT_DECOMMIT_DELAY].value == 25 && t[MT_SEGMENT_DECOMMIT_DELAY].value == 500);
        for (int i = 0; i < MT_NOPT; i++) CHECK(t[i].init == 0);
    }

    unsigned char *ps = find_str("process done: 0x%zx\n"), *px = ps ? find_xref(ps) : NULL, *pf = px ? fstart(px) : NULL;
    uint64_t col = pf ? mt_find_movcl1_call(pf, (size_t)(px - pf), (uint64_t)pf) : 0;
    CHECK(col && !memcmp((void *)col, MT_SIG_MI_COLLECT, sizeof MT_SIG_MI_COLLECT));
    printf("mi_process_done rva %llx, mi_collect rva %llx\n", pf ? RVA(pf) : 0ULL, col ? RVA(col) : 0ULL);

    unsigned char *os = find_str("out of memory in 'new'"), *ox = os ? find_xref(os) : NULL, *of = ox ? fstart(ox) : NULL;
    uint64_t gnh = of ? mt_find_call_to(of, (size_t)(ox - of), (uint64_t)of, MT_SIG_GET_NEW_HANDLER, sizeof MT_SIG_GET_NEW_HANDLER, real_peek, NULL) : 0;
    CHECK(gnh != 0);
    uint64_t em = ox ? mt_find_errmsg_call(ox, 0x20, (uint64_t)ox, (uint64_t)os) : 0;
    uint64_t eh = em ? mt_find_error_handler_global((const unsigned char *)em, 0x200, em) : 0;
    CHECK(em && eh);
    printf("mi_heap_try_new rva %llx, get_new_handler rva %llx, _mi_error_message rva %llx, mi_error_handler rva %llx\n",
           of ? RVA(of) : 0ULL, gnh ? RVA(gnh) : 0ULL, em ? RVA(em) : 0ULL, eh ? RVA(eh) : 0ULL);

    unsigned char *ts = find_str("Transition {} took: {} seconds"), *tx = ts ? find_xref(ts) : NULL, *tf = tx ? fstart(tx) : NULL;
    CHECK(tf != NULL);
    printf("transition fn rva %llx\n", tf ? RVA(tf) : 0ULL);
    /* GUI animation pool insert: unique pattern, fn = 0x39304d0, prologue, and the call in 0x391a160 targets it */
    unsigned char *tx_; size_t txn;
    CHECK(sect(".text", &tx_, &txn));
    LARGE_INTEGER q0, q1, qf; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
    ptrdiff_t po = mt_find_unique(tx_, txn, MT_POOL_PAT, MT_POOL_MASK, sizeof MT_POOL_PAT);
    QueryPerformanceCounter(&q1);
    printf("pool pattern scan over %zu MiB .text: %.1f ms\n", txn >> 20, (q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart);
    CHECK(po >= MT_POOL_PAT_OFF);
    if (po >= MT_POOL_PAT_OFF) {
        g_pool_fn = tx_ + po - MT_POOL_PAT_OFF;
        printf("gui anim pool insert rva %llx\n", RVA(g_pool_fn));
        CHECK(RVA(g_pool_fn) == 0x39304d0 && fstart(g_pool_fn + MT_POOL_PAT_OFF) == g_pool_fn);
        CHECK(!memcmp(g_pool_fn, MT_SIG_POOL_PROLOGUE, sizeof MT_SIG_POOL_PROLOGUE));
        unsigned char *cs = g_base + 0x391a545;
        CHECK(cs[0] == 0xE8 && cs + 5 + mt_rd32(cs + 1) == g_pool_fn);
    }
    /* mimalloc stats / cookie key / deferred-free slot inside the mimalloc code window */
    if (col) {
        mt_mi_anchors A; int got = mt_mi_anchor((unsigned char *)col - 0x20000, 0x40000, col - 0x20000, &A);
        printf("mimalloc anchors: got %d, stats rva %llx, cookie key rva %llx, deferred fn rva %llx arg %llx\n", got,
               RVA(A.stats), RVA(A.cookie_key), RVA(A.deferred_fn), RVA(A.deferred_arg));
        CHECK(got == 7 && RVA(A.stats) == 0x5bb96c0 && RVA(A.cookie_key) == 0x5409628);
        CHECK(RVA(A.deferred_fn) == 0x5bb9648 && RVA(A.deferred_arg) == 0x5bb9650);
        CHECK(*(void **)A.deferred_fn == NULL);                                  /* nothing registered statically */
        mt_mi_os os; CHECK(mt_mi_os_read(A.stats, &os) == 0 && os.committed == 0);   /* image not running: zeros */
        /* the 1.15.1 layout constants the parser relies on, straight from the code */
        const unsigned char *mf = g_base + 0x43d94b0;
        CHECK(!memcmp(mf + 0x26, "\x49\x8b\x92\x00\x01\x00\x00\x49\x8d\x8a\x08\x01\x00\x00", 14));   /* thread_id +0x100, slices +0x108 */
        CHECK(!memcmp(g_base + 0x43db160, "\x0f\x11\x43\x70", 4) && !memcmp(g_base + 0x43db1a6, "\x0f\x11\x43\x30", 4)); /* commit/decommit mask */
        g_mi_stats = A.stats; g_mi_slot = (void **)A.deferred_fn;
    }
    g_tf = tf; g_gnh = (void *)gnh; g_col = (void *)col; g_tab = off >= 0 ? (mt_entry *)(data + off) : NULL; g_eh = (void **)eh;
}

/* ---------------- plugin under a fake API */
#include "../include/v3x.h"
static void *hooked[8]; static int nhooked, nframes, nevents, ncmds;
static v3x_frame_fn frame_cb; static void (*event_cb)(const char *, void *, void *);
static char logbuf[16384];
static void f_log(const char *tag, const char *fmt, ...)
{
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("  [%s] %s\n", tag, b); strncat(logbuf, b, sizeof logbuf - strlen(logbuf) - 2); strcat(logbuf, "\n");
}
static uintptr_t f_base(void) { return (uintptr_t)g_base; }
static size_t f_size(void) { return g_size; }
static const char *f_ver(void) { return "test"; }
static void *f_scan(const char *p) { (void)p; return NULL; }
static void *f_str(const char *s) { return find_str(s); }
static void *f_xref(const void *t, int i) { return i == 0 ? find_xref(t) : NULL; }
static void *f_fs(const void *a) { return fstart(a); }
static int stub_calls;
static int pool_stub(void *pool, void *src) { (void)pool; (void)src; stub_calls++; return 42; }
static int (*pool_detour)(void *, void *);
static int f_hook(void *t, void *d, void **o)
{
    hooked[nhooked++ & 7] = t; *o = t;
    if (t == (void *)g_pool_fn) { pool_detour = (int (*)(void *, void *))d; *o = (void *)pool_stub; }   /* never run game code */
    return 0;
}
static int f_unhook(void *t) { (void)t; return 0; }
static int f_patch(void *a, const void *b, size_t n) { (void)a; (void)b; (void)n; return 0; }
static int f_frame(v3x_frame_fn fn, void *u) { (void)u; frame_cb = fn; nframes++; return 0; }
static char tmpdir[MAX_PATH];
static const char *f_dir(void) { return tmpdir; }
static v3x_command_fn memstat_cb;
static int f_cmd(const char *n, const char *h, v3x_command_fn fn, void *u) { (void)h; (void)u; if (!strcmp(n, "memstat")) memstat_cb = fn; ncmds++; return 0; }
static char printbuf[16384];
static int f_print(const char *fmt, ...)    /* appends: memstat prints several lines */
{
    char b[2048]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); printf("  print: %s\n", b);
    strncat(printbuf, b, sizeof printbuf - strlen(printbuf) - 2); strcat(printbuf, "\n"); return 0;
}
static int f_main(v3x_frame_fn fn, void *u) { (void)fn; (void)u; return 0; }
static int f_exec(const char *l) { (void)l; return -1; }
static int f_cap(const char *l, void (*sk)(const char *, void *), void *u) { (void)l; (void)sk; (void)u; return -1; }
static int f_on_event(const char *n, void (*fn)(const char *, void *, void *), void *u) { (void)u; if (strcmp(n, "load_transition")) return -1; event_cb = fn; nevents++; return 0; }
static int f_emit(const char *n, void *d) { (void)n; (void)d; return 0; }

static void test_plugin(const char *dll, int ver)
{
    CHECK(g_base && g_tab && g_gnh && g_tf);
    if (!g_base || !g_tab) return;
    nhooked = nframes = nevents = ncmds = 0; frame_cb = NULL; event_cb = NULL; logbuf[0] = 0;
    GetTempPathA(sizeof tmpdir, tmpdir);           /* no mem_tuner.ini there -> built-in defaults */
    for (int i = 0; i < MT_NOPT; i++) { g_tab[i].init = 0; }
    g_tab[MT_EAGER_COMMIT].value = 1; g_tab[MT_DECOMMIT_DELAY].value = 25;
    void *eh_saved = *g_eh; *g_eh = NULL;
    V3X_API a = { (uint32_t)ver, sizeof(V3X_API), f_log, f_base, f_size, f_ver, f_scan, f_str, f_xref, f_fs, f_hook, f_unhook,
                  f_patch, f_frame, f_dir, f_dir, f_cmd, f_print, f_main, f_exec, f_cap, f_on_event, f_emit };
    HMODULE m = LoadLibraryA(dll);
    CHECK(m != NULL); if (!m) return;
    v3x_plugin_load_fn load = (v3x_plugin_load_fn)GetProcAddress(m, "v3x_plugin_load");
    CHECK(load && load(&a) == 0);
    CHECK(g_tab[MT_EAGER_COMMIT].value == 0 && g_tab[MT_EAGER_COMMIT].init == 2);
    CHECK(g_tab[MT_ABANDONED_PAGE_DECOMMIT].value == 1 && g_tab[MT_DECOMMIT_DELAY].value == 10);
    CHECK(g_tab[MT_SEGMENT_DECOMMIT_DELAY].value == 100 && g_tab[MT_DECOMMIT_EXTEND_DELAY].value == 0);
    CHECK(g_tab[MT_LARGE_OS_PAGES].value == 0 && g_tab[MT_LARGE_OS_PAGES].init == 0);
    CHECK(*g_eh != NULL);                                           /* error handler registered */
    int gnh_hooked = 0, tf_hooked = 0;
    for (int i = 0; i < nhooked; i++) { gnh_hooked |= hooked[i] == g_gnh; tf_hooked |= hooked[i] == (void *)g_tf; }
    CHECK(gnh_hooked);
    CHECK(ncmds == 2 && nframes == 1 && memstat_cb);           /* memcollect + memstat; one on_frame */
    CHECK(strstr(logbuf, "periodic mi_collect(force=0) on main thread when commit +256 MiB") && strstr(logbuf, ": on;"));
    CHECK(strstr(logbuf, "mimalloc walk: stats exe+0x5bb96c0, cookie key exe+0x5409628, deferred-free slot exe+0x5bb9648; header buffer ok"));
    CHECK(strstr(logbuf, "thread collect: deferred-free slot exe+0x5bb9648 set") != NULL);
    CHECK(g_mi_slot && *g_mi_slot != NULL);
    if (g_mi_slot && *g_mi_slot) {
        /* not armed before the first frame: the callback must return without calling mi_collect (game code) */
        void (*cb)(_Bool, unsigned long long, void *) = (void (*)(_Bool, unsigned long long, void *))*g_mi_slot;
        for (int i = 0; i < 1000; i++) cb(0, (unsigned long long)i, NULL);
        CHECK(1);
    }
    if (memstat_cb) {
        /* a fully committed 32 MiB-aligned 32 MiB reservation must show up as a mimalloc segment */
        char *r = VirtualAlloc(NULL, 64u << 20, MEM_RESERVE, PAGE_NOACCESS), *al = NULL;
        if (r) { al = (char *)(((uintptr_t)r + (32u << 20) - 1) & ~(uintptr_t)((32u << 20) - 1)); VirtualFree(r, 0, MEM_RELEASE);
                 al = VirtualAlloc(al, 32u << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }
        CHECK(al != NULL);
        printbuf[0] = 0; memstat_cb("", NULL);
        CHECK(strstr(printbuf, "census(console): private commit") && strstr(printbuf, "incl. OOM reserve 128 MiB"));
        unsigned long long segs = 0, full = 0;
        const char *q = strstr(printbuf, "= mimalloc ");
        CHECK(q && sscanf(q, "= mimalloc %llu x 32M segs %*llu MiB (%llu fully", &segs, &full) == 2 && full >= 1);
        /* zero header = not a segment (unparsed); a synthetic 2.0.x header parses (key global is 0 in the mapped image) */
        CHECK(strstr(printbuf, "mimalloc os: committed 0 MiB") && strstr(printbuf, "segments 0 (+0 huge 0 MiB, 1 unparsed 32 MiB"));
        if (al) {
            build_seg((unsigned char *)al, (uint64_t)(uintptr_t)al, 0);
            printbuf[0] = 0; memstat_cb("threads", NULL);
            CHECK(strstr(printbuf, "segments 1 (+0 huge 0 MiB, 0 unparsed 0 MiB, mask!=VQ 1): committed 0 MiB") != NULL);
            CHECK(strstr(printbuf, "mimalloc by owner thread") && strstr(printbuf, "[teb 7000] 0/0 (1)"));
            CHECK(strstr(printbuf, "thread collect armed/off, gen 1, 0 runs") != NULL);
        }
        if (al) VirtualFree(al, 0, MEM_RELEASE);
        /* pool diag: detour forwards to the original and the census reports the pool */
        CHECK(pool_detour != NULL && strstr(logbuf, "gui anim pool insert at exe+0x39304d0: diag hook installed"));
        if (pool_detour) {
            static unsigned char fake_pool[0x78]; uint64_t d = 0x5BAE8000000ull; int32_t v;
            memcpy(fake_pool, &d, 8); v = 708000; memcpy(fake_pool + 8, &v, 4); v = 700000; memcpy(fake_pool + 0xc, &v, 4);
            v = 1000; memcpy(fake_pool + 0x74, &v, 4);
            stub_calls = 0;
            CHECK(pool_detour(fake_pool, NULL) == 42 && pool_detour(fake_pool, NULL) == 42 && stub_calls == 2);
            printbuf[0] = 0; memstat_cb("", NULL);
            CHECK(strstr(printbuf, "gui anim pool 0: live 699000 of 708000 slots, array 523 MiB") != NULL);
            CHECK(strstr(printbuf, "gui anim pool 1") == NULL);   /* same pool registered once */
            v = 900000; memcpy(fake_pool + 0xc, &v, 4);          /* garbage -> dropped */
            printbuf[0] = 0; memstat_cb("", NULL);
            CHECK(strstr(printbuf, "gui anim pool 0: gone") != NULL);
        }
    }
    if (ver >= 3) {
        CHECK(nevents == 1 && event_cb && !tf_hooked);
        if (event_cb) {
            V3X_LoadTransition t = { "Empty", "Game", 171.7 };
            event_cb("load_transition", &t, NULL);
            CHECK(strstr(logbuf, "load_transition Empty -> Game") != NULL);
        }
    } else {
        CHECK(nevents == 0 && tf_hooked);
    }
    *g_eh = eh_saved;
    if (g_mi_slot) *g_mi_slot = NULL;               /* the DLL is unloaded below */
    FreeLibrary(m);                                 /* reserve leaks for the test process only */
}

int main(int argc, char **argv)
{
    test_fake();
    test_periodic_census();
    test_mi_parse();
    if (argc > 1) test_real(argv[1]);
    if (argc > 2) { int v = argc > 3 ? atoi(argv[3]) : 3; printf("plugin test, api_version %d\n", v); test_plugin(argv[2], v); }
    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails != 0;
}
