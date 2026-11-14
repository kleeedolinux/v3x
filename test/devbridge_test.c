/* End-to-end test for plugins/devbridge without the game.
 * Links build/version.dll like victoria3.exe, with devconsole + devbridge loaded and no console
 * window, then acts as a pipe client of the bridge: session token (auth line, wrong / missing
 * token), owner-only DACL on the pipe and the token file, the default read-only allow list,
 * requests, pipelining, one-client-at-a-time, overlong request, reconnect. A child process (same
 * dir, its own pipe, devbridge.ini allow = *) checks unknown command and output truncation.
 * The pipe must be private (never the game's \\.\pipe\v3x_devbridge): the exe reads the name the
 * plugin opened from v3x.log and refuses to run against the default one.
 * Setup: <dir>/devbridge_test.exe, <dir>/version.dll, <dir>/v3x/plugins/{devconsole,devbridge}.dll
 * Run:   V3X_NO_CONSOLE=1 V3X_DEVBRIDGE_PIPE=v3x_devbridge_test <dir>/devbridge_test.exe   exit 0 = ALL OK
 *        ... <dir>/devbridge_test.exe --serve N   no checks: print "READY <pipe>", serve N seconds
 *                                                 (used by test/test_mcp.py, which sets the env)
 *        ... <dir>/devbridge_test.exe --all       (child, started by the test itself) */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/v3x.h"
#include "../plugins/devbridge/db_core.h"

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

static char g_token[DB_TOKEN_HEX + 1];
static int read_token(void)
{
    char p[MAX_PATH]; snprintf(p, sizeof p, "%sdevbridge.token", api->v3x_dir());
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    size_t n = fread(g_token, 1, DB_TOKEN_HEX, f); fclose(f);
    g_token[n] = 0;
    return n == DB_TOKEN_HEX;
}
/* connect + "auth <token>"; INVALID_HANDLE_VALUE on failure */
static HANDLE open_authed(DWORD *err)
{
    HANDLE h = open_pipe(err);
    if (h == INVALID_HANDLE_VALUE) return h;
    char line[128]; snprintf(line, sizeof line, "auth %s\n", g_token);
    int rc; send_str(h, line); recv_resp(h, "auth", &rc, NULL);
    if (rc != 0) { CloseHandle(h); g_have = 0; *err = (DWORD)-3; return INVALID_HANDLE_VALUE; }
    return h;
}
/* 1 if the object's DACL is protected and has exactly one ACE, an allow for the current user */
static int owner_only(HANDLE obj, const wchar_t *file)
{
    PSECURITY_DESCRIPTOR sd = NULL; PACL dacl = NULL;
    DWORD e = obj ? GetSecurityInfo(obj, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &dacl, NULL, &sd)
                  : GetNamedSecurityInfoW(file, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &dacl, NULL, &sd);
    if (e != ERROR_SUCCESS || !dacl) return 0;
    SECURITY_DESCRIPTOR_CONTROL ctl = 0; DWORD rev;
    GetSecurityDescriptorControl(sd, &ctl, &rev);
    HANDLE tok; union { TOKEN_USER tu; char raw[256]; } u; DWORD got;
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok); GetTokenInformation(tok, TokenUser, &u, sizeof u, &got); CloseHandle(tok);
    int ok = (ctl & SE_DACL_PROTECTED) && dacl->AceCount == 1;
    ACCESS_ALLOWED_ACE *ace = NULL;
    if (ok && GetAce(dacl, 0, (void **)&ace)) ok = ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && EqualSid((PSID)&ace->SidStart, u.tu.User.Sid);
    else ok = 0;
    LocalFree(sd);
    return ok;
}

