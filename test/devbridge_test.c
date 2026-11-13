/* End-to-end test for plugins/devbridge without the game.
 * Links build/version.dll like victoria3.exe, with devconsole + devbridge loaded and no console
 * window, then acts as a pipe client of the bridge: requests, pipelining, unknown command, output
 * truncation, one-client-at-a-time, overlong request, reconnect.
 * The pipe must be private (never the game's \\.\pipe\v3x_devbridge): the exe reads the name the
 * plugin opened from v3x.log and refuses to run against the default one.
 * Setup: <dir>/devbridge_test.exe, <dir>/version.dll, <dir>/v3x/plugins/{devconsole,devbridge}.dll
 * Run:   V3X_NO_CONSOLE=1 V3X_DEVBRIDGE_PIPE=v3x_devbridge_test <dir>/devbridge_test.exe   exit 0 = ALL OK
 *        ... <dir>/devbridge_test.exe --serve N   no checks: print "READY <pipe>", serve N seconds
 *                                                 (used by test/test_mcp.py, which sets the env) */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/v3x.h"

static int fails;
static void check(int ok, const char *what) { printf("%-64s %s\n", what, ok ? "OK" : "FAIL"); fails += !ok; }

static const V3X_API *api;
static char g_pipe[MAX_PATH];

/* prints 400 x 1000 chars = ~400 KB, more than the 256 KB response limit */
static void cmd_tst_big(const char *args, void *user)
{
    (void)args; (void)user;
    static char line[1001];
    memset(line, 'x', 1000); line[1000] = 0;
    for (int i = 0; i < 400; i++) api->print("%03d %s", i, line);
}

static HANDLE open_pipe(DWORD *err)
{
    for (int i = 0; i < 100; i++) {
        HANDLE h = CreateFileA(g_pipe, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) return h;
        *err = GetLastError();
        if (*err != ERROR_FILE_NOT_FOUND) return INVALID_HANDLE_VALUE;
        Sleep(50);   /* pipe instance between DisconnectNamedPipe and ConnectNamedPipe */
    }
    return INVALID_HANDLE_VALUE;
}

static int send_str(HANDLE h, const char *s)
{
    DWORD w; return WriteFile(h, s, (DWORD)strlen(s), &w, NULL) && w == strlen(s);
}

/* Reads until a line "<id> END <rc>"; returns the output before it (static), rc in *rc. */
static char g_buf[1 << 20];
static size_t g_have;   /* bytes in g_buf (may hold the start of the next response) */
static const char *recv_resp(HANDLE h, const char *id, int *rc, size_t *outlen)
{
    static char out[1 << 20];
    char pat[64]; snprintf(pat, sizeof pat, "%s END ", id);
    size_t pl = strlen(pat);
    *rc = -999;
    for (;;) {
        for (size_t i = 0; i + pl <= g_have; i++) {
            if ((i == 0 || g_buf[i - 1] == '\n') && !memcmp(g_buf + i, pat, pl)) {
                char *nl = memchr(g_buf + i, '\n', g_have - i);
                if (!nl) break;
                *rc = atoi(g_buf + i + pl);
                memcpy(out, g_buf, i); out[i] = 0;
                if (outlen) *outlen = i;
                size_t used = (size_t)(nl + 1 - g_buf);
                memmove(g_buf, g_buf + used, g_have - used); g_have -= used;
                return out;
            }
        }
        DWORD got = 0;
        if (g_have >= sizeof g_buf - 1 || !ReadFile(h, g_buf + g_have, (DWORD)(sizeof g_buf - 1 - g_have), &got, NULL) || !got) {
            out[0] = 0; if (outlen) *outlen = 0; return out;
        }
        g_have += got;
    }
}

