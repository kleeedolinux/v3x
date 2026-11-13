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
#include <share.h>

#include "MinHook.h"
#include "../include/v3x.h"

int v3x_proxy_resolve(void);
/* hook registry, src/safe_hook.c: per-target ownership, shared hook_safe thunks, exclusive raw hooks */
int v3x_hook_add_safe(void *owner, void *target, v3x_hook_fn pre, v3x_hook_fn post, void *user, void **busy_owner);
int v3x_hook_add_raw(void *owner, void *target, void *detour, void **original, void **busy_owner);
int v3x_hook_remove(void *owner, void *target, void **busy_owner);
int v3x_hook_subscribers(void *target);
int v3x_hook_remove_owner(void *owner);

/* ------------------------------------------------------------------ state */

/* Loader semver (API v5 loader_version). Bump: patch = fixes, minor = new API fields/commands,
 * major = never without a new V3X_API generation (docs/api-stability.md). */
#define V3X_LOADER_SEMVER "0.5.0"

static HMODULE g_self;
static uint8_t *g_base;
static size_t g_size;
static uint8_t *g_text; static size_t g_text_size;
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
 * skipped). NULL = not a module (e.g. heap code) or the core. */
#define MAX_FRAME_CBS 64
static struct { v3x_frame_fn fn; void *user; HMODULE mod; } g_frame_cbs[MAX_FRAME_CBS];
static volatile LONG g_frame_count;

/* loaded-plugin records for the `plugins` command */
#define MAX_PLUGINS 64
static struct {
    char name[64]; char file[64]; char status[128];
    char version[24]; char author[48]; char desc[112];   /* from v3x_plugin_manifest (API v4), may be "" */
    HMODULE mod;                                          /* NULL if not loaded */
} g_plugins[MAX_PLUGINS];
static int g_plugin_count;

/* console commands (API v2) */
#define MAX_COMMANDS 128
static struct { char name[32]; char help[160]; v3x_command_fn fn; void *user; HMODULE mod; } g_cmds[MAX_COMMANDS];
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

static void api_log_at(int level, const char *tag, const char *fmt, ...)
{
    static const char *prefix[] = {"", "ERROR: ", "WARNING: ", "", "debug: ", "trace: "};
    if (level < V3X_LOG_ERROR) level = V3X_LOG_ERROR;
    if (level > V3X_LOG_TRACE) level = V3X_LOG_TRACE;
    if (level > api_log_level(tag)) return;
    va_list ap; va_start(ap, fmt); v3x_vlog(tag, prefix[level], fmt, ap); va_end(ap);
}

/* API v2 print: plain text to console + v3x.log, newline appended if missing. */
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

