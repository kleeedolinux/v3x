/* crash_logger: our own crash / diagnostic log, because the game's crash reporter gives almost nothing.
 *
 * Writes <game>/binaries/v3x/crash.log (previous run kept as crash.prev.log). Every line goes straight
 * to WriteFile (no CRT buffering), so it survives the process dying right after.
 *
 *   - Vectored exception handler (first in chain, never swallows anything):
 *       * MSVC C++ throws (0xE06D7363): thrown type names decoded from ThrowInfo, what() for
 *         std::exception-derived types, stack. Rate limited (200 total, 3 per type+throw site).
 *       * Fatal SEH codes (access violation, stack overflow, heap corruption, ...): code, fault
 *         address, module+offset, registers, stack.
 *   - SetUnhandledExceptionFilter (chains to the previous one) and SIGABRT handler (std::terminate
 *     -> abort) that log, then hand over to whatever would have run anyway.
 *   - Memory sampler on api->on_frame: logs big changes in private bytes / available commit, and
 *     low available commit (< 1 GB).
 *
 * The exception path allocates no heap memory (static buffers, one global spin lock), because the
 * crashes we hunt are likely out-of-memory. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "v3x.h"

V3X_EXPORT const char *v3x_plugin_name = "crash_logger";
V3X_PLUGIN_MANIFEST("crash_logger", "0.5.0", "v3x", "crash and diagnostic log (crash.log)", "*");

#define MAX_FRAMES 48
#define MAX_THROWS_LOGGED 200
#define MAX_REPEATS_PER_SITE 3
#define SITE_SLOTS 512
#define MAX_FATAL_LOGGED 64
#define MB (1024ull * 1024ull)

static const V3X_API *api;
static HANDLE g_file = INVALID_HANDLE_VALUE;
static volatile LONG g_owner;              /* thread id holding the log lock, 0 = free */
static char g_line[2048];
static char g_modname[MAX_PATH];
static WCHAR g_wmod[MAX_PATH];
static CONTEXT g_walk_ctx;
static DWORD64 g_frames[MAX_FRAMES];
static int g_nframes;
static LONG g_throws_seen, g_throws_logged, g_throws_suppressed, g_fatal_logged;
static struct { unsigned long long key; LONG count; } g_sites[SITE_SLOTS];
static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter;
static void (__cdecl *g_prev_sigabrt)(int);

/* ------------------------------------------------------------------ lock + raw output */

/* Returns 1 if acquired. Fails on re-entry from the same thread (exception inside our own handler)
 * and after ~2 s of waiting (another thread died holding it). */
static int lock_acquire(void)
{
    LONG me = (LONG)GetCurrentThreadId();
    if (g_owner == me) return 0;
    for (int i = 0; i < 2000; i++) {
        if (InterlockedCompareExchange(&g_owner, me, 0) == 0) return 1;
        Sleep(1);
    }
    return 0;
}
static void lock_release(void) { InterlockedExchange(&g_owner, 0); }

/* One-line summary of the current event, mirrored to api->log (the V3X console) after the lock is
 * released. Full detail stays in crash.log. Never used on the stack-overflow path. */
static char g_summary[512];
static volatile LONG g_mirror_owner;       /* thread id currently inside api->log for a summary */

static void summary(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (vsnprintf(g_summary, sizeof g_summary, fmt, ap) < 0) g_summary[0] = 0;
    va_end(ap);
}

/* Caller holds the lock: copies the pending summary, releases the lock, then mirrors it. Skipped
 * when this thread is already inside a mirror call (exception inside api->log) or another thread is. */
static void release_and_mirror(void)
{
    char s[sizeof g_summary];
    memcpy(s, g_summary, sizeof s);
    g_summary[0] = 0;
    lock_release();
    if (!s[0] || !api) return;
    LONG me = (LONG)GetCurrentThreadId();
    if (g_mirror_owner == me || InterlockedCompareExchange(&g_mirror_owner, me, 0) != 0) return;
    api->log("crash_logger", "%s", s);
    InterlockedExchange(&g_mirror_owner, 0);
}

