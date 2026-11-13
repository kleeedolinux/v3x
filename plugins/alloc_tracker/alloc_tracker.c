/* alloc_tracker: who holds the engine's memory? Diagnostic, default OFF (alloc_tracker.ini enabled=1).
 *
 * Hooks the engine's mimalloc entry points (docs/re/memory.md):
 *   mi_new          operator new   "out of memory in 'new'" -> mi_heap_try_new <-jmp- mi_heap_alloc_new <-jmp- mi_new
 *   mi_new_aligned  aligned new    function of the second xref of the same string
 *   mi_malloc       malloc         TLS-heap prologue + tail jmp to _mi_malloc_generic ("unable to allocate memory")
 *   mi_free         free/delete    unique byte pattern (leaf function, no .pdata)
 * calloc/realloc variants are only reachable through mimalloc's own pointer table (0 direct calls).
 *
 * Detour cost: small allocations = one TEB read/write + subtract (per-thread byte countdown kept in
 * a TLS slot read directly from the TEB, so GetLastError is untouched). Every `sample_kb` KiB of small
 * allocations one event is recorded with that weight. Allocations >= big_kb are always recorded and
 * kept in a live table so mi_free can subtract them: per call site we get LIVE bytes, not just totals.
 * Recorded events capture a short stack (RtlCaptureStackBackTrace) and are attributed to the first
 * frame outside allocator wrappers (std::_Allocate instances found by pattern + ini skip list).
 * No locks and no heap use inside detours; tables are VirtualAlloc'd once at load.
 *
 * Sampled small blocks also go into the live table (weight = sample period), so mi_free removes them
 * again: per site we get an unbiased LIVE estimate for small blocks too (small_live=1).
 *
 * The report adds mimalloc's own view (../mem_tuner/mt_mi.h): OS commit counter + segment walk
 * (in use / slack / empty pages / free spans / pending decommit), and (vm_owners=1) who reserved the
 * NON-mimalloc private memory: NtAllocateVirtualMemory(Ex) MEM_RESERVE calls are recorded by base
 * with the first return address outside ntdll/kernelbase/kernel32/CRT DLLs; committed private
 * allocations without a record were created by the kernel (GPU/driver mappings) or before the hook.
 * Every hook forwards 8 integer args (PASS8).
 *
 * Output: console `allocs [N]` (API v2) and <v3x>/alloc_report.txt every report_interval_s seconds
 * and on the "load_transition" event (API v3). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "v3x.h"
#include "at_core.h"
#include "../mem_tuner/mt_mi.h"

V3X_EXPORT const char *v3x_plugin_name = "alloc_tracker";
#define TAG "alloc_tracker"

static const V3X_API *api;
static unsigned char *g_base; static size_t g_size;
static char g_ini[MAX_PATH], g_report[MAX_PATH];
static at_tables T;
static volatile LONG g_on;
static int64_t g_big = 65536, g_period = 262144, g_stack_big = 1 << 20;
static DWORD g_tls_off;                    /* gs offset of our TEB TlsSlots entry */
#define MAX_SKIP 16384
static at_range g_skip[MAX_SKIP]; static int g_nskip;
static volatile LONG64 g_events_small, g_events_big, g_frees_matched, g_small_dropped;
static SRWLOCK g_report_lock = SRWLOCK_INIT;
static int g_small_live = 1;

#define PASS8_ARGS void *a0_, void *a1_, void *a2_, void *a3_, void *a4_, void *a5_, void *a6_, void *a7_
#define PASS8 a0_, a1_, a2_, a3_, a4_, a5_, a6_, a7_
typedef void *(*p8_fn)(PASS8_ARGS);
typedef LONG (*nt8_fn)(PASS8_ARGS);
static p8_fn o_new, o_malloc, o_new_al, o_free;

static int cfg(const char *k, int d) { return (int)GetPrivateProfileIntA("alloc_tracker", k, d, g_ini); }
static uint32_t to_rva(const void *p)
{
    uintptr_t a = (uintptr_t)p, b = (uintptr_t)g_base;
    return a >= b && a < b + g_size ? (uint32_t)(a - b) : 0;
}

/* ------------------------------------------------------------------ recording */

static __attribute__((noinline)) void record(void *p, size_t n, void *ret, int64_t weight, int big)
{
    void *raw[AT_FRAMES + 2]; uint32_t fr[AT_FRAMES + 3];
    USHORT k = RtlCaptureStackBackTrace(2, AT_FRAMES + 2, raw, NULL);   /* skip record + detour */
    int m = 0;
    fr[m++] = to_rva(ret);
    for (USHORT i = 0; i < k && m < AT_FRAMES + 3; i++) {
        uint32_t r = to_rva(raw[i]);
        if (r && r != fr[0]) fr[m++] = r;
    }
    uint32_t key = at_pick_frame(fr, m, g_skip, g_nskip);
    int s = at_site_get(&T, key ? key : 1);
    if (s >= 0 && (int64_t)n >= g_stack_big && !T.sites[s].frames_set &&
        InterlockedCompareExchange(&T.sites[s].frames_set, 1, 0) == 0)
        for (int i = 0; i < AT_FRAMES; i++) T.sites[s].frames[i] = i < m ? fr[i] : 0;
    if (big) {
        at_site_add(&T, s, (int64_t)n, 1, (int64_t)n, 1);
        at_live_insert(&T, (uint64_t)(uintptr_t)p, (int64_t)n, s, AT_BIG);
        InterlockedIncrement64(&g_events_big);
    } else {
        at_site_add(&T, s, weight, weight / (n ? (int64_t)n : 1), 0, 0);
        if (g_small_live && at_live_insert(&T, (uint64_t)(uintptr_t)p, weight, s, AT_SMALL)) InterlockedIncrement64(&g_small_dropped);
        InterlockedIncrement64(&g_events_small);
    }
}

