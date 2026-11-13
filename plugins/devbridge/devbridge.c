/* devbridge: runs existing V3X console commands for a local tool (the MCP server mcp/v3x_mcp.py)
 * over a named pipe, and returns their text output. It adds no commands and no hooks.
 *
 * Pipe: \\.\pipe\v3x_devbridge (name overridable: pipe=<name> in v3x/devbridge.ini, or env
 *   V3X_DEVBRIDGE_PIPE, which wins; tests use it so they never touch a running game's pipe).
 *   Default security (the creating user), PIPE_REJECT_REMOTE_CLIENTS, one instance = one client at
 *   a time (others get ERROR_PIPE_BUSY). A client may send any number of requests per connection.
 * Request:  one line "<id> <command line>\n" (id: 1..32 chars without spaces, max 4 KB per line;
 *   a longer line gets "0 END -2" and the connection is closed).
 * Response: the captured output (api->print / api->log made on the bridge thread while the command
 *   runs, one or more lines), then "<id> END <rc>\n" (rc as from exec_command: 0 ran, -1 unknown).
 *   Output over 256 KB is cut, with a "[devbridge: output truncated ...]" line before END.
 *   With a loader older than API v3 the command still runs (exec_command) and the reply is
 *   "[devbridge: output not captured (loader API vN < 3)]".
 * Commands run synchronously on the bridge thread (not the main thread), exactly as if typed in
 * the console; each command decides itself whether to use run_on_main.
 * Cost: one thread blocked in ConnectNamedPipe/ReadFile while idle, fixed 4 KB + 256 KB buffers.
 * Config: v3x/devbridge.ini   enabled=1 (0 = no pipe, no thread)   pipe=v3x_devbridge */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "v3x.h"

#define TAG "devbridge"
#define REQ_MAX 4096
#define RESP_MAX (256 * 1024)
#define TRUNC_NOTE "[devbridge: output truncated at 256 KB]\n"

V3X_EXPORT const char *v3x_plugin_name = "devbridge";

static const V3X_API *api;
static int g_has_capture;
static char g_pipe[MAX_PATH] = "\\\\.\\pipe\\v3x_devbridge";

static char g_req[REQ_MAX + 1];
static char g_resp[RESP_MAX + 64];   /* + room for the END line */
static size_t g_resp_n;
static int g_trunc;

/* ------------------------------------------------------------------ config */

static int load_config(void)
{
    int enabled = 1;
    char path[MAX_PATH], name[200] = "";
    snprintf(path, sizeof path, "%sdevbridge.ini", api->v3x_dir());
    FILE *f = fopen(path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            if (*s == ';' || *s == '#' || *s == '[' || !*s) continue;
            char *eq = strchr(s, '=');
            if (!eq) continue;
            *eq = 0;
            char *k = s, *v = eq + 1;
            for (char *e = eq - 1; e >= k && (*e == ' ' || *e == '\t'); e--) *e = 0;
            while (*v == ' ' || *v == '\t') v++;
            for (char *e = v + strlen(v) - 1; e >= v && strchr(" \t\r\n", *e); e--) *e = 0;
            if (!_stricmp(k, "enabled")) enabled = atoi(v) != 0;
            else if (!_stricmp(k, "pipe") && *v) snprintf(name, sizeof name, "%s", v);
        }
        fclose(f);
    }
    char env[200];
    DWORD n = GetEnvironmentVariableA("V3X_DEVBRIDGE_PIPE", env, sizeof env);
    if (n && n < sizeof env) snprintf(name, sizeof name, "%s", env);
    if (*name) {
        for (char *c = name; *c; c++)
            if (*c == '\\' || *c == '/') *c = '_';
        snprintf(g_pipe, sizeof g_pipe, "\\\\.\\pipe\\%s", name);
    }
    return enabled;
}

/* ------------------------------------------------------------------ response buffer */

static void resp_add(const char *s, size_t k)
{
    if (g_trunc) return;
    size_t room = RESP_MAX - sizeof TRUNC_NOTE - g_resp_n;
    if (k > room) {
        memcpy(g_resp + g_resp_n, s, room);
        g_resp_n += room;
        if (g_resp_n && g_resp[g_resp_n - 1] != '\n') g_resp[g_resp_n++] = '\n';
        memcpy(g_resp + g_resp_n, TRUNC_NOTE, sizeof TRUNC_NOTE - 1);
        g_resp_n += sizeof TRUNC_NOTE - 1;
        g_trunc = 1;
        return;
    }
    memcpy(g_resp + g_resp_n, s, k);
    g_resp_n += k;
}

