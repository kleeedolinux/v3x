/* End-to-end test for plugins/devconsole without the game.
 * Links build/version.dll like victoria3.exe (so this exe is the "game" image that scan/xref/fn
 * search), runs with no console window (V3X_NO_CONSOLE=1), drives the commands through
 * api->exec_command and checks what they printed into v3x/v3x.log. Also covers: reads that stay
 * fault-proof while another thread flips a page NOACCESS, and API v3 exec_capture (sink gets the
 * calling thread's print/log output only; nesting; NULL sink).
 * Setup: <dir>/devconsole_test.exe, <dir>/version.dll, <dir>/v3x/plugins/devconsole.dll.
 * Run:   V3X_NO_CONSOLE=1 <dir>/devconsole_test.exe        exit 0 = ALL OK */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/v3x.h"

/* Known instruction bytes (in .rdata): sub rsp,28h; lea rcx,[rip+0]; call +0; xor eax,eax; add rsp,28h; ret */
static const unsigned char CODE[] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8D, 0x0D, 0x00, 0x00, 0x00, 0x00, 0xE8, 0x00, 0x00,
                                     0x00, 0x00, 0x31, 0xC0, 0x48, 0x83, 0xC4, 0x28, 0xC3};
__attribute__((aligned(16))) static const char ANCHOR[] = "devconsole_test xref anchor 42";
static const char *volatile g_sink;

__attribute__((noinline)) int dc_target(int x)
{
    g_sink = ANCHOR;                          /* rip-relative lea to ANCHOR */
    for (int i = 0; i < x; i++) x ^= i * 7;
    return x;
}

/* Three functions containing the same 8-byte nop with one differing byte: scan with a wildcard must
 * find exactly these 3 (the pattern text itself lives in .rdata, not .text). */
#define MARK(b) __asm__ volatile(".byte 0x0F,0x1F,0x84,0x00,0xD5,0xC0," #b ",0x77")
__attribute__((noinline)) void dc_mark1(void) { MARK(0x11); }
__attribute__((noinline)) void dc_mark2(void) { MARK(0x22); }
__attribute__((noinline)) void dc_mark3(void) { MARK(0x33); }

/* exec_capture sink */
typedef struct { char buf[1 << 16]; size_t n; int calls; } Cap;
static void cap_sink(const char *text, void *user)
{
    Cap *c = user;
    c->calls++;
    size_t k = strlen(text);
    if (c->n + k < sizeof c->buf) { memcpy(c->buf + c->n, text, k); c->n += k; c->buf[c->n] = 0; }
}

static int fails;
static void check(int ok, const char *what) { printf("%-64s %s\n", what, ok ? "OK" : "FAIL"); fails += !ok; }

static const V3X_API *api;
static char g_log[MAX_PATH];
static char g_exe[MAX_PATH];   /* exe basename, e.g. devconsole_test.exe */

static char *read_file(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    n = (long)fread(b, 1, n, f); b[n] = 0;
    fclose(f);
    if (len) *len = n;
    return b;
}

/* Runs one console line; returns (static) what it appended to v3x.log, rc in *rc. */
static char g_out[1 << 20];
static const char *run(const char *line, int *rc)
{
    long before = 0, after = 0;
    free(read_file(g_log, &before));
    int r = api->exec_command(line);
    if (rc) *rc = r;
    char *all = read_file(g_log, &after);
    snprintf(g_out, sizeof g_out, "%s", all && after > before ? all + before : "");
    free(all);
    return g_out;
}

static DWORD WINAPI other_thread_print(void *p) { (void)p; api->print("OTHER_THREAD_LINE"); return 0; }
static void cmd_tst_cross(const char *args, void *user)
{
    (void)args; (void)user;
    HANDLE t = CreateThread(NULL, 0, other_thread_print, NULL, 0, NULL);
    WaitForSingleObject(t, INFINITE); CloseHandle(t);
    api->print("SAME_THREAD_PRINT");
    api->log("tst", "SAME_THREAD_LOG");
}
static Cap g_inner;
static void cmd_tst_nest(const char *args, void *user)
{
    (void)args; (void)user;
    api->print("OUTER_BEFORE");
    api->exec_capture("tst_inner", cap_sink, &g_inner);
    api->print("OUTER_AFTER");
}
static void cmd_tst_inner(const char *args, void *user) { (void)args; (void)user; api->print("INNER_LINE"); }

