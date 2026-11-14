/* Compatibility plugin for test/compat_test.c. Built once per API generation against the FROZEN
 * header of that generation, so it has exactly the V3X_API layout an old plugin binary has:
 *   -DCOMPAT_V=1  test/compat_v1.h   log, scanning, hook/unhook, patch, on_frame, dirs
 *   -DCOMPAT_V=2  test/compat_v2.h   + register_command, print, run_on_main, exec_command
 *   -DCOMPAT_V=3  test/compat_v3.h   + exec_capture, on_event, emit_event
 *   -DCOMPAT_V=4  test/compat_v4.h   + hook_safe, main_thread_id, game_build_id, enum_commands, manifest, V3X_HAS
 *   -DCOMPAT_V=5  test/compat_v5.h   + log_at, log_level, loader_version, build_matches (V3X_HAS-guarded:
 *                                       on a loader without v5 it degrades to api->log instead of failing)
 *   -DCOMPAT_V=6  include/v3x.h      + off_frame, off_event, unregister_command, wide/UTF-8 dirs, manifest
 *                                       capabilities (V3X_HAS-guarded: on an older loader the v6 part is skipped)
 * Output name: compat_v<N>.dll. Results are exported data the host reads with GetProcAddress:
 *   compat_result  "ok" or "FAIL <first failed expression>"
 *   compat_frames / compat_cmds / compat_events / compat_main   callback counters
 *   compat_degraded  (v5) 1 = ran on a loader without API v5
 *   compat_probe(api)  (v5) 1 if V3X_HAS sees log_at in this table and the v5 calls work, 0 if absent
 *   compat_v6_ok       (v6) 1 = the v6 calls worked, 0 = ran on a loader without API v6 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#if COMPAT_V == 1
#include "compat_v1.h"
#elif COMPAT_V == 2
#include "compat_v2.h"
#elif COMPAT_V == 3
#include "compat_v3.h"
#elif COMPAT_V == 4
#include "compat_v4.h"
#elif COMPAT_V == 5
#include "compat_v5.h"
#elif COMPAT_V == 6
#include "../include/v3x.h"
#else
#error "build with -DCOMPAT_V=1..6"
#endif

#define STR2_(x) #x
#define STR_(x) STR2_(x)
#define NAME "compat_v" STR_(COMPAT_V)

V3X_EXPORT volatile long compat_frames, compat_cmds, compat_events, compat_main, compat_degraded;
V3X_EXPORT char compat_result[200];
V3X_EXPORT const char *v3x_plugin_name = NAME;
#if COMPAT_V >= 6
V3X_PLUGIN_MANIFEST_CAPS(NAME, "1.0." STR_(COMPAT_V), "v3x", "compat test plugin", "*", 0u, 0u, "hook patch exec");
V3X_EXPORT volatile long compat_v6_ok;
#elif COMPAT_V >= 4
V3X_PLUGIN_MANIFEST(NAME, "1.0." STR_(COMPAT_V), "v3x", "compat test plugin", "*");
#endif

static void fail(const char *what)
{
    if (!strcmp(compat_result, "ok")) snprintf(compat_result, sizeof compat_result, "FAIL %s", what);
}
#define REQ(c) do { if (!(c)) fail(#c); } while (0)

static volatile long sink_;
__attribute__((noinline)) int victim(int a, int b)
{
    sink_ += a;
    sink_ ^= b;
    return a * 3 + b;
}
static int (*volatile p_victim)(int, int) = victim;
static int (*victim_orig)(int, int);
static int victim_detour(int a, int b) { return victim_orig(a, b) + 1; }
static uint8_t patch_buf[8] = {1, 2, 3, 4};

static void on_frame(void *u) { (void)u; compat_frames++; }
#if COMPAT_V >= 2
static void on_cmd(const char *args, void *u) { (void)args; (void)u; compat_cmds++; }
static void on_main(void *u) { (void)u; compat_main++; }
#endif
#if COMPAT_V >= 3
static void on_ev(const char *name, void *data, void *u) { (void)name; (void)data; (void)u; compat_events++; }
#endif
#if COMPAT_V >= 4
static void pre_add(V3X_HookCall *c) { c->args[0] += 1; }
#endif

#if COMPAT_V >= 5
V3X_EXPORT int compat_probe(const V3X_API *api)
{
    if (!V3X_HAS(api, log_at) || !V3X_HAS(api, build_matches)) {
        api->log(NAME, "loader has no API v5 (api v%u, struct_size %u): plain log", api->api_version, api->struct_size);
        return 0;
    }
    int ok = 1;
    api->log_at(V3X_LOG_INFO, NAME, "loader %s, api v%u", api->loader_version(), api->api_version);
    ok &= api->log_level(NAME) >= V3X_LOG_ERROR;
    ok &= api->loader_version() && api->loader_version()[0] != 0;
    ok &= api->build_matches("*") == 1 && api->build_matches("no-such-build") == 0;
    return ok;
}
#endif

V3X_EXPORT int v3x_plugin_load(const V3X_API *api)
{
    strcpy(compat_result, "ok");
    REQ(api && api->api_version >= 1);
#if COMPAT_V < 5
    REQ(api->api_version >= COMPAT_V);
    REQ(api->struct_size >= sizeof(V3X_API));   /* the current loader's table covers this old layout */