static inline void after_alloc(void *p, size_t n, void *ret)
{
    if (!p || !g_on) return;
    if ((int64_t)n >= g_big) { record(p, n, ret, 0, 1); return; }
    int64_t left = (int64_t)__readgsqword(g_tls_off);
    if (left == 0) left = g_period;                         /* new thread */
    int64_t w = at_sample(&left, (int64_t)n, g_period);
    __writegsqword(g_tls_off, (unsigned long long)left);
    if (w) record(p, n, ret, w, 0);
}

static void *new_detour(PASS8_ARGS)
{
    void *p = o_new(PASS8);
    after_alloc(p, (size_t)a0_, __builtin_return_address(0));
    return p;
}
static void *malloc_detour(PASS8_ARGS)
{
    void *p = o_malloc(PASS8);
    after_alloc(p, (size_t)a0_, __builtin_return_address(0));
    return p;
}
static void *new_al_detour(PASS8_ARGS)            /* (size, alignment) */
{
    void *p = o_new_al(PASS8);
    after_alloc(p, (size_t)a0_, __builtin_return_address(0));
    return p;
}
static void free_detour(PASS8_ARGS)
{
    void *p = a0_;
    /* remove before freeing: after mi_free another thread may get the same address */
    if (p && T.live_blocks && at_live_remove(&T, (uint64_t)(uintptr_t)p)) InterlockedIncrement64(&g_frees_matched);
    o_free(PASS8);
}

/* ------------------------------------------------------------------ VM owners
 * NtAllocateVirtualMemory(Process, *Base, ZeroBits, *Size, Type, Protect)
 * NtAllocateVirtualMemoryEx(Process, *Base, *Size, Type, Protect, ExtParams, Count)
 * Only successful MEM_RESERVE calls on the current process are recorded (base = allocation base). */
#define VM_SLOTS (1u << 15)
static at_vm *g_vm; static volatile LONG64 g_vm_calls, g_vm_dropped;
static nt8_fn o_ntava, o_ntavaex;
static DWORD g_vm_tls;                              /* gs offset of a recursion flag */
typedef struct { uint64_t lo, hi; } va_range;
static va_range g_sys[8]; static int g_nsys;
static uint64_t g_mi_lo, g_mi_hi;                   /* mimalloc code range in the exe (owner = mimalloc) */

static int in_sys(uint64_t a) { for (int i = 0; i < g_nsys; i++) if (a >= g_sys[i].lo && a < g_sys[i].hi) return 1; return 0; }
static __attribute__((noinline)) void vm_note(void *base, SIZE_T size)
{
    if (__readgsqword(g_vm_tls)) return;
    __writegsqword(g_vm_tls, 1);
    void *fr[32]; USHORT k = RtlCaptureStackBackTrace(1, 32, fr, NULL);
    uint64_t owner = 0, exe = 0, b = (uint64_t)(uintptr_t)g_base;
    for (USHORT i = 0; i < k; i++) {
        uint64_t a = (uint64_t)(uintptr_t)fr[i];
        if (!owner && !in_sys(a)) owner = a;
        if (!exe && a >= b && a < b + g_size && !(a >= g_mi_lo && a < g_mi_hi)) exe = a;
        if (owner && exe) break;
    }
    if (at_vm_put(g_vm, VM_SLOTS, (uint64_t)(uintptr_t)base, owner, exe, size)) InterlockedIncrement64(&g_vm_dropped);
    InterlockedIncrement64(&g_vm_calls);
    __writegsqword(g_vm_tls, 0);
}
static LONG ntava_detour(PASS8_ARGS)
{
    LONG st = o_ntava(PASS8);
    if (st >= 0 && a0_ == (void *)(intptr_t)-1 && ((ULONG)(uintptr_t)a4_ & MEM_RESERVE) && a1_ && a3_)
        vm_note(*(void **)a1_, *(SIZE_T *)a3_);
    return st;
}
static LONG ntavaex_detour(PASS8_ARGS)
{
    LONG st = o_ntavaex(PASS8);
    if (st >= 0 && a0_ == (void *)(intptr_t)-1 && ((ULONG)(uintptr_t)a3_ & MEM_RESERVE) && a1_ && a2_)
        vm_note(*(void **)a1_, *(SIZE_T *)a2_);
    return st;
}
static void add_sys(HMODULE m)
{
    if (!m || g_nsys >= 8) return;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((unsigned char *)m + ((IMAGE_DOS_HEADER *)m)->e_lfanew);
    g_sys[g_nsys].lo = (uint64_t)(uintptr_t)m; g_sys[g_nsys].hi = g_sys[g_nsys].lo + nt->OptionalHeader.SizeOfImage; g_nsys++;
}

/* ------------------------------------------------------------------ anchors */

static int section(const char *name, unsigned char **p, size_t *n)
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
        if (!strncmp((const char *)s->Name, name, 8)) { *p = g_base + s->VirtualAddress; *n = s->Misc.VirtualSize; return 1; }
    return 0;
}
static int32_t rd32(const unsigned char *p) { int32_t v; memcpy(&v, p, 4); return v; }

/* first `jmp rel32` (E9) in [lo, hi) whose target is `t` */
static unsigned char *find_jmp_to(unsigned char *lo, unsigned char *hi, const unsigned char *t)
{
    for (unsigned char *c = lo; c + 5 <= hi; c++) if (*c == 0xE9 && c + 5 + rd32(c + 1) == t) return c;
    return NULL;
}

/* mi_malloc passes zero=false, huge_alignment=0 to _mi_malloc_generic: xor r9d,r9d; xor r8d,r8d
 * (the zero-initializing variant sets r8b=1 instead) */
