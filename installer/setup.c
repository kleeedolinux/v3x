/* V3X-Setup.exe: installs, repairs and uninstalls V3X for one Victoria 3 folder. No admin rights.
 *
 * The exe is a stub; installer/make_dist.py appends the payload (the files) to it:
 *   [file bytes ...][index: UTF-8 text lines][footer 64 bytes]
 *   index line:  <kind> <offset> <size> <sha256 hex> <path relative to binaries/, '/'-separated>
 *   kinds:       bin    always installed, overwritten when its hash differs (version.dll, plugins)
 *                ini    written only if the file is missing (user settings are never overwritten)
 *                v3xini v3x/v3x.ini: written if missing; else only missing "<plugin>=0" lines added
 *                meta   meta.txt (version, supported builds, plugin profile), not installed
 *   footer:      "V3XPAYL1" u64 index_offset u32 index_size u32 count u8 index_sha256[32] "V3XEND\0\0"
 * Every hash is checked before anything is written; a damaged download changes nothing.
 *
 * Install / repair (same operation; repair = rewrite every missing or changed bin file):
 *   refuse while <game>/binaries/victoria3.exe is running (process image path compare);
 *   refuse (GUI: ask) when the exe's PE TimeDateStamp-SizeOfImage (= core's game_build_id) is not
 *   listed in meta.txt; back up a binaries/version.dll that is not V3X to v3x/backup/;
 *   write binaries/v3x/install_manifest.txt (every file we own + its SHA-256).
 * Uninstall: removes exactly the manifest's files (settings files only if unchanged), restores the
 *   backed-up version.dll, keeps logs/reports unless --purge (then binaries/v3x/ goes entirely).
 *
 * CLI (for tests and scripted use; without --silent the GUI opens):
 *   --silent            no window; output on stdout
 *   --game-dir <path>   the "Victoria 3" folder (or its binaries folder); default: found via Steam
 *   --uninstall         uninstall instead of install       --repair   same as install
 *   --purge             with --uninstall: also delete binaries/v3x/ (logs, reports, settings)
 *   --force             install even if the game build is not supported
 *   --status            print what is installed, change nothing
 * Exit codes: 0 ok, 1 error, 2 unsupported game build, 3 game running, 4 game not found,
 *   5 not installed (uninstall without manifest), 6 damaged payload, 7 usage. */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum { RC_OK = 0, RC_ERR = 1, RC_UNSUPPORTED = 2, RC_RUNNING = 3, RC_NOGAME = 4, RC_NOTINST = 5,
       RC_PAYLOAD = 6, RC_USAGE = 7 };

#define MANIFEST_REL L"v3x\\install_manifest.txt"
#define BACKUP_DIR_REL L"v3x\\backup"

/* ------------------------------------------------------------------ output */

static wchar_t *g_out; static size_t g_out_n, g_out_cap;
static int g_silent;

static void say(const wchar_t *fmt, ...)
{
    wchar_t line[2048];
    va_list ap; va_start(ap, fmt);
    int n = _vsnwprintf(line, 2046, fmt, ap);
    va_end(ap);
    if (n < 0) n = 2046;
    /* manifest paths use '/': show them Windows-style after every "binaries\" */
    for (wchar_t *q = line; (q = wcsstr(q, L"binaries\\")) != NULL;)
        for (q += 9; *q && *q != L' ' && *q != L'('; q++) if (*q == L'/') *q = L'\\';
    line[n++] = L'\n'; line[n] = 0;
    if (g_out_n + (size_t)n + 1 > g_out_cap) {
        size_t cap = (g_out_cap + (size_t)n + 1) * 2;
        wchar_t *p = realloc(g_out, cap * sizeof(wchar_t));
        if (!p) return;
        g_out = p; g_out_cap = cap;
    }
    memcpy(g_out + g_out_n, line, ((size_t)n + 1) * sizeof(wchar_t));
    g_out_n += (size_t)n;
}

static void flush_stdout(void)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS))
            h = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    }
    if (!h || h == INVALID_HANDLE_VALUE || !g_out) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, g_out, (int)g_out_n, NULL, 0, NULL, NULL);
    char *u = malloc((size_t)n + 1);
    if (!u) return;
    WideCharToMultiByte(CP_UTF8, 0, g_out, (int)g_out_n, u, n, NULL, NULL);
    DWORD w; WriteFile(h, u, (DWORD)n, &w, NULL);
    free(u);
}

/* ------------------------------------------------------------------ small helpers */

static void sha256(const void *data, size_t n, uint8_t out[32])
{
    BCRYPT_ALG_HANDLE alg = NULL; BCRYPT_HASH_HANDLE h = NULL;
    memset(out, 0, 32);
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)) return;
    if (!BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0)) {
        BCryptHashData(h, (PUCHAR)data, (ULONG)n, 0);
        BCryptFinishHash(h, out, 32, 0);
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
}

static void hex32(const uint8_t h[32], char out[65])
{
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = d[h[i] >> 4]; out[2 * i + 1] = d[h[i] & 15]; }
    out[64] = 0;
}

static int read_all(const wchar_t *path, uint8_t **buf, size_t *len)
{
    *buf = NULL; *len = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart > (1LL << 30)) { CloseHandle(f); return 0; }
    uint8_t *b = malloc((size_t)sz.QuadPart + 1);
    if (!b) { CloseHandle(f); return 0; }
    size_t got = 0;
    while (got < (size_t)sz.QuadPart) {
        DWORD r = 0;
        if (!ReadFile(f, b + got, (DWORD)min((size_t)sz.QuadPart - got, (size_t)1 << 24), &r, NULL) || !r) break;
        got += r;
    }
    CloseHandle(f);
    if (got != (size_t)sz.QuadPart) { free(b); return 0; }
    b[got] = 0;
    *buf = b; *len = got;
    return 1;
}

