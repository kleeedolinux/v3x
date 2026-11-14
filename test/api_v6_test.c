/* End-to-end test of the API v6 containment features of loader 0.6.0 (no game): the loader must protect the
 * game from buggy plugins. Links version.dll like victoria3.exe; the loader boots and loads every DLL in
 * <dir>/v3x/plugins (variants of test/api_v6_plugin.c). The host calls the exported v6_t_* test functions of
 * those plugins, which call the API like a plugin does; calls "from nowhere" (code in no module) go through a
 * tiny thunk in VirtualAlloc'd memory.
 *
 *   api_v6_test.exe <trustdir>   everything; <trustdir> is a second directory (exe + version.dll + v6_plain.dll)
 *                                in which the trust scenarios run in child processes with other environments
 *   api_v6_test.exe idle         child mode: boots the loader and exits
 *   api_v6_test.exe trustcmd     child mode: runs the `trust` console command (see test_trust)
 *   api_v6_test.exe exitchild    child mode: tries to unhook the exit hook, then ExitProcess(7)
 * Needs V3X_TRUST=dev (the plugins are not signed; test/run_all.sh exports it) and V3X_NO_CONSOLE=1.
 * Exit code 0 = ALL OK. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/v3x.h"
#include "../src/v3x_pe.h"

__declspec(dllimport) const V3X_API *v3x_api(void);

static int fails, checks;
#define CHECK(c, ...) do { int ok_ = !!(c); checks++; printf("%s: ", ok_ ? "OK  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); fails += !ok_; } while (0)

/* ---- what the plugins hook / patch (exports of this exe, found with GetProcAddress) */
static volatile long long sink_;
__declspec(dllexport) __attribute__((noinline)) long long host_target(long long x) { sink_ += x; return x * 2; }
__declspec(dllexport) __attribute__((noinline)) long long host_target2(long long x) { sink_ ^= x; return x * 5; }
__declspec(dllexport) __attribute__((noinline)) long long host_target3(long long x) { sink_ -= x; return x * 3; }
__declspec(dllexport) __attribute__((noinline)) long long host_target4(long long x) { sink_ |= x; return x * 7; }
static long long (*volatile p_t1)(long long) = host_target;
static long long (*volatile p_t2)(long long) = host_target2;
static long long (*volatile p_t3)(long long) = host_target3;
static long long (*volatile p_t4)(long long) = host_target4;
__declspec(dllexport) volatile unsigned char host_bytes[64] = {0x11, 0x22, 0x33};
__declspec(dllexport) volatile unsigned char host_bytes_r[16] = {0x11, 0x22, 0x33};   /* v6_rollback patches these */
__declspec(dllexport) const unsigned char host_ro[64] = {9, 8, 7, 6};
__attribute__((aligned(4096))) static unsigned char host_big[70000];
static unsigned char host_src[70000];
__declspec(dllexport) __attribute__((noinline, aligned(64))) int host_code_fn(int x) { return x * 11 + 3; }
static int (*volatile p_code)(int) = host_code_fn;
static const char host_marker[] = "v3x_v6_host_marker_string";

/* ---- helpers */
static void *sym(const char *dll, const char *name)
{
    HMODULE m = GetModuleHandleA(dll);
    return m ? (void *)GetProcAddress(m, name) : NULL;
}
static long val(const char *dll, const char *name)
{
    volatile long *p = (volatile long *)sym(dll, name);
    return p ? *p : -12345;
}
typedef int (*fn_p_t)(void *);
typedef int (*fn_i_t)(int);
typedef int (*fn_ii_t)(int, int);
typedef int (*fn_s_t)(const char *);
typedef int (*fn_patch_t)(void *, const void *, size_t);
typedef int (*fn_ps_t)(void *, void *);
#define MISSING (-99999)
static int call_p(const char *dll, const char *fn, void *a) { fn_p_t f = (fn_p_t)sym(dll, fn); return f ? f(a) : MISSING; }
static int call_i(const char *dll, const char *fn, int a) { fn_i_t f = (fn_i_t)sym(dll, fn); return f ? f(a) : MISSING; }
static int call_ii(const char *dll, const char *fn, int a, int b) { fn_ii_t f = (fn_ii_t)sym(dll, fn); return f ? f(a, b) : MISSING; }
static int call_s(const char *dll, const char *fn, const char *a) { fn_s_t f = (fn_s_t)sym(dll, fn); return f ? f(a) : MISSING; }
static int call_patch(const char *dll, void *addr, const void *bytes, size_t n)
{
    fn_patch_t f = (fn_patch_t)sym(dll, "v6_t_patch");
    return f ? f(addr, bytes, n) : MISSING;
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
static char *read_text(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(buf, 1, cap - 1, f) : 0;
    if (f) fclose(f);
    buf[n] = 0;
    return buf;
}
static char *read_log(void)
{
    static char log[524288];
    return read_text("v3x/v3x.log", log, sizeof log);
}
static int file_exists(const char *path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }
static DWORD prot_of(const void *p)
{
    MEMORY_BASIC_INFORMATION m;
    return VirtualQuery(p, &m, sizeof m) ? m.Protect : 0xFFFFFFFFu;
}

/* a thunk in VirtualAlloc'd memory: calls fn with the caller's first four arguments. The loader sees the
 * return address inside it: no module, and no plugin callback runs on this thread = the "unknown owner". */
static void *make_thunk(void *fn)
{
    uint8_t *m = (uint8_t *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    static const uint8_t pre[] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0xB8}, post[] = {0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x28, 0xC3};
    if (!m) return NULL;
    memcpy(m, pre, sizeof pre);
    memcpy(m + sizeof pre, &fn, 8);
    memcpy(m + sizeof pre + 8, post, sizeof post);
    return m;
}
/* the hook_safe stub (hook stub pool) a hooked function jumps to: target -> jmp rel32 -> MinHook relay (jmp [rip+0]) -> stub */
static void *stub_of(void *target)
{
    const uint8_t *p = (const uint8_t *)target;
    int32_t rel, d;
    void *abs;
    if (p[0] != 0xE9) return NULL;
    memcpy(&rel, p + 1, 4);
    const uint8_t *relay = p + 5 + rel;
    if (relay[0] != 0xFF || relay[1] != 0x25) return NULL;
    memcpy(&d, relay + 2, 4);
    memcpy(&abs, relay + 6 + d, 8);
    return abs;
}

/* ---- children */
static int run_child(const char *dir, const char *mode, const char *trust_env, const char *localappdata, DWORD *code)
{
    char old_trust[64] = "", old_la[MAX_PATH] = "";
    int had_trust = (int)GetEnvironmentVariableA("V3X_TRUST", old_trust, sizeof old_trust) > 0;
    int had_la = (int)GetEnvironmentVariableA("LOCALAPPDATA", old_la, sizeof old_la) > 0;
    SetEnvironmentVariableA("V3X_TRUST", trust_env);          /* NULL = unset */
    SetEnvironmentVariableA("LOCALAPPDATA", localappdata);
    char cmd[MAX_PATH * 2], cwd[MAX_PATH];
    snprintf(cwd, sizeof cwd, "%s", dir);
    snprintf(cmd, sizeof cmd, "\"%s\\api_v6_test.exe\" %s", dir, mode);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, cwd, &si, &pi);
    SetEnvironmentVariableA("V3X_TRUST", had_trust ? old_trust : NULL);
    SetEnvironmentVariableA("LOCALAPPDATA", had_la ? old_la : NULL);
    if (!ok) { printf("cannot start %s (error %lu)\n", cmd, GetLastError()); *code = 0xFFFF; return 0; }
    if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0) { TerminateProcess(pi.hProcess, 0xDEAD); *code = 0xDEAD; }
    else GetExitCodeProcess(pi.hProcess, code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 1;
}