static int count_lines(const char *s) { int n = 0; for (; *s; s++) n += *s == '\n'; return n; }

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    DWORD vh, vsz = GetFileVersionInfoSizeA("C:/Windows/System32/kernel32.dll", &vh);   /* imports version.dll */
    v3x_api_fn get = (v3x_api_fn)(void *)GetProcAddress(GetModuleHandleA("version.dll"), "v3x_api");
    api = get ? get() : NULL;
    if (!api || !vsz) { printf("no v3x_api export\nFAILED\n"); return 1; }

    /* the pipe the plugin really opened, from its log line */
    char log[MAX_PATH]; snprintf(log, sizeof log, "%sv3x.log", api->v3x_dir());
    FILE *f = fopen(log, "rb"); static char lg[1 << 16]; size_t ln = f ? fread(lg, 1, sizeof lg - 1, f) : 0; lg[ln] = 0;
    if (f) fclose(f);
    const char *lp = strstr(lg, "[devbridge] listening on ");
    if (lp) { lp += 25; size_t k = strcspn(lp, " \r\n"); snprintf(g_pipe, sizeof g_pipe, "%.*s", (int)k, lp); }
    if (!lp || !_stricmp(g_pipe, "\\\\.\\pipe\\v3x_devbridge")) {
        printf("devbridge not listening on a private pipe (pipe='%s'): set V3X_DEVBRIDGE_PIPE=<name> and V3X_NO_CONSOLE=1\nFAILED\n", g_pipe);
        return 1;
    }

    if (argc >= 3 && !strcmp(argv[1], "--serve")) {
        int secs = atoi(argv[2]);
        printf("READY %s\n", g_pipe);
        for (DWORD t0 = GetTickCount(); GetTickCount() - t0 < (DWORD)secs * 1000;) {
            MSG msg; PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE);   /* frame tick for run_on_main */
            Sleep(10);
        }
        return 0;
    }

    check(strstr(lg, "loaded plugin 'devbridge'") != NULL, "devbridge plugin loaded");
    check(strstr(lg, "output capture on") != NULL, "listening on a private pipe, capture on");
    api->register_command("tst_big", "test: ~400 KB output", cmd_tst_big, NULL);

    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, sizeof exe);
    const char *bs = strrchr(exe, '\\'); bs = bs ? bs + 1 : exe;
    char at1000[MAX_PATH]; snprintf(at1000, sizeof at1000, "%s+0x1000", bs);

    DWORD err = 0; int rc; size_t olen; const char *o;
    HANDLE h = open_pipe(&err);
    check(h != INVALID_HANDLE_VALUE, "connect to pipe");
    if (h == INVALID_HANDLE_VALUE) { printf("error %lu\nFAILED\n", err); return 1; }

    send_str(h, "1 help\n");
    o = recv_resp(h, "1", &rc, NULL);
    check(rc == 0 && strstr(o, "  dis ") && strstr(o, "  xref ") && strstr(o, "  help"), "1 help -> commands listed, 1 END 0");

    send_str(h, "2 dis +1000 3\n");
    o = recv_resp(h, "2", &rc, NULL);
    check(rc == 0 && strstr(o, at1000) && count_lines(o) == 3, "2 dis +1000 3 -> 3 lines from exe+0x1000, 2 END 0");

    send_str(h, "3 nosuchcmd 1\n");
    o = recv_resp(h, "3", &rc, NULL);
    check(rc == -1 && strstr(o, "unknown command 'nosuchcmd'"), "3 nosuchcmd -> message, 3 END -1");

    send_str(h, "a5 version\r\nb6 mods devbridge.dll\n");   /* pipelined, CRLF tolerated */
    o = recv_resp(h, "a5", &rc, NULL);
    int rc2; const char *o2 = recv_resp(h, "b6", &rc2, NULL);
    check(rc == 0 && rc2 == 0 && strstr(o2, "devbridge.dll") && strstr(o2, "1 module(s)"), "two pipelined requests, both answered in order");

    send_str(h, "7 tst_big\n");
    o = recv_resp(h, "7", &rc, &olen);
    check(rc == 0 && olen <= 256 * 1024 && olen > 250 * 1024 && strstr(o, "[devbridge: output truncated at 256 KB]\n") &&
          !strstr(o, "399 xxx"), "7 tst_big (~400 KB) -> cut at 256 KB with note, END follows");

    send_str(h, "8\n");
    o = recv_resp(h, "8", &rc, NULL);
    check(rc == -1 && strstr(o, "empty command"), "id without command -> 8 END -1");

    DWORD e2 = 0;
    HANDLE h2 = CreateFileA(g_pipe, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    e2 = GetLastError();
    check(h2 == INVALID_HANDLE_VALUE && e2 == ERROR_PIPE_BUSY, "second client while one is connected -> ERROR_PIPE_BUSY");
    if (h2 != INVALID_HANDLE_VALUE) CloseHandle(h2);

    static char longreq[5001];
    memset(longreq, 'z', 5000);
    send_str(h, longreq);
    o = recv_resp(h, "0", &rc, NULL);
    check(rc == -2 && strstr(o, "request line too long"), "5000-byte line -> 0 END -2");
    DWORD got; char c;
    check(!ReadFile(h, &c, 1, &got, NULL), "... and the server closes the connection");
    CloseHandle(h); g_have = 0;

    h = open_pipe(&err);
    check(h != INVALID_HANDLE_VALUE, "reconnect after close");
    if (h != INVALID_HANDLE_VALUE) {
        send_str(h, "9 dis +1000 1\n");
        o = recv_resp(h, "9", &rc, NULL);
        check(rc == 0 && strstr(o, at1000) && count_lines(o) == 1, "9 dis +1000 1 on the new connection");
        CloseHandle(h);
    }

    MSG msg; PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE);
    printf("%s\n", fails ? "FAILED" : "ALL OK");
    return fails ? 1 : 0;
}