/* events: each subscriber appends "<tag>:<name>:<data>;" to g_ev */
static char g_ev[1024];
static void ev_append(const char *tag, const char *name, void *data)
{
    size_t l = strlen(g_ev);
    snprintf(g_ev + l, sizeof g_ev - l, "%s:%s:%p;", tag, name, data);
}
static void ev_a(const char *name, void *data, void *user) { (void)user; ev_append("A", name, data); }
static void ev_b(const char *name, void *data, void *user) { ev_append((const char *)user, name, data); }
static void ev_late(const char *name, void *data, void *user) { (void)user; ev_append("LATE", name, data); }
static void ev_nester(const char *name, void *data, void *user)
{
    (void)data; (void)user;
    ev_append("N", name, NULL);
    api->on_event("ev_test", ev_late, NULL);   /* added during emit: not called by this emit */
    api->emit_event("ev_inner", NULL);         /* nested emit */
}
static double g_lt_seconds; static const char *g_lt_to;
static void ev_load(const char *name, void *data, void *user)
{
    (void)name; (void)user;
    const V3X_LoadTransition *t = data;
    if (t) { g_lt_seconds = t->seconds; g_lt_to = t->to; }
}

/* flips a page between READWRITE and NOACCESS while commands read it */
static volatile LONG g_flip_stop;
static DWORD WINAPI flipper(void *p)
{
    DWORD old;
    while (!g_flip_stop) {
        VirtualProtect(p, 0x1000, PAGE_NOACCESS, &old);
        VirtualProtect(p, 0x1000, PAGE_READWRITE, &old);
    }
    return 0;
}