#endif
    /* ---- v1 */
    api->log(NAME, "compat plugin built for API v%d on loader api v%u (struct_size %u)", COMPAT_V, api->api_version,
             api->struct_size);
    REQ(api->image_base() != 0 && api->image_size() > 0x1000);
    REQ(api->game_version() != NULL);
    REQ(api->game_dir() != NULL && api->v3x_dir() != NULL && strstr(api->v3x_dir(), "v3x") != NULL);
    void *s = api->find_string("v3x_compat_host_marker_string");
    REQ(s != NULL);
    void *x = s ? api->find_xref(s, 0) : NULL;
    REQ(x != NULL);
    REQ(x == NULL || api->function_start(x) != NULL);
    REQ(api->hook((void *)victim, (void *)victim_detour, (void **)&victim_orig) == 0);
    REQ(p_victim(2, 1) == 8);
    REQ(api->unhook((void *)victim) == 0);
    REQ(p_victim(2, 1) == 7);
    uint8_t nb[2] = {9, 9};
    REQ(api->patch(patch_buf, nb, 2) == 0 && patch_buf[0] == 9 && patch_buf[2] == 3);
    REQ(api->on_frame(on_frame, NULL) == 0);
#if COMPAT_V >= 2
    REQ(api->register_command(NAME, "compat test command", on_cmd, NULL) == 0);
    REQ(api->register_command(NAME, "again", on_cmd, NULL) == -2);
    REQ(api->print("%s ready", NAME) > 0);
    REQ(api->run_on_main(on_main, NULL) == 0);
#endif
#if COMPAT_V >= 3
    REQ(api->on_event("compat_ping", on_ev, NULL) == 0);
    REQ(api->on_event("compat_ping", on_ev, NULL) == -2);
#endif
#if COMPAT_V >= 4
    REQ(V3X_HAS(api, hook_safe) && V3X_HAS(api, enum_commands));
    REQ(api->hook_safe((void *)victim, pre_add, NULL, NULL) == 0);
    REQ(p_victim(2, 1) == 10);
    REQ(api->unhook((void *)victim) == 0);
    REQ(p_victim(2, 1) == 7);
    REQ(api->main_thread_id() == GetCurrentThreadId());
    REQ(api->game_build_id() && api->game_build_id()[0]);
#endif
#if COMPAT_V >= 5
    compat_degraded = !compat_probe(api);
    if (!compat_degraded) REQ(api->api_version >= 5);
#endif
#if COMPAT_V >= 6
    if (V3X_HAS(api, off_frame) && V3X_HAS(api, v3x_dir_utf8)) {
        static int dummy;
        REQ(api->off_frame(on_frame, &dummy) == V3X_ENOTFOUND);
        REQ(api->on_frame(on_frame, &dummy) == 0 && api->off_frame(on_frame, &dummy) == 0);
        REQ(api->off_event("compat_ping", on_ev, &dummy) == V3X_ENOTFOUND);
        REQ(api->register_command("compat_v6_tmp", "x", on_cmd, NULL) == 0 && api->unregister_command("compat_v6_tmp") == 0);
        REQ(api->unregister_command("help") == V3X_EPERM);
        REQ(api->v3x_dir_w() && wcsstr(api->v3x_dir_w(), L"v3x") && api->v3x_dir_utf8() && strstr(api->v3x_dir_utf8(), "v3x"));
        REQ(api->game_dir_w() && api->game_dir_utf8() && api->game_dir_utf8()[0]);
        REQ(api->unhook(NULL) == V3X_EARG);
        compat_v6_ok = 1;
    }
#endif
    return 0;
}
