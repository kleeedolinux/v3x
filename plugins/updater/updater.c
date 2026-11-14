/* updater - finds V3X releases on GitHub that support the RUNNING game build and installs them after
 * the game exits (docs/updating.md).
 *
 * One background thread (never the main thread, never blocks the game): at most one automatic check
 * per 6 h (sooner when the game build / V3X version changed), result cached in v3x/update_cache.json.
 * Selection and parsing are pure (up_logic.c, test/updater_test.c).
 * Trust (the security review H1/M4): a release counts as SIGNED only when its compat.json.sig is an
 * Ed25519 signature by a key of installer/release_keys.h (up_sig.c); the setup's hash and size then come
 * from that signed compat.json. Only signed releases are downloaded and installed automatically
 * ([update] auto = 1); an unsigned one is only announced, and installed only after `update now`.
 * The download goes to v3x/update/V3X-Setup.exe (fixed name), is SHA-256 verified, and at
 * process_exit the file is opened without write/delete sharing, hashed again through that handle and
 * started while the handle is held (no swap between check and start), detached as
 * `V3X-Setup.exe --update --silent --game-dir <dir> --wait-pid <pid>`, so files are swapped only after
 * the game is gone. update/floor.txt keeps the highest V3X version that ran here or was started by the
 * updater; nothing below it is ever offered (rollback floor). Any verification failure = nothing is started.
 *
 * v3x.ini:
 *   [update]
 *   check = 1         ; automatic checks (0 = only `update check`)
 *   auto = 1          ; install a compatible SIGNED update when the game exits (0 = notify only;
 *                     ; `update now`). Unsigned releases are never installed automatically.
 *   channel = stable  ; beta = pre-releases too
 * env V3X_NO_UPDATE=1 disables network use and installing entirely; automatic checks run only inside
 * victoria3.exe (or with env V3X_UPDATER_FORCE=1), so test harnesses stay offline. env V3X_UPDATER_NO_NET=1
 * (tests) makes every request fail like a dead network and changes nothing else.
 *
 * Manifest "*": it must load on unknown game builds (safe mode), because that is the case it fixes. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "v3x.h"
#include "v3x_ini.h"
#include "v3x_util.h"
#include "v3x_safe.h"
#include "up_logic.h"
#include "updater.h"

#define TAG "updater"
#define UPDATER_VERSION "0.1.0"
#define MAX_ATTEMPTS 3   /* failed automatic installs of the same version before it stops trying */

V3X_EXPORT const char *v3x_plugin_name = TAG;
V3X_PLUGIN_MANIFEST(TAG, UPDATER_VERSION, "v3x",
                    "checks GitHub for V3X releases that support your game build; installs them after the game exits",
                    "*");

static const V3X_API *api;
static struct { int check, autoapply, beta, delay_s, allowed, nonet; } cfg;

/* the running install */
static char g_ver[48] = "0.0.0", g_build[64], g_branch[96];
static up_local g_local;

/* results: written by the updater thread, read by commands / process_exit under g_lock */
static CRITICAL_SECTION g_lock;
static up_catalog g_cat;          /* updater thread only */
static up_choice g_choice = {UP_UNKNOWN, -1, -1};
static up_release g_best;         /* copy of the chosen release */
static char g_status[600] = "update check has not run yet.";
static char g_last_err[200];
static long long g_last_attempt;

/* the verified setup on disk */
static volatile LONG g_ready, g_armed, g_spawned;
static char g_ready_path[MAX_PATH], g_ready_ver[48], g_ready_sha[65];
static int g_ready_signed;
static char g_floor[48];          /* rollback floor (update/floor.txt), >= g_ver */

static HANDLE g_wake;
static volatile LONG g_req;       /* REQ_* */
#define REQ_CHECK 1
#define REQ_DOWNLOAD 2

/* ------------------------------------------------------------------ WinHTTP (loaded on the thread) */