/* child mode `trustcmd`: the console command `trust` lists the refused plugin with its hash and `trust <plugin>`
 * appends "<sha256> <file>" to %LOCALAPPDATA%\V3X\trusted_plugins.txt. 0 = all as expected. */
static int trustcmd_child(const V3X_API *api)
{
    int bad = 0;
    static uint8_t dll[8u << 20];
    FILE *f = fopen("v3x/plugins/v6_plain.dll", "rb");
    size_t n = f ? fread(dll, 1, sizeof dll, f) : 0;
    if (f) fclose(f);
    char want[65];
    v3x_sha256_hex(dll, n, want);
    const char *out = run(api, "trust");
    char needle[96];
    snprintf(needle, sizeof needle, "sha256 %s", want);
    printf("trustcmd: `trust` -> %s", out);
    if (!line_has(out, "v6_plain", needle)) { printf("trustcmd FAIL: `trust` does not list v6_plain with sha256 %s\n", want); bad++; }
    out = run(api, "trust nosuchplugin");
    if (!strstr(out, "was not refused")) { printf("trustcmd FAIL: `trust nosuchplugin` -> %s\n", out); bad++; }
    out = run(api, "trust v6_plain");
    printf("trustcmd: `trust v6_plain` -> %s", out);
    if (!strstr(out, "trusted v6_plain.dll")) { printf("trustcmd FAIL: `trust v6_plain` did not confirm\n"); bad++; }
    char la[MAX_PATH], path[MAX_PATH * 2], txt[4096];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", la, sizeof la)) { printf("trustcmd FAIL: no LOCALAPPDATA\n"); return 1; }
    snprintf(path, sizeof path, "%s\\V3X\\trusted_plugins.txt", la);
    read_text(path, txt, sizeof txt);
    snprintf(needle, sizeof needle, "%s v6_plain.dll", want);
    if (!strstr(txt, needle)) { printf("trustcmd FAIL: %s does not contain \"%s\" (%s)\n", path, needle, txt); bad++; }
    printf("trustcmd: %s\n", bad ? "FAILED" : "ok");
    return bad;
}

static void test_trust(const char *dir)
{
    char path[MAX_PATH * 2], la1[MAX_PATH], la2[MAX_PATH], la3[MAX_PATH], la4[MAX_PATH], log[262144], mk[MAX_PATH * 2], ini[MAX_PATH * 2];
    snprintf(mk, sizeof mk, "%s\\v6_marker_v6_plain.txt", dir);
    snprintf(path, sizeof path, "%s\\v3x\\v3x.log", dir);
    snprintf(ini, sizeof ini, "%s\\v3x\\v3x.ini", dir);
    snprintf(la1, sizeof la1, "%s\\la1", dir); snprintf(la2, sizeof la2, "%s\\la2", dir);
    snprintf(la3, sizeof la3, "%s\\la3", dir); snprintf(la4, sizeof la4, "%s\\la4", dir);
    CreateDirectoryA(la1, NULL); CreateDirectoryA(la2, NULL); CreateDirectoryA(la3, NULL); CreateDirectoryA(la4, NULL);
    DeleteFileA(mk); DeleteFileA(ini);
    DWORD code = 0;

    /* S1: no V3X_TRUST, empty per-user list, no signed plugins.json: refused; `trust v6_plain` stores its hash */
    CHECK(run_child(dir, "trustcmd", NULL, la1, &code) && code == 0, "trust S1: untrusted plugin: the `trust` command lists it and appends its sha256 (child exit %lu)", code);
    CHECK(!file_exists(mk), "trust S1: untrusted plugin NOT loaded: its DllMain never ran (no marker file)");
    read_text(path, log, sizeof log);
    CHECK(strstr(log, "plugin v6_plain.dll NOT loaded: not trusted") != NULL && !strstr(log, "WARNING: trust = dev"),
          "trust S1: refusal logged with its sha256, no dev-trust warning");
    /* S2: the same file is in the allowlist now */
    CHECK(run_child(dir, "idle", NULL, la1, &code) && code == 0 && file_exists(mk), "trust S2: after `trust` the same plugin loads (marker written by its DllMain)");
    read_text(path, log, sizeof log);
    CHECK(strstr(log, "trusted (allowlist)") != NULL && strstr(log, "loaded plugin 'v6_plain'") != NULL, "trust S2: logged as trusted (allowlist)");
    /* S3: env V3X_TRUST=dev, empty list */
    DeleteFileA(mk);
    CHECK(run_child(dir, "idle", "dev", la2, &code) && code == 0 && file_exists(mk), "trust S3: V3X_TRUST=dev loads it without any list");
    read_text(path, log, sizeof log);
    CHECK(strstr(log, "WARNING: trust = dev") != NULL && strstr(log, "dev (unchecked)") != NULL, "trust S3: the dev-trust WARNING is logged and the plugin is shown as dev (unchecked)");
    /* S4: the v3x.ini key `trust = dev` has no effect any more */
    DeleteFileA(mk);
    {
        FILE *f = fopen(ini, "wb");
        if (f) { fputs("[plugins]\ntrust = dev\n", f); fclose(f); }
    }
    CHECK(run_child(dir, "idle", NULL, la3, &code) && code == 0 && !file_exists(mk), "trust S4: v3x.ini [plugins] trust = dev does NOT make it load");
    read_text(path, log, sizeof log);
    CHECK(strstr(log, "NOT loaded: not trusted") != NULL && !strstr(log, "WARNING: trust = dev"), "trust S4: still refused, no dev-trust warning");
    CHECK(run_child(dir, "idle", "nodev", la3, &code) && code == 0 && !file_exists(mk), "trust S4b: V3X_TRUST=<anything but dev> does not trust either");
    DeleteFileA(ini);
    /* S5: a list holding another hash only */
    {
        char lp[MAX_PATH * 2];
        snprintf(lp, sizeof lp, "%s\\V3X", la4);
        CreateDirectoryA(lp, NULL);
        snprintf(lp, sizeof lp, "%s\\V3X\\trusted_plugins.txt", la4);
        FILE *f = fopen(lp, "wb");
        if (f) { fputs("# someone else's plugin\r\n0000000000000000000000000000000000000000000000000000000000000000 v6_plain.dll\r\n", f); fclose(f); }
    }
    CHECK(run_child(dir, "idle", NULL, la4, &code) && code == 0 && !file_exists(mk), "trust S5: an allowlist with a different hash does not trust it");
    /* S6: the file changes after it was trusted (S1/S2 hash): not loaded */
    {
        char dllp[MAX_PATH * 2];
        snprintf(dllp, sizeof dllp, "%s\\v3x\\plugins\\v6_plain.dll", dir);
        FILE *f = fopen(dllp, "ab");
        if (f) { fputc(0, f); fclose(f); }
    }
    CHECK(run_child(dir, "idle", NULL, la1, &code) && code == 0 && !file_exists(mk), "trust S6: a plugin changed after it was trusted (one byte appended) is refused again");
}