/* child: allow = * on its own pipe -> unknown command and output truncation */
static int child_all(void)
{
    char lg_path[MAX_PATH];   /* a second instance logs to v3x.<pid>.log */
    snprintf(lg_path, sizeof lg_path, "%sv3x.%lu.log", api->v3x_dir(), GetCurrentProcessId());
    FILE *f = fopen(lg_path, "rb");
    if (!f) { snprintf(lg_path, sizeof lg_path, "%sv3x.log", api->v3x_dir()); f = fopen(lg_path, "rb"); } static char lg[1 << 16]; size_t ln = f ? fread(lg, 1, sizeof lg - 1, f) : 0; lg[ln] = 0;
    if (f) fclose(f);
    check(strstr(lg, "allow * (everything)") != NULL, "child: devbridge.ini allow = * -> everything allowed");
    api->register_command("tst_big", "test: ~400 KB output", cmd_tst_big, NULL);
    DWORD err = 0; int rc; size_t olen; const char *o;
    check(read_token(), "child: token file rewritten for this session");
    HANDLE h = open_authed(&err);
    check(h != INVALID_HANDLE_VALUE, "child: connect + auth");
    if (h == INVALID_HANDLE_VALUE) return 1;
    send_str(h, "3 nosuchcmd 1\n");
    o = recv_resp(h, "3", &rc, NULL);
    check(rc == -1 && strstr(o, "unknown command 'nosuchcmd'"), "child: 3 nosuchcmd -> message, 3 END -1");
    send_str(h, "7 tst_big\n");
    o = recv_resp(h, "7", &rc, &olen);
    check(rc == 0 && olen <= 256 * 1024 && olen > 250 * 1024 && strstr(o, "[devbridge: output truncated at 256 KB]\n") &&
          !strstr(o, "399 xxx"), "child: 7 tst_big (~400 KB) -> cut at 256 KB with note, END follows");
    CloseHandle(h);
    MSG msg; PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE);
    return fails ? 1 : 0;
}

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
    if (argc >= 2 && !strcmp(argv[1], "--all")) {          /* the log may hold the parent's lines too */
        char env[200]; DWORD en = GetEnvironmentVariableA("V3X_DEVBRIDGE_PIPE", env, sizeof env);
        if (en && en < sizeof env) snprintf(g_pipe, sizeof g_pipe, "\\\\.\\pipe\\%s", env);
        return child_all();
    }
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

    DWORD err = 0; int rc; const char *o;
    check(read_token() && strspn(g_token, "0123456789abcdef") == DB_TOKEN_HEX, "session token: 64 hex chars in v3x/devbridge.token");
    wchar_t tokw[MAX_PATH]; char tok8[MAX_PATH]; snprintf(tok8, sizeof tok8, "%sdevbridge.token", api->v3x_dir());
    MultiByteToWideChar(CP_UTF8, 0, tok8, -1, tokw, MAX_PATH);
    check(owner_only(NULL, tokw), "token file: protected DACL, one ACE = the current user");

    HANDLE h = open_pipe(&err);
    check(h != INVALID_HANDLE_VALUE, "connect to pipe");
    if (h == INVALID_HANDLE_VALUE) { printf("error %lu\nFAILED\n", err); return 1; }
    check(owner_only(h, NULL), "pipe: protected DACL, one ACE = the current user");
    send_str(h, "1 help\n");                              /* no auth line first */
    o = recv_resp(h, "auth", &rc, NULL);
    check(rc == -3 && strstr(o, "authentication failed"), "request without auth -> auth END -3");
    DWORD got0; char c0;
    check(!ReadFile(h, &c0, 1, &got0, NULL), "... and the connection is closed");
    CloseHandle(h); g_have = 0;
    h = open_pipe(&err);
    char bad[128]; snprintf(bad, sizeof bad, "auth %.63s%c\n", g_token, g_token[63] == '0' ? '1' : '0');
    send_str(h, bad);
    o = recv_resp(h, "auth", &rc, NULL);
    check(rc == -3, "wrong token (last hex digit changed) -> auth END -3");
    CloseHandle(h); g_have = 0;

    h = open_authed(&err);
    check(h != INVALID_HANDLE_VALUE, "connect + auth <token> -> auth END 0");
    if (h == INVALID_HANDLE_VALUE) { printf("error %lu\nFAILED\n", err); return 1; }

    send_str(h, "1 help\n");
    o = recv_resp(h, "1", &rc, NULL);
    check(rc == 0 && strstr(o, "  dis ") && strstr(o, "  xref ") && strstr(o, "  help"), "1 help -> commands listed, 1 END 0");

    send_str(h, "2 dis +1000 3\n");
    o = recv_resp(h, "2", &rc, NULL);
    check(rc == 0 && strstr(o, at1000) && count_lines(o) == 3, "2 dis +1000 3 -> 3 lines from exe+0x1000, 2 END 0");

    send_str(h, "3 nosuchcmd 1\n");
    o = recv_resp(h, "3", &rc, NULL);
    check(rc == -4 && strstr(o, "'nosuchcmd' is not in the allow list"), "default allow list: 3 nosuchcmd -> refused, 3 END -4");
    send_str(h, "3b reload devbridge\n");
    o = recv_resp(h, "3b", &rc, NULL);
    check(rc == -4, "default allow list: reload (changes state) -> refused");
    send_str(h, "3c HELP\n");
    o = recv_resp(h, "3c", &rc, NULL);
    check(rc == 0 && strstr(o, "  dis "), "allow list is case-insensitive (HELP)");

    send_str(h, "a5 version\r\nb6 mods devbridge.dll\n");   /* pipelined, CRLF tolerated */
    o = recv_resp(h, "a5", &rc, NULL);
    int rc2; const char *o2 = recv_resp(h, "b6", &rc2, NULL);
    check(rc == 0 && rc2 == 0 && strstr(o2, "devbridge.dll") && strstr(o2, "1 module(s)"), "two pipelined requests, both answered in order");

    send_str(h, "7 tst_big\n");
    o = recv_resp(h, "7", &rc, NULL);
    check(rc == -4, "7 tst_big (not in the default allow list) -> refused");

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

    h = open_authed(&err);
    check(h != INVALID_HANDLE_VALUE, "reconnect (+ auth) after close");
    if (h != INVALID_HANDLE_VALUE) {
        send_str(h, "9 dis +1000 1\n");
        o = recv_resp(h, "9", &rc, NULL);
        check(rc == 0 && strstr(o, at1000) && count_lines(o) == 1, "9 dis +1000 1 on the new connection");
        CloseHandle(h);
    }

    /* allow = * in a child process (own pipe, same dir) */
    {
        char ini[MAX_PATH]; snprintf(ini, sizeof ini, "%sdevbridge.ini", api->v3x_dir());
        FILE *fi = fopen(ini, "w"); if (fi) { fputs("; written by devbridge_test\nallow = *\n", fi); fclose(fi); }
        char env[200]; DWORD en = GetEnvironmentVariableA("V3X_DEVBRIDGE_PIPE", env, sizeof env);
        char child_pipe[240]; snprintf(child_pipe, sizeof child_pipe, "%.*s_all", (int)(en < sizeof env ? en : 0), env);
        SetEnvironmentVariableA("V3X_DEVBRIDGE_PIPE", child_pipe);
        char cl[MAX_PATH + 16]; snprintf(cl, sizeof cl, "\"%s\" --all", exe);
        STARTUPINFOA si = {sizeof si}; PROCESS_INFORMATION pi;
        DWORD code = 99;
        if (CreateProcessA(NULL, cl, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 60000); GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        }
        check(code == 0, "child with allow = *: unknown command + truncation checks passed");
        DeleteFileA(ini);
    }

    MSG msg; PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE);
    printf("%s\n", fails ? "FAILED" : "ALL OK");
    return fails ? 1 : 0;
}
