/* devtools: who uses CPU and RAM inside the game.
 *
 * One background thread ("v3x_devtools") does everything except frame timing:
 *   - thread monitor: CPU% per thread (GetThreadTimes deltas), name, start address,
 *     stack-root function, ideal processor and P/E core class
 *   - sampling profiler (optional): ~1 kHz, suspends the main thread + the K busiest threads,
 *     reads RIP and copies the top of the stack, resumes, then attributes the sample to
 *     functions (self + inclusive via RtlVirtualUnwind on the copied stack)
 *   - memory monitor: process counters, VirtualQuery walk, heaps, DXGI video memory
 *   - reports to v3x/devtools.log every interval_sec and on the hotkey;
 *     v3x/profile_latest.csv (cumulative samples by RVA) for tools/symbolize.py,
 *     v3x/profile_edges.csv (caller->callee sample counts along unwound stacks)
 *   - API v3: on each "load_transition" event, snapshot to v3x/profile_<from>_to_<to>_<n>.csv and
 *     profile_edges_<from>_to_<to>_<n>.csv (+ profile_edges.csv), then reset the cumulative tables;
 *     "load_transition_begin" does the same as profile_<from>_before_<to>_<n>.csv, so the end
 *     snapshot covers exactly the transition
 * on_frame (main thread) only records frame-to-frame time and polls the hotkey.
 *
 * Config: v3x/devtools.ini (key=value, ; or # comments)
 *   profile=1          start the profiler (also: env V3X_PROFILE=1). Ctrl+hotkey toggles it.
 *                      With API v2 the console command "profile on|off|report|reset" also works.
 *   interval_sec=10    report period
 *   hotkey_report=F10  F1..F24, a VK code (0x79 / 121), or none
 *   sample_hz=1000     sampling rate per sampled thread
 *   sample_threads=4   busiest threads sampled in addition to the main thread
 *   unwind_depth=8     frames walked per sample for inclusive attribution (1 = RIP only)
 *
 * Rule for the sampler: between SuspendThread and ResumeThread only syscalls and memcpy into
 * preallocated buffers. No malloc, no CRT I/O, no loader calls, no RtlLookupFunctionEntry
 * (the suspended thread may hold the heap, loader or function-table locks).
 * Process exit (API v3 core event "process_exit", on the exiting thread before ExitProcess kills the
 * other threads): sampling stops for good and the exiting thread waits until no thread is suspended.
 * Without that, ExitProcess could kill this thread between SuspendThread and ResumeThread of the
 * exiting thread itself, which then stays suspended forever (the game hung at exit with profile=1).
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <objbase.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "v3x.h"

V3X_EXPORT const char *v3x_plugin_name = "devtools";

#define TAG "devtools"
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif

/* ------------------------------------------------------------------ config / state */

static struct {
    int profile, interval_sec, hotkey, hz, top_threads, depth;
} cfg = {0, 10, VK_F10, 1000, 4, 8};

static const V3X_API *api;
static DWORD g_pid, g_main_tid, g_self_tid;
static HANDLE g_self_thread;
static uintptr_t g_exe_base, g_exe_end, g_text_lo, g_text_hi;
static struct { uintptr_t lo, hi; } g_data[8]; static int g_ndata;
static char g_exe_name[64];
static char g_log_path[MAX_PATH], g_csv_path[MAX_PATH], g_edges_path[MAX_PATH];
static volatile LONG g_profiling;
/* exit handshake (Dekker, interlocked = full barriers): the sampler raises g_in_suspend before it
 * reads g_exit_seen; on_process_exit sets g_exit_seen before it reads g_in_suspend. */
static volatile LONG g_exit_seen, g_in_suspend;
static HANDLE g_wake;                     /* auto-reset: report / toggle requests */
static volatile LONG g_req_report;       /* REQ_*; handled on the devtools thread */
enum { REQ_NONE, REQ_REPORT, REQ_TOGGLE, REQ_ON, REQ_OFF, REQ_RESET };
static LARGE_INTEGER g_qpf;

/* dynamic imports */
typedef HRESULT(WINAPI *GetThreadDescription_t)(HANDLE, PWSTR *);
typedef LONG(NTAPI *NtQueryInformationThread_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef LONG(NTAPI *NtGetNextThread_t)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);
typedef BOOL(WINAPI *GetThreadIdealProcessorEx_t)(HANDLE, PPROCESSOR_NUMBER);
typedef BOOL(WINAPI *GetSystemCpuSetInformation_t)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
typedef BOOL(WINAPI *HeapSummary_t)(HANDLE, DWORD, LPHEAP_SUMMARY);
typedef SHORT(WINAPI *GetAsyncKeyState_t)(int);
typedef HRESULT(WINAPI *CreateDXGIFactory1_t)(REFIID, void **);
typedef UINT(WINAPI *timeBeginPeriod_t)(UINT);
static GetThreadDescription_t pGetThreadDescription;
static NtQueryInformationThread_t pNtQueryInformationThread;
static NtGetNextThread_t pNtGetNextThread;
static GetThreadIdealProcessorEx_t pGetThreadIdealProcessorEx;
static GetSystemCpuSetInformation_t pGetSystemCpuSetInformation;
static HeapSummary_t pHeapSummary;
static GetAsyncKeyState_t pGetAsyncKeyState;

static double qpc_sec(LONGLONG t) { return (double)t / (double)g_qpf.QuadPart; }
static LONGLONG qpc_now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

/* ------------------------------------------------------------------ ini */

static int parse_vk(const char *v)
{
    if (!_stricmp(v, "none") || !_stricmp(v, "off")) return 0;
    if ((v[0] == 'F' || v[0] == 'f') && isdigit((unsigned char)v[1])) {
        int n = atoi(v + 1);
        return n >= 1 && n <= 24 ? VK_F1 + n - 1 : VK_F10;
    }
    return (int)strtol(v, NULL, 0);
}

static void load_config(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%sdevtools.ini", api->v3x_dir());
    FILE *f = fopen(path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            if (*s == ';' || *s == '#' || *s == '[' || !*s) continue;
            char *eq = strchr(s, '=');
            if (!eq) continue;
            *eq = 0;
            char *k = s, *v = eq + 1;
            for (char *e = eq - 1; e >= k && (*e == ' ' || *e == '\t'); e--) *e = 0;
            while (*v == ' ' || *v == '\t') v++;
            for (char *e = v + strlen(v) - 1; e >= v && strchr(" \t\r\n", *e); e--) *e = 0;
            if (!_stricmp(k, "profile")) cfg.profile = atoi(v);
            else if (!_stricmp(k, "interval_sec")) cfg.interval_sec = atoi(v);
            else if (!_stricmp(k, "hotkey_report")) cfg.hotkey = parse_vk(v);
            else if (!_stricmp(k, "sample_hz")) cfg.hz = atoi(v);
            else if (!_stricmp(k, "sample_threads")) cfg.top_threads = atoi(v);
            else if (!_stricmp(k, "unwind_depth")) cfg.depth = atoi(v);
        }
        fclose(f);
    }
    char env[16];
    if (GetEnvironmentVariableA("V3X_PROFILE", env, sizeof env) && env[0] == '1') cfg.profile = 1;
    if (cfg.interval_sec < 1) cfg.interval_sec = 1;
    if (cfg.hz < 10) cfg.hz = 10;
    if (cfg.hz > 4000) cfg.hz = 4000;
    if (cfg.top_threads < 0) cfg.top_threads = 0;
    if (cfg.top_threads > 32) cfg.top_threads = 32;
    if (cfg.depth < 1) cfg.depth = 1;
    if (cfg.depth > 32) cfg.depth = 32;
}

/* ------------------------------------------------------------------ thread names set via exception 0x406D1388 */

#define VEH_NAMES 512
static struct { volatile LONG tid; char name[64]; } g_veh_names[VEH_NAMES];