static void out_raw(const char *s, DWORD n)
{
    DWORD w;
    if (g_file != INVALID_HANDLE_VALUE && n) WriteFile(g_file, s, n, &w, NULL);
}
static void out_str(const char *s) { out_raw(s, (DWORD)strlen(s)); }

/* Caller holds the lock. Prefixes a timestamp and the thread id. */
static void clog(const char *fmt, ...)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = snprintf(g_line, sizeof g_line, "%02u:%02u:%02u.%03u [%5lu] ", t.wHour, t.wMinute, t.wSecond,
                     t.wMilliseconds, GetCurrentThreadId());
    if (n < 0) n = 0;
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(g_line + n, sizeof g_line - n - 2, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    n += m;
    if (n > (int)sizeof g_line - 2) n = (int)sizeof g_line - 2;
    g_line[n++] = '\n';
    out_raw(g_line, (DWORD)n);
}

/* ------------------------------------------------------------------ memory helpers */

static int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    const char *a = (const char *)p;
    if ((uintptr_t)a < 0x10000) return 0;
    while (n) {
        if (!VirtualQuery(a, &mbi, sizeof mbi)) return 0;
        if (mbi.State != MEM_COMMIT) return 0;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
        if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return 0;
        size_t left = (size_t)((const char *)mbi.BaseAddress + mbi.RegionSize - a);
        if (left >= n) return 1;
        a += left; n -= left;
    }
    return 1;
}

/* Copies a NUL-terminated string from untrusted memory, checking each page. */
static void safe_strcpy(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    dst[0] = 0;
    if (!src) return;
    while (i + 1 < cap) {
        if (i == 0 || (((uintptr_t)(src + i)) & 0xFFF) == 0)
            if (!readable(src + i, 1)) break;
        char c = src[i];
        if (!c) break;
        dst[i++] = (c >= 32 && c < 127) ? c : '?';
    }
    dst[i] = 0;
}

static unsigned long long g_snap_avail_mb, g_snap_priv_mb;   /* from the last mem_snapshot */

static void mem_snapshot(void)
{
    MEMORYSTATUSEX ms = {sizeof ms};
    PROCESS_MEMORY_COUNTERS_EX pc = {sizeof pc};
    GlobalMemoryStatusEx(&ms);
    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pc, sizeof pc);
    g_snap_avail_mb = ms.ullAvailPageFile / MB;
    g_snap_priv_mb = (unsigned long long)pc.PrivateUsage / MB;
    clog("  memory: private=%llu MB working_set=%llu MB | phys avail=%llu/%llu MB | commit avail=%llu MB "
         "of limit %llu MB (load %lu%%)",
         (unsigned long long)pc.PrivateUsage / MB, (unsigned long long)pc.WorkingSetSize / MB,
         ms.ullAvailPhys / MB, ms.ullTotalPhys / MB, ms.ullAvailPageFile / MB, ms.ullTotalPageFile / MB,
         ms.dwMemoryLoad);
}

/* "victoria3.exe+0x1234" into g_modname-based static output. Returns pointer to static buffer. */
static char g_symbuf[MAX_PATH + 32];
static const char *addr_str(DWORD64 a)
{
    HMODULE m = NULL;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)(uintptr_t)a, &m) && m) {
        DWORD n = GetModuleFileNameW(m, g_wmod, MAX_PATH);
        const WCHAR *b = g_wmod;
        for (DWORD i = 0; i < n; i++) if (g_wmod[i] == L'\\' || g_wmod[i] == L'/') b = g_wmod + i + 1;
        if (!WideCharToMultiByte(CP_UTF8, 0, b, -1, g_modname, sizeof g_modname, NULL, NULL))
            strcpy(g_modname, "?");
        snprintf(g_symbuf, sizeof g_symbuf, "%s+0x%llx", g_modname, (unsigned long long)(a - (uintptr_t)m));
    } else {
        g_modname[0] = 0;
        snprintf(g_symbuf, sizeof g_symbuf, "0x%llx", (unsigned long long)a);
    }
    return g_symbuf;
}