/* ---- sections of the main run */
static void test_hooks(const V3X_API *api)
{
    int rc = call_p("v6_main.dll", "v6_t_hook_safe", (void *)host_target);
    CHECK(rc == 0 && p_t1(3) == 6 && val("v6_main.dll", "v6_pre_count") == 1, "hook_safe by v6_main on host_target works (rc %d, pre_count %ld)", rc,
          val("v6_main.dll", "v6_pre_count"));
    rc = call_p("v6_other.dll", "v6_t_hook_raw", (void *)host_target2);
    CHECK(rc == 0 && p_t2(1) == 1005, "raw hook by v6_other on host_target2 works (rc %d, %lld)", rc, p_t2(1));

    /* unhook(NULL): used to disable EVERY hook in the process */
    long fa0 = val("v6_main.dll", "v6_frames_a");
    CHECK(call_p("v6_main.dll", "v6_t_unhook", NULL) == V3X_EARG && api->unhook(NULL) == V3X_EARG, "unhook(NULL) returns V3X_EARG (from a plugin and from the host)");
    long pre0 = val("v6_main.dll", "v6_pre_count");
    CHECK(p_t1(4) == 8 && val("v6_main.dll", "v6_pre_count") == pre0 + 1, "after unhook(NULL) the hook_safe hook still fires");
    CHECK(p_t2(1) == 1005, "after unhook(NULL) the raw hook of another plugin is still active");
    pump(3);
    CHECK(val("v6_main.dll", "v6_frames_a") >= fa0 + 3, "after unhook(NULL) on_frame still runs (%ld -> %ld)", fa0, val("v6_main.dll", "v6_frames_a"));
    CHECK(val("v6_main.dll", "v6_load_unhook_null") == V3X_EARG, "unhook(NULL) from v3x_plugin_load: V3X_EARG");

    /* unknown address */
    CHECK(call_p("v6_main.dll", "v6_t_unhook", (void *)host_target3) == V3X_EMH_NOT_CREATED, "unhook of a function nobody hooked = MH_ERROR_NOT_CREATED (4)");
    CHECK(call_p("v6_main.dll", "v6_t_unhook", (void *)host_bytes) == V3X_EMH_NOT_CREATED && call_p("v6_main.dll", "v6_t_unhook", (void *)(uintptr_t)8) == V3X_EMH_NOT_CREATED &&
              api->unhook((void *)host_code_fn) == V3X_EMH_NOT_CREATED,
          "unhook of a data address, of 0x8 and of an unhooked function: not created, nothing touched");
    CHECK(p_t1(5) == 10 && p_t2(1) == 1005, "the real hooks survived the bogus unhooks");

    /* another plugin's hooks */
    CHECK(call_p("v6_other.dll", "v6_t_unhook", (void *)host_target) == V3X_EHOOKBUSY, "v6_other unhook of v6_main's hook_safe target: V3X_EHOOKBUSY");
    CHECK(call_p("v6_main.dll", "v6_t_unhook", (void *)host_target2) == V3X_EHOOKBUSY && api->unhook((void *)host_target2) == V3X_EHOOKBUSY,
          "v6_main (and the host) unhook of v6_other's raw hook: V3X_EHOOKBUSY");
    pre0 = val("v6_main.dll", "v6_pre_count");
    CHECK(p_t1(1) == 2 && val("v6_main.dll", "v6_pre_count") == pre0 + 1 && p_t2(1) == 1005, "both hooks still work after the refused unhooks");
    CHECK(call_p("v6_other.dll", "v6_t_hook_raw", (void *)host_target) == V3X_EHOOKBUSY && call_p("v6_other.dll", "v6_t_hook_safe", (void *)host_target2) == V3X_EHOOKBUSY,
          "a raw hook on a hook_safe target and hook_safe on a raw-hooked target: V3X_EHOOKBUSY");

    /* the loader's own hooks: entry (live while plugins load), PeekMessageW (frame tick), RtlExitUserProcess */
    CHECK(val("v6_main.dll", "v6_load_unhook_entry") == V3X_EHOOKBUSY && val("v6_main.dll", "v6_load_unhook_peek") == V3X_EHOOKBUSY &&
              val("v6_main.dll", "v6_load_unhook_exit") == V3X_EHOOKBUSY,
          "unhook of the core's entry / PeekMessageW / RtlExitUserProcess hooks from v3x_plugin_load: V3X_EHOOKBUSY (%ld %ld %ld)",
          val("v6_main.dll", "v6_load_unhook_entry"), val("v6_main.dll", "v6_load_unhook_peek"), val("v6_main.dll", "v6_load_unhook_exit"));
    void *(*core_target)(int) = (void *(*)(int))sym("v6_main.dll", "v6_t_core_target");
    void *peek = core_target(1), *exitf = core_target(2);
    long fb0 = val("v6_main.dll", "v6_frames_b");
    CHECK(call_i("v6_main.dll", "v6_t_unhook_core", 1) == V3X_EHOOKBUSY && call_i("v6_other.dll", "v6_t_unhook_core", 1) == V3X_EHOOKBUSY && api->unhook(peek) == V3X_EHOOKBUSY,
          "unhook(PeekMessageW) from two plugins and the host: V3X_EHOOKBUSY");
    CHECK(call_i("v6_main.dll", "v6_t_unhook_core", 2) == V3X_EHOOKBUSY && api->unhook(exitf) == V3X_EHOOKBUSY, "unhook(RtlExitUserProcess): V3X_EHOOKBUSY");
    CHECK(call_i("v6_main.dll", "v6_t_hook_core", 1) == V3X_EHOOKBUSY && call_i("v6_main.dll", "v6_t_hook_core", 2) == V3X_EHOOKBUSY,
          "hook_safe on the core's PeekMessageW / RtlExitUserProcess targets: V3X_EHOOKBUSY (a plugin cannot share or replace them)");
    pump(3);
    CHECK(val("v6_main.dll", "v6_frames_b") >= fb0 + 3, "the PeekMessageW frame hook still drives on_frame (%ld -> %ld)", fb0, val("v6_main.dll", "v6_frames_b"));
    CHECK(call_i("v6_main.dll", "v6_t_unhook_core", 0) != 0, "unhook(entry point) after load does not succeed either");
    CHECK(call_p("v6_main.dll", "v6_t_unhook", api->unhook) == V3X_EMH_NOT_CREATED, "unhook(the loader's own unhook function): not created");
}