static LONG CALLBACK veh_thread_name(PEXCEPTION_POINTERS ep)
{
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    if (r->ExceptionCode != 0x406D1388 || r->NumberParameters < 3 || r->ExceptionInformation[0] != 0x1000)
        return EXCEPTION_CONTINUE_SEARCH;
    const char *name = (const char *)r->ExceptionInformation[1];
    DWORD tid = (DWORD)r->ExceptionInformation[2];
    if (tid == (DWORD)-1) tid = GetCurrentThreadId();
    if (!name || !tid) return EXCEPTION_CONTINUE_SEARCH;
    for (unsigned i = 0, h = (tid * 2654435761u) % VEH_NAMES; i < VEH_NAMES; i++, h = (h + 1) % VEH_NAMES) {
        LONG cur = g_veh_names[h].tid;
        if (cur == (LONG)tid || (cur == 0 && InterlockedCompareExchange(&g_veh_names[h].tid, (LONG)tid, 0) == 0)) {
            int n = 0;
            for (; n < 63 && name[n]; n++) g_veh_names[h].name[n] = name[n];
            g_veh_names[h].name[n] = 0;
            break;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;   /* let the debugger / the game's own handler see it */
}

static const char *veh_name(DWORD tid)
{
    for (unsigned i = 0, h = (tid * 2654435761u) % VEH_NAMES; i < VEH_NAMES; i++, h = (h + 1) % VEH_NAMES) {
        if (g_veh_names[h].tid == (LONG)tid) return g_veh_names[h].name;
        if (g_veh_names[h].tid == 0) break;
    }
    return NULL;
}

/* ------------------------------------------------------------------ frame timing (main thread) */

#define FT_CAP 65536
typedef struct { float ms[FT_CAP]; int n; long long count; double sum, min, max; int over33, over100; } FrameBuf;
static FrameBuf g_fb[2];
static int g_fb_cur;
static CRITICAL_SECTION g_fb_lock;
static LONGLONG g_last_frame;
static int g_key_down;

static void on_frame(void *user)
{
    (void)user;
    LONGLONG now = qpc_now();
    if (g_last_frame) {
        double ms = qpc_sec(now - g_last_frame) * 1000.0;
        EnterCriticalSection(&g_fb_lock);
        FrameBuf *b = &g_fb[g_fb_cur];
        if (b->n < FT_CAP) b->ms[b->n++] = (float)ms;
        if (!b->count || ms < b->min) b->min = ms;
        if (ms > b->max) b->max = ms;
        b->count++; b->sum += ms;
        b->over33 += ms > 33.4; b->over100 += ms > 100.0;
        LeaveCriticalSection(&g_fb_lock);
    }
    g_last_frame = now;
    if (cfg.hotkey && pGetAsyncKeyState) {
        int down = (pGetAsyncKeyState(cfg.hotkey) & 0x8000) != 0;
        if (down && !g_key_down) {
            int ctrl = (pGetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            InterlockedExchange(&g_req_report, ctrl ? REQ_TOGGLE : REQ_REPORT);
            SetEvent(g_wake);
        }
        g_key_down = down;
    }
}

/* ------------------------------------------------------------------ modules */

/* Fault-proof read of memory we do not own: another thread's TEB/stack (it can exit and free them
 * at any moment) or a DLL image (it can be unloaded), and an access violation on the devtools thread would kill the game.
 * ReadProcessMemory on our own process is one syscall (NtReadVirtualMemory, no user-mode locks),
 * so it is also allowed while a target is suspended. Returns 1 only if all n bytes were read. */
static int safe_read(uintptr_t a, void *buf, size_t n)
{
    SIZE_T got = 0;
    return a && ReadProcessMemory(GetCurrentProcess(), (const void *)a, buf, n, &got) && got == n;
}


typedef struct { uintptr_t base, end; char name[64]; } Mod;
#define MAX_MODS 1024
static Mod g_mods[MAX_MODS]; static int g_nmods;

static int mod_cmp(const void *a, const void *b)
{
    uintptr_t x = ((const Mod *)a)->base, y = ((const Mod *)b)->base;
    return x < y ? -1 : x > y;
}

static void refresh_modules(void)
{
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    for (int tries = 0; s == INVALID_HANDLE_VALUE && GetLastError() == ERROR_BAD_LENGTH && tries < 5; tries++)
        s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (s == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32 me; me.dwSize = sizeof me;
    int n = 0;
    for (BOOL ok = Module32First(s, &me); ok && n < MAX_MODS; ok = Module32Next(s, &me)) {
        g_mods[n].base = (uintptr_t)me.modBaseAddr;
        g_mods[n].end = (uintptr_t)me.modBaseAddr + me.modBaseSize;
        snprintf(g_mods[n].name, sizeof g_mods[n].name, "%s", me.szModule);
        n++;
    }
    CloseHandle(s);
    qsort(g_mods, n, sizeof(Mod), mod_cmp);
    g_nmods = n;
}

static const Mod *mod_find(uintptr_t a)
{
    int lo = 0, hi = g_nmods - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (a < g_mods[m].base) hi = m - 1;
        else if (a >= g_mods[m].end) lo = m + 1;
        else return &g_mods[m];
    }
    return NULL;
}

static void fmt_addr(uintptr_t a, char *out, size_t cap)
{
    const Mod *m = mod_find(a);
    if (m) snprintf(out, cap, "%s+0x%llX", m->name, (unsigned long long)(a - m->base));
    else snprintf(out, cap, "0x%llX", (unsigned long long)a);
}

/* Nearest export at or below `a` (system DLLs have no source strings but do have exports). */
static int nearest_export(const Mod *m, uintptr_t a, char *out, size_t cap)
{
    /* every read goes through safe_read: the DLL may be unloaded while we look at it */
    uintptr_t b = m->base;
    IMAGE_DOS_HEADER dos; IMAGE_NT_HEADERS nt; IMAGE_EXPORT_DIRECTORY ed;
    if (!safe_read(b, &dos, sizeof dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;
    if (!safe_read(b + dos.e_lfanew, &nt, sizeof nt)) return 0;
    IMAGE_DATA_DIRECTORY dd = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd.VirtualAddress || !dd.Size || !safe_read(b + dd.VirtualAddress, &ed, sizeof ed)) return 0;
    DWORD nn = ed.NumberOfNames, nf = ed.NumberOfFunctions;
    if (!nn || !nf || nn > 65536 || nf > 65536) return 0;
    DWORD *funcs = malloc(nf * 4), *names = malloc(nn * 4);
    WORD *ords = malloc(nn * 2);
    DWORD rva = (DWORD)(a - b), best = 0, bestname = 0;
    int ok = funcs && names && ords && safe_read(b + ed.AddressOfFunctions, funcs, nf * 4) &&
             safe_read(b + ed.AddressOfNames, names, nn * 4) && safe_read(b + ed.AddressOfNameOrdinals, ords, nn * 2);
    for (DWORD i = 0; ok && i < nn; i++) {
        WORD o = ords[i];
        if (o >= nf) continue;
        DWORD f = funcs[o];
        if (f >= dd.VirtualAddress && f < dd.VirtualAddress + dd.Size) continue;  /* forwarder */
        if (f <= rva && f >= best) { best = f; bestname = names[i]; }
    }
    free(funcs); free(names); free(ords);
    if (!ok || !bestname || rva - best > 0x10000) return 0;
    char nm[97] = {0};
    uintptr_t na = b + bestname;
    size_t n = m->end > na && m->end - na < 96 ? m->end - na : 96;
    if (!safe_read(na, nm, n)) return 0;
    nm[n] = 0;
    if (rva == best) snprintf(out, cap, "%s", nm);
    else snprintf(out, cap, "%s+0x%lX", nm, rva - best);
    return 1;
}

/* ------------------------------------------------------------------ function lookup */

static PRUNTIME_FUNCTION primary_rf(DWORD64 ib, PRUNTIME_FUNCTION rf)
{
    for (int guard = 0; rf && guard < 32; guard++) {
        if (rf->UnwindData & 1) { rf = (PRUNTIME_FUNCTION)(ib + (rf->UnwindData & ~1u)); continue; }
        uint8_t *ui = (uint8_t *)ib + rf->UnwindData;
        if (!((ui[0] >> 3) & UNW_FLAG_CHAININFO)) break;
        rf = (PRUNTIME_FUNCTION)(ui + 4 + ((ui[2] + 1) & ~1u) * 2);
    }
    return rf;
}

/* RIP -> function start, direct-mapped cache in front of RtlLookupFunctionEntry.
 * Addresses without .pdata (leaf functions, JIT code) map to their 64-byte block, flagged. */
#define FC_BITS 16
static struct { uintptr_t addr, fn; } g_fcache[1u << FC_BITS];
#define NOPDATA_FLAG ((uintptr_t)1 << 63)

static uintptr_t func_of(uintptr_t a)
{
    unsigned h = (unsigned)((a * 0x9E3779B97F4A7C15ull) >> (64 - FC_BITS));
    if (g_fcache[h].addr == a && a) return g_fcache[h].fn;
    DWORD64 ib = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(a, &ib, NULL);
    uintptr_t fn = rf ? (uintptr_t)(ib + primary_rf(ib, rf)->BeginAddress) : ((a & ~(uintptr_t)63) | NOPDATA_FLAG);
    g_fcache[h].addr = a; g_fcache[h].fn = fn;
    return fn;
}

/* ------------------------------------------------------------------ hot function tables */

typedef struct { uintptr_t key; uint32_t self, incl; } Hot;
#define HOT_BITS 14
#define HOT_CAP (1u << HOT_BITS)
typedef struct { Hot e[HOT_CAP]; uint32_t used; uint64_t samples, dropped; } HotTab;
static HotTab g_hot_iv, g_hot_all;

static void hot_add(HotTab *t, uintptr_t key, int self, int incl)
{
    unsigned h = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> (64 - HOT_BITS));
    for (unsigned i = 0; i < HOT_CAP; i++, h = (h + 1) & (HOT_CAP - 1)) {
        Hot *e = &t->e[h];
        if (e->key == key) { e->self += self; e->incl += incl; return; }
        if (!e->key) {
            if (t->used >= HOT_CAP * 7 / 8) { t->dropped++; return; }
            e->key = key; e->self = self; e->incl = incl; t->used++;
            return;
        }
    }
    t->dropped++;
}

/* Caller->callee edges seen along unwound stacks (cumulative, reset with g_hot_all).
 * Fixed open-addressing table: no allocation on the sampler path; inserts are dropped when full. */
typedef struct { uintptr_t caller, callee; uint32_t samples; } Edge;
#define EDGE_BITS 16
#define EDGE_CAP (1u << EDGE_BITS)
static struct { Edge e[EDGE_CAP]; uint32_t used; uint64_t dropped; } g_edges;

static void edge_add(uintptr_t caller, uintptr_t callee)
{
    uint64_t k = caller * 0x9E3779B97F4A7C15ull ^ (callee + 0x632BE59BD9B4E019ull) * 0xC2B2AE3D27D4EB4Full;
    unsigned h = (unsigned)(k >> (64 - EDGE_BITS));
    for (unsigned i = 0; i < EDGE_CAP; i++, h = (h + 1) & (EDGE_CAP - 1)) {
        Edge *e = &g_edges.e[h];
        if (e->caller == caller && e->callee == callee) { e->samples++; return; }
        if (!e->caller) {
            if (g_edges.used >= EDGE_CAP * 7 / 8) { g_edges.dropped++; return; }
            e->caller = caller; e->callee = callee; e->samples = 1; g_edges.used++;
            return;
        }
    }
    g_edges.dropped++;
}

/* ------------------------------------------------------------------ threads */

typedef struct {
    DWORD tid; HANDLE h; int seen, can_sample, fresh;
    uintptr_t teb, start;
    ULONG64 cpu_rep, cpu_ref, cycles;
    double recent;      /* fraction of one core over the last refresh period */
    uint32_t samples;   /* this report interval */
} Thr;
#define MAX_THR 1024
static Thr g_thr[MAX_THR]; static int g_nthr;
static int g_targets[33]; static int g_ntargets;
static LONGLONG g_last_refresh;

typedef struct { LONG ExitStatus; PVOID Teb; HANDLE Pid, Tid; ULONG_PTR Affinity; LONG Prio, BasePrio; } TBI;

static ULONG64 ft64(FILETIME f) { return ((ULONG64)f.dwHighDateTime << 32) | f.dwLowDateTime; }
static ULONG64 thread_cpu(HANDLE h)
{
    FILETIME c, e, k, u;
    return GetThreadTimes(h, &c, &e, &k, &u) ? ft64(k) + ft64(u) : 0;
}

#define THR_ACCESS (THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT)

/* Record a thread seen during enumeration. Takes ownership of `h` (may be NULL: open by tid). */
static void see_thread(DWORD tid, HANDLE h)
{
    for (int i = 0; i < g_nthr; i++)
        if (g_thr[i].tid == tid) { g_thr[i].seen = 1; if (h) CloseHandle(h); return; }
    if (g_nthr >= MAX_THR) { if (h) CloseHandle(h); return; }
    Thr *t = &g_thr[g_nthr];
    memset(t, 0, sizeof *t);
    t->tid = tid;
    t->h = h ? h : OpenThread(THR_ACCESS, FALSE, tid);
    t->can_sample = t->h != NULL;
    if (!t->h) t->h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!t->h) return;
    g_nthr++;
    t->seen = 1;
    if (pNtQueryInformationThread) {
        PVOID sa = 0; TBI tbi;
        if (pNtQueryInformationThread(t->h, 9, &sa, sizeof sa, NULL) == 0) t->start = (uintptr_t)sa;
        if (pNtQueryInformationThread(t->h, 0, &tbi, sizeof tbi, NULL) == 0) t->teb = (uintptr_t)tbi.Teb;
    }
    t->cpu_rep = t->cpu_ref = thread_cpu(t->h);
    /* seed the recent CPU rate from the thread's lifetime so new busy threads get sampled at once */
    FILETIME c, e, k, u, nowf;
    GetSystemTimeAsFileTime(&nowf);
    if (GetThreadTimes(t->h, &c, &e, &k, &u) && ft64(nowf) > ft64(c) + 100000)
        t->recent = (double)(ft64(k) + ft64(u)) / (double)(ft64(nowf) - ft64(c));
    t->fresh = 1;
}

static void refresh_threads(void)
{
    LONGLONG now = qpc_now();
    double wall = g_last_refresh ? qpc_sec(now - g_last_refresh) : 0;
    g_last_refresh = now;
    for (int i = 0; i < g_nthr; i++) g_thr[i].seen = 0;
    if (pNtGetNextThread) {
        /* walks only this process's threads (Toolhelp snapshots every thread in the system: ~60 ms CPU) */
        HANDLE prev = NULL, next;
        while (pNtGetNextThread(GetCurrentProcess(), prev, THR_ACCESS, 0, 0, &next) == 0) {
            if (prev) CloseHandle(prev);
            DWORD tid = GetThreadId(next);
            if (tid) see_thread(tid, NULL);   /* opens its own handle only for new threads */
            prev = next;
        }
        if (prev) CloseHandle(prev);
    } else {
        HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (s == INVALID_HANDLE_VALUE) return;
        THREADENTRY32 te; te.dwSize = sizeof te;
        for (BOOL ok = Thread32First(s, &te); ok; ok = Thread32Next(s, &te))
            if (te.th32OwnerProcessID == g_pid) see_thread(te.th32ThreadID, NULL);
        CloseHandle(s);
    }
    /* drop exited threads, update recent CPU */
    int w = 0;
    for (int i = 0; i < g_nthr; i++) {
        Thr *t = &g_thr[i];
        if (!t->seen || WaitForSingleObject(t->h, 0) == WAIT_OBJECT_0) { CloseHandle(t->h); continue; }
        ULONG64 c = thread_cpu(t->h);
        if (!t->fresh) t->recent = wall > 0 ? (double)(c - t->cpu_ref) / 1e7 / wall : 0;
        t->fresh = 0;
        t->cpu_ref = c;
        if (w != i) g_thr[w] = *t;
        w++;
    }
    g_nthr = w;
    /* sampling targets: main thread + the busiest others */
    g_ntargets = 0;
    for (int i = 0; i < g_nthr; i++)
        if (g_thr[i].tid == g_main_tid && g_thr[i].can_sample) g_targets[g_ntargets++] = i;
    for (int k = 0; k < cfg.top_threads; k++) {
        int best = -1;
        for (int i = 0; i < g_nthr; i++) {
            Thr *t = &g_thr[i];
            if (t->tid == g_main_tid || t->tid == g_self_tid || !t->can_sample || t->recent < 0.02) continue;
            int dup = 0;
            for (int j = 0; j < g_ntargets; j++) dup |= g_targets[j] == i;
            if (!dup && (best < 0 || t->recent > g_thr[best].recent)) best = i;
        }
        if (best < 0) break;
        g_targets[g_ntargets++] = best;
    }
}

/* ------------------------------------------------------------------ sampler */

#define STACK_COPY (16 * 1024)
#define STACK_SLACK (256 * 1024)  /* RtlVirtualUnwind may read past the copy on huge frames */
#define IDLE_CYCLES 50000         /* < ~1.5% of a 1 ms tick at 3 GHz */
static uint8_t *g_stack;
static CONTEXT g_ctx __attribute__((aligned(16)));
static CONTEXT g_uctx __attribute__((aligned(16)));
static struct { uint64_t taken, idle_skipped, failed; LONGLONG stall; } g_ps;


static void remap_regs(CONTEXT *c, uintptr_t lo, uintptr_t hi, intptr_t delta)
{
    DWORD64 *r = &c->Rax;
    for (int i = 0; i < 16; i++)
        if (r[i] >= lo && r[i] < hi) r[i] += delta;
}

static int frame_reg(DWORD64 ib, PRUNTIME_FUNCTION rf)
{
    for (int guard = 0; rf && guard < 32; guard++) {
        if (rf->UnwindData & 1) { rf = (PRUNTIME_FUNCTION)(ib + (rf->UnwindData & ~1u)); continue; }
        uint8_t *ui = (uint8_t *)ib + rf->UnwindData;
        if (ui[3] & 0x0F) return ui[3] & 0x0F;
        if (!((ui[0] >> 3) & UNW_FLAG_CHAININFO)) break;
        rf = (PRUNTIME_FUNCTION)(ui + 4 + ((ui[2] + 1) & ~1u) * 2);
    }
    return 0;
}

/* ---- code-address validation before RtlLookupFunctionEntry / RtlVirtualUnwind ----
 * Those read the module's .pdata/unwind info and code bytes; if the DLL is being unloaded (or the
 * address is JIT/dynamic code with a function-table callback) they can fault inside ntdll.
 * So before unwinding through an address we require it to be in a loaded module and keep that
 * module pinned (GetModuleHandleExW without UNCHANGED_REFCOUNT) until the sample is attributed.
 * Runs after ResumeThread, so loader calls are allowed here. Modules that never unload (exe,
 * ntdll, kernel32, kernelbase, this DLL) are checked by range only. */
typedef struct { uintptr_t lo, hi; HMODULE h; } Pin;
static Pin g_fixed[8]; static int g_nfixed;
static Pin g_pins[16]; static int g_npins;

static uintptr_t image_end(uintptr_t b)
{
    IMAGE_DOS_HEADER dos; IMAGE_NT_HEADERS nt;
    if (!safe_read(b, &dos, sizeof dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;
    if (!safe_read(b + dos.e_lfanew, &nt, sizeof nt) || nt.Signature != IMAGE_NT_SIGNATURE) return 0;
    return b + nt.OptionalHeader.SizeOfImage;
}

static void add_fixed(HMODULE h)
{
    uintptr_t e = h ? image_end((uintptr_t)h) : 0;
    if (e && g_nfixed < 8) { g_fixed[g_nfixed].lo = (uintptr_t)h; g_fixed[g_nfixed].hi = e; g_nfixed++; }
}

static void init_fixed_modules(void)
{
    HMODULE self = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void *)init_fixed_modules, &self);
    add_fixed(GetModuleHandleW(NULL));
    add_fixed(GetModuleHandleW(L"ntdll.dll"));
    add_fixed(GetModuleHandleW(L"kernel32.dll"));
    add_fixed(GetModuleHandleW(L"kernelbase.dll"));
    add_fixed(self);
}

/* 1 if `a` is code in a loaded module that stays mapped until unpin_all() */
static int code_ok(uintptr_t a)
{
    if (!a) return 0;
    for (int i = 0; i < g_nfixed; i++) if (a >= g_fixed[i].lo && a < g_fixed[i].hi) return 1;
    for (int i = 0; i < g_npins; i++) if (a >= g_pins[i].lo && a < g_pins[i].hi) return 1;
    if (g_npins >= 16) return 0;
    HMODULE h = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)a, &h) || !h) return 0;
    uintptr_t e = image_end((uintptr_t)h);
    if (!e || a < (uintptr_t)h || a >= e) { FreeLibrary(h); return 0; }
    g_pins[g_npins].lo = (uintptr_t)h; g_pins[g_npins].hi = e; g_pins[g_npins].h = h; g_npins++;
    return 1;
}

