/* V3X core: runs inside victoria3.exe after being loaded as version.dll.
 *
 * Boot sequence
 *   1. DllMain(PROCESS_ATTACH): resolve the real version.dll, init MinHook and hook the
 *      exe's entry point. Nothing else happens under the loader lock.
 *   2. Entry detour (main thread, before CRT startup): load every DLL in v3x/plugins,
 *      call its v3x_plugin_load(&api), then jump to the real entry point.
 *   3. PeekMessageW hook drives on_frame callbacks (and the run_on_main queue) for plugins.
 *   4. API v2: if the console window is open, a "v3x_console_input" thread reads lines from
 *      CONIN$ and dispatches them to registered commands (built-ins: help, plugins, log, ...).
 *      It starts after all plugins loaded, so a plugin hooking console input sees the first read.
 *   5. API v3: exec_capture runs a command and also hands its print/log output (made on the
 *      calling thread) to a sink, via a per-thread capture slot (TLS index g_cap_tls).
 *      on_event / emit_event: named plugin-to-plugin events (fixed table of 64 subscriptions).
 *      API v4: hook_safe (generic pass-through thunk, src/safe_hook.c; several plugins may share
 *      one target, raw api->hook stays exclusive -> V3X_EHOOKBUSY), main_thread_id,
 *      game_build_id, enum_commands; the optional `v3x_plugin_manifest` export is read before
 *      v3x_plugin_load (shown by `plugins`; min_api / V3X_MANIFEST_REQUIRE_BUILD refuse the load).
 *   6. Process exit. ntdll!RtlExitUserProcess (ExitProcess) is hooked: on the first call the core
 *      emits "process_exit" on the exiting thread while every thread still runs (final reports go
 *      there), and waits (bounded) for an api->hook in flight, because MinHook suspends all other
 *      threads and a suspender killed mid-way would leave the exiting thread suspended forever.
 *      Then ExitProcess kills every other thread (any of them may die holding
 *      g_log_lock, a CRT FILE lock, the heap lock or MinHook's spin lock), then runs DLL detach.
 *      From then on (process_exiting()) every API call that would lock, write or hook returns at
 *      once without doing anything, and DllMain(DLL_PROCESS_DETACH) does nothing but set a flag:
 *      no MH_Uninitialize, no fclose, no thread waits. The OS closes handles and frees memory.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <share.h>

#include <tlhelp32.h>

#include "MinHook.h"
#include "../include/v3x.h"
#include "../sdk/v3x_safe.h"
#include "core_parse.h"   /* plugins.json / allowlist / capabilities / pattern parsers */
#include "v3x_pe.h"       /* SHA-256 + bounded PE reader (plugin files are checked before LoadLibrary) */
#include "../installer/release_sig.h"   /* Ed25519 release signature (plugins.json.sig), ADR 0001; Monocypher is in core_sig.c */

int v3x_proxy_resolve(void);
/* hook registry, src/safe_hook.c: per-target ownership, shared hook_safe thunks, exclusive raw hooks */
int v3x_hook_add_safe(void *owner, void *target, v3x_hook_fn pre, v3x_hook_fn post, void *user, void **busy_owner);
int v3x_hook_add_raw(void *owner, void *target, void *detour, void **original, void **busy_owner);
int v3x_hook_remove(void *owner, void *target, void **busy_owner);
int v3x_hook_subscribers(void *target);
int v3x_hook_remove_owner(void *owner);
void v3x_hook_lock(void);
void v3x_hook_unlock(void);
int v3x_hook_pool_overlaps_locked(const void *p, size_t n);
void *v3x_hook_conflict_locked(const void *p, size_t n, void *me);

/* ------------------------------------------------------------------ state */

/* Loader semver (API v5 loader_version). Bump: patch = fixes, minor = new API fields/commands,
 * major = never without a new V3X_API generation (docs/api-stability.md). */
#define V3X_LOADER_SEMVER "0.6.0"

static HMODULE g_self;
static uint8_t *g_base;
static size_t g_size;
static uint8_t *g_text; static size_t g_text_size;
/* Paths. The core itself only uses the wide (UTF-16) forms for its own files and for loading plugins, so a
 * game folder with characters outside the ANSI code page works (audit L13). api->game_dir / v3x_dir
 * stay ANSI (the frozen API returns const char *): plugins pass them to fopen/CreateFileA; if the folder
 * cannot be represented in the ANSI code page boot() logs a WARNING. */
#define V3X_WPATH 1024
static wchar_t g_game_dirW[V3X_WPATH];   /* ...\binaries\ */
static wchar_t g_v3x_dirW[V3X_WPATH];    /* ...\binaries\v3x\ */
static char g_game_dir[MAX_PATH];
static char g_v3x_dir[MAX_PATH];
static char g_version[128] = "unknown";
static char g_build_id[40] = "unknown";   /* "<PE TimeDateStamp>-<SizeOfImage>", API v4 game_build_id */
static CRITICAL_SECTION g_log_lock;
static FILE *g_log;

/* TRUE once ExitProcess has terminated the other threads (ntdll RtlDllShutdownInProgress, resolved
 * in boot) or our own DLL_PROCESS_DETACH ran. Lock-free; see boot step 6. */
typedef BOOLEAN(NTAPI *shutdown_fn)(void);
static shutdown_fn g_shutdown_in_progress;
static volatile LONG g_detached;
static volatile LONG g_exit_started;   /* RtlExitUserProcess entered: no new hooks */
static volatile LONG g_hook_busy;      /* api_hook/api_unhook calls in flight */
static int process_exiting(void)
{
    return g_detached || (g_shutdown_in_progress && g_shutdown_in_progress());
}

typedef struct { uint8_t *ptr; size_t size; } Range;
static Range g_data_ranges[8]; static int g_data_count;

/* `mod` = the module the callback's code lives in (fault isolation: a faulted plugin's callbacks are
 * skipped). NULL = not a module (e.g. heap code) or the core. `owner` = the module that registered it
 * (API v6 off_frame and quotas). `id` = unique per registration (a removed entry is never called again,
 * even from a tick snapshot taken before the removal). */
#define MAX_FRAME_CBS 64
typedef struct { v3x_frame_fn fn; void *user; HMODULE mod; void *owner; LONG id; } FrameCb;
static FrameCb g_frame_cbs[MAX_FRAME_CBS];
static LONG g_frame_count;               /* under g_frame_lock */
static volatile LONG g_frame_gen;        /* bumped by every removal */
static volatile LONG g_reg_ids;          /* id source for frame / event registrations */
static SRWLOCK g_frame_lock = SRWLOCK_INIT;   /* writers exclusive; the tick takes it shared to snapshot */

/* loaded-plugin records for the `plugins` command. Append-only; `mod` is published last (interlocked)
 * so lock-free readers (owner / capability checks on any thread) see complete records. */
#define MAX_PLUGINS 64
static struct {
    char name[64]; char file[64]; char status[128];
    char version[24]; char author[48]; char desc[112];   /* from v3x_plugin_manifest (API v4), may be "" */
    char stem[64];                                        /* file name without .dll */
    char sha[65];                                         /* sha256 of the file as loaded / refused, or "" */
    char trust[32];                                       /* "official", "trusted (allowlist)", "dev", "UNTRUSTED" */
    char caps_text[64];                                   /* manifest capabilities as declared, "" = undeclared */
    uint32_t caps; int caps_declared;                     /* CP_CAP_* mask (all when undeclared) */
    volatile LONG caps_warned;                            /* capability refusals already logged (mask) */
    int untrusted;                                        /* refused by the trust check (see `trust`) */
    HMODULE volatile mod;                                 /* NULL if not loaded */
} g_plugins[MAX_PLUGINS];
static volatile LONG g_plugin_count;

/* console commands (API v2) */
#define MAX_COMMANDS 128
typedef struct { char name[32]; char help[160]; v3x_command_fn fn; void *user; HMODULE mod; void *owner; } Cmd;
static Cmd g_cmds[MAX_COMMANDS];
static int g_cmd_count;
static CRITICAL_SECTION g_cmd_lock;

/* run_on_main queue, drained in the frame tick */
#define MAIN_QUEUE 256
static struct { v3x_frame_fn fn; void *user; } g_mainq[MAIN_QUEUE];
static unsigned g_mainq_head, g_mainq_tail; /* tail - head = queued items */
static CRITICAL_SECTION g_mainq_lock;
static volatile LONG g_mainq_len;

/* ------------------------------------------------------------------ log */

/* Live console window (like BepInEx/SKSE). Opened on the main thread in hk_entry, never under
 * the loader lock. Lines logged before it opens are kept in g_early and replayed. Disable with
 * env V3X_NO_CONSOLE=1 or a line "console=0" in v3x/v3x.ini. */
static HANDLE g_con;
static char g_early[16384]; static size_t g_early_len;

