/* Test plugins for test/api_v5_test.c (loader 0.5.0 / API v5). One source, one variant per -D flag;
 * the DLL name is the variant name (the host finds each one with GetModuleHandle):
 *   -DV5_FRAME_FAULT   v5_frame_fault.dll  on_frame faults (write to NULL) on its 3rd call; also has a
 *                                           command v5ff_cmd and a "v5_evt" subscriber (must be skipped after)
 *   -DV5_CMD_FAULT     v5_cmd_fault.dll    command v5cf_cmd faults (read of NULL); on_frame counter
 *   -DV5_EVENT_FAULT   v5_event_fault.dll  "v5_evt" subscriber raises an MSVC C++ exception; on_frame counter
 *   -DV5_MAIN_FAULT    v5_main_fault.dll   run_on_main callback divides by zero; on_frame counter
 *   -DV5_LOAD_FAULT    v5_load_fault.dll   v3x_plugin_load hooks host_target (hook_safe) + host_target2 (raw),
 *                                           patches host_bytes, registers on_frame, then faults -> rollback
 *   -DV5_LOAD_FAIL     v5_load_fail.dll    same, but returns 7 -> rollback
 *   -DV5_RELOAD        v5_reload.dll       subscribes config_reload, logs with log_at at every level
 *   -DV5_MARKED        v5_marked.dll       must never load (the host plants loading.marker = v5_marked)
 *   -DV5_GATE_STAR / _VER / _PREFIX / _NONE  manifests "*", "unknown" (exact game_version), "unkn*", none:
 *                                           for the unknown-build gate (sets env V3X_<VARIANT>=1 when loaded)
 * Counters are exported data: v5_frames, v5_cmds, v5_events, v5_reloads, v5_reload_tid, v5_reload_name. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "v3x.h"

V3X_EXPORT volatile long v5_frames, v5_cmds, v5_events, v5_reloads;
V3X_EXPORT volatile unsigned long v5_reload_tid;
V3X_EXPORT char v5_reload_name[64];

static const V3X_API *g;
static volatile int *volatile g_null;   /* NULL, through a volatile so the compiler cannot drop the fault */
static volatile int g_zero;

static void count_frames(void *u) { (void)u; v5_frames++; }