static int has_zero_r8r9(const unsigned char *lo, const unsigned char *hi)
{
    static const unsigned char z[] = { 0x45, 0x33, 0xC9, 0x45, 0x33, 0xC0 };
    for (const unsigned char *c = lo; c + sizeof z <= hi; c++) if (!memcmp(c, z, sizeof z)) return 1;
    return 0;
}

/* mi_malloc checks size <= MI_SMALL_SIZE_MAX (cmp rbx,400h); mi_malloc_small (pointer table only,
 * 0 direct calls) does not */
static const unsigned char SIG_SMALL_CHECK[] = { 0x48, 0x81, 0xFB, 0x00, 0x04, 0x00, 0x00 };
static int has_bytes(const unsigned char *lo, const unsigned char *hi, const unsigned char *b, size_t n)
{
    for (const unsigned char *c = lo; c + n <= hi; c++) if (!memcmp(c, b, n)) return 1;
    return 0;
}

static const unsigned char SIG_TLS_HEAP[] = {            /* mov [rsp+8],rbx; push rdi; sub rsp,20h; mov rax,gs:[58h] */
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00 };
static const unsigned char SIG_NEW_AL[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF9 };

typedef struct { void *mi_new, *mi_new_al, *mi_malloc, *mi_free; int nwrap; } anchors;

static int find_anchors(anchors *A)
{
    memset(A, 0, sizeof *A);
    void *s = api->find_string("out of memory in 'new'");
    unsigned char *x0 = s ? api->find_xref(s, 0) : NULL, *x1 = s ? api->find_xref(s, 1) : NULL;
    unsigned char *f0 = x0 ? api->function_start(x0) : NULL, *f1 = x1 ? api->function_start(x1) : NULL;
    unsigned char *try_new = NULL, *al = NULL;
    /* mi_heap_try_new is the one with a 3rd (nothrow) argument; mi_new_aligned has the 0x40 frame */
    if (f0 && f1) {
        if (!memcmp(f1, SIG_NEW_AL, sizeof SIG_NEW_AL)) { try_new = f0; al = f1; }
        else if (!memcmp(f0, SIG_NEW_AL, sizeof SIG_NEW_AL)) { try_new = f1; al = f0; }
    }
    if (!try_new) { api->log(TAG, "anchor: mi_heap_try_new / mi_new_aligned not found"); return 1; }
    A->mi_new_al = al;
    unsigned char *j1 = find_jmp_to(try_new, try_new + 0x400, try_new);       /* mi_heap_alloc_new -> try_new */
    unsigned char *han = j1 ? api->function_start(j1) : NULL;
    unsigned char *j2 = han ? find_jmp_to(han, han + 0x200, han) : NULL;      /* mi_new -> mi_heap_alloc_new */
    unsigned char *mn = j2 ? api->function_start(j2) : NULL;
    if (!mn || memcmp(mn, SIG_TLS_HEAP, sizeof SIG_TLS_HEAP)) { api->log(TAG, "anchor: mi_new not found"); return 1; }
    A->mi_new = mn;
    /* mi_malloc: TLS-heap prologue, tail jmp to _mi_malloc_generic, within 0x1000 before mi_new */
    void *gs = api->find_string("unable to allocate memory (%zu bytes)\n");
    if (!gs) gs = api->find_string("unable to allocate memory (%zu bytes)");
    unsigned char *gx = gs ? api->find_xref(gs, 0) : NULL, *gen = gx ? api->function_start(gx) : NULL;
    int nm = 0;
    if (gen)
        for (unsigned char *c = mn - 0x1000; c < mn; c += 16)
            if (!memcmp(c, SIG_TLS_HEAP, sizeof SIG_TLS_HEAP) && find_jmp_to(c, c + 0x90, gen) &&
                has_zero_r8r9(c, c + 0x90) && has_bytes(c, c + 0x60, SIG_SMALL_CHECK, sizeof SIG_SMALL_CHECK) &&
                api->function_start(c) == c) { A->mi_malloc = c; nm++; }
    if (nm != 1) { api->log(TAG, "anchor: mi_malloc %s (%d candidates)", nm ? "ambiguous" : "not found", nm); A->mi_malloc = NULL; }
    A->mi_free = api->pattern_scan("4C 8B C9 48 85 C9 74 ?? 65 4C 8B 04 25 30 00 00 00 4C 8D 51 FF 49 81 E2 00 00 00 FE");
    if (!A->mi_free) { api->log(TAG, "anchor: mi_free not found"); return 1; }
    return 0;
}

/* Thin allocation wrappers (std::_Allocate, make_X(){return new X;} ...): every .pdata function of
 * at most `max_bytes` that directly calls one of the allocator entry points. Frames inside them are
 * skipped when attributing, so the site is their caller. */
static void find_wrappers(void *const *targets, int nt, uint32_t max_bytes)
{
    IMAGE_NT_HEADERS *nt_h = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *d = &nt_h->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    RUNTIME_FUNCTION *rf = (RUNTIME_FUNCTION *)(g_base + d->VirtualAddress);
    size_t cnt = d->Size / sizeof *rf;
    for (size_t i = 0; i < cnt && g_nskip < MAX_SKIP; i++) {
        uint32_t b = rf[i].BeginAddress, e = rf[i].EndAddress;
        if (!b || e <= b || e - b > max_bytes || e > g_size) continue;
        unsigned char *c = g_base + b, *end = g_base + e;
        for (; c + 5 <= end; c++) {
            if (*c != 0xE8 && *c != 0xE9) continue;
            unsigned char *t = c + 5 + rd32(c + 1);
            int hit = 0;
            for (int k = 0; k < nt; k++) hit |= targets[k] && t == (unsigned char *)targets[k];
            if (hit) { g_skip[g_nskip].begin = b; g_skip[g_nskip].end = e; g_nskip++; break; }
        }
    }
}