static void console_line(const char *tag, const char *line)
{
    if (!g_con) {
        size_t n = strlen(line);
        if (g_early_len + n + 1 < sizeof g_early) {
            memcpy(g_early + g_early_len, line, n);
            g_early_len += n;
            g_early[g_early_len++] = '\n';
        }
        return;
    }
    WORD color = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;            /* v3x: cyan */
    if (tag && strcmp(tag, "v3x")) color = FOREGROUND_GREEN | FOREGROUND_INTENSITY;  /* plugins: green */
    if (strstr(line, "FAIL") || strstr(line, "ERROR") || strstr(line, "EXCEPTION") || strstr(line, "WARNING"))
        color = FOREGROUND_RED | FOREGROUND_INTENSITY;
    SetConsoleTextAttribute(g_con, color);
    DWORD w;
    WriteConsoleA(g_con, line, (DWORD)strlen(line), &w, NULL);
    WriteConsoleA(g_con, "\r\n", 2, &w, NULL);
    SetConsoleTextAttribute(g_con, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

/* API v3 capture: per-thread current sink, set by api_exec_capture for the duration of a command. */
typedef struct { void (*sink)(const char *text, void *user); void *user; } Capture;
static DWORD g_cap_tls = TLS_OUT_OF_INDEXES;

/* Hands text to the calling thread's sink, if any. Called outside g_log_lock so the sink may do
 * anything; the slot is cleared while it runs so its own print/log calls are not fed back to it.
 * Preserves GetLastError (TlsGetValue resets it). */
static void capture_deliver(const char *text)
{
    if (g_cap_tls == TLS_OUT_OF_INDEXES) return;
    DWORD err = GetLastError();
    Capture *c = (Capture *)TlsGetValue(g_cap_tls);
    if (c && c->sink) {
        TlsSetValue(g_cap_tls, NULL);
        c->sink(text, c->user);
        TlsSetValue(g_cap_tls, c);
    }
    SetLastError(err);
}

/* One log line: "[hh:mm:ss.mmm][tag] " + prefix (API v5 level prefix, may be "") + message. */
static void v3x_vlog(const char *tag, const char *prefix, const char *fmt, va_list ap) __attribute__((format(printf, 3, 0)));
static void v3x_vlog(const char *tag, const char *prefix, const char *fmt, va_list ap)
{
    if (process_exiting() || !fmt) return;   /* the lock/FILE/console may belong to a dead thread */
    char line[2048];
    SYSTEMTIME t; GetLocalTime(&t);
    int n = snprintf(line, sizeof line, "[%02d:%02d:%02d.%03d][%s] %s", t.wHour, t.wMinute, t.wSecond,
                     t.wMilliseconds, tag ? tag : "?", prefix);
    if (n < 0) return;
    if ((size_t)n >= sizeof line) n = (int)sizeof line - 1;
    vsnprintf(line + n, sizeof line - n, fmt, ap);
    EnterCriticalSection(&g_log_lock);
    if (g_log) { fputs(line, g_log); fputc('\n', g_log); fflush(g_log); }
    console_line(tag, line);
    LeaveCriticalSection(&g_log_lock);
    size_t L = strlen(line);
    if (L + 1 < sizeof line) { line[L] = '\n'; line[L + 1] = 0; }
    capture_deliver(line);
}

static void v3x_log(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void v3x_log(const char *tag, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); v3x_vlog(tag, "", fmt, ap); va_end(ap);
}

/* ------------------------------------------------------------------ log levels (API v5) */

/* Threshold per tag from v3x.ini [log] (read in boot), changed at runtime by `loglevel`. Readers
 * are lock-free: an entry's tag is written before g_lvl_count is raised, levels are single LONGs.
 * Writers hold g_log_lock. 0 = off (nothing, not even errors). */
#define MAX_LOG_TAGS 32
static struct { char tag[32]; volatile LONG level; } g_lvl[MAX_LOG_TAGS];
static volatile LONG g_lvl_count;
static volatile LONG g_lvl_global = V3X_LOG_INFO;

/* "error|err|warn|warning|info|debug|trace|off" or 0-5 -> level, -1 if not a level. */
static int parse_level(const char *s, size_t n)
{
    static const char *names[] = {"off", "error", "warn", "info", "debug", "trace"};
    if (n == 1 && s[0] >= '0' && s[0] <= '9') return s[0] - '0' > 5 ? 5 : s[0] - '0';
    for (int i = 0; i < 6; i++)
        if (strlen(names[i]) == n && !_strnicmp(s, names[i], n)) return i;
    if (n == 3 && !_strnicmp(s, "err", 3)) return V3X_LOG_ERROR;
    if (n == 7 && !_strnicmp(s, "warning", 7)) return V3X_LOG_WARN;
    return -1;
}

static int api_log_level(const char *tag)
{
    if (tag) {
        LONG n = g_lvl_count;
        for (LONG i = 0; i < n; i++)
            if (!_stricmp(g_lvl[i].tag, tag)) return (int)g_lvl[i].level;
    }
    return (int)g_lvl_global;
}

/* tag NULL = global. Returns 0, or -1 if the table is full / the tag is too long. */
static int set_log_level(const char *tag, size_t tn, int level)
{
    int rc = 0;
    if (!tag) { InterlockedExchange(&g_lvl_global, level); return 0; }
    if (!tn || tn >= sizeof g_lvl[0].tag) return -1;
    EnterCriticalSection(&g_log_lock);
    LONG n = g_lvl_count, i;
    for (i = 0; i < n; i++)
        if (strlen(g_lvl[i].tag) == tn && !_strnicmp(g_lvl[i].tag, tag, tn)) break;
    if (i < n) InterlockedExchange(&g_lvl[i].level, level);
    else if (n >= MAX_LOG_TAGS) rc = -1;
    else {
        memcpy(g_lvl[n].tag, tag, tn); g_lvl[n].tag[tn] = 0;
        g_lvl[n].level = level;
        InterlockedIncrement(&g_lvl_count);   /* full barrier: tag and level are visible first */
    }
    LeaveCriticalSection(&g_log_lock);
    return rc;
}

static void api_log_at(int level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void api_log_at(int level, const char *tag, const char *fmt, ...)
{
    static const char *prefix[] = {"", "ERROR: ", "WARNING: ", "", "debug: ", "trace: "};
    if (level < V3X_LOG_ERROR) level = V3X_LOG_ERROR;
    if (level > V3X_LOG_TRACE) level = V3X_LOG_TRACE;
    if (level > api_log_level(tag)) return;
    va_list ap; va_start(ap, fmt); v3x_vlog(tag, prefix[level], fmt, ap); va_end(ap);
}

/* API v2 print: plain text to console + v3x.log, newline appended if missing. */
static int api_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int api_print(const char *fmt, ...)
{
    char buf[8192];
    if (!fmt || process_exiting()) return -1;
    va_list ap; va_start(ap, fmt); int n = vsnprintf(buf, sizeof buf - 1, fmt, ap); va_end(ap);
    if (n < 0) return -1;
    size_t len = strlen(buf);
    if (!len || buf[len - 1] != '\n') { buf[len++] = '\n'; buf[len] = 0; }
    EnterCriticalSection(&g_log_lock);
    if (g_log) { fputs(buf, g_log); fflush(g_log); }
    if (g_con) {
        DWORD w;
        SetConsoleTextAttribute(g_con, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
        /* LF -> CRLF so it also renders right if processed output is ever turned off */
        const char *p = buf, *end = buf + len;
        while (p < end) {
            const char *nl = memchr(p, '\n', end - p);
            size_t k = nl ? (size_t)(nl - p) : (size_t)(end - p);
            if (k && p[k - 1] == '\r') k--;
            if (k) WriteConsoleA(g_con, p, (DWORD)k, &w, NULL);
            if (nl) WriteConsoleA(g_con, "\r\n", 2, &w, NULL);
            p = nl ? nl + 1 : end;
        }
        SetConsoleTextAttribute(g_con, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    }
    LeaveCriticalSection(&g_log_lock);
    capture_deliver(buf);
    return (int)len;
}

/* out = dir + name (ASCII/UTF-8 name). 0, or -1 (out = L"") if it does not fit. Bounded, always terminated. */
static int wpath(wchar_t *out, size_t cap, const wchar_t *dir, const char *name)
{
    size_t n = wcslen(dir);
    if (!cap) return -1;
    out[0] = 0;
    if (n >= cap) return -1;
    wmemcpy(out, dir, n);
    int k = MultiByteToWideChar(CP_UTF8, 0, name, -1, out + n, (int)(cap - n));
    if (k <= 0) { out[0] = 0; return -1; }
    return 0;
}

/* fopen of <v3x_dir><name> through the wide path (L13). NULL if missing or the path does not fit. */
static FILE *v3x_fopen(const char *name, const wchar_t *mode)
{
    wchar_t p[V3X_WPATH];
    if (wpath(p, V3X_WPATH, g_v3x_dirW, name)) return NULL;
    return _wfopen(p, mode);
}

/* UTF-8 form of a wide string for log lines ("?" on failure). */
static const char *utf8_of(const wchar_t *w, char *buf, size_t n)
{
    if (!n) return "";
    if (!w || WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)n, NULL, NULL) <= 0) snprintf(buf, n, "?");
    return buf;
}

static int console_wanted(void)
{
    if (GetEnvironmentVariableA("V3X_NO_CONSOLE", NULL, 0)) return 0;
    FILE *f = v3x_fopen("v3x.ini", L"r");
    if (!f) return 1;
    char buf[256]; int want = 1;
    while (fgets(buf, sizeof buf, f))
        if (!strncmp(buf, "console=0", 9) || !strncmp(buf, "console = 0", 11)) want = 0;
    fclose(f);
    return want;
}

static void start_console_input(void);

static void open_console(void)
{
    if (!console_wanted()) return;
    if (!AllocConsole()) return;
    SetConsoleTitleA("V3X console - Victoria 3 native loader");
    SetConsoleOutputCP(CP_UTF8);
    /* Closing a console window kills its process: remove the close button so the game survives. */
    HWND w = GetConsoleWindow();
    if (w) { HMENU m = GetSystemMenu(w, FALSE); if (m) DeleteMenu(m, SC_CLOSE, MF_BYCOMMAND); }
    EnterCriticalSection(&g_log_lock);
    /* Open the console screen buffer directly. GetStdHandle(STD_OUTPUT_HANDLE) can be an
     * inherited redirect (pipe/NUL from a launcher), which would leave the window blank. */
    g_con = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
    if (g_con == INVALID_HANDLE_VALUE) g_con = NULL;
    if (g_con) {
        /* replay lines logged before the console existed */
        char *p = g_early, *end = g_early + g_early_len;
        while (p < end) {
            char *nl = memchr(p, '\n', end - p);
            if (!nl) break;
            *nl = 0;
            const char *tag = strstr(p, "][v3x]") ? "v3x" : "plugin";
            console_line(tag, p);
            p = nl + 1;
        }
        g_early_len = 0;
    }
    LeaveCriticalSection(&g_log_lock);
    /* the input thread starts in hk_entry after load_plugins, so plugins that filter console input
     * (console_plus hooks ReadConsoleA) are installed before its first read */
}
#define LOG(...) v3x_log("v3x", __VA_ARGS__)

/* ------------------------------------------------------------------ fault isolation (loader 0.5.0) */

/* Every call from the core into plugin code that the core itself starts (v3x_plugin_load, on_frame,
 * run_on_main, console commands, event subscribers) runs inside a structured-exception guard (SEH
 * __try/__except; the core is built with -fms-extensions). Frame-based, so handlers deeper in the
 * stack (the plugin's own, the engine's) see the exception first; vectored handlers (crash_logger)
 * see it before that and may log it. Only hardware faults and C++ exceptions escaping the plugin are
 * caught (not stack overflow, not breakpoints). After a fault the plugin's module is marked faulted:
 * its frame/event/command/run_on_main callbacks are skipped from then on; its hooks and patches stay
 * (removing a hook from inside a call that may be on the stack is not safe). What cannot be isolated:
 * faults inside hooked engine code, in hook detours / hook_safe pre/post, or in threads the plugin
 * starts itself. */
typedef struct { DWORD code; void *addr; ULONG_PTR info[2]; DWORD nparams; } Fault;

static int fault_filter(EXCEPTION_POINTERS *ep, Fault *f)
{
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    switch (er->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_IN_PAGE_ERROR: case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION: case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: case EXCEPTION_DATATYPE_MISALIGNMENT:
    case 0xE06D7363u:   /* MSVC C++ exception that no plugin frame caught */
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (process_exiting()) return EXCEPTION_CONTINUE_SEARCH;
    f->code = er->ExceptionCode;
    f->addr = er->ExceptionAddress;
    f->nparams = er->NumberParameters;
    f->info[0] = er->NumberParameters > 0 ? er->ExceptionInformation[0] : 0;
    f->info[1] = er->NumberParameters > 1 ? er->ExceptionInformation[1] : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}

/* ---- callback context (audit N2). While the core runs a plugin's callback on a thread, the thread's
 * slot g_ctx_tls holds that plugin's module. An API call whose return address is inside the core (a
 * tail call: `return api->hook(...)` compiles to a jmp, so the return address is the core's guard) is
 * attributed to it. Nested callbacks save and restore the previous value. */
static DWORD g_ctx_tls = TLS_OUT_OF_INDEXES;
static void *ctx_enter(HMODULE m)
{
    if (g_ctx_tls == TLS_OUT_OF_INDEXES) return NULL;
    DWORD err = GetLastError();
    void *prev = TlsGetValue(g_ctx_tls);
    TlsSetValue(g_ctx_tls, (void *)m);
    SetLastError(err);
    return prev;
}
static void ctx_leave(void *prev)
{
    if (g_ctx_tls == TLS_OUT_OF_INDEXES) return;
    DWORD err = GetLastError();
    TlsSetValue(g_ctx_tls, prev);
    SetLastError(err);
}

/* Each guard returns 0 if the callback returned normally, 1 if it faulted (f filled). `m` = the module
 * the callback belongs to (the callback context above). */
static int guard_frame_fn_(v3x_frame_fn fn, void *user, Fault *f)
{
    __try { fn(user); return 0; }
    __except (fault_filter(GetExceptionInformation(), f)) { return 1; }
}
static int guard_frame_fn(HMODULE m, v3x_frame_fn fn, void *user, Fault *f)
{
    void *prev = ctx_enter(m);
    int r = guard_frame_fn_(fn, user, f);
    ctx_leave(prev);
    return r;
}
static int guard_command_fn_(v3x_command_fn fn, const char *args, void *user, Fault *f)
{
    __try { fn(args, user); return 0; }
    __except (fault_filter(GetExceptionInformation(), f)) { return 1; }
}
static int guard_command_fn(HMODULE m, v3x_command_fn fn, const char *args, void *user, Fault *f)
{
    void *prev = ctx_enter(m);
    int r = guard_command_fn_(fn, args, user, f);
    ctx_leave(prev);
    return r;
}
typedef void (*v3x_event_fn)(const char *name, void *data, void *user);
static int guard_event_fn_(v3x_event_fn fn, const char *name, void *data, void *user, Fault *f)
{
    __try { fn(name, data, user); return 0; }
    __except (fault_filter(GetExceptionInformation(), f)) { return 1; }
}
static int guard_event_fn(HMODULE m, v3x_event_fn fn, const char *name, void *data, void *user, Fault *f)
{
    void *prev = ctx_enter(m);
    int r = guard_event_fn_(fn, name, data, user, f);
    ctx_leave(prev);
    return r;
}
typedef void (*v3x_enum_fn)(const char *name, const char *help, void *user);
static int guard_enum_fn_(v3x_enum_fn fn, const char *name, const char *help, void *user, Fault *f)
{
    __try { fn(name, help, user); return 0; }
    __except (fault_filter(GetExceptionInformation(), f)) { return 1; }
}
static int guard_load_fn_(v3x_plugin_load_fn fn, const V3X_API *api, int *rc, Fault *f)
{
    __try { *rc = fn(api); return 0; }
    __except (fault_filter(GetExceptionInformation(), f)) { return 1; }
}
static int guard_load_fn(HMODULE m, v3x_plugin_load_fn fn, const V3X_API *api, int *rc, Fault *f)
{
    void *prev = ctx_enter(m);
    int r = guard_load_fn_(fn, api, rc, f);
    ctx_leave(prev);
    return r;
}

/* The module containing addr (no refcount change), NULL if none. */
static HMODULE module_of(const void *addr)
{
    HMODULE m = NULL;
    if (!addr || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     (LPCWSTR)addr, &m))
        return NULL;
    return m;
}

static const char *module_base_name(HMODULE m, char *buf, size_t n)
{
    wchar_t path[V3X_WPATH];
    DWORD k = m ? GetModuleFileNameW(m, path, V3X_WPATH) : 0;
    if (!k || k >= V3X_WPATH) { snprintf(buf, n, "?"); return buf; }
    const wchar_t *b = wcsrchr(path, L'\\');
    return utf8_of(b ? b + 1 : path, buf, n);
}

/* Modules whose callbacks are skipped (faulted, or v3x_plugin_load failed). Append-only; the module
 * is written before the count is raised, readers are lock-free. */
#define MAX_BAD_MODS 64
static HMODULE g_bad_mods[MAX_BAD_MODS];
static volatile LONG g_bad_count;
static volatile LONG g_faults;   /* faults caught so far (for tests / `plugins`) */

static int module_disabled(HMODULE m)
{
    LONG n = g_bad_count;
    if (!m || !n) return 0;
    for (LONG i = 0; i < n; i++)
        if (g_bad_mods[i] == m) return 1;
    return 0;
}

static int api_emit_event(const char *name, void *data);
static int emit_core_event(const char *name, void *data);

/* The plugin record of module m (published records only), or -1. Lock-free. */
static int plugin_index(HMODULE m)
{
    LONG n = g_plugin_count;
    if (!m) return -1;
    for (LONG i = 0; i < n && i < MAX_PLUGINS; i++)
        if (g_plugins[i].mod == m) return (int)i;
    return -1;
}

/* Marks m disabled (its callbacks are skipped from now on) and, the first time, tells every subscriber
 * of "plugin_disabled" (API v6) synchronously on this thread, so registries that call plugin code
 * themselves (script_ext) stop calling it. Never called with a core lock held. */
static void module_disable(HMODULE m, const char *status)
{
    if (!m || m == g_self) return;
    int fresh = 0;
    char stem[64] = "?";
    EnterCriticalSection(&g_log_lock);
    if (!module_disabled(m) && g_bad_count < MAX_BAD_MODS) {
        g_bad_mods[g_bad_count] = m;
        InterlockedIncrement(&g_bad_count);
        fresh = 1;
    }
    for (int i = 0; i < g_plugin_count; i++)
        if (g_plugins[i].mod == m) {
            snprintf(g_plugins[i].status, sizeof g_plugins[i].status, "%s", status);
            snprintf(stem, sizeof stem, "%s", g_plugins[i].stem);
        }
    LeaveCriticalSection(&g_log_lock);
    if (fresh) {
        V3X_PluginDisabled d;
        memset(&d, 0, sizeof d);
        d.struct_size = (uint32_t)sizeof d;
        d.name = stem;
        d.module = (void *)m;
        d.reason = status;
        emit_core_event(V3X_EVENT_PLUGIN_DISABLED, &d);
    }
}

/* ---- owners (audit N1/N2). Every hook, patch and registration belongs to the module that made the
 * call: the module containing the return address, or, when that is the core itself (a tail call out of
 * a callback the core invoked) or no module at all, the plugin whose callback runs on this thread. If
 * neither exists the owner is OWNER_UNKNOWN, which owns nothing: it may not hook, unhook or patch. The
 * core's own hooks are owned by g_self, which no API caller can ever resolve to. */
static char g_unknown_owner_tag;
#define OWNER_UNKNOWN ((void *)&g_unknown_owner_tag)
static void *caller_owner(void *ret_addr)
{
    HMODULE m = module_of(ret_addr);
    if (m && m != g_self) return (void *)m;
    if (g_ctx_tls != TLS_OUT_OF_INDEXES) {
        DWORD err = GetLastError();
        HMODULE c = (HMODULE)TlsGetValue(g_ctx_tls);
        SetLastError(err);
        if (c && c != g_self) return (void *)c;
    }
    return OWNER_UNKNOWN;
}

/* Capability check (API v6 manifest field). Only plugins that DECLARE capabilities are restricted;
 * other modules (the exe, tools, plugins with older manifests) are not. Logged once per plugin and cap. */
static int caps_allow(void *owner, uint32_t cap, const char *what)
{
    static const char *names[] = {"hook", "patch", "fs", "net", "script", "exec"};
    int i = plugin_index((HMODULE)owner);
    if (i < 0 || !g_plugins[i].caps_declared || (g_plugins[i].caps & cap)) return 1;
    LONG old = InterlockedOr(&g_plugins[i].caps_warned, (LONG)cap);
    if (!(old & (LONG)cap)) {
        const char *cn = "?";
        for (int k = 0; k < 6; k++) if (cap == 1u << k) cn = names[k];
        LOG("WARNING: plugin %s called %s but its manifest does not declare the capability \"%s\" (it declares "
            "\"%s\"): refused", g_plugins[i].stem, what, cn, g_plugins[i].caps_text);
    }
    return 0;
}

/* 1 if module m is a loaded plugin that already holds `have` entries of a table with quota q. m is the OWNER of
 * the registration (the module that called the API), counted by .owner: a callback address outside the plugin
 * (executable heap, another module) must not escape the quota (test/api_v6_test.c). Lock-free,
 * may be called under a table lock; quota_log (takes the log lock) only after releasing it. */
static int quota_full(HMODULE m, int have, int q) { return have >= q && plugin_index(m) >= 0; }
static void quota_log(HMODULE m, int q, const char *what)
{
    int i = plugin_index(m);
    LOG("WARNING: plugin %s: %s quota reached (%d per plugin): refused", i >= 0 ? g_plugins[i].stem : "?", what, q);
}

/* A guarded callback of module `owner` faulted: log where, disable the module's callbacks. */
static void on_fault(HMODULE owner, const char *what, const char *detail, const Fault *f)
{
    char pm[MAX_PATH], fm[MAX_PATH], st[128];
    HMODULE at = module_of(f->addr);
    InterlockedIncrement(&g_faults);
    module_base_name(owner, pm, sizeof pm);
    if (f->code == EXCEPTION_ACCESS_VIOLATION && f->nparams >= 2)
        LOG("FAULT in plugin %s during %s%s%s: exception 0x%08lx (%s of 0x%llx) at %s+0x%llx", pm, what,
            detail ? " " : "", detail ? detail : "", f->code, f->info[0] == 0 ? "read" : f->info[0] == 1 ? "write" : "execute",
            (unsigned long long)f->info[1], at ? module_base_name(at, fm, sizeof fm) : "?",
            (unsigned long long)((uintptr_t)f->addr - (uintptr_t)at));
    else
        LOG("FAULT in plugin %s during %s%s%s: exception 0x%08lx at %s+0x%llx", pm, what, detail ? " " : "",
            detail ? detail : "", f->code, at ? module_base_name(at, fm, sizeof fm) : "?",
            (unsigned long long)((uintptr_t)f->addr - (uintptr_t)at));
    if (!owner || owner == g_self || !strcmp(what, "v3x_plugin_load")) return;   /* load: rollback_module logs */
    LOG("ERROR: plugin %s disabled: its frame/event/command/run_on_main callbacks are skipped from now on "
        "(its hooks stay). The game continues; restart to load it again.", pm);
    snprintf(st, sizeof st, "FAULTED in %s (exception 0x%08lx), callbacks disabled", what, f->code);
    module_disable(owner, st);
}

/* ------------------------------------------------------------------ console commands (API v2) */

/* Copies a plugin string into buf (cut at cap-1, UTF-8 safe); 1 if it fit, 0 if cut. Outside any lock. */
static int copy_in(char *buf, size_t cap, const char *s)
{
    size_t n = strnlen(s, cap);
    if (n < cap) { memcpy(buf, s, n + 1); return 1; }
    n = v3x_utf8_trim_tail(s, cap - 1, 0);
    memcpy(buf, s, n);
    buf[n] = 0;
    return 0;
}

static int register_command_as(void *owner, const char *name, const char *help, v3x_command_fn fn, void *user)
{
    Cmd c;
    if (!name || !fn || process_exiting()) return -1;
    memset(&c, 0, sizeof c);
    /* audit N8: everything the plugin points to is copied before the lock is taken */
    if (!copy_in(c.name, sizeof c.name, name) || !c.name[0] || strpbrk(c.name, " \t\r\n")) return -1;
    copy_in(c.help, sizeof c.help, help ? help : "");
    c.fn = fn; c.user = user; c.mod = module_of((const void *)fn); c.owner = owner;
    int rc = 0, mine = 0;
    EnterCriticalSection(&g_cmd_lock);
    for (int i = 0; i < g_cmd_count; i++) {
        if (!_stricmp(g_cmds[i].name, c.name)) { rc = -2; break; }
        mine += g_cmds[i].owner == owner;
    }
    int quota = !rc && quota_full((HMODULE)owner, mine, V3X_QUOTA_COMMANDS);
    if (!rc && (quota || g_cmd_count >= MAX_COMMANDS)) rc = -1;
    if (!rc) g_cmds[g_cmd_count++] = c;
    LeaveCriticalSection(&g_cmd_lock);
    if (quota) quota_log((HMODULE)owner, V3X_QUOTA_COMMANDS, "register_command");
    return rc;
}

static int api_register_command(const char *name, const char *help, v3x_command_fn fn, void *user)
{
    return register_command_as(caller_owner(__builtin_return_address(0)), name, help, fn, user);
}

/* API v6: removes a command the caller registered (or whose handler lives in the caller's module). */
static int api_unregister_command(const char *name)
{
    void *me = caller_owner(__builtin_return_address(0));
    char nm[64];
    if (!name || process_exiting()) return V3X_EARG;
    if (!copy_in(nm, sizeof nm, name) || !nm[0]) return V3X_ENOTFOUND;
    int rc = V3X_ENOTFOUND;
    EnterCriticalSection(&g_cmd_lock);
    for (int i = 0; i < g_cmd_count; i++) {
        if (_stricmp(g_cmds[i].name, nm)) continue;
        if (me == OWNER_UNKNOWN || (g_cmds[i].owner != me && (void *)g_cmds[i].mod != me)) { rc = V3X_EPERM; break; }
        memmove(&g_cmds[i], &g_cmds[i + 1], sizeof g_cmds[0] * (size_t)(g_cmd_count - i - 1));
        g_cmd_count--;
        rc = 0;
        break;
    }
    LeaveCriticalSection(&g_cmd_lock);
    return rc;
}

/* Snapshot of the command table (it may shrink while callers iterate, API v6). Heap; NULL on failure. */
static Cmd *cmd_snapshot(int *n)
{
    *n = 0;
    Cmd *s = (Cmd *)HeapAlloc(GetProcessHeap(), 0, sizeof(Cmd) * MAX_COMMANDS);
    if (!s) return NULL;
    EnterCriticalSection(&g_cmd_lock);
    *n = g_cmd_count;
    memcpy(s, g_cmds, sizeof(Cmd) * (size_t)*n);
    LeaveCriticalSection(&g_cmd_lock);
    return s;
}

/* Command threading rule (security hardening, include/v3x.h thread rules): command handlers never run
 * concurrently. exec_command (console input thread, devbridge, plugins on any thread) holds the
 * command-run lock while the handler runs. It is recursive per thread, so a handler may exec other
 * commands (exec_capture("help") inside a command). Another thread waits up to CMD_RUN_WAIT_MS for it;
 * after that the command is NOT run and exec_command returns -1 ("busy"), so a handler that waits for
 * the main thread while the main thread execs a command cannot deadlock the game. The lock is a plain
 * owner word (no OS object), so a thread killed at process exit cannot leave a kernel lock behind
 * (exec_command returns at once then anyway). */
#define CMD_RUN_WAIT_MS 5000
static volatile LONG g_cmdrun_owner;   /* thread id holding the lock, 0 = free (thread ids are never 0) */
static LONG g_cmdrun_depth;            /* owner only */

static int cmdrun_enter(void)
{
    LONG me = (LONG)GetCurrentThreadId();
    if (g_cmdrun_owner == me) { g_cmdrun_depth++; return 1; }
    DWORD t0 = GetTickCount();
    for (int spin = 0;; spin++) {
        if (InterlockedCompareExchange(&g_cmdrun_owner, me, 0) == 0) { g_cmdrun_depth = 1; return 1; }
        if (process_exiting() || GetTickCount() - t0 >= CMD_RUN_WAIT_MS) return 0;
        if (spin < 64) YieldProcessor(); else Sleep(1);
    }
}

static void cmdrun_leave(void)
{
    if (--g_cmdrun_depth == 0) InterlockedExchange(&g_cmdrun_owner, 0);
}

/* Runs one console line on the calling thread. The command runs outside g_cmd_lock so it may
 * itself register or exec commands; it runs inside the command-run lock (rule above). Internal entry
 * (console input thread, core); the API entries below add the caller's capability check. */
static int exec_line(const char *line)
{
    if (!line || process_exiting()) return -1;
    while (*line == ' ' || *line == '\t') line++;
    char name[64]; size_t k = 0;
    while (line[k] && line[k] != ' ' && line[k] != '\t' && line[k] != '\r' && line[k] != '\n' && k < sizeof name - 1) {
        name[k] = line[k]; k++;
    }
    name[k] = 0;
    if (!k) return -1;
    const char *args = line + k;
    while (*args == ' ' || *args == '\t') args++;
    char argbuf[2048];
    snprintf(argbuf, sizeof argbuf, "%s", args);
    size_t an = strlen(argbuf);
    while (an && (argbuf[an - 1] == '\r' || argbuf[an - 1] == '\n' || argbuf[an - 1] == ' ' || argbuf[an - 1] == '\t'))
        argbuf[--an] = 0;

    v3x_command_fn fn = NULL; void *user = NULL; HMODULE mod = NULL;
    EnterCriticalSection(&g_cmd_lock);
    for (int i = 0; i < g_cmd_count; i++)
        if (!_stricmp(g_cmds[i].name, name)) { fn = g_cmds[i].fn; user = g_cmds[i].user; mod = g_cmds[i].mod; break; }
    LeaveCriticalSection(&g_cmd_lock);
    if (!fn) { api_print("unknown command '%s' (type 'help')", name); return -1; }
    if (module_disabled(mod)) { api_print("command '%s' is disabled: its plugin faulted (see v3x.log)", name); return -1; }
    if (!cmdrun_enter()) {
        api_print("command '%s' not run: another command is still running (waited %d ms)", name, CMD_RUN_WAIT_MS);
        return -1;
    }
    Fault f;
    int faulted = guard_command_fn(mod, fn, argbuf, user, &f);
    cmdrun_leave();
    if (faulted) on_fault(mod, "console command", name, &f);
    return 0;
}

/* exec_line with the calling thread's print/log output also sent to sink. */
static int exec_capture_line(const char *line, void (*sink)(const char *text, void *user), void *user)
{
    if (!line) return -1;
    if (!sink || g_cap_tls == TLS_OUT_OF_INDEXES) return exec_line(line);
    Capture c = {sink, user};
    DWORD err = GetLastError();
    Capture *prev = (Capture *)TlsGetValue(g_cap_tls);
    TlsSetValue(g_cap_tls, &c);
    SetLastError(err);
    int rc = exec_line(line);
    err = GetLastError();
    TlsSetValue(g_cap_tls, prev);
    SetLastError(err);
    return rc;
}

/* API v2 / v3 entries: a plugin that declares capabilities without "exec" may not run commands. */
static int api_exec_command(const char *line)
{
    if (!caps_allow(caller_owner(__builtin_return_address(0)), CP_CAP_EXEC, "exec_command")) return -1;
    return exec_line(line);
}
static int api_exec_capture(const char *line, void (*sink)(const char *text, void *user), void *user)
{
    if (!caps_allow(caller_owner(__builtin_return_address(0)), CP_CAP_EXEC, "exec_capture")) return -1;
    return exec_capture_line(line, sink, user);
}

/* ------------------------------------------------------------------ events (API v3) */

#define MAX_EVENT_SUBS 64
typedef struct { char name[64]; v3x_event_fn fn; void *user; HMODULE mod; void *owner; LONG id; } Sub;
static Sub g_subs[MAX_EVENT_SUBS];
static int g_sub_count;
static volatile LONG g_sub_gen;   /* bumped by every off_event */
static CRITICAL_SECTION g_event_lock;

/* Copies an event name (1..63 chars) before any lock (audit N8). 1 ok. */
static int event_name_in(char out[64], const char *name) { return name && copy_in(out, 64, name) && out[0]; }

static int on_event_as(void *owner, const char *name, v3x_event_fn fn, void *user)
{
    Sub s;
    memset(&s, 0, sizeof s);
    if (!event_name_in(s.name, name) || !fn || process_exiting()) return -1;
    s.fn = fn; s.user = user; s.mod = module_of((const void *)fn); s.owner = owner;
    s.id = InterlockedIncrement(&g_reg_ids);
    int rc = 0, mine = 0;
    EnterCriticalSection(&g_event_lock);
    for (int i = 0; i < g_sub_count; i++) {
        if (g_subs[i].fn == fn && g_subs[i].user == user && !strcmp(g_subs[i].name, s.name)) { rc = -2; break; }
        mine += g_subs[i].owner == owner;
    }
    int quota = !rc && quota_full((HMODULE)owner, mine, V3X_QUOTA_EVENTS);
    if (!rc && (quota || g_sub_count >= MAX_EVENT_SUBS)) rc = -1;
    if (!rc) g_subs[g_sub_count++] = s;
    LeaveCriticalSection(&g_event_lock);
    if (quota) quota_log((HMODULE)owner, V3X_QUOTA_EVENTS, "on_event");
    return rc;
}

static int api_on_event(const char *name, v3x_event_fn fn, void *user)
{
    return on_event_as(caller_owner(__builtin_return_address(0)), name, fn, user);
}

/* API v6 */
static int api_off_event(const char *name, v3x_event_fn fn, void *user)
{
    void *me = caller_owner(__builtin_return_address(0));
    char nm[64];
    if (!event_name_in(nm, name) || !fn || process_exiting()) return V3X_EARG;
    int rc = V3X_ENOTFOUND;
    EnterCriticalSection(&g_event_lock);
    for (int i = 0; i < g_sub_count; i++) {
        if (g_subs[i].fn != fn || g_subs[i].user != user || strcmp(g_subs[i].name, nm)) continue;
        if (me == OWNER_UNKNOWN || (g_subs[i].owner != me && (void *)g_subs[i].mod != me)) { rc = V3X_EPERM; break; }
        memmove(&g_subs[i], &g_subs[i + 1], sizeof g_subs[0] * (size_t)(g_sub_count - i - 1));
        g_sub_count--;
        InterlockedIncrement(&g_sub_gen);
        rc = 0;
        break;
    }
    LeaveCriticalSection(&g_event_lock);
    return rc;
}

static int sub_alive(LONG id)
{
    int alive = 0;
    EnterCriticalSection(&g_event_lock);
    for (int i = 0; i < g_sub_count && !alive; i++) alive = g_subs[i].id == id;
    LeaveCriticalSection(&g_event_lock);
    return alive;
}

/* Snapshot the matching subscribers under the lock, call them outside it (they may subscribe/emit).
 * A subscriber removed (off_event) after the snapshot is not called. */
static int emit_core_event(const char *name, void *data)
{
    char nm[64];
    if (!event_name_in(nm, name) || process_exiting()) return -1;
    struct { v3x_event_fn fn; void *user; HMODULE mod; LONG id; } hit[MAX_EVENT_SUBS];
    int n = 0, called = 0;
    EnterCriticalSection(&g_event_lock);
    LONG gen = g_sub_gen;
    for (int i = 0; i < g_sub_count; i++)
        if (!strcmp(g_subs[i].name, nm)) {
            hit[n].fn = g_subs[i].fn; hit[n].user = g_subs[i].user; hit[n].mod = g_subs[i].mod; hit[n].id = g_subs[i].id;
            n++;
        }
    LeaveCriticalSection(&g_event_lock);
    for (int i = 0; i < n; i++) {
        if (module_disabled(hit[i].mod)) continue;   /* faulted plugin */
        if (g_sub_gen != gen && !sub_alive(hit[i].id)) continue;
        Fault f;
        called++;
        if (guard_event_fn(hit[i].mod, hit[i].fn, nm, data, hit[i].user, &f)) on_fault(hit[i].mod, "event", nm, &f);
    }
    return called;
}

/* API entry: the core's own events cannot be emitted by plugins (v6). */
static int api_emit_event(const char *name, void *data)
{
    char nm[64];
    if (!event_name_in(nm, name)) return -1;
    if (!strcmp(nm, V3X_EVENT_PROCESS_EXIT) || !strcmp(nm, V3X_EVENT_PLUGIN_DISABLED) || !strcmp(nm, V3X_EVENT_PLUGINS_LOADED))
        return -1;
    return emit_core_event(nm, data);
}

static int api_run_on_main(v3x_frame_fn fn, void *user)
{
    if (!fn || process_exiting()) return -1;
    int rc = -1;
    EnterCriticalSection(&g_mainq_lock);
    if (g_mainq_tail - g_mainq_head < MAIN_QUEUE) {
        g_mainq[g_mainq_tail % MAIN_QUEUE].fn = fn;
        g_mainq[g_mainq_tail % MAIN_QUEUE].user = user;
        g_mainq_tail++;
        InterlockedIncrement(&g_mainq_len);
        rc = 0;
    }
    LeaveCriticalSection(&g_mainq_lock);
    return rc;
}

/* Main thread only. Runs what was queued before this tick; callbacks queued meanwhile wait. */
static void drain_main_queue(void)
{
    LONG n = g_mainq_len;
    for (LONG i = 0; i < n; i++) {
        v3x_frame_fn fn = NULL; void *user = NULL;
        EnterCriticalSection(&g_mainq_lock);
        if (g_mainq_tail != g_mainq_head) {
            fn = g_mainq[g_mainq_head % MAIN_QUEUE].fn;
            user = g_mainq[g_mainq_head % MAIN_QUEUE].user;
            g_mainq_head++;
            InterlockedDecrement(&g_mainq_len);
        }
        LeaveCriticalSection(&g_mainq_lock);
        if (!fn) break;
        HMODULE mod = module_of((const void *)fn);
        if (module_disabled(mod)) continue;
        Fault f;
        if (guard_frame_fn(mod, fn, user, &f)) on_fault(mod, "run_on_main callback", NULL, &f);
    }
}

static void cmd_help(const char *args, void *user)
{
    (void)user;
    int n;
    Cmd *c = cmd_snapshot(&n);   /* the table may shrink (unregister_command, v6) */
    if (!c) { api_print("help: out of memory"); return; }
    if (*args) {
        int i;
        for (i = 0; i < n; i++)
            if (!_stricmp(c[i].name, args)) { api_print("%s  %s", c[i].name, c[i].help); break; }
        if (i == n) api_print("no command '%s'", args);
    } else {
        api_print("%d commands (type a command and press Enter):", n);
        for (int i = 0; i < n; i++) api_print("  %-10s %s", c[i].name, c[i].help);
    }
    HeapFree(GetProcessHeap(), 0, c);
}

static void cmd_plugins(const char *args, void *user)
{
    (void)args; (void)user;
    LONG n = g_plugin_count;
    api_print("%ld plugin file(s):", n);
    for (LONG i = 0; i < n; i++) {
        api_print("  %-20s %-10s %-28s %s", g_plugins[i].name, g_plugins[i].version[0] ? g_plugins[i].version : "-",
                  g_plugins[i].file, g_plugins[i].status);
        if (g_plugins[i].desc[0] || g_plugins[i].author[0])
            api_print("  %-20s %s%s%s", "", g_plugins[i].desc, g_plugins[i].author[0] ? "  -- " : "", g_plugins[i].author);
        if (g_plugins[i].trust[0])
            api_print("  %-20s trust: %s; capabilities: %s", "", g_plugins[i].trust,
                      g_plugins[i].caps_declared ? g_plugins[i].caps_text : "(undeclared: all)");
    }
}

static void cmd_log(const char *args, void *user)
{
    (void)user;
    v3x_log("console", "%s", args);
}

static void cmd_version(const char *args, void *user)
{
    (void)args; (void)user;
    api_print("V3X %s api v%d, game %s (build %s), exe base %p size 0x%zx", V3X_LOADER_SEMVER, V3X_API_VERSION,
              g_version, g_build_id, g_base, g_size);
}

static void cmd_quit(const char *args, void *user)
{
    (void)args; (void)user;
    api_print("V3X never exits the game from this console. Quit from the game's menu; the console\n"
              "window closes with the game (its close button is disabled because closing it would\n"
              "kill the game). Hide it next time with console=0 in v3x/v3x.ini.");
}

/* API v5 `loglevel [tag] [level]`: show or set the threshold of log_at. */
static void cmd_loglevel(const char *args, void *user)
{
    (void)user;
    static const char *names[] = {"off", "error", "warn", "info", "debug", "trace"};
    const char *a = args, *ae = a;
    while (*ae && *ae != ' ' && *ae != '\t') ae++;
    const char *b = ae;
    while (*b == ' ' || *b == '\t') b++;
    size_t an = (size_t)(ae - a), bn = strlen(b);
    if (!an) {
        api_print("log level: %s (global)", names[g_lvl_global]);
        LONG n = g_lvl_count;
        for (LONG i = 0; i < n; i++) api_print("  %-20s %s", g_lvl[i].tag, names[g_lvl[i].level]);
        api_print("usage: loglevel <level> | loglevel <tag> <level>   (off error warn info debug trace)");
        return;
    }
    int lv = bn ? parse_level(b, bn) : parse_level(a, an);
    if (lv < 0) { api_print("not a level: %s (off error warn info debug trace, or 0-5)", bn ? b : a); return; }
    if (!bn) { set_log_level(NULL, 0, lv); api_print("log level: %s (global)", names[lv]); return; }
    if (set_log_level(a, an, lv)) { api_print("loglevel: tag table full or tag too long"); return; }
    api_print("log level of %.*s: %s", (int)an, a, names[lv]);
}

/* API v5 `reload [plugin]`: emits "config_reload" on the main thread (next frame tick). */
static void reload_on_main(void *user)
{
    char *name = (char *)user;
    int n = api_emit_event(V3X_EVENT_CONFIG_RELOAD, name);
    api_print("reload %s: %d handler(s)", name ? name : "(all)", n < 0 ? 0 : n);
    if (name) HeapFree(GetProcessHeap(), 0, name);
}

static void cmd_reload(const char *args, void *user)
{
    (void)user;
    char *name = NULL;
    size_t n = strlen(args);
    if (n) {
        if (n > 63 || !(name = (char *)HeapAlloc(GetProcessHeap(), 0, n + 1))) { api_print("reload: bad name"); return; }
        memcpy(name, args, n + 1);
    }
    if (api_run_on_main(reload_on_main, name)) {
        api_print("reload: main-thread queue full, try again");
        if (name) HeapFree(GetProcessHeap(), 0, name);
    }
}

static void cmd_trust(const char *args, void *user);

static void register_builtins(void)
{
    register_command_as((void *)g_self, "help", "[command]  list commands", cmd_help, NULL);
    register_command_as((void *)g_self, "plugins", "list plugin DLLs and their load status", cmd_plugins, NULL);
    register_command_as((void *)g_self, "log", "<text>  write a line to v3x.log", cmd_log, NULL);
    register_command_as((void *)g_self, "version", "loader API version, game version, exe base", cmd_version, NULL);
    register_command_as((void *)g_self, "quit", "(does not quit) how to close the game", cmd_quit, NULL);
    register_command_as((void *)g_self, "exit", "(does not quit) same as quit", cmd_quit, NULL);
    register_command_as((void *)g_self, "loglevel", "[tag] [level]  show/set the log_at threshold (off error warn info debug trace)", cmd_loglevel, NULL);
    register_command_as((void *)g_self, "trust", "[plugin]  list plugins refused as untrusted / allow one (next start)", cmd_trust, NULL);
    register_command_as((void *)g_self, "reload", "[plugin]  ask plugins to re-read their config (event config_reload)", cmd_reload, NULL);
}

/* Ctrl+C / Ctrl+Break in a console would run the default handler = ExitProcess. Swallow them. */
static BOOL WINAPI console_ctrl(DWORD type)
{
    return type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT;
}

static DWORD WINAPI console_input_thread(void *arg)
{
    (void)arg;
    typedef HRESULT(WINAPI * SetThreadDescription_t)(HANDLE, PCWSTR);
    SetThreadDescription_t std_ = (SetThreadDescription_t)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadDescription");
    if (std_) std_(GetCurrentThread(), L"v3x_console_input");

    HANDLE in = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, 0, NULL);
    if (in == INVALID_HANDLE_VALUE) { LOG("console input unavailable (error %lu)", GetLastError()); return 0; }
    /* Line input with echo. ENABLE_EXTENDED_FLAGS without ENABLE_QUICK_EDIT_MODE turns QuickEdit off:
     * with it on, a stray click starts a selection that blocks every WriteConsole, i.e. any
     * thread that logs (including the game's main thread) would freeze until Esc. */
    SetConsoleMode(in, ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_INSERT_MODE |
                           ENABLE_EXTENDED_FLAGS);
    char buf[2048];
    int failures = 0;
    for (;;) {
        DWORD n = 0;
        if (!ReadConsoleA(in, buf, sizeof buf - 1, &n, NULL)) {
            DWORD e = GetLastError();
            if (e == ERROR_OPERATION_ABORTED) continue; /* Ctrl+C */
            if (++failures > 20) { LOG("console input stopped (error %lu)", e); break; }
            Sleep(200);
            continue;
        }
        failures = 0;
        buf[n] = 0;
        const char *p = buf;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) continue;
        exec_line(p);
    }
    CloseHandle(in);
    return 0;
}

static void start_console_input(void)
{
    SetConsoleCtrlHandler(console_ctrl, TRUE);
    HANDLE t = CreateThread(NULL, 0, console_input_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    api_print("V3X console is interactive: type 'help' and press Enter.");
}

/* ------------------------------------------------------------------ module info */

static uintptr_t api_image_base(void) { return (uintptr_t)g_base; }
static size_t api_image_size(void) { return g_size; }
static const char *api_game_version(void) { return g_version; }
static const char *api_game_dir(void) { return g_game_dir; }
static const char *api_v3x_dir(void) { return g_v3x_dir; }
/* API v6: exact (wide / UTF-8) forms of the folders; set in boot, immutable afterwards */
static char g_game_dir8[V3X_WPATH * 3], g_v3x_dir8[V3X_WPATH * 3];
static const wchar_t *api_game_dir_w(void) { return g_game_dirW; }
static const wchar_t *api_v3x_dir_w(void) { return g_v3x_dirW; }
static const char *api_game_dir_utf8(void) { return g_game_dir8; }
static const char *api_v3x_dir_utf8(void) { return g_v3x_dir8; }
static const char *api_game_build_id(void) { return g_build_id; }

static void read_module_layout(void)
{
    g_base = (uint8_t *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    g_size = nt->OptionalHeader.SizeOfImage;
    snprintf(g_build_id, sizeof g_build_id, "%08lx-%lx", (unsigned long)nt->FileHeader.TimeDateStamp,
             (unsigned long)nt->OptionalHeader.SizeOfImage);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        uint8_t *p = g_base + sec->VirtualAddress;
        size_t n = sec->Misc.VirtualSize;
        if (!memcmp(sec->Name, ".text", 5)) { g_text = p; g_text_size = n; }
        else if ((!memcmp(sec->Name, ".rdata", 6) || !memcmp(sec->Name, ".data", 5)) && g_data_count < 8)
            g_data_ranges[g_data_count++] = (Range){p, n};
    }
}

/* ------------------------------------------------------------------ scanning */

/* Strict since v6 (audit N9, core_parse.h cp_pattern_parse): a bad token or a pattern longer than 256
 * bytes is an error (NULL + WARNING), never silently read as 00 / cut. */
static void *api_pattern_scan(const char *pat)
{
    uint8_t bytes[256]; uint8_t mask[256];
    if (!pat) return NULL;
    size_t pl = strnlen(pat, 4096);
    int n = cp_pattern_parse(pat, pl, bytes, mask, 256);
    if (n < 0 || pl >= 4096) {
        LOG("WARNING: pattern_scan(\"%.60s%s\"): %s; returns NULL", pat, pl > 60 ? "..." : "",
            n == -2 || pl >= 4096 ? "longer than 256 bytes" : "bad token (want 2 hex digits, ? or ?? separated by spaces)");
        return NULL;
    }
    if (!n || !g_text) return NULL;
    for (size_t i = 0; i + n <= g_text_size; i++) {
        if (mask[0] && g_text[i] != bytes[0]) continue;
        int k = 1;
        while (k < n && (!mask[k] || g_text[i + k] == bytes[k])) k++;
        if (k == n) return g_text + i;
    }
    return NULL;
}

static void *api_find_string(const char *s)
{
    if (!s || !*s) return NULL;
    size_t n = strlen(s) + 1; /* include NUL so we match the whole string */
    for (int r = 0; r < g_data_count; r++) {
        uint8_t *p = g_data_ranges[r].ptr; size_t sz = g_data_ranges[r].size;
        for (size_t i = 0; i + n <= sz; i++) {
            if (p[i] == (uint8_t)s[0] && !memcmp(p + i, s, n) && (i == 0 || p[i - 1] == 0))
                return p + i;
        }
    }
    return NULL;
}

/* lea/mov reg, [rip+disp32]: REX.W(48/4C) 8D|8B modrm(mod=00, rm=101) disp32 */
static void *api_find_xref(const void *target, int index)
{
    if (!g_text || !target || index < 0) return NULL;
    const uint8_t *t = (const uint8_t *)target;
    for (size_t i = 0; i + 7 <= g_text_size; i++) {
        uint8_t rex = g_text[i];
        if (rex != 0x48 && rex != 0x4C) continue;
        uint8_t op = g_text[i + 1];
        if (op != 0x8D && op != 0x8B) continue;
        if ((g_text[i + 2] & 0xC7) != 0x05) continue;
        int32_t disp; memcpy(&disp, g_text + i + 3, 4);
        if (g_text + i + 7 + disp == t) {
            if (index-- == 0) return g_text + i;
        }
    }
    return NULL;
}

static void *api_function_start(const void *addr)
{
    DWORD64 image = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)addr, &image, NULL);
    if (!rf) return NULL;
    /* Follow chained unwind info (cold/split chunks) back to the primary function. */
    for (int guard = 0; guard < 32; guard++) {
        uint8_t *ui = (uint8_t *)image + rf->UnwindData;
        uint8_t flags = ui[0] >> 3;
        if (!(flags & UNW_FLAG_CHAININFO)) break;
        uint8_t codes = ui[2];
        size_t off = 4 + ((codes + 1) & ~1u) * 2;
        rf = (PRUNTIME_FUNCTION)(ui + off);
    }
    return (uint8_t *)image + rf->BeginAddress;
}

