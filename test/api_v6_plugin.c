/* Test plugins for test/api_v6_test.c (loader 0.6.0 / API v6 containment). One source, one DLL per -D flag.
 * Every DLL writes a marker file "v6_marker_<stem>.txt" (current directory) in DllMain: a plugin the loader
 * refuses BEFORE LoadLibrary must leave no marker, because its DllMain never ran.
 *
 *   -DV6_MAIN       v6_main.dll       well-behaved; no capabilities declared; the test host calls its exported
 *                                     v6_t_* functions, which call the API exactly as a plugin would. Subscribes to
 *                                     plugins_loaded / plugin_disabled / process_exit and records what it gets.
 *   -DV6_OTHER      v6_other.dll      same code, second plugin (owner rules: it must not touch v6_main's things)
 *   -DV6_CAPS_HOOK  v6_caps_hook.dll  manifest capabilities "hook bogus"
 *   -DV6_CAPS_PATCH v6_caps_patch.dll manifest capabilities "patch"
 *   -DV6_FAULT      v6_fault.dll      on_frame faults (write to NULL) on its 2nd call -> "plugin_disabled"
 *   -DV6_QUOTA      v6_quota.dll      registers nothing at load; v6_t_quota(kind, n) registers n callbacks
 *   -DV6_ROLLBACK   v6_rollback.dll   v3x_plugin_load patches host_bytes_r then returns 5 -> rolled back
 *   -DV6_MINAPI     v6_minapi.dll     manifest min_api 99                      (must never be mapped)
 *   -DV6_REQBUILD   v6_reqbuild.dll   REQUIRE_BUILD for a build that is not this one (must never be mapped)
 *   -DV6_NOLOAD     v6_noload.dll     no v3x_plugin_load export                (must never be mapped)
 *   -DV6_PLAIN      v6_plain.dll      well-behaved, marker only; the trust scenarios and the corrupt / truncated
 *                                     copies (made by test/run_all.sh) are built from it */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "v3x.h"

#if defined(V6_MAIN)
#define STEM "v6_main"
#define CAPS ((const char *)0)
#elif defined(V6_OTHER)
#define STEM "v6_other"
#define CAPS ((const char *)0)
#elif defined(V6_CAPS_HOOK)
#define STEM "v6_caps_hook"
#define CAPS "hook bogus"
#elif defined(V6_CAPS_PATCH)
#define STEM "v6_caps_patch"
#define CAPS "patch"
#elif defined(V6_FAULT)
#define STEM "v6_fault"
#define CAPS ((const char *)0)
#elif defined(V6_QUOTA)
#define STEM "v6_quota"
#define CAPS ((const char *)0)
#elif defined(V6_ROLLBACK)
#define STEM "v6_rollback"
#define CAPS ((const char *)0)
#elif defined(V6_MINAPI)
#define STEM "v6_minapi"
#define CAPS ((const char *)0)
#elif defined(V6_REQBUILD)
#define STEM "v6_reqbuild"
#define CAPS ((const char *)0)
#elif defined(V6_NOLOAD)
#define STEM "v6_noload"
#define CAPS ((const char *)0)
#elif defined(V6_PLAIN)
#define STEM "v6_plain"
#define CAPS ((const char *)0)
#else
#error "pick a variant, see the header comment"
#endif

#if defined(V6_MINAPI)
V3X_PLUGIN_MANIFEST_CAPS(STEM, "1.0", "test", "needs API 99", "*", 0u, 99u, CAPS);
#elif defined(V6_REQBUILD)
V3X_PLUGIN_MANIFEST_CAPS(STEM, "1.0", "test", "wants another game build", "no-such-build-1.0", V3X_MANIFEST_REQUIRE_BUILD, 0u, CAPS);
#else
V3X_PLUGIN_MANIFEST_CAPS(STEM, "1.0", "test", "API v6 test plugin", "*", 0u, 0u, CAPS);
#endif