static int file_exists(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int dir_exists(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* sha256 hex of a file; 0 if it cannot be read */
static int hash_file(const wchar_t *path, char hex[65])
{
    uint8_t *b; size_t n; uint8_t h[32];
    if (!read_all(path, &b, &n)) return 0;
    sha256(b, n, h); free(b); hex32(h, hex);
    return 1;
}

static void mkdirs_for(const wchar_t *file)
{
    wchar_t p[MAX_PATH * 2];
    wcsncpy(p, file, MAX_PATH * 2 - 1); p[MAX_PATH * 2 - 1] = 0;
    for (wchar_t *s = p + 3; *s; s++) {
        if (*s == L'\\') { *s = 0; CreateDirectoryW(p, NULL); *s = L'\\'; }
    }
}

/* write via temp file + rename so a crash never leaves half a DLL */
static int write_all(const wchar_t *path, const void *data, size_t n)
{
    mkdirs_for(path);
    wchar_t tmp[MAX_PATH * 2];
    _snwprintf(tmp, MAX_PATH * 2, L"%s.v3xtmp", path);
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD w = 0; BOOL ok = n ? WriteFile(f, data, (DWORD)n, &w, NULL) && w == n : TRUE;
    ok = FlushFileBuffers(f) && ok;
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp);
        return 0;
    }
    return 1;
}

static void join(wchar_t *out, const wchar_t *a, const wchar_t *b)
{
    _snwprintf(out, MAX_PATH * 2, L"%s\\%s", a, b);
    out[MAX_PATH * 2 - 1] = 0;
}

static void utf8_to_w(const char *s, wchar_t *out, int cap)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap);
    if (n <= 0) out[0] = 0;
}

static void rel_to_path(const wchar_t *bin_dir, const char *rel, wchar_t *out)
{
    wchar_t w[MAX_PATH];
    utf8_to_w(rel, w, MAX_PATH);
    for (wchar_t *s = w; *s; s++) if (*s == L'/') *s = L'\\';
    join(out, bin_dir, w);
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t' || (unsigned char)s[i] == 0xef || (unsigned char)s[i] == 0xbb ||
           (unsigned char)s[i] == 0xbf) i++;
    if (i) memmove(s, s + i, n - i + 1);
}

/* ------------------------------------------------------------------ payload */

typedef struct { char kind[8]; uint64_t off, size; char sha[65]; char rel[260]; const uint8_t *data; } Entry;

static uint8_t *g_self; static size_t g_self_n;
static Entry *g_ent; static int g_nent;
static char g_version[64] = "?";
typedef struct { char id[40]; char branch[64]; } Build;
static Build g_builds[16]; static int g_nbuilds;
typedef struct { char name[64]; int enabled; } Prof;
static Prof g_prof[64]; static int g_nprof;

static int load_payload(void)
{
    wchar_t self[MAX_PATH * 2];
    GetModuleFileNameW(NULL, self, MAX_PATH * 2);
    if (!read_all(self, &g_self, &g_self_n) || g_self_n < 64) return 0;
    const uint8_t *ft = g_self + g_self_n - 64;
    if (memcmp(ft, "V3XPAYL1", 8) || memcmp(ft + 56, "V3XEND\0\0", 8)) return 0;
    uint64_t ioff; uint32_t isz, cnt;
    memcpy(&ioff, ft + 8, 8); memcpy(&isz, ft + 16, 4); memcpy(&cnt, ft + 20, 4);
    if (ioff + isz > g_self_n - 64 || cnt > 4096) return 0;
    uint8_t h[32];
    sha256(g_self + ioff, isz, h);
    if (memcmp(h, ft + 24, 32)) return 0;
    char *idx = malloc(isz + 1);
    memcpy(idx, g_self + ioff, isz); idx[isz] = 0;
    g_ent = calloc(cnt ? cnt : 1, sizeof(Entry));
    g_nent = 0;
    for (char *line = strtok(idx, "\n"); line; line = strtok(NULL, "\n")) {
        Entry *e = &g_ent[g_nent];
        unsigned long long off, size;
        if (g_nent >= (int)cnt || sscanf(line, "%7s %llu %llu %64s %259s", e->kind, &off, &size, e->sha, e->rel) != 5) {
            free(idx); return 0;
        }
        e->off = off; e->size = size;
        if (off + size > ioff) { free(idx); return 0; }
        e->data = g_self + off;
        char hx[65]; sha256(e->data, (size_t)size, h); hex32(h, hx);
        if (strcmp(hx, e->sha)) { free(idx); return 0; }
        g_nent++;
    }
    free(idx);
    if (g_nent != (int)cnt) return 0;
    for (int i = 0; i < g_nent; i++) {
        if (strcmp(g_ent[i].kind, "meta")) continue;
        char *m = malloc(g_ent[i].size + 1);
        memcpy(m, g_ent[i].data, g_ent[i].size); m[g_ent[i].size] = 0;
        for (char *line = strtok(m, "\n"); line; line = strtok(NULL, "\n")) {
            char a[64], b[64], c[64];
            int k = sscanf(line, "%63s %63s %63s", a, b, c);
            if (k >= 2 && !strcmp(a, "version")) snprintf(g_version, sizeof g_version, "%s", b);
            else if (k >= 2 && !strcmp(a, "build") && g_nbuilds < 16) {
                snprintf(g_builds[g_nbuilds].id, 40, "%s", b);
                snprintf(g_builds[g_nbuilds].branch, 64, "%s", k >= 3 ? c : "");
                g_nbuilds++;
            } else if (k >= 3 && !strcmp(a, "plugin") && g_nprof < 64) {
                snprintf(g_prof[g_nprof].name, 64, "%s", b);
                g_prof[g_nprof].enabled = atoi(c);
                g_nprof++;
            }
        }
        free(m);
    }
    return 1;
}

/* ------------------------------------------------------------------ game detection */

static int is_game_dir(const wchar_t *g)
{
    wchar_t p[MAX_PATH * 2];
    join(p, g, L"binaries\\victoria3.exe");
    return file_exists(p);
}

/* accepts the "Victoria 3" folder, its binaries folder or the exe; writes the game root */
static int normalize_game_dir(const wchar_t *in, wchar_t *out)
{
    wchar_t p[MAX_PATH * 2];
    if (!GetFullPathNameW(in, MAX_PATH * 2, p, NULL)) return 0;
    size_t n = wcslen(p);
    while (n > 3 && (p[n - 1] == L'\\' || p[n - 1] == L'/')) p[--n] = 0;
    if (n > 14 && !_wcsicmp(p + n - 14, L"\\victoria3.exe")) { p[n - 14] = 0; n -= 14; }
    if (is_game_dir(p)) { wcscpy(out, p); return 1; }
    if (n > 9 && !_wcsicmp(p + n - 9, L"\\binaries")) {
        p[n - 9] = 0;
        if (is_game_dir(p)) { wcscpy(out, p); return 1; }
    }
    return 0;
}