static struct {
    HMODULE m;
    __typeof__(&WinHttpOpen) Open;
    __typeof__(&WinHttpConnect) Connect;
    __typeof__(&WinHttpOpenRequest) OpenRequest;
    __typeof__(&WinHttpSendRequest) SendRequest;
    __typeof__(&WinHttpReceiveResponse) ReceiveResponse;
    __typeof__(&WinHttpQueryHeaders) QueryHeaders;
    __typeof__(&WinHttpReadData) ReadData;
    __typeof__(&WinHttpCloseHandle) CloseHandle;
    __typeof__(&WinHttpSetTimeouts) SetTimeouts;
    __typeof__(&WinHttpCrackUrl) CrackUrl;
    HINTERNET session;
} W;

static int winhttp_init(void)
{
    if (W.session) return 0;
    /* System32 only (M4): no search-path fallback, a winhttp.dll next to the game is never loaded */
    if (!W.m) W.m = LoadLibraryExW(L"winhttp.dll", NULL, 0x800 /* LOAD_LIBRARY_SEARCH_SYSTEM32 */);
    if (!W.m) return -1;
#define LD(f) if (!(*(FARPROC *)&W.f = GetProcAddress(W.m, "WinHttp" #f))) return -1
    LD(Open); LD(Connect); LD(OpenRequest); LD(SendRequest); LD(ReceiveResponse); LD(QueryHeaders);
    LD(ReadData); LD(CloseHandle); LD(SetTimeouts); LD(CrackUrl);
#undef LD
    char ua8[64];
    wchar_t ua[64];
    snprintf(ua8, sizeof ua8, "V3X/%s", g_ver);
    if (!MultiByteToWideChar(CP_UTF8, 0, ua8, -1, ua, 64)) return -1;
    W.session = W.Open(ua, 4 /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, Win 8.1+ */, WINHTTP_NO_PROXY_NAME,
                       WINHTTP_NO_PROXY_BYPASS, 0);
    if (!W.session)
        W.session = W.Open(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!W.session) return -1;
    W.SetTimeouts(W.session, 10000, 10000, 10000, 10000);   /* resolve, connect, send, receive: 10 s each */
    return 0;
}

/* up_http_fn over WinHTTP: HTTPS only, redirects followed (GitHub sends downloads to its CDN). */
static int win_get(void *ctx, const char *url, size_t max, char **body, size_t *len)
{
    (void)ctx;
    *body = NULL;
    *len = 0;
    if (cfg.nonet || winhttp_init()) return -10;
    wchar_t wurl[1024], host[256], path[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024)) return -11;
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 1024;
    wchar_t extra[512];
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 512;
    if (!W.CrackUrl(wurl, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return -12;
    size_t pl = wcslen(path), el = wcslen(extra);
    if (pl + el >= 1024) return -12;
    memcpy(path + pl, extra, (el + 1) * sizeof(wchar_t));
    int api_host = !wcscmp(host, L"api.github.com");
    int st = -13;
    HINTERNET c = W.Connect(W.session, host, uc.nPort, 0), r = NULL;
    if (!c) goto out;
    r = W.OpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!r) goto out;
    const wchar_t *hdr = api_host ? L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n"
                                  : L"Accept: application/octet-stream\r\n";
    if (!W.SendRequest(r, hdr, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !W.ReceiveResponse(r, NULL)) {
        st = -(int)(GetLastError() % 100000) - 100;
        goto out;
    }
    DWORD code = 0, sz = sizeof code;
    if (!W.QueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz,
                        WINHTTP_NO_HEADER_INDEX)) goto out;
    st = (int)code;
    if (st != 200) goto out;
    size_t cap = 65536, n = 0;
    char *b = (char *)v3x_alloc_array(cap + 1, 1);
    ULONGLONG deadline = GetTickCount64() + 300000;   /* whole body: 5 min max */
    for (;;) {
        if (!b) { st = -14; break; }
        if (n == cap) {
            if (cap >= max) { st = -15; break; }
            size_t nc = cap * 2 > max ? max : cap * 2;
            char *nb = (char *)v3x_realloc_array(b, nc + 1, 1);
            if (!nb) { st = -14; break; }
            b = nb; cap = nc;
        }
        DWORD got = 0;
        if (!W.ReadData(r, b + n, (DWORD)(cap - n > 1u << 20 ? 1u << 20 : cap - n), &got)) { st = -16; break; }
        if (!got) break;
        n += got;
        if (GetTickCount64() > deadline) { st = -17; break; }
    }
    if (st == 200) { b[n] = 0; *body = b; *len = n; }
    else free(b);
out:
    if (r) W.CloseHandle(r);
    if (c) W.CloseHandle(c);
    return st;
}