/* ------------------------------------------------------------------ stack walk */

static void walk_stack(const CONTEXT *start)
{
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    g_walk_ctx = *start;
    CONTEXT *c = &g_walk_ctx;
    g_nframes = 0;
    while (g_nframes < MAX_FRAMES && c->Rip) {
        g_frames[g_nframes++] = c->Rip;
        if (c->Rsp < lo || c->Rsp + 8 > hi) break;
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c->Rip, &base, NULL);
        if (!fe) {                                  /* leaf function: return address is at [rsp] */
            c->Rip = *(DWORD64 *)c->Rsp;
            c->Rsp += 8;
        } else {
            PVOID handler_data; DWORD64 establisher;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c->Rip, fe, c, &handler_data, &establisher, NULL);
        }
        if (c->Rip && !readable((void *)(uintptr_t)c->Rip, 1)) { g_frames[g_nframes++] = c->Rip; break; }
    }
}

static void log_stack(void)
{
    for (int i = 0; i < g_nframes; i++) clog("  #%02d %s", i, addr_str(g_frames[i]));
    if (g_nframes == MAX_FRAMES) clog("  ... (truncated at %d frames)", MAX_FRAMES);
}

static void log_regs(const CONTEXT *c)
{
    clog("  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx", c->Rax, c->Rbx, c->Rcx, c->Rdx);
    clog("  rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx", c->Rsi, c->Rdi, c->Rbp, c->Rsp);
    clog("  r8 =%016llx r9 =%016llx r10=%016llx r11=%016llx", c->R8, c->R9, c->R10, c->R11);
    clog("  r12=%016llx r13=%016llx r14=%016llx r15=%016llx", c->R12, c->R13, c->R14, c->R15);
    clog("  rip=%016llx eflags=%08lx", c->Rip, c->EFlags);
}

/* ------------------------------------------------------------------ MSVC C++ throw decoding */

#define CXX_MAGIC 0xE06D7363u
#define CXX_SIG1 0x19930520u
#define CXX_SIG2 0x19930521u
#define CXX_SIG3 0x19930522u

static char g_types[512];
static char g_what[512];

/* ".?AVruntime_error@std@@" -> "std::runtime_error". Leaves anything templated/odd untouched. */
static void demangle_type(const char *in, char *out, size_t cap)
{
    if (strncmp(in, ".?A", 3) || !in[3] || strpbrk(in + 4, "?$") || !strstr(in, "@@")) {
        snprintf(out, cap, "%s", in);
        return;
    }
    const char *s = in + 4, *end = strstr(s, "@@");
    const char *parts[16]; size_t lens[16]; int np = 0;
    while (s < end && np < 16) {
        const char *at = s; while (at < end && *at != '@') at++;
        parts[np] = s; lens[np] = (size_t)(at - s); np++;
        s = at + 1;
    }
    size_t o = 0;
    out[0] = 0;
    for (int i = np - 1; i >= 0; i--) {
        if (o + lens[i] + 3 >= cap) break;
        memcpy(out + o, parts[i], lens[i]); o += lens[i];
        if (i) { out[o++] = ':'; out[o++] = ':'; }
    }
    out[o] = 0;
}