static void test_owner(const V3X_API *api)
{
    int (*t_hook)(void *, void *, void **) = (int (*)(void *, void *, void **))make_thunk((void *)api->hook);
    int (*t_hook_safe)(void *, v3x_hook_fn, v3x_hook_fn, void *) = (int (*)(void *, v3x_hook_fn, v3x_hook_fn, void *))make_thunk((void *)api->hook_safe);
    int (*t_unhook)(void *) = (int (*)(void *))make_thunk((void *)api->unhook);
    int (*t_patch)(void *, const void *, size_t) = (int (*)(void *, const void *, size_t))make_thunk((void *)api->patch);
    int (*t_off_frame)(v3x_frame_fn, void *) = (int (*)(v3x_frame_fn, void *))make_thunk((void *)api->off_frame);
    int (*t_unreg)(const char *) = (int (*)(const char *))make_thunk((void *)api->unregister_command);
    CHECK(t_hook && t_hook_safe && t_unhook && t_patch && t_off_frame && t_unreg, "thunks in VirtualAlloc'd memory created");
    void *orig = NULL;
    CHECK(t_hook((void *)host_target3, (void *)host_target4, &orig) == V3X_EPERM, "hook from code outside any module: V3X_EPERM");
    CHECK(t_hook_safe((void *)host_target3, (v3x_hook_fn)host_target4, NULL, NULL) == V3X_EPERM, "hook_safe from code outside any module: V3X_EPERM");
    CHECK(p_t3(2) == 6 && api->unhook((void *)host_target3) == V3X_EMH_NOT_CREATED, "host_target3 was not hooked by the refused calls");
    long pre0 = val("v6_main.dll", "v6_pre_count");
    CHECK(t_unhook((void *)host_target) == V3X_EPERM && t_unhook((void *)host_target2) == V3X_EPERM, "unhook from code outside any module: V3X_EPERM");
    CHECK(p_t1(1) == 2 && val("v6_main.dll", "v6_pre_count") == pre0 + 1 && p_t2(1) == 1005, "the hooks it tried to remove still work");
    unsigned char before = host_bytes[0];
    static const unsigned char ee = 0xEE;
    int prc = t_patch((void *)host_bytes, &ee, 1);
    CHECK(prc == ERROR_ACCESS_DENIED && host_bytes[0] == before, "patch from code outside any module: ERROR_ACCESS_DENIED, nothing written (rc %d)", prc);
    void *fa = sym("v6_main.dll", "v6_frame_a");
    CHECK(t_off_frame((v3x_frame_fn)fa, (void *)0xA) == V3X_EPERM && api->off_frame((v3x_frame_fn)fa, (void *)0xA) == V3X_EPERM,
          "off_frame of another module's callback (from nowhere and from the host exe): V3X_EPERM");
    CHECK(t_unreg("v6cmd_b") == V3X_EPERM && api->unregister_command("v6cmd_b") == V3X_EPERM && api->unregister_command("help") == V3X_EPERM,
          "unregister_command of a plugin's / a built-in command from nowhere or the host: V3X_EPERM");
    long fa0 = val("v6_main.dll", "v6_frames_a");
    pump(2);
    CHECK(val("v6_main.dll", "v6_frames_a") >= fa0 + 2 && api->exec_command("v6cmd_b") == 0 && api->exec_command("help") == 0, "all of them are still registered and run");
    CHECK(strstr(read_log(), "called from code outside any module") != NULL && strstr(read_log(), "refused (V3X_EPERM, unknown owner)") != NULL,
          "the refusal is logged (\"called from code outside any module\")");
}