/* ------------------------------------------------------------------ files */

static int read_file(const char *path, char **out, size_t *n, size_t max)
{
    *out = NULL;
    *n = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char *b = max < (size_t)-1 ? (char *)v3x_alloc_array(max + 1, 1) : NULL;
    if (!b) { fclose(f); return -1; }
    size_t k = fread(b, 1, max, f);
    fclose(f);
    b[k] = 0;
    *out = b;
    *n = k;
    return 0;
}

static void save_cache(void)
{
    static char buf[1 << 20];
    char path[MAX_PATH];
    size_t n = up_cache_write(&g_cat, buf, sizeof buf);
    if (n && v3x_path(api, path, sizeof path, "update_cache.json")) v3x_file_write_atomic(path, buf, n);
}

static void load_cache(void)
{
    char path[MAX_PATH], *b;
    size_t n;
    if (!v3x_path(api, path, sizeof path, "update_cache.json") || read_file(path, &b, &n, 2 << 20)) return;
    if (up_cache_read(b, n, &g_cat)) memset(&g_cat, 0, sizeof g_cat);
    free(b);
}

static void update_dir(char *out, size_t cap)
{
    v3x_path(api, out, cap, "update");
    CreateDirectoryA(out, NULL);
}

/* "<version> <count>" in update/attempts.txt */
static int attempts_get(const char *ver)
{
    char p[MAX_PATH], v[48] = "";
    int n = 0;
    if (!v3x_path(api, p, sizeof p, "update\\attempts.txt")) return 0;
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    if (fscanf(f, "%47s %d", v, &n) != 2) n = 0;
    fclose(f);
    return strcmp(v, ver) ? 0 : n;
}
static void attempts_set(const char *ver, int n)
{
    char p[MAX_PATH], t[80];
    if (!v3x_path(api, p, sizeof p, "update\\attempts.txt")) return;
    int k = snprintf(t, sizeof t, "%s %d\n", ver, n);
    v3x_file_write_atomic(p, t, (size_t)k);
}

/* rollback floor: update/floor.txt = the highest V3X version that ran here or that the updater
 * started; raised, never lowered (an edited lower value is ignored: g_ver is always part of it) */
static void floor_load(void)
{
    char p[MAX_PATH], v[48] = "";
    snprintf(g_floor, sizeof g_floor, "%s", g_ver);
    if (!v3x_path(api, p, sizeof p, "update\\floor.txt")) return;
    FILE *f = fopen(p, "rb");
    if (f) {
        if (fscanf(f, "%47s", v) != 1) v[0] = 0;
        fclose(f);
    }
    if (v[0] && up_version_cmp(v, g_floor) > 0) snprintf(g_floor, sizeof g_floor, "%s", v);
}
static void floor_raise(const char *ver)
{
    char p[MAX_PATH], t[64];
    if (up_version_cmp(ver, g_floor) > 0) snprintf(g_floor, sizeof g_floor, "%s", ver);
    update_dir(p, sizeof p);
    if (!v3x_path(api, p, sizeof p, "update\\floor.txt")) return;
    int k = snprintf(t, sizeof t, "%s\n", g_floor);
    if (k > 0 && (size_t)k < sizeof t) v3x_file_write_atomic(p, t, (size_t)k);
}