/* Fills g_types ("std::runtime_error, std::exception") and g_what; returns a hash of the type list. */
static unsigned long long decode_cxx(const EXCEPTION_RECORD *er)
{
    unsigned long long h = 1469598103934665603ull;
    g_types[0] = 0; g_what[0] = 0;
    if (er->NumberParameters < 4) { strcpy(g_types, "?(no params)"); return h; }
    uintptr_t obj = er->ExceptionInformation[1];
    const uint8_t *ti = (const uint8_t *)er->ExceptionInformation[2];
    uintptr_t base = er->ExceptionInformation[3];
    if (!ti || !readable(ti, 16)) { strcpy(g_types, "?(no ThrowInfo)"); return h; }
    int32_t cta_rva = *(const int32_t *)(ti + 12);
    const uint8_t *cta = (const uint8_t *)(base + (uint32_t)cta_rva);
    if (!cta_rva || !readable(cta, 4)) { strcpy(g_types, "?(no CatchableTypeArray)"); return h; }
    int32_t n = *(const int32_t *)cta;
    if (n < 0 || n > 64 || !readable(cta + 4, (size_t)n * 4)) { strcpy(g_types, "?(bad CatchableTypeArray)"); return h; }
    int is_exception = 0;
    size_t o = 0;
    for (int i = 0; i < n; i++) {
        int32_t ct_rva = *(const int32_t *)(cta + 4 + i * 4);
        const uint8_t *ct = (const uint8_t *)(base + (uint32_t)ct_rva);
        if (!readable(ct, 8)) continue;
        int32_t td_rva = *(const int32_t *)(ct + 4);
        const char *td = (const char *)(base + (uint32_t)td_rva);
        if (!readable(td, 17)) continue;
        char raw[256], nice[256];
        safe_strcpy(raw, sizeof raw, td + 16);
        for (const char *p = raw; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
        if (!strcmp(raw, ".?AVexception@std@@") || strstr(raw, "exception")) is_exception = 1;
        demangle_type(raw, nice, sizeof nice);
        int w = snprintf(g_types + o, sizeof g_types - o, "%s%s", o ? ", " : "", nice);
        if (w < 0 || (size_t)w >= sizeof g_types - o) break;
        o += (size_t)w;
    }
    /* MSVC std::exception: { vptr; __std_exception_data { const char *_What; bool _DoFree; } } */
    if (is_exception && obj && readable((void *)obj, 16)) {
        const char *what = *(const char *const *)(obj + 8);
        if (what) safe_strcpy(g_what, sizeof g_what, what);
        else strcpy(g_what, "(null)");
    }
    return h;
}

/* Returns 1 if this (type, site) should be logged, counting it either way. */
static int site_should_log(unsigned long long key)
{
    unsigned idx = (unsigned)(key ^ (key >> 29)) % SITE_SLOTS;
    for (int probe = 0; probe < SITE_SLOTS; probe++, idx = (idx + 1) % SITE_SLOTS) {
        if (g_sites[idx].key == key) return ++g_sites[idx].count <= MAX_REPEATS_PER_SITE;
        if (g_sites[idx].key == 0) { g_sites[idx].key = key; g_sites[idx].count = 1; return 1; }
    }
    return 1; /* table full: just log (still bounded by MAX_THROWS_LOGGED) */
}

/* First walked frame outside the throw machinery (RaiseException / _CxxThrowException / CRT). */
static const char *throw_site_str(void)
{
    static const char *skip[] = {"KERNELBASE", "ntdll", "VCRUNTIME", "ucrtbase", "msvcp"};
    for (int i = 0; i < g_nframes; i++) {
        const char *s = addr_str(g_frames[i]);
        int internal = 0;
        for (size_t k = 0; k < sizeof skip / sizeof *skip; k++)
            if (!_strnicmp(g_modname, skip[k], strlen(skip[k]))) internal = 1;
        if (!internal) return s;
    }
    return g_nframes ? addr_str(g_frames[0]) : "?";
}

static void handle_cxx(EXCEPTION_POINTERS *ep)
{
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    g_throws_seen++;
    if (g_throws_logged >= MAX_THROWS_LOGGED) {
        if (g_throws_logged == MAX_THROWS_LOGGED) {
            g_throws_logged++;
            clog("C++ throw limit (%d) reached; further throws are not logged", MAX_THROWS_LOGGED);
            summary("WARNING C++ throw log limit (%d) reached; further throws are not logged", MAX_THROWS_LOGGED);
        }
        return;
    }
    unsigned long long key = decode_cxx(er);
    walk_stack(ep->ContextRecord);
    /* Throw site: frames RaiseException -> _CxxThrowException -> thrower (3 frames), not the callers. */
    for (int i = 0; i < g_nframes && i < 3; i++) key = (key ^ g_frames[i]) * 1099511628211ull;
    if (!key) key = 1;
    if (!site_should_log(key)) { g_throws_suppressed++; return; }
    g_throws_logged++;
    uint32_t sig = er->NumberParameters ? (uint32_t)er->ExceptionInformation[0] : 0;
    clog("C++ EXCEPTION #%ld thrown: %s%s%s%s (magic %08x, object %p)", g_throws_seen, g_types,
         g_what[0] ? " | what(): \"" : "", g_what, g_what[0] ? "\"" : "", sig,
         er->NumberParameters > 1 ? (void *)er->ExceptionInformation[1] : NULL);
    if (g_throws_suppressed) clog("  (%ld repeated throws suppressed so far)", g_throws_suppressed);
    mem_snapshot();
    log_stack();
    summary("EXCEPTION C++ %s%s%s%s at %s (commit avail %llu MB, private %llu MB)", g_types,
            g_what[0] ? " what()=\"" : "", g_what, g_what[0] ? "\"" : "", throw_site_str(),
            g_snap_avail_mb, g_snap_priv_mb);
}

/* ------------------------------------------------------------------ fatal SEH codes */

static const char *fatal_name(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW: return "INT_OVERFLOW";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_INVALID_DISPOSITION: return "INVALID_DISPOSITION";
    case 0xC0000374: return "HEAP_CORRUPTION";
    case 0xC0000409: return "STACK_BUFFER_OVERRUN/FAIL_FAST";
    case 0xC0000602: return "FAIL_FAST_EXCEPTION";
    case 0xC0000017: return "NO_MEMORY";
    case 0xC000012D: return "COMMITMENT_LIMIT";
    default: return 0;
    }
}