static void unpin_all(void)
{
    for (int i = 0; i < g_npins; i++) FreeLibrary(g_pins[i].h);
    g_npins = 0;
}

static void attribute_walk(uintptr_t orig_rsp, size_t copied)
{
    uintptr_t seen[32]; int ns = 0;
    /* RIP outside any loaded module (JIT, unloading DLL): count its 64-byte block, no stack */
    int leaf_ok = code_ok(g_ctx.Rip);
    uintptr_t fn = leaf_ok ? func_of(g_ctx.Rip) : ((g_ctx.Rip & ~(uintptr_t)63) | NOPDATA_FLAG);
    seen[ns++] = fn;
    hot_add(&g_hot_iv, fn, 1, 1);
    hot_add(&g_hot_all, fn, 1, 1);
    if (cfg.depth <= 1 || !copied || !leaf_ok) return;

    /* edges: (frame d+1 function, frame d function), each distinct edge once per sample */
    uintptr_t prev = fn, eseen[32][2]; int ne = 0;
    memcpy(&g_uctx, &g_ctx, sizeof g_uctx);
    uintptr_t lo = (uintptr_t)g_stack, hi = lo + copied;
    intptr_t delta = (intptr_t)(lo - orig_rsp);
    remap_regs(&g_uctx, orig_rsp, orig_rsp + copied, delta);
    for (int d = 1; d < cfg.depth; d++) {
        if (g_uctx.Rsp < lo || g_uctx.Rsp + 8 > hi) break;
        DWORD64 ib = 0, pc = g_uctx.Rip;
        if (!code_ok(pc)) break;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(pc, &ib, NULL);
        if (rf) {
            int fr = frame_reg(ib, rf);
            if (fr && ((&g_uctx.Rax)[fr] < lo || (&g_uctx.Rax)[fr] >= hi)) break;
            PVOID hd; DWORD64 ef;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, pc, rf, &g_uctx, &hd, &ef, NULL);
        } else {
            g_uctx.Rip = *(DWORD64 *)g_uctx.Rsp;
            g_uctx.Rsp += 8;
        }
        if (!g_uctx.Rip || g_uctx.Rsp < lo || g_uctx.Rsp > hi) break;
        remap_regs(&g_uctx, orig_rsp, orig_rsp + copied, delta);
        if (!code_ok(g_uctx.Rip - 1)) break;      /* return address not in a loaded module: stop */
        uintptr_t f = func_of(g_uctx.Rip - 1);   /* -1: return address may be past the call's function */
        if (f != prev && ne < 32) {
            int edup = 0;
            for (int j = 0; j < ne; j++) edup |= eseen[j][0] == f && eseen[j][1] == prev;
            if (!edup) { eseen[ne][0] = f; eseen[ne][1] = prev; ne++; edge_add(f, prev); }
        }
        prev = f;
        int dup = 0;
        for (int j = 0; j < ns; j++) dup |= seen[j] == f;
        if (dup) continue;
        seen[ns++] = f;
        hot_add(&g_hot_iv, f, 0, 1);
        hot_add(&g_hot_all, f, 0, 1);
    }
}