static void test_patch(const V3X_API *api)
{
    static const unsigned char ee[4] = {0xEE, 0xEE, 0xEE, 0xEE};
    unsigned char save[4];
    memcpy(save, (const void *)host_bytes, 4);
    CHECK(call_patch("v6_main.dll", (void *)host_bytes, ee, 4) == 0 && host_bytes[0] == 0xEE && host_bytes[3] == 0xEE, "patch of the exe's data by a plugin works (positive control)");
    CHECK(call_patch("v6_main.dll", (void *)host_bytes, save, 4) == 0, "patch back");

    /* the loader's own memory */
    HMODULE vd = GetModuleHandleA("version.dll");
    uint8_t *code = (uint8_t *)(void *)GetProcAddress(vd, "v3x_api");
    uint8_t c0 = code[0], h0 = ((uint8_t *)vd)[0x40];
    void *tbl = (void *)api;
    uintptr_t log0 = (uintptr_t)api->log;
    CHECK(call_patch("v6_main.dll", code, ee, 1) == ERROR_ACCESS_DENIED && code[0] == c0, "patch of version.dll code (v3x_api): ERROR_ACCESS_DENIED, unchanged");
    CHECK(call_patch("v6_main.dll", (uint8_t *)vd + 0x40, ee, 1) == ERROR_ACCESS_DENIED && ((uint8_t *)vd)[0x40] == h0, "patch of the version.dll PE header: ERROR_ACCESS_DENIED");
    uintptr_t null_fn = 0;
    CHECK(call_patch("v6_main.dll", (uint8_t *)tbl + offsetof(V3X_API, log), &null_fn, sizeof null_fn) == ERROR_ACCESS_DENIED && (uintptr_t)api->log == log0,
          "patch of the loader's API table (api->log := NULL): ERROR_ACCESS_DENIED, the table is intact");
    CHECK(call_patch("v6_main.dll", (void *)api->patch, ee, 1) == ERROR_ACCESS_DENIED, "patch of the loader's patch function itself: ERROR_ACCESS_DENIED");

    /* the hook stub pool */
    void *stub = stub_of((void *)host_target);
    CHECK(stub != NULL && (uintptr_t)stub != (uintptr_t)host_target, "decoded the hook_safe stub of host_target (%p)", stub);
    if (stub) {
        uint8_t s0 = *(uint8_t *)stub;
        CHECK(call_patch("v6_main.dll", stub, ee, 1) == ERROR_ACCESS_DENIED && *(uint8_t *)stub == s0, "patch of a hook stub (stub pool, code page): ERROR_ACCESS_DENIED");
        CHECK(call_patch("v6_other.dll", (uint8_t *)stub + 4, ee, 4) == ERROR_ACCESS_DENIED, "patch of the middle of a hook stub by another plugin: ERROR_ACCESS_DENIED");
    }
    /* another plugin's module */
    unsigned char *od = (unsigned char *)sym("v6_other.dll", "v6_rw_data");
    unsigned char od0 = od ? od[0] : 0;
    CHECK(od && call_patch("v6_main.dll", od, ee, 1) == ERROR_ACCESS_DENIED && od[0] == od0, "patch of another plugin's module (its .data): ERROR_ACCESS_DENIED");
    CHECK(call_patch("v6_main.dll", sym("v6_other.dll", "v6_t_patch"), ee, 1) == ERROR_ACCESS_DENIED, "patch of another plugin's code: ERROR_ACCESS_DENIED");
    unsigned char *own = (unsigned char *)sym("v6_main.dll", "v6_rw_data");
    CHECK(own && call_patch("v6_main.dll", own, ee, 1) == 0 && own[0] == 0xEE, "patch of the plugin's own .data works");
    /* another owner's hook bytes (first 16 bytes of the hooked function) */
    uint8_t t1b[16];
    memcpy(t1b, (void *)host_target, 16);
    CHECK(call_patch("v6_other.dll", (void *)host_target, ee, 4) == ERROR_ACCESS_DENIED && call_patch("v6_other.dll", (uint8_t *)host_target + 8, ee, 4) == ERROR_ACCESS_DENIED &&
              call_patch("v6_main.dll", (void *)host_target2, ee, 4) == ERROR_ACCESS_DENIED && !memcmp(t1b, (void *)host_target, 16),
          "patch of the first 16 bytes of a function hooked by ANOTHER plugin: ERROR_ACCESS_DENIED, hook bytes unchanged");
    void *peek = ((void *(*)(int))sym("v6_main.dll", "v6_t_core_target"))(1);
    CHECK(call_patch("v6_main.dll", peek, ee, 1) == ERROR_ACCESS_DENIED, "patch of the core's own PeekMessageW hook bytes: ERROR_ACCESS_DENIED");
    uint8_t pk0 = *(uint8_t *)peek;
    CHECK(api->patch(peek, ee, 1) == ERROR_ACCESS_DENIED && *(uint8_t *)peek == pk0, "the same from the host exe (not the owner of that hook): ERROR_ACCESS_DENIED");
    CHECK(p_t1(2) == 4 && p_t2(1) == 1005, "the hooks work after all the refused patches");
    pump(1);

    /* original page protection is restored */
    DWORD p_ro = prot_of(sym("v6_main.dll", "v6_ro_data")), p_exe_ro = prot_of(host_ro), p_code = prot_of((void *)host_code_fn), p_rw = prot_of((void *)host_bytes);
    unsigned char *ro = (unsigned char *)sym("v6_main.dll", "v6_ro_data");
    CHECK(p_ro == PAGE_READONLY && p_exe_ro == PAGE_READONLY && p_code == PAGE_EXECUTE_READ, "test pages: plugin .rdata %lx, exe .rdata %lx (want %x), exe .text %lx (want %x)", p_ro,
          p_exe_ro, PAGE_READONLY, p_code, PAGE_EXECUTE_READ);
    CHECK(call_patch("v6_main.dll", ro, ee, 4) == 0 && ro[0] == 0xEE && ro[3] == 0xEE && prot_of(ro) == p_ro, "patch of a PAGE_READONLY page works, protection restored (%lx)", prot_of(ro));
    volatile unsigned char *hro = (volatile unsigned char *)(void *)host_ro;   /* volatile: the array is const, the compiler must read it */
    CHECK(call_patch("v6_main.dll", (void *)(uintptr_t)hro, ee, 2) == 0 && hro[1] == 0xEE && prot_of((const void *)hro) == p_exe_ro, "patch of the exe's .rdata works, protection restored (%lx)", prot_of((const void *)hro));
    uint8_t cb[16];
    memcpy(cb, (void *)host_code_fn, 16);
    CHECK(call_patch("v6_main.dll", (void *)host_code_fn, cb, 16) == 0 && prot_of((void *)host_code_fn) == p_code, "patch of code (same bytes): works, still PAGE_EXECUTE_READ (%lx)",
          prot_of((void *)host_code_fn));
    CHECK(prot_of((void *)host_bytes) == p_rw, "an already writable page keeps its protection");

    /* size limit */
    for (size_t i = 0; i < sizeof host_src; i++) host_src[i] = (unsigned char)(i * 7 + 1);
    CHECK(call_patch("v6_main.dll", host_big + 100, host_src, V3X_PATCH_MAX) == 0 && !memcmp(host_big + 100, host_src, V3X_PATCH_MAX),
          "patch of exactly V3X_PATCH_MAX (65536) bytes across 17 pages works");
    memset(host_big, 0, sizeof host_big);
    CHECK(call_patch("v6_main.dll", host_big, host_src, V3X_PATCH_MAX + 1) == ERROR_INVALID_PARAMETER && host_big[0] == 0 && host_big[V3X_PATCH_MAX] == 0,
          "patch of V3X_PATCH_MAX + 1 bytes: ERROR_INVALID_PARAMETER, nothing written");
    CHECK(call_patch("v6_main.dll", host_big, host_src, 0) == ERROR_INVALID_PARAMETER && call_patch("v6_main.dll", NULL, ee, 1) == ERROR_INVALID_PARAMETER &&
              call_patch("v6_main.dll", host_big, NULL, 4) == ERROR_INVALID_PARAMETER && call_patch("v6_main.dll", (void *)(uintptr_t)-8, ee, 16) == ERROR_INVALID_PARAMETER,
          "patch with n = 0, NULL addr / bytes, or a wrapping range: ERROR_INVALID_PARAMETER");

    /* memory that is not committed */
    uint8_t *r = (uint8_t *)VirtualAlloc(NULL, 3 * 4096, MEM_RESERVE, PAGE_NOACCESS);
    uint8_t *c = r ? (uint8_t *)VirtualAlloc(r, 4096, MEM_COMMIT, PAGE_READWRITE) : NULL;
    uint8_t *na = r ? (uint8_t *)VirtualAlloc(r + 2 * 4096, 4096, MEM_COMMIT, PAGE_NOACCESS) : NULL;
    if (c) memset(c, 0x5A, 4096);
    static const unsigned char big16[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    CHECK(c && na && call_patch("v6_main.dll", c + 4096 - 8, big16, 16) == ERROR_INVALID_ADDRESS && c[4096 - 1] == 0x5A && c[4096 - 8] == 0x5A,
          "patch across the end of committed memory: ERROR_INVALID_ADDRESS, the committed part is NOT written either");
    CHECK(c && call_patch("v6_main.dll", r + 4096, big16, 4) == ERROR_INVALID_ADDRESS && call_patch("v6_main.dll", na, big16, 4) == ERROR_INVALID_ADDRESS,
          "patch of reserved memory and of a PAGE_NOACCESS page: ERROR_INVALID_ADDRESS");
    CHECK(c && call_patch("v6_main.dll", c, big16, 4) == 0 && c[3] == 4, "patch inside the committed page works");
    if (r) VirtualFree(r, 0, MEM_RELEASE);

    /* rollback of a failed v3x_plugin_load */
    CHECK(host_bytes_r[0] == 0x11 && host_bytes_r[1] == 0x22 && host_bytes_r[2] == 0x33 && val("v6_rollback.dll", "v6_load_patch_rc") == 0,
          "rollback: v6_rollback patched host_bytes_r (rc %ld) and returned 5; the bytes are restored (%02x %02x %02x)", val("v6_rollback.dll", "v6_load_patch_rc"), host_bytes_r[0],
          host_bytes_r[1], host_bytes_r[2]);
    const char *plugins = run(api, "plugins");
    CHECK(line_has(plugins, "v6_rollback", "FAILED: v3x_plugin_load returned 5, rolled back") && line_has(read_log(), "'v6_rollback' rolled back", "1 patch(es) restored"),
          "rollback shown by `plugins` and logged (1 patch restored)");
    long tf0 = val("v6_rollback.dll", "v6_frames_total");
    pump(2);
    CHECK(val("v6_rollback.dll", "v6_frames_total") == tf0, "the rolled-back plugin's on_frame callback never runs");
}

static void test_quota(const V3X_API *api)
{
    (void)api;
    const char *q = "v6_quota.dll";
    int *rc = (int *)sym(q, "v6_q_rc");
    volatile long *calls = (volatile long *)sym(q, "v6_q_calls");
    CHECK(rc && calls, "v6_quota loaded");
    if (!rc || !calls) return;
    /* frames */
    int ok = call_ii(q, "v6_t_quota", 0, 17);
    CHECK(ok == V3X_QUOTA_FRAME && rc[V3X_QUOTA_FRAME - 1] == 0 && rc[V3X_QUOTA_FRAME] == -1, "on_frame: the 16th is accepted, the 17th is refused (%d accepted, rc[16] = %d)", ok, rc[V3X_QUOTA_FRAME]);
    CHECK(call_i("v6_main.dll", "v6_t_on_frame", 2) == 0, "another plugin can still register on_frame while v6_quota is at its quota");
    CHECK(call_i("v6_quota.dll", "v6_t_quota_thunk", 0) == -1,
          "on_frame quota counts the CALLER: a callback that lives outside the plugin module (executable heap thunk) is refused too");
    pump(2);
    CHECK(calls[0] >= 2 && calls[15] >= 2, "all 16 registered frame callbacks run");
    long c0 = calls[0], c1 = calls[1];
    CHECK(call_ii(q, "v6_t_quota_off", 0, 0) == 0, "off_frame removes one of them");
    pump(3);
    CHECK(calls[0] == c0 && calls[1] >= c1 + 3, "the removed callback is no longer called, the others are");
    CHECK(call_ii(q, "v6_t_quota_reg", 0, 40) == 0 && call_ii(q, "v6_t_quota_reg", 0, 41) == -1, "the quota is on LIVE registrations: one slot freed = exactly one new registration");
    CHECK(call_i("v6_main.dll", "v6_t_off_frame", 2) == 0, "(cleanup v6_main extra frame cb)");
    for (int i = 1; i < 16; i++) call_ii(q, "v6_t_quota_off", 0, i);
    call_ii(q, "v6_t_quota_off", 0, 40);
    /* events */
    ok = call_ii(q, "v6_t_quota", 1, 25);
    CHECK(ok == V3X_QUOTA_EVENTS && rc[V3X_QUOTA_EVENTS - 1] == 0 && rc[V3X_QUOTA_EVENTS] == -1, "on_event: the 24th is accepted, the 25th is refused (%d accepted, rc[24] = %d)", ok, rc[V3X_QUOTA_EVENTS]);
    CHECK(call_i("v6_quota.dll", "v6_t_quota_thunk", 1) == -1, "on_event quota: a subscriber outside the plugin module is refused too");
    CHECK(call_i("v6_main.dll", "v6_t_on_event", 2) == 0, "another plugin can still subscribe");
    c0 = calls[3];
    CHECK(api->emit_event("v6q_ev3", NULL) == 1 && calls[3] == c0 + 1, "an accepted subscription gets its event");
    CHECK(call_ii(q, "v6_t_quota_off", 1, 3) == 0 && api->emit_event("v6q_ev3", NULL) == 0 && calls[3] == c0 + 1, "off_event: the removed subscriber is not called any more");
    CHECK(call_ii(q, "v6_t_quota_off", 1, 3) == V3X_ENOTFOUND, "off_event of a removed subscription: V3X_ENOTFOUND");
    CHECK(call_ii(q, "v6_t_quota_reg", 1, 3) == 0 && call_ii(q, "v6_t_quota_reg", 1, 50) == -1, "one freed slot = one new subscription");
    for (int i = 0; i < 25; i++) call_ii(q, "v6_t_quota_off", 1, i);
    call_i("v6_main.dll", "v6_t_off_event", 2);
    /* commands */
    ok = call_ii(q, "v6_t_quota", 2, 25);
    CHECK(ok == V3X_QUOTA_COMMANDS && rc[V3X_QUOTA_COMMANDS - 1] == 0 && rc[V3X_QUOTA_COMMANDS] == -1, "register_command: the 24th is accepted, the 25th is refused (%d accepted, rc[24] = %d)", ok,
          rc[V3X_QUOTA_COMMANDS]);
    CHECK(call_i("v6_quota.dll", "v6_t_quota_thunk", 2) == -1, "register_command quota: a handler outside the plugin module is refused too");
    CHECK(call_i("v6_main.dll", "v6_t_reg_cmd", 2) == 0 && call_s("v6_main.dll", "v6_t_unreg", "v6cmd_x") == 0, "another plugin can still register and remove its own command");
    c0 = calls[5];
    CHECK(api->exec_command("v6qc5") == 0 && calls[5] == c0 + 1, "an accepted command runs");
    CHECK(call_ii(q, "v6_t_quota_off", 2, 5) == 0 && api->exec_command("v6qc5") == -1 && calls[5] == c0 + 1, "unregister_command: the command is gone and its handler is not called");
    CHECK(call_ii(q, "v6_t_quota_reg", 2, 5) == 0 && api->exec_command("v6qc5") == 0 && calls[5] == c0 + 2, "the name is free again after unregister_command");
    for (int i = 0; i < 25; i++) call_ii(q, "v6_t_quota_off", 2, i);
    char *log = read_log();
    CHECK(strstr(log, "plugin v6_quota: on_frame quota reached (16 per plugin)") && strstr(log, "plugin v6_quota: on_event quota reached (24 per plugin)") &&
              strstr(log, "plugin v6_quota: register_command quota reached (24 per plugin)"),
          "the three quota refusals are logged with the plugin name");
}

static void test_off(const V3X_API *api)
{
    const char *m = "v6_main.dll", *o = "v6_other.dll";
    pump(2);
    long a0 = val(m, "v6_frames_a"), b0 = val(m, "v6_frames_b");
    CHECK(call_i(m, "v6_t_off_frame", 0) == 0, "off_frame of the plugin's own callback: 0");
    pump(3);
    CHECK(val(m, "v6_frames_a") == a0 && val(m, "v6_frames_b") >= b0 + 3, "the removed frame callback is not called again; the other one is (%ld -> %ld, b %ld -> %ld)", a0, val(m, "v6_frames_a"), b0, val(m, "v6_frames_b"));
    CHECK(call_i(m, "v6_t_off_frame", 0) == V3X_ENOTFOUND && call_i(m, "v6_t_off_frame", 3) == V3X_EARG, "off_frame twice = V3X_ENOTFOUND; off_frame(NULL) = V3X_EARG");
    CHECK(call_i(m, "v6_t_on_frame", 0) == 0, "register it again");
    pump(2);
    CHECK(val(m, "v6_frames_a") >= a0 + 2, "and it runs again");
    /* a second plugin cannot remove it */
    void *fb = sym(m, "v6_frame_b");
    long b1 = val(m, "v6_frames_b");
    CHECK(((fn_ps_t)sym(o, "v6_t_off_frame_any"))((void *)fb, (void *)0xB) == V3X_EPERM, "off_frame of another plugin's callback: V3X_EPERM");
    pump(2);
    CHECK(val(m, "v6_frames_b") >= b1 + 2, "it keeps running");
    CHECK(((fn_ps_t)sym(o, "v6_t_off_frame_any"))((void *)sym(o, "v6_t_patch"), NULL) == V3X_ENOTFOUND, "off_frame of something never registered: V3X_ENOTFOUND");
    /* removal from inside the callback */
    CHECK(call_i(m, "v6_t_on_frame", 3) == 0, "register a callback that removes itself");
    pump(4);
    CHECK(val(m, "v6_self_calls") == 1, "off_frame from inside the callback: it ran exactly once (%ld)", val(m, "v6_self_calls"));

    /* events */
    long ea = val(m, "v6_ev_a"), eb = val(m, "v6_ev_b");
    CHECK(api->emit_event("v6_ev", NULL) == 2 && val(m, "v6_ev_a") == ea + 1 && val(m, "v6_ev_b") == eb + 1, "emit_event reaches both subscribers");
    CHECK(call_i(m, "v6_t_off_event", 0) == 0 && api->emit_event("v6_ev", NULL) == 1 && val(m, "v6_ev_a") == ea + 1 && val(m, "v6_ev_b") == eb + 2,
          "off_event removes only that subscription; the removed one is not called");
    CHECK(call_i(m, "v6_t_off_event", 0) == V3X_ENOTFOUND && call_i(m, "v6_t_off_event", 3) == V3X_EARG, "off_event twice = V3X_ENOTFOUND; off_event(NULL) = V3X_EARG");
    CHECK(((fn_ps_t)sym(o, "v6_t_off_frame_any")) != NULL && ((int (*)(const char *, void *, void *))sym(o, "v6_t_off_event_any"))("v6_ev", sym(m, "v6_ev_cb_b"), (void *)0xB) == V3X_EPERM &&
              api->off_event("v6_ev", (void (*)(const char *, void *, void *))sym(m, "v6_ev_cb_b"), (void *)0xB) == V3X_EPERM,
          "off_event of another module's subscription (from a plugin and from the host): V3X_EPERM");
    CHECK(api->emit_event("v6_ev", NULL) == 1, "it is still subscribed");
    CHECK(call_i(m, "v6_t_on_event", 0) == 0 && call_i(m, "v6_t_on_event", 0) == V3X_EEXIST, "subscribe again (0); the same (name, fn, user) twice = V3X_EEXIST");
    CHECK(call_i(m, "v6_t_on_event", 2) == 0 && api->emit_event("v6_ev_self", NULL) == 1 && api->emit_event("v6_ev_self", NULL) == 0 && val(m, "v6_ev_self") == 1,
          "off_event from inside the handler: called once");

    /* commands */
    long ca = val(m, "v6_cmd_a");
    CHECK(api->exec_command("v6cmd_a") == 0 && val(m, "v6_cmd_a") == ca + 1, "command runs");
    CHECK(call_s(m, "v6_t_unreg", "V6CMD_A") == 0, "unregister_command is case-insensitive: 0");
    CHECK(api->exec_command("v6cmd_a") == -1 && val(m, "v6_cmd_a") == ca + 1, "the unregistered command is gone and not called");
    CHECK(call_s(m, "v6_t_unreg", "v6cmd_a") == V3X_ENOTFOUND && call_s(m, "v6_t_unreg", NULL) == V3X_EARG, "unregister of an unknown name: V3X_ENOTFOUND; NULL: V3X_EARG");
    CHECK(call_i(m, "v6_t_reg_cmd", 0) == 0 && api->exec_command("v6cmd_a") == 0 && val(m, "v6_cmd_a") == ca + 2, "the name is free again; the new registration works");
    CHECK(call_s(m, "v6_t_unreg", "help") == V3X_EPERM && call_s(m, "v6_t_unreg", "plugins") == V3X_EPERM && call_s(m, "v6_t_unreg", "trust") == V3X_EPERM,
          "a plugin cannot unregister built-in commands: V3X_EPERM");
    CHECK(call_s(o, "v6_t_unreg", "v6cmd_b") == V3X_EPERM && api->exec_command("v6cmd_b") == 0, "a plugin cannot unregister another plugin's command: V3X_EPERM");
    CHECK(api->exec_command("help") == 0 && api->exec_command("trust") == 0, "built-in commands still run");
    /* a command registered by one plugin cannot be taken over by another with the same name */
    CHECK(call_i(o, "v6_t_reg_cmd", 0) == V3X_EEXIST, "register_command of a taken name: V3X_EEXIST");
}

static void test_events(const V3X_API *api)
{
    const char *m = "v6_main.dll";
    CHECK(api->emit_event("process_exit", NULL) == -1 && api->emit_event("plugin_disabled", NULL) == -1 && api->emit_event("plugins_loaded", NULL) == -1,
          "emit_event of the reserved core events process_exit / plugin_disabled / plugins_loaded returns -1 (host)");
    int (*emit)(const char *, void *) = (int (*)(const char *, void *))sym("v6_other.dll", "v6_t_emit");
    long bad_pd = 0;
    CHECK(emit && emit("process_exit", NULL) == -1 && emit("plugin_disabled", &bad_pd) == -1 && emit("plugins_loaded", NULL) == -1, "... and from a plugin");
    CHECK(api->emit_event("v6_ev", NULL) >= 1, "(positive control: an ordinary event is delivered)");
    CHECK(val(m, "v6_exit_seen") == 0, "no subscriber saw a forged process_exit");
    CHECK(val(m, "v6_pl_count") == 1 && *(void *volatile *)sym(m, "v6_pl_data") == NULL, "plugins_loaded was received exactly once after the load (data NULL), a forged emit did not add one (%ld)", val(m, "v6_pl_count"));
    CHECK(val(m, "v6_pl_tid") == (long)GetCurrentThreadId() && val(m, "v6_pl_tid") == (long)api->main_thread_id(), "plugins_loaded arrived on the main thread");

    typedef struct PdRec { uint32_t struct_size, reserved; char name[48]; void *module; char reason[160]; unsigned long tid; } PdRec;
    PdRec *pd = (PdRec *)sym(m, "v6_pd");
    long n = val(m, "v6_pd_count");
    int rb = -1, ft = -1;
    for (long i = 0; pd && i < n && i < 8; i++) {
        if (!strcmp(pd[i].name, "v6_rollback")) rb = (int)i;
        if (!strcmp(pd[i].name, "v6_fault")) ft = (int)i;
    }
    CHECK(rb >= 0 && strstr(pd[rb].reason, "returned 5, rolled back") && pd[rb].module == (void *)GetModuleHandleA("v6_rollback.dll") && pd[rb].struct_size == 32 && sizeof(V3X_PluginDisabled) == 32,
          "plugin_disabled for the rolled-back plugin: name, module, reason, struct_size 32 (%s)", rb >= 0 ? pd[rb].reason : "missing");
    pump(4);
    n = val(m, "v6_pd_count");
    ft = -1;
    for (long i = 0; pd && i < n && i < 8; i++)
        if (!strcmp(pd[i].name, "v6_fault")) ft = (int)i;
    CHECK(ft >= 0 && pd[ft].struct_size == 32 && pd[ft].reserved == 0 && pd[ft].module == (void *)GetModuleHandleA("v6_fault.dll") && strstr(pd[ft].reason, "FAULTED in on_frame callback"),
          "plugin_disabled for the plugin that faulted in on_frame: struct_size %u, module ok, reason \"%s\"", ft >= 0 ? pd[ft].struct_size : 0, ft >= 0 ? pd[ft].reason : "missing");
    CHECK(ft >= 0 && pd[ft].tid == GetCurrentThreadId(), "plugin_disabled was emitted on the thread that disabled it (the main thread here)");
    long c = val("v6_fault.dll", "v6_fault_calls");
    pump(3);
    CHECK(c == 2 && val("v6_fault.dll", "v6_fault_calls") == 2, "the faulted callback ran twice and never again (%ld)", val("v6_fault.dll", "v6_fault_calls"));
    long fa = val(m, "v6_frames_a");
    pump(2);
    CHECK(val(m, "v6_frames_a") >= fa + 2, "other plugins keep running after the fault");
    long before = val(m, "v6_pd_count");
    pump(3);
    CHECK(val(m, "v6_pd_count") == before, "plugin_disabled is emitted once per plugin");
    CHECK(line_has(run(api, "plugins"), "v6_fault", "FAULTED in on_frame"), "`plugins` shows the fault");
}

static void test_refusal(const V3X_API *api)
{
    /* every plugin that is mapped wrote its marker in DllMain (positive control) */
    static const char *const mapped[] = {"v6_main", "v6_other", "v6_caps_hook", "v6_caps_patch", "v6_fault", "v6_quota", "v6_rollback", "v6_plain"};
    int all = 1;
    char p[64];
    for (size_t i = 0; i < sizeof mapped / sizeof mapped[0]; i++) {
        snprintf(p, sizeof p, "v6_marker_%s.txt", mapped[i]);
        if (!file_exists(p)) { all = 0; printf("  no marker for %s\n", mapped[i]); }
    }
    CHECK(all, "positive control: every plugin that was loaded wrote its DllMain marker (so a missing marker means DllMain never ran)");
    /* refused before LoadLibrary */
    static const char *const refused[] = {"v6_minapi", "v6_reqbuild", "v6_noload", "v6_corrupt", "v6_trunc"};
    static const char *const why[] = {"refused: needs loader API v99", "refused: game", "skipped (no v3x_plugin_load)", "FAILED:", "FAILED:"};
    const char *out = run(api, "plugins");
    printf("---- plugins\n%s----\n", out);
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        snprintf(p, sizeof p, "v6_marker_%s.txt", refused[i]);
        char dll[64];
        snprintf(dll, sizeof dll, "%s.dll", refused[i]);
        CHECK(!file_exists(p) && !GetModuleHandleA(dll) && line_has(out, dll, why[i]), "%s: not loaded, DllMain never ran (no marker, not mapped), `plugins` says \"%s\"", refused[i], why[i]);
    }
    CHECK(!line_has(out, "v6_minapi", "loaded at") && !line_has(out, "v6_noload", "loaded at"), "none of them is shown as loaded");
    CHECK(strstr(out, "trust: dev (unchecked)") != NULL, "`plugins` shows the trust level of a loaded plugin");
    CHECK(strstr(run(api, "trust"), "0 plugin(s) not loaded because they are not trusted") != NULL, "`trust` lists nothing refused under V3X_TRUST=dev");
}