/* ------------------------------------------------------------------ hooks / patches */

/* MinHook takes a spin lock and suspends threads: never at process exit (MH_ERROR_NOT_INITIALIZED). */
/* g_hook_busy is raised before g_exit_started is read (interlocked, full barriers); the exit
 * detour sets g_exit_started before it reads g_hook_busy. So either this call sees the exit and
 * refuses, or the exit waits for it to finish (and resume the threads MinHook froze). */
static int hook_enter(void)
{
    InterlockedIncrement(&g_hook_busy);
    if (g_exit_started || process_exiting()) { InterlockedDecrement(&g_hook_busy); return 0; }
    return 1;
}

static const char *module_label(void *m, char *buf, size_t n)
{
    if (!m) return "unknown (hooked outside the V3X API)";
    if (m == OWNER_UNKNOWN) return "unknown owner (not a module)";
    if (m == (void *)g_self) return "v3x core";
    if (!strcmp(module_base_name((HMODULE)m, buf, n), "?")) snprintf(buf, n, "module %p", m);
    return buf;
}

static void log_busy(const char *what, void *target, void *me, void *holder)
{
    char a[MAX_PATH], b[MAX_PATH];
    int subs = v3x_hook_subscribers(target);
    LOG("%s %p by %s: V3X_EHOOKBUSY, target held by %s (%s)%s", what, target, module_label(me, a, sizeof a),
        module_label(holder, b, sizeof b), subs < 0 ? "raw hook" : subs ? "hook_safe" : "raw hook outside the registry",
        subs < 0 || !subs ? "; raw hooks are exclusive, use api->hook_safe to share a function" : "");
}