/* After an automatic install: report install.log's last update-result line once. */
static void report_last_install(void)
{
    char p[MAX_PATH], *b, detail[160];
    size_t n;
    if (!v3x_path(api, p, sizeof p, "update\\attempts.txt") || GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) return;
    if (!v3x_path(api, p, sizeof p, "install.log") || read_file(p, &b, &n, 1 << 20)) return;
    int ok = up_install_result(b, n, detail, sizeof detail);
    free(b);
    if (ok == 1 && !up_version_cmp(detail, g_ver)) {
        api->log(TAG, "V3X was updated to %s after the last session", g_ver);
        if (v3x_path(api, p, sizeof p, "update\\attempts.txt")) DeleteFileA(p);
    } else if (ok == 0) {
        V3X_LOGW(api, TAG, "the last automatic update failed (install.log: %s)", detail);
    }
}

static void remove_stale_setup(void)
{
    char dir[MAX_PATH], p[MAX_PATH];
    update_dir(dir, sizeof dir);
    snprintf(p, sizeof p, "%s\\" UP_SETUP_NAME, dir);
    if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES && DeleteFileA(p)) api->log(TAG, "removed an old downloaded setup");
}

/* ------------------------------------------------------------------ events */

static V3X_UpdateInfo g_info;
static char g_info_installed[48], g_info_available[48], g_info_msg[600];
static char g_emitted[64];   /* "<version>:<ready>" last emitted */

static void emit_main(void *u)
{
    (void)u;
    EnterCriticalSection(&g_lock);
    snprintf(g_info_installed, sizeof g_info_installed, "%s", g_ver);
    snprintf(g_info_available, sizeof g_info_available, "%s", g_best.version);
    snprintf(g_info_msg, sizeof g_info_msg, "%s", g_status);
    g_info.struct_size = sizeof g_info;
    g_info.state = (int32_t)g_choice.state;
    g_info.installed = g_info_installed;
    g_info.available = g_info_available;
    g_info.message = g_info_msg;
    g_info.auto_install = cfg.autoapply || g_armed;
    g_info.ready = g_ready && !strcmp(g_ready_ver, g_best.version);
    LeaveCriticalSection(&g_lock);
    api->emit_event(V3X_EVENT_UPDATE_AVAILABLE, &g_info);
}

static void maybe_emit(void)
{
    char key[64];
    if (g_choice.state != UP_AVAILABLE && g_choice.state != UP_URGENT) return;
    snprintf(key, sizeof key, "%s:%d", g_best.version, (int)g_ready);
    if (!strcmp(key, g_emitted)) return;
    snprintf(g_emitted, sizeof g_emitted, "%s", key);
    if (V3X_HAS(api, emit_event) && api->run_on_main) api->run_on_main(emit_main, NULL);
}

/* ------------------------------------------------------------------ the check */

static void publish(up_choice c)
{
    char text[600];
    up_status_text(&g_cat, &g_local, c, text, sizeof text);
    EnterCriticalSection(&g_lock);
    int changed = strcmp(text, g_status) != 0 || c.state != g_choice.state;
    g_choice = c;
    if (c.best >= 0) g_best = g_cat.r[c.best];
    else memset(&g_best, 0, sizeof g_best);
    snprintf(g_status, sizeof g_status, "%s", text);
    LeaveCriticalSection(&g_lock);
    if (changed) {
        int lvl = c.state == UP_URGENT || c.state == UP_UNSUPPORTED_NOFIX ? V3X_LOG_WARN : V3X_LOG_INFO;
        v3x_logf(api, lvl, TAG, "%s", text);
        if (c.state == UP_URGENT || c.state == UP_AVAILABLE) {
            if (api->print && api->api_version >= 2) {
                api->print("V3X: %s %s", text,
                           cfg.autoapply ? "It will be installed when you quit the game."
                                         : "Type 'update now' to install it when you quit the game.");
            }
        }
    }
}