static void test_caps(const V3X_API *api)
{
    const char *h = "v6_caps_hook.dll", *p = "v6_caps_patch.dll", *m = "v6_main.dll";
    static const unsigned char ee = 0xEE;
    unsigned char before = host_bytes[5];
    CHECK(call_patch(h, (void *)(host_bytes + 5), &ee, 1) == ERROR_ACCESS_DENIED && host_bytes[5] == before, "capabilities \"hook\": patch is refused (ERROR_ACCESS_DENIED), nothing written");
    CHECK(call_s(h, "v6_t_exec", "version") == -1, "capabilities \"hook\": exec_command is refused (-1)");
    CHECK(call_p(h, "v6_t_hook_safe", (void *)host_target4) == 0 && p_t4(1) == 7 && call_p(h, "v6_t_unhook", (void *)host_target4) == 0, "capabilities \"hook\": hook_safe and unhook work");
    CHECK(call_patch(p, (void *)(host_bytes + 5), &ee, 1) == 0 && host_bytes[5] == 0xEE, "capabilities \"patch\": patch works");
    CHECK(call_p(p, "v6_t_hook_safe", (void *)host_target4) == V3X_EPERM && call_p(p, "v6_t_hook_raw", (void *)host_target4) == V3X_EPERM && call_p(p, "v6_t_unhook", (void *)host_target4) == V3X_EPERM,
          "capabilities \"patch\": hook_safe / hook / unhook are refused (V3X_EPERM)");
    CHECK(p_t4(1) == 7 && api->unhook((void *)host_target4) == V3X_EMH_NOT_CREATED, "the refused hook was not installed");
    CHECK(call_s(p, "v6_t_exec", "version") == -1, "capabilities \"patch\": exec_command is refused");
    host_bytes[5] = before;
    unsigned char b2 = host_bytes[6];
    CHECK(call_patch(m, (void *)(host_bytes + 6), &ee, 1) == 0 && call_s(m, "v6_t_exec", "version") == 0, "undeclared capabilities = everything allowed (patch, exec_command)");
    host_bytes[6] = b2;
    const char *out = run(api, "plugins");
    CHECK(strstr(out, "capabilities: hook bogus") != NULL && strstr(out, "capabilities: patch\n") != NULL && strstr(out, "capabilities: (undeclared: all)") != NULL,
          "`plugins` shows declared and undeclared capabilities");
    char *log = read_log();
    CHECK(strstr(log, "plugin v6_caps_hook called patch but its manifest does not declare the capability \"patch\"") != NULL &&
              strstr(log, "plugin v6_caps_patch called hook_safe but its manifest does not declare the capability \"hook\"") != NULL &&
              strstr(log, "plugin 'v6_caps_hook': 1 unknown capability word(s) in its manifest (first: \"bogus\")") != NULL,
          "refusals and the unknown capability word are logged");
}