/* hook / hook_safe / unhook may be called only by an identifiable module with the "hook" capability. */
static int hook_caller_ok(void *me, const char *what)
{
    if (me == OWNER_UNKNOWN) {
        static volatile LONG once;
        if (!InterlockedExchange(&once, 1))
            LOG("WARNING: %s called from code outside any module (no plugin callback running on this thread): "
                "refused (V3X_EPERM, unknown owner)", what);
        return 0;
    }
    return caps_allow(me, CP_CAP_HOOK, what);
}

static int api_hook(void *target, void *detour, void **original)
{
    void *me = caller_owner(__builtin_return_address(0)), *holder;
    if (!hook_caller_ok(me, "hook")) return V3X_EPERM;
    if (!hook_enter()) return MH_ERROR_NOT_INITIALIZED;
    int s = v3x_hook_add_raw(me, target, detour, original, &holder);
    InterlockedDecrement(&g_hook_busy);
    if (s == V3X_EHOOKBUSY) log_busy("hook", target, me, holder);
    return s;
}

/* N1: unhook(NULL) is an error (MinHook's MH_DisableHook(NULL) = every hook); a target the registry does
 * not know is MH_ERROR_NOT_CREATED (src/safe_hook.c v3x_hook_remove). */
static int api_unhook(void *target)
{
    void *me = caller_owner(__builtin_return_address(0)), *holder;
    if (!target) return V3X_EARG;
    if (!hook_caller_ok(me, "unhook")) return V3X_EPERM;
    if (!hook_enter()) return MH_ERROR_NOT_INITIALIZED;
    int s = v3x_hook_remove(me, target, &holder);
    InterlockedDecrement(&g_hook_busy);
    if (s == V3X_EHOOKBUSY) log_busy("unhook", target, me, holder);
    return s;
}