static void do_check(int forced)
{
    char key[200];
    long long now = (long long)time(NULL);
    up_local_key(&g_local, key, sizeof key);
    int fetch = forced ? up_force_allowed(g_cat.attempt_at, now)
                       : cfg.check && cfg.allowed && up_should_fetch(&g_cat, key, now, UP_INTERVAL_S);
    if (fetch) {
        char err[160];
        g_last_attempt = now;
        if (up_fetch_catalog(win_get, NULL, &g_local, now, &g_cat, err, sizeof err) == 0) {
            g_last_err[0] = 0;
            api->log(TAG, "checked GitHub: %d release(s)", g_cat.n);
        } else {
            g_cat.attempt_at = now;
            snprintf(g_last_err, sizeof g_last_err, "%s", err);
            api->log(TAG, "update check failed: %s", err);   /* the one line when offline */
        }
        save_cache();
    }
    if (g_cat.checked_at <= 0) {
        up_choice c = {fetch ? UP_OFFLINE : UP_UNKNOWN, -1, -1};
        publish(c);
        return;
    }
    up_choice c = up_select(&g_cat, &g_local);
    publish(c);
    if (c.state == UP_CURRENT) remove_stale_setup();
}

/* Downloads + verifies the chosen setup (or reuses a verified one on disk). */
static void ensure_download(int asked)
{
    static up_release r;   /* large (signed compat.json kept verbatim); updater thread only */
    EnterCriticalSection(&g_lock);
    r = g_best;
    int have = g_choice.state == UP_AVAILABLE || g_choice.state == UP_URGENT;
    LeaveCriticalSection(&g_lock);
    if (!have || !r.version[0]) return;
    if (g_ready && !strcmp(g_ready_ver, r.version)) return;
    int signed_ok = up_release_auto_ok(&r);
    if (!signed_ok && !asked) {   /* H1: unsigned = announce only, never fetched or armed on its own */
        static char said[48];
        if (strcmp(said, r.version)) {
            snprintf(said, sizeof said, "%s", r.version);
            V3X_LOGW(api, TAG, "V3X %s is NOT signed (no valid compat.json.sig): it is not installed automatically; "
                     "type 'update now' to install it anyway", r.version);
        }
        return;
    }
    char dir[MAX_PATH], path[MAX_PATH], hex[65];
    update_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s\\" UP_SETUP_NAME, dir);   /* M4: never a name from the network / cache */
    if (up_sha256_file(path, hex) || !up_hex_eq(hex, r.setup_sha)) {
        char *data, err[160];
        size_t n;
        api->log(TAG, "downloading V3X %s setup", r.version);
        if (up_download_setup(win_get, NULL, &r, &data, &n, err, sizeof err)) {
            V3X_LOGW(api, TAG, "download of V3X %s not used: %s", r.version, err);
            return;
        }
        int w = v3x_file_write_atomic(path, data, n);
        free(data);
        if (w || up_sha256_file(path, hex) || !up_hex_eq(hex, r.setup_sha)) {
            V3X_LOGW(api, TAG, "could not store the verified setup at %s", path);
            DeleteFileA(path);
            return;
        }
    }
    EnterCriticalSection(&g_lock);
    snprintf(g_ready_path, sizeof g_ready_path, "%s", path);
    snprintf(g_ready_ver, sizeof g_ready_ver, "%s", r.version);
    snprintf(g_ready_sha, sizeof g_ready_sha, "%s", r.setup_sha);
    g_ready_signed = signed_ok;
    InterlockedExchange(&g_ready, 1);
    if (cfg.autoapply && signed_ok) InterlockedExchange(&g_armed, 1);
    LeaveCriticalSection(&g_lock);
    if (signed_ok)
        api->log(TAG, "V3X %s setup downloaded and verified (signed, key %s; SHA-256 %.16s...); %s", r.version, r.key_id,
                 r.setup_sha, g_armed ? "it is installed when the game exits" : "type 'update now' to install it when the game exits");
    else
        V3X_LOGW(api, TAG, "V3X %s setup downloaded (SHA-256 %.16s... matches, but the release is NOT signed); "
                 "installed when the game exits because you typed 'update now'", r.version, r.setup_sha);
    maybe_emit();
}