static void test_scan(const V3X_API *api)
{
    CHECK(api->pattern_scan("48 ZZ") == NULL && api->pattern_scan("48 8B 0x05") == NULL && api->pattern_scan("4 8") == NULL && api->pattern_scan("???") == NULL,
          "pattern_scan with a non-hex token returns NULL");
    char big[3 * 300 + 1];
    for (int i = 0; i < 257; i++) memcpy(big + 3 * i, "90 ", 3);
    big[3 * 257] = 0;
    CHECK(api->pattern_scan(big) == NULL, "pattern_scan of 257 bytes returns NULL");
    big[3 * 256] = 0;
    (void)api->pattern_scan(big);   /* 256 bytes: allowed (no crash) */
    CHECK(api->pattern_scan("") == NULL && api->pattern_scan(NULL) == NULL, "pattern_scan of \"\" / NULL returns NULL");
    char pat[64];
    const uint8_t *f = (const uint8_t *)(void *)host_code_fn;
    snprintf(pat, sizeof pat, "%02x %02X ?? %02x ? %02x %02x %02x", f[0], f[1], f[3], f[5], f[6], f[7]);
    void *hit = api->pattern_scan(pat);
    CHECK(hit != NULL && (uintptr_t)hit >= api->image_base() && (uintptr_t)hit < api->image_base() + api->image_size(), "pattern_scan: a valid pattern (lower/upper hex, ?? and ?) is found (%s)", pat);
    const char *s = (const char *)api->find_string("v3x_v6_host_marker_string");
    void *x0 = s ? api->find_xref(s, 0) : NULL;
    CHECK(s != NULL && x0 != NULL, "find_string + find_xref(index 0) work (positive control)");
    CHECK(api->find_xref(s, -1) == NULL && api->find_xref(s, -1000000) == NULL && api->find_xref(s, 1000000) == NULL && api->find_xref(NULL, 0) == NULL,
          "find_xref with a negative index (or a huge one) returns NULL");
}