static void attribute_sample(uintptr_t orig_rsp, size_t copied)
{
    attribute_walk(orig_rsp, copied);
    unpin_all();
}

static void sample_tick(void)
{
    for (int k = 0; k < g_ntargets; k++) {
        Thr *t = &g_thr[g_targets[k]];
        ULONG64 cyc = 0;
        /* Skip threads that barely ran since the last tick: suspending a waiting thread forces a
         * wake-up (~100 us) and the sample would only show a wait stub. The suspend APC itself
         * charges ~10k cycles to the target, hence a threshold instead of equality. */
        if (QueryThreadCycleTime(t->h, &cyc) && cyc - t->cycles < IDLE_CYCLES) {
            g_ps.idle_skipped++;
            continue;
        }
        t->cycles = cyc;
        LONGLONG t0 = qpc_now();
        InterlockedExchange(&g_in_suspend, 1);
        if (g_exit_seen) { InterlockedExchange(&g_in_suspend, 0); return; }   /* process exiting: never suspend again */
        if (SuspendThread(t->h) == (DWORD)-1) { InterlockedExchange(&g_in_suspend, 0); g_ps.failed++; continue; }
        /* ---- target suspended: syscalls and memcpy only ---- */
        g_ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        BOOL ok = GetThreadContext(t->h, &g_ctx);
        size_t copied = 0;
        uintptr_t rsp = ok ? g_ctx.Rsp : 0;
        NT_TIB tib;   /* read live each sample: fibers switch stacks */
        if (ok && cfg.depth > 1 && safe_read(t->teb, &tib, sizeof tib)) {
            uintptr_t base = (uintptr_t)tib.StackBase, limit = (uintptr_t)tib.StackLimit;
            if (rsp >= limit && rsp < base) {
                size_t n = base - rsp < STACK_COPY ? base - rsp : STACK_COPY;
                copied = safe_read(rsp, g_stack, n) ? n : 0;
            }
        }
        ResumeThread(t->h);
        InterlockedExchange(&g_in_suspend, 0);
        /* ---- resumed ---- */
        g_ps.stall += qpc_now() - t0;
        if (!ok) { g_ps.failed++; continue; }
        g_ps.taken++;
        t->samples++;
        g_hot_iv.samples++; g_hot_all.samples++;
        attribute_sample(rsp, copied);
    }
}

/* ------------------------------------------------------------------ string hints */

static int in_data(uintptr_t a)
{
    for (int i = 0; i < g_ndata; i++)
        if (a >= g_data[i].lo && a < g_data[i].hi) return 1;
    return 0;
}

/* Copy the candidate string at `a` (up to 241 bytes, clipped to its data section) into buf[242].
 * Returns the number of bytes copied, 0 if unreadable. */
static int read_string(uintptr_t a, char *buf)
{
    for (int i = 0; i < g_ndata; i++) {
        if (a < g_data[i].lo || a >= g_data[i].hi) continue;
        size_t n = g_data[i].hi - a < 241 ? g_data[i].hi - a : 241;
        if (!safe_read(a, buf, n)) return 0;
        buf[n] = 0;
        return (int)n;
    }
    return 0;
}

/* 3 = source path, 2 = identifier-like, 1 = other text, 0 = not a string */
/* s = copy of up to `avail` bytes of the candidate (avail < 241: section ends there) */
static int classify_string(const char *s, int avail, int *len)
{
    int n = 0, alpha = 0, space = 0, ident = 1;
    for (; n < 240; n++) {
        if (n >= avail) return 0;   /* no NUL before the section end */
        unsigned char c = (unsigned char)s[n];
        if (!c) break;
        if (c < 0x20 || c > 0x7E) return 0;
        alpha |= isalpha(c) != 0;
        space |= c == ' ';
        if (!(isalnum(c) || c == '_' || c == ':' || c == '.' || c == '-' || c == '<' || c == '>')) ident = 0;
    }
    if (n >= 240 || n < 4 || !alpha) return 0;
    *len = n;
    const char *ext = strrchr(s, '.');
    if (ext && (!strcmp(ext, ".cpp") || !strcmp(ext, ".h") || !strcmp(ext, ".hpp") || !strcmp(ext, ".c") ||
                !strncmp(ext, ".cpp:", 5)))
        return 3;
    if (ident && !space) return 2;
    return n >= 6 ? 1 : 0;
}

static void shorten_path(const char *s, char *out, size_t cap)
{
    const char *p = s + strlen(s);
    int parts = 0;
    while (p > s) { p--; if ((*p == '\\' || *p == '/') && ++parts == 3) { p++; break; } }
    snprintf(out, cap, "%s", p);
}

/* Up to 2 strings referenced by `lea reg,[rip+x]` inside the function body; best class first. */
static void string_hints(uintptr_t fn, char *out, size_t cap)
{
    out[0] = 0;
    if (fn < g_exe_base || fn >= g_exe_end) return;
    DWORD64 ib = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(fn, &ib, NULL);
    if (!rf) return;
    rf = primary_rf(ib, rf);
    uintptr_t lo = ib + rf->BeginAddress, hi = ib + rf->EndAddress;
    if (hi - lo > 65536) hi = lo + 65536;
    static uint8_t body[65536];   /* devtools thread only */
    if (hi <= lo + 7 || !safe_read(lo, body, hi - lo)) return;
    static char best[2][242], cand[242];
    int bestc[2] = {0};
    for (size_t k = 0; k + 7 <= hi - lo; k++) {
        const uint8_t *p = body + k;
        if ((p[0] & 0xF0) != 0x40 || p[1] != 0x8D || (p[2] & 0xC7) != 0x05) continue;
        int32_t disp; memcpy(&disp, p + 3, 4);
        uintptr_t tgt = lo + k + 7 + disp;
        if (!in_data(tgt)) continue;
        int avail = read_string(tgt, cand), len, c = avail ? classify_string(cand, avail, &len) : 0;
        if (!c) continue;
        if ((bestc[0] && !strcmp(best[0], cand)) || (bestc[1] && !strcmp(best[1], cand))) continue;
        if (c > bestc[0]) { memcpy(best[1], best[0], sizeof best[1]); bestc[1] = bestc[0]; memcpy(best[0], cand, sizeof cand); bestc[0] = c; }
        else if (c > bestc[1]) { memcpy(best[1], cand, sizeof cand); bestc[1] = c; }
    }
    size_t o = 0;
    for (int i = 0; i < 2 && bestc[i]; i++) {
        char tmp[80];
        if (bestc[i] == 3) shorten_path(best[i], tmp, sizeof tmp);
        else snprintf(tmp, sizeof tmp, "%.60s", best[i]);
        o += snprintf(out + o, cap - o, "%s\"%s\"", i ? " " : "", tmp);
        if (o >= cap) break;
    }
}