/* API v4: shared pass-through thunk around the target (src/safe_hook.c); same exit guard as api_hook. */
static int api_hook_safe(void *target, v3x_hook_fn pre, v3x_hook_fn post, void *user)
{
    if (!target || (!pre && !post)) return -1;
    void *me = caller_owner(__builtin_return_address(0)), *holder;
    if (!hook_caller_ok(me, "hook_safe")) return V3X_EPERM;
    if (!hook_enter()) return MH_ERROR_NOT_INITIALIZED;
    int s = v3x_hook_add_safe(me, target, pre, post, user, &holder);
    InterlockedDecrement(&g_hook_busy);
    if (s == V3X_EHOOKBUSY) log_busy("hook_safe", target, me, holder);
    else if (s == 0 && v3x_hook_subscribers(target) > 1) {
        char a[MAX_PATH];
        LOG("hook_safe %p by %s: shared target, %d subscribers", target, module_label(me, a, sizeof a),
            v3x_hook_subscribers(target));
    }
    return s;
}

/* The core's own hooks (entry, PeekMessageW, RtlExitUserProcess) go through the registry as owner g_self,
 * so no plugin can remove or replace them (audit N1). */
static int core_hook(void *target, void *detour, void **original)
{
    void *holder;
    return v3x_hook_add_raw((void *)g_self, target, detour, original, &holder);
}

/* ---- patches (audit N3) ------------------------------------------------------------------------------
 * Writing code that other threads may execute: the other threads are suspended while the bytes change,
 * with MinHook's scheme (Toolhelp32 thread snapshot, SuspendThread each, ResumeThread after), and under
 * the hook registry's lock, so a patch and a MinHook enable/disable never suspend each other. Nothing
 * that could take a lock another thread holds (heap, log, loader) runs while threads are suspended:
 * memory is allocated and thread handles are opened before, logging happens after. */
typedef struct { HANDLE *h; DWORD n; } Frozen;

static void freeze_open(Frozen *fz)
{
    fz->h = NULL; fz->n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId(), cap = 0;
    THREADENTRY32 te;
    for (int pass = 0; pass < 2; pass++) {   /* pass 0 counts, pass 1 opens */
        if (pass == 1 && (!cap || !(fz->h = (HANDLE *)HeapAlloc(GetProcessHeap(), 0, sizeof(HANDLE) * cap)))) break;
        te.dwSize = sizeof te;
        if (!Thread32First(snap, &te)) break;
        do {
            if (te.dwSize >= offsetof(THREADENTRY32, th32OwnerProcessID) + sizeof(DWORD) && te.th32OwnerProcessID == pid &&
                te.th32ThreadID != me) {
                if (pass == 0) cap++;
                else if (fz->n < cap) {
                    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                    if (t) fz->h[fz->n++] = t;
                }
            }
            te.dwSize = sizeof te;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}
static void freeze_all(Frozen *fz) { for (DWORD i = 0; i < fz->n; i++) SuspendThread(fz->h[i]); }
static void thaw_all(Frozen *fz) { for (DWORD i = 0; i < fz->n; i++) ResumeThread(fz->h[i]); }
static void freeze_close(Frozen *fz)
{
    for (DWORD i = 0; i < fz->n; i++) CloseHandle(fz->h[i]);
    if (fz->h) HeapFree(GetProcessHeap(), 0, fz->h);
    fz->h = NULL; fz->n = 0;
}

#define PATCH_MAX_PAGES (V3X_PATCH_MAX / 4096 + 2)
static DWORD prot_exec(DWORD p) { return (p & 0xff) & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY); }
static DWORD prot_write(DWORD p) { return (p & 0xff) & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY); }

/* Every page of [addr, addr+n) committed and accessible? Fills pg/np. 0 or ERROR_INVALID_ADDRESS. */
typedef struct { uint8_t *page; DWORD old; int changed; } PatchPage;
static int patch_pages(uint8_t *addr, size_t n, PatchPage *pg, int *np, uintptr_t *ps_out)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t ps = si.dwPageSize ? si.dwPageSize : 4096, a = (uintptr_t)addr & ~(ps - 1), end = (uintptr_t)addr + n;
    *np = 0;
    *ps_out = ps;
    for (uintptr_t p = a; p < end; p += ps) {
        MEMORY_BASIC_INFORMATION mbi;
        if (*np >= PATCH_MAX_PAGES) return ERROR_INVALID_PARAMETER;
        if (!VirtualQuery((void *)p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || !mbi.Protect ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return ERROR_INVALID_ADDRESS;
        pg[*np].page = (uint8_t *)p; pg[*np].old = mbi.Protect; pg[*np].changed = 0;
        (*np)++;
    }
    return 0;
}

/* Copies n bytes to addr with the page rules above; orig (n bytes, may be NULL) receives the replaced
 * bytes. The caller holds v3x_hook_lock. 0 or a Win32 error (nothing written then). */
static int patch_write_locked(uint8_t *addr, const void *bytes, size_t n, uint8_t *orig)
{
    PatchPage pg[PATCH_MAX_PAGES];
    int np, rc;
    uintptr_t ps;
    if ((rc = patch_pages(addr, n, pg, &np, &ps)) != 0) return rc;
    Frozen fz;
    freeze_open(&fz);   /* before suspending anyone: heap + handles */
    freeze_all(&fz);
    for (int i = 0; i < np && !rc; i++) {
        DWORD old = pg[i].old, was;
        if (prot_write(old)) continue;   /* already writable: left as it is */
        DWORD want = (prot_exec(old) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE) | (old & (PAGE_NOCACHE | PAGE_WRITECOMBINE));
        if (!VirtualProtect(pg[i].page, ps, want, &was)) rc = (int)GetLastError();
        else pg[i].changed = 1;
    }
    if (!rc) {
        if (orig) memcpy(orig, addr, n);
        memcpy(addr, bytes, n);
    }
    int exec = 0;
    for (int i = 0; i < np; i++) {
        DWORD was;
        exec |= prot_exec(pg[i].old) != 0;
        if (pg[i].changed) VirtualProtect(pg[i].page, ps, pg[i].old, &was);   /* back to the original rights */
    }
    if (!rc && exec) FlushInstructionCache(GetCurrentProcess(), addr, n);
    thaw_all(&fz);
    freeze_close(&fz);
    return rc;
}

/* Patches are recorded per calling module with the bytes they replaced, so a plugin whose
 * v3x_plugin_load fails can be rolled back (rollback_module). Table full = refused (v6), never untracked. */
#define MAX_PATCHES 512
static struct { HMODULE owner; uint8_t *addr; size_t n; uint8_t *orig; } g_patches[MAX_PATCHES];
static int g_patch_count;
static CRITICAL_SECTION g_patch_lock;

static int image_overlaps(HMODULE m, const uint8_t *p, size_t n)
{
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const uint8_t *)m + ((const IMAGE_DOS_HEADER *)m)->e_lfanew);
    uintptr_t s = (uintptr_t)m, e = s + nt->OptionalHeader.SizeOfImage;
    return (uintptr_t)p < e && s < (uintptr_t)p + n;
}

/* Why [p, p+n) may not be patched by `me` (NULL = allowed): the loader's image (code, API table), the
 * hook stub pool, other plugins' modules, another owner's hook bytes. With v3x_hook_lock held. */
static const char *patch_refused_locked(void *me, const uint8_t *p, size_t n)
{
    if (image_overlaps(g_self, p, n)) return "memory of the V3X loader";
    if (v3x_hook_pool_overlaps_locked(p, n)) return "the hook stub pool";
    if (v3x_hook_conflict_locked(p, n, me)) return "the hook bytes of a function hooked by another module or the core";
    LONG pc = g_plugin_count;
    for (LONG i = 0; i < pc && i < MAX_PLUGINS; i++) {
        HMODULE m = g_plugins[i].mod;
        if (m && (void *)m != me && image_overlaps(m, p, n)) return "another plugin's module";
    }
    return NULL;
}

static int api_patch(void *addr, const void *bytes, size_t n)
{
    void *me = caller_owner(__builtin_return_address(0));
    if (!addr || !bytes || !n || n > V3X_PATCH_MAX || (uintptr_t)addr + n < (uintptr_t)addr) return ERROR_INVALID_PARAMETER;
    if (process_exiting()) return ERROR_PROCESS_ABORTED;
    if (me == OWNER_UNKNOWN) {
        LOG("WARNING: patch %p (%zu bytes) from code outside any module: refused (unknown owner)", addr, n);
        return ERROR_ACCESS_DENIED;
    }
    if (!caps_allow(me, CP_CAP_PATCH, "patch")) return ERROR_ACCESS_DENIED;
    /* audit N8: the caller's bytes are copied before any lock; a bad pointer faults here, in its context */
    uint8_t *src = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, n), *orig = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, n);
    if (!src || !orig) {
        if (src) HeapFree(GetProcessHeap(), 0, src);
        if (orig) HeapFree(GetProcessHeap(), 0, orig);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    memcpy(src, bytes, n);
    int rc;
    const char *why = NULL;
    if (!hook_enter()) rc = ERROR_PROCESS_ABORTED;
    else {
        EnterCriticalSection(&g_patch_lock);   /* order: patch lock, then the registry lock (rollback too) */
        if (g_patch_count >= MAX_PATCHES) rc = ERROR_NOT_ENOUGH_MEMORY;
        else {
            v3x_hook_lock();
            why = patch_refused_locked(me, (const uint8_t *)addr, n);
            rc = why ? ERROR_ACCESS_DENIED : patch_write_locked((uint8_t *)addr, src, n, orig);
            v3x_hook_unlock();
            if (rc == 0) {
                g_patches[g_patch_count].owner = (HMODULE)me; g_patches[g_patch_count].addr = (uint8_t *)addr;
                g_patches[g_patch_count].n = n; g_patches[g_patch_count].orig = orig;
                g_patch_count++;
                orig = NULL;
            }
        }
        LeaveCriticalSection(&g_patch_lock);
        InterlockedDecrement(&g_hook_busy);
    }
    if (rc && rc != ERROR_PROCESS_ABORTED) {
        char a[MAX_PATH];
        LOG("WARNING: patch %p (%zu bytes) by %s refused: %s", addr, n, module_label(me, a, sizeof a),
            why ? why : rc == ERROR_NOT_ENOUGH_MEMORY ? "patch table full (512 live patches)" :
            rc == ERROR_INVALID_ADDRESS ? "not committed / not accessible memory" : "VirtualProtect failed");
    }
    HeapFree(GetProcessHeap(), 0, src);
    if (orig) HeapFree(GetProcessHeap(), 0, orig);
    return rc;
}