static int try_library(const wchar_t *lib, wchar_t *out)
{
    wchar_t g[MAX_PATH * 2];
    join(g, lib, L"steamapps\\common\\Victoria 3");
    if (is_game_dir(g)) { wcscpy(out, g); return 1; }
    return 0;
}

static int reg_str(HKEY root, const wchar_t *key, const wchar_t *val, wchar_t *out, DWORD cap_chars)
{
    DWORD sz = cap_chars * sizeof(wchar_t);
    if (RegGetValueW(root, key, val, RRF_RT_REG_SZ, NULL, out, &sz)) return 0;
    for (wchar_t *s = out; *s; s++) if (*s == L'/') *s = L'\\';
    return out[0] != 0;
}

/* Steam: SteamPath (HKCU) / InstallPath (HKLM), then every "path" in steamapps/libraryfolders.vdf
 * (Victoria 3 = appid 529340, folder steamapps/common/Victoria 3). */
static int find_game(wchar_t *out)
{
    wchar_t steam[MAX_PATH * 2];
    int have = reg_str(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", steam, MAX_PATH * 2) ||
               reg_str(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath", steam, MAX_PATH * 2) ||
               reg_str(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", steam, MAX_PATH * 2);
    if (have) {
        if (try_library(steam, out)) return 1;
        wchar_t vdf[MAX_PATH * 2];
        join(vdf, steam, L"steamapps\\libraryfolders.vdf");
        uint8_t *b; size_t n;
        if (read_all(vdf, &b, &n)) {
            /* lines like:  "path"		"D:\\SteamLibrary"   (backslashes escaped) */
            char *s = (char *)b;
            char *hit = NULL;
            while ((s = strstr(s, "\"path\"")) != NULL) {
                s += 6;
                char *q = strchr(s, '"');
                if (!q) break;
                char *e = strchr(q + 1, '"');
                if (!e) break;
                char path[1024]; size_t k = 0;
                for (char *c = q + 1; c < e && k < sizeof path - 1; c++) {
                    if (c[0] == '\\' && c[1] == '\\') c++;
                    path[k++] = *c;
                }
                path[k] = 0;
                wchar_t wp[MAX_PATH * 2];
                utf8_to_w(path, wp, MAX_PATH * 2);
                if (try_library(wp, out)) { hit = s; break; }
                s = e + 1;
            }
            free(b);
            if (hit) return 1;
        }
    }
    /* last resort: common default spots on every fixed drive */
    for (wchar_t d = L'C'; d <= L'Z'; d++) {
        wchar_t root[8] = {d, L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        const wchar_t *libs[] = {L"SteamLibrary", L"Steam", L"Program Files (x86)\\Steam", L"Games\\Steam",
                                 L"Games\\SteamLibrary"};
        for (int i = 0; i < 5; i++) {
            wchar_t lib[MAX_PATH]; _snwprintf(lib, MAX_PATH, L"%c:\\%s", d, libs[i]);
            if (try_library(lib, out)) return 1;
        }
    }
    return 0;
}

/* "<TimeDateStamp>-<SizeOfImage>" exactly like src/core.c read_module_layout (game_build_id) */
static int pe_build_id(const wchar_t *exe, char out[40])
{
    strcpy(out, "unknown");
    HANDLE f = CreateFileW(exe, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    uint8_t h[4096]; DWORD r = 0;
    ReadFile(f, h, sizeof h, &r, NULL);
    CloseHandle(f);
    if (r < 0x200 || h[0] != 'M' || h[1] != 'Z') return 0;
    uint32_t lfanew; memcpy(&lfanew, h + 0x3c, 4);
    if (lfanew + 0x58 > r || memcmp(h + lfanew, "PE\0\0", 4)) return 0;
    uint32_t ts, soi;
    memcpy(&ts, h + lfanew + 8, 4);
    memcpy(&soi, h + lfanew + 24 + 56, 4);
    snprintf(out, 40, "%08lx-%lx", (unsigned long)ts, (unsigned long)soi);
    return 1;
}

static void read_branch(const wchar_t *game, char out[128])
{
    wchar_t p[MAX_PATH * 2]; uint8_t *b; size_t n;
    strcpy(out, "unknown");
    join(p, game, L"caligula_branch.txt");
    if (!read_all(p, &b, &n)) return;
    snprintf(out, 128, "%s", (char *)b);
    free(b);
    trim(out);
}

/* 1 = supported, 0 = not; sets *branch_ok when the branch name matches a supported one */
static int build_supported(const char *id, const char *branch, int *branch_ok)
{
    *branch_ok = 0;
    for (int i = 0; i < g_nbuilds; i++) if (!_stricmp(branch, g_builds[i].branch)) *branch_ok = 1;
    for (int i = 0; i < g_nbuilds; i++) if (!_stricmp(id, g_builds[i].id)) return 1;
    return 0;
}

/* Is <game>/binaries/victoria3.exe running? Compares the full image path of every process named
 * victoria3.exe, so another Victoria 3 install that is running does not block this one. A process
 * whose path cannot be read counts as running (safe side). */
static int game_running(const wchar_t *game)
{
    wchar_t want[MAX_PATH * 2], wfull[MAX_PATH * 2];
    join(want, game, L"binaries\\victoria3.exe");
    if (!GetFullPathNameW(want, MAX_PATH * 2, wfull, NULL)) wcscpy(wfull, want);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = {sizeof pe};
    int running = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !running; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"victoria3.exe")) continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!p) { running = 1; break; }
        wchar_t img[MAX_PATH * 2]; DWORD n = MAX_PATH * 2;
        if (!QueryFullProcessImageNameW(p, 0, img, &n)) running = 1;
        else if (!_wcsicmp(img, wfull)) running = 1;
        CloseHandle(p);
    }
    CloseHandle(snap);
    return running;
}

/* a version.dll built from this repo: src/core.c checks the env var V3X_DISABLE and resolves the
 * plugin entry point v3x_plugin_load by name, so both strings are in every V3X loader */
static int is_v3x_dll(const wchar_t *path)
{
    uint8_t *b; size_t n; int hit = 0;
    if (!read_all(path, &b, &n)) return 0;
    static const char m1[] = "V3X_DISABLE", m2[] = "v3x_plugin_load";
    int h1 = 0, h2 = 0;
    for (size_t i = 0; i + sizeof m2 <= n && !(h1 && h2); i++) {
        if (b[i] == 'V' && !memcmp(b + i, m1, sizeof m1)) h1 = 1;
        if (b[i] == 'v' && !memcmp(b + i, m2, sizeof m2)) h2 = 1;
    }
    hit = h1 && h2;
    free(b);
    return hit;
}

/* ------------------------------------------------------------------ manifest */

typedef struct { char kind[8]; char sha[65]; char rel[260]; } MFile;
typedef struct {
    int valid;
    char version[64];
    char backup_rel[260], backup_sha[65];
    MFile *f; int n, cap;
} Manifest;

static void mf_add(Manifest *m, const char *kind, const char *sha, const char *rel)
{
    for (int i = 0; i < m->n; i++)
        if (!_stricmp(m->f[i].rel, rel)) { snprintf(m->f[i].kind, 8, "%s", kind); snprintf(m->f[i].sha, 65, "%s", sha); return; }
    if (m->n == m->cap) { m->cap = m->cap ? m->cap * 2 : 32; m->f = realloc(m->f, (size_t)m->cap * sizeof(MFile)); }
    MFile *e = &m->f[m->n++];
    snprintf(e->kind, 8, "%s", kind); snprintf(e->sha, 65, "%s", sha); snprintf(e->rel, 260, "%s", rel);
}

static MFile *mf_find(Manifest *m, const char *rel)
{
    for (int i = 0; i < m->n; i++) if (!_stricmp(m->f[i].rel, rel)) return &m->f[i];
    return NULL;
}

static void mf_load(const wchar_t *bin, Manifest *m)
{
    memset(m, 0, sizeof *m);
    wchar_t p[MAX_PATH * 2]; uint8_t *b; size_t n;
    join(p, bin, MANIFEST_REL);
    if (!read_all(p, &b, &n)) return;
    for (char *line = strtok((char *)b, "\n"); line; line = strtok(NULL, "\n")) {
        trim(line);
        char a[16], x[260], y[260], z[260];
        int k = sscanf(line, "%15s %259s %259s %259s", a, x, y, z);
        if (k >= 2 && !strcmp(a, "v3x_manifest")) m->valid = 1;
        else if (k >= 2 && !strcmp(a, "version")) snprintf(m->version, 64, "%s", x);
        else if (k >= 3 && !strcmp(a, "backup")) { snprintf(m->backup_sha, 65, "%s", x); snprintf(m->backup_rel, 260, "%s", y); }
        else if (k >= 4 && !strcmp(a, "file")) mf_add(m, x, y, z);
    }
    free(b);
}

static int mf_save(const wchar_t *bin, Manifest *m, const char *build, const char *branch)
{
    size_t cap = 4096 + (size_t)m->n * 360, n = 0;
    char *s = malloc(cap);
    SYSTEMTIME t; GetLocalTime(&t);
    n += (size_t)snprintf(s + n, cap - n,
        "# V3X install manifest, written by V3X-Setup.exe. Repair and uninstall use it: do not edit.\n"
        "# file <kind> <sha256> <path relative to binaries/>   kind: bin = program file, config = settings file we created\n"
        "v3x_manifest 1\nversion %s\ninstalled %04d-%02d-%02d %02d:%02d\ngame_build %s %s\n",
        m->version, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, build, branch);
    if (m->backup_rel[0]) n += (size_t)snprintf(s + n, cap - n, "backup %s %s\n", m->backup_sha, m->backup_rel);
    for (int i = 0; i < m->n; i++)
        n += (size_t)snprintf(s + n, cap - n, "file %s %s %s\n", m->f[i].kind, m->f[i].sha, m->f[i].rel);
    wchar_t p[MAX_PATH * 2];
    join(p, bin, MANIFEST_REL);
    int ok = write_all(p, s, n);
    free(s);
    return ok;
}

/* ------------------------------------------------------------------ v3x.ini merge */

/* 1 if text has a line "<name> = ..." inside a [plugins] section (same rules as core's ini parser) */
static int ini_has_plugin_key(const char *txt, const char *name)
{
    int in = 0;
    size_t nl = strlen(name);
    for (const char *p = txt; *p;) {
        const char *e = p; while (*e && *e != '\n') e++;
        const char *a = p, *b = e;
        if (b > a && b[-1] == '\r') b--;
        while (a < b && (*a == ' ' || *a == '\t')) a++;
        if (a < b && *a == '[') {
            const char *c = a + 1; while (c < b && (*c == ' ' || *c == '\t')) c++;
            in = (size_t)(b - c) >= 8 && !_strnicmp(c, "plugins", 7) && (c[7] == ']' || c[7] == ' ' || c[7] == '\t');
        } else if (in && a < b && *a != '#' && *a != ';') {
            const char *eq = memchr(a, '=', (size_t)(b - a));
            if (eq) {
                const char *ke = eq; while (ke > a && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
                if ((size_t)(ke - a) == nl && !_strnicmp(a, name, nl)) return 1;
            }
        }
        p = *e ? e + 1 : e;
    }
    return 0;
}

/* existing v3x.ini: add "<plugin>=0" for every shipped-but-disabled plugin that has no line yet,
 * directly under the first [plugins] header (or a new section at the end). Existing lines are
 * never changed. Returns the number of lines added (-1 = write error). */
static int merge_v3x_ini(const wchar_t *path)
{
    uint8_t *b; size_t n;
    if (!read_all(path, &b, &n)) return -1;
    char add[4096]; size_t an = 0; int count = 0;
    for (int i = 0; i < g_nprof; i++) {
        if (g_prof[i].enabled || ini_has_plugin_key((char *)b, g_prof[i].name)) continue;
        an += (size_t)snprintf(add + an, sizeof add - an, "%s=0\r\n", g_prof[i].name);
        count++;
    }
    if (!count) { free(b); return 0; }
    /* find the first [plugins] header line */
    char *hdr_end = NULL;
    for (char *p = (char *)b; *p;) {
        char *e = p; while (*e && *e != '\n') e++;
        char *a = p; while (*a == ' ' || *a == '\t') a++;
        if (*a == '[') {
            char *c = a + 1; while (*c == ' ' || *c == '\t') c++;
            if (!_strnicmp(c, "plugins", 7) && (c[7] == ']' || c[7] == ' ' || c[7] == '\t')) { hdr_end = *e ? e + 1 : e; break; }
        }
        p = *e ? e + 1 : e;
    }
    size_t cap = n + an + 64, k = 0;
    char *out = malloc(cap);
    if (hdr_end) {
        size_t pre = (size_t)(hdr_end - (char *)b);
        memcpy(out, b, pre); k = pre;
        if (pre && out[pre - 1] != '\n') { out[k++] = '\r'; out[k++] = '\n'; }
        memcpy(out + k, add, an); k += an;
        memcpy(out + k, hdr_end, n - pre); k += n - pre;
    } else {
        memcpy(out, b, n); k = n;
        if (n && out[n - 1] != '\n') { out[k++] = '\r'; out[k++] = '\n'; }
        k += (size_t)snprintf(out + k, cap - k, "\r\n[plugins]\r\n");
        memcpy(out + k, add, an); k += an;
    }
    int ok = write_all(path, out, k);
    free(out); free(b);
    return ok ? count : -1;
}

/* ------------------------------------------------------------------ install / uninstall */

typedef struct {
    wchar_t game[MAX_PATH * 2], bin[MAX_PATH * 2];
    char build[40], branch[128];
    int supported, branch_ok, installed;
    Manifest mf;
} State;

static void state_read(State *s, const wchar_t *game)
{
    memset(s, 0, sizeof *s);
    wcscpy(s->game, game);
    join(s->bin, game, L"binaries");
    wchar_t exe[MAX_PATH * 2];
    join(exe, game, L"binaries\\victoria3.exe");
    pe_build_id(exe, s->build);
    read_branch(game, s->branch);
    s->supported = build_supported(s->build, s->branch, &s->branch_ok);
    mf_load(s->bin, &s->mf);
    s->installed = s->mf.valid;
}

static const char *supported_list(void)
{
    static char buf[512]; size_t n = 0; buf[0] = 0;
    for (int i = 0; i < g_nbuilds; i++)
        n += (size_t)snprintf(buf + n, sizeof buf - n, "%s%s (%s)", i ? ", " : "", g_builds[i].branch, g_builds[i].id);
    return buf;
}

static void append_log(const wchar_t *bin)
{
    wchar_t p[MAX_PATH * 2];
    join(p, bin, L"v3x\\install.log");
    HANDLE f = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE || !g_out) { if (f != INVALID_HANDLE_VALUE) CloseHandle(f); return; }
    SYSTEMTIME t; GetLocalTime(&t);
    char hdr[128];
    int hn = snprintf(hdr, sizeof hdr, "---- %04d-%02d-%02d %02d:%02d:%02d V3X-Setup %s\r\n", t.wYear, t.wMonth, t.wDay,
                      t.wHour, t.wMinute, t.wSecond, g_version);
    DWORD w; WriteFile(f, hdr, (DWORD)hn, &w, NULL);
    int n = WideCharToMultiByte(CP_UTF8, 0, g_out, (int)g_out_n, NULL, 0, NULL, NULL);
    char *u = malloc((size_t)n + 1);
    if (u) { WideCharToMultiByte(CP_UTF8, 0, g_out, (int)g_out_n, u, n, NULL, NULL); WriteFile(f, u, (DWORD)n, &w, NULL); free(u); }
    CloseHandle(f);
}

static int do_install(State *s, int force)
{
    if (game_running(s->game)) {
        say(L"Victoria 3 is running from this folder. Close the game, then run setup again.");
        return RC_RUNNING;
    }
    if (!s->supported) {
        say(L"Game build %hs (%hs) is not supported. This V3X was made for: %hs.", s->build, s->branch, supported_list());
        if (!force) { say(L"Nothing was changed."); return RC_UNSUPPORTED; }
        say(L"Installing anyway (--force). Plugins switch themselves off where the engine code differs.");
    }
    Manifest *old = &s->mf;
    Manifest nm; memset(&nm, 0, sizeof nm);
    snprintf(nm.version, 64, "%s", g_version);
    snprintf(nm.backup_rel, 260, "%s", old->backup_rel);
    snprintf(nm.backup_sha, 65, "%s", old->backup_sha);
    int written = 0, same = 0, cfg_new = 0, cfg_kept = 0, errors = 0;
    wchar_t p[MAX_PATH * 2];

    /* 1. a foreign version.dll (another mod / tool) is backed up, never lost */
    join(p, s->bin, L"version.dll");
    if (file_exists(p)) {
        char h[65] = "";
        hash_file(p, h);
        MFile *of = mf_find(old, "version.dll");
        int ours = (of && !strcmp(of->sha, h)) || is_v3x_dll(p);
        for (int i = 0; i < g_nent && !ours; i++)
            if (!strcmp(g_ent[i].kind, "bin") && !strcmp(g_ent[i].rel, "version.dll") && !strcmp(g_ent[i].sha, h)) ours = 1;
        if (!ours) {
            if (nm.backup_rel[0] && !strcmp(nm.backup_sha, h)) {
                say(L"version.dll: same file as the existing backup %hs", nm.backup_rel);
            } else {
                char rel[260]; wchar_t bp[MAX_PATH * 2];
                for (int i = 0; i < 100; i++) {
                    if (i) snprintf(rel, sizeof rel, "v3x/backup/version.%d.dll", i);
                    else snprintf(rel, sizeof rel, "v3x/backup/version.dll");
                    rel_to_path(s->bin, rel, bp);
                    if (!file_exists(bp)) break;
                }
                mkdirs_for(bp);
                if (!CopyFileW(p, bp, TRUE)) {
                    say(L"ERROR: could not back up the existing version.dll (error %lu). Nothing was changed.", GetLastError());
                    return RC_ERR;
                }
                if (nm.backup_rel[0]) say(L"note: an older backup %hs is kept as well", nm.backup_rel);
                snprintf(nm.backup_rel, 260, "%s", rel);
                snprintf(nm.backup_sha, 65, "%s", h);
                say(L"version.dll: existing file is not V3X, backed up to binaries\\%hs (restored on uninstall)", rel);
            }
        }
    }

    /* 2. program files */
    for (int i = 0; i < g_nent; i++) {
        Entry *e = &g_ent[i];
        if (strcmp(e->kind, "bin")) continue;
        rel_to_path(s->bin, e->rel, p);
        char h[65] = "";
        if (file_exists(p) && hash_file(p, h) && !strcmp(h, e->sha)) { same++; }
        else {
            int existed = file_exists(p);
            if (!write_all(p, e->data, (size_t)e->size)) {
                say(L"ERROR: could not write binaries\\%hs (error %lu; is it in use?)", e->rel, GetLastError());
                errors++; continue;
            }
            say(L"%s binaries\\%hs", existed ? L"updated  " : L"installed", e->rel);
            written++;
        }
        mf_add(&nm, "bin", e->sha, e->rel);
    }

    /* 3. settings: only created when missing */
    for (int i = 0; i < g_nent; i++) {
        Entry *e = &g_ent[i];
        if (strcmp(e->kind, "ini") && strcmp(e->kind, "v3xini")) continue;
        rel_to_path(s->bin, e->rel, p);
        MFile *of = mf_find(old, e->rel);
        if (!file_exists(p)) {
            if (!write_all(p, e->data, (size_t)e->size)) { say(L"ERROR: could not write binaries\\%hs", e->rel); errors++; continue; }
            say(L"created   binaries\\%hs", e->rel);
            mf_add(&nm, "config", e->sha, e->rel);
            cfg_new++;
            continue;
        }
        char before[65] = ""; hash_file(p, before);
        cfg_kept++;
        if (!strcmp(e->kind, "v3xini")) {
            int added = merge_v3x_ini(p);
            if (added > 0) say(L"kept      binaries\\%hs (your settings), added %d line(s) for plugins that ship disabled", e->rel, added);
            else if (added < 0) { say(L"ERROR: could not update binaries\\%hs", e->rel); errors++; }
            /* still ours if it was ours and unchanged by the user before the merge */
            if (of && !strcmp(of->sha, before)) { char now[65] = ""; hash_file(p, now); mf_add(&nm, "config", now, e->rel); }
            else if (of) mf_add(&nm, "config", of->sha, e->rel);
        } else if (of) {
            mf_add(&nm, "config", of->sha, e->rel); /* uninstall deletes it only if still unchanged */
        }
    }
    if (cfg_kept) say(L"kept %d existing settings file(s) unchanged", cfg_kept);

    /* 4. program files of an older V3X that this one no longer ships */
    for (int i = 0; i < old->n; i++) {
        MFile *of = &old->f[i];
        if (strcmp(of->kind, "bin") || mf_find(&nm, of->rel)) continue;
        rel_to_path(s->bin, of->rel, p);
        char h[65] = "";
        if (file_exists(p) && hash_file(p, h) && !strcmp(h, of->sha)) {
            if (DeleteFileW(p)) say(L"removed   binaries\\%hs (no longer part of V3X)", of->rel);
            else { say(L"could not remove binaries\\%hs, still listed", of->rel); mf_add(&nm, "bin", of->sha, of->rel); }
        }
    }

    if (!mf_save(s->bin, &nm, s->build, s->branch)) { say(L"ERROR: could not write binaries\\v3x\\install_manifest.txt"); errors++; }
    say(L"%d file(s) written, %d already up to date, %d settings file(s) created.", written, same, cfg_new);
    if (errors) { say(L"%d error(s). Close the game and run setup again (Repair).", errors); append_log(s->bin); return RC_ERR; }
    say(L"V3X %hs is installed in %s", g_version, s->bin);
    append_log(s->bin);
    return RC_OK;
}

static int delete_tree(const wchar_t *dir)
{
    wchar_t pat[MAX_PATH * 2]; WIN32_FIND_DATAW fd;
    join(pat, dir, L"*");
    HANDLE h = FindFirstFileW(pat, &fd);
    int fails = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wchar_t p[MAX_PATH * 2]; join(p, dir, fd.cFileName);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                fails += delete_tree(p);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (!RemoveDirectoryW(p)) fails++; }
            else { SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL); if (!DeleteFileW(p)) fails++; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!RemoveDirectoryW(dir)) fails++;
    return fails;
}