/* Stack overflow: very little stack left, so no printf, no walk. */
static char g_so_buf[256];
static void hex64(char **p, unsigned long long v)
{
    static const char d[] = "0123456789abcdef";
    *(*p)++ = '0'; *(*p)++ = 'x';
    int started = 0;
    for (int s = 60; s >= 0; s -= 4) {
        int x = (int)((v >> s) & 15);
        if (x || started || s == 0) { *(*p)++ = d[x]; started = 1; }
    }
}
static void log_stack_overflow(const EXCEPTION_POINTERS *ep)
{
    char *p = g_so_buf;
    const char *a = "STACK_OVERFLOW (0xC00000FD) rip=";
    while (*a) *p++ = *a++;
    hex64(&p, ep->ContextRecord->Rip);
    a = " rsp="; while (*a) *p++ = *a++;
    hex64(&p, ep->ContextRecord->Rsp);
    a = " tid="; while (*a) *p++ = *a++;
    hex64(&p, GetCurrentThreadId());
    HMODULE m = NULL;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)(uintptr_t)ep->ContextRecord->Rip, &m) && m) {
        DWORD n = GetModuleFileNameW(m, g_wmod, MAX_PATH);
        DWORD b = 0;
        for (DWORD i = 0; i < n; i++) if (g_wmod[i] == L'\\' || g_wmod[i] == L'/') b = i + 1;
        a = " module="; while (*a) *p++ = *a++;
        for (DWORD i = b; i < n && p < g_so_buf + 160; i++) *p++ = g_wmod[i] < 128 ? (char)g_wmod[i] : '?';
        a = " module_base="; while (*a) *p++ = *a++;
        hex64(&p, (uintptr_t)m);
        a = " offset="; while (*a) *p++ = *a++;
        hex64(&p, ep->ContextRecord->Rip - (uintptr_t)m);
    }
    *p++ = '\n';
    out_raw(g_so_buf, (DWORD)(p - g_so_buf));
}

