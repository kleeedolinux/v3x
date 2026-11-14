/* ABI freeze of include/v3x.h (docs/api-stability.md). Every public struct layout, field offset,
 * function-pointer signature and constant is pinned here with _Static_assert, so an accidental
 * change (reordering, resizing, removing, retyping a field, renumbering a constant) FAILS TO COMPILE.
 * Appending a new V3X_API field at the end is the only allowed change: add its offset/type line
 * below, bump V3X_API_VERSION, and update V3X_ABI_LAST_FIELD.
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -o $T/abi.exe test/abi_test.c && $T/abi.exe
 * (it also includes sdk/v3x_script.h and plugins/updater/updater.h by relative path: their public structs
 * V3X_ScriptExt, V3X_ScriptScope and V3X_UpdateInfo are pinned too)
 * The program itself only re-checks the event-name strings and prints the layout table.
 * Exit code 0 = ALL OK. */
#include <stdio.h>
#include <string.h>
#include "v3x.h"   /* -Iinclude; run_all.sh also compiles it against a mutated copy and expects a failure */
#include "../sdk/v3x_script.h"          /* V3X_ScriptExt / V3X_ScriptScope (script_ext's table) */
#include "../plugins/updater/updater.h" /* V3X_UpdateInfo ("update_available" event payload) */

#define SA(c) _Static_assert(c, #c)
#define OFF(T, f, o) SA(offsetof(T, f) == (o))
#define TYPE(T, f, ...) SA(__builtin_types_compatible_p(__typeof__(((T *)0)->f), __VA_ARGS__))
#define API_FN(i, f, ...) OFF(V3X_API, f, 8 + 8 * (i)); TYPE(V3X_API, f, __VA_ARGS__)

/* ---- platform assumptions the ABI is built on (Win64, LLP64) */
SA(sizeof(void *) == 8);
SA(sizeof(long) == 4);
SA(sizeof(int) == 4);
SA(sizeof(double) == 8 && sizeof(float) == 4);

/* ---- constants (values are part of the contract) */
SA(V3X_API_VERSION == 6);
SA(V3X_HOOK_MAX_ARGS == 16);
SA(V3X_HOOK_MAX_SUBSCRIBERS == 8);
SA(V3X_EHOOKBUSY == 100);
SA(V3X_MANIFEST_REQUIRE_BUILD == 1u);
SA(V3X_OK == 0 && V3X_EFAIL == -1 && V3X_EEXIST == -2);
SA(V3X_EARG == -1 && V3X_ENOTFOUND == -3 && V3X_EPERM == -4);   /* v6 */
SA(V3X_QUOTA_FRAME == 16 && V3X_QUOTA_EVENTS == 24 && V3X_QUOTA_COMMANDS == 24 && V3X_PATCH_MAX == 65536);   /* v6 */
SA(V3X_EMH_NOT_INITIALIZED == 2 && V3X_EMH_ALREADY_CREATED == 3 && V3X_EMH_NOT_CREATED == 4);
SA(V3X_EMH_NOT_EXECUTABLE == 7 && V3X_EMH_UNSUPPORTED_FUNCTION == 8 && V3X_EMH_MEMORY_ALLOC == 9);
SA(V3X_EMH_MEMORY_PROTECT == 10);
SA(V3X_LOG_ERROR == 1 && V3X_LOG_WARN == 2 && V3X_LOG_INFO == 3 && V3X_LOG_DEBUG == 4 && V3X_LOG_TRACE == 5);

/* ---- callback types */
SA(__builtin_types_compatible_p(v3x_frame_fn, void (*)(void *)));
SA(__builtin_types_compatible_p(v3x_command_fn, void (*)(const char *, void *)));
SA(__builtin_types_compatible_p(v3x_hook_fn, void (*)(V3X_HookCall *)));
SA(__builtin_types_compatible_p(v3x_plugin_load_fn, int (*)(const V3X_API *)));
SA(__builtin_types_compatible_p(v3x_api_fn, const V3X_API *(*)(void)));

/* ---- V3X_Xmm */
SA(sizeof(V3X_Xmm) == 16);
OFF(V3X_Xmm, d, 0); OFF(V3X_Xmm, f, 0); OFF(V3X_Xmm, u64, 0); OFF(V3X_Xmm, u32, 0); OFF(V3X_Xmm, b, 0);

