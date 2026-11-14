/* End-to-end test of API v5 and the loader 0.5.0 hardening (no game). Links version.dll like
 * victoria3.exe; the loader boots and loads every DLL in <dir>/v3x/plugins (variants of test/api_v5_plugin.c).
 *   api_v5_test.exe        loader_version, build_matches, log_at / log_level / `loglevel`, `reload` ->
 *                          config_reload on the main thread, fault isolation (on_frame, command, event,
 *                          run_on_main), rollback of a failed / faulting v3x_plugin_load (hooks + patches),
 *                          crash-loop marker, last-crash line from crash.log, threading rules (commands
 *                          serialised across threads incl. nested exec, concurrent on_frame registration)
 *   api_v5_test.exe gate   with V3X_BUILD_GATE=1: unknown build, safe mode (only '*' / prefix manifests load)
 *   api_v5_test.exe gateload   same plugins with V3X_UNKNOWN_BUILD=load: all load
 * test/run_all.sh builds the plugin variants and the directories ($T/api_v5, $T/api_v5_gate):
 *   v3x/v3x.ini = "[log]\nlevel = info\nv5dbg = debug\n", v3x/loading.marker = "v5_marked",
 *   v3x/crash.log containing "UNHANDLED EXCEPTION 0xc0000005 at fake_mod.dll+0x1234 - process is going down".
 * Exit code 0 = ALL OK. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/v3x.h"

__declspec(dllimport) const V3X_API *v3x_api(void);

static int fails, checks;
#define CHECK(c, ...) do { int ok_ = !!(c); checks++; printf("%s: ", ok_ ? "OK  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); fails += !ok_; } while (0)

/* hooked / patched by v5_load_fault and v5_load_fail; must be back to normal after their rollback */
static volatile long long sink_;
__declspec(dllexport) __attribute__((noinline)) long long host_target(long long x) { sink_ += x; return x * 2; }
__declspec(dllexport) __attribute__((noinline)) long long host_target2(long long x) { sink_ ^= x; return x * 5; }
__declspec(dllexport) volatile unsigned char host_bytes[16] = {0x11, 0x22, 0x33};
static long long (*volatile p_t1)(long long) = host_target;
static long long (*volatile p_t2)(long long) = host_target2;

static void *sym(const char *dll, const char *name)
{
    HMODULE m = GetModuleHandleA(dll);
    return m ? (void *)GetProcAddress(m, name) : NULL;
}
static long val(const char *dll, const char *name)
{
    volatile long *p = (volatile long *)sym(dll, name);
    return p ? *p : -1;
}
static void pump(int n)
{
    MSG msg;
    for (int i = 0; i < n; i++) PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE);
}

static char g_out[32768];
static void sink(const char *text, void *user) { (void)user; strncat(g_out, text, sizeof g_out - strlen(g_out) - 1); }
static const char *run(const V3X_API *api, const char *line)
{
    g_out[0] = 0;
    api->exec_capture(line, sink, NULL);
    return g_out;
}
static int line_has(const char *hay, const char *a, const char *b)
{
    for (const char *p = hay; (p = strstr(p, a));) {
        const char *ls = p, *le = strchr(p, '\n');
        while (ls > hay && ls[-1] != '\n') ls--;
        if (!le) le = p + strlen(p);
        const char *q = strstr(ls, b);
        if (q && q < le) return 1;
        p += strlen(a);
    }
    return 0;
}
static char *read_log(void)
{
    static char log[262144];
    FILE *f = fopen("v3x/v3x.log", "rb");
    size_t n = f ? fread(log, 1, sizeof log - 1, f) : 0;
    if (f) fclose(f);
    log[n] = 0;
    return log;
}

static int g_reloads;
static char g_reload_name[64];
static DWORD g_reload_tid;
static void on_reload(const char *n, void *d, void *u)
{
    (void)n; (void)u;
    g_reloads++;
    g_reload_tid = GetCurrentThreadId();
    snprintf(g_reload_name, sizeof g_reload_name, "%s", d ? (const char *)d : "(null)");
}