static void handle_fatal(EXCEPTION_POINTERS *ep, const char *name)
{
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (g_fatal_logged >= MAX_FATAL_LOGGED) {
        if (g_fatal_logged++ == MAX_FATAL_LOGGED) {
            clog("fatal exception log limit reached");
            summary("WARNING fatal exception log limit (%d) reached", MAX_FATAL_LOGGED);
        }
        return;
    }
    g_fatal_logged++;
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || er->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) {
        ULONG_PTR kind = er->NumberParameters > 0 ? er->ExceptionInformation[0] : 9;
        ULONG_PTR addr = er->NumberParameters > 1 ? er->ExceptionInformation[1] : 0;
        const char *k = kind == 0 ? "read" : kind == 1 ? "write" : kind == 8 ? "execute (DEP)" : "?";
        const char *at = addr_str((DWORD64)(uintptr_t)er->ExceptionAddress);
        clog("EXCEPTION %s (0x%08lx) %s of address 0x%llx at %s", name, er->ExceptionCode, k,
             (unsigned long long)addr, at);
        log_regs(ep->ContextRecord);
        mem_snapshot();
        summary("EXCEPTION %s %s 0x%llx at %s (commit avail %llu MB)", name, k, (unsigned long long)addr, at,
                g_snap_avail_mb);
    } else {
        const char *at = addr_str((DWORD64)(uintptr_t)er->ExceptionAddress);
        clog("EXCEPTION %s (0x%08lx) at %s", name, er->ExceptionCode, at);
        log_regs(ep->ContextRecord);
        mem_snapshot();
        summary("EXCEPTION %s (0x%08lx) at %s (commit avail %llu MB)", name, er->ExceptionCode, at,
                g_snap_avail_mb);
    }
    walk_stack(ep->ContextRecord);
    log_stack();
}

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    const char *fname = 0;
    if (code != CXX_MAGIC && code != EXCEPTION_STACK_OVERFLOW && !(fname = fatal_name(code)))
        return EXCEPTION_CONTINUE_SEARCH;
    DWORD saved_err = GetLastError();
    if (!lock_acquire()) return EXCEPTION_CONTINUE_SEARCH;
    if (code == EXCEPTION_STACK_OVERFLOW) {
        log_stack_overflow(ep);          /* no api->log here: almost no stack left */
        lock_release();
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_summary[0] = 0;
    if (code == CXX_MAGIC) {
        uint32_t sig = ep->ExceptionRecord->NumberParameters ? (uint32_t)ep->ExceptionRecord->ExceptionInformation[0] : 0;
        if (sig == CXX_SIG1 || sig == CXX_SIG2 || sig == CXX_SIG3) handle_cxx(ep);
    } else {
        handle_fatal(ep, fname);
    }
    release_and_mirror();
    SetLastError(saved_err);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ------------------------------------------------------------------ unhandled filter + abort */

static LONG WINAPI unhandled_filter(EXCEPTION_POINTERS *ep)
{
    if (lock_acquire()) {
        const EXCEPTION_RECORD *er = ep->ExceptionRecord;
        const char *at = addr_str((DWORD64)(uintptr_t)er->ExceptionAddress);
        clog("UNHANDLED EXCEPTION 0x%08lx at %s - process is going down", er->ExceptionCode, at);
        if (er->ExceptionCode != EXCEPTION_STACK_OVERFLOW) {
            summary("EXCEPTION UNHANDLED 0x%08lx at %s - process is going down (details in crash.log)",
                    er->ExceptionCode, at);
            mem_snapshot();
            walk_stack(ep->ContextRecord);
            log_stack();
        }
        FlushFileBuffers(g_file);
        if (er->ExceptionCode != EXCEPTION_STACK_OVERFLOW) release_and_mirror();
        else lock_release();
    }
    return g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

static void __cdecl on_sigabrt(int sig)
{
    if (lock_acquire()) {
        clog("abort called (SIGABRT) - typically std::terminate after an uncaught C++ exception "
             "(see the last C++ EXCEPTION above)");
        mem_snapshot();
        CONTEXT c;
        RtlCaptureContext(&c);
        walk_stack(&c);
        log_stack();
        FlushFileBuffers(g_file);
        summary("ERROR abort called (SIGABRT), likely std::terminate after an uncaught C++ exception "
                "(commit avail %llu MB, private %llu MB; details in crash.log)", g_snap_avail_mb, g_snap_priv_mb);
        release_and_mirror();
    }
    signal(SIGABRT, g_prev_sigabrt ? g_prev_sigabrt : SIG_DFL);
    raise(sig);
}

/* ------------------------------------------------------------------ memory sampler */

static ULONGLONG g_last_sample, g_last_delta_check, g_last_low_log;
static unsigned long long g_logged_priv, g_logged_avail;
static int g_sampled_once;

static void on_frame(void *user)
{
    (void)user;
    ULONGLONG now = GetTickCount64();
    if (g_sampled_once && now - g_last_sample < 1000) return;
    g_last_sample = now;
    MEMORYSTATUSEX ms = {sizeof ms};
    PROCESS_MEMORY_COUNTERS_EX pc = {sizeof pc};
    GlobalMemoryStatusEx(&ms);
    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pc, sizeof pc);
    unsigned long long priv = pc.PrivateUsage, avail = ms.ullAvailPageFile;
    const char *why = 0;
    if (!g_sampled_once) why = "first frame";
    else if (avail < 1024 * MB && now - g_last_low_log >= 1000) { why = "LOW COMMIT"; g_last_low_log = now; }
    else if (now - g_last_delta_check >= 5000) {
        g_last_delta_check = now;
        long long dp = (long long)(priv - g_logged_priv), da = (long long)(avail - g_logged_avail);
        if (dp > (long long)(256 * MB) || dp < -(long long)(256 * MB) || da > (long long)(256 * MB) ||
            da < -(long long)(256 * MB))
            why = "changed";
    }
    if (!g_sampled_once) { g_sampled_once = 1; g_last_delta_check = now; }
    if (!why) return;
    g_logged_priv = priv; g_logged_avail = avail;
    if (!lock_acquire()) return;
    clog("mem (%s): private=%llu MB | commit avail=%llu MB of limit %llu MB | phys avail=%llu MB", why,
         priv / MB, avail / MB, ms.ullTotalPageFile / MB, ms.ullAvailPhys / MB);
    if (!strcmp(why, "LOW COMMIT"))
        summary("WARNING LOW COMMIT: commit avail %llu MB of limit %llu MB, private %llu MB (out-of-memory risk)",
                avail / MB, ms.ullTotalPageFile / MB, priv / MB);
    else
        summary("mem (%s): private %llu MB, commit avail %llu MB of limit %llu MB", why, priv / MB, avail / MB,
                ms.ullTotalPageFile / MB);
    release_and_mirror();
}

/* ------------------------------------------------------------------ load */

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    char path[MAX_PATH], prev[MAX_PATH];
    snprintf(path, sizeof path, "%scrash.log", api->v3x_dir());
    snprintf(prev, sizeof prev, "%scrash.prev.log", api->v3x_dir());
    MoveFileExA(path, prev, MOVEFILE_REPLACE_EXISTING);
    g_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_file == INVALID_HANDLE_VALUE) {
        api->log("crash_logger", "cannot open %s (error %lu)", path, GetLastError());
        return 1;
    }
    /* Leave room to log on this (main) thread after a stack overflow. */
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);

    if (lock_acquire()) {
        char exe[MAX_PATH];
        GetModuleFileNameA(NULL, exe, sizeof exe);
        SYSTEMTIME t; GetLocalTime(&t);
        clog("crash_logger started %04u-%02u-%02u, pid %lu, exe %s, game %s", t.wYear, t.wMonth, t.wDay,
             GetCurrentProcessId(), exe, api->game_version());
        mem_snapshot();
        lock_release();
    }

    void *h = AddVectoredExceptionHandler(1, veh);
    g_prev_filter = SetUnhandledExceptionFilter(unhandled_filter);
    g_prev_sigabrt = signal(SIGABRT, on_sigabrt);
    if (g_prev_sigabrt == SIG_ERR) g_prev_sigabrt = SIG_DFL;
    api->log("crash_logger", "logging to %s (VEH %s, SIGABRT handler installed)", path, h ? "ok" : "FAILED");
    if (api->on_frame(on_frame, 0) != 0) api->log("crash_logger", "on_frame registration failed");
    return h ? 0 : 1;
}