/* DllMain: the proof that mapping happened. */
BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID reserved)
{
    (void)reserved;
    if (why == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        HANDLE f = CreateFileA("v6_marker_" STEM ".txt", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w;
            WriteFile(f, "attached", 8, &w, NULL);
            CloseHandle(f);
        }
    }
    return TRUE;
}

/* A call into the loader must keep this module's return address (the loader attributes by it): no tail calls. */
#define KEEP(expr) do { volatile int r_ = (expr); return r_; } while (0)

static const V3X_API *g;

/* ---- recorded events (exported data: the host reads them with GetProcAddress) */
V3X_EXPORT volatile long v6_pl_count;                 /* "plugins_loaded" received */
V3X_EXPORT volatile unsigned long v6_pl_tid;          /* thread it arrived on */
V3X_EXPORT void *volatile v6_pl_data;
typedef struct PdRec { uint32_t struct_size, reserved; char name[48]; void *module; char reason[160]; unsigned long tid; } PdRec;
V3X_EXPORT PdRec v6_pd[8];                            /* "plugin_disabled" received (first 8) */
V3X_EXPORT volatile long v6_pd_count;
V3X_EXPORT volatile long v6_exit_seen;
V3X_EXPORT volatile long v6_frames_a, v6_frames_b, v6_frames_extra, v6_self_calls;
V3X_EXPORT volatile long v6_ev_a, v6_ev_b, v6_ev_self, v6_cmd_a, v6_cmd_b, v6_cmd_x, v6_pre_count, v6_frames_total;
V3X_EXPORT volatile int v6_load_unhook_entry = -77, v6_load_unhook_peek = -77, v6_load_unhook_exit = -77, v6_load_unhook_null = -77;
V3X_EXPORT volatile int v6_load_patch_rc = -77;
V3X_EXPORT int v6_q_rc[64];                           /* results of v6_t_quota */
V3X_EXPORT volatile long v6_q_calls[64];              /* v6_q_frame / v6_q_event / v6_q_cmd calls per registration index */
V3X_EXPORT const unsigned char v6_ro_data[64] = {1, 2, 3, 4, 5, 6, 7, 8};   /* .rdata: PAGE_READONLY */
V3X_EXPORT unsigned char v6_rw_data[64] = {1, 2, 3, 4};                      /* .data */