static int do_uninstall(State *s, int purge)
{
    if (game_running(s->game)) {
        say(L"Victoria 3 is running from this folder. Close the game, then run setup again.");
        return RC_RUNNING;
    }
    if (!s->mf.valid) {
        say(L"V3X was not installed by this setup here (no binaries\\v3x\\install_manifest.txt). Nothing was changed.");
        say(L"Manual removal: delete binaries\\version.dll and the binaries\\v3x folder.");
        return RC_NOTINST;
    }
    wchar_t p[MAX_PATH * 2];
    int removed = 0, kept = 0, errors = 0;
    for (int i = 0; i < s->mf.n; i++) {
        MFile *f = &s->mf.f[i];
        rel_to_path(s->bin, f->rel, p);
        if (!file_exists(p)) continue;
        char h[65] = ""; hash_file(p, h);
        int match = !strcmp(h, f->sha);
        if (!match && !strcmp(f->kind, "bin") && !_stricmp(f->rel, "version.dll") && is_v3x_dll(p)) match = 1;
        if (!match && !purge) {
            say(L"kept      binaries\\%hs (%s)", f->rel, strcmp(f->kind, "config") ? L"changed since install, not V3X's file any more" : L"you changed it");
            kept++; continue;
        }
        if (DeleteFileW(p)) { removed++; }
        else { say(L"ERROR: could not delete binaries\\%hs (error %lu)", f->rel, GetLastError()); errors++; }
    }
    say(L"removed %d V3X file(s)", removed);
    if (s->mf.backup_rel[0]) {
        wchar_t bp[MAX_PATH * 2], vp[MAX_PATH * 2];
        rel_to_path(s->bin, s->mf.backup_rel, bp);
        join(vp, s->bin, L"version.dll");
        if (!file_exists(bp)) say(L"note: backup binaries\\%hs is missing, nothing to restore", s->mf.backup_rel);
        else if (file_exists(vp)) say(L"note: binaries\\version.dll exists (not V3X's), the backup stays in binaries\\%hs", s->mf.backup_rel);
        else if (MoveFileExW(bp, vp, MOVEFILE_WRITE_THROUGH)) say(L"restored  binaries\\version.dll (the file that was there before V3X)");
        else { say(L"ERROR: could not restore binaries\\version.dll from binaries\\%hs", s->mf.backup_rel); errors++; }
    }
    if (errors) { say(L"%d error(s); the manifest is kept so you can retry.", errors); return RC_ERR; }
    join(p, s->bin, MANIFEST_REL); DeleteFileW(p);
    join(p, s->bin, BACKUP_DIR_REL); RemoveDirectoryW(p);
    join(p, s->bin, L"v3x\\plugins"); RemoveDirectoryW(p);
    join(p, s->bin, L"v3x");
    if (purge) {
        if (dir_exists(p) && delete_tree(p)) { say(L"ERROR: some files in binaries\\v3x could not be deleted"); return RC_ERR; }
        say(L"deleted binaries\\v3x (logs, reports and settings)");
    } else if (RemoveDirectoryW(p)) {
        say(L"binaries\\v3x was empty and is removed");
    } else if (dir_exists(p)) {
        say(L"kept binaries\\v3x: your logs, reports and changed settings (delete the folder if you do not need them)");
    }
    say(L"V3X is uninstalled. %s", kept ? L"Some files were kept, see above." : L"The game is vanilla again.");
    return RC_OK;
}