#if defined(V5_FRAME_FAULT)
V3X_PLUGIN_MANIFEST("v5_frame_fault", "1.0", "test", "faults in on_frame", "*");
static void frame(void *u) { (void)u; if (++v5_frames == 3) *g_null = 1; }
static void cmd(const char *a, void *u) { (void)a; (void)u; v5_cmds++; }
static void ev(const char *n, void *d, void *u) { (void)n; (void)d; (void)u; v5_events++; }
V3X_EXPORT int v3x_plugin_load(const V3X_API *api)
{
    g = api;
    return api->on_frame(frame, NULL) || api->register_command("v5ff_cmd", "test", cmd, NULL) || api->on_event("v5_evt", ev, NULL);
}
#elif defined(V5_CMD_FAULT)
V3X_PLUGIN_MANIFEST("v5_cmd_fault", "1.0", "test", "faults in a command", "*");
static void cmd(const char *a, void *u) { (void)a; (void)u; v5_cmds++; v5_cmds += *g_null; }
V3X_EXPORT int v3x_plugin_load(const V3X_API *api)
{
    return api->on_frame(count_frames, NULL) || api->register_command("v5cf_cmd", "test", cmd, NULL);
}
#elif defined(V5_EVENT_FAULT)
V3X_PLUGIN_MANIFEST("v5_event_fault", "1.0", "test", "throws in an event", "*");
static void ev(const char *n, void *d, void *u)
{
    (void)n; (void)d; (void)u;
    v5_events++;
    RaiseException(0xE06D7363u, EXCEPTION_NONCONTINUABLE, 0, NULL);   /* what an escaping C++ throw looks like */
}
V3X_EXPORT int v3x_plugin_load(const V3X_API *api) { return api->on_frame(count_frames, NULL) || api->on_event("v5_evt", ev, NULL); }
#elif defined(V5_MAIN_FAULT)
V3X_PLUGIN_MANIFEST("v5_main_fault", "1.0", "test", "faults in run_on_main", "*");
static void on_main(void *u) { (void)u; v5_cmds = 100 / g_zero; }
V3X_EXPORT int v3x_plugin_load(const V3X_API *api) { return api->on_frame(count_frames, NULL) || api->run_on_main(on_main, NULL); }
#elif defined(V5_LOAD_FAULT) || defined(V5_LOAD_FAIL)
#ifdef V5_LOAD_FAULT
V3X_PLUGIN_MANIFEST("v5_load_fault", "1.0", "test", "faults in v3x_plugin_load", "*");
#else
V3X_PLUGIN_MANIFEST("v5_load_fail", "1.0", "test", "fails v3x_plugin_load", "*");
#endif
static void pre_add(V3X_HookCall *c) { c->args[0] += 100; }
static long long (*orig2)(long long);
static long long det2(long long x) { return orig2(x) + 1000; }
V3X_EXPORT int v3x_plugin_load(const V3X_API *api)
{
    HMODULE host = GetModuleHandleA(NULL);
    void *t1 = (void *)GetProcAddress(host, "host_target"), *t2 = (void *)GetProcAddress(host, "host_target2");
    uint8_t *bytes = (uint8_t *)(void *)GetProcAddress(host, "host_bytes");
    static const uint8_t ee[2] = {0xEE, 0xEE};
    if (!t1 || !t2 || !bytes) return 2;
    if (api->hook_safe(t1, pre_add, NULL, NULL) || api->hook(t2, (void *)det2, (void **)&orig2) || api->patch(bytes, ee, 2))
        return 3;
    api->on_frame(count_frames, NULL);
    v5_events = 1;   /* "got this far" */
#ifdef V5_LOAD_FAULT
    *g_null = 1;
    return 0;
#else
    return 7;
#endif
}
#elif defined(V5_RELOAD)
V3X_PLUGIN_MANIFEST("v5_reload", "1.0", "test", "config_reload + log_at", "*");
static void on_reload(const char *n, void *d, void *u)
{
    (void)n; (void)u;
    v5_reloads++;
    v5_reload_tid = GetCurrentThreadId();
    snprintf(v5_reload_name, sizeof v5_reload_name, "%s", d ? (const char *)d : "(null)");
}
V3X_EXPORT int v3x_plugin_load(const V3X_API *api)
{
    if (!V3X_HAS(api, log_at)) return 1;
    api->log_at(V3X_LOG_ERROR, "v5_reload", "v5-error-line");
    api->log_at(V3X_LOG_WARN, "v5_reload", "v5-warn-line");
    api->log_at(V3X_LOG_INFO, "v5_reload", "v5-info-line");
    api->log_at(V3X_LOG_DEBUG, "v5_reload", "v5-debug-line-hidden");
    api->log_at(V3X_LOG_TRACE, "v5dbg", "v5-trace-line-hidden");
    api->log_at(V3X_LOG_DEBUG, "v5dbg", "v5-debug-line-shown");
    return api->on_event(V3X_EVENT_CONFIG_RELOAD, on_reload, NULL);
}
#elif defined(V5_MARKED)
V3X_EXPORT int v3x_plugin_load(const V3X_API *api) { (void)api; SetEnvironmentVariableA("V3X_V5_MARKED", "1"); return 0; }
#elif defined(V5_GATE_STAR)
V3X_PLUGIN_MANIFEST("v5_gate_star", "1.0", "test", "any build", "*");
V3X_EXPORT int v3x_plugin_load(const V3X_API *api) { (void)api; SetEnvironmentVariableA("V3X_V5_GATE_STAR", "1"); return 0; }
#elif defined(V5_GATE_VER)
V3X_PLUGIN_MANIFEST("v5_gate_ver", "1.0", "test", "exact game_version only", "unknown");
V3X_EXPORT int v3x_plugin_load(const V3X_API *api) { (void)api; SetEnvironmentVariableA("V3X_V5_GATE_VER", "1"); return 0; }
#elif defined(V5_GATE_PREFIX)
V3X_PLUGIN_MANIFEST("v5_gate_prefix", "1.0", "test", "prefix", "no-such;unkn*");
V3X_EXPORT int v3x_plugin_load(const V3X_API *api) { (void)api; SetEnvironmentVariableA("V3X_V5_GATE_PREFIX", "1"); return 0; }
#elif defined(V5_GATE_NONE)
V3X_EXPORT int v3x_plugin_load(const V3X_API *api) { (void)api; SetEnvironmentVariableA("V3X_V5_GATE_NONE", "1"); return 0; }
#else
#error "pick a variant, see the header comment"
#endif