static int gate_test(const V3X_API *api, int load_all)
{
    char e[8];
    const char *out = run(api, "plugins");
    printf("---- plugins\n%s----\n", out);
    CHECK(GetEnvironmentVariableA("V3X_V5_GATE_STAR", e, sizeof e) > 0, "manifest '*' loads on an unknown build");
    CHECK(GetEnvironmentVariableA("V3X_V5_GATE_PREFIX", e, sizeof e) > 0, "manifest prefix 'unkn*' loads on an unknown build");
    if (load_all) {
        CHECK(GetEnvironmentVariableA("V3X_V5_GATE_VER", e, sizeof e) > 0 && GetEnvironmentVariableA("V3X_V5_GATE_NONE", e, sizeof e) > 0,
              "unknown_build = load: exact-version and manifest-less plugins load too");
        CHECK(strstr(read_log(), "unknown_build = load: loading every plugin anyway") != NULL, "load-anyway WARNING logged");
    } else {
        CHECK(!GetEnvironmentVariableA("V3X_V5_GATE_VER", e, sizeof e), "exact game_version string does NOT pass the gate");
        CHECK(!GetEnvironmentVariableA("V3X_V5_GATE_NONE", e, sizeof e), "plugin without manifest skipped on an unknown build");
        CHECK(line_has(out, "v5_gate_ver", "skipped: unknown game build") && line_has(out, "v5_gate_none", "no manifest"),
              "`plugins` shows why they were skipped");
        CHECK(strstr(read_log(), "Safe mode: only plugins whose manifest declares this build are loaded") != NULL, "safe-mode WARNING logged");
    }
    printf("%d checks, %d failed\n%s\n", checks, fails, fails ? "FAILED" : "ALL OK");
    return fails ? 1 : 0;
}