static void do_status(State *s)
{
    say(L"game folder: %s", s->game);
    say(L"game build:  %hs (%hs) - %s", s->build, s->branch, s->supported ? L"supported" : L"NOT supported");
    say(L"running:     %s", game_running(s->game) ? L"yes" : L"no");
    if (s->installed) say(L"installed:   V3X %hs, %d file(s) in the manifest%s", s->mf.version, s->mf.n,
                          s->mf.backup_rel[0] ? L", with a backup of a foreign version.dll" : L"");
    else say(L"installed:   no (no install manifest)");
    say(L"this setup:  V3X %hs, supports %hs", g_version, supported_list());
}

/* ------------------------------------------------------------------ GUI */

static const wchar_t *TITLE = L"V3X Setup";
static wchar_t g_title_ver[128];

static int pick_folder(HWND owner, wchar_t *out)
{
    for (;;) {
        BROWSEINFOW bi = {0};
        bi.hwndOwner = owner;
        bi.lpszTitle = L"Select your \"Victoria 3\" game folder (the one that contains the \"binaries\" folder).\n"
                       L"Steam: right-click Victoria 3 > Manage > Browse local files.";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_NONEWFOLDERBUTTON;
        PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi);
        if (!id) return 0;
        wchar_t p[MAX_PATH * 2];
        BOOL ok = SHGetPathFromIDListW(id, p);
        CoTaskMemFree(id);
        if (ok && normalize_game_dir(p, out)) return 1;
        MessageBoxW(owner, L"That folder does not contain binaries\\victoria3.exe. Please choose the \"Victoria 3\" folder.",
                    TITLE, MB_OK | MB_ICONWARNING);
    }
}