/* Name for a hot function: string hints in the exe, nearest export elsewhere. */
static void func_hint(uintptr_t fn, char *out, size_t cap)
{
    out[0] = 0;
    if (fn & NOPDATA_FLAG) { snprintf(out, cap, "(no unwind info)"); return; }
    if (fn >= g_exe_base && fn < g_exe_end) { string_hints(fn, out, cap); return; }
    const Mod *m = mod_find(fn);
    if (m) nearest_export(m, fn, out, cap);
}

/* Outermost exe function on a thread's stack (the real thread routine behind CRT start stubs):
 * highest return address into exe .text found in the top page(s) below StackBase. */
static int is_return_addr(uintptr_t r)
{
    if (r < g_text_lo + 8 || r >= g_text_hi) return 0;
    const uint8_t *p = (const uint8_t *)r;
    if (p[-5] == 0xE8) return 1;
    for (int k = 2; k <= 7; k++)
        if (p[-k] == 0xFF && ((p[-k + 1] >> 3) & 7) == 2) return 1;
    return 0;
}

/* handles lack SYNCHRONIZE, so ask for the exit code (STILL_ACTIVE = running) */
static int thread_alive(const Thr *t)
{
    DWORD code;
    return GetExitCodeThread(t->h, &code) && code == STILL_ACTIVE;
}

static uintptr_t stack_root(const Thr *t)
{
    /* the thread is not suspended and may exit (freeing TEB + stack) at any moment: copy both
     * with safe_read, never dereference them. thread_alive just skips the obvious dead ones. */
    static uintptr_t top[8192 / 8];
    NT_TIB tib;
    if (!t->teb || !thread_alive(t) || !safe_read(t->teb, &tib, sizeof tib)) return 0;
    uintptr_t base = (uintptr_t)tib.StackBase, limit = (uintptr_t)tib.StackLimit;
    if (!base || base <= limit) return 0;
    uintptr_t lo = base - sizeof top > limit ? base - sizeof top : limit;
    if (!safe_read(lo, top, base - lo)) { lo = base - 4096; if (lo < limit || !safe_read(lo, top, 4096)) return 0; }
    for (size_t i = (base - lo) / 8; i-- > 0;) {
        if (is_return_addr(top[i])) return func_of(top[i] - 1);
    }
    return 0;
}

/* ------------------------------------------------------------------ CPU sets (P/E cores) */

static BYTE g_eff[1024]; static int g_eff_min = 255, g_eff_max = -1, g_ncpu, g_np, g_ne;

static void init_cpu_sets(void)
{
    g_ncpu = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (!pGetSystemCpuSetInformation) return;
    ULONG len = 0;
    pGetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
    if (!len) return;
    uint8_t *buf = malloc(len);
    if (!buf) return;
    if (pGetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf, len, &len, GetCurrentProcess(), 0)) {
        for (ULONG o = 0; o < len;) {
            SYSTEM_CPU_SET_INFORMATION *i = (SYSTEM_CPU_SET_INFORMATION *)(buf + o);
            if (!i->Size) break;
            if (i->Type == CpuSetInformation) {
                int idx = i->CpuSet.Group * 64 + i->CpuSet.LogicalProcessorIndex;
                int e = i->CpuSet.EfficiencyClass;
                if (idx < 1024) g_eff[idx] = (BYTE)e;
                if (e < g_eff_min) g_eff_min = e;
                if (e > g_eff_max) g_eff_max = e;
            }
            o += i->Size;
        }
        if (g_eff_max > g_eff_min)
            for (int c = 0; c < g_ncpu && c < 1024; c++) { if (g_eff[c] == g_eff_max) g_np++; else g_ne++; }
    }
    free(buf);
}

static const char *core_class(int idx)
{
    if (g_eff_max <= g_eff_min || idx < 0 || idx >= 1024) return "-";
    return g_eff[idx] == g_eff_max ? "P" : "E";
}

/* ------------------------------------------------------------------ DXGI */

static const GUID my_IID_IDXGIFactory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
static const GUID my_IID_IDXGIAdapter3 = {0x645967a4, 0x1392, 0x4310, {0xa7, 0x98, 0x80, 0x53, 0xce, 0x3e, 0x93, 0xfd}};
static IDXGIAdapter3 *g_gpu[8]; static char g_gpu_name[8][128]; static SIZE_T g_gpu_ded[8];
static int g_ngpu = -1;   /* -1 = not initialised */
static char g_gpu_err[96];

static void init_dxgi(void)
{
    g_ngpu = 0;
    HMODULE d = LoadLibraryA("dxgi.dll");
    CreateDXGIFactory1_t create = d ? (CreateDXGIFactory1_t)(void *)GetProcAddress(d, "CreateDXGIFactory1") : NULL;
    if (!create) { snprintf(g_gpu_err, sizeof g_gpu_err, "dxgi.dll / CreateDXGIFactory1 not available"); return; }
    IDXGIFactory1 *f = NULL;
    HRESULT hr = create(&my_IID_IDXGIFactory1, (void **)&f);
    if (FAILED(hr) || !f) { snprintf(g_gpu_err, sizeof g_gpu_err, "CreateDXGIFactory1 failed 0x%08lX", (unsigned long)hr); return; }
    IDXGIAdapter1 *a;
    for (UINT i = 0; g_ngpu < 8 && IDXGIFactory1_EnumAdapters1(f, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        IDXGIAdapter3 *a3 = NULL;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(IDXGIAdapter1_QueryInterface(a, &my_IID_IDXGIAdapter3, (void **)&a3)) && a3) {
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, g_gpu_name[g_ngpu], 128, NULL, NULL);
            g_gpu_ded[g_ngpu] = desc.DedicatedVideoMemory;
            g_gpu[g_ngpu++] = a3;
        }
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
    if (!g_ngpu) snprintf(g_gpu_err, sizeof g_gpu_err, "no hardware adapter with IDXGIAdapter3");
}

/* ------------------------------------------------------------------ report helpers */

static const char *fmt_bytes(double b, char *out)
{
    double a = b < 0 ? -b : b;
    if (a >= 1024.0 * 1024 * 1024) sprintf(out, "%.2f GB", b / (1024.0 * 1024 * 1024));
    else if (a >= 1024.0 * 1024) sprintf(out, "%.1f MB", b / (1024.0 * 1024));
    else sprintf(out, "%.0f KB", b / 1024.0);
    return out;
}
#define B(x) fmt_bytes((double)(x), (char[24]){0})

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}
static int cmp_hot_self(const void *a, const void *b)
{
    const Hot *x = a, *y = b;
    return x->self != y->self ? (x->self < y->self ? 1 : -1) : (x->incl < y->incl) - (x->incl > y->incl);
}
static int cmp_hot_incl(const void *a, const void *b)
{
    const Hot *x = a, *y = b;
    return (x->incl < y->incl) - (x->incl > y->incl);
}

typedef struct { int idx; double pct; } ThrRow;
static int cmp_thr(const void *a, const void *b)
{
    double x = ((const ThrRow *)a)->pct, y = ((const ThrRow *)b)->pct;
    return (x < y) - (x > y);
}

static void thread_name(const Thr *t, char *out, size_t cap)
{
    out[0] = 0;
    if (pGetThreadDescription) {
        PWSTR w = NULL;
        if (SUCCEEDED(pGetThreadDescription(t->h, &w)) && w) {
            WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
            LocalFree(w);
        }
    }
    if (!out[0]) { const char *v = veh_name(t->tid); if (v) snprintf(out, cap, "%s", v); }
    if (!out[0] && t->tid == g_main_tid) snprintf(out, cap, "(main)");
    if (!out[0] && t->tid == g_self_tid) snprintf(out, cap, "v3x_devtools");
}

/* ------------------------------------------------------------------ report sections */

static void report_frames(FILE *f, double wall)
{
    EnterCriticalSection(&g_fb_lock);
    FrameBuf *b = &g_fb[g_fb_cur];
    g_fb_cur ^= 1;
    FrameBuf *nb = &g_fb[g_fb_cur];
    nb->n = 0; nb->count = 0; nb->sum = nb->min = nb->max = 0; nb->over33 = nb->over100 = 0;
    LeaveCriticalSection(&g_fb_lock);
    if (!b->count) { fprintf(f, "[frames] no frames this interval (on_frame not called)\n"); return; }
    qsort(b->ms, b->n, sizeof(float), cmp_float);
    float p50 = b->ms[b->n / 2], p99 = b->ms[(int)((b->n - 1) * 0.99)];
    fprintf(f, "[frames] %lld frames in %.1f s = %.1f fps | ms: min %.2f avg %.2f p50 %.2f p99 %.2f max %.2f | "
               "hitches >33ms %d, >100ms %d\n",
            b->count, wall, b->count / wall, b->min, b->sum / b->count, p50, p99, b->max, b->over33, b->over100);
}

static ULONG64 g_proc_cpu_rep, g_self_cpu_rep;