static DWORD WINAPI updater_thread(void *u)
{
    (void)u;
    load_cache();
    report_last_install();
    if (cfg.delay_s > 0 && WaitForSingleObject(g_wake, (DWORD)cfg.delay_s * 1000) == WAIT_OBJECT_0)
        ResetEvent(g_wake);
    int first = 1;
    for (;;) {
        LONG req = InterlockedExchange(&g_req, 0);
        if (first || (req & REQ_CHECK)) do_check((req & REQ_CHECK) != 0);
        first = 0;
        maybe_emit();
        if ((cfg.autoapply && cfg.allowed) || (req & REQ_DOWNLOAD) || g_armed) ensure_download((req & REQ_DOWNLOAD) != 0 || g_armed);
        WaitForSingleObject(g_wake, INFINITE);
    }
    return 0;
}

/* ------------------------------------------------------------------ apply at exit */

static void on_exit(const char *name, void *data, void *u)
{
    (void)name; (void)data; (void)u;
    if (!g_armed || !g_ready || InterlockedExchange(&g_spawned, 1)) return;
    char path[MAX_PATH], ver[48], sha[65], hex[65], root[MAX_PATH], cmd[2048], dir[MAX_PATH];
    EnterCriticalSection(&g_lock);
    snprintf(path, sizeof path, "%s", g_ready_path);
    snprintf(ver, sizeof ver, "%s", g_ready_ver);
    snprintf(sha, sizeof sha, "%s", g_ready_sha);
    LeaveCriticalSection(&g_lock);
    if (up_version_cmp(ver, g_ver) <= 0 || up_version_cmp(ver, g_floor) < 0) return;   /* never downgrade */
    /* M4: open without write/delete sharing and hash through this handle; it stays open until the
     * process is created, so the file that runs is the file that was checked */
    HANDLE hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        V3X_LOGE(api, TAG, "update NOT started: cannot open %s (error %lu)", path, GetLastError());
        return;
    }
    {
        static unsigned char buf[1 << 16];
        up_sha256 hs;
        uint8_t dg[32];
        DWORD got = 0;
        unsigned long long total = 0;
        int rd = 1;
        up_sha256_init(&hs);
        while ((rd = ReadFile(hf, buf, sizeof buf, &got, NULL) != 0) && got) {
            up_sha256_update(&hs, buf, got);
            total += got;
            if (total > UP_MAX_SETUP) { rd = 0; break; }
        }
        up_sha256_final(&hs, dg);
        static const char hx[] = "0123456789abcdef";
        for (int i = 0; i < 32; i++) { hex[2 * i] = hx[dg[i] >> 4]; hex[2 * i + 1] = hx[dg[i] & 15]; }
        hex[64] = 0;
        if (!rd || !up_hex_eq(hex, sha)) {
            CloseHandle(hf);
            V3X_LOGE(api, TAG, "update NOT started: %s changed on disk (SHA-256 mismatch)", path);
            return;
        }
    }
    int tries = attempts_get(ver);
    if (tries >= MAX_ATTEMPTS) {
        CloseHandle(hf);
        V3X_LOGW(api, TAG, "update to %s NOT started: it failed %d times (see install.log; run %s by hand)", ver, tries, path);
        return;
    }
    if (up_game_root(api->game_dir(), root, sizeof root) ||
        up_build_cmdline(cmd, sizeof cmd, path, root, GetCurrentProcessId())) {
        CloseHandle(hf);
        V3X_LOGE(api, TAG, "update NOT started: path too long");
        return;
    }
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '\\');
    if (slash) *slash = 0;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    DWORD fl = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    BOOL ok = CreateProcessA(path, cmd, NULL, NULL, FALSE, fl | CREATE_BREAKAWAY_FROM_JOB, NULL, dir, &si, &pi);
    if (!ok) ok = CreateProcessA(path, cmd, NULL, NULL, FALSE, fl, NULL, dir, &si, &pi);
    DWORD cerr = ok ? 0 : GetLastError();
    CloseHandle(hf);   /* the image section now holds the file */
    if (!ok) {
        V3X_LOGE(api, TAG, "update NOT started: CreateProcess failed (error %lu)", cerr);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    attempts_set(ver, tries + 1);
    floor_raise(ver);
    api->log(TAG, "started the V3X %s setup (pid %lu, %s); it installs after the game has exited: %s", ver, pi.dwProcessId,
             g_ready_signed ? "signed release" : "UNSIGNED release, started because you typed 'update now'", cmd);
}

