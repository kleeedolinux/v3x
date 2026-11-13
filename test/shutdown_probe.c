/* Test-only plugin for test/shutdown_test.c (never installed). It does at DLL_PROCESS_DETACH what an
 * unsafe plugin would do (api->log, api->print, run_on_main, emit_event, exec_command, hook) and then
 * proves the detach returned by writing <v3x>/shutdown_probe.txt with CreateFileW/WriteFile only:
 *   "detach terminating=1 print=-1 hook=1 run=-1 emit=-1 exec=-1 done\n"
 * (print/run/emit/exec return -1 and hook returns non-zero because the core refuses them at exit).
 * It also starts one plugin thread that blocks forever on an event (never signaled, never waited on).
 * Build: zig cc -shared -Iinclude -o <dir>/v3x/plugins/shutdown_probe.dll test/shutdown_probe.c */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "v3x.h"

V3X_EXPORT const char *v3x_plugin_name = "shutdown_probe";

static const V3X_API *api;
static WCHAR g_marker[MAX_PATH];
static HANDLE g_never;
static char g_out[256]; static int g_n;

static void put(const char *s) { while (*s && g_n < (int)sizeof g_out - 1) g_out[g_n++] = *s++; }
static void put_int(int v)
{
    char d[16]; int k = 0;
    if (v < 0) { put("-"); v = -v; }
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 15);
    while (k) { char c[2] = { d[--k], 0 }; put(c); }
}

static void noop(void *u) { (void)u; }
static void noop_cmd(const char *a, void *u) { (void)a; (void)u; }
static int dummy_target(int x) { return x + 1; }
static int dummy_detour(int x) { return x; }

static DWORD WINAPI blocked_thread(void *u)
{
    (void)u;
    WaitForSingleObject(g_never, INFINITE);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)h;
    if (reason != DLL_PROCESS_DETACH || !api || !g_marker[0]) return TRUE;
    /* deliberately unsafe calls: with the exit guard they must all return at once */
    api->log("shutdown_probe", "detach log %d", 1);
    int pr = api->print("detach print");
    void *orig = 0;
    int hk = api->hook((void *)dummy_target, (void *)dummy_detour, &orig);
    int rn = api->run_on_main(noop, 0);
    int em = api->emit_event("shutdown_probe_event", 0);
    int ex = api->exec_command("help");
    g_n = 0;
    put("detach terminating="); put_int(reserved != NULL);
    put(" print="); put_int(pr);
    put(" hook="); put_int(hk != 0);
    put(" run="); put_int(rn);
    put(" emit="); put_int(em);
    put(" exec="); put_int(ex);
    put(" done\n");
    HANDLE f = CreateFileW(g_marker, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, g_out, (DWORD)g_n, &w, NULL); CloseHandle(f); }
    return TRUE;
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    if (a->api_version < 3) return 1;
    const char *d = a->v3x_dir();
    char p[MAX_PATH]; int i = 0;
    for (; d[i] && i < MAX_PATH - 32; i++) p[i] = d[i];
    const char *n = "shutdown_probe.txt";
    for (int k = 0; n[k]; k++) p[i++] = n[k];
    p[i] = 0;
    MultiByteToWideChar(CP_ACP, 0, p, -1, g_marker, MAX_PATH);
    a->register_command("shutdown_probe", "test-only", noop_cmd, 0);
    a->on_frame(noop, 0);
    g_never = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE t = g_never ? CreateThread(NULL, 0, blocked_thread, NULL, 0, NULL) : NULL;
    if (t) CloseHandle(t);
    a->log("shutdown_probe", "loaded, marker %s, blocked thread %s", p, t ? "started" : "FAILED");
    return 0;
}
