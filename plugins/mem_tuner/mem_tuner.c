/* mem_tuner: tune the engine's statically linked mimalloc (v2.0.x) for lower commit, give
 * memory back after loads, and turn "out of memory -> std::terminate" into one logged rescue.
 * Background and RVAs: docs/re/memory.md. Anchors are mirrored in tools/re_mem_check.py.
 *
 * 1. Options. v3x_plugin_load runs before CRT init, i.e. before mimalloc's _mi_options_init.
 *    "show_errors" (.rdata) -> the one 8-byte pointer to it in .data = name field of entry 0
 *    of the option table -> validate all 26 entries (index + name) -> write value, init=2.
 *    Any mismatch: nothing is written (fail closed).
 * 2. mi_collect(true). "process done: 0x%zx\n" -> mi_process_done -> its `mov cl,1; call` =
 *    mi_collect (prologue byte-checked). Called on the main thread (on_frame) a few frames
 *    after each state transition: API v3 = event "load_transition" (emitted by fast_load, which
 *    owns that hook); older loaders = own hook on the "Transition {} took: {} seconds" fn.
 *    Also console command `memcollect` (API v2).
 * 3. OOM guard. "out of memory in 'new'" -> mi_heap_try_new -> first call whose target has the
 *    get_new_handler prologue -> hooked: when the game has no new_handler (always), return ours.
 *    Ours (first OOM): free the reserve, mi_collect(true), log WARNING + commit stats, return
 *    -> mimalloc retries. Later OOMs: log and return NULL -> bad_alloc as before.
 * 4. Error log. _mi_error_message's `mi_error_handler` global (found by pattern, only set when
 *    NULL) -> log mimalloc errors (ENOMEM, double free, ...) rate-limited.
 *
 * 5. Periodic collect + census. Monitor thread samples commit every 30 s; on +256 MiB growth (min
 *    5 min apart) the main thread runs mi_collect(false) (on_frame), logging commit before/after and
 *    auto-disabling itself if one call exceeds max_ms. Every 10 min (and after each collect) a
 *    VirtualQuery census logs mimalloc segments vs other private commit. Console `memstat`.
 * 6. GUI animation-state pool diag (pass-through hook on its insert): census adds live/cap slots.
 * 7. mimalloc walk (mt_mi.h): census/memstat add mimalloc's own commit counter and a segment walk
 *    (in use vs page slack vs empty pages vs free spans vs pending decommit, by block size and thread).
 * 8. Thread collect: mimalloc's deferred-free slot (called by every thread at the start of the
 *    allocation slow path, heap consistent, recursion-guarded) runs mi_collect(false) once per
 *    generation on EACH thread; mi_collect only works on the calling thread's heap, so the main-thread
 *    collect alone never reaches the ~30 worker heaps. The monitor bumps the generation every every_s.
 * 9. Heap compact: HeapCompact on the process heaps from the monitor thread (logged, self-disabling).
 * Every hook forwards 8 integer args (PASS8).
 *
 * Config: <game>/binaries/v3x/mem_tuner.ini (see mem_tuner.ini.example; all keys optional). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdlib.h>
#include <intrin.h>
#include <tlhelp32.h>
#include "v3x.h"
#include "mt_core.h"
#include "mt_mi.h"

V3X_EXPORT const char *v3x_plugin_name = "mem_tuner";
V3X_PLUGIN_MANIFEST("mem_tuner", "0.5.0", "v3x", "tunes the engine allocator for lower memory commit", "release/1.15.1");
#define TAG "mem_tuner"

static const V3X_API *api;
static unsigned char *g_base; static size_t g_size;
static char g_ini[MAX_PATH];

#define PASS8_ARGS void *a0_, void *a1_, void *a2_, void *a3_, void *a4_, void *a5_, void *a6_, void *a7_
#define PASS8 a0_, a1_, a2_, a3_, a4_, a5_, a6_, a7_
typedef void (*new_handler_t)(void);
typedef new_handler_t (*get_new_handler_fn)(PASS8_ARGS);
typedef void (*mi_collect_fn)(_Bool force);
typedef void (*mi_error_fun)(int err, void *arg);
typedef uintptr_t (*transition_fn)(PASS8_ARGS);

static mi_collect_fn g_collect;
static get_new_handler_fn g_orig_gnh;
static transition_fn g_orig_transition;
static void *volatile g_reserve; static SIZE_T g_reserve_size;
static volatile LONG g_oom_events, g_rescues, g_err_logs, g_pending_collect;
static void mt_error_fun(int err, void *arg);
static int g_err_log_max = 32, g_collect_delay_frames = 30;
static volatile LONG g_frames_left = -1;

/* ------------------------------------------------------------------ helpers */

static int cfg(const char *sec, const char *key, int def) { return (int)GetPrivateProfileIntA(sec, key, def, g_ini); }

static int in_image(uint64_t va, size_t n) { return va >= (uint64_t)g_base && va + n <= (uint64_t)g_base + g_size; }
static const char *img_str(void *ctx, uint64_t va)
{
    (void)ctx;
    if (!in_image(va, 1)) return NULL;
    const char *s = (const char *)va;
    size_t max = (size_t)((uint64_t)g_base + g_size - va);
    return memchr(s, 0, max < 64 ? max : 64) ? s : NULL;
}
static const unsigned char *img_peek(void *ctx, uint64_t va, size_t n) { (void)ctx; return in_image(va, n) ? (const unsigned char *)va : NULL; }