/* ------------------------------------------------------------------ console */

static void wake(LONG bits)
{
    InterlockedOr(&g_req, bits);
    if (g_wake) SetEvent(g_wake);
}

static void cmd_update(const char *args, void *u)
{
    (void)u;
    char a[32] = "";
    sscanf(args, "%31s", a);
    EnterCriticalSection(&g_lock);
    up_choice c = g_choice;
    char status[600], bestv[48], err[200];
    snprintf(status, sizeof status, "%s", g_status);
    snprintf(bestv, sizeof bestv, "%s", g_best.version);
    snprintf(err, sizeof err, "%s", g_last_err);
    int ready = g_ready && !strcmp(g_ready_ver, bestv);
    LeaveCriticalSection(&g_lock);
    int offer = c.state == UP_AVAILABLE || c.state == UP_URGENT;
    if (!_stricmp(a, "check")) {
        long long now = (long long)time(NULL);
        if (!up_force_allowed(g_last_attempt, now)) {
            api->print("update: checked %lld s ago; try again in a minute.", now - g_last_attempt);
            return;
        }
        wake(REQ_CHECK);
        api->print("update: checking GitHub in the background; type 'update' for the result.");
    } else if (!_stricmp(a, "now")) {
        if (!offer) { api->print("update: nothing to install. %s", status); return; }
        EnterCriticalSection(&g_lock);
        int sgn = up_release_auto_ok(&g_best);
        LeaveCriticalSection(&g_lock);
        if (!sgn) api->print("update: WARNING: V3X %s is not signed by the V3X release key; installing it because you asked.", bestv);
        InterlockedExchange(&g_armed, 1);
        if (ready) api->print("update: V3X %s will be installed when you quit the game.", bestv);
        else {
            wake(REQ_DOWNLOAD);
            api->print("update: downloading V3X %s in the background; it is installed when you quit the game.", bestv);
        }
    } else if (!_stricmp(a, "cancel")) {
        InterlockedExchange(&g_armed, 0);
        api->print("update: nothing will be installed when the game exits (this session).");
    } else if (!a[0] || !_stricmp(a, "status")) {
        api->print("update: %s", status);
        api->print("  installed V3X %s (api v%d), game %s build %s%s; channel %s; auto-install %s; checks %s",
                   g_ver, g_local.api_version, g_branch, g_build, g_local.build_supported ? "" : " (not supported by this V3X)",
                   cfg.beta ? "beta" : "stable", cfg.autoapply ? "on" : "off",
                   !cfg.allowed ? "off (not victoria3.exe / V3X_NO_UPDATE)" : cfg.check ? "every 6 h" : "manual");
        if (offer)
            api->print("  V3X %s (%s): %s%s", bestv, up_release_auto_ok(&g_best) ? "signed" : "NOT signed", ready ? "downloaded and verified" : "not downloaded yet",
                       g_armed && ready ? ", installed when you quit the game" : g_armed ? ", will be installed when you quit" : "");
        if (err[0]) api->print("  last check failed: %s", err);
    } else {
        api->print("usage: update [status] | update check | update now | update cancel");
    }
}

/* ------------------------------------------------------------------ load */

static int build_known(const char *id)
{
    HMODULE core = GetModuleHandleA("version.dll");
    const char *known = core ? (const char *)(void *)GetProcAddress(core, "v3x_loader_known_builds") : NULL;
    if (!known) return 1;   /* loader before 0.5.0: no gate, so the build counts as supported */
    size_t k = strlen(id);
    for (const char *p = known; *p;) {
        while (*p == ';' || *p == ' ') p++;
        const char *e = p;
        while (*e && *e != ';') e++;
        if ((size_t)(e - p) == k && k && !_strnicmp(p, id, k)) return 1;
        p = e;
    }
    return 0;
}