static void sort_skips(void)
{
    qsort(g_skip, (size_t)g_nskip, sizeof g_skip[0], at_range_cmp);
    int w = 0;                                             /* drop overlaps (chained .pdata entries) */
    for (int i = 0; i < g_nskip; i++) {
        if (w && g_skip[i].begin < g_skip[w - 1].end) { if (g_skip[i].end > g_skip[w - 1].end) g_skip[w - 1].end = g_skip[i].end; continue; }
        g_skip[w++] = g_skip[i];
    }
    g_nskip = w;
}

/* ------------------------------------------------------------------ report */

typedef struct { uint32_t rva; int64_t live, live_small, small_n, total, count, big, maxsz; uint32_t frames[AT_FRAMES]; } row;
static row g_rows[1 << 15];

static int by_live(const void *a, const void *b)
{
    int64_t x = ((const row *)a)->live + ((const row *)a)->live_small, y = ((const row *)b)->live + ((const row *)b)->live_small;
    return x < y ? 1 : x > y ? -1 : 0;
}
static int by_total(const void *a, const void *b) { int64_t x = ((const row *)a)->total, y = ((const row *)b)->total; return x < y ? 1 : x > y ? -1 : 0; }

/* nearest .rdata string loaded by a rip-relative lea within [site-0x600, site+0x100] of the function */
static const char *hint(uint32_t rva, char *buf, size_t bn)
{
    buf[0] = 0;
    unsigned char *rd; size_t rn;
    if (!section(".rdata", &rd, &rn)) return buf;
    unsigned char *tx; size_t tn;
    if (!section(".text", &tx, &tn)) return buf;
    unsigned char *site = g_base + rva;
    if (site < tx + 0x600 || site + 0x100 > tx + tn) return buf;       /* key 1 = caller outside the exe */
    unsigned char *f = api->function_start(site);
    unsigned char *lo = site - 0x600, *hi = site + 0x100;
    if (f && f > lo) lo = f;
    size_t best = (size_t)-1;
    for (unsigned char *c = lo; c + 7 <= hi; c++) {
        if ((c[0] != 0x48 && c[0] != 0x4C) || c[1] != 0x8D || (c[2] & 0xC7) != 0x05) continue;
        unsigned char *s = c + 7 + rd32(c + 3);
        if (s < rd || s >= rd + rn) continue;
        size_t L = 0; while (L < 120 && s + L < rd + rn && s[L] >= 32 && s[L] < 127) L++;
        if (L < 6 || s[L] != 0) continue;
        size_t d = (size_t)(c > site ? c - site : site - c);
        int cpp = strstr((const char *)s, ".cpp") || strstr((const char *)s, ".h");
        if (cpp) d /= 4;                                      /* prefer source paths */
        if (d < best) {
            best = d;
            const char *bs = strrchr((const char *)s, '\\');
            snprintf(buf, bn, "%.70s", bs ? bs + 1 : (const char *)s);
        }
    }
    return buf;
}

typedef void (*out_fn)(void *ctx, const char *line);

/* ---- LIVE by source hint (sites grouped by the string hint of their function) ---- */
typedef struct { char h[72]; int64_t live, live_small, total; int sites; } hrow;
static hrow g_hrows[2048];
static int by_hlive(const void *a, const void *b)
{
    int64_t x = ((const hrow *)a)->live + ((const hrow *)a)->live_small, y = ((const hrow *)b)->live + ((const hrow *)b)->live_small;
    return x < y ? 1 : x > y ? -1 : 0;
}
static void hint_report(int n, out_fn out, void *ctx)
{
    char line[300], h[96]; int nh = 0;
    for (int i = 0; i < n; i++) {
        row *r = &g_rows[i];
        if (r->live + r->live_small <= 0) continue;
        hint(r->rva, h, sizeof h);
        if (!h[0]) {
            unsigned char *f = r->rva > 1 && r->rva < g_size ? api->function_start(g_base + r->rva) : NULL;
            snprintf(h, sizeof h, "(no hint) fn %08x", f ? to_rva(f) : r->rva);
        }
        int k;
        for (k = 0; k < nh; k++) if (!strcmp(g_hrows[k].h, h)) break;
        if (k == nh) {
            if (nh >= (int)(sizeof g_hrows / sizeof g_hrows[0])) continue;
            memset(&g_hrows[nh], 0, sizeof g_hrows[0]); snprintf(g_hrows[nh].h, sizeof g_hrows[nh].h, "%s", h); nh++;
        }
        g_hrows[k].live += r->live; g_hrows[k].live_small += r->live_small; g_hrows[k].total += r->total; g_hrows[k].sites++;
    }
    qsort(g_hrows, (size_t)nh, sizeof g_hrows[0], by_hlive);
    out(ctx, "--- LIVE by source hint (sites grouped by nearest string of their function) ---   live MiB (big + small est)  total MiB  sites  hint");
    for (int i = 0; i < nh && i < 30; i++) {
        snprintf(line, sizeof line, "%3d  %9.1f (%7.1f + %7.1f)  %9.1f  %4d  %s", i + 1, (g_hrows[i].live + g_hrows[i].live_small) / 1048576.0,
                 g_hrows[i].live / 1048576.0, g_hrows[i].live_small / 1048576.0, g_hrows[i].total / 1048576.0, g_hrows[i].sites, g_hrows[i].h);
        out(ctx, line);
    }
}