static void report_threads(FILE *f, double wall)
{
    static ThrRow rows[MAX_THR];
    FILETIME c, e, k, u;
    ULONG64 pc = GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) ? ft64(k) + ft64(u) : 0;
    double proc_pct = (double)(pc - g_proc_cpu_rep) / 1e5 / wall;
    g_proc_cpu_rep = pc;
    int n = 0;
    for (int i = 0; i < g_nthr; i++) {
        ULONG64 cpu = thread_cpu(g_thr[i].h);
        rows[n].idx = i;
        rows[n].pct = (double)(cpu - g_thr[i].cpu_rep) / 1e5 / wall;
        g_thr[i].cpu_rep = cpu;
        n++;
    }
    qsort(rows, n, sizeof rows[0], cmp_thr);
    fprintf(f, "[threads] %d threads, process CPU %.1f%% of one core (%d logical CPUs", n, proc_pct, g_ncpu);
    if (g_np) fprintf(f, ", hybrid %dP+%dE logical", g_np, g_ne);
    fprintf(f, ")\n  %-6s %6s %4s %6s %4s  %-24s %-28s %s\n", "tid", "cpu%", "prio", "ideal", "core", "name",
            "start", "stack root (outermost exe fn) / hint");
    for (int r = 0; r < n && r < 24; r++) {
        Thr *t = &g_thr[rows[r].idx];
        if (r >= 8 && rows[r].pct < 0.5) break;
        char name[64], start[96], root[96] = "", hint[200] = "", ideal[16] = "?";
        const char *cls = "-";
        thread_name(t, name, sizeof name);
        fmt_addr(t->start, start, sizeof start);
        PROCESSOR_NUMBER pn;
        if (pGetThreadIdealProcessorEx && pGetThreadIdealProcessorEx(t->h, &pn)) {
            snprintf(ideal, sizeof ideal, "%u:%u", pn.Group, pn.Number);
            cls = core_class(pn.Group * 64 + pn.Number);
        }
        uintptr_t rt = stack_root(t);
        if (rt && !(rt & NOPDATA_FLAG)) { fmt_addr(rt, root, sizeof root); string_hints(rt, hint, sizeof hint); }
        fprintf(f, "  %-6lu %6.1f %4d %6s %4s  %-24.24s %-28.28s %s %s\n", t->tid, rows[r].pct,
                GetThreadPriority(t->h), ideal, cls, name, start, root, hint);
    }
}

static void report_profiler(FILE *f, double wall)
{
    static Hot rows[HOT_CAP];
    ULONG64 sc = thread_cpu(GetCurrentThread());
    double self_pct = (double)(sc - g_self_cpu_rep) / 1e5 / wall;
    g_self_cpu_rep = sc;
    if (!g_profiling && !g_hot_iv.samples) {
        fprintf(f, "[profiler] OFF (enable: profile=1 in v3x/devtools.ini, env V3X_PROFILE=1, or Ctrl+hotkey) | "
                   "devtools thread CPU %.2f%% of one core\n", self_pct);
        return;
    }
    fprintf(f, "[profiler] %s %d Hz, depth %d, %d targets; %llu samples (idle-skipped %llu, failed %llu)\n"
               "  overhead: avg stall %.1f us/sample = %.2f%% of each sampled thread's time; devtools thread CPU "
               "%.2f%% of one core\n",
            g_profiling ? "ON" : "OFF(stopped)", cfg.hz, cfg.depth, g_ntargets, (unsigned long long)g_ps.taken,
            (unsigned long long)g_ps.idle_skipped, (unsigned long long)g_ps.failed,
            g_ps.taken ? qpc_sec(g_ps.stall) * 1e6 / g_ps.taken : 0.0,
            g_ntargets ? qpc_sec(g_ps.stall) * 100 / wall / g_ntargets : 0.0, self_pct);
    fprintf(f, "  samples per thread:");
    for (int i = 0; i < g_nthr; i++)
        if (g_thr[i].samples) {
            char nm[64]; thread_name(&g_thr[i], nm, sizeof nm);
            fprintf(f, " %lu(%s)=%u", g_thr[i].tid, nm, g_thr[i].samples);
            g_thr[i].samples = 0;
        }
    fprintf(f, "\n");
    int n = 0;
    for (unsigned i = 0; i < HOT_CAP; i++) if (g_hot_iv.e[i].key) rows[n++] = g_hot_iv.e[i];
    double tot = g_hot_iv.samples ? (double)g_hot_iv.samples : 1;
    qsort(rows, n, sizeof(Hot), cmp_hot_self);
    fprintf(f, "  hottest functions (self time):\n  %4s %7s %6s %6s  %-34s %s\n", "#", "samples", "self%", "incl%",
            "function", "hint");
    for (int r = 0; r < n && r < 25 && rows[r].self; r++) {
        char a[96], h[200];
        fmt_addr(rows[r].key & ~NOPDATA_FLAG, a, sizeof a);
        func_hint(rows[r].key, h, sizeof h);
        fprintf(f, "  %4d %7u %6.2f %6.2f  %-34s %s\n", r + 1, rows[r].self, rows[r].self * 100 / tot,
                rows[r].incl * 100 / tot, a, h);
    }
    if (cfg.depth > 1) {
        qsort(rows, n, sizeof(Hot), cmp_hot_incl);
        fprintf(f, "  top inclusive (function or its callees on the stack, %d frames):\n", cfg.depth);
        for (int r = 0; r < n && r < 15; r++) {
            char a[96], h[200];
            fmt_addr(rows[r].key & ~NOPDATA_FLAG, a, sizeof a);
            func_hint(rows[r].key, h, sizeof h);
            fprintf(f, "  %4d %7u %6.2f %6.2f  %-34s %s\n", r + 1, rows[r].incl, rows[r].self * 100 / tot,
                    rows[r].incl * 100 / tot, a, h);
        }
    }
    if (g_hot_iv.dropped) fprintf(f, "  (%llu samples dropped: table full)\n", (unsigned long long)g_hot_iv.dropped);
    memset(&g_hot_iv, 0, sizeof g_hot_iv);
    memset(&g_ps, 0, sizeof g_ps);
}

/* cumulative table -> csv (rva,samples,inclusive,module). always_write: also write an empty table. */
static void write_csv_to(const char *path, int always_write)
{
    static Hot rows[HOT_CAP];
    int n = 0;
    for (unsigned i = 0; i < HOT_CAP; i++) if (g_hot_all.e[i].key) rows[n++] = g_hot_all.e[i];
    if (!n && !always_write) return;
    qsort(rows, n, sizeof(Hot), cmp_hot_self);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "rva,samples,inclusive,module\n");
    for (int r = 0; r < n; r++) {
        uintptr_t a = rows[r].key & ~NOPDATA_FLAG;
        const Mod *m = mod_find(a);
        if (m) fprintf(f, "0x%llX,%u,%u,%s\n", (unsigned long long)(a - m->base), rows[r].self, rows[r].incl, m->name);
    }
    fclose(f);
}

static void write_csv(void) { write_csv_to(g_csv_path, 0); }

static int cmp_edge(const void *a, const void *b)
{
    uint32_t x = ((const Edge *)a)->samples, y = ((const Edge *)b)->samples;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* edge table -> csv (caller_rva,callee_rva,samples,caller_module,callee_module), most samples first */
static void write_edges_to(const char *path, int always_write)
{
    static Edge rows[EDGE_CAP];
    int n = 0;
    for (unsigned i = 0; i < EDGE_CAP; i++) if (g_edges.e[i].caller) rows[n++] = g_edges.e[i];
    if (!n && !always_write) return;
    qsort(rows, n, sizeof(Edge), cmp_edge);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "caller_rva,callee_rva,samples,caller_module,callee_module\n");
    for (int r = 0; r < n; r++) {
        uintptr_t a = rows[r].caller & ~NOPDATA_FLAG, b = rows[r].callee & ~NOPDATA_FLAG;
        const Mod *ma = mod_find(a), *mb = mod_find(b);
        if (ma && mb) fprintf(f, "0x%llX,0x%llX,%u,%s,%s\n", (unsigned long long)(a - ma->base),
                              (unsigned long long)(b - mb->base), rows[r].samples, ma->name, mb->name);
    }
    if (g_edges.dropped) fprintf(f, "# %llu edge inserts dropped (table full)\n", (unsigned long long)g_edges.dropped);
    fclose(f);
}

typedef struct { uintptr_t base; SIZE_T commit, reserve; DWORD type; } Alloc;
static double g_last_private;
static DWORD g_last_faults;