static void read_config(void)
{
    char path[MAX_PATH], ch[16];
    V3xIni ini;
    cfg.check = cfg.autoapply = 1;
    cfg.beta = 0;
    cfg.delay_s = 20;
    if (v3x_path(api, path, sizeof path, "v3x.ini") && v3x_ini_load(&ini, path) == 0) {
        cfg.check = v3x_ini_bool(&ini, "update", "check", 1);
        cfg.autoapply = v3x_ini_bool(&ini, "update", "auto", 1);
        v3x_ini_str(&ini, "update", "channel", "stable", ch, sizeof ch);
        cfg.beta = !_stricmp(ch, "beta");
        cfg.delay_s = (int)v3x_ini_int(&ini, "update", "delay_sec", 20);
        if (cfg.delay_s < 0 || cfg.delay_s > 600) cfg.delay_s = 20;
        v3x_ini_free(&ini);
    }
    char exe[MAX_PATH], env[8];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    const char *b = strrchr(exe, '\\');
    b = b ? b + 1 : exe;
    cfg.allowed = !_stricmp(b, "victoria3.exe") ||
                  (GetEnvironmentVariableA("V3X_UPDATER_FORCE", env, sizeof env) && env[0] == '1');
    if (GetEnvironmentVariableA("V3X_NO_UPDATE", env, sizeof env) && env[0] == '1') cfg.allowed = 0, cfg.nonet = 1;
    /* test hook: everything as usual except that every request fails like a dead network */
    if (GetEnvironmentVariableA("V3X_UPDATER_NO_NET", env, sizeof env) && env[0] == '1') cfg.nonet = 1;
    if (!cfg.allowed) cfg.autoapply = 0;
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    InitializeCriticalSection(&g_lock);
    if (V3X_HAS(a, loader_version)) snprintf(g_ver, sizeof g_ver, "%s", a->loader_version());
    else {
        HMODULE core = GetModuleHandleA("version.dll");
        const char *s = core ? (const char *)(void *)GetProcAddress(core, "v3x_loader_semver") : NULL;
        if (s) snprintf(g_ver, sizeof g_ver, "%s", s);
    }
    snprintf(g_build, sizeof g_build, "%s", V3X_HAS(a, game_build_id) ? a->game_build_id() : "");
    snprintf(g_branch, sizeof g_branch, "%s", a->game_version ? a->game_version() : "");
    read_config();
    g_local.loader_version = g_ver;
    g_local.api_version = (int)a->api_version;
    g_local.build_id = g_build;
    g_local.branch = g_branch;
    g_local.beta = cfg.beta;
    g_local.build_supported = build_known(g_build);
    floor_load();
    floor_raise(g_ver);   /* the running version is part of the floor from now on */
    g_local.floor = g_floor;
    if (a->api_version >= 2) a->register_command("update", "V3X updates: update [status|check|now|cancel]", cmd_update, NULL);
    if (a->api_version >= 3) {
        int erc = a->on_event(V3X_EVENT_PROCESS_EXIT, on_exit, NULL);
        if (erc != 0 && erc != V3X_EEXIST)
            V3X_LOGW(a, TAG, "on_event(%s) failed (%d): a download in progress is not stopped at game exit", V3X_EVENT_PROCESS_EXIT, erc);
    }
    g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE t = g_wake ? CreateThread(NULL, 256 * 1024, updater_thread, NULL, 0, NULL) : NULL;
    if (!t) { V3X_LOGW(a, TAG, "could not start the update thread"); return 0; }
    CloseHandle(t);
    a->log(TAG, "V3X %s, game %s build %s%s; checks %s, auto-install %s, channel %s", g_ver, g_branch, g_build,
           g_local.build_supported ? "" : " (not supported by this V3X)",
           !cfg.allowed ? "off" : cfg.check ? "on" : "manual", cfg.autoapply ? "on" : "off", cfg.beta ? "beta" : "stable");
    return 0;
}