static int console_wanted(void)
{
    if (GetEnvironmentVariableA("V3X_NO_CONSOLE", NULL, 0)) return 0;
    char ini[MAX_PATH]; snprintf(ini, sizeof ini, "%sv3x.ini", g_v3x_dir);
    FILE *f = fopen(ini, "r");
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

/* Each guard returns 0 if the callback returned normally, 1 if it faulted (f filled). */
static int guard_frame_fn(v3x_frame_fn fn, void *user, Fault *f)
{
    __try { fn(user); return 0; }
    __except (fault_filter(GetExceptionInformation(), f)) { return 1; }
}
static int guard_command_fn(v3x_command_fn fn, const char *args, void *user, Fault *f)
{
    __try { fn(args, user); return 0; }
    __except (fault_filter(GetExceptionInformation(), f)) { return 1; }
}
typedef void (*v3x_event_fn)(const char *name, void *data, void *user);
static int guard_event_fn(v3x_event_fn fn, const char *name, void *data, void *user, Fault *f)
{
    __try { fn(name, data, user); return 0; }
    __except (fault_filter(GetExceptionInformation(), f)) { return 1; }
}
static int guard_load_fn(v3x_plugin_load_fn fn, const V3X_API *api, int *rc, Fault *f)
{
    __try { *rc = fn(api); return 0; }
    __except (fault_filter(GetExceptionInformation(), f)) { return 1; }
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
    char path[MAX_PATH];
    if (!m || !GetModuleFileNameA(m, path, sizeof path)) { snprintf(buf, n, "?"); return buf; }
    const char *b = strrchr(path, '\\');
    snprintf(buf, n, "%s", b ? b + 1 : path);
    return buf;
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

static void module_disable(HMODULE m, const char *status)
{
    if (!m || m == g_self) return;
    EnterCriticalSection(&g_log_lock);
    if (!module_disabled(m) && g_bad_count < MAX_BAD_MODS) {
        g_bad_mods[g_bad_count] = m;
        InterlockedIncrement(&g_bad_count);
    }
    for (int i = 0; i < g_plugin_count; i++)
        if (g_plugins[i].mod == m) snprintf(g_plugins[i].status, sizeof g_plugins[i].status, "%s", status);
    LeaveCriticalSection(&g_log_lock);
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

static int api_register_command(const char *name, const char *help, v3x_command_fn fn, void *user)
{
    if (!name || !*name || !fn || strlen(name) >= sizeof g_cmds[0].name || strpbrk(name, " \t\r\n")) return -1;
    if (process_exiting()) return -1;
    int rc = 0;
    EnterCriticalSection(&g_cmd_lock);
    for (int i = 0; i < g_cmd_count; i++)
        if (!_stricmp(g_cmds[i].name, name)) { rc = -2; break; }
    if (!rc && g_cmd_count >= MAX_COMMANDS) rc = -1;
    if (!rc) {
        int i = g_cmd_count;
        snprintf(g_cmds[i].name, sizeof g_cmds[i].name, "%s", name);
        snprintf(g_cmds[i].help, sizeof g_cmds[i].help, "%s", help ? help : "");
        g_cmds[i].fn = fn; g_cmds[i].user = user; g_cmds[i].mod = module_of((const void *)fn);
        g_cmd_count = i + 1;
    }
    LeaveCriticalSection(&g_cmd_lock);
    return rc;
}

/* Runs one console line on the calling thread. The command runs outside g_cmd_lock so it may
 * itself register or exec commands. */
static int api_exec_command(const char *line)
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
    Fault f;
    if (guard_command_fn(fn, argbuf, user, &f)) { on_fault(mod, "console command", name, &f); return 0; }
    return 0;
}

/* API v3: exec_command with the calling thread's print/log output also sent to sink. */
static int api_exec_capture(const char *line, void (*sink)(const char *text, void *user), void *user)
{
    if (!line) return -1;
    if (!sink || g_cap_tls == TLS_OUT_OF_INDEXES) return api_exec_command(line);
    Capture c = {sink, user};
    DWORD err = GetLastError();
    Capture *prev = (Capture *)TlsGetValue(g_cap_tls);
    TlsSetValue(g_cap_tls, &c);
    SetLastError(err);
    int rc = api_exec_command(line);
    err = GetLastError();
    TlsSetValue(g_cap_tls, prev);
    SetLastError(err);
    return rc;
}

/* ------------------------------------------------------------------ events (API v3) */

#define MAX_EVENT_SUBS 64
static struct { char name[64]; v3x_event_fn fn; void *user; HMODULE mod; } g_subs[MAX_EVENT_SUBS];
static int g_sub_count;
static CRITICAL_SECTION g_event_lock;

static int event_name_ok(const char *name) { return name && *name && strlen(name) < sizeof g_subs[0].name; }

static int api_on_event(const char *name, v3x_event_fn fn, void *user)
{
    if (!event_name_ok(name) || !fn || process_exiting()) return -1;
    int rc = 0;
    EnterCriticalSection(&g_event_lock);
    for (int i = 0; i < g_sub_count; i++)
        if (g_subs[i].fn == fn && g_subs[i].user == user && !strcmp(g_subs[i].name, name)) { rc = -2; break; }
    if (!rc && g_sub_count >= MAX_EVENT_SUBS) rc = -1;
    if (!rc) {
        int i = g_sub_count;
        snprintf(g_subs[i].name, sizeof g_subs[i].name, "%s", name);
        g_subs[i].fn = fn; g_subs[i].user = user; g_subs[i].mod = module_of((const void *)fn);
        g_sub_count = i + 1;
    }
    LeaveCriticalSection(&g_event_lock);
    return rc;
}

/* Snapshot the matching subscribers under the lock, call them outside it (they may subscribe/emit). */
static int api_emit_event(const char *name, void *data)
{
    if (!event_name_ok(name) || process_exiting()) return -1;
    struct { v3x_event_fn fn; void *user; HMODULE mod; } hit[MAX_EVENT_SUBS];
    int n = 0, called = 0;
    EnterCriticalSection(&g_event_lock);
    for (int i = 0; i < g_sub_count; i++)
        if (!strcmp(g_subs[i].name, name)) { hit[n].fn = g_subs[i].fn; hit[n].user = g_subs[i].user; hit[n].mod = g_subs[i].mod; n++; }
    LeaveCriticalSection(&g_event_lock);
    for (int i = 0; i < n; i++) {
        if (module_disabled(hit[i].mod)) continue;   /* faulted plugin */
        Fault f;
        called++;
        if (guard_event_fn(hit[i].fn, name, data, hit[i].user, &f)) on_fault(hit[i].mod, "event", name, &f);
    }
    return called;
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
        if (guard_frame_fn(fn, user, &f)) on_fault(mod, "run_on_main callback", NULL, &f);
    }
}

static void cmd_help(const char *args, void *user)
{
    (void)user;
    EnterCriticalSection(&g_cmd_lock);
    int n = g_cmd_count;
    LeaveCriticalSection(&g_cmd_lock); /* entries are append-only, [0, n) stays valid */
    if (*args) {
        for (int i = 0; i < n; i++)
            if (!_stricmp(g_cmds[i].name, args)) { api_print("%s  %s", g_cmds[i].name, g_cmds[i].help); return; }
        api_print("no command '%s'", args);
        return;
    }
    api_print("%d commands (type a command and press Enter):", n);
    for (int i = 0; i < n; i++) api_print("  %-10s %s", g_cmds[i].name, g_cmds[i].help);
}

static void cmd_plugins(const char *args, void *user)
{
    (void)args; (void)user;
    api_print("%d plugin file(s):", g_plugin_count);
    for (int i = 0; i < g_plugin_count; i++) {
        api_print("  %-20s %-10s %-28s %s", g_plugins[i].name, g_plugins[i].version[0] ? g_plugins[i].version : "-",
                  g_plugins[i].file, g_plugins[i].status);
        if (g_plugins[i].desc[0] || g_plugins[i].author[0])
            api_print("  %-20s %s%s%s", "", g_plugins[i].desc, g_plugins[i].author[0] ? "  -- " : "", g_plugins[i].author);
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

static void register_builtins(void)
{
    api_register_command("help", "[command]  list commands", cmd_help, NULL);
    api_register_command("plugins", "list plugin DLLs and their load status", cmd_plugins, NULL);
    api_register_command("log", "<text>  write a line to v3x.log", cmd_log, NULL);
    api_register_command("version", "loader API version, game version, exe base", cmd_version, NULL);
    api_register_command("quit", "(does not quit) how to close the game", cmd_quit, NULL);
    api_register_command("exit", "(does not quit) same as quit", cmd_quit, NULL);
    api_register_command("loglevel", "[tag] [level]  show/set the log_at threshold (off error warn info debug trace)", cmd_loglevel, NULL);
    api_register_command("reload", "[plugin]  ask plugins to re-read their config (event config_reload)", cmd_reload, NULL);
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
        api_exec_command(p);
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

static void *api_pattern_scan(const char *pat)
{
    uint8_t bytes[256]; uint8_t mask[256]; int n = 0;
    if (!pat) return NULL;
    for (const char *s = pat; *s && n < 256;) {
        if (*s == ' ') { s++; continue; }
        if (*s == '?') { bytes[n] = 0; mask[n++] = 0; s++; if (*s == '?') s++; continue; }
        char hex[3] = {s[0], s[1] ? s[1] : 0, 0};
        bytes[n] = (uint8_t)strtoul(hex, NULL, 16); mask[n++] = 1;
        s += s[1] ? 2 : 1;
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
    if (!g_text || !target) return NULL;
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

/* The module (plugin DLL) a call came from: hooks are owned per module, so unhook removes only the
 * caller's own hook and a busy target can name its owner. Falls back to the core itself. */
static void *caller_module(void *ret_addr)
{
    HMODULE m = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)ret_addr, &m) || !m)
        m = g_self;
    return (void *)m;
}

static const char *module_label(void *m, char *buf, size_t n)
{
    char path[MAX_PATH];
    if (!m) return "unknown (hooked outside the V3X API)";
    if (m == (void *)g_self) return "v3x core";
    if (!GetModuleFileNameA((HMODULE)m, path, sizeof path)) { snprintf(buf, n, "module %p", m); return buf; }
    const char *b = strrchr(path, '\\');
    snprintf(buf, n, "%s", b ? b + 1 : path);
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

static int api_hook(void *target, void *detour, void **original)
{
    void *me = caller_module(__builtin_return_address(0)), *holder;
    if (!hook_enter()) return MH_ERROR_NOT_INITIALIZED;
    int s = v3x_hook_add_raw(me, target, detour, original, &holder);
    InterlockedDecrement(&g_hook_busy);
    if (s == V3X_EHOOKBUSY) log_busy("hook", target, me, holder);
    return s;
}

static int api_unhook(void *target)
{
    void *me = caller_module(__builtin_return_address(0)), *holder;
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
    void *me = caller_module(__builtin_return_address(0)), *holder;
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

static int raw_patch(void *addr, const void *bytes, size_t n)
{
    DWORD old;
    if (!VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, &old)) return (int)GetLastError();
    memcpy(addr, bytes, n);
    VirtualProtect(addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, n);
    return 0;
}

/* Patches are recorded per calling module with the bytes they replaced, so a plugin whose
 * v3x_plugin_load fails can be rolled back (rollback_module). Table full = patched but not tracked. */
#define MAX_PATCHES 512
static struct { HMODULE owner; uint8_t *addr; size_t n; uint8_t *orig; } g_patches[MAX_PATCHES];
static int g_patch_count;
static CRITICAL_SECTION g_patch_lock;

static int api_patch(void *addr, const void *bytes, size_t n)
{
    HMODULE me = (HMODULE)caller_module(__builtin_return_address(0));
    if (!addr || !bytes || !n) return ERROR_INVALID_PARAMETER;
    if (process_exiting()) return ERROR_PROCESS_ABORTED;
    uint8_t *orig = NULL;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
        (orig = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, n)))
        memcpy(orig, addr, n);   /* fault here = the patch below would fault too; only for committed memory */
    int rc = raw_patch(addr, bytes, n);
    EnterCriticalSection(&g_patch_lock);
    if (rc == 0 && orig && g_patch_count < MAX_PATCHES) {
        g_patches[g_patch_count].owner = me; g_patches[g_patch_count].addr = (uint8_t *)addr;
        g_patches[g_patch_count].n = n; g_patches[g_patch_count].orig = orig;
        g_patch_count++;
        orig = NULL;
    }
    LeaveCriticalSection(&g_patch_lock);
    if (orig) HeapFree(GetProcessHeap(), 0, orig);
    return rc;
}

/* Undoes what module m installed: its hooks (raw + hook_safe, src/safe_hook.c) and its patches (newest
 * first). Only called on the main thread at load time, right after m's v3x_plugin_load failed. */
static void rollback_module(HMODULE m, const char *name)
{
    int hooks = 0, patches = 0;
    if (hook_enter()) { hooks = v3x_hook_remove_owner((void *)m); InterlockedDecrement(&g_hook_busy); }
    EnterCriticalSection(&g_patch_lock);
    for (int i = g_patch_count - 1; i >= 0; i--) {
        if (g_patches[i].owner != m || !g_patches[i].orig) continue;
        if (raw_patch(g_patches[i].addr, g_patches[i].orig, g_patches[i].n) == 0) patches++;
        HeapFree(GetProcessHeap(), 0, g_patches[i].orig);
        g_patches[i].orig = NULL;
    }
    LeaveCriticalSection(&g_patch_lock);
    LOG("plugin '%s' rolled back: %d hook target(s) released, %d patch(es) restored, callbacks disabled", name, hooks, patches);
}

static int api_on_frame(v3x_frame_fn fn, void *user)
{
    LONG i = g_frame_count;
    if (!fn || i >= MAX_FRAME_CBS) return -1;
    g_frame_cbs[i].fn = fn; g_frame_cbs[i].user = user; g_frame_cbs[i].mod = module_of((const void *)fn);
    InterlockedIncrement(&g_frame_count);
    return 0;
}

typedef BOOL(WINAPI *PeekMessageW_t)(LPMSG, HWND, UINT, UINT, UINT);
static PeekMessageW_t g_PeekMessageW;
static DWORD g_main_thread;

static uint32_t api_main_thread_id(void) { return g_main_thread; }

/* API v4: every registered command, in order, called outside g_cmd_lock (entries are append-only). */
static int api_enum_commands(void (*fn)(const char *name, const char *help, void *user), void *user)
{
    if (!fn || process_exiting()) return -1;
    EnterCriticalSection(&g_cmd_lock);
    int n = g_cmd_count;
    LeaveCriticalSection(&g_cmd_lock);
    for (int i = 0; i < n; i++) fn(g_cmds[i].name, g_cmds[i].help, user);
    return n;
}

static BOOL WINAPI hk_PeekMessageW(LPMSG msg, HWND wnd, UINT lo, UINT hi, UINT rm)
{
    if (GetCurrentThreadId() == g_main_thread) {
        LONG n = g_frame_count;
        for (LONG i = 0; i < n; i++) {
            if (module_disabled(g_frame_cbs[i].mod)) continue;   /* faulted plugin: skipped */
            Fault f;
            if (guard_frame_fn(g_frame_cbs[i].fn, g_frame_cbs[i].user, &f)) on_fault(g_frame_cbs[i].mod, "on_frame callback", NULL, &f);
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
};

/* For tests/tools that are not plugins: GetProcAddress(GetModuleHandleA("version.dll"), "v3x_api"). */
__declspec(dllexport) const V3X_API *v3x_api(void) { return &g_api; }

static int plugin_record(const char *name, const char *file, const char *fmt, ...)
{
    if (g_plugin_count >= MAX_PLUGINS) return -1;
    int i = g_plugin_count++;
    snprintf(g_plugins[i].name, sizeof g_plugins[i].name, "%s", name);
    snprintf(g_plugins[i].file, sizeof g_plugins[i].file, "%s", file);
    va_list ap; va_start(ap, fmt); vsnprintf(g_plugins[i].status, sizeof g_plugins[i].status, fmt, ap); va_end(ap);
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

/* Reads the optional `v3x_plugin_manifest` data export (API v4). Returns 0 = load the plugin,
 * otherwise a refusal reason in why. name/version/author/desc are filled (or left ""). */
static int read_manifest(HMODULE m, const char **name, char *version, char *author, char *desc, char *why, size_t why_n,
                         int gate)
{
    const V3X_PluginManifest *mf = (const V3X_PluginManifest *)(void *)GetProcAddress(m, "v3x_plugin_manifest");
    if (!mf) {
        if (!gate) return 0;
        snprintf(why, why_n, "skipped: unknown game build, no manifest (unknown_build = load to force)");
        return 1;
    }
    uint32_t sz = mf->struct_size;
    if (sz < offsetof(V3X_PluginManifest, name)) { snprintf(why, why_n, "bad v3x_plugin_manifest (size %u)", sz); return 1; }
#define MF_STR(f) (sz >= offsetof(V3X_PluginManifest, f) + sizeof(const char *) && mf->f ? mf->f : NULL)
    if (MF_STR(name) && *MF_STR(name)) *name = MF_STR(name);
    snprintf(version, 24, "%s", MF_STR(version) ? MF_STR(version) : "");
    snprintf(author, 48, "%s", MF_STR(author) ? MF_STR(author) : "");
    snprintf(desc, 112, "%s", MF_STR(description) ? MF_STR(description) : "");
    const char *builds = MF_STR(game_builds);
#undef MF_STR
    if (mf->min_api > V3X_API_VERSION) {
        snprintf(why, why_n, "refused: needs loader API v%u (this is v%d)", mf->min_api, V3X_API_VERSION);
        return 1;
    }
    if (gate && !build_matches_ex(builds, 1)) {
        snprintf(why, why_n, "skipped: unknown game build %s not declared (unknown_build = load to force)", g_build_id);
        return 1;
    }
    if (!build_matches(builds)) {
        if (mf->flags & V3X_MANIFEST_REQUIRE_BUILD) {
            snprintf(why, why_n, "refused: game %s / %s not in its builds", g_version, g_build_id);
            return 1;
        }
        LOG("WARNING: plugin '%s' lists game builds \"%s\"; running %s (build %s). Loading anyway.", *name,
            builds, g_version, g_build_id);
    }
    return 0;
}

/* ------------------------------------------------------------------ plugin loading */

/* [plugins] section of v3x/v3x.ini: "<dll file name without .dll> = 0" skips that plugin (written by
 * mod_menu, or by hand). Same rules as plugins/mod_menu/mm_logic.c mm_ini_enabled: section and key
 * names are case-insensitive and trimmed, '#'/';' lines are comments, the last matching line wins,
 * 0/off/no/false = disabled, anything else or no line = enabled. */
static int ini_ieq(const char *a, size_t an, const char *b)
{
    size_t bn = strlen(b);
    if (an != bn) return 0;
    for (size_t i = 0; i < an; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return 1;
}

/* Calls fn(key, key_len, value, value_len, ctx) for every "key = value" line inside [section] of an
 * ini text, in file order (so "last wins" is up to fn). Rules as in ini_plugin_enabled. */
typedef void (*ini_kv_fn)(const char *k, size_t kn, const char *v, size_t vn, void *ctx);
static void ini_each(const char *ini, const char *section, ini_kv_fn fn, void *ctx)
{
    int in = !*section;   /* section "" = the keys before the first [section] */
    for (const char *p = ini; p && *p;) {
        const char *e = p;
        while (*e && *e != '\n') e++;
        const char *a = p, *b = e;
        if (b > a && b[-1] == '\r') b--;
        while (a < b && (*a == ' ' || *a == '\t')) a++;
        while (b > a && (b[-1] == ' ' || b[-1] == '\t')) b--;
        if (a < b && *a == '[') {
            const char *c = memchr(a, ']', (size_t)(b - a));
            if (c) {
                const char *s0 = a + 1, *t = c;
                while (s0 < t && (*s0 == ' ' || *s0 == '\t')) s0++;
                while (t > s0 && (t[-1] == ' ' || t[-1] == '\t')) t--;
                in = ini_ieq(s0, (size_t)(t - s0), section);
            }
        } else if (in && a < b && *a != '#' && *a != ';') {
            const char *eq = memchr(a, '=', (size_t)(b - a));
            if (eq) {
                const char *ke = eq, *vs = eq + 1;
                while (ke > a && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
                while (vs < b && (*vs == ' ' || *vs == '\t')) vs++;
                fn(a, (size_t)(ke - a), vs, (size_t)(b - vs), ctx);
            }
        }
        p = *e ? e + 1 : e;
    }
}

struct plugin_en { const char *stem; int en; };
static void plugin_en_kv(const char *k, size_t kn, const char *v, size_t vn, void *ctx)
{
    struct plugin_en *pe = (struct plugin_en *)ctx;
    if (ini_ieq(k, kn, pe->stem))
        pe->en = !(ini_ieq(v, vn, "0") || ini_ieq(v, vn, "off") || ini_ieq(v, vn, "no") || ini_ieq(v, vn, "false"));
}

static int ini_plugin_enabled(const char *ini, const char *stem)
{
    struct plugin_en pe = {stem, 1};
    ini_each(ini, "plugins", plugin_en_kv, &pe);
    return pe.en;
}

/* v3x/v3x.ini as text (empty if missing; at most 64 KB). */
static void read_v3x_ini(char *buf, size_t cap)
{
    char path[MAX_PATH];
    buf[0] = 0;
    snprintf(path, sizeof path, "%sv3x.ini", g_v3x_dir);
    FILE *f = fopen(path, "rb");
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
static void marker_path(char *out, size_t n) { snprintf(out, n, "%sloading.marker", g_v3x_dir); }

static void marker_set(const char *stem)
{
    char p[MAX_PATH];
    marker_path(p, sizeof p);
    HANDLE h = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(h, stem, (DWORD)strlen(stem), &w, NULL);
    CloseHandle(h);
}

static void marker_clear(void)
{
    char p[MAX_PATH];
    marker_path(p, sizeof p);
    DeleteFileA(p);
}

/* The stem left by a load that never returned last start ("" = none); removes the marker. */
static void marker_take(char *out, size_t n)
{
    char p[MAX_PATH];
    marker_path(p, sizeof p);
    out[0] = 0;
    FILE *f = fopen(p, "rb");
    if (!f) return;
    size_t k = fread(out, 1, n - 1, f);
    fclose(f);
    out[k] = 0;
    while (k && (out[k - 1] == '\r' || out[k - 1] == '\n' || out[k - 1] == ' ')) out[--k] = 0;
    DeleteFileA(p);
}

/* Read-only: the last "UNHANDLED EXCEPTION" line crash_logger wrote last session (crash.log is still
 * last session's file here, crash_logger rotates it when it loads). Logged so the player and the
 * `plugins` list can see which module crashed. */
static char g_last_crash[200];
static void read_last_crash(void)
{
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%scrash.log", g_v3x_dir);
    FILE *f = fopen(p, "rb");
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
    char exe[MAX_PATH], env[16];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    const char *b = strrchr(exe, '\\');
    b = b ? b + 1 : exe;
    int forced = GetEnvironmentVariableA("V3X_BUILD_GATE", env, sizeof env) && env[0] == '1';
    if (!forced && _stricmp(b, "victoria3.exe")) return 0;
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

static void load_plugins(void)
{
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%splugins\\*.dll", g_v3x_dir);
    char crashed[MAX_PATH];
    marker_take(crashed, sizeof crashed);
    if (crashed[0]) {
        LOG("ERROR: the game crashed while loading plugin %s last start: skipping it this start (the next start "
            "tries again; disable it with [plugins] %s = 0 in v3x.ini)", crashed, crashed);
        if (g_con) api_print("V3X: plugin %s crashed the game while loading last time; it is skipped this start.", crashed);
    }
    read_last_crash();
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { LOG("no plugins found (%s)", pattern); return; }
    int ok = 0, total = 0, disabled = 0, failed = 0, skipped = 0;
    static char ini[65536];
    read_v3x_ini(ini, sizeof ini);
    int gate = build_gate_active(ini);
    do {
        total++;
        char stem[MAX_PATH];
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
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%splugins\\%s", g_v3x_dir, fd.cFileName);
        marker_set(stem);
        HMODULE m = LoadLibraryA(path);
        if (!m) {
            DWORD e = GetLastError();
            marker_clear();
            LOG("FAILED to load %s (error %lu)", fd.cFileName, e);
            plugin_record("?", fd.cFileName, "FAILED LoadLibrary (error %lu)", e);
            continue;
        }
        v3x_plugin_load_fn load = (v3x_plugin_load_fn)GetProcAddress(m, "v3x_plugin_load");
        const char **namep = (const char **)GetProcAddress(m, "v3x_plugin_name");
        const char *name = namep && *namep ? *namep : fd.cFileName;
        char ver[24] = "", author[48] = "", desc[112] = "", why[128] = "";
        if (load && read_manifest(m, &name, ver, author, desc, why, sizeof why, gate)) {
            marker_clear();
            LOG("plugin '%s' (%s) %s", name, fd.cFileName, why);
            int r = plugin_record(name, fd.cFileName, "%s", why);
            if (r >= 0) { snprintf(g_plugins[r].version, sizeof g_plugins[r].version, "%s", ver); }
            FreeLibrary(m);
            continue;
        }
        if (!load) {
            marker_clear();
            LOG("%s: no v3x_plugin_load export, skipped", fd.cFileName);
            plugin_record("?", fd.cFileName, "skipped (no v3x_plugin_load)");
            FreeLibrary(m);
            continue;
        }
        int r = plugin_record(name, fd.cFileName, "loading");
        if (r >= 0) {
            g_plugins[r].mod = m;
            snprintf(g_plugins[r].version, sizeof g_plugins[r].version, "%s", ver);
            snprintf(g_plugins[r].author, sizeof g_plugins[r].author, "%s", author);
            snprintf(g_plugins[r].desc, sizeof g_plugins[r].desc, "%s", desc);
        }
        int rc = -1;
        Fault f;
        int faulted = guard_load_fn(load, &g_api, &rc, &f);
        marker_clear();
        if (faulted) {
            failed++;
            on_fault(m, "v3x_plugin_load", NULL, &f);
            module_disable(m, "FAULTED in v3x_plugin_load, rolled back");
            rollback_module(m, name);
        } else if (rc == 0) {
            ok++;
            LOG("loaded plugin '%s'%s%s (%s)", name, ver[0] ? " v" : "", ver, fd.cFileName);
            if (r >= 0) snprintf(g_plugins[r].status, sizeof g_plugins[r].status, "loaded at %p", (void *)m);
        } else {
            char st[64];
            failed++;
            LOG("plugin '%s' returned %d from v3x_plugin_load", name, rc);
            snprintf(st, sizeof st, "FAILED: v3x_plugin_load returned %d, rolled back", rc);
            module_disable(m, st);
            rollback_module(m, name);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (disabled) LOG("%d/%d plugins loaded, %d disabled in v3x.ini", ok, total - disabled, disabled);
    else LOG("%d/%d plugins loaded", ok, total);
    if (failed || skipped) LOG("%d plugin(s) failed and were rolled back, %d skipped after a crash", failed, skipped);
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
        api_emit_event("process_exit", &status);
        for (int i = 0; g_hook_busy && i < 1000; i++) Sleep(1);   /* let MinHook resume what it froze */
    }
    g_RtlExitUserProcess(status);
}

static void hook_process_exit(void)
{
    void *f = (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlExitUserProcess");
    MH_STATUS s = f ? MH_CreateHook(f, (void *)hk_RtlExitUserProcess, (void **)&g_RtlExitUserProcess) : MH_ERROR_FUNCTION_NOT_FOUND;
    if (s == MH_OK) s = MH_EnableHook(f);
    LOG("exit hook (RtlExitUserProcess): %s", s == MH_OK ? "ok" : MH_StatusToString(s));
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
                                                                         : LoadLibraryA("user32.dll"),
                                        "PeekMessageW");
    int s = peek ? api_hook(peek, (void *)hk_PeekMessageW, (void **)&g_PeekMessageW) : -1;
    LOG("frame hook (PeekMessageW): %s", s == 0 ? "ok" : "FAILED");
    hook_process_exit();

    load_plugins();
    if (g_con) start_console_input();

    /* Restore the entry so nothing re-enters us, then continue normal startup. */
    MH_DisableHook((void *)((uint8_t *)g_base + ((IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew))->OptionalHeader.AddressOfEntryPoint));
    return g_real_entry();
}

static void read_text_file(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "rb");
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
    InitializeCriticalSection(&g_log_lock);
    InitializeCriticalSection(&g_event_lock);
    InitializeCriticalSection(&g_cmd_lock);
    InitializeCriticalSection(&g_mainq_lock);
    InitializeCriticalSection(&g_patch_lock);
    register_builtins();

    /* Paths come from the exe, not from version.dll, so this works wherever the game lives. */
    GetModuleFileNameA(NULL, g_game_dir, MAX_PATH);
    char *slash = strrchr(g_game_dir, '\\');
    if (slash) slash[1] = 0;
    snprintf(g_v3x_dir, sizeof g_v3x_dir, "%sv3x\\", g_game_dir);
    CreateDirectoryA(g_v3x_dir, NULL);

    char logpath[MAX_PATH];
    snprintf(logpath, sizeof logpath, "%sv3x.log", g_v3x_dir);
    g_log = _fsopen(logpath, "w", _SH_DENYWR);
    if (!g_log) {
        /* v3x.log can be held open by another process (e.g. Paradox's CrashReporter after a crash):
         * fall back to a per-process file so the session's log is never lost. */
        snprintf(logpath, sizeof logpath, "%sv3x.%lu.log", g_v3x_dir, GetCurrentProcessId());
        g_log = _fsopen(logpath, "w", _SH_DENYWR);
    }

    char vpath[MAX_PATH];
    snprintf(vpath, sizeof vpath, "%s..\\caligula_branch.txt", g_game_dir);
    read_text_file(vpath, g_version, sizeof g_version);

    read_module_layout();
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    LOG("V3X %s api v%d, exe=%s base=%p size=0x%zx game=%s", V3X_LOADER_SEMVER, V3X_API_VERSION, exe, g_base, g_size,
        g_version);
    log_config_load();

    if (GetEnvironmentVariableA("V3X_DISABLE", NULL, 0)) { LOG("V3X_DISABLE set, not hooking"); return; }

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK) { LOG("MH_Initialize failed: %d", s); return; }

    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    void *entry = g_base + nt->OptionalHeader.AddressOfEntryPoint;
    s = MH_CreateHook(entry, (void *)hk_entry, (void **)&g_real_entry);
    if (s == MH_OK) s = MH_EnableHook(entry);
    LOG("entry hook at %p: %s", entry, s == MH_OK ? "ok" : MH_StatusToString(s));
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        int missing = v3x_proxy_resolve();
        boot();
        if (missing) LOG("WARNING: %d version.dll exports unresolved", missing);
    } else if (reason == DLL_PROCESS_DETACH) {
        /* Process exit or unload: never block here. Only a plain store; hooks stay, the log FILE,
         * console and plugin threads are left to the OS (v3x.log is flushed after every line). */
        g_detached = 1;
    }
    return TRUE;
}