/* ---- V3X_HookCall (filled by the core's thunk, src/safe_hook.c, at these exact offsets) */
OFF(V3X_HookCall, args, 0x00);           TYPE(V3X_HookCall, args, uint64_t[16]);
OFF(V3X_HookCall, xmm, 0x80);            TYPE(V3X_HookCall, xmm, V3X_Xmm[4]);
OFF(V3X_HookCall, ret, 0xc0);            TYPE(V3X_HookCall, ret, uint64_t);
OFF(V3X_HookCall, skip_original, 0xc8);  TYPE(V3X_HookCall, skip_original, int32_t);
OFF(V3X_HookCall, reserved, 0xcc);       TYPE(V3X_HookCall, reserved, int32_t);
OFF(V3X_HookCall, xmm_ret, 0xd0);        TYPE(V3X_HookCall, xmm_ret, V3X_Xmm);
OFF(V3X_HookCall, user, 0xe0);           TYPE(V3X_HookCall, user, void *);
OFF(V3X_HookCall, target, 0xe8);         TYPE(V3X_HookCall, target, void *);
OFF(V3X_HookCall, original, 0xf0);       TYPE(V3X_HookCall, original, void *);
OFF(V3X_HookCall, return_address, 0xf8); TYPE(V3X_HookCall, return_address, void *);
OFF(V3X_HookCall, scratch, 0x100);       TYPE(V3X_HookCall, scratch, uint64_t[2]);
SA(sizeof(V3X_HookCall) == 0x110);

/* ---- V3X_PluginManifest (read by the loader from the plugin's data export) */
OFF(V3X_PluginManifest, struct_size, 0);  TYPE(V3X_PluginManifest, struct_size, uint32_t);
OFF(V3X_PluginManifest, flags, 4);        TYPE(V3X_PluginManifest, flags, uint32_t);
OFF(V3X_PluginManifest, min_api, 8);      TYPE(V3X_PluginManifest, min_api, uint32_t);
OFF(V3X_PluginManifest, reserved, 12);    TYPE(V3X_PluginManifest, reserved, uint32_t);
OFF(V3X_PluginManifest, name, 16);        TYPE(V3X_PluginManifest, name, const char *);
OFF(V3X_PluginManifest, version, 24);     TYPE(V3X_PluginManifest, version, const char *);
OFF(V3X_PluginManifest, author, 32);      TYPE(V3X_PluginManifest, author, const char *);
OFF(V3X_PluginManifest, description, 40); TYPE(V3X_PluginManifest, description, const char *);
OFF(V3X_PluginManifest, game_builds, 48); TYPE(V3X_PluginManifest, game_builds, const char *);
OFF(V3X_PluginManifest, capabilities, 56); TYPE(V3X_PluginManifest, capabilities, const char *);   /* v6, appended */
SA(sizeof(V3X_PluginManifest) == 64);

/* ---- event payloads */
OFF(V3X_LoadTransition, from, 0); OFF(V3X_LoadTransition, to, 8); OFF(V3X_LoadTransition, seconds, 16);
TYPE(V3X_LoadTransition, from, const char *); TYPE(V3X_LoadTransition, seconds, double);
SA(sizeof(V3X_LoadTransition) == 24);
OFF(V3X_SaveBegin, name, 0); TYPE(V3X_SaveBegin, name, const char *);
SA(sizeof(V3X_SaveBegin) == 8);
OFF(V3X_PluginDisabled, struct_size, 0); TYPE(V3X_PluginDisabled, struct_size, uint32_t);   /* v6 */
OFF(V3X_PluginDisabled, reserved, 4); OFF(V3X_PluginDisabled, name, 8); TYPE(V3X_PluginDisabled, name, const char *);
OFF(V3X_PluginDisabled, module, 16); TYPE(V3X_PluginDisabled, module, void *);
OFF(V3X_PluginDisabled, reason, 24); TYPE(V3X_PluginDisabled, reason, const char *);
SA(sizeof(V3X_PluginDisabled) == 32);