static void report_memory(FILE *f)
{
    PROCESS_MEMORY_COUNTERS_EX pm; pm.cb = sizeof pm;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm)) {
        double d = g_last_private ? (double)pm.PrivateUsage - g_last_private : 0;
        fprintf(f, "[memory] private %s (%s%s since last report), working set %s (peak %s), page faults +%lu\n",
                B(pm.PrivateUsage), d >= 0 ? "+" : "", B(d), B(pm.WorkingSetSize), B(pm.PeakWorkingSetSize),
                pm.PageFaultCount - g_last_faults);
        g_last_private = (double)pm.PrivateUsage; g_last_faults = pm.PageFaultCount;
    }
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms))
        fprintf(f, "  system: RAM free %s of %s (load %lu%%), commit available %s of %s limit\n", B(ms.ullAvailPhys),
                B(ms.ullTotalPhys), ms.dwMemoryLoad, B(ms.ullAvailPageFile), B(ms.ullTotalPageFile));

    /* address-space walk, grouped by allocation (one VirtualAlloc / mapping / image) */
    Alloc top[10]; int ntop = 0;
    SIZE_T c_priv = 0, c_map = 0, c_img = 0, reserved = 0; int nalloc = 0;
    Alloc cur = {0};
    uintptr_t a = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (;;) {
        int more = VirtualQuery((void *)a, &mbi, sizeof mbi) != 0;
        uintptr_t ab = more ? (uintptr_t)mbi.AllocationBase : 0;
        if ((!more || mbi.State == MEM_FREE || ab != cur.base) && cur.base) {
            nalloc++;
            int pos = ntop;
            while (pos > 0 && top[pos - 1].commit < cur.commit) pos--;
            if (pos < 10) {
                if (ntop < 10) ntop++;
                memmove(&top[pos + 1], &top[pos], (ntop - 1 - pos) * sizeof(Alloc));
                top[pos] = cur;
            }
            cur.base = 0;
        }
        if (!more) break;
        if (mbi.State != MEM_FREE) {
            if (!cur.base) { cur.base = ab; cur.commit = cur.reserve = 0; cur.type = mbi.Type; }
            cur.reserve += mbi.RegionSize;
            if (mbi.State == MEM_COMMIT) {
                cur.commit += mbi.RegionSize;
                if (mbi.Type == MEM_PRIVATE) c_priv += mbi.RegionSize;
                else if (mbi.Type == MEM_MAPPED) c_map += mbi.RegionSize;
                else if (mbi.Type == MEM_IMAGE) c_img += mbi.RegionSize;
            } else reserved += mbi.RegionSize;
        }
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= a) break;
        a = next;
    }
    fprintf(f, "  address space: committed private %s, mapped %s, image %s; reserved-only %s; %d allocations\n",
            B(c_priv), B(c_map), B(c_img), B(reserved), nalloc);
    fprintf(f, "  largest committed allocations:\n    %-18s %10s %10s %-7s %s\n", "base", "committed", "reserved",
            "type", "owner");
    for (int i = 0; i < ntop; i++) {
        const Mod *m = top[i].type == MEM_IMAGE ? mod_find(top[i].base) : NULL;
        fprintf(f, "    0x%016llX %10s %10s %-7s %s\n", (unsigned long long)top[i].base, B(top[i].commit),
                B(top[i].reserve),
                top[i].type == MEM_PRIVATE ? "private" : top[i].type == MEM_MAPPED ? "mapped" : "image",
                m ? m->name : "");
    }

    /* heaps */
    HANDLE heaps[256];
    DWORD nh = GetProcessHeaps(256, heaps);
    HANDLE def = GetProcessHeap();
    if (nh > 256) nh = 256;
    if (!pHeapSummary) { fprintf(f, "  heaps: %lu (HeapSummary not available)\n", nh); return; }
    struct { HANDLE h; SIZE_T commit, alloc; } hs[256];
    SIZE_T tc = 0, ta = 0; int nhs = 0;
    for (DWORD i = 0; i < nh; i++) {
        HEAP_SUMMARY s; memset(&s, 0, sizeof s); s.cb = sizeof s;
        if (!pHeapSummary(heaps[i], 0, &s)) continue;
        hs[nhs].h = heaps[i]; hs[nhs].commit = s.cbCommitted; hs[nhs].alloc = s.cbAllocated; nhs++;
        tc += s.cbCommitted; ta += s.cbAllocated;
    }
    fprintf(f, "  heaps: %lu Win32 heaps, committed %s, allocated %s (engine allocators on VirtualAlloc show as private above)\n",
            nh, B(tc), B(ta));
    for (int k = 0; k < 5 && k < nhs; k++) {
        int best = k;
        for (int j = k + 1; j < nhs; j++) if (hs[j].commit > hs[best].commit) best = j;
        if (best != k) { __typeof__(hs[0]) tmp = hs[k]; hs[k] = hs[best]; hs[best] = tmp; }
        fprintf(f, "    heap %p committed %10s allocated %10s%s\n", hs[k].h, B(hs[k].commit), B(hs[k].alloc),
                hs[k].h == def ? "  (process default / CRT)" : "");
    }
}

static void report_gpu(FILE *f)
{
    if (g_ngpu < 0) init_dxgi();
    if (!g_ngpu) { fprintf(f, "[gpu] unavailable: %s\n", g_gpu_err); return; }
    for (int i = 0; i < g_ngpu; i++) {
        DXGI_QUERY_VIDEO_MEMORY_INFO l, nl;
        memset(&l, 0, sizeof l); memset(&nl, 0, sizeof nl);
        HRESULT h1 = IDXGIAdapter3_QueryVideoMemoryInfo(g_gpu[i], 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &l);
        HRESULT h2 = IDXGIAdapter3_QueryVideoMemoryInfo(g_gpu[i], 0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nl);
        fprintf(f, "[gpu] adapter %d \"%s\" (dedicated %s)\n", i, g_gpu_name[i], B(g_gpu_ded[i]));
        if (SUCCEEDED(h1))
            fprintf(f, "  local (VRAM):        process usage %s, budget %s, reserved %s\n", B(l.CurrentUsage),
                    B(l.Budget), B(l.CurrentReservation));
        if (SUCCEEDED(h2))
            fprintf(f, "  non-local (shared):  process usage %s, budget %s, reserved %s\n", B(nl.CurrentUsage),
                    B(nl.Budget), B(nl.CurrentReservation));
    }
}

static int g_report_no;
static LONGLONG g_last_report;

static void write_report(const char *trigger)
{
    LONGLONG now = qpc_now();
    double wall = qpc_sec(now - g_last_report);
    if (wall < 0.001) wall = 0.001;
    g_last_report = now;
    refresh_modules();
    refresh_threads();
    /* keep the log bounded */
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA(g_log_path, GetFileExInfoStandard, &fa) && (fa.nFileSizeLow > 32u << 20 || fa.nFileSizeHigh)) {
        char old[MAX_PATH + 8]; snprintf(old, sizeof old, "%s.old", g_log_path);
        MoveFileExA(g_log_path, old, MOVEFILE_REPLACE_EXISTING);
    }
    FILE *f = fopen(g_log_path, "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "\n===== devtools report #%d at %02d:%02d:%02d (%s, interval %.1f s) =====\n", ++g_report_no,
            st.wHour, st.wMinute, st.wSecond, trigger, wall);
    report_frames(f, wall);
    report_threads(f, wall);
    report_profiler(f, wall);
    report_memory(f);
    report_gpu(f);
    fclose(f);
    write_csv();
    write_edges_to(g_edges_path, 0);
}

/* ------------------------------------------------------------------ worker thread */