/* ---- mimalloc's own view (mt_mi.h) ---- */
static mt_mi_anchors g_mia; static int g_mia_got; static unsigned char *g_mi_buf;
static int in_img(uint64_t va, size_t n) { return va >= (uint64_t)(uintptr_t)g_base && va + n <= (uint64_t)(uintptr_t)g_base + g_size; }
static void mi_report(out_fn out, void *ctx)
{
    if (!g_mi_buf) return;
    static mt_mi_sum s; mt_mi_os os; char b[1200];
    uint64_t key = (g_mia_got & 2) ? *(volatile uint64_t *)(uintptr_t)g_mia.cookie_key : 0;
    mt_mi_walk(&s, key, g_mi_buf);
    int have = (g_mia_got & 1) && mt_mi_os_read(g_mia.stats, &os) == 0;
    out(ctx, "--- mimalloc (own OS commit counter + segment walk; in use = page.used x block size) ---");
    mt_mi_format(&s, &os, have, b, sizeof b); out(ctx, b);
    mt_mi_format_hist(&s, b, sizeof b); out(ctx, b);
    snprintf(b, sizeof b, "estimate check: in use %llu MiB (walk) + huge %llu MiB vs tracked live big %lld + small est %lld MiB",
             MT_MIB(s.used), MT_MIB(s.huge_committed), (long long)(T.live_bytes >> 20), (long long)(T.live_small_bytes >> 20));
    out(ctx, b);
}

/* ---- VM owners report ---- */
typedef struct { uint64_t owner, exe; uint64_t committed; uint32_t n; } vrow;
static vrow g_vrows[1024];
static int by_vcom(const void *a, const void *b) { uint64_t x = ((const vrow *)a)->committed, y = ((const vrow *)b)->committed; return x < y ? 1 : x > y ? -1 : 0; }
static void mod_name(uint64_t va, char *out, size_t n, uint64_t *rva_out)
{
    HMODULE m = NULL; char p[MAX_PATH];
    out[0] = 0; *rva_out = va;
    if (va && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)va, &m) &&
        GetModuleFileNameA(m, p, sizeof p)) {
        const char *b = strrchr(p, '\\'); snprintf(out, n, "%s", b ? b + 1 : p); *rva_out = va - (uint64_t)(uintptr_t)m;
    } else snprintf(out, n, "%s", va ? "?" : "NO USER-MODE RESERVE SEEN (kernel/driver mapping or before hook)");
}
static void vm_report(out_fn out, void *ctx)
{
    char line[400];
    /* drop records of bases that are free again */
    for (uint32_t i = 0; i < VM_SLOTS; i++) {
        LONG64 k = g_vm[i].key; MEMORY_BASIC_INFORMATION q;
        if (k <= 1) continue;
        if (VirtualQuery((void *)(uintptr_t)k, &q, sizeof q) != sizeof q || q.State == MEM_FREE || (LONG64)(uintptr_t)q.AllocationBase != k)
            InterlockedCompareExchange64(&g_vm[i].key, 1, k);
    }
    int nv = 0; uint64_t prot_unknown[4] = { 0 }, total = 0, mimalloc = 0; uint32_t mimalloc_n = 0;
    SYSTEM_INFO si; GetSystemInfo(&si);
    unsigned char *p = (unsigned char *)si.lpMinimumApplicationAddress, *end = (unsigned char *)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mb;
    uint64_t cur = 0, com = 0; DWORD prot = 0; int cur_priv = 0;
    for (;;) {
        int more = p < end && VirtualQuery(p, &mb, sizeof mb) == sizeof mb;
        uint64_t ab = more && mb.State != MEM_FREE ? (uint64_t)(uintptr_t)mb.AllocationBase : 0;
        if (!more || ab != cur) {
            if (cur && cur_priv && com) {
                at_vm *v = at_vm_get(g_vm, VM_SLOTS, cur);
                uint64_t owner = v ? (uint64_t)v->owner : 0, exe = v ? (uint64_t)v->exe : 0;
                total += com;
                if (v && owner >= g_mi_lo && owner < g_mi_hi) { mimalloc += com; mimalloc_n++; }
                else {
                    if (!v) {
                        int pb = prot == PAGE_READWRITE ? 0 : prot == (PAGE_READWRITE | PAGE_WRITECOMBINE) ? 1 : prot == (PAGE_READWRITE | PAGE_NOCACHE) ? 2 : 3;
                        prot_unknown[pb] += com;
                    }
                    int k;
                    for (k = 0; k < nv; k++) if (g_vrows[k].owner == owner && g_vrows[k].exe == exe) break;
                    if (k == nv && nv < (int)(sizeof g_vrows / sizeof g_vrows[0])) { g_vrows[nv].owner = owner; g_vrows[nv].exe = exe; g_vrows[nv].committed = 0; g_vrows[nv].n = 0; nv++; }
                    if (k < nv) { g_vrows[k].committed += com; g_vrows[k].n++; }
                }
            }
            cur = ab; com = 0; prot = 0; cur_priv = more && mb.State != MEM_FREE && mb.Type == MEM_PRIVATE;
        }
        if (!more) break;
        if (mb.State == MEM_COMMIT && cur_priv) { com += mb.RegionSize; if (!prot) prot = mb.Protect; }
        unsigned char *nx = (unsigned char *)mb.BaseAddress + mb.RegionSize;
        if (nx <= p) break;
        p = nx;
    }
    qsort(g_vrows, (size_t)nv, sizeof g_vrows[0], by_vcom);
    snprintf(line, sizeof line, "--- VM owners: committed PRIVATE memory by who reserved it (%lld NtAllocateVirtualMemory reserves seen, %lld dropped) ---",
             (long long)g_vm_calls, (long long)g_vm_dropped);
    out(ctx, line);
    snprintf(line, sizeof line, "private committed %llu MiB = mimalloc os.c %llu MiB in %u allocs + others below; unrecorded, by first committed page protection: "
             "RW %llu, RW|WRITECOMBINE %llu, RW|NOCACHE %llu, other %llu MiB",
             MT_MIB(total), MT_MIB(mimalloc), mimalloc_n, MT_MIB(prot_unknown[0]), MT_MIB(prot_unknown[1]), MT_MIB(prot_unknown[2]), MT_MIB(prot_unknown[3]));
    out(ctx, line);
    static struct { char m[72]; uint64_t c; uint32_t n; } mods[64]; int nm = 0;
    for (int i = 0; i < nv; i++) {
        char mn[72]; uint64_t r; mod_name(g_vrows[i].owner, mn, sizeof mn, &r);
        int k; for (k = 0; k < nm; k++) if (!strcmp(mods[k].m, mn)) break;
        if (k == nm) { if (nm >= 64) continue; snprintf(mods[nm].m, sizeof mods[nm].m, "%s", mn); mods[nm].c = 0; mods[nm].n = 0; nm++; }
        mods[k].c += g_vrows[i].committed; mods[k].n += g_vrows[i].n;
    }
    for (int i = 0; i < nm; i++) {
        snprintf(line, sizeof line, "  module %-40s %8.1f MiB in %u allocs", mods[i].m, mods[i].c / 1048576.0, mods[i].n);
        out(ctx, line);
    }
    out(ctx, "  top owners: committed MiB, allocs, owner frame, first victoria3.exe frame outside mimalloc (game call site)");
    for (int i = 0; i < nv && i < 25; i++) {
        char mn[72], h[96]; uint64_t r; mod_name(g_vrows[i].owner, mn, sizeof mn, &r);
        uint32_t ex = g_vrows[i].exe ? to_rva((void *)(uintptr_t)g_vrows[i].exe) : 0;
        snprintf(line, sizeof line, "  %3d %8.1f %5u  %s+0x%llx  exe %08x %s", i + 1, g_vrows[i].committed / 1048576.0, g_vrows[i].n,
                 mn, (unsigned long long)r, ex, ex ? hint(ex, h, sizeof h) : "");
        out(ctx, line);
    }
}