/* Undoes what module m installed: its hooks (raw + hook_safe, src/safe_hook.c: DISABLED only, the
 * trampolines are never freed, docs/adr/0003-hook-lifetime.md) and its patches (newest first). Only
 * called on the main thread at load time, right after m's v3x_plugin_load failed. */
static void rollback_module(HMODULE m, const char *name)
{
    int hooks = 0, patches = 0;
    if (hook_enter()) {
        hooks = v3x_hook_remove_owner((void *)m);
        EnterCriticalSection(&g_patch_lock);
        for (int i = g_patch_count - 1; i >= 0; i--) {
            if (g_patches[i].owner != m || !g_patches[i].orig) continue;
            v3x_hook_lock();
            if (patch_write_locked(g_patches[i].addr, g_patches[i].orig, g_patches[i].n, NULL) == 0) patches++;
            v3x_hook_unlock();
            HeapFree(GetProcessHeap(), 0, g_patches[i].orig);
            g_patches[i].orig = NULL;
        }
        int k = 0;   /* rolled-back records give their slots back */
        for (int i = 0; i < g_patch_count; i++)
            if (g_patches[i].orig) g_patches[k++] = g_patches[i];
        g_patch_count = k;
        LeaveCriticalSection(&g_patch_lock);
        InterlockedDecrement(&g_hook_busy);
    }
    LOG("plugin '%s' rolled back: %d hook target(s) released, %d patch(es) restored, callbacks disabled", name, hooks, patches);
}

/* Any thread (audit M3): entries are written under g_frame_lock (exclusive); the tick snapshots them
 * under the shared lock. Quota V3X_QUOTA_FRAME per plugin module (v6). */
static int api_on_frame(v3x_frame_fn fn, void *user)
{
    if (!fn || process_exiting()) return -1;
    void *me = caller_owner(__builtin_return_address(0));
    HMODULE mod = module_of((const void *)fn);   /* outside the lock (takes the loader lock) */
    LONG id = InterlockedIncrement(&g_reg_ids);
    int rc = -1, mine = 0;
    AcquireSRWLockExclusive(&g_frame_lock);
    for (LONG k = 0; k < g_frame_count; k++) mine += g_frame_cbs[k].owner == me;
    int quota = quota_full((HMODULE)me, mine, V3X_QUOTA_FRAME);
    LONG i = g_frame_count;
    if (!quota && i < MAX_FRAME_CBS) {
        g_frame_cbs[i].fn = fn; g_frame_cbs[i].user = user; g_frame_cbs[i].mod = mod; g_frame_cbs[i].owner = me;
        g_frame_cbs[i].id = id;
        g_frame_count = i + 1;
        rc = 0;
    }
    ReleaseSRWLockExclusive(&g_frame_lock);
    if (quota) quota_log((HMODULE)me, V3X_QUOTA_FRAME, "on_frame");
    return rc;
}

/* API v6 */
static int api_off_frame(v3x_frame_fn fn, void *user)
{
    void *me = caller_owner(__builtin_return_address(0));
    if (!fn || process_exiting()) return V3X_EARG;
    int rc = V3X_ENOTFOUND;
    AcquireSRWLockExclusive(&g_frame_lock);
    for (LONG i = 0; i < g_frame_count; i++) {
        if (g_frame_cbs[i].fn != fn || g_frame_cbs[i].user != user) continue;
        if (me == OWNER_UNKNOWN || (g_frame_cbs[i].owner != me && (void *)g_frame_cbs[i].mod != me)) { rc = V3X_EPERM; break; }
        memmove(&g_frame_cbs[i], &g_frame_cbs[i + 1], sizeof g_frame_cbs[0] * (size_t)(g_frame_count - i - 1));
        g_frame_count--;
        InterlockedIncrement(&g_frame_gen);
        rc = 0;
        break;
    }
    ReleaseSRWLockExclusive(&g_frame_lock);
    return rc;
}

static int frame_alive(LONG id)
{
    int alive = 0;
    AcquireSRWLockShared(&g_frame_lock);
    for (LONG i = 0; i < g_frame_count && !alive; i++) alive = g_frame_cbs[i].id == id;
    ReleaseSRWLockShared(&g_frame_lock);
    return alive;
}

typedef BOOL(WINAPI *PeekMessageW_t)(LPMSG, HWND, UINT, UINT, UINT);
static PeekMessageW_t g_PeekMessageW;
static DWORD g_main_thread;

static uint32_t api_main_thread_id(void) { return g_main_thread; }

/* API v4: every registered command, in order, called outside g_cmd_lock on a snapshot (the table may
 * shrink since v6), inside the fault guard (v6). */
static int api_enum_commands(void (*fn)(const char *name, const char *help, void *user), void *user)
{
    if (!fn || process_exiting()) return -1;
    int n;
    Cmd *c = cmd_snapshot(&n);
    if (!c) return -1;
    HMODULE mod = module_of((const void *)fn);
    for (int i = 0; i < n; i++) {
        Fault f;
        void *prev = ctx_enter(mod);
        int bad = guard_enum_fn_(fn, c[i].name, c[i].help, user, &f);
        ctx_leave(prev);
        if (bad) { on_fault(mod, "enum_commands callback", NULL, &f); break; }
    }
    HeapFree(GetProcessHeap(), 0, c);
    return n;
}

static BOOL WINAPI hk_PeekMessageW(LPMSG msg, HWND wnd, UINT lo, UINT hi, UINT rm)
{
    if (GetCurrentThreadId() == g_main_thread) {
        FrameCb snap[MAX_FRAME_CBS];
        AcquireSRWLockShared(&g_frame_lock);
        LONG n = g_frame_count, gen = g_frame_gen;
        memcpy(snap, g_frame_cbs, sizeof snap[0] * (size_t)n);
        ReleaseSRWLockShared(&g_frame_lock);
        for (LONG i = 0; i < n; i++) {
            if (module_disabled(snap[i].mod)) continue;   /* faulted plugin: skipped */
            if (g_frame_gen != gen && !frame_alive(snap[i].id)) continue;   /* removed by off_frame meanwhile */
            Fault f;
            if (guard_frame_fn(snap[i].mod, snap[i].fn, snap[i].user, &f)) on_fault(snap[i].mod, "on_frame callback", NULL, &f);
        }
        if (g_mainq_len) drain_main_queue();
    }
    return g_PeekMessageW(msg, wnd, lo, hi, rm);
}

/* ------------------------------------------------------------------ API table */

__declspec(dllexport) const uint32_t v3x_loader_api = V3X_API_VERSION;
__declspec(dllexport) const char v3x_loader_semver[] = V3X_LOADER_SEMVER;
#define V3X_KNOWN_BUILDS "6af3094f-60fb000"   /* release/1.15.1; the unknown-build gate, see build_gate_active */
__declspec(dllexport) const char v3x_loader_known_builds[] = V3X_KNOWN_BUILDS;
static const char *api_loader_version(void) { return v3x_loader_semver; }
static int build_matches(const char *list);

static const V3X_API g_api = {
    V3X_API_VERSION, sizeof(V3X_API),
    v3x_log,
    api_image_base, api_image_size, api_game_version,
    api_pattern_scan, api_find_string, api_find_xref, api_function_start,
    api_hook, api_unhook, api_patch,
    api_on_frame,
    api_game_dir, api_v3x_dir,
    /* v2 */
    api_register_command, api_print, api_run_on_main, api_exec_command,
    /* v3 */
    api_exec_capture, api_on_event, api_emit_event,
    /* v4 */
    api_hook_safe, api_main_thread_id, api_game_build_id, api_enum_commands,
    /* v5 */
    api_log_at, api_log_level, api_loader_version, build_matches,
    /* v6 */
    api_off_frame, api_off_event, api_unregister_command, api_game_dir_w, api_v3x_dir_w, api_game_dir_utf8,
    api_v3x_dir_utf8,
};

/* For tests/tools that are not plugins: GetProcAddress(GetModuleHandleA("version.dll"), "v3x_api"). */
__declspec(dllexport) const V3X_API *v3x_api(void) { return &g_api; }

static int plugin_record(const char *name, const char *file, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int plugin_record(const char *name, const char *file, const char *fmt, ...)
{
    if (g_plugin_count >= MAX_PLUGINS) return -1;
    int i = (int)g_plugin_count;   /* main thread only (load_plugins); fields first, then the count */
    snprintf(g_plugins[i].name, sizeof g_plugins[i].name, "%s", name);
    snprintf(g_plugins[i].file, sizeof g_plugins[i].file, "%s", file);
    snprintf(g_plugins[i].stem, sizeof g_plugins[i].stem, "%s", file);
    char *dot = strrchr(g_plugins[i].stem, '.');
    if (dot && !_stricmp(dot, ".dll")) *dot = 0;
    g_plugins[i].caps = CP_CAP_ALL;
    va_list ap; va_start(ap, fmt); vsnprintf(g_plugins[i].status, sizeof g_plugins[i].status, fmt, ap); va_end(ap);
    InterlockedIncrement(&g_plugin_count);
    return i;
}

/* 1 if the running game matches one entry of a manifest's game_builds list (';'-separated;
 * game_version() or game_build_id(), case-insensitive; a trailing '*' matches a prefix;
 * NULL / "" / "*" match everything). */
static int build_matches_ex(const char *list, int strict);
static int build_matches(const char *list) { return build_matches_ex(list, 0); }

/* strict (the unknown-build gate): an exact game_version entry does not count, because a hotfix keeps
 * the branch name; only '*', prefix patterns and the exact build id do. NULL / "" match nothing. */
static int build_matches_ex(const char *list, int strict)
{
    if (!list || !*list) return !strict;
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == ';') p++;
        const char *e = p;
        while (*e && *e != ';') e++;
        size_t n = (size_t)(e - p);
        while (n && p[n - 1] == ' ') n--;
        if (n) {
            int prefix = p[n - 1] == '*';
            size_t k = prefix ? n - 1 : n;
            const char *ids[2] = {g_version, g_build_id};
            for (int j = strict && !prefix ? 1 : 0; j < 2; j++) {
                if (prefix ? !_strnicmp(ids[j], p, k) : (strlen(ids[j]) == k && !_strnicmp(ids[j], p, k))) return 1;
            }
        }
        p = e;
    }
    return 0;
}

/* Decides from the manifest read out of the FILE (src/v3x_pe.h, before LoadLibrary, audit N4) whether the
 * plugin may load. Returns 0 = load, otherwise the refusal reason is in why. */
static int manifest_check(const V3xPePlugin *pe, const char *name, char *why, size_t why_n, int gate)
{
    if (!pe->has_manifest) {
        if (!gate) return 0;
        snprintf(why, why_n, "skipped: unknown game build, no manifest (unknown_build = load to force)");
        return 1;
    }
    if (pe->mf_min_api > V3X_API_VERSION) {
        snprintf(why, why_n, "refused: needs loader API v%u (this is v%d)", pe->mf_min_api, V3X_API_VERSION);
        return 1;
    }
    if (gate && !build_matches_ex(pe->builds, 1)) {
        snprintf(why, why_n, "skipped: unknown game build %s not declared (unknown_build = load to force)", g_build_id);
        return 1;
    }
    if (!build_matches(pe->builds)) {
        if (pe->mf_flags & V3X_MANIFEST_REQUIRE_BUILD) {
            snprintf(why, why_n, "refused: game %s / %s not in its builds", g_version, g_build_id);
            return 1;
        }
        LOG("WARNING: plugin '%s' lists game builds \"%s\"; running %s (build %s). Loading anyway.", name,
            pe->builds, g_version, g_build_id);
    }
    return 0;
}

/* ------------------------------------------------------------------ plugin loading */

#include "core_ini.h"   /* ini_ieq, ini_each (rules there) */

/* v3x/v3x.ini as text (empty if missing; at most 64 KB). */
static void read_v3x_ini(char *buf, size_t cap)
{
    buf[0] = 0;
    FILE *f = v3x_fopen("v3x.ini", L"rb");
    if (!f) return;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
}

/* API v5: [log] section of v3x.ini: "level = debug" (global) and "<tag> = <level>". Env
 * V3X_LOG_LEVEL is the global default when the ini does not set one. Unknown values are logged. */
static void log_kv(const char *k, size_t kn, const char *v, size_t vn, void *ctx)
{
    (void)ctx;
    int lv = parse_level(v, vn);
    if (lv < 0) { LOG("WARNING: v3x.ini [log] %.*s = %.*s: not a level (off/error/warn/info/debug/trace/0-5)", (int)kn, k, (int)vn, v); return; }
    if (ini_ieq(k, kn, "level")) set_log_level(NULL, 0, lv);
    else if (set_log_level(k, kn, lv)) LOG("WARNING: v3x.ini [log] %.*s: too many tags or tag too long", (int)kn, k);
}

static void log_config_load(void)
{
    static char ini[65536];
    char env[16];
    DWORD n = GetEnvironmentVariableA("V3X_LOG_LEVEL", env, sizeof env);
    if (n && n < sizeof env) {
        int lv = parse_level(env, n);
        if (lv >= 0) set_log_level(NULL, 0, lv);
    }
    read_v3x_ini(ini, sizeof ini);
    ini_each(ini, "log", log_kv, NULL);
}

/* ---- crash-loop safe mode (loader 0.5.0)
 * <v3x>/loading.marker holds the file stem of the plugin being loaded: written before LoadLibrary
 * (its DllMain may crash too), deleted when v3x_plugin_load returned. If the game dies inside a
 * plugin's load, the marker survives; on the next start that plugin is skipped ONCE (and the marker
 * removed, so the start after that tries it again). */
static int marker_path(wchar_t *out, size_t n) { return wpath(out, n, g_v3x_dirW, "loading.marker"); }

static void marker_set(const char *stem)
{
    wchar_t p[V3X_WPATH];
    if (marker_path(p, V3X_WPATH)) return;
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(h, stem, (DWORD)strlen(stem), &w, NULL);
    CloseHandle(h);
}

static void marker_clear(void)
{
    wchar_t p[V3X_WPATH];
    if (!marker_path(p, V3X_WPATH)) DeleteFileW(p);
}

/* The stem left by a load that never returned last start ("" = none); removes the marker. */
static void marker_take(char *out, size_t n)
{
    wchar_t p[V3X_WPATH];
    out[0] = 0;
    if (marker_path(p, V3X_WPATH)) return;
    FILE *f = _wfopen(p, L"rb");
    if (!f) return;
    size_t k = fread(out, 1, n - 1, f);
    fclose(f);
    out[k] = 0;
    while (k && (out[k - 1] == '\r' || out[k - 1] == '\n' || out[k - 1] == ' ')) out[--k] = 0;
    DeleteFileW(p);
}