static int section(const char *name, unsigned char **p, size_t *n)
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
        if (!strncmp((const char *)s->Name, name, 8)) { *p = g_base + s->VirtualAddress; *n = s->Misc.VirtualSize; return 1; }
    return 0;
}
static unsigned long long rva(const void *p) { return (unsigned long long)((const unsigned char *)p - g_base); }

/* PROCESS_MEMORY_COUNTERS_EX (cb and PageFaultCount are DWORDs; 80 bytes on x64) */
typedef struct { DWORD cb, pf; SIZE_T peak_ws, ws, qpp, qp, qpnp, qnp, pagefile, peak_pagefile, priv; } pmc_ex;
typedef BOOL (WINAPI *gpmi_fn)(HANDLE, void *, DWORD);
static unsigned long long proc_commit_mb(void)
{
    static gpmi_fn f;
    if (!f) f = (gpmi_fn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo");
    pmc_ex m; memset(&m, 0, sizeof m); m.cb = sizeof m;
    _Static_assert(sizeof(pmc_ex) == 80, "PROCESS_MEMORY_COUNTERS_EX layout");
    return f && f(GetCurrentProcess(), &m, sizeof m) ? m.priv >> 20 : 0;
}
static void mem_line(char *buf, size_t n)
{
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms; GlobalMemoryStatusEx(&ms);
    snprintf(buf, n, "process commit %llu MiB, system commit free %llu of %llu MiB, RAM free %llu of %llu MiB",
             proc_commit_mb(), ms.ullAvailPageFile >> 20, ms.ullTotalPageFile >> 20, ms.ullAvailPhys >> 20, ms.ullTotalPhys >> 20);
}

/* ------------------------------------------------------------------ 1. options */

static int apply_options(void)
{
    if (!cfg("options", "enabled", 1)) { api->log(TAG, "options: disabled in ini"); return 0; }
    void *s0 = api->find_string("show_errors");
    unsigned char *data; size_t n;
    if (!s0 || !section(".data", &data, &n)) { api->log(TAG, "options: anchor not found, nothing written"); return 1; }
    ptrdiff_t off = mt_find_table(data, n, (uint64_t)s0, img_str, NULL);
    if (off < 0) { api->log(TAG, "options: table %s, nothing written (unsupported mimalloc build)", off == -1 ? "not found" : "ambiguous"); return 1; }
    mt_entry *t = (mt_entry *)(data + off);
    for (int i = 0; i < MT_NOPT; i++)
        if (t[i].init != 0) { api->log(TAG, "options: '%s' already initialized (init=%d): too late, nothing written", MT_NAMES[i], t[i].init); return 1; }
    /* Defaults: lower commit (mimalloc 2.0.x semantics, docs/re/memory.md section 4). INT_MIN = leave alone. */
    int def[MT_NOPT];
    for (int i = 0; i < MT_NOPT; i++) def[i] = INT_MIN;
    def[MT_EAGER_COMMIT] = 0;               /* 1: commit whole 32 MiB segments after a thread's 4th  */
    def[MT_ABANDONED_PAGE_DECOMMIT] = 1;    /* 0: free pages of exited threads stay committed        */
    def[MT_DECOMMIT_DELAY] = 10;            /* 25 ms: delay before freed spans are decommitted       */
    def[MT_SEGMENT_DECOMMIT_DELAY] = 100;   /* 500 ms: same for cached segments                      */
    def[MT_DECOMMIT_EXTEND_DELAY] = 0;      /* 1 ms: extra delay added on repeated decommit requests */
    int changed = 0;
    for (int i = 0; i < MT_NOPT; i++) {
        int v = cfg("options", MT_NAMES[i], def[i]);
        if (v == INT_MIN) continue;
        if (!strncmp(MT_NAMES[i], "deprecated_", 11)) continue;
        api->log(TAG, "option %-24s %d -> %d", MT_NAMES[i], t[i].value, v);
        t[i].value = v; t[i].init = MT_INIT_INITIALIZED; changed++;
    }
    api->log(TAG, "options: table at exe+0x%llx, %d option(s) set", rva(t), changed);
    return 0;
}

/* ------------------------------------------------------------------ 2. mi_collect */

static void do_collect(const char *why)
{
    if (!g_collect) return;
    unsigned long long before = proc_commit_mb();
    ULONGLONG t0 = GetTickCount64();
    g_collect(1);
    api->log(TAG, "mi_collect(%s): commit %llu -> %llu MiB in %llu ms", why, before, proc_commit_mb(), GetTickCount64() - t0);
}

/* ------------------------------------------------------------------ 5. periodic collect + census
 * A low-priority monitor thread samples process commit every check_sec. When commit grew by grow_mb
 * since the lowest point after the last collect (and min_interval_min passed) it asks the MAIN
 * thread for mi_collect(force). Why not collect on the monitor thread: mi_collect works on the
 * calling thread's heap and returns at once when that heap is mi_heap_empty (mi_heap_collect_ex
 * 0x43e4390: `cmp rcx,&_mi_heap_empty ; je ret`), which it is for any thread that never allocated
 * through the exe's mimalloc - i.e. ours. The main thread's call also covers what a background call
 * could reach (abandoned segments of exited threads, the global segment cache) plus the main heap.
 * Census: VirtualQuery walk, splits committed private memory into mimalloc segments / huge
 * segments / other (heaps, driver, D3D) so the log shows where commit lives (mt_core.h). */
static mt_periodic g_per;                          /* owned by the monitor thread */
static volatile LONG64 g_done_mb, g_done_ms; static volatile LONG g_done_flag;   /* main -> monitor: collect happened */
static volatile LONG g_per_pending, g_per_enabled, g_census_now, g_census_min = 10;
static int g_per_force, g_per_max_ms = 100, g_per_check_sec = 30;
static long long g_per_saved_mb; static LONG g_per_runs;
static HANDLE g_mon_event;

static void census_walk(mt_census *c)
{
    memset(c, 0, sizeof *c);
    SYSTEM_INFO si; GetSystemInfo(&si);
    unsigned char *p = (unsigned char *)si.lpMinimumApplicationAddress, *end = (unsigned char *)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mb;
    void *own = g_reserve;                         /* our OOM reserve: "other", even when 32 MiB aligned by chance */
    while (p < end && VirtualQuery(p, &mb, sizeof mb) == sizeof mb) {
        if (mb.State != MEM_FREE)
            mt_census_region(c, (uint64_t)(uintptr_t)mb.AllocationBase, mb.RegionSize, mb.State == MEM_COMMIT,
                             mb.Type == MEM_IMAGE ? MT_IMAGE : mb.Type == MEM_MAPPED ? MT_MAPPED
                             : own && mb.AllocationBase == own ? MT_PRIVATE_NOT_MI : MT_PRIVATE);
        unsigned char *n = (unsigned char *)mb.BaseAddress + mb.RegionSize;
        if (n <= p) break;
        p = n;
    }
    mt_census_finish(c);
}
/* GUI animation-state pool diagnostic (section 6 below; read with ReadProcessMemory so a pool
 * that was freed meanwhile can never fault the monitor thread). */
#define MT_MAXPOOLS 4
static void *volatile g_pools[MT_MAXPOOLS];
static void pool_line(char *b, size_t n)
{
    size_t o = 0; b[0] = 0;
    for (int i = 0; i < MT_MAXPOOLS; i++) {
        if (o + 1 >= n) break;
        void *p = g_pools[i]; if (!p) continue;
        unsigned char h[0x78]; SIZE_T got = 0; long long live, bytes, cap;
        if (!ReadProcessMemory(GetCurrentProcess(), p, h, sizeof h, &got) || got != sizeof h || mt_pool_check(h, &live, &bytes, &cap)) {
            g_pools[i] = NULL; o += (size_t)snprintf(b + o, o < n ? n - o : 0, " | gui anim pool %d: gone", i); continue;
        }
        o += (size_t)snprintf(b + o, o < n ? n - o : 0, " | gui anim pool %d: live %lld of %lld slots, array %lld MiB", i, live, cap, bytes >> 20);
    }
}

static void census_line(char *b, size_t n, const char *why)
{
    mt_census c; census_walk(&c);
    char pl[300]; pool_line(pl, sizeof pl);
#define MB(x) ((unsigned long long)((x) >> 20))
    snprintf(b, n, "census(%s): private commit %llu MiB = mimalloc %llu x 32M segs %llu MiB (%llu fully committed) + %llu huge %llu MiB"
             " | other %llu allocs %llu MiB [<1M %llu/%llu, <16M %llu/%llu, <64M %llu/%llu, >=64M %llu/%llu MiB] incl. OOM reserve %llu MiB"
             " | mimalloc reserved-only %llu MiB, mapped %llu, image %llu MiB%s",
             why, MB(c.total_private_commit), (unsigned long long)c.seg_n, MB(c.seg_commit), (unsigned long long)c.seg_full_n,
             (unsigned long long)c.huge_n, MB(c.huge_commit), (unsigned long long)c.other_n, MB(c.other_commit),
             (unsigned long long)c.bn[0], MB(c.bc[0]), (unsigned long long)c.bn[1], MB(c.bc[1]), (unsigned long long)c.bn[2], MB(c.bc[2]),
             (unsigned long long)c.bn[3], MB(c.bc[3]), g_reserve ? (unsigned long long)(g_reserve_size >> 20) : 0ULL,
             MB(c.mi_reserved_only), MB(c.mapped_commit), MB(c.image_commit), pl);
#undef MB
}

/* ------------------------------------------------------------------ 7. mimalloc walk */
static mt_mi_anchors g_mia; static int g_mia_got;
static unsigned char *g_mi_buf;                    /* MI_HDR_BYTES, monitor thread / console only */
static SRWLOCK g_mi_lock = SRWLOCK_INIT;

typedef LONG (WINAPI *nqit_fn)(HANDLE, int, void *, ULONG, ULONG *);
typedef HRESULT (WINAPI *gtd_fn)(HANDLE, PWSTR *);
typedef struct { LONG status; void *teb; uintptr_t pid, tid; uintptr_t affinity; LONG prio, base; } mt_tbi;
/* mimalloc's thread id is the TEB address (mi_free compares segment+0x100 with gs:[30h]) */
static void teb_name(uint64_t teb, char *out, size_t n)
{
    static nqit_fn nq; static gtd_fn gd;
    if (!nq) nq = (nqit_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    if (!gd) gd = (gtd_fn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    if (!teb) { snprintf(out, n, "abandoned"); return; }
    snprintf(out, n, "teb %llx", (unsigned long long)teb);
    if (!nq) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof te;
    DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!th) continue;
        mt_tbi bi; memset(&bi, 0, sizeof bi);
        if (nq(th, 0, &bi, sizeof bi, NULL) == 0 && (uint64_t)(uintptr_t)bi.teb == teb) {
            PWSTR w = NULL; char nm[64] = "";
            if (gd && SUCCEEDED(gd(th, &w)) && w) { WideCharToMultiByte(CP_UTF8, 0, w, -1, nm, sizeof nm, NULL, NULL); LocalFree(w); }
            snprintf(out, n, "tid %lu%s%s", te.th32ThreadID, nm[0] ? " " : "", nm);
            CloseHandle(th); break;
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
}

static int by_free(const void *a, const void *b)
{
    uint64_t x = ((const mt_mi_thr *)a)->free, y = ((const mt_mi_thr *)b)->free;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* Logs (log=1) or prints the mimalloc lines. Returns 0 when the walk ran. */
static int mi_lines(int log, int threads)
{
    if (!g_mi_buf) return -1;
    static mt_mi_sum s; mt_mi_os os; char b[1200];
    AcquireSRWLockExclusive(&g_mi_lock);
    uint64_t key = (g_mia_got & 2) ? *(volatile uint64_t *)(uintptr_t)g_mia.cookie_key : 0;
    LARGE_INTEGER q0, q1, qf; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
    mt_mi_walk_skip(&s, key, g_mi_buf, (uint64_t)(uintptr_t)g_reserve);   /* our OOM reserve is never a segment */
    QueryPerformanceCounter(&q1);
    int have_os = (g_mia_got & 1) && mt_mi_os_read(g_mia.stats, &os) == 0;
    mt_mi_format(&s, &os, have_os, b, sizeof b);
    size_t L = strlen(b);
    snprintf(b + L, sizeof b - L, "; walk %.1f ms%s", (q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart, key ? ", cookie-checked" : "");
    if (log) api->log(TAG, "%s", b); else api->print("%s", b);
    mt_mi_format_hist(&s, b, sizeof b);
    if (log) api->log(TAG, "%s", b); else api->print("%s", b);
    if (threads && s.nthr) {
        qsort(s.thr, (size_t)s.nthr, sizeof s.thr[0], by_free);
        size_t o = (size_t)snprintf(b, sizeof b, "mimalloc by owner thread, most committed-but-not-in-use first (committed/in use MiB, segs):");
        for (int i = 0; i < s.nthr && i < 10 && o < sizeof b; i++) {
            char nm[96]; teb_name(s.thr[i].tid, nm, sizeof nm);
            o += (size_t)snprintf(b + o, sizeof b - o, " [%s] %llu/%llu (%u)", nm, MT_MIB(s.thr[i].committed), MT_MIB(s.thr[i].used), s.thr[i].segs);
        }
        if (log) api->log(TAG, "%s", b); else api->print("%s", b);
    }
    ReleaseSRWLockExclusive(&g_mi_lock);
    return 0;
}

static void setup_mi(void)
{
    if (!cfg("mimalloc_walk", "enabled", 1)) return;
    /* search window: the mimalloc code (~0x17000 bytes) around mi_collect / mi_heap_try_new */
    unsigned char *center = (unsigned char *)g_collect;
    if (!center) { void *s = api->find_string("out of memory in 'new'"); center = s ? api->find_xref(s, 0) : NULL; }
    unsigned char *text; size_t tn;
    if (!center || !section(".text", &text, &tn)) { api->log(TAG, "mimalloc walk: no anchor, off"); return; }
    unsigned char *lo = center - text > 0x20000 ? center - 0x20000 : text;
    unsigned char *hi = text + tn - center > 0x20000 ? center + 0x20000 : text + tn;
    g_mia_got = mt_mi_anchor(lo, (size_t)(hi - lo), (uint64_t)(uintptr_t)lo, &g_mia);
    mt_mi_os os;
    if ((g_mia_got & 1) && (!in_image(g_mia.stats, MI_S_SIZE) || mt_mi_os_read(g_mia.stats, &os))) g_mia_got &= ~1;
    if ((g_mia_got & 2) && !in_image(g_mia.cookie_key, 8)) g_mia_got &= ~2;
    if ((g_mia_got & 4) && !in_image(g_mia.deferred_fn, 16)) g_mia_got &= ~4;
    g_mi_buf = VirtualAlloc(NULL, MI_HDR_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    api->log(TAG, "mimalloc walk: stats %s%llx, cookie key %s%llx, deferred-free slot %s%llx; header buffer %s",
             g_mia_got & 1 ? "exe+0x" : "not found ", g_mia_got & 1 ? rva((void *)(uintptr_t)g_mia.stats) : 0ULL,
             g_mia_got & 2 ? "exe+0x" : "not found ", g_mia_got & 2 ? rva((void *)(uintptr_t)g_mia.cookie_key) : 0ULL,
             g_mia_got & 4 ? "exe+0x" : "not found ", g_mia_got & 4 ? rva((void *)(uintptr_t)g_mia.deferred_fn) : 0ULL,
             g_mi_buf ? "ok" : "FAILED");
}

/* ------------------------------------------------------------------ 8. thread collect (deferred-free slot)
 * _mi_malloc_generic (allocation slow path, every thread) starts with the inlined _mi_deferred_free:
 *   if (deferred_free && !tld->recurse) { tld->recurse = 1; deferred_free(false, tld->heartbeat, arg); tld->recurse = 0; }
 * At that point the heap is consistent (the fast path changed nothing) and mi_collect's own
 * _mi_deferred_free call (0x43dd260) is skipped by the recurse flag. Nothing in the exe writes the slot
 * (mi_register_deferred_free was stripped), so it is NULL and ours alone. The callback costs a gs
 * read + compare unless the generation changed; then this thread runs mi_collect(false) once. */
static DWORD g_tc_tls;                              /* gs offset of our TEB TlsSlots entry */
static volatile LONG g_tc_gen = 1, g_tc_on, g_tc_threads;
static volatile LONG64 g_tc_us_total, g_tc_us_max, g_tc_runs;
static int g_tc_every_s = 120, g_tc_max_ms = 50, g_tc_ok, g_tc_armed;
static LARGE_INTEGER g_qpf;

static void tc_callback(_Bool force, unsigned long long heartbeat, void *arg)
{
    (void)heartbeat; (void)arg;
    LONG g = g_tc_gen;
    if (!g_tc_on || (LONG)__readgsqword(g_tc_tls) == g) return;
    __writegsqword(g_tc_tls, (unsigned long long)(ULONG)g);
    if (force || !g_collect) return;                /* inside an explicit forced collect already */
    DWORD le = GetLastError();
    LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
    g_collect(0);
    QueryPerformanceCounter(&b);
    SetLastError(le);
    LONG64 us = (b.QuadPart - a.QuadPart) * 1000000 / (g_qpf.QuadPart ? g_qpf.QuadPart : 1);
    InterlockedAdd64(&g_tc_us_total, us); InterlockedIncrement64(&g_tc_runs); InterlockedIncrement(&g_tc_threads);
    LONG64 m = g_tc_us_max;
    while (us > m) { LONG64 p = InterlockedCompareExchange64(&g_tc_us_max, us, m); if (p == m) break; m = p; }
    if (us > (LONG64)g_tc_max_ms * 1000) InterlockedExchange(&g_tc_on, 0);   /* the monitor logs it */
}

static void setup_thread_collect(void)
{
    if (!cfg("thread_collect", "enabled", 1)) return;
    if (!g_collect || !(g_mia_got & 4)) { api->log(TAG, "thread collect: %s, off", g_collect ? "deferred-free slot not found" : "no mi_collect"); return; }
    void *volatile *slot = (void *volatile *)(uintptr_t)g_mia.deferred_fn;
    if (*slot) { api->log(TAG, "thread collect: deferred-free slot already in use (%p), off", *slot); return; }
    DWORD idx = TlsAlloc();
    if (idx == TLS_OUT_OF_INDEXES || idx >= 64) { api->log(TAG, "thread collect: no TEB TLS slot < 64, off"); return; }
    g_tc_tls = 0x1480 + idx * 8;
    g_tc_every_s = cfg("thread_collect", "every_s", 120); if (g_tc_every_s < 10) g_tc_every_s = 10;
    g_tc_max_ms = cfg("thread_collect", "max_ms", 50);
    QueryPerformanceFrequency(&g_qpf);
    *(void *volatile *)(uintptr_t)g_mia.deferred_arg = NULL;
    InterlockedExchangePointer(slot, (void *)tc_callback);
    g_tc_ok = 1;                                    /* armed on the first frame (not during CRT init) */
    api->log(TAG, "thread collect: deferred-free slot exe+0x%llx set; every thread runs mi_collect(false) once every %d s "
             "and after each load (armed at first frame, off if one call > %d ms)", rva((void *)slot), g_tc_every_s, g_tc_max_ms);
}

/* ------------------------------------------------------------------ 9. heap compact (monitor thread) */
static int g_hc_on, g_hc_max_ms = 100;
typedef struct { DWORD cb; SIZE_T allocated, committed, reserved, max_reserve; } mt_heap_summary;
typedef BOOL (WINAPI *heap_summary_fn)(HANDLE, DWORD, mt_heap_summary *);
static void heap_compact(void)
{
    static heap_summary_fn hs;
    if (!hs) hs = (heap_summary_fn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "HeapSummary");
    HANDLE hp[64]; DWORD n = GetProcessHeaps(64, hp); if (n > 64) n = 64;
    unsigned long long before = 0, after = 0, alloc = 0;
    ULONGLONG t0 = GetTickCount64(), worst = 0;
    for (DWORD i = 0; i < n; i++) {
        mt_heap_summary a = { sizeof a, 0, 0, 0, 0 }, b = { sizeof b, 0, 0, 0, 0 };
        if (hs && hs(hp[i], 0, &a)) { before += a.committed; alloc += a.allocated; }
        ULONGLONG t = GetTickCount64();
        HeapCompact(hp[i], 0);
        t = GetTickCount64() - t; if (t > worst) worst = t;
        if (hs && hs(hp[i], 0, &b)) after += b.committed;
    }
    api->log(TAG, "heap compact: %lu heaps, committed %llu -> %llu MiB (allocated %llu MiB) in %llu ms (worst heap %llu ms)",
             n, before >> 20, after >> 20, alloc >> 20, GetTickCount64() - t0, worst);
    if ((int)worst > g_hc_max_ms) { g_hc_on = 0; api->log(TAG, "heap compact took %llu ms > max_ms %d: off for this session", worst, g_hc_max_ms); }
}

static void periodic_collect_main(void)
{
    long long before = (long long)proc_commit_mb();
    ULONGLONG t0 = GetTickCount64();
    g_collect(g_per_force != 0);
    ULONGLONG ms = GetTickCount64() - t0;
    long long after = (long long)proc_commit_mb();
    g_done_mb = after; g_done_ms = (LONG64)GetTickCount64(); InterlockedExchange(&g_done_flag, 1);
    g_per_saved_mb += before - after; InterlockedIncrement(&g_per_runs);
    api->log(TAG, "mi_collect(periodic, force=%d): commit %lld -> %lld MiB (%+lld) in %llu ms; session total %lld MiB in %ld runs",
             g_per_force, before, after, after - before, ms, g_per_saved_mb, g_per_runs);
    if ((int)ms > g_per_max_ms) {
        InterlockedExchange(&g_per_enabled, 0);
        api->log(TAG, "periodic collect took %llu ms > max_ms %d: periodic collect disabled for this session", ms, g_per_max_ms);
    }
    InterlockedExchange(&g_census_now, 1);
    if (g_mon_event) SetEvent(g_mon_event);
}

static DWORD WINAPI monitor_thread(void *u)
{
    (void)u;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    uint64_t next_census = 0, next_heap = 0;
    mt_census_due(GetTickCount64(), &next_census, g_census_min);
    int heap_min = cfg("heap_compact", "every_min", 10);
    mt_census_due(GetTickCount64(), &next_heap, heap_min);
    ULONGLONG next_tc = 0, tc_log = 0; long long tc_before = -1; int tc_reported_off = 0;
    for (;;) {
        DWORD wait = (DWORD)g_per_check_sec * 1000;
        if (g_tc_ok && wait > 10000) wait = 10000;
        WaitForSingleObject(g_mon_event, wait);
        ULONGLONG now = GetTickCount64();
        if (g_tc_ok && g_tc_on) {
            mt_mi_os os; int have = (g_mia_got & 1) && mt_mi_os_read(g_mia.stats, &os) == 0;
            if (tc_log && now >= tc_log) {
                api->log(TAG, "thread collect gen %ld: %ld thread(s) collected, %lld runs total, %.1f ms total, max %.1f ms; mimalloc committed %lld -> %lld MiB",
                         g_tc_gen, InterlockedExchange(&g_tc_threads, 0), (long long)g_tc_runs, g_tc_us_total / 1000.0, g_tc_us_max / 1000.0,
                         tc_before, have ? (long long)(os.committed >> 20) : -1LL);
                tc_log = 0;
            }
            if (!next_tc) next_tc = now + (ULONGLONG)g_tc_every_s * 1000;
            else if (now >= next_tc) {
                tc_before = have ? (long long)(os.committed >> 20) : -1;
                InterlockedIncrement(&g_tc_gen);
                next_tc = now + (ULONGLONG)g_tc_every_s * 1000; tc_log = now + 10000;
            }
        } else if (g_tc_ok && g_tc_armed && !g_tc_on && !tc_reported_off) {
            tc_reported_off = 1;
            api->log(TAG, "thread collect: one call took %.1f ms > max_ms %d: off for this session", g_tc_us_max / 1000.0, g_tc_max_ms);
        }
        if (g_hc_on && mt_census_due(now, &next_heap, heap_min)) heap_compact();
        if (InterlockedExchange(&g_done_flag, 0)) mt_periodic_done(&g_per, (uint64_t)g_done_ms, (long long)g_done_mb);
        if (g_per_enabled && !g_per_pending && mt_periodic_due(&g_per, now, (long long)proc_commit_mb()))
            InterlockedExchange(&g_per_pending, 1);
        int census = InterlockedExchange(&g_census_now, 0);
        if (mt_census_due(now, &next_census, g_census_min)) census = 1;
        if (census) { char b[1000]; census_line(b, sizeof b, "monitor"); api->log(TAG, "%s", b); mi_lines(1, 1); }
    }
    return 0;
}

static void setup_periodic(int *need_frame)
{
    int per = cfg("periodic", "enabled", 1) && g_collect;
    g_census_min = cfg("census", "every_min", 10);
    if (!cfg("census", "enabled", 1)) g_census_min = 0;
    g_hc_on = cfg("heap_compact", "enabled", 1); g_hc_max_ms = cfg("heap_compact", "max_ms", 100);
    if (!per && g_census_min <= 0 && !g_tc_ok && !g_hc_on) return;
    g_per_force = cfg("periodic", "force", 0);
    g_per_max_ms = cfg("periodic", "max_ms", 100);
    g_per_check_sec = cfg("periodic", "check_sec", 30); if (g_per_check_sec < 1) g_per_check_sec = 1;
    g_per.grow_mb = cfg("periodic", "grow_mb", 256);
    g_per.min_interval_ms = (uint64_t)cfg("periodic", "min_interval_min", 5) * 60000;
    g_per_enabled = per;
    g_mon_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE t = g_mon_event ? CreateThread(NULL, 64 * 1024, monitor_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL) : NULL;
    if (!t) { g_per_enabled = 0; api->log(TAG, "periodic/census: monitor thread FAILED"); return; }
    CloseHandle(t);
    if (per) *need_frame = 1;
    api->log(TAG, "periodic mi_collect(force=%d) on main thread when commit +%lld MiB (check %d s, min interval %llu min, max %d ms): %s;"
             " census every %ld min", g_per_force, g_per.grow_mb, g_per_check_sec, (unsigned long long)(g_per.min_interval_ms / 60000),
             g_per_max_ms, per ? "on" : "off", g_census_min);
}

static void cmd_memstat(const char *args, void *u)
{
    (void)args; (void)u;
    char b[1000], m[200]; census_line(b, sizeof b, "console"); mem_line(m, sizeof m);
    api->print("%s\n%s; periodic collect %s, %ld runs, %lld MiB returned; thread collect %s, gen %ld, %lld runs, %.1f ms total, max %.1f ms",
               b, m, g_per_enabled ? "on" : "off", g_per_runs, g_per_saved_mb, g_tc_on ? "on" : g_tc_ok ? "armed/off" : "unavailable",
               g_tc_gen, (long long)g_tc_runs, g_tc_us_total / 1000.0, g_tc_us_max / 1000.0);
    mi_lines(0, args && strstr(args, "threads") != NULL);
}

/* ------------------------------------------------------------------ 6. GUI animation pool diag
 * Pass-through hook on the pool insert (0x39304d0 in 1.15.1): remembers the pool pointer so the
 * census can log live slots / capacity. Changes nothing. No cap/eviction: see docs/re/memory.md 10. */
typedef uintptr_t (*pool_insert_fn)(PASS8_ARGS);
static pool_insert_fn g_orig_pool_insert;
static uintptr_t pool_insert_detour(PASS8_ARGS)
{
    void *pool = a0_;
    int i;
    for (i = 0; i < MT_MAXPOOLS; i++) if (g_pools[i] == pool) break;
    if (i == MT_MAXPOOLS)
        for (i = 0; i < MT_MAXPOOLS; i++)
            if (InterlockedCompareExchangePointer((void *volatile *)&g_pools[i], pool, NULL) == NULL) {
                api->log(TAG, "gui anim pool %d at %p (first insert)", i, pool); break;
            }
    return g_orig_pool_insert(PASS8);
}
static void setup_guipool(void)
{
    if (!cfg("gui_anim_pool", "diag", 1)) return;
    unsigned char *text; size_t tn;
    if (!section(".text", &text, &tn)) return;
    ptrdiff_t off = mt_find_unique(text, tn, MT_POOL_PAT, MT_POOL_MASK, sizeof MT_POOL_PAT);
    unsigned char *fn = off >= MT_POOL_PAT_OFF ? text + off - MT_POOL_PAT_OFF : NULL;
    if (!fn || api->function_start(fn + MT_POOL_PAT_OFF) != fn || memcmp(fn, MT_SIG_POOL_PROLOGUE, sizeof MT_SIG_POOL_PROLOGUE)) {
        api->log(TAG, "gui anim pool insert not found (%s): diag off", off == -2 ? "ambiguous" : "no match"); return;
    }
    int rc = api->hook(fn, (void *)pool_insert_detour, (void **)&g_orig_pool_insert);
    api->log(TAG, "gui anim pool insert at exe+0x%llx: diag hook %s (rc %d)", rva(fn), rc ? "FAILED" : "installed", rc);
}

static void on_frame(void *u)
{
    (void)u;
    if (g_tc_ok && !g_tc_armed) { g_tc_armed = 1; InterlockedExchange(&g_tc_on, 1); }
    if (InterlockedExchange(&g_pending_collect, 0)) g_frames_left = g_collect_delay_frames;
    if (g_frames_left > 0 && --g_frames_left == 0) {
        g_frames_left = -1; do_collect("after transition");
        g_done_mb = (LONG64)proc_commit_mb(); g_done_ms = (LONG64)GetTickCount64(); InterlockedExchange(&g_done_flag, 1);
        InterlockedExchange(&g_census_now, 1); if (g_mon_event) SetEvent(g_mon_event);
    }
    if (InterlockedExchange(&g_per_pending, 0) && g_per_enabled && g_collect) periodic_collect_main();
}

static uintptr_t transition_detour(PASS8_ARGS)
{
    uintptr_t r = g_orig_transition(PASS8);
    InterlockedExchange(&g_pending_collect, 1);
    InterlockedIncrement(&g_tc_gen);
    return r;
}

static void on_load_transition(const char *name, void *data, void *user)
{
    (void)name; (void)user;
    const V3X_LoadTransition *t = (const V3X_LoadTransition *)data;
    if (t) api->log(TAG, "load_transition %s -> %s (%.1f s): mi_collect in %d frames", t->from ? t->from : "?",
                    t->to ? t->to : "?", t->seconds, g_collect_delay_frames);
    InterlockedExchange(&g_pending_collect, 1);
    InterlockedIncrement(&g_tc_gen);                 /* every thread collects once after a load */
}

static void main_collect(void *u) { (void)u; do_collect("console"); char m[200]; mem_line(m, sizeof m); api->print("memcollect done: %s", m); }
static void cmd_memcollect(const char *args, void *u)
{
    (void)args; (void)u;
    InterlockedIncrement(&g_tc_gen);                 /* worker heaps too (thread collect) */
    if (api->run_on_main(main_collect, NULL)) api->print("memcollect: queue full");
}

static void setup_collect(int *need_frame)
{
    void *s = api->find_string("process done: 0x%zx\n");
    void *x = s ? api->find_xref(s, 0) : NULL;
    unsigned char *fn = x ? api->function_start(x) : NULL;
    uint64_t c = fn ? mt_find_movcl1_call(fn, (size_t)((unsigned char *)x - fn), (uint64_t)fn) : 0;
    if (!c || !in_image(c, sizeof MT_SIG_MI_COLLECT) || memcmp((void *)c, MT_SIG_MI_COLLECT, sizeof MT_SIG_MI_COLLECT)) {
        api->log(TAG, "mi_collect not found / signature mismatch: collect disabled"); return;
    }
    g_collect = (mi_collect_fn)c;
    api->log(TAG, "mi_collect at exe+0x%llx", rva((void *)c));
    g_collect_delay_frames = cfg("collect", "delay_frames", 30);
    if (cfg("collect", "after_transition", 1)) {
        /* API v3: fast_load owns the transition hook and emits "load_transition" (main thread).
         * Older loaders (or own_hook=1): hook the transition fn ourselves. */
        int via_event = api->api_version >= 3 && api->struct_size >= offsetof(V3X_API, on_event) + sizeof(void *)
                        && !cfg("collect", "own_hook", 0);
        int ok = 0;
        if (via_event) {
            int rc = api->on_event("load_transition", on_load_transition, NULL);
            api->log(TAG, "subscribed to event load_transition: %s (rc %d)", rc == 0 ? "ok" : "FAILED", rc);
            ok = rc == 0;
        } else {
            void *ts = api->find_string("Transition {} took: {} seconds");
            void *tx = ts ? api->find_xref(ts, 0) : NULL;
            void *tf = tx ? api->function_start(tx) : NULL;
            int rc = tf ? api->hook(tf, (void *)transition_detour, (void **)&g_orig_transition) : -1;
            api->log(TAG, "transition fn %s%llx: hook %s (rc %d)", tf ? "exe+0x" : "?", tf ? rva(tf) : 0ULL, rc == 0 ? "installed" : "FAILED", rc);
            ok = rc == 0;
        }
        if (ok) *need_frame = 1;
    }
    if (api->api_version >= 2 && api->struct_size >= offsetof(V3X_API, exec_command) + sizeof(void *))
        api->register_command("memcollect", "mimalloc: return free memory to Windows (mi_collect(true))", cmd_memcollect, NULL);
}

/* ------------------------------------------------------------------ 3. OOM guard */

static void oom_handler(void)
{
    void *r = InterlockedExchangePointer((void *volatile *)&g_reserve, NULL);
    char m[200];
    if (r) VirtualFree(r, 0, MEM_RELEASE);
    if (g_collect) g_collect(1);
    InterlockedIncrement(&g_rescues);
    mem_line(m, sizeof m);
    api->log(TAG, "WARNING: out of memory in operator new; released %llu MiB reserve + mi_collect, retrying. %s. "
             "Windows commit limit is exhausted: enlarge the pagefile or lower texture settings.",
             r ? (unsigned long long)(g_reserve_size >> 20) : 0ULL, m);
}

static new_handler_t gnh_detour(PASS8_ARGS)
{
    new_handler_t h = g_orig_gnh(PASS8);
    if (h) return h;                                   /* game installed its own: respect it */
    LONG k = InterlockedIncrement(&g_oom_events);
    if (g_reserve) return oom_handler;                 /* rescue once per reserve */
    if (k <= 16) {
        char m[200]; mem_line(m, sizeof m);
        api->log(TAG, "WARNING: out of memory again (event %ld, reserve spent): std::bad_alloc will be thrown. %s", k, m);
    }
    return NULL;
}

static void setup_oom(void)
{
    if (!cfg("oom", "enabled", 1)) return;
    void *s = api->find_string("out of memory in 'new'");
    void *x = s ? api->find_xref(s, 0) : NULL;
    unsigned char *fn = x ? api->function_start(x) : NULL;
    uint64_t g = fn && (unsigned char *)x > fn ? mt_find_call_to(fn, (size_t)((unsigned char *)x - fn), (uint64_t)fn,
                                       MT_SIG_GET_NEW_HANDLER, sizeof MT_SIG_GET_NEW_HANDLER, img_peek, NULL) : 0;
    if (!g) { api->log(TAG, "get_new_handler not found: OOM guard disabled"); return; }
    int mb = cfg("oom", "reserve_mb", 128);
    if (mb > 0) {
        g_reserve_size = (SIZE_T)mb << 20;
        g_reserve = VirtualAlloc(NULL, g_reserve_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);  /* commit only, never touched */
    }
    int rc = api->hook((void *)g, (void *)gnh_detour, (void **)&g_orig_gnh);
    api->log(TAG, "get_new_handler at exe+0x%llx: hook %s; reserve %d MiB %s", rva((void *)g), rc ? "FAILED" : "installed",
             mb, mb <= 0 ? "(off)" : g_reserve ? "committed" : "FAILED");

    /* 4. error handler global (optional) */
    if (!cfg("oom", "log_mimalloc_errors", 1)) return;
    uint64_t em = mt_find_errmsg_call((const unsigned char *)x, 0x20, (uint64_t)x, (uint64_t)s);
    if (!em || !in_image(em, 0x200)) { api->log(TAG, "_mi_error_message not found: error log off"); return; }
    uint64_t h = mt_find_error_handler_global((const unsigned char *)em, 0x200, em);
    if (!h || !in_image(h, 16)) { api->log(TAG, "mi_error_handler global not found: error log off"); return; }
    g_err_log_max = cfg("oom", "max_error_logs", 32);
    if (*(void **)h == NULL) { *(void **)h = (void *)mt_error_fun; api->log(TAG, "mi_error_handler at exe+0x%llx registered", rva((void *)h)); }
    else api->log(TAG, "mi_error_handler already set, left alone");
}

static void mt_error_fun(int err, void *arg)
{
    (void)arg;
    LONG k = InterlockedIncrement(&g_err_logs);
    if (k > g_err_log_max) return;
    char m[200]; mem_line(m, sizeof m);
    api->log(TAG, "mimalloc error %d (%s)%s. %s", err, err == 12 ? "ENOMEM" : err == 14 ? "EFAULT/corruption" : err == 22 ? "EINVAL" : err == 75 ? "EOVERFLOW" : "?",
             k == g_err_log_max ? " [further errors not logged]" : "", m);
}

/* ------------------------------------------------------------------ entry */

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    g_base = (unsigned char *)api->image_base(); g_size = api->image_size();
    snprintf(g_ini, sizeof g_ini, "%smem_tuner.ini", api->v3x_dir());
    if (!cfg("mem_tuner", "enabled", 1)) { api->log(TAG, "disabled in %s", g_ini); return 0; }
    apply_options();
    int need_frame = 0;
    setup_collect(&need_frame);
    setup_oom();                                       /* before periodic: census reports the reserve */
    setup_guipool();
    setup_mi();
    setup_thread_collect();
    if (g_tc_ok) need_frame = 1;
    setup_periodic(&need_frame);
    if (need_frame) api->on_frame(on_frame, NULL);
    if (api->api_version >= 2 && api->struct_size >= offsetof(V3X_API, exec_command) + sizeof(void *))
        api->register_command("memstat", "memstat [threads]: commit census, mimalloc in-use/free walk, collect stats", cmd_memstat, NULL);
    return 0;
}