/* ---- threading rules (audit M3): command handlers are serialised, on_frame registration is atomic */
static volatile LONG g_inside, g_max_inside, g_runs;
static const V3X_API *g_tapi;
static void cmd_conc(const char *args, void *user)
{
    (void)args; (void)user;
    LONG now = InterlockedIncrement(&g_inside);
    for (LONG m = g_max_inside; now > m; m = g_max_inside) InterlockedCompareExchange(&g_max_inside, now, m);
    Sleep(1);
    InterlockedIncrement(&g_runs);
    InterlockedDecrement(&g_inside);
}
static void cmd_nest(const char *args, void *user) { (void)args; (void)user; g_tapi->exec_command("v5conc"); }
static DWORD WINAPI conc_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 25; i++) g_tapi->exec_command(i & 1 ? "v5nest" : "v5conc");
    return 0;
}
#define FR_THREADS 8
#define FR_PER 3
static volatile LONG g_fr_calls[FR_THREADS * FR_PER], g_fr_ok;
static void fr_cb(void *user) { InterlockedIncrement(&g_fr_calls[(intptr_t)user]); }
static DWORD WINAPI frame_thread(void *arg)
{
    intptr_t t = (intptr_t)arg;
    for (int k = 0; k < FR_PER; k++)
        if (g_tapi->on_frame(fr_cb, (void *)(t * FR_PER + k)) == 0) InterlockedIncrement(&g_fr_ok);
    return 0;
}
static void test_threading(const V3X_API *api)
{
    g_tapi = api;
    CHECK(api->register_command("v5conc", "test", cmd_conc, NULL) == 0 && api->register_command("v5nest", "test", cmd_nest, NULL) == 0,
          "threading: test commands registered");
    HANDLE th[FR_THREADS];
    for (int i = 0; i < 4; i++) th[i] = CreateThread(NULL, 0, conc_thread, NULL, 0, NULL);
    WaitForMultipleObjects(4, th, TRUE, 60000);
    for (int i = 0; i < 4; i++) CloseHandle(th[i]);
    CHECK(g_runs == 100 && g_max_inside == 1, "threading: 4 threads x 25 commands (half nested): %ld ran, at most %ld at once (want 100, 1)",
          g_runs, g_max_inside);
    for (intptr_t i = 0; i < FR_THREADS; i++) th[i] = CreateThread(NULL, 0, frame_thread, (void *)i, 0, NULL);
    WaitForMultipleObjects(FR_THREADS, th, TRUE, 60000);
    for (int i = 0; i < FR_THREADS; i++) CloseHandle(th[i]);
    pump(1);
    int ran = 0, dup = 0;
    for (int i = 0; i < FR_THREADS * FR_PER; i++) { ran += g_fr_calls[i] == 1; dup += g_fr_calls[i] > 1; }
    CHECK(g_fr_ok > 0 && ran == g_fr_ok && dup == 0,
          "threading: %d threads registering on_frame at once: %ld accepted, %d called exactly once, %d twice", FR_THREADS, g_fr_ok, ran, dup);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const V3X_API *api = v3x_api();
    if (!api) { printf("FAIL: no v3x_api\nFAILED\n"); return 1; }
    if (argc > 1 && !strcmp(argv[1], "gate")) return gate_test(api, 0);
    if (argc > 1 && !strcmp(argv[1], "gateload")) return gate_test(api, 1);

    CHECK(api->api_version == 5 && V3X_HAS(api, build_matches), "api v%u with all v5 fields", api->api_version);
    CHECK(!strcmp(api->loader_version(), "0.5.0"), "loader_version %s", api->loader_version());
    CHECK(api->build_matches(NULL) == 1 && api->build_matches("*") == 1 && api->build_matches(api->game_build_id()) == 1 &&
              api->build_matches("x;  UNKNOWN ") == 1 && api->build_matches("unk*") == 1 && api->build_matches("no-such-build") == 0,
          "build_matches: NULL, '*', own build id, case-insensitive version in a list, prefix; mismatch 0");

    /* ---- log levels */
    CHECK(api->log_level(NULL) == V3X_LOG_INFO && api->log_level("v5dbg") == V3X_LOG_DEBUG && api->log_level("V5DBG") == V3X_LOG_DEBUG &&
              api->log_level("other") == V3X_LOG_INFO,
          "log_level from v3x.ini [log]: global info, v5dbg debug (case-insensitive)");
    api->log_at(V3X_LOG_DEBUG, "other", "host-debug-hidden");
    api->log_at(V3X_LOG_ERROR, "other", "host-error-%d", 42);
    api->log_at(99, "other", "host-clamped-trace-hidden");
    api->log_at(-5, "other", "host-clamped-error");
    run(api, "loglevel other trace");
    CHECK(api->log_level("other") == V3X_LOG_TRACE, "`loglevel other trace` sets a tag level at runtime");
    api->log_at(V3X_LOG_TRACE, "other", "host-trace-shown");
    run(api, "loglevel warn");
    CHECK(api->log_level(NULL) == V3X_LOG_WARN && api->log_level("v5dbg") == V3X_LOG_DEBUG, "`loglevel warn` sets the global level only");
    api->log_at(V3X_LOG_INFO, "nobody", "host-info-hidden-at-warn");
    run(api, "loglevel info");
    CHECK(strstr(run(api, "loglevel nonsense"), "not a level") != NULL, "`loglevel nonsense` refused");
    char *log = read_log();
    CHECK(!strstr(log, "host-debug-hidden") && !strstr(log, "host-clamped-trace-hidden") && !strstr(log, "host-info-hidden-at-warn") &&
              !strstr(log, "v5-debug-line-hidden") && !strstr(log, "v5-trace-line-hidden"),
          "lines above the threshold are not written");
    CHECK(line_has(log, "[other] ERROR: host-error-42", "") && line_has(log, "[other] ERROR: host-clamped-error", "") &&
              line_has(log, "[other] trace: host-trace-shown", "") && line_has(log, "[v5dbg] debug: v5-debug-line-shown", "") &&
              line_has(log, "[v5_reload] WARNING: v5-warn-line", "") && line_has(log, "[v5_reload] v5-info-line", ""),
          "written lines carry the level prefix (ERROR:/WARNING:/debug:/trace:, none for info)");

    /* ---- reload -> config_reload on the main thread */
    CHECK(api->on_event(V3X_EVENT_CONFIG_RELOAD, on_reload, NULL) == 0, "subscribe config_reload");
    run(api, "reload v5_reload");
    CHECK(g_reloads == 0, "reload is deferred to the next frame tick");
    pump(1);
    CHECK(g_reloads == 1 && !strcmp(g_reload_name, "v5_reload") && g_reload_tid == GetCurrentThreadId(),
          "config_reload ran on the main thread with data = \"%s\"", g_reload_name);
    CHECK(val("v5_reload.dll", "v5_reloads") == 1, "the plugin's config_reload handler ran");
    run(api, "reload");
    pump(1);
    CHECK(g_reloads == 2 && !strcmp(g_reload_name, "(null)"), "`reload` without a name: data = NULL (all plugins)");

    /* ---- fault isolation */
    pump(6);
    long ff = val("v5_frame_fault.dll", "v5_frames");
    CHECK(ff == 3, "on_frame faulted on its 3rd call and was not called again (%ld calls); the host keeps running", ff);
    CHECK(api->exec_command("v5ff_cmd") == -1 && val("v5_frame_fault.dll", "v5_cmds") == 0, "faulted plugin's command is disabled");
    long cf0 = val("v5_cmd_fault.dll", "v5_frames");
    CHECK(api->exec_command("v5cf_cmd") == 0 && val("v5_cmd_fault.dll", "v5_cmds") == 1, "faulting command returned to the caller");
    pump(3);
    CHECK(val("v5_cmd_fault.dll", "v5_frames") == cf0 && cf0 > 0, "after a command fault its plugin's on_frame stops (%ld)", cf0);
    long ef0 = val("v5_event_fault.dll", "v5_frames");
    int called = api->emit_event("v5_evt", NULL);
    CHECK(called == 1 && val("v5_event_fault.dll", "v5_events") == 1, "event: C++-style exception caught (%d subscriber(s) called)", called);
    CHECK(api->emit_event("v5_evt", NULL) == 0 && val("v5_event_fault.dll", "v5_events") == 1 && val("v5_frame_fault.dll", "v5_events") == 0,
          "faulted subscribers are skipped afterwards");
    pump(2);
    CHECK(val("v5_event_fault.dll", "v5_frames") == ef0, "after an event fault its plugin's on_frame stops");
    CHECK(val("v5_main_fault.dll", "v5_frames") <= 1, "run_on_main fault (divide by zero) in the first tick stops the plugin (%ld frames)",
          val("v5_main_fault.dll", "v5_frames"));

    /* ---- rollback of a failed / faulting load */
    CHECK(p_t1(1) == 2 && p_t2(1) == 5, "host_target / host_target2 unhooked after the rollbacks (%lld, %lld)", p_t1(1), p_t2(1));
    CHECK(host_bytes[0] == 0x11 && host_bytes[1] == 0x22 && host_bytes[2] == 0x33, "patched bytes restored");
    CHECK(val("v5_load_fault.dll", "v5_events") == 1 && val("v5_load_fail.dll", "v5_events") == 1, "both loads got past hook+patch");
    CHECK(val("v5_load_fault.dll", "v5_frames") == 0 && val("v5_load_fail.dll", "v5_frames") == 0, "their on_frame never runs");

    /* ---- crash-loop marker, last crash */
    char e[8];
    CHECK(!GetEnvironmentVariableA("V3X_V5_MARKED", e, sizeof e), "plugin named in loading.marker was skipped");
    CHECK(GetFileAttributesA("v3x/loading.marker") == INVALID_FILE_ATTRIBUTES, "loading.marker removed (next start retries, no stale marker)");

    const char *out = run(api, "plugins");
    printf("---- plugins\n%s----\n", out);
    CHECK(line_has(out, "v5_marked", "disabled: crashed during load last start"), "`plugins`: v5_marked crashed during load last start");
    CHECK(line_has(out, "v5_frame_fault", "FAULTED in on_frame callback") && line_has(out, "v5_cmd_fault", "FAULTED in console command") &&
              line_has(out, "v5_event_fault", "FAULTED in event") && line_has(out, "v5_main_fault", "FAULTED in run_on_main"),
          "`plugins`: runtime faults shown");
    CHECK(line_has(out, "v5_load_fault", "FAULTED in v3x_plugin_load, rolled back") &&
              line_has(out, "v5_load_fail", "FAILED: v3x_plugin_load returned 7, rolled back"),
          "`plugins`: failed loads rolled back");
    log = read_log();
    CHECK(line_has(log, "FAULT in plugin v5_frame_fault.dll during on_frame callback", "exception 0xc0000005 (write of 0x0) at v5_frame_fault.dll+0x"),
          "fault logged with kind, address and module+offset");
    CHECK(line_has(log, "FAULT in plugin v5_main_fault.dll", "0xc0000094") && line_has(log, "FAULT in plugin v5_event_fault.dll", "0xe06d7363"),
          "divide-by-zero and C++ exception codes logged");
    CHECK(line_has(log, "'v5_load_fault' rolled back", "2 hook target(s) released, 1 patch(es) restored") &&
              line_has(log, "'v5_load_fail' rolled back", "2 hook target(s) released, 1 patch(es) restored"),
          "rollback logged (hooks + patches)");
    CHECK(strstr(log, "crashed while loading plugin v5_marked last start") != NULL, "marker skip logged");
    CHECK(strstr(log, "last session crashed (crash.log): UNHANDLED EXCEPTION 0xc0000005 at fake_mod.dll+0x1234") != NULL,
          "last crash from crash.log logged");
    test_threading(api);
    printf("%d checks, %d failed\n%s\n", checks, fails, fails ? "FAILED" : "ALL OK");
    return fails ? 1 : 0;
}