static int task(HWND owner, PCWSTR icon, const wchar_t *main, const wchar_t *content, const TASKDIALOG_BUTTON *btns,
                int nb, int defbtn, const wchar_t *verify, BOOL *verified, const wchar_t *expanded, const wchar_t *footer,
                DWORD common)
{
    TASKDIALOGCONFIG c = {sizeof c};
    c.hwndParent = owner;
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT | (nb ? TDF_USE_COMMAND_LINKS : 0) |
                (expanded ? TDF_EXPAND_FOOTER_AREA : 0);
    c.pszWindowTitle = g_title_ver;
    c.pszMainIcon = icon;
    c.pszMainInstruction = main;
    c.pszContent = content;
    c.pButtons = btns; c.cButtons = (UINT)nb;
    c.nDefaultButton = defbtn;
    c.dwCommonButtons = common;
    c.pszVerificationText = verify;
    c.pszExpandedInformation = expanded;
    c.pszExpandedControlText = L"Hide details"; c.pszCollapsedControlText = L"Show details";
    c.pszFooter = footer; c.pszFooterIcon = footer ? TD_INFORMATION_ICON : NULL;
    int btn = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&c, &btn, NULL, verified))) {
        /* very old comctl32: plain message box */
        wchar_t msg[4096]; _snwprintf(msg, 4096, L"%s\n\n%s", main, content ? content : L"");
        btn = MessageBoxW(owner, msg, g_title_ver, MB_OKCANCEL) == IDOK ? (nb ? btns[0].nButtonID : IDOK) : IDCANCEL;
    }
    return btn;
}