static void log_line(const char *fmt, const char *arg)
{
    FILE *f = fopen(g_log_path, "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    fprintf(f, fmt, arg);
    fputc('\n', f);
    fclose(f);
}

/* ---- per-phase snapshots (API v3 "load_transition"): the event arrives on the main thread,
 * the devtools thread (owner of the tables) writes the CSVs and resets the cumulative tables. */
/* "load_transition_begin" (user = 1) snapshots as profile_<from>_before_<to>_<n>.csv, so the
 * following "load_transition" snapshot covers exactly the transition itself (e.g. teardown). */
typedef struct { char from[32], to[32]; double seconds; const char *sep; } Phase;
static Phase g_phq[16];
static unsigned g_phq_head, g_phq_tail;     /* guarded by g_phq_lock */
static CRITICAL_SECTION g_phq_lock;
static int g_snap_no;

static void phase_name(char *out, size_t cap, const char *s)
{
    size_t o = 0;
    if (!s || !*s) s = "none";
    for (; *s && o + 1 < cap; s++)
        out[o++] = (isalnum((unsigned char)*s) || *s == '-') ? *s : '_';
    out[o] = 0;
}

/* "process_exit" (core, the thread calling ExitProcess, all threads alive): stop sampling and wait
 * (bounded) until the sampler has resumed whatever it suspended. */
static void on_process_exit(const char *name, void *data, void *user)
{
    (void)name; (void)data; (void)user;
    InterlockedExchange(&g_exit_seen, 1);
    InterlockedExchange(&g_profiling, 0);
    for (int i = 0; g_in_suspend && i < 2000; i++) { if (i < 64) YieldProcessor(); else Sleep(1); }
}

static void on_load_transition(const char *name, void *data, void *user)
{
    (void)name;
    const V3X_LoadTransition *lt = data;
    Phase ph;
    ph.sep = user ? "before" : "to";
    phase_name(ph.from, sizeof ph.from, lt ? lt->from : NULL);
    phase_name(ph.to, sizeof ph.to, lt ? lt->to : NULL);
    ph.seconds = lt ? lt->seconds : 0;
    EnterCriticalSection(&g_phq_lock);
    if (g_phq_head - g_phq_tail < 16) g_phq[g_phq_head++ % 16] = ph;
    LeaveCriticalSection(&g_phq_lock);
    SetEvent(g_wake);
}

static void drain_phases(void)
{
    for (;;) {
        Phase ph;
        EnterCriticalSection(&g_phq_lock);
        int have = g_phq_tail != g_phq_head;
        if (have) ph = g_phq[g_phq_tail++ % 16];
        LeaveCriticalSection(&g_phq_lock);
        if (!have) return;
        char p[MAX_PATH + 96], e[MAX_PATH + 96], msg[200];
        int n = ++g_snap_no;
        snprintf(p, sizeof p, "%sprofile_%s_%s_%s_%d.csv", api->v3x_dir(), ph.from, ph.sep, ph.to, n);
        snprintf(e, sizeof e, "%sprofile_edges_%s_%s_%s_%d.csv", api->v3x_dir(), ph.from, ph.sep, ph.to, n);
        write_csv_to(p, 1);
        write_edges_to(e, 1);
        write_edges_to(g_edges_path, 1);
        snprintf(msg, sizeof msg, "phase %s %s %s (%.2f s): %llu samples -> profile_%s_%s_%s_%d.csv",
                 ph.from, *ph.sep == 'b' ? "begin ->" : "->", ph.to, ph.seconds,
                 (unsigned long long)g_hot_all.samples, ph.from, ph.sep, ph.to, n);
        log_line("%s", msg);
        memset(&g_hot_all, 0, sizeof g_hot_all);
        memset(&g_edges, 0, sizeof g_edges);
    }
}

static DWORD WINAPI devtools_thread(void *arg)
{
    (void)arg;
    g_self_tid = GetCurrentThreadId();
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    int hires = timer != NULL;
    if (!timer) {
        timer = CreateWaitableTimerW(NULL, FALSE, NULL);
        HMODULE w = LoadLibraryA("winmm.dll");
        timeBeginPeriod_t tbp = w ? (timeBeginPeriod_t)(void *)GetProcAddress(w, "timeBeginPeriod") : NULL;
        if (tbp) tbp(1);
    }
    /* time-critical so samples land on schedule; the thread sleeps between ticks */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    refresh_modules();
    refresh_threads();
    g_last_report = qpc_now();
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) g_proc_cpu_rep = ft64(k) + ft64(u);
    g_self_cpu_rep = thread_cpu(GetCurrentThread());
    log_line("sampler timer: %s", hires ? "high-resolution waitable timer" : "waitable timer + timeBeginPeriod(1)");
    LONGLONG period = (LONGLONG)(g_qpf.QuadPart * (double)cfg.interval_sec);
    LONGLONG next_report = g_last_report + period, next_refresh = g_last_report + g_qpf.QuadPart;
    int was_profiling = g_profiling;
    LONGLONG tick = g_qpf.QuadPart / cfg.hz, next_tick = qpc_now() + tick;
    for (;;) {
        DWORD wr = WAIT_TIMEOUT;
        if (g_profiling) {
            /* deadline-based: timer wake-ups overshoot, so aim each wait at the next absolute tick */
            LONGLONG now = qpc_now();
            if (now - next_tick > 4 * tick) next_tick = now;   /* fell far behind (report, stall): resync */
            LONGLONG left = next_tick - now;
            next_tick += tick;
            if (left > 0) {
                LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(qpc_sec(left) * 1e7);
                if (!due.QuadPart) due.QuadPart = -1;
                SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
                HANDLE hs[2] = {timer, g_wake};
                wr = WaitForMultipleObjects(2, hs, FALSE, 100);
            }
        } else {
            LONGLONG left = next_report - qpc_now();
            DWORD ms = left > 0 ? (DWORD)(qpc_sec(left) * 1000) + 1 : 0;
            wr = WaitForSingleObject(g_wake, ms);
        }
        (void)wr;
        LONG req = InterlockedExchange(&g_req_report, 0);
        drain_phases();
        if (req == REQ_TOGGLE) InterlockedExchange(&g_profiling, !g_profiling);
        else if (req == REQ_ON) InterlockedExchange(&g_profiling, 1);
        else if (req == REQ_OFF) InterlockedExchange(&g_profiling, 0);
        else if (req == REQ_RESET) {
            /* tables are only touched on this thread, so clearing here needs no lock */
            memset(&g_hot_iv, 0, sizeof g_hot_iv);
            memset(&g_hot_all, 0, sizeof g_hot_all);
            memset(&g_edges, 0, sizeof g_edges);
            memset(&g_ps, 0, sizeof g_ps);
            log_line("profiler %s", "samples reset");
        }
        if (g_profiling != was_profiling) {
            was_profiling = g_profiling;
            log_line("profiler %s", g_profiling ? "ON" : "OFF");
            if (g_profiling) { refresh_threads(); next_refresh = qpc_now() + g_qpf.QuadPart; }
        }
        if (g_profiling) sample_tick();
        LONGLONG now = qpc_now();
        if (g_profiling && now >= next_refresh) { refresh_threads(); next_refresh = now + g_qpf.QuadPart; }
        if (req == REQ_REPORT || now >= next_report) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);   /* report I/O must not preempt the game */
            write_report(req == REQ_REPORT ? "request" : "timer");
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            next_report = qpc_now() + period;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ console command (API v2) */

/* "profile on|off|report|reset": runs on the console input thread, so it only posts a request
 * to the devtools thread (which owns all profiler state) and returns. */
static void cmd_profile(const char *args, void *user)
{
    (void)user;
    static const struct { const char *name; LONG req; const char *msg; } verbs[] = {
        {"on", REQ_ON, "profiler ON requested"},
        {"off", REQ_OFF, "profiler OFF requested"},
        {"report", REQ_REPORT, "report requested (written to %s)"},
        {"reset", REQ_RESET, "profiler samples reset requested"},
    };
    char verb[16] = {0};
    sscanf(args, "%15s", verb);
    for (size_t i = 0; i < sizeof verbs / sizeof verbs[0]; i++) {
        if (_stricmp(verb, verbs[i].name)) continue;
        InterlockedExchange(&g_req_report, verbs[i].req);
        SetEvent(g_wake);
        api->print(verbs[i].msg, g_log_path);
        return;
    }
    api->print("usage: profile on|off|report|reset  (profiler is %s, log %s)", g_profiling ? "ON" : "off", g_log_path);
}

/* ------------------------------------------------------------------ load */

static void read_exe_layout(void)
{
    uint8_t *b = (uint8_t *)api->image_base();
    g_exe_base = (uintptr_t)b; g_exe_end = g_exe_base + api->image_size();
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        uintptr_t lo = g_exe_base + s->VirtualAddress, hi = lo + s->Misc.VirtualSize;
        if (!memcmp(s->Name, ".text", 5)) { g_text_lo = lo; g_text_hi = hi; }
        else if ((!memcmp(s->Name, ".rdata", 6) || !memcmp(s->Name, ".data", 5)) && g_ndata < 8)
            { g_data[g_ndata].lo = lo; g_data[g_ndata].hi = hi; g_ndata++; }
    }
    char p[MAX_PATH]; GetModuleFileNameA(NULL, p, MAX_PATH);
    const char *bs = strrchr(p, '\\');
    snprintf(g_exe_name, sizeof g_exe_name, "%s", bs ? bs + 1 : p);
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    /* only needs the v1 fields (through v3x_dir), so newer/longer API tables are fine */
    if (a->api_version < 1 || a->struct_size < offsetof(V3X_API, v3x_dir) + sizeof(a->v3x_dir)) return 1;
    QueryPerformanceFrequency(&g_qpf);
    g_pid = GetCurrentProcessId();
    g_main_tid = GetCurrentThreadId();
    load_config();
    read_exe_layout();
    init_fixed_modules();
    snprintf(g_log_path, sizeof g_log_path, "%sdevtools.log", api->v3x_dir());
    snprintf(g_csv_path, sizeof g_csv_path, "%sprofile_latest.csv", api->v3x_dir());
    snprintf(g_edges_path, sizeof g_edges_path, "%sprofile_edges.csv", api->v3x_dir());
    InitializeCriticalSection(&g_phq_lock);

    HMODULE k32 = GetModuleHandleA("kernel32.dll"), nt = GetModuleHandleA("ntdll.dll");
    HMODULE kb = GetModuleHandleA("kernelbase.dll"), u32 = GetModuleHandleA("user32.dll");
    if (!u32) u32 = LoadLibraryA("user32.dll");
#define RESOLVE(var, mod, name) var = (__typeof__(var))(void *)GetProcAddress(mod, name)
    RESOLVE(pGetThreadDescription, k32, "GetThreadDescription");
    if (!pGetThreadDescription && kb) RESOLVE(pGetThreadDescription, kb, "GetThreadDescription");
    RESOLVE(pNtQueryInformationThread, nt, "NtQueryInformationThread");
    RESOLVE(pNtGetNextThread, nt, "NtGetNextThread");
    RESOLVE(pGetThreadIdealProcessorEx, k32, "GetThreadIdealProcessorEx");
    RESOLVE(pGetSystemCpuSetInformation, k32, "GetSystemCpuSetInformation");
    RESOLVE(pHeapSummary, k32, "HeapSummary");
    if (u32) RESOLVE(pGetAsyncKeyState, u32, "GetAsyncKeyState");
    init_cpu_sets();

    AddVectoredExceptionHandler(1, veh_thread_name);
    InitializeCriticalSection(&g_fb_lock);
    g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_stack = VirtualAlloc(NULL, STACK_COPY + STACK_SLACK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_wake || !g_stack) return 2;
    g_profiling = cfg.profile != 0;

    FILE *f = fopen(g_log_path, "a");
    if (f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "\n##### devtools session %04d-%02d-%02d %02d:%02d:%02d | %s pid %lu | game %s | profile=%d "
                   "interval_sec=%d hotkey=0x%X sample_hz=%d sample_threads=%d unwind_depth=%d\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, g_exe_name, g_pid,
                api->game_version(), cfg.profile, cfg.interval_sec, cfg.hotkey, cfg.hz, cfg.top_threads, cfg.depth);
        fclose(f);
    }
    g_self_thread = CreateThread(NULL, 256 * 1024, devtools_thread, NULL, 0, NULL);
    if (!g_self_thread) return 3;
    {
        typedef HRESULT(WINAPI * SetThreadDescription_t)(HANDLE, PCWSTR);
        SetThreadDescription_t sd;
        RESOLVE(sd, k32, "SetThreadDescription");
        if (sd) sd(g_self_thread, L"v3x_devtools");
    }
    api->log(TAG, "profiler %s, report every %d s to %s, hotkey 0x%X (Ctrl+hotkey toggles profiler)",
             g_profiling ? "ON" : "off", cfg.interval_sec, g_log_path, cfg.hotkey);
    if (a->api_version >= 2 && a->struct_size >= offsetof(V3X_API, print) + sizeof(a->print) && a->register_command) {
        int rc = api->register_command("profile", "profile on|off|report|reset - devtools sampling profiler", cmd_profile, NULL);
        if (rc) api->log(TAG, "register_command(profile) failed: %d", rc);
    }
    if (a->api_version >= 3 && a->struct_size >= offsetof(V3X_API, on_event) + sizeof(a->on_event) && a->on_event) {
        int rc = api->on_event("load_transition", on_load_transition, NULL);
        if (rc) api->log(TAG, "on_event(load_transition) failed: %d", rc);
        rc = api->on_event("load_transition_begin", on_load_transition, (void *)1);
        if (rc) api->log(TAG, "on_event(load_transition_begin) failed: %d", rc);
        rc = api->on_event("process_exit", on_process_exit, NULL);
        if (rc) api->log(TAG, "on_event(process_exit) failed: %d", rc);
    }
    return api->on_frame(on_frame, NULL);
}