/* ---- SDK / plugin tables other plugins bind to (audit action 14; values measured by fix-sdk) */
SA(sizeof(V3X_ScriptExt) == 80); SA(offsetof(V3X_ScriptExt, struct_size) == 0); SA(offsetof(V3X_ScriptExt, version) == 4);
SA(offsetof(V3X_ScriptExt, begin_pack) == 8); SA(offsetof(V3X_ScriptExt, write_file) == 16);
SA(offsetof(V3X_ScriptExt, end_pack) == 24); SA(offsetof(V3X_ScriptExt, packs_dir) == 32);
SA(offsetof(V3X_ScriptExt, features) == 40); SA(offsetof(V3X_ScriptExt, run_effect_text) == 48);
SA(offsetof(V3X_ScriptExt, eval_trigger_text) == 56); SA(offsetof(V3X_ScriptExt, register_effect) == 64);
SA(offsetof(V3X_ScriptExt, register_trigger) == 72); SA(V3X_SCRIPT_VERSION == 2);
SA(sizeof(V3X_ScriptScope) == 16); SA(offsetof(V3X_ScriptScope, type) == 0); SA(offsetof(V3X_ScriptScope, reserved) == 2);
SA(offsetof(V3X_ScriptScope, id) == 8); SA(V3X_SCOPE_COUNTRY == 5);
SA(sizeof(V3X_UpdateInfo) == 40); SA(offsetof(V3X_UpdateInfo, struct_size) == 0); SA(offsetof(V3X_UpdateInfo, state) == 4);
SA(offsetof(V3X_UpdateInfo, installed) == 8); SA(offsetof(V3X_UpdateInfo, available) == 16);
SA(offsetof(V3X_UpdateInfo, message) == 24); SA(offsetof(V3X_UpdateInfo, auto_install) == 32);
SA(offsetof(V3X_UpdateInfo, ready) == 36); SA(V3X_UPDATE_AVAILABLE == 3); SA(V3X_UPDATE_URGENT == 5);

/* ---- V3X_API: header, then one 8-byte function pointer per slot, in this order forever */
OFF(V3X_API, api_version, 0); TYPE(V3X_API, api_version, uint32_t);
OFF(V3X_API, struct_size, 4); TYPE(V3X_API, struct_size, uint32_t);
/* v1 */
API_FN(0, log, void (*)(const char *, const char *, ...));
API_FN(1, image_base, uintptr_t (*)(void));
API_FN(2, image_size, size_t (*)(void));
API_FN(3, game_version, const char *(*)(void));
API_FN(4, pattern_scan, void *(*)(const char *));
API_FN(5, find_string, void *(*)(const char *));
API_FN(6, find_xref, void *(*)(const void *, int));
API_FN(7, function_start, void *(*)(const void *));
API_FN(8, hook, int (*)(void *, void *, void **));
API_FN(9, unhook, int (*)(void *));
API_FN(10, patch, int (*)(void *, const void *, size_t));
API_FN(11, on_frame, int (*)(v3x_frame_fn, void *));
API_FN(12, game_dir, const char *(*)(void));
API_FN(13, v3x_dir, const char *(*)(void));
#define V3X_ABI_V1_SIZE (8 + 8 * 14)
/* v2 */
API_FN(14, register_command, int (*)(const char *, const char *, v3x_command_fn, void *));
API_FN(15, print, int (*)(const char *, ...));
API_FN(16, run_on_main, int (*)(v3x_frame_fn, void *));
API_FN(17, exec_command, int (*)(const char *));
#define V3X_ABI_V2_SIZE (8 + 8 * 18)
/* v3 */
API_FN(18, exec_capture, int (*)(const char *, void (*)(const char *, void *), void *));
API_FN(19, on_event, int (*)(const char *, void (*)(const char *, void *, void *), void *));
API_FN(20, emit_event, int (*)(const char *, void *));
#define V3X_ABI_V3_SIZE (8 + 8 * 21)
/* v4 */
API_FN(21, hook_safe, int (*)(void *, v3x_hook_fn, v3x_hook_fn, void *));
API_FN(22, main_thread_id, uint32_t (*)(void));
API_FN(23, game_build_id, const char *(*)(void));
API_FN(24, enum_commands, int (*)(void (*)(const char *, const char *, void *), void *));
#define V3X_ABI_V4_SIZE (8 + 8 * 25)
/* v5 */
API_FN(25, log_at, void (*)(int, const char *, const char *, ...));
API_FN(26, log_level, int (*)(const char *));
API_FN(27, loader_version, const char *(*)(void));
API_FN(28, build_matches, int (*)(const char *));
#define V3X_ABI_V5_SIZE (8 + 8 * 29)
/* v6 */
API_FN(29, off_frame, int (*)(v3x_frame_fn, void *));
API_FN(30, off_event, int (*)(const char *, void (*)(const char *, void *, void *), void *));
API_FN(31, unregister_command, int (*)(const char *));
API_FN(32, game_dir_w, const wchar_t *(*)(void));
API_FN(33, v3x_dir_w, const wchar_t *(*)(void));
API_FN(34, game_dir_utf8, const char *(*)(void));
API_FN(35, v3x_dir_utf8, const char *(*)(void));
#define V3X_ABI_V6_SIZE (8 + 8 * 36)
#define V3X_ABI_LAST_FIELD v3x_dir_utf8
/* nothing after the last pinned field: a new field must be pinned above first */
SA(sizeof(V3X_API) == V3X_ABI_V6_SIZE);
SA(sizeof(V3X_API) == offsetof(V3X_API, V3X_ABI_LAST_FIELD) + sizeof(void *));