static void sink(const char *text, void *user)
{
    (void)user;
    size_t k = strlen(text);
    resp_add(text, k);
    if (k && text[k - 1] != '\n') resp_add("\n", 1);
}

static int write_all(HANDLE p, const char *b, size_t n)
{
    while (n) {
        DWORD w = 0, chunk = n > 65536 ? 65536 : (DWORD)n;
        if (!WriteFile(p, b, chunk, &w, NULL) || !w) return -1;
        b += w; n -= w;
    }
    return 0;
}

/* Runs one request line (no '\n'); returns 0, or -1 if the client is gone. */
static int handle_line(HANDLE p, char *line)
{
    size_t L = strlen(line);
    while (L && (line[L - 1] == '\r' || line[L - 1] == ' ' || line[L - 1] == '\t')) line[--L] = 0;
    char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return 0;                              /* blank line: ignored */
    char id[33]; size_t i = 0;
    while (s[i] && s[i] != ' ' && s[i] != '\t' && i < sizeof id - 1) { id[i] = s[i]; i++; }
    id[i] = 0;
    const char *cmd = s + i;
    if (*cmd && *cmd != ' ' && *cmd != '\t') { snprintf(id, sizeof id, "0"); cmd = ""; }   /* id too long */
    while (*cmd == ' ' || *cmd == '\t') cmd++;

    g_resp_n = 0; g_trunc = 0;
    int rc;
    if (!*cmd) {
        resp_add("[devbridge: empty command]\n", 27);
        rc = -1;
    } else if (g_has_capture) {
        rc = api->exec_capture(cmd, sink, NULL);
    } else {
        rc = api->exec_command(cmd);
        char note[96];
        int k = snprintf(note, sizeof note, "[devbridge: output not captured (loader API v%u < 3)]\n", api->api_version);
        resp_add(note, (size_t)k);
    }
    int k = snprintf(g_resp + g_resp_n, sizeof g_resp - g_resp_n, "%s END %d\n", id, rc);
    g_resp_n += (size_t)k;
    return write_all(p, g_resp, g_resp_n);
}

/* Serves one connected client until it disconnects or misbehaves. */
static void serve_client(HANDLE p)
{
    size_t have = 0;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(p, g_req + have, (DWORD)(REQ_MAX - have), &got, NULL) && GetLastError() != ERROR_MORE_DATA) return;
        if (!got) continue;
        have += got;
        size_t start = 0;
        for (size_t i = 0; i < have; i++) {
            if (g_req[i] != '\n') continue;
            g_req[i] = 0;
            if (handle_line(p, g_req + start) < 0) return;
            start = i + 1;
        }
        if (start) { memmove(g_req, g_req + start, have - start); have -= start; }
        if (have >= REQ_MAX) {
            static const char msg[] = "[devbridge: request line too long (max 4096 bytes), closing]\n0 END -2\n";
            write_all(p, msg, sizeof msg - 1);
            FlushFileBuffers(p);
            return;
        }
    }
}

static DWORD WINAPI bridge_thread(void *arg)
{
    HANDLE p = (HANDLE)arg;
    for (;;) {
        if (ConnectNamedPipe(p, NULL) || GetLastError() == ERROR_PIPE_CONNECTED) serve_client(p);
        DisconnectNamedPipe(p);
    }
    return 0;
}

/* ------------------------------------------------------------------ load */

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    if (a->api_version < 2 || a->struct_size < offsetof(V3X_API, exec_command) + sizeof(void *)) {
        a->log(TAG, "loader API v%u is too old (need v2 console commands), not loading", a->api_version);
        return 1;
    }
    g_has_capture = a->api_version >= 3 && a->struct_size >= offsetof(V3X_API, exec_capture) + sizeof(void *) && a->exec_capture;
    if (!load_config()) { a->log(TAG, "disabled (enabled=0 in v3x/devbridge.ini)"); return 0; }

    HANDLE p = CreateNamedPipeA(g_pipe, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                1, 64 * 1024, REQ_MAX, 0, NULL);
    if (p == INVALID_HANDLE_VALUE) {
        a->log(TAG, "CreateNamedPipe %s failed (error %lu; another game instance?), bridge off", g_pipe, GetLastError());
        return 0;
    }
    HANDLE t = CreateThread(NULL, 1024 * 1024, bridge_thread, p, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t) {
        a->log(TAG, "CreateThread failed (error %lu), bridge off", GetLastError());
        CloseHandle(p);
        return 0;
    }
    CloseHandle(t);
    a->log(TAG, "listening on %s (output capture %s)", g_pipe, g_has_capture ? "on" : "OFF: loader API < 3");
    return 0;
}