static const wchar_t *SP_NOTE =
    L"V3X is for single player only. Do not use it in multiplayer: uninstall it first, or rename "
    L"binaries\\version.dll (for example to version.dll.off) to start the game without V3X.";

static int gui(const wchar_t *game_arg)
{
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    _snwprintf(g_title_ver, 128, L"%s %hs", TITLE, g_version);
    wchar_t game[MAX_PATH * 2] = L"";
    if (game_arg && !normalize_game_dir(game_arg, game)) game[0] = 0;
    if (!game[0] && !find_game(game)) {
        TASKDIALOG_BUTTON b[] = {{100, L"Choose the game folder...\nUsually ...\\steamapps\\common\\Victoria 3"}};
        if (task(NULL, TD_INFORMATION_ICON, L"Victoria 3 was not found automatically",
                 L"Setup looked in your Steam libraries. Please show it where Victoria 3 is installed.", b, 1, 100,
                 NULL, NULL, NULL, NULL, TDCBF_CANCEL_BUTTON) != 100 || !pick_folder(NULL, game))
            return RC_NOGAME;
    }
    for (;;) {
        State s; state_read(&s, game);
        wchar_t content[4096];
        _snwprintf(content, 4096,
                   L"Game folder:\n%s\n\nGame version: %hs  (%s)\nV3X: %s%hs",
                   s.game, s.branch, s.supported ? L"supported" : (s.branch_ok ? L"different build: not tested" : L"NOT supported"),
                   s.installed ? L"installed, version " : L"not installed", s.installed ? s.mf.version : "");
        int en = 0, dis = 0;
        for (int i = 0; i < g_nprof; i++) { if (g_prof[i].enabled) en++; else dis++; }
        wchar_t inst[256], rep[256];
        _snwprintf(inst, 256, L"Install V3X\nAdds version.dll and %d plugins (%d on, %d off). No game file is changed.", en + dis, en, dis);
        _snwprintf(rep, 256, L"Repair / update V3X\nRe-copies missing or changed V3X files. Your settings stay as they are.");
        TASKDIALOG_BUTTON b[4]; int nb = 0;
        b[nb].nButtonID = 100; b[nb++].pszButtonText = s.installed ? rep : inst;
        if (s.installed) { b[nb].nButtonID = 102; b[nb++].pszButtonText = L"Uninstall V3X\nRemoves V3X's files and restores what was there before."; }
        b[nb].nButtonID = 103; b[nb++].pszButtonText = L"Choose another game folder...";
        int btn = task(NULL, s.supported ? TD_INFORMATION_ICON : TD_WARNING_ICON,
                       s.installed ? L"V3X is installed" : L"Install V3X for Victoria 3", content, b, nb, 100, NULL, NULL,
                       NULL, SP_NOTE, TDCBF_CLOSE_BUTTON);
        if (btn == 103) { wchar_t g2[MAX_PATH * 2]; if (pick_folder(NULL, g2)) wcscpy(game, g2); continue; }
        if (btn != 100 && btn != 102) return RC_OK;
        if (game_running(game)) {
            task(NULL, TD_WARNING_ICON, L"Victoria 3 is running",
                 L"Close the game first (the Paradox launcher may stay open), then try again.", NULL, 0, 0,
                 NULL, NULL, NULL, NULL, TDCBF_OK_BUTTON);
            continue;
        }
        int rc, force = 0, purge = 0;
        if (btn == 100 && !s.supported) {
            wchar_t w[1024];
            _snwprintf(w, 1024, L"Your game is %hs (build %hs). This V3X was made and tested for %hs.\n\n"
                       L"Plugins check the engine code and switch themselves off where it changed, but this "
                       L"combination was never tested. The usual fix is to wait for a V3X update.",
                       s.branch, s.build, supported_list());
            TASKDIALOG_BUTTON fb[] = {{200, L"Install anyway"}};
            if (task(NULL, TD_WARNING_ICON, L"This game version is not supported", w, fb, 1, 0, NULL, NULL, NULL,
                     NULL, TDCBF_CANCEL_BUTTON) != 200) continue;
            force = 1;
        }
        if (btn == 102) {
            BOOL v = FALSE;
            TASKDIALOG_BUTTON ub[] = {{201, L"Uninstall"}};
            if (task(NULL, TD_WARNING_ICON, L"Uninstall V3X?",
                     L"Removes version.dll and the V3X plugins and restores a version.dll that was there before. "
                     L"Your logs and changed settings in binaries\\v3x are kept unless you tick the box.",
                     ub, 1, 0, L"Also delete logs, reports and settings (the whole binaries\\v3x folder)", &v, NULL,
                     NULL, TDCBF_CANCEL_BUTTON) != 201) continue;
            purge = v;
        }
        g_out_n = 0; if (g_out) g_out[0] = 0;
        rc = btn == 100 ? do_install(&s, force) : do_uninstall(&s, purge);
        const wchar_t *head;
        wchar_t body[1024];
        if (rc == RC_OK && btn == 100) {
            head = L"V3X is installed";
            _snwprintf(body, 1024, L"Start Victoria 3 as usual (Steam or the Paradox launcher). A small \"V3X console\" "
                       L"window opens next to the game and lists what loaded.\n\nTurn plugins on or off: main menu > V3X "
                       L"button, or binaries\\v3x\\v3x.ini. Run this setup again to repair or uninstall.");
        } else if (rc == RC_OK) {
            head = L"V3X is uninstalled";
            _snwprintf(body, 1024, L"The game starts without V3X now.");
        } else {
            head = L"Setup did not finish";
            _snwprintf(body, 1024, L"%s", g_out ? g_out : L"");
        }
        task(NULL, rc == RC_OK ? TD_INFORMATION_ICON : TD_ERROR_ICON, head, body, NULL, 0, 0, NULL, NULL,
             rc == RC_OK ? g_out : NULL, rc == RC_OK && btn == 100 ? SP_NOTE : NULL, TDCBF_OK_BUTTON);
        return rc;
    }
}