static void test_exit_child(void)
{
    char dir[MAX_PATH];
    GetCurrentDirectoryA(sizeof dir, dir);
    DeleteFileA("v6_exit_marker.txt");
    DWORD code = 0;
    int ran = run_child(dir, "exitchild", "dev", getenv("LOCALAPPDATA"), &code);
    char t[32] = "";
    read_text("v6_exit_marker.txt", t, sizeof t);
    CHECK(ran && code == 7, "exit child: ExitProcess(7) after a refused unhook of RtlExitUserProcess (exit code %lu)", code);
    CHECK(!strcmp(t, "7"), "the core's exit hook still works: process_exit reached the plugin with code 7 (marker \"%s\")", t);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("marker: %s\n", host_marker);   /* found by find_string / find_xref below */
    const V3X_API *api = v3x_api();
    if (!api) { printf("FAIL: no v3x_api\nFAILED\n"); return 1; }
    if (argc > 1 && !strcmp(argv[1], "idle")) return 0;
    if (argc > 1 && !strcmp(argv[1], "trustcmd")) return trustcmd_child(api);
    if (argc > 1 && !strcmp(argv[1], "exitchild")) {
        int rc = call_i("v6_main.dll", "v6_t_unhook_core", 2);
        printf("exitchild: unhook(RtlExitUserProcess) = %d\n", rc);
        ExitProcess(rc == V3X_EHOOKBUSY ? 7 : 8);
    }
    const char *trustdir = argc > 1 ? argv[1] : NULL;

    CHECK(api->api_version == 6 && V3X_HAS(api, off_frame) && V3X_HAS(api, unregister_command) && V3X_HAS(api, v3x_dir_utf8), "api v%u with all v6 fields", api->api_version);
    CHECK(api->game_dir_w()[0] && api->v3x_dir_w()[0] && api->game_dir_utf8()[0] && api->v3x_dir_utf8()[0] && !strcmp(api->v3x_dir_utf8(), api->v3x_dir()) &&
              api->v3x_dir_w()[wcslen(api->v3x_dir_w()) - 1] == L'\\' && api->game_dir_utf8()[strlen(api->game_dir_utf8()) - 1] == '\\',
          "game_dir_w / v3x_dir_w / *_utf8: same folders as the ANSI getters, trailing backslash");
    pump(4);
    test_hooks(api);
    test_owner(api);
    test_patch(api);
    test_off(api);
    test_quota(api);
    test_events(api);
    test_refusal(api);
    test_caps(api);
    test_scan(api);
    test_exit_child();
    if (trustdir) test_trust(trustdir);
    else CHECK(0, "no trust directory given (argv[1])");
    printf("%d checks, %d failed\n%s\n", checks, fails, fails ? "FAILED" : "ALL OK");
    return fails ? 1 : 0;
}