/* ---- V3X_HAS: struct_size-based, so it is false for fields beyond an older table */
static int has_check(void)
{
    static V3X_API t;
    int ok = 1;
    t.log = (void (*)(const char *, const char *, ...))1;
    t.hook_safe = (int (*)(void *, v3x_hook_fn, v3x_hook_fn, void *))1;
    t.build_matches = (int (*)(const char *))1;
    t.struct_size = V3X_ABI_V1_SIZE;
    ok &= V3X_HAS(&t, log) && !V3X_HAS(&t, hook_safe);
    t.struct_size = V3X_ABI_V4_SIZE;
    ok &= V3X_HAS(&t, hook_safe) && !V3X_HAS(&t, build_matches);
    t.struct_size = V3X_ABI_V5_SIZE;
    ok &= V3X_HAS(&t, build_matches) && !V3X_HAS(&t, log_at);   /* NULL field = absent */
    t.off_frame = (int (*)(v3x_frame_fn, void *))1;
    ok &= !V3X_HAS(&t, off_frame);                               /* beyond a v5 table */
    t.struct_size = V3X_ABI_V6_SIZE;
    ok &= V3X_HAS(&t, off_frame) && !V3X_HAS(&t, v3x_dir_utf8);
    ok &= !V3X_HAS((const V3X_API *)NULL, log);
    return ok;
}

int main(void)
{
    int fails = 0;
#define RCHK(c, ...) do { int ok_ = !!(c); printf("%s: ", ok_ ? "OK  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); fails += !ok_; } while (0)
    RCHK(1, "compile-time layout checks passed (every offset, type and constant above)");
    RCHK(!strcmp(V3X_EVENT_PROCESS_EXIT, "process_exit") && !strcmp(V3X_EVENT_CONFIG_RELOAD, "config_reload") &&
             !strcmp(V3X_EVENT_LOAD_TRANSITION, "load_transition") &&
             !strcmp(V3X_EVENT_LOAD_TRANSITION_BEGIN, "load_transition_begin") &&
             !strcmp(V3X_EVENT_SAVE_BEGIN, "save_begin") && !strcmp(V3X_EVENT_SAVE_END, "save_end") &&
             !strcmp(V3X_EVENT_PLUGIN_DISABLED, "plugin_disabled") && !strcmp(V3X_EVENT_PLUGINS_LOADED, "plugins_loaded"),
         "event names unchanged");
    RCHK(has_check(), "V3X_HAS honours struct_size and NULL fields");
    printf("table sizes: v1 %d, v2 %d, v3 %d, v4 %d, v5 %d, v6 %d bytes; V3X_HookCall 0x%zx, V3X_PluginManifest %zu\n",
           V3X_ABI_V1_SIZE, V3X_ABI_V2_SIZE, V3X_ABI_V3_SIZE, V3X_ABI_V4_SIZE, V3X_ABI_V5_SIZE, V3X_ABI_V6_SIZE, sizeof(V3X_HookCall),
           sizeof(V3X_PluginManifest));
    printf("%d checks, %d failed\n%s\n", 3, fails, fails ? "FAILED" : "ALL OK");
    return fails ? 1 : 0;
}