static void mem_line(char *b, size_t n)
{
    typedef struct { DWORD cb, pf; SIZE_T peak_ws, ws, qpp, qp, qpnp, qnp, pagefile, peak_pagefile, priv; } pmc_ex;
    typedef BOOL (WINAPI *gpmi_fn)(HANDLE, void *, DWORD);
    static gpmi_fn f;
    if (!f) f = (gpmi_fn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo");
    pmc_ex m; memset(&m, 0, sizeof m); m.cb = sizeof m;
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms; GlobalMemoryStatusEx(&ms);
    if (!f || !f(GetCurrentProcess(), &m, sizeof m)) memset(&m, 0, sizeof m);
    snprintf(b, n, "process commit %llu MiB, working set %llu MiB, system commit free %llu of %llu MiB, RAM free %llu of %llu MiB",
             (unsigned long long)(m.priv >> 20), (unsigned long long)(m.ws >> 20), ms.ullAvailPageFile >> 20,
             ms.ullTotalPageFile >> 20, ms.ullAvailPhys >> 20, ms.ullTotalPhys >> 20);
}

static void report(int top, out_fn out, void *ctx)
{
    char line[512], h[96];
    AcquireSRWLockExclusive(&g_report_lock);
    int n = 0;
    for (uint32_t i = 0; i < T.nsites && n < (int)(sizeof g_rows / sizeof g_rows[0]); i++) {
        at_site *s = &T.sites[i];
        if (!s->key) continue;
        row *r = &g_rows[n++];
        r->rva = (uint32_t)s->key; r->live = s->live_bytes; r->total = s->total_bytes; r->count = s->total_count;
        r->live_small = s->live_small_bytes; r->small_n = s->live_small_count;
        r->big = s->big_count; r->maxsz = s->max_size; memcpy(r->frames, (const void *)s->frames, sizeof r->frames);
    }
    mem_line(line, sizeof line); out(ctx, line);
    snprintf(line, sizeof line, "tracked: live big blocks %lld (%lld MiB), live small (sampled estimate) %lld MiB from %lld samples, sites %d, "
             "events big %lld small %lld, frees matched %lld, dropped sites %lld live %lld small %lld; big>=%lld KiB, small sampled every %lld KiB",
             (long long)(T.live_blocks - T.live_small_blocks), (long long)(T.live_bytes >> 20), (long long)(T.live_small_bytes >> 20),
             (long long)T.live_small_blocks, n,
             (long long)g_events_big, (long long)g_events_small, (long long)g_frees_matched, (long long)T.dropped_sites,
             (long long)T.dropped_live, (long long)g_small_dropped, (long long)(g_big >> 10), (long long)(g_period >> 10));
    out(ctx, line);
    mi_report(out, ctx);
    for (int pass = 0; pass < 2; pass++) {
        qsort(g_rows, (size_t)n, sizeof g_rows[0], pass ? by_total : by_live);
        snprintf(line, sizeof line, "--- top %d call sites by %s ---   site(rva)  function   live MiB (big + small est)  total MiB  big#  max MiB  hint",
                 top, pass ? "TOTAL bytes allocated (big exact + small estimate)" : "LIVE bytes (big exact + sampled small estimate)");
        out(ctx, line);
        for (int i = 0; i < n && i < top; i++) {
            row *r = &g_rows[i];
            if (pass == 0 && r->live + r->live_small <= 0) break;
            unsigned char *f = r->rva > 1 && r->rva < g_size ? api->function_start(g_base + r->rva) : NULL;
            snprintf(line, sizeof line, "%3d  %08x  %08x  %9.1f (%7.1f + %7.1f)  %9.1f  %5lld  %7.1f  %s", i + 1, r->rva, f ? to_rva(f) : 0,
                     (r->live + r->live_small) / 1048576.0, r->live / 1048576.0, r->live_small / 1048576.0,
                     r->total / 1048576.0, (long long)r->big, r->maxsz / 1048576.0, hint(r->rva, h, sizeof h));
            out(ctx, line);
            if (r->frames[0]) {
                char st[160]; int o = 0;
                for (int k = 0; k < AT_FRAMES && r->frames[k] && o < (int)sizeof st - 12; k++) o += snprintf(st + o, sizeof st - o, " %08x", r->frames[k]);
                snprintf(line, sizeof line, "       stack (first >= %lld MiB block):%s", (long long)(g_stack_big >> 20), st);
                out(ctx, line);
            }
        }
    }
    hint_report(n, out, ctx);
    if (g_vm) vm_report(out, ctx);
    ReleaseSRWLockExclusive(&g_report_lock);
}

static void out_print(void *ctx, const char *l) { (void)ctx; api->print("%s", l); }
static void out_file(void *ctx, const char *l) { fprintf((FILE *)ctx, "%s\n", l); }

static volatile LONG g_reports;

/* Write <v3x>/alloc_report.txt via a temp file + rename (a reader never sees a half-written report). */
static int write_report(const char *why)
{
    char tmp[MAX_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_report);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        api->log(TAG, "report FAILED (%s): cannot open %s (errno %d, win32 %lu)", why, tmp, errno, GetLastError());
        return -1;
    }
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "alloc_tracker report #%ld (%s) %02d:%02d:%02d   RVAs relative to victoria3.exe; tools/symbolize.py can map them\n",
            g_reports + 1, why, t.wHour, t.wMinute, t.wSecond);
    report(cfg("report_top", 40), out_file, f);
    int bad = ferror(f);
    fclose(f);
    if (bad || !MoveFileExA(tmp, g_report, MOVEFILE_REPLACE_EXISTING)) {
        api->log(TAG, "report FAILED (%s): write/rename error (win32 %lu)", why, GetLastError());
        return -1;
    }
    LONG n = InterlockedIncrement(&g_reports);
    api->log(TAG, "report written: %s (#%ld, %s)", g_report, n, why);
    return 0;
}

