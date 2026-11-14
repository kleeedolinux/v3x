/* devbridge: runs existing V3X console commands for a local tool (the MCP server mcp/v3x_mcp.py)
 * over a named pipe, and returns their text output. It adds no commands and no hooks.
 *
 * Pipe: \\.\pipe\v3x_devbridge (name overridable: pipe=<name> in v3x/devbridge.ini, or env
 *   V3X_DEVBRIDGE_PIPE, which wins; tests use it so they never touch a running game's pipe).
 *   Security (docs/security-audit.md M5):
 *   - DACL: only the user running the game (protected, no inherited ACEs), FILE_FLAG_FIRST_PIPE_INSTANCE
 *     (fails if anyone else already created the name), PIPE_REJECT_REMOTE_CLIENTS, one instance =
 *     one client at a time (others get ERROR_PIPE_BUSY).
 *   - session token: 32 random bytes (BCryptGenRandom), hex, written at load to
 *     binaries/v3x/devbridge.token (created new with the same owner-only DACL). The first line of
 *     every connection must be "auth <token>"; reply "auth END 0", else
 *     "[devbridge: authentication failed]" + "auth END -3" and the connection is closed.
 *   - allow list: allow=<commands> in devbridge.ini (comma separated; leading words of the command
 *     line, "x$" = bare command only); default DB_ALLOW_DEFAULT in db_core.h (read-only inspection);
 *     allow = * runs everything. A refused line gets "<id> END -4".
 *   No token file / no random source / no DACL -> no pipe (fail closed).
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
 * Config: v3x/devbridge.ini   enabled=1 (0 = no pipe, no thread)   pipe=v3x_devbridge   allow=... */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "v3x.h"
#include "v3x_safe.h"
#include "db_core.h"

#define TAG "devbridge"
#define REQ_MAX DB_REQ_MAX
#define RESP_MAX (256 * 1024)
#define TRUNC_NOTE "[devbridge: output truncated at 256 KB]\n"

V3X_EXPORT const char *v3x_plugin_name = "devbridge";
V3X_PLUGIN_MANIFEST("devbridge", "0.6.0", "v3x", "runs V3X console commands for local tools (MCP server)", "release/1.15.1");

static const V3X_API *api;
static int g_has_capture;
static char g_pipe[MAX_PATH] = "\\\\.\\pipe\\v3x_devbridge";
static char g_token[DB_TOKEN_HEX + 1];
static db_allow g_allow;
static char g_allow_src[1024];

static char g_req[REQ_MAX + 1];
static char g_resp[RESP_MAX + 128];   /* + room for the END line */
static size_t g_resp_n;
static int g_trunc;

/* ------------------------------------------------------------------ config */

static int load_config(void)
{
    int enabled = 1;
    char path[MAX_PATH], name[200] = "";
    snprintf(g_allow_src, sizeof g_allow_src, "%s", DB_ALLOW_DEFAULT);
    snprintf(path, sizeof path, "%sdevbridge.ini", api->v3x_dir());
    FILE *f = fopen(path, "r");
    if (f) {
        char line[1024];
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
            else if (!_stricmp(k, "allow")) snprintf(g_allow_src, sizeof g_allow_src, "%s", v);
        }
        fclose(f);
    }
    db_allow_parse(&g_allow, g_allow_src);
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

/* ------------------------------------------------------------------ security */

/* "D:P(A;;GA;;;<user sid>)": protected DACL, full access for the user running the game only. */
static PSECURITY_DESCRIPTOR owner_only_sd(void)
{
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return NULL;
    union { TOKEN_USER tu; char raw[256]; } u;
    DWORD got = 0;
    BOOL ok = GetTokenInformation(tok, TokenUser, &u, sizeof u, &got);
    CloseHandle(tok);
    if (!ok) return NULL;
    LPWSTR sid = NULL;
    if (!ConvertSidToStringSidW(u.tu.User.Sid, &sid)) return NULL;
    wchar_t sddl[256];
    int k = _snwprintf(sddl, sizeof sddl / sizeof sddl[0] - 1, L"D:P(A;;GA;;;%s)", sid);
    sddl[sizeof sddl / sizeof sddl[0] - 1] = 0;
    LocalFree(sid);
    PSECURITY_DESCRIPTOR sd = NULL;
    if (k <= 0 || !ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, NULL)) return NULL;
    return sd;   /* LocalFree */
}