/* Read-only: the last "UNHANDLED EXCEPTION" line crash_logger wrote last session (crash.log is still
 * last session's file here, crash_logger rotates it when it loads). Logged so the player and the
 * `plugins` list can see which module crashed. */
static char g_last_crash[200];
static void read_last_crash(void)
{
    FILE *f = v3x_fopen("crash.log", L"rb");
    if (!f) return;
    static char buf[262144];
    if (fseek(f, 0, SEEK_END) == 0) {
        long sz = ftell(f);
        fseek(f, sz > (long)sizeof buf - 1 ? sz - (long)(sizeof buf - 1) : 0, SEEK_SET);
    }
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *hit = NULL;
    for (const char *q = buf; (q = strstr(q, "UNHANDLED EXCEPTION")); q++) hit = q;
    if (!hit) return;
    size_t k = 0;
    while (hit[k] && hit[k] != '\n' && hit[k] != '\r' && k < sizeof g_last_crash - 1) k++;
    memcpy(g_last_crash, hit, k);
    g_last_crash[k] = 0;
    LOG("WARNING: last session crashed (crash.log): %s", g_last_crash);
}

/* ---- unknown-build gate (loader 0.5.0)
 * The loader knows the builds it was tested on. On any other build of victoria3.exe (a game patch or
 * hotfix) the default `unknown_build = safe` loads only plugins whose manifest declares that build
 * by '*', a prefix ("release/1.15*") or the exact build id; everything else is skipped (anchors that
 * fail closed are the second line of defence, this is the first). `unknown_build = load` (top of
 * v3x.ini, or env V3X_UNKNOWN_BUILD=load) loads everything as before. Active only when the host exe
 * is victoria3.exe, or when env V3X_BUILD_GATE=1 (tests). */
/* V3X_KNOWN_BUILDS: next to the API table (also exported as v3x_loader_known_builds) */

struct gate_cfg { int load; };
static void gate_kv(const char *k, size_t kn, const char *v, size_t vn, void *ctx)
{
    if (ini_ieq(k, kn, "unknown_build")) ((struct gate_cfg *)ctx)->load = ini_ieq(v, vn, "load");
}

/* 1 = the gate applies to this start (unknown build, safe mode). */
static int build_gate_active(const char *ini)
{
    wchar_t exe[V3X_WPATH];
    char env[16];
    DWORD en = GetModuleFileNameW(NULL, exe, V3X_WPATH);
    if (en >= V3X_WPATH) en = 0;
    exe[en] = 0;
    const wchar_t *b = wcsrchr(exe, L'\\');
    b = b ? b + 1 : exe;
    int forced = GetEnvironmentVariableA("V3X_BUILD_GATE", env, sizeof env) && env[0] == '1';
    if (!forced && _wcsicmp(b, L"victoria3.exe")) return 0;
    if (build_matches_ex(V3X_KNOWN_BUILDS, 1)) return 0;
    struct gate_cfg g = {0};
    ini_each(ini, "", gate_kv, &g);
    DWORD n = GetEnvironmentVariableA("V3X_UNKNOWN_BUILD", env, sizeof env);
    if (n && n < sizeof env) g.load = !_stricmp(env, "load");
    if (g.load) {
        LOG("WARNING: game build %s (%s) is not one this loader was tested on (%s); unknown_build = load: "
            "loading every plugin anyway", g_build_id, g_version, V3X_KNOWN_BUILDS);
        return 0;
    }
    LOG("WARNING: game build %s (%s) is not one this loader was tested on (%s). Safe mode: only plugins whose "
        "manifest declares this build are loaded. Set unknown_build = load in v3x.ini to load all.",
        g_build_id, g_version, V3X_KNOWN_BUILDS);
    if (g_con) api_print("V3X: the game was updated (build %s). Plugins not marked compatible are skipped until "
                         "they are updated (unknown_build = load in v3x/v3x.ini loads them anyway).", g_build_id);
    return 1;
}

/* ---- plugin trust (docs/adr/0002-trust-model.md, loader 0.6.0)
 * A plugin DLL is loaded only if its SHA-256 (of the bytes read through the handle that stays open,
 * without write/delete sharing, until LoadLibrary returned) is
 *   1. listed in binaries/v3x/plugins.json for its file name, and plugins.json.sig is a valid release
 *      signature over it (installer/release_sig.h, the keys of installer/release_keys.h) = "official", or
 *   2. in the per-user allowlist %LOCALAPPDATA%\V3X\trusted_plugins.txt = "trusted" (the player said so
 *      with the console command `trust <plugin>`), or
 *   3. the check is off: env V3X_TRUST=dev (developers, tests; no v3x.ini key on purpose).
 * Anything else is NOT loaded (never mapped, so none of its code runs) and is listed by `trust`. Honest
 * limit: this is authenticity + consent, not a sandbox; a trusted plugin is native code with the game's
 * rights, and DLLs that a plugin imports itself are not checked. */
static int g_trust_dev;
static char *g_official;           /* verified plugins.json text (NUL-terminated) or NULL */
static size_t g_official_n;
static char *g_allow;              /* trusted_plugins.txt text or NULL */
static size_t g_allow_n;
static wchar_t g_allow_dirW[V3X_WPATH], g_allow_pathW[V3X_WPATH];
static int g_untrusted_count;

/* Whole file (at most max bytes, else NULL) into a NUL-terminated heap buffer. */
static char *read_file_w(const wchar_t *path, size_t max, size_t *n)
{
    *n = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    char *b = NULL;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart >= 0 && (unsigned long long)sz.QuadPart <= max &&
        (b = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)sz.QuadPart + 1))) {
        DWORD got = 0;
        size_t want = (size_t)sz.QuadPart;
        if (want && (!ReadFile(h, b, (DWORD)want, &got, NULL) || got != want)) { HeapFree(GetProcessHeap(), 0, b); b = NULL; }
        else { b[want] = 0; *n = want; }
    }
    CloseHandle(h);
    return b;
}

/* Dev trust comes only from the environment (V3X_TRUST=dev) or the per-user allowlist; there is deliberately
 * NO v3x.ini key for it (a file next to the game, which any program can write, must not switch the check off). */
static void trust_init(void)
{
    char env[16];
    DWORD en = GetEnvironmentVariableA("V3X_TRUST", env, sizeof env);
    g_trust_dev = en && en < sizeof env && !_stricmp(env, "dev");
    if (g_trust_dev)
        LOG("WARNING: trust = dev (env V3X_TRUST): plugin DLLs are loaded WITHOUT the signature / "
            "allowlist check. For plugin development only.");

    wchar_t p[V3X_WPATH], ps[V3X_WPATH];
    size_t jn = 0, sn = 0;
    char *json = !wpath(p, V3X_WPATH, g_v3x_dirW, "plugins.json") ? read_file_w(p, 1u << 20, &jn) : NULL;
    char *sig = !wpath(ps, V3X_WPATH, g_v3x_dirW, "plugins.json.sig") ? read_file_w(ps, 4096, &sn) : NULL;
    if (json) {
        V3xSigResult r;
        if (!sig) LOG("WARNING: plugins.json has no plugins.json.sig: the official plugin list is ignored");
        else if (!v3x_release_verify(json, jn, sig, sn, &r))
            LOG("WARNING: plugins.json.sig does not verify (%s, key %s): the official plugin list is ignored", r.why,
                r.key_id[0] ? r.key_id : "?");
        else {
            g_official = json; g_official_n = jn; json = NULL;
            LOG("official plugin list: plugins.json signed by release key %s%s", r.key_id,
                r.slot == V3X_SIG_SLOT_DEVTEST ? " (DEV TEST KEY)" : "");
        }
    }
    if (json) HeapFree(GetProcessHeap(), 0, json);
    if (sig) HeapFree(GetProcessHeap(), 0, sig);

    DWORD ln = GetEnvironmentVariableW(L"LOCALAPPDATA", g_allow_dirW, V3X_WPATH - 40);
    if (ln && ln < V3X_WPATH - 40) {
        wmemcpy(g_allow_dirW + ln, L"\\V3X", 5);
        if (!wpath(g_allow_pathW, V3X_WPATH, g_allow_dirW, "\\trusted_plugins.txt")) g_allow = read_file_w(g_allow_pathW, 1u << 20, &g_allow_n);
    } else {
        g_allow_dirW[0] = 0;
    }
}

/* "official" / "trusted (allowlist)" / "dev (unchecked)" or NULL = not trusted */
static const char *trust_of(const char *file, const char *sha)
{
    if (g_official && cp_official_has(g_official, g_official_n, file, sha)) return "official";
    if (g_allow && cp_allow_has(g_allow, g_allow_n, sha)) return "trusted (allowlist)";
    if (g_trust_dev) return "dev (unchecked)";
    return NULL;
}

/* `trust [plugin]`: list the plugins refused as untrusted this start, or add one to the allowlist (its
 * hash as refused at start: what is trusted is exactly the file that was checked). */
static void cmd_trust(const char *args, void *user)
{
    (void)user;
    LONG n = g_plugin_count;
    if (!*args) {
        api_print("%d plugin(s) not loaded because they are not trusted:", g_untrusted_count);
        for (LONG i = 0; i < n; i++)
            if (g_plugins[i].untrusted) api_print("  %-20s sha256 %s", g_plugins[i].stem, g_plugins[i].sha);
        api_print("usage: trust <plugin>  adds it to %s (takes effect at the next start). Trust only plugins whose "
                  "author you trust: a plugin is a program with the game's rights.", g_allow_pathW[0] ? "your trusted list" : "(no %LOCALAPPDATA%)");
        return;
    }
    LONG i;
    for (i = 0; i < n; i++)
        if (!_stricmp(g_plugins[i].stem, args) || !_stricmp(g_plugins[i].file, args)) break;
    if (i == n || !g_plugins[i].untrusted) {
        api_print("trust: '%s' was not refused as untrusted this start (type 'trust' for the list)", args);
        return;
    }
    if (!g_allow_pathW[0]) { api_print("trust: %%LOCALAPPDATA%% is not set, cannot store the trusted list"); return; }
    CreateDirectoryW(g_allow_dirW, NULL);
    FILE *f = _wfopen(g_allow_pathW, L"ab");
    if (!f) { api_print("trust: cannot write the trusted list (error %lu)", GetLastError()); return; }
    int ok = fprintf(f, "%s %s\r\n", g_plugins[i].sha, g_plugins[i].file) > 0;
    ok &= fclose(f) == 0;
    if (!ok) { api_print("trust: writing the trusted list failed"); return; }
    char p8[V3X_WPATH * 3];
    LOG("trust: %s (sha256 %s) added to %s by the console", g_plugins[i].file, g_plugins[i].sha, utf8_of(g_allow_pathW, p8, sizeof p8));
    api_print("trusted %s (sha256 %s). Restart the game to load it. To undo, delete its line in %s.", g_plugins[i].file,
              g_plugins[i].sha, p8);
}

/* The plugin file read through a handle the caller keeps open across LoadLibrary (no write/delete
 * sharing, so the hashed bytes are the mapped bytes). NULL + *err on failure. */
#define PLUGIN_FILE_MAX (64u << 20)
static uint8_t *read_plugin_file(const wchar_t *path, HANDLE *held, size_t *n, DWORD *err)
{
    *n = 0; *err = 0;
    *held = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (*held == INVALID_HANDLE_VALUE) { *err = GetLastError(); *held = NULL; return NULL; }
    LARGE_INTEGER sz;
    uint8_t *b = NULL;
    if (!GetFileSizeEx(*held, &sz) || sz.QuadPart <= 0 || (unsigned long long)sz.QuadPart > PLUGIN_FILE_MAX) { *err = ERROR_FILE_TOO_LARGE; return NULL; }
    if (!(b = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, (size_t)sz.QuadPart))) { *err = ERROR_NOT_ENOUGH_MEMORY; return NULL; }
    DWORD got = 0;
    if (!ReadFile(*held, b, (DWORD)sz.QuadPart, &got, NULL) || got != (DWORD)sz.QuadPart) {
        *err = GetLastError() ? GetLastError() : ERROR_READ_FAULT;
        HeapFree(GetProcessHeap(), 0, b);
        return NULL;
    }
    *n = (size_t)sz.QuadPart;
    return b;
}