/* Periodic reports. Two drivers share one deadline so a report is written about every interval:
 *   on_frame      main thread, while frames are running (menus, game);
 *   report_thread background, covers long main-thread stalls (loading) when no frame arrives. */
static volatile LONG64 g_next_report_ms;
static ULONGLONG g_interval_ms;

static void maybe_report(const char *why)
{
    if (!g_interval_ms) return;
    LONG64 now = (LONG64)GetTickCount64(), due = g_next_report_ms;
    if (now < due) return;
    if (InterlockedCompareExchange64(&g_next_report_ms, now + (LONG64)g_interval_ms, due) != due) return;  /* other driver won */
    write_report(why);
}

static void report_frame(void *u) { (void)u; maybe_report("periodic, main thread"); }

static DWORD WINAPI report_thread(void *u)
{
    (void)u;
    api->log(TAG, "report thread started: every %llu s -> %s", (unsigned long long)(g_interval_ms / 1000), g_report);
    for (;;) { Sleep(1000); maybe_report("periodic, background"); }
}

static void cmd_allocs(const char *args, void *u)
{
    (void)u;
    int n = 20;
    if (args && *args) { const char *p = args; if (!strncmp(p, "top", 3)) p += 3; int v = atoi(p); if (v > 0 && v <= 500) n = v; }
    report(n, out_print, NULL);
    write_report("console");
}

static void on_transition(const char *name, void *data, void *user)
{
    (void)name; (void)user;
    const V3X_LoadTransition *t = (const V3X_LoadTransition *)data;
    char why[160];
    snprintf(why, sizeof why, "load_transition %s -> %s", t && t->from ? t->from : "?", t && t->to ? t->to : "?");
    write_report(why);
}

/* ------------------------------------------------------------------ entry */