/* ------------------------------------------------------------------ main */

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)hi; (void)prev; (void)cmdline; (void)show;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *game_arg = NULL;
    int uninstall = 0, purge = 0, force = 0, status = 0;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--silent")) g_silent = 1;
        else if (!wcscmp(argv[i], L"--uninstall")) uninstall = 1;
        else if (!wcscmp(argv[i], L"--repair") || !wcscmp(argv[i], L"--install")) uninstall = 0;
        else if (!wcscmp(argv[i], L"--purge")) purge = 1;
        else if (!wcscmp(argv[i], L"--force")) force = 1;
        else if (!wcscmp(argv[i], L"--status")) { status = 1; g_silent = 1; }
        else if (!wcscmp(argv[i], L"--game-dir") && i + 1 < argc) game_arg = argv[++i];
        else {
            g_silent = 1;
            say(L"usage: V3X-Setup.exe [--silent] [--game-dir <Victoria 3 folder>] [--install|--repair|--uninstall [--purge]] [--force] [--status]");
            flush_stdout();
            return RC_USAGE;
        }
    }
    if (!load_payload()) {
        if (g_silent) { say(L"ERROR: the setup file is damaged or incomplete (payload check failed). Download it again."); flush_stdout(); }
        else MessageBoxW(NULL, L"This setup file is damaged or incomplete. Please download it again.", TITLE, MB_OK | MB_ICONERROR);
        return RC_PAYLOAD;
    }
    if (!g_silent) return gui(game_arg);

    wchar_t game[MAX_PATH * 2] = L"";
    int found = game_arg ? normalize_game_dir(game_arg, game) : find_game(game);
    if (!found) {
        say(L"Victoria 3 not found%s%s. Pass --game-dir \"<...>\\steamapps\\common\\Victoria 3\".",
            game_arg ? L" in " : L"", game_arg ? game_arg : L"");
        flush_stdout();
        return RC_NOGAME;
    }
    State s; state_read(&s, game);
    int rc = RC_OK;
    if (status) do_status(&s);
    else {
        say(L"V3X-Setup %hs: %s %s (game %hs, build %hs)", g_version, uninstall ? L"uninstall from" : L"install into",
            s.game, s.branch, s.build);
        rc = uninstall ? do_uninstall(&s, purge) : do_install(&s, force);
    }
    flush_stdout();
    return rc;
}