static int random_token(char *hex)
{
    typedef LONG(WINAPI * gen_fn)(void *, unsigned char *, ULONG, ULONG);
    HMODULE b = LoadLibraryExW(L"bcrypt.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    gen_fn gen = b ? (gen_fn)(void *)GetProcAddress(b, "BCryptGenRandom") : NULL;
    unsigned char r[DB_TOKEN_HEX / 2];
    if (!gen || gen(NULL, r, sizeof r, 2 /* BCRYPT_USE_SYSTEM_PREFERRED_RNG */) != 0) return 0;
    for (size_t i = 0; i < sizeof r; i++) snprintf(hex + 2 * i, 3, "%02x", r[i]);
    SecureZeroMemory(r, sizeof r);
    hex[DB_TOKEN_HEX] = 0;
    return 1;
}

/* Replaces binaries/v3x/devbridge.token with a new owner-only file holding the token. A file that
 * cannot be deleted (other owner / read-only) or a reparse point in its place -> 0 (no bridge). */
static int write_token_file(SECURITY_ATTRIBUTES *sa)
{
    char p8[MAX_PATH];
    wchar_t w[MAX_PATH];
    snprintf(p8, sizeof p8, "%sdevbridge.token", api->v3x_dir());
    if (!MultiByteToWideChar(CP_UTF8, 0, p8, -1, w, MAX_PATH)) return 0;
    if (!DeleteFileW(w) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        api->log(TAG, "cannot replace %s (error %lu)", p8, GetLastError());
        return 0;
    }
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (h == INVALID_HANDLE_VALUE) { api->log(TAG, "cannot create %s (error %lu)", p8, GetLastError()); return 0; }
    char line[DB_TOKEN_HEX + 2];
    memcpy(line, g_token, DB_TOKEN_HEX);
    line[DB_TOKEN_HEX] = '\n';
    DWORD wr = 0;
    BOOL ok = WriteFile(h, line, DB_TOKEN_HEX + 1, &wr, NULL) && wr == DB_TOKEN_HEX + 1;
    CloseHandle(h);
    SecureZeroMemory(line, sizeof line);
    if (!ok) { DeleteFileW(w); return 0; }
    return 1;
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
    char *id, *cmd;
    int pr = db_parse_line(line, &id, &cmd);
    if (pr == 0) return 0;                          /* blank line: ignored */

    g_resp_n = 0; g_trunc = 0;
    int rc;
    if (!*cmd) {
        resp_add("[devbridge: empty command]\n", 27);
        rc = -1;
    } else if (!db_allowed(&g_allow, cmd)) {
        char note[200], first[48];
        size_t fl = strcspn(cmd, " \t");
        v3x_sanitize_linen(first, sizeof first, cmd, fl);
        snprintf(note, sizeof note, "[devbridge: '%s' is not in the allow list (allow= in v3x/devbridge.ini; allow = * for all)]\n", first);
        resp_add(note, strlen(note));
        rc = -4;
    } else if (g_has_capture) {
        rc = api->exec_capture(cmd, sink, NULL);
    } else {
        rc = api->exec_command(cmd);
        char note[96];
        int k = snprintf(note, sizeof note, "[devbridge: output not captured (loader API v%u < 3)]\n", api->api_version);
        resp_add(note, k > 0 && (size_t)k < sizeof note ? (size_t)k : 0);
    }
    /* id <= 32 chars, so the END line always fits in the 128 spare bytes behind RESP_MAX */
    V3xStr eb = {g_resp, sizeof g_resp, g_resp_n, 0};
    v3x_str_appendf(&eb, "%s END %d\n", id, rc);
    g_resp_n = eb.len;
    return write_all(p, g_resp, g_resp_n);
}

typedef struct { HANDLE p; int authed; int stop; } client;

static int on_line(char *line, void *u)
{
    client *c = (client *)u;
    if (!c->authed) {
        char *s = line;
        while (*s == ' ' || *s == '\t' || *s == '\r') s++;
        if (!*s) return 0;
        if (db_auth_ok(s, g_token)) {
            c->authed = 1;
            return write_all(c->p, "auth END 0\n", 11);
        }
        static const char msg[] = "[devbridge: authentication failed (first line must be 'auth <token from v3x/devbridge.token>')]\nauth END -3\n";
        write_all(c->p, msg, sizeof msg - 1);
        FlushFileBuffers(c->p);
        c->stop = 1;
        return -1;
    }
    return handle_line(c->p, line);
}

/* Serves one connected client until it disconnects or misbehaves. */
static void serve_client(HANDLE p)
{
    size_t have = 0;
    client c = {p, 0, 0};
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(p, g_req + have, (DWORD)(REQ_MAX - have), &got, NULL) && GetLastError() != ERROR_MORE_DATA) return;
        if (!got) continue;
        have += got;
        int too_long = 0;
        size_t kept = db_feed(g_req, have, on_line, &c, &too_long);
        if (too_long) {
            static const char msg[] = "[devbridge: request line too long (max 4096 bytes), closing]\n0 END -2\n";
            write_all(p, msg, sizeof msg - 1);
            FlushFileBuffers(p);
            return;
        }
        if (kept == (size_t)-1) return;
        have = kept;
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

    PSECURITY_DESCRIPTOR sd = owner_only_sd();
    if (!sd) { a->log(TAG, "cannot build the owner-only pipe DACL (error %lu), bridge off", GetLastError()); return 0; }
    SECURITY_ATTRIBUTES sa = {sizeof sa, sd, FALSE};
    if (!random_token(g_token) || !write_token_file(&sa)) {
        a->log(TAG, "no session token (random source or v3x/devbridge.token failed), bridge off");
        LocalFree(sd);
        return 0;
    }
    HANDLE p = CreateNamedPipeA(g_pipe, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                1, 64 * 1024, REQ_MAX, 0, &sa);
    LocalFree(sd);
    if (p == INVALID_HANDLE_VALUE) {
        a->log(TAG, "CreateNamedPipe %s failed (error %lu; another game instance or a squatter?), bridge off", g_pipe, GetLastError());
        return 0;
    }
    HANDLE t = CreateThread(NULL, 1024 * 1024, bridge_thread, p, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t) {
        a->log(TAG, "CreateThread failed (error %lu), bridge off", GetLastError());
        CloseHandle(p);
        return 0;
    }
    CloseHandle(t);
    a->log(TAG, "listening on %s (output capture %s; owner-only, token in v3x/devbridge.token; allow %s)", g_pipe,
           g_has_capture ? "on" : "OFF: loader API < 3", g_allow.all ? "* (everything)" : g_allow_src);
    return 0;
}