static int hook(void *t, void *d, void **o, const char *what)
{
    int rc = t ? api->hook(t, d, o) : -1;
    api->log(TAG, "%-15s exe+0x%08x hook %s", what, t ? to_rva(t) : 0, rc == 0 ? "installed" : "FAILED");
    return rc;
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    g_base = (unsigned char *)api->image_base(); g_size = api->image_size();
    snprintf(g_ini, sizeof g_ini, "%salloc_tracker.ini", api->v3x_dir());
    snprintf(g_report, sizeof g_report, "%salloc_report.txt", api->v3x_dir());
    if (!cfg("enabled", 0)) { api->log(TAG, "disabled (set enabled=1 in %s)", g_ini); return 0; }

    g_big = (int64_t)cfg("big_kb", 64) << 10;
    g_period = (int64_t)cfg("sample_kb", 256) << 10;
    g_stack_big = (int64_t)cfg("stack_mb", 1) << 20;
    if (g_big < 4096) g_big = 4096;
    if (g_period < 4096) g_period = 4096;

    DWORD idx = TlsAlloc();
    if (idx == TLS_OUT_OF_INDEXES || idx >= 64) { api->log(TAG, "no TEB TLS slot < 64 (got %lu): disabled", (unsigned long)idx); return 1; }
    g_tls_off = 0x1480 + idx * 8;

    g_small_live = cfg("small_live", 1);
    int lg = cfg("live_slots_log2", 19); if (lg < 12) lg = 12; if (lg > 24) lg = 24;
    T.nsites = 1u << 15; T.nlive = 1u << lg;
    T.sites = VirtualAlloc(NULL, sizeof(at_site) * T.nsites, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    T.live = VirtualAlloc(NULL, sizeof(at_live) * T.nlive, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!T.sites || !T.live) { api->log(TAG, "table allocation failed: disabled"); return 1; }

    anchors A;
    if (find_anchors(&A)) { api->log(TAG, "anchors missing: disabled (nothing hooked)"); return 1; }
    void *targets[3] = { A.mi_new, A.mi_malloc, A.mi_new_al };
    find_wrappers(targets, 3, (uint32_t)cfg("wrapper_max_bytes", 256));
    char skip[512]; GetPrivateProfileStringA("alloc_tracker", "skip", "", skip, sizeof skip, g_ini);
    for (char *p = strtok(skip, ", "); p && g_nskip < MAX_SKIP; p = strtok(NULL, ", ")) {
        uint32_t r = (uint32_t)strtoul(p, NULL, 16);
        unsigned char *f = r ? api->function_start(g_base + r) : NULL;
        if (f) { g_skip[g_nskip].begin = to_rva(f); g_skip[g_nskip].end = to_rva(f) + 0x200; g_nskip++; }
    }
    sort_skips();
    api->log(TAG, "attribution skips %d allocator wrapper function(s)", g_nskip);

    /* mimalloc's own view: anchors inside the mimalloc code around mi_free */
    {
        unsigned char *tx; size_t tn, mf = (size_t)((unsigned char *)A.mi_free - g_base);
        g_mi_lo = (uint64_t)(uintptr_t)A.mi_free - 0x8000; g_mi_hi = (uint64_t)(uintptr_t)A.mi_free + 0x18000;   /* ~0x43d1000..0x43f1000 */
        if (cfg("mimalloc_walk", 1) && section(".text", &tx, &tn) && mf > (size_t)(tx - g_base) + 0x20000 && mf + 0x20000 < (size_t)(tx - g_base) + tn) {
            g_mia_got = mt_mi_anchor((unsigned char *)A.mi_free - 0x20000, 0x40000, (uint64_t)(uintptr_t)A.mi_free - 0x20000, &g_mia);
            mt_mi_os os;
            if ((g_mia_got & 1) && (!in_img(g_mia.stats, MI_S_SIZE) || mt_mi_os_read(g_mia.stats, &os))) g_mia_got &= ~1;
            if ((g_mia_got & 2) && !in_img(g_mia.cookie_key, 8)) g_mia_got &= ~2;
            g_mi_buf = VirtualAlloc(NULL, MI_HDR_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            api->log(TAG, "mimalloc walk: stats %s, cookie key %s", g_mia_got & 1 ? "found" : "NOT found", g_mia_got & 2 ? "found" : "NOT found");
        }
    }

    int bad = 0;
    bad |= hook(A.mi_free, (void *)free_detour, (void **)&o_free, "mi_free");
    bad |= hook(A.mi_new, (void *)new_detour, (void **)&o_new, "mi_new");
    if (A.mi_new_al) hook(A.mi_new_al, (void *)new_al_detour, (void **)&o_new_al, "mi_new_aligned");
    if (A.mi_malloc) hook(A.mi_malloc, (void *)malloc_detour, (void **)&o_malloc, "mi_malloc");
    if (bad) { api->log(TAG, "core hooks failed: tracking stays off"); return 1; }
    InterlockedExchange(&g_on, 1);

    if (cfg("vm_owners", 1)) {
        DWORD vi = TlsAlloc();
        g_vm = vi != TLS_OUT_OF_INDEXES && vi < 64 ? VirtualAlloc(NULL, sizeof(at_vm) * VM_SLOTS, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) : NULL;
        if (g_vm) {
            g_vm_tls = 0x1480 + vi * 8;
            static const char *const sys[] = { "ntdll.dll", "kernelbase.dll", "kernel32.dll", "ucrtbase.dll", "msvcrt.dll", "vcruntime140.dll" };
            for (int i = 0; i < 6; i++) add_sys(GetModuleHandleA(sys[i]));
            HMODULE self = NULL;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(void *)vm_note, &self);
            add_sys(self);
            HMODULE nt = GetModuleHandleA("ntdll.dll");
            void *f1 = nt ? (void *)GetProcAddress(nt, "NtAllocateVirtualMemory") : NULL, *f2 = nt ? (void *)GetProcAddress(nt, "NtAllocateVirtualMemoryEx") : NULL;
            int r1 = f1 ? api->hook(f1, (void *)ntava_detour, (void **)&o_ntava) : -1;
            int r2 = f2 ? api->hook(f2, (void *)ntavaex_detour, (void **)&o_ntavaex) : -1;
            api->log(TAG, "vm owners: NtAllocateVirtualMemory hook %s, NtAllocateVirtualMemoryEx hook %s (%d system ranges skipped)",
                     r1 == 0 ? "installed" : "FAILED", r2 == 0 ? "installed" : f2 ? "FAILED" : "n/a", g_nsys);
            if (r1 != 0 && r2 != 0) g_vm = NULL;
        } else api->log(TAG, "vm owners: no TLS slot / table: off");
    }
    api->log(TAG, "tracking on: big >= %lld KiB exact + live, small sampled every %lld KiB; report %s",
             (long long)(g_big >> 10), (long long)(g_period >> 10), g_report);

    if (api->api_version >= 2 && api->struct_size >= offsetof(V3X_API, exec_command) + sizeof(void *))
        api->register_command("allocs", "allocs [top N]: top allocation call sites by live and total bytes", cmd_allocs, NULL);
    if (api->api_version >= 3 && api->struct_size >= offsetof(V3X_API, on_event) + sizeof(void *))
        api->on_event("load_transition", on_transition, NULL);
    g_interval_ms = (ULONGLONG)cfg("report_interval_s", 30) * 1000;
    if (g_interval_ms) {
        g_next_report_ms = (LONG64)(GetTickCount64() + g_interval_ms);
        int rf = api->on_frame(report_frame, NULL);
        HANDLE th = CreateThread(NULL, 0, report_thread, NULL, 0, NULL);
        api->log(TAG, "periodic report every %llu s: on_frame %s, background thread %s",
                 (unsigned long long)(g_interval_ms / 1000), rf == 0 ? "ok" : "FAILED", th ? "ok" : "FAILED");
        if (th) CloseHandle(th);
    } else api->log(TAG, "periodic report off (report_interval_s=0)");
    return 0;
}