static void write_marker(const char *name, const char *text)
{
    HANDLE f = CreateFileA(name, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(f, text, (DWORD)strlen(text), &w, NULL);
    CloseHandle(f);
}

/* ---- callbacks */
V3X_EXPORT void v6_frame_a(void *u) { (void)u; InterlockedIncrement(&v6_frames_a); }
V3X_EXPORT void v6_frame_b(void *u) { (void)u; InterlockedIncrement(&v6_frames_b); }
V3X_EXPORT void v6_frame_extra(void *u) { (void)u; InterlockedIncrement(&v6_frames_extra); }
static void frame_self(void *u)   /* removes itself from inside the callback */
{
    InterlockedIncrement(&v6_self_calls);
    volatile int rc = g->off_frame(frame_self, u);
    (void)rc;
}
static void frame_count(void *u) { (void)u; InterlockedIncrement(&v6_frames_total); }
V3X_EXPORT void v6_ev_cb_a(const char *n, void *d, void *u) { (void)n; (void)d; (void)u; InterlockedIncrement(&v6_ev_a); }
V3X_EXPORT void v6_ev_cb_b(const char *n, void *d, void *u) { (void)n; (void)d; (void)u; InterlockedIncrement(&v6_ev_b); }
static void ev_self(const char *n, void *d, void *u)
{
    (void)d;
    InterlockedIncrement(&v6_ev_self);
    volatile int rc = g->off_event(n, ev_self, u);
    (void)rc;
}
V3X_EXPORT void v6_cmd_cb_a(const char *a, void *u) { (void)a; (void)u; InterlockedIncrement(&v6_cmd_a); }
V3X_EXPORT void v6_cmd_cb_b(const char *a, void *u) { (void)a; (void)u; InterlockedIncrement(&v6_cmd_b); }
static void cmd_x(const char *a, void *u) { (void)a; (void)u; InterlockedIncrement(&v6_cmd_x); }
static void pre_count(V3X_HookCall *c) { (void)c; InterlockedIncrement(&v6_pre_count); }

static void on_plugins_loaded(const char *n, void *d, void *u)
{
    (void)n; (void)u;
    InterlockedIncrement(&v6_pl_count);
    v6_pl_tid = GetCurrentThreadId();
    v6_pl_data = d;
}
static void on_plugin_disabled(const char *n, void *d, void *u)
{
    (void)n; (void)u;
    const V3X_PluginDisabled *p = (const V3X_PluginDisabled *)d;
    long i = InterlockedIncrement(&v6_pd_count) - 1;
    if (!p || i >= 8) return;
    v6_pd[i].struct_size = p->struct_size;
    v6_pd[i].reserved = p->reserved;
    snprintf(v6_pd[i].name, sizeof v6_pd[i].name, "%s", p->name ? p->name : "(null)");
    v6_pd[i].module = p->module;
    snprintf(v6_pd[i].reason, sizeof v6_pd[i].reason, "%s", p->reason ? p->reason : "(null)");
    v6_pd[i].tid = GetCurrentThreadId();
}
static void on_process_exit(const char *n, void *d, void *u)
{
    (void)n; (void)u;
    InterlockedIncrement(&v6_exit_seen);
    char t[32];
    snprintf(t, sizeof t, "%ld", d ? *(const long *)d : -1L);
    write_marker("v6_exit_marker.txt", t);
}

/* ---- exported test functions: each calls the API the way a plugin does and returns the result */
V3X_EXPORT int v6_t_unhook(void *t) { KEEP(g->unhook(t)); }
V3X_EXPORT int v6_t_hook_safe(void *t) { KEEP(g->hook_safe(t, pre_count, NULL, NULL)); }
static long long (*hook_orig)(long long);
static long long hook_detour(long long x) { return hook_orig(x) + 1000; }
V3X_EXPORT int v6_t_hook_raw(void *t) { KEEP(g->hook(t, (void *)hook_detour, (void **)&hook_orig)); }
V3X_EXPORT int v6_t_patch(void *addr, const void *bytes, size_t n) { KEEP((int)g->patch(addr, bytes, n)); }
V3X_EXPORT int v6_t_exec(const char *line) { KEEP(g->exec_command(line)); }
V3X_EXPORT int v6_t_emit(const char *name, void *data) { KEEP(g->emit_event(name, data)); }

/* addresses of the loader's own hooks: 0 = entry point, 1 = PeekMessageW, 2 = RtlExitUserProcess */
static void *core_target(int which)
{
    if (which == 0) {
        uint8_t *b = (uint8_t *)GetModuleHandleA(NULL);
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(b + ((const IMAGE_DOS_HEADER *)b)->e_lfanew);
        return b + nt->OptionalHeader.AddressOfEntryPoint;
    }
    if (which == 1) return (void *)GetProcAddress(GetModuleHandleA("user32.dll"), "PeekMessageW");
    return (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlExitUserProcess");
}
V3X_EXPORT void *v6_t_core_target(int which) { return core_target(which); }
V3X_EXPORT int v6_t_unhook_core(int which) { KEEP(g->unhook(core_target(which))); }
V3X_EXPORT int v6_t_hook_core(int which) { KEEP(g->hook_safe(core_target(which), pre_count, NULL, NULL)); }

V3X_EXPORT int v6_t_on_frame(int which)
{
    KEEP(which == 0 ? g->on_frame(v6_frame_a, (void *)0xA) : which == 1 ? g->on_frame(v6_frame_b, (void *)0xB)
         : which == 2 ? g->on_frame(v6_frame_extra, NULL) : g->on_frame(frame_self, (void *)0x5E));
}
V3X_EXPORT int v6_t_off_frame(int which)
{
    KEEP(which == 0 ? g->off_frame(v6_frame_a, (void *)0xA) : which == 1 ? g->off_frame(v6_frame_b, (void *)0xB)
         : which == 2 ? g->off_frame(v6_frame_extra, NULL) : g->off_frame(NULL, NULL));
}
V3X_EXPORT int v6_t_off_frame_any(v3x_frame_fn fn, void *user) { KEEP(g->off_frame(fn, user)); }
V3X_EXPORT int v6_t_on_event(int which)
{
    KEEP(which == 0 ? g->on_event("v6_ev", v6_ev_cb_a, (void *)0xA) : which == 1 ? g->on_event("v6_ev", v6_ev_cb_b, (void *)0xB)
         : g->on_event("v6_ev_self", ev_self, (void *)0x5E));
}
V3X_EXPORT int v6_t_off_event(int which)
{
    KEEP(which == 0 ? g->off_event("v6_ev", v6_ev_cb_a, (void *)0xA) : which == 1 ? g->off_event("v6_ev", v6_ev_cb_b, (void *)0xB)
         : which == 2 ? g->off_event("v6_ev_self", ev_self, (void *)0x5E) : g->off_event(NULL, v6_ev_cb_a, NULL));
}
V3X_EXPORT int v6_t_off_event_any(const char *name, void (*fn)(const char *, void *, void *), void *user) { KEEP(g->off_event(name, fn, user)); }
V3X_EXPORT int v6_t_reg_cmd(int which)
{
    KEEP(which == 0 ? g->register_command("v6cmd_a", "test a", v6_cmd_cb_a, NULL) : which == 1 ? g->register_command("v6cmd_b", "test b", v6_cmd_cb_b, NULL)
         : g->register_command("v6cmd_x", "test x", cmd_x, NULL));
}
V3X_EXPORT int v6_t_unreg(const char *name) { KEEP(g->unregister_command(name)); }

/* quotas (V6_QUOTA): kind 0 = on_frame, 1 = on_event "v6q_ev<i>", 2 = register_command "v6qc<i>" */
static void q_frame(void *u) { InterlockedIncrement(&v6_q_calls[(intptr_t)u & 63]); }
static void q_event(const char *n, void *d, void *u) { (void)n; (void)d; InterlockedIncrement(&v6_q_calls[(intptr_t)u & 63]); }
static void q_cmd(const char *a, void *u) { (void)a; InterlockedIncrement(&v6_q_calls[(intptr_t)u & 63]); }
static int q_reg(int kind, int i)
{
    char nm[32];
    if (kind == 0) KEEP(g->on_frame(q_frame, (void *)(intptr_t)i));
    if (kind == 1) { snprintf(nm, sizeof nm, "v6q_ev%d", i); KEEP(g->on_event(nm, q_event, (void *)(intptr_t)i)); }
    snprintf(nm, sizeof nm, "v6qc%d", i);
    KEEP(g->register_command(nm, "quota test", q_cmd, (void *)(intptr_t)i));
}
static int q_off(int kind, int i)
{
    char nm[32];
    if (kind == 0) KEEP(g->off_frame(q_frame, (void *)(intptr_t)i));
    if (kind == 1) { snprintf(nm, sizeof nm, "v6q_ev%d", i); KEEP(g->off_event(nm, q_event, (void *)(intptr_t)i)); }
    snprintf(nm, sizeof nm, "v6qc%d", i);
    KEEP(g->unregister_command(nm));
}
/* registers n callbacks; v6_q_rc[i] = result of the i-th; returns how many were accepted */
V3X_EXPORT int v6_t_quota(int kind, int n)
{
    int ok = 0;
    for (int i = 0; i < n && i < 64; i++) { v6_q_rc[i] = q_reg(kind, i); ok += v6_q_rc[i] == 0; }
    return ok;
}
V3X_EXPORT int v6_t_quota_off(int kind, int i) { return q_off(kind, i); }
V3X_EXPORT int v6_t_quota_reg(int kind, int i) { return q_reg(kind, i); }
/* a registration whose callback does not live in this module (executable heap memory): quota must still count it */
V3X_EXPORT int v6_t_quota_thunk(int kind)
{
    static uint8_t *thunk;
    if (!thunk) {
        thunk = (uint8_t *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!thunk) return -99;
        memset(thunk, 0xC3, 64);   /* ret */
    }
    char nm[32];
    if (kind == 0) KEEP(g->on_frame((v3x_frame_fn)(void *)thunk, (void *)(intptr_t)0x77));
    if (kind == 1) { snprintf(nm, sizeof nm, "v6q_thunk"); KEEP(g->on_event(nm, (void (*)(const char *, void *, void *))(void *)thunk, (void *)(intptr_t)0x77)); }
    KEEP(g->register_command("v6qthunk", "quota test", (v3x_command_fn)(void *)thunk, NULL));
}
#if defined(V6_FAULT)
static volatile int *volatile g_null;
static volatile long fault_calls;
V3X_EXPORT volatile long v6_fault_calls;
static void fault_frame(void *u)
{
    (void)u;
    v6_fault_calls = ++fault_calls;
    if (fault_calls == 2) *g_null = 1;
}
#endif

/* ---- v3x_plugin_load */
#if !defined(V6_NOLOAD)
V3X_EXPORT int v3x_plugin_load(const V3X_API *api)
{
    g = api;
    if (!V3X_HAS(api, off_frame) || !V3X_HAS(api, v3x_dir_utf8)) return 1;
#if defined(V6_MAIN)
    /* a core hook cannot be removed by a plugin, even from v3x_plugin_load (the entry hook is live then) */
    v6_load_unhook_entry = api->unhook(core_target(0));
    v6_load_unhook_peek = api->unhook(core_target(1));
    v6_load_unhook_exit = api->unhook(core_target(2));
    v6_load_unhook_null = api->unhook(NULL);
    if (api->on_frame(v6_frame_a, (void *)0xA) || api->on_frame(v6_frame_b, (void *)0xB) || api->on_frame(frame_count, NULL)) return 2;
    if (api->on_event("v6_ev", v6_ev_cb_a, (void *)0xA) || api->on_event("v6_ev", v6_ev_cb_b, (void *)0xB)) return 3;
    if (api->register_command("v6cmd_a", "test a", v6_cmd_cb_a, NULL) || api->register_command("v6cmd_b", "test b", v6_cmd_cb_b, NULL)) return 4;
    if (api->on_event(V3X_EVENT_PLUGINS_LOADED, on_plugins_loaded, NULL) || api->on_event(V3X_EVENT_PLUGIN_DISABLED, on_plugin_disabled, NULL) ||
        api->on_event(V3X_EVENT_PROCESS_EXIT, on_process_exit, NULL))
        return 5;
    return 0;
#elif defined(V6_OTHER) || defined(V6_CAPS_HOOK) || defined(V6_CAPS_PATCH) || defined(V6_PLAIN)
    return api->on_frame(frame_count, NULL);
#elif defined(V6_FAULT)
    return api->on_frame(fault_frame, NULL);
#elif defined(V6_QUOTA)
    return 0;
#elif defined(V6_ROLLBACK)
    uint8_t *bytes = (uint8_t *)(void *)GetProcAddress(GetModuleHandleA(NULL), "host_bytes_r");
    static const uint8_t ee[3] = {0xEE, 0xEE, 0xEE};
    if (!bytes) return 2;
    v6_load_patch_rc = (int)api->patch(bytes, ee, sizeof ee);
    api->on_frame(frame_count, NULL);
    return 5;
#else
    return 0;   /* V6_MINAPI / V6_REQBUILD: never reached */
#endif
}
#endif