static void load_plugins(void)
{
    wchar_t pattern[V3X_WPATH];
    char pattern8[MAX_PATH * 3];
    if (wpath(pattern, V3X_WPATH, g_v3x_dirW, "plugins\\*.dll")) { LOG("ERROR: plugin folder path too long"); return; }
    utf8_of(pattern, pattern8, sizeof pattern8);
    char crashed[MAX_PATH];
    marker_take(crashed, sizeof crashed);
    if (crashed[0]) {
        LOG("ERROR: the game crashed while loading plugin %s last start: skipping it this start (the next start "
            "tries again; disable it with [plugins] %s = 0 in v3x.ini)", crashed, crashed);
        if (g_con) api_print("V3X: plugin %s crashed the game while loading last time; it is skipped this start.", crashed);
    }
    read_last_crash();
    static char ini[65536];
    read_v3x_ini(ini, sizeof ini);
    trust_init();
    WIN32_FIND_DATAW fdw;
    HANDLE h = FindFirstFileW(pattern, &fdw);
    if (h == INVALID_HANDLE_VALUE) { LOG("no plugins found (%s)", pattern8); return; }
    int ok = 0, total = 0, disabled = 0, failed = 0, skipped = 0;
    int gate = build_gate_active(ini);
    static V3xPePlugin pe;   /* main thread only */
    do {
        struct { char cFileName[MAX_PATH * 3]; } fd;   /* UTF-8 file name for logs, records and the ini */
        utf8_of(fdw.cFileName, fd.cFileName, sizeof fd.cFileName);
        total++;
        char stem[MAX_PATH * 3];
        snprintf(stem, sizeof stem, "%s", fd.cFileName);
        char *dot = strrchr(stem, '.');
        if (dot && !_stricmp(dot, ".dll")) *dot = 0;
        if (!ini_plugin_enabled(ini, stem)) {
            disabled++;
            LOG("plugin %s disabled (v3x.ini)", stem);
            plugin_record(stem, fd.cFileName, "disabled (v3x.ini [plugins])");
            continue;
        }
        if (crashed[0] && !_stricmp(crashed, stem)) {
            skipped++;
            plugin_record(stem, fd.cFileName, "disabled: crashed during load last start");
            continue;
        }
        wchar_t path[V3X_WPATH];
        size_t dl = wcslen(g_v3x_dirW), fl = wcslen(fdw.cFileName);
        if (dl + 8 + fl + 1 > V3X_WPATH) {
            LOG("FAILED to load %s: path too long", fd.cFileName);
            plugin_record("?", fd.cFileName, "FAILED: path too long");
            continue;
        }
        wmemcpy(path, g_v3x_dirW, dl);
        wmemcpy(path + dl, L"plugins\\", 8);
        wmemcpy(path + dl + 8, fdw.cFileName, fl + 1);

        /* ---- everything below up to LoadLibrary works on the FILE (audit N4): nothing of it runs yet */
        HANDLE held = NULL;
        size_t fsz = 0;
        DWORD ferr = 0;
        uint8_t *img = read_plugin_file(path, &held, &fsz, &ferr);
        char sha[65] = "", why[160] = "";
        int r;
        if (!img) {
            if (held) CloseHandle(held);
            failed++;
            LOG("FAILED to read %s (error %lu)", fd.cFileName, ferr);
            plugin_record("?", fd.cFileName, "FAILED: cannot read the file (error %lu)", ferr);
            continue;
        }
        v3x_sha256_hex(img, fsz, sha);
        int perr = v3x_pe_read_plugin(img, fsz, &pe);
        HeapFree(GetProcessHeap(), 0, img);
        const char *name = pe.name[0] ? pe.name : pe.has_name && pe.plugin_name[0] ? pe.plugin_name : fd.cFileName;
        const char *trust = trust_of(fd.cFileName, sha);
        if (!trust) {
            CloseHandle(held);
            skipped++;
            g_untrusted_count++;
            LOG("WARNING: plugin %s NOT loaded: not trusted (sha256 %s is neither in the signed plugins.json nor in your "
                "trusted list). If you installed it yourself and trust its author, type 'trust %s' in the console.",
                fd.cFileName, sha, stem);
            r = plugin_record(stem, fd.cFileName, "NOT LOADED: untrusted (type 'trust %s' to allow it)", stem);
            if (r >= 0) { g_plugins[r].untrusted = 1; snprintf(g_plugins[r].sha, sizeof g_plugins[r].sha, "%s", sha); }
            continue;
        }
        if (perr) {
            CloseHandle(held);
            failed++;
            LOG("FAILED to load %s: %s (checked before loading, nothing of it ran)", fd.cFileName, pe.why);
            plugin_record("?", fd.cFileName, "FAILED: %s", pe.why);
            continue;
        }
        if (!pe.has_load) {
            CloseHandle(held);
            LOG("%s: no v3x_plugin_load export, skipped (not loaded)", fd.cFileName);
            plugin_record("?", fd.cFileName, "skipped (no v3x_plugin_load)");
            continue;
        }
        if (manifest_check(&pe, name, why, sizeof why, gate)) {
            CloseHandle(held);
            LOG("plugin '%s' (%s) %s", name, fd.cFileName, why);
            r = plugin_record(name, fd.cFileName, "%s", why);
            if (r >= 0) snprintf(g_plugins[r].version, sizeof g_plugins[r].version, "%s", pe.version);
            continue;
        }
        r = plugin_record(name, fd.cFileName, "loading");
        if (r >= 0) {
            snprintf(g_plugins[r].version, sizeof g_plugins[r].version, "%s", pe.version);
            snprintf(g_plugins[r].author, sizeof g_plugins[r].author, "%s", pe.author);
            snprintf(g_plugins[r].desc, sizeof g_plugins[r].desc, "%s", pe.desc);
            snprintf(g_plugins[r].sha, sizeof g_plugins[r].sha, "%s", sha);
            snprintf(g_plugins[r].trust, sizeof g_plugins[r].trust, "%s", trust);
            if (pe.mf_has_caps) {
                int unk = 0;
                char first[24];
                g_plugins[r].caps = cp_caps_parse(pe.caps, strlen(pe.caps), &unk, first, sizeof first);
                g_plugins[r].caps_declared = 1;
                snprintf(g_plugins[r].caps_text, sizeof g_plugins[r].caps_text, "%s", pe.caps);
                if (unk) LOG("WARNING: plugin '%s': %d unknown capability word(s) in its manifest (first: \"%s\"), ignored", name, unk, first);
            }
        }
        marker_set(stem);
        HMODULE m = LoadLibraryW(path);
        DWORD lerr = m ? 0 : GetLastError();
        CloseHandle(held);   /* mapped now (or failed): the file may change again */
        if (!m) {
            marker_clear();
            failed++;
            LOG("FAILED to load %s (error %lu)", fd.cFileName, lerr);
            if (r >= 0) snprintf(g_plugins[r].status, sizeof g_plugins[r].status, "FAILED LoadLibrary (error %lu)", lerr);
            continue;
        }
        v3x_plugin_load_fn load = (v3x_plugin_load_fn)GetProcAddress(m, "v3x_plugin_load");
        if (!load) {   /* the file said it has one; a mapped image without it is not the file we checked */
            marker_clear();
            failed++;
            LOG("FAILED to load %s: v3x_plugin_load missing after mapping", fd.cFileName);
            if (r >= 0) snprintf(g_plugins[r].status, sizeof g_plugins[r].status, "FAILED: v3x_plugin_load missing");
            continue;   /* not freed: DllMain already ran, unloading it may leave its threads without code */
        }
        if (r >= 0) InterlockedExchangePointer((PVOID volatile *)&g_plugins[r].mod, (PVOID)m);   /* published last */
        int rc = -1;
        Fault f;
        int faulted = guard_load_fn(m, load, &g_api, &rc, &f);
        marker_clear();
        if (faulted) {
            failed++;
            on_fault(m, "v3x_plugin_load", NULL, &f);
            module_disable(m, "FAULTED in v3x_plugin_load, rolled back");
            rollback_module(m, name);
        } else if (rc == 0) {
            ok++;
            LOG("loaded plugin '%s'%s%s (%s, %s%s%s)", name, pe.version[0] ? " v" : "", pe.version, fd.cFileName, trust,
                pe.mf_has_caps ? ", capabilities: " : "", pe.mf_has_caps ? pe.caps : "");
            if (r >= 0) snprintf(g_plugins[r].status, sizeof g_plugins[r].status, "loaded at %p", (void *)m);
        } else {
            char st[64];
            failed++;
            LOG("plugin '%s' returned %d from v3x_plugin_load", name, rc);
            snprintf(st, sizeof st, "FAILED: v3x_plugin_load returned %d, rolled back", rc);
            module_disable(m, st);
            rollback_module(m, name);
        }
    } while (FindNextFileW(h, &fdw));
    FindClose(h);
    if (disabled) LOG("%d/%d plugins loaded, %d disabled in v3x.ini", ok, total - disabled, disabled);
    else LOG("%d/%d plugins loaded", ok, total);
    if (failed || skipped) LOG("%d plugin(s) failed and were rolled back, %d skipped (crash last start or untrusted)", failed, skipped);
    if (g_untrusted_count && g_con)
        api_print("V3X: %d plugin(s) were NOT loaded because they are not signed by V3X and not in your trusted list. "
                  "Type 'trust' to see them, 'trust <plugin>' to allow one (next start).", g_untrusted_count);
    emit_core_event(V3X_EVENT_PLUGINS_LOADED, NULL);
}

/* ------------------------------------------------------------------ process exit */

typedef void(NTAPI *RtlExitUserProcess_t)(LONG status);
static RtlExitUserProcess_t g_RtlExitUserProcess;

/* Runs on the thread calling ExitProcess, before ntdll kills the other threads. Subscribers of
 * "process_exit" run here with every thread alive (data = const LONG *exit status); they must stay
 * short and must not wait for other threads without a timeout. Only the first call does anything. */
static void NTAPI hk_RtlExitUserProcess(LONG status)
{
    if (!InterlockedExchange(&g_exit_started, 1)) {
        LOG("process exit (code %ld) on thread %lu", status, GetCurrentThreadId());
        emit_core_event(V3X_EVENT_PROCESS_EXIT, &status);
        for (int i = 0; g_hook_busy && i < 1000; i++) Sleep(1);   /* let MinHook resume what it froze */
    }
    g_RtlExitUserProcess(status);
}

static void hook_process_exit(void)
{
    void *f = (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlExitUserProcess");
    int s = f ? core_hook(f, (void *)hk_RtlExitUserProcess, (void **)&g_RtlExitUserProcess) : MH_ERROR_FUNCTION_NOT_FOUND;
    LOG("exit hook (RtlExitUserProcess): %s", s == MH_OK ? "ok" : s == V3X_EHOOKBUSY ? "busy" : MH_StatusToString((MH_STATUS)s));
}

/* ------------------------------------------------------------------ boot */

typedef int(WINAPI *entry_t)(void);
static entry_t g_real_entry;

static int WINAPI hk_entry(void)
{
    g_main_thread = GetCurrentThreadId();
    open_console();
    LOG("entry point reached on main thread %lu", g_main_thread);

    void *peek = (void *)GetProcAddress(GetModuleHandleA("user32.dll") ? GetModuleHandleA("user32.dll")
                                                                         : LoadLibraryExW(L"user32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32),
                                        "PeekMessageW");
    int s = peek ? core_hook(peek, (void *)hk_PeekMessageW, (void **)&g_PeekMessageW) : -1;   /* owned by the core */
    LOG("frame hook (PeekMessageW): %s", s == 0 ? "ok" : "FAILED");
    hook_process_exit();

    load_plugins();
    if (g_con) start_console_input();

    /* Restore the entry so nothing re-enters us, then continue normal startup (the trampoline stays). */
    void *busy;
    v3x_hook_remove((void *)g_self, (uint8_t *)g_base + ((IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew))->OptionalHeader.AddressOfEntryPoint, &busy);
    return g_real_entry();
}

static void read_text_file(const wchar_t *path, char *out, size_t cap)
{
    FILE *f = _wfopen(path, L"rb");
    if (!f) return;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = 0;
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) out[--n] = 0;
}

static void boot(void)
{
    g_shutdown_in_progress = (shutdown_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlDllShutdownInProgress");
    g_cap_tls = TlsAlloc();   /* TLS_OUT_OF_INDEXES -> exec_capture degrades to exec_command */
    g_ctx_tls = TlsAlloc();   /* callback context (owner of tail calls); TLS_OUT_OF_INDEXES -> none */
    InitializeCriticalSection(&g_log_lock);
    InitializeCriticalSection(&g_event_lock);
    InitializeCriticalSection(&g_cmd_lock);
    InitializeCriticalSection(&g_mainq_lock);
    InitializeCriticalSection(&g_patch_lock);
    register_builtins();

    /* Paths come from the exe, not from version.dll, so this works wherever the game lives. Wide first
     * (L13); a path that does not fit leaves the dirs empty (files then resolve relative to the cwd). */
    wchar_t exeW[V3X_WPATH];
    DWORD en = GetModuleFileNameW(NULL, exeW, V3X_WPATH);
    int path_bad = en == 0 || en >= V3X_WPATH - 16;
    if (path_bad) en = 0;
    exeW[en] = 0;
    wmemcpy(g_game_dirW, exeW, (size_t)en + 1);
    wchar_t *slash = wcsrchr(g_game_dirW, L'\\');
    if (slash) slash[1] = 0;
    size_t gl = wcslen(g_game_dirW);
    wmemcpy(g_v3x_dirW, g_game_dirW, gl);
    wmemcpy(g_v3x_dirW + gl, L"v3x\\", 5);
    if (gl) CreateDirectoryW(g_v3x_dirW, NULL);
    /* ANSI copies for the API (frozen const char *); lossy = characters outside the ANSI code page */
    BOOL lossy = FALSE;
    int an = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, g_game_dirW, -1, g_game_dir, MAX_PATH - 5, NULL, &lossy);
    if (an <= 0) { g_game_dir[0] = 0; lossy = TRUE; }
    snprintf(g_v3x_dir, sizeof g_v3x_dir, "%sv3x\\", g_game_dir);
    if (WideCharToMultiByte(CP_UTF8, 0, g_game_dirW, -1, g_game_dir8, (int)sizeof g_game_dir8, NULL, NULL) <= 0) g_game_dir8[0] = 0;
    if (WideCharToMultiByte(CP_UTF8, 0, g_v3x_dirW, -1, g_v3x_dir8, (int)sizeof g_v3x_dir8, NULL, NULL) <= 0) g_v3x_dir8[0] = 0;

    wchar_t logpath[V3X_WPATH];
    if (!wpath(logpath, V3X_WPATH, g_v3x_dirW, "v3x.log")) g_log = _wfsopen(logpath, L"w", _SH_DENYWR);
    if (!g_log) {
        /* v3x.log can be held open by another process (e.g. Paradox's CrashReporter after a crash):
         * fall back to a per-process file so the session's log is never lost. */
        char alt[64];
        snprintf(alt, sizeof alt, "v3x.%lu.log", GetCurrentProcessId());
        if (!wpath(logpath, V3X_WPATH, g_v3x_dirW, alt)) g_log = _wfsopen(logpath, L"w", _SH_DENYWR);
    }

    wchar_t vpath[V3X_WPATH];
    if (!wpath(vpath, V3X_WPATH, g_game_dirW, "..\\caligula_branch.txt")) read_text_file(vpath, g_version, sizeof g_version);

    read_module_layout();
    char exe[V3X_WPATH * 3];
    LOG("V3X %s api v%d, exe=%s base=%p size=0x%zx game=%s", V3X_LOADER_SEMVER, V3X_API_VERSION, utf8_of(exeW, exe, sizeof exe),
        g_base, g_size, g_version);
    if (path_bad) LOG("ERROR: the game path is too long (> %d chars): v3x files are not found", V3X_WPATH - 16);
    if (lossy)
        LOG("WARNING: the game folder has characters outside the ANSI code page: the loader works, but plugins that "
            "open files through api->game_dir / v3x_dir with ANSI functions (fopen, CreateFileA) will fail. "
            "Move the game to a plain path to be safe.");
    log_config_load();

    if (GetEnvironmentVariableA("V3X_DISABLE", NULL, 0)) { LOG("V3X_DISABLE set, not hooking"); return; }

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK) { LOG("MH_Initialize failed: %d", s); return; }

    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    void *entry = g_base + nt->OptionalHeader.AddressOfEntryPoint;
    int es = core_hook(entry, (void *)hk_entry, (void **)&g_real_entry);   /* owned by the core (audit N1) */
    LOG("entry hook at %p: %s", entry, es == MH_OK ? "ok" : es == V3X_EHOOKBUSY ? "busy" : MH_StatusToString((MH_STATUS)es));
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        int missing = v3x_proxy_resolve();
        boot();
        if (missing < 0)
            LOG("ERROR: the system version.dll could not be loaded: its functions fail closed for the game "
                "(return 0 / error, GetLastError = ERROR_MOD_NOT_FOUND)");
        else if (missing)
            LOG("WARNING: %d version.dll exports unresolved (they fail closed: return 0 / error)", missing);
    } else if (reason == DLL_PROCESS_DETACH) {
        /* Process exit or unload: never block here. Only a plain store; hooks stay, the log FILE,
         * console and plugin threads are left to the OS (v3x.log is flushed after every line). */
        g_detached = 1;
    }
    return TRUE;
}