static const char *where(const void *p)
{
    static char b[4][MAX_PATH]; static int k;
    char *o = b[k++ & 3];
    snprintf(o, MAX_PATH, "%s+0x%llx", g_exe, (unsigned long long)((uintptr_t)p - (uintptr_t)GetModuleHandleA(NULL)));
    return o;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    char cmd[512]; const char *o; int rc;

    DWORD h, sz = GetFileVersionInfoSizeA("C:/Windows/System32/kernel32.dll", &h);
    check(sz != 0, "version.dll forwarding");

    v3x_api_fn get = (v3x_api_fn)(void *)GetProcAddress(GetModuleHandleA("version.dll"), "v3x_api");
    api = get ? get() : NULL;
    check(api && api->api_version >= 2, "v3x_api export, api v2");
    if (!api) { printf("FAILED\n"); return 1; }

    GetModuleFileNameA(NULL, g_exe, sizeof g_exe);
    const char *bs = strrchr(g_exe, '\\');
    memmove(g_exe, bs ? bs + 1 : g_exe, strlen(bs ? bs + 1 : g_exe) + 1);
    snprintf(g_log, sizeof g_log, "%sv3x.log", api->v3x_dir());

    /* --- loading / registration (no console window) ------------------------------------------ */
    long n0; char *log0 = read_file(g_log, &n0);
    check(log0 && strstr(log0, "loaded plugin 'devconsole'"), "plugin loaded");
    check(log0 && strstr(log0, "devconsole] 6 commands registered (Zydis"), "6 commands registered without console");
    free(log0);
    static const char *names[] = {"dis", "fn", "xref", "scan", "mem", "mods"};
    int taken = 0;
    for (int i = 0; i < 6; i++) taken += api->register_command(names[i], "x", (v3x_command_fn)(void *)check, NULL) == -2;
    check(taken == 6, "all 6 names taken (re-register -> -2)");
    o = run("help", &rc);
    check(rc == 0 && strstr(o, "  dis ") && strstr(o, "  mods ") && strstr(o, "  xref "), "help lists devconsole commands");
    o = run("plugins", &rc);
    check(strstr(o, "devconsole") && strstr(o, "loaded at"), "plugins: devconsole loaded");

    /* --- dis ---------------------------------------------------------------------------------- */
    snprintf(cmd, sizeof cmd, "dis 0x%llx 6", (unsigned long long)(uintptr_t)CODE);
    o = run(cmd, &rc);
    check(rc == 0, "dis: exec_command returns 0");
    check(strstr(o, where(CODE)) && strstr(o, "48 83 EC 28") && strstr(o, "sub rsp, 0x28"), "dis: addr, bytes, sub rsp, 0x28");
    check(strstr(o, "lea rcx, [") && strstr(o, "; ") && strstr(o, where(CODE + 11)), "dis: lea rip-relative target resolved");
    check(strstr(o, "call 0x") && strstr(o, "xor eax, eax") && strstr(o, "add rsp, 0x28") && strstr(o, "ret"),
          "dis: call / xor / add / ret");
    snprintf(cmd, sizeof cmd, "dis +0x%llx 1", (unsigned long long)((uintptr_t)CODE - api->image_base()));
    o = run(cmd, NULL);
    check(strstr(o, "sub rsp, 0x28") && !strstr(o, "lea"), "dis +rva 1: one instruction");
    snprintf(cmd, sizeof cmd, "dis %s 1", where(CODE + 4));
    o = run(cmd, NULL);
    check(strstr(o, "lea rcx, [") != NULL, "dis module+off (as printed)");
    unsigned long long ca = (unsigned long long)(uintptr_t)CODE;
    snprintf(cmd, sizeof cmd, "dis %08llx`%08llx 1", ca >> 32, ca & 0xffffffffull);
    o = run(cmd, NULL);
    check(strstr(o, "sub rsp, 0x28") != NULL, "dis windbg-style hi`lo address");
    o = run("dis", NULL);
    check(strstr(o, "usage: dis") != NULL, "dis without args: usage");
    o = run("dis 0x10", &rc);
    check(rc == 0 && strstr(o, "<not readable>"), "dis unmapped address: error, no crash");
    snprintf(cmd, sizeof cmd, "dis 0x%llx 200", (unsigned long long)(uintptr_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateFileW"));
    o = run(cmd, NULL);
    check(strstr(o, "KERNEL32.DLL+0x") || strstr(o, "kernel32.dll+0x"), "dis 200 instrs of kernel32 code");

    /* --- fn ----------------------------------------------------------------------------------- */
    snprintf(cmd, sizeof cmd, "fn 0x%llx", (unsigned long long)((uintptr_t)dc_target + 4));
    o = run(cmd, NULL);
    snprintf(cmd, sizeof cmd, "function %s ", where((void *)dc_target));
    check(strstr(o, cmd) != NULL, "fn: start found from mid-function address");
    check(strstr(o, "end of function") && strstr(o, "ret"), "fn: disassembles to the end, ends with ret");
    check(strstr(o, "lea ") && strstr(o, where(ANCHOR)) && strstr(o, "\"devconsole_test xref anchor 42\""),
          "fn: lea annotated with target + string");
    o = run("fn 0x10", NULL);
    check(strstr(o, "no .pdata entry") != NULL, "fn on non-code: no .pdata entry");

    /* --- xref --------------------------------------------------------------------------------- */
    void *site = api->find_xref(ANCHOR, 0);
    check(site && (uintptr_t)site > (uintptr_t)dc_target && api->function_start(site) == (void *)dc_target,
          "(precondition) loader finds the lea in dc_target");
    int nx = 0;                               /* main() leas ANCHOR too: expect the loader's own count */
    while (api->find_xref(ANCHOR, nx)) nx++;
    char nxs[32]; snprintf(nxs, sizeof nxs, "\n%d xref(s)", nx);
    o = run("xref \"devconsole_test xref anchor 42\"", NULL);
    snprintf(cmd, sizeof cmd, "(fn %s)", where((void *)dc_target));
    check(strstr(o, "string \"devconsole_test xref anchor 42\" at") && strstr(o, where(ANCHOR)), "xref \"str\": string located");
    check(strstr(o, "[0] ") && strstr(o, where(site)) && strstr(o, cmd) && nx >= 2 && strstr(o, nxs), "xref \"str\": [0] in dc_target, all sites listed");
    snprintf(cmd, sizeof cmd, "xref %s", where(ANCHOR));
    o = run(cmd, NULL);
    check(strstr(o, nxs) != NULL, "xref <addr>: same count");
    o = run("xref \"devconsole_test xref anchor 42\" 1", NULL);
    check(strstr(o, "1 xref(s) (stopped at max)") != NULL, "xref max=1: stopped at max");
    o = run("xref \"no such string zz9\"", NULL);
    check(strstr(o, "not found") != NULL, "xref missing string: not found");
    o = run("xref", NULL);
    check(strstr(o, "usage: xref") != NULL, "xref without args: usage");

    /* --- scan --------------------------------------------------------------------------------- */
    if (site) {
        const unsigned char *s = site;
        /* exact 7 bytes: the rip-relative disp makes them unique to this site */
        snprintf(cmd, sizeof cmd, "scan %02X %02X %02X %02X %02X %02X %02X", s[0], s[1], s[2], s[3], s[4], s[5], s[6]);
        o = run(cmd, NULL);
        char fnw[MAX_PATH]; snprintf(fnw, sizeof fnw, "(fn %s)", where((void *)dc_target));
        check(strstr(o, "[0] ") && strstr(o, where(site)) && strstr(o, fnw) && strstr(o, "1 match(es) in .text\r\n"), "scan: exact lea bytes -> 1 match in dc_target");
    } else check(0, "scan (no site)");
    o = run("scan 0F 1F 84 00 D5 C0 ?? 77", NULL);
    {
        char a1[80], a2[80], a3[80];
        snprintf(a1, sizeof a1, "(fn %s)", where((void *)dc_mark1));
        snprintf(a2, sizeof a2, "(fn %s)", where((void *)dc_mark2));
        snprintf(a3, sizeof a3, "(fn %s)", where((void *)dc_mark3));
        check(strstr(o, "3 match(es) in .text\r\n") && strstr(o, a1) && strstr(o, a2) && strstr(o, a3) && strstr(o, "[2] ") && !strstr(o, "[3] "),
              "scan wildcard: all 3 matches listed, each with its fn");
    }
    o = run("scan -n 2 0F1F8400D5C0??77", NULL);
    check(strstr(o, "2 match(es) in .text (stopped at max)") && !strstr(o, "[2] "), "scan -n 2, compact hex: stopped at max");
    o = run("scan CC", NULL);
    check(strstr(o, "[19] ") && !strstr(o, "[20] ") && strstr(o, "20 match(es) in .text (stopped at max)"), "scan default max 20");
    o = run("scan ?? ??", NULL);
    check(strstr(o, "usage: scan") != NULL, "scan all-wildcard: usage");
    o = run("scan 4", NULL);
    check(strstr(o, "usage: scan") != NULL, "scan odd hex digit count: usage");
    o = run("scan 0F 0B 0F 0B 0F 0B 0F 0B 0F 0B 0F 0B CC CC", NULL);
    check(strstr(o, "0 match(es) in .text\r\n") != NULL, "scan: no match");
    o = run("scan zz 12", NULL);
    check(strstr(o, "usage: scan") != NULL, "scan bad pattern: usage");

    /* --- mem ---------------------------------------------------------------------------------- */
    snprintf(cmd, sizeof cmd, "mem 0x%llx 23", (unsigned long long)(uintptr_t)CODE);
    o = run(cmd, NULL);
    check(strstr(o, "48 83 EC 28 48 8D 0D 00 00 00 00 E8") && strstr(o, "C4 28 C3") && strstr(o, "|H..(H"), "mem: hex + ascii");
    o = run("mem 0x10", NULL);
    check(strstr(o, "not readable") != NULL, "mem unmapped: not readable");
    unsigned char *pg = VirtualAlloc(NULL, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD old; VirtualProtect(pg + 0x1000, 0x1000, PAGE_NOACCESS, &old);
    memcpy(pg + 0xFF8, "TAILDATA", 8);
    snprintf(cmd, sizeof cmd, "mem 0x%llx 32", (unsigned long long)(uintptr_t)(pg + 0xFF8));
    o = run(cmd, NULL);
    check(strstr(o, "|TAILDATA|") && strstr(o, "(only 8 of 32 bytes readable)"), "mem across NOACCESS page: clipped, no crash");
    snprintf(cmd, sizeof cmd, "dis 0x%llx 20", (unsigned long long)(uintptr_t)(pg + 0xFFC));
    o = run(cmd, NULL);
    check(strstr(o, "<not readable>") != NULL, "dis running into NOACCESS page stops cleanly");

    /* fault-proof reads: another thread flips the page NOACCESS/READWRITE during mem/dis/xref hint */
    unsigned char *fp = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    memcpy(fp, "FLIPPAGE string for hint", 25);
    HANDLE ft = CreateThread(NULL, 0, flipper, fp, 0, NULL);
    char c1[128], c2[128];
    snprintf(c1, sizeof c1, "mem 0x%llx 256", (unsigned long long)(uintptr_t)fp);
    snprintf(c2, sizeof c2, "dis 0x%llx 40", (unsigned long long)(uintptr_t)fp);
    int ran = 0;
    for (int i = 0; i < 400; i++) ran += api->exec_command(c1) == 0, ran += api->exec_command(c2) == 0;
    g_flip_stop = 1; WaitForSingleObject(ft, INFINITE); CloseHandle(ft);
    check(ran == 800, "800 mem/dis on a page flipping NOACCESS: no crash");

    /* --- API v3 exec_capture ------------------------------------------------------------------ */
    check(api->api_version >= 3 && api->struct_size >= offsetof(V3X_API, exec_capture) + sizeof(void *), "api v3, exec_capture present");
    static Cap cap;
    snprintf(cmd, sizeof cmd, "dis 0x%llx 6", (unsigned long long)(uintptr_t)CODE);
    rc = api->exec_capture(cmd, cap_sink, &cap);
    check(rc == 0 && strstr(cap.buf, "sub rsp, 0x28") && strstr(cap.buf, "xor eax, eax") && strstr(cap.buf, "ret\n") && cap.calls == 6,
          "exec_capture dis: 6 lines delivered to sink");
    o = run(cmd, NULL);
    check(strstr(o, "sub rsp, 0x28") != NULL, "exec_capture output also still goes to v3x.log path");
    memset(&cap, 0, sizeof cap);
    rc = api->exec_capture("nosuchcmd", cap_sink, &cap);
    check(rc == -1 && strstr(cap.buf, "unknown command 'nosuchcmd'"), "exec_capture unknown: -1, message captured");
    api->register_command("tst_cross", "test", cmd_tst_cross, NULL);
    api->register_command("tst_nest", "test", cmd_tst_nest, NULL);
    api->register_command("tst_inner", "test", cmd_tst_inner, NULL);
    memset(&cap, 0, sizeof cap);
    o = run("log precheck", NULL);
    long b0; free(read_file(g_log, &b0));
    rc = api->exec_capture("tst_cross", cap_sink, &cap);
    char *lg = read_file(g_log, NULL);
    check(rc == 0 && strstr(cap.buf, "SAME_THREAD_PRINT\n") && strstr(cap.buf, "[tst] SAME_THREAD_LOG\n"), "exec_capture: print + log on calling thread captured");
    check(!strstr(cap.buf, "OTHER_THREAD_LINE") && lg && strstr(lg + b0, "OTHER_THREAD_LINE"), "exec_capture: other thread's print not captured (still logged)");
    free(lg);
    memset(&cap, 0, sizeof cap); memset(&g_inner, 0, sizeof g_inner);
    rc = api->exec_capture("tst_nest", cap_sink, &cap);
    check(rc == 0 && strstr(cap.buf, "OUTER_BEFORE") && strstr(cap.buf, "OUTER_AFTER") && !strstr(cap.buf, "INNER_LINE") &&
          strstr(g_inner.buf, "INNER_LINE") && !strstr(g_inner.buf, "OUTER"), "nested exec_capture: inner sink, outer restored");
    int before = cap.calls;
    api->print("after capture");
    check(cap.calls == before, "after exec_capture returns, print is not captured");
    check(api->exec_capture(NULL, cap_sink, &cap) == -1, "exec_capture(NULL line) -> -1");
    memset(&cap, 0, sizeof cap);
    rc = api->exec_capture("tst_inner", NULL, NULL);
    check(rc == 0, "exec_capture with NULL sink behaves like exec_command");

    /* --- API v3 on_event / emit_event ---------------------------------------------------------- */
    check(api->struct_size >= offsetof(V3X_API, emit_event) + sizeof(void *), "api v3, on_event/emit_event present");
    int s1 = api->on_event("ev_test", ev_a, NULL);
    int s2 = api->on_event("ev_test", ev_b, (void *)"B");
    int s3 = api->on_event("ev_test", ev_a, NULL);
    int s4 = api->on_event("ev_test", ev_b, (void *)"B2");   /* same fn, other user: allowed */
    check(s1 == 0 && s2 == 0 && s3 == -2 && s4 == 0, "on_event: 0, 0, duplicate -2, same fn other user 0");
    check(api->on_event(NULL, ev_a, NULL) == -1 && api->on_event("", ev_a, NULL) == -1 && api->on_event("x", NULL, NULL) == -1 &&
          api->on_event("a_name_that_is_way_too_long_for_the_event_table_64_chars_or_more_x", ev_a, NULL) == -1,
          "on_event bad args: -1");
    g_ev[0] = 0;
    int nc = api->emit_event("ev_test", (void *)0x1234);
    check(nc == 3 && !strcmp(g_ev, "A:ev_test:0000000000001234;B:ev_test:0000000000001234;B2:ev_test:0000000000001234;"),
          "emit: 3 subscribers in order, data passed through");
    g_ev[0] = 0;
    check(api->emit_event("EV_TEST", NULL) == 0 && api->emit_event("nobody", NULL) == 0 && !g_ev[0], "emit: case-sensitive, no subscribers -> 0");
    check(api->emit_event(NULL, NULL) == -1, "emit_event(NULL) -> -1");
    api->on_event("ev_nest", ev_nester, NULL);
    api->on_event("ev_inner", ev_a, NULL);
    g_ev[0] = 0;
    nc = api->emit_event("ev_nest", NULL);
    check(nc == 1 && !strcmp(g_ev, "N:ev_nest:0000000000000000;A:ev_inner:0000000000000000;"), "emit from inside a subscriber (nested)");
    g_ev[0] = 0;
    nc = api->emit_event("ev_test", NULL);
    check(nc == 4 && strstr(g_ev, "LATE:ev_test"), "subscriber added during an emit runs from the next emit");
    api->on_event("load_transition", ev_load, NULL);
    V3X_LoadTransition lt = {"loading", "ingame", 12.5};
    nc = api->emit_event("load_transition", &lt);
    check(nc == 1 && g_lt_seconds == 12.5 && g_lt_to && !strcmp(g_lt_to, "ingame"), "load_transition with V3X_LoadTransition data");
    int added = 0, last = 0;
    for (int i = 0; i < 100 && (last = api->on_event("ev_fill", ev_b, (void *)(uintptr_t)(i + 1))) == 0; i++) added++;
    check(last == -1 && added == 64 - 7, "table fixed at 64 subscriptions (then -1)");

    /* --- mods --------------------------------------------------------------------------------- */
    o = run("mods", NULL);
    snprintf(cmd, sizeof cmd, "%s  (game)", g_exe);
    check(strstr(o, cmd) && strstr(o, "version.dll") && strstr(o, "devconsole.dll") && strstr(o, " module(s)"), "mods: game, version.dll, devconsole.dll");
    o = run("mods KERNEL32", NULL);
    check(strstr(o, "KERNEL32.DLL") && !strstr(o, "version.dll") && strstr(o, "1 module(s)"), "mods filter (case-insensitive)");

    o = run("nosuchcmd 1", &rc);
    check(rc == -1 && strstr(o, "unknown command"), "unknown command: -1");

    MSG msg; PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE);   /* frame tick still fine */
    printf("%s\n", fails ? "FAILED" : "ALL OK");
    return fails ? 1 : 0;
}
