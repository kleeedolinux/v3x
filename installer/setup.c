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
 *   --doctor            offline checks (files, conflicts, game build, disk, page file, v3x.ini, last
 *                       crash); exit code = number of PROBLEMs (see do_doctor_checks)
 *   --wait-pid <pid>    first wait (up to 10 min) for that process to exit; still running = exit 3
 *   --update            for the in-game updater (plugins/updater): implies --silent, install/repair
 *                       keeping every ini, never --force; exit code 0 = ok, 1 = anything else. The
 *                       last line appended to binaries/v3x/install.log is
 *                       "update-result: ok version=<ver>" or "update-result: failed code=<n> <reason>"
 *                       (n = the exit codes below, reason = error|unsupported|running|timeout|nogame|payload)
 *   --verify-release <dir>  checks a downloaded release folder (installer/install.ps1 runs it before
 *                       installing): <dir>/compat.json.sig must be a valid Ed25519 signature over
 *                       <dir>/compat.json by a key of installer/release_keys.h, compat.json's
 *                       v3x_version must be this setup's version, and every asset of compat.json
 *                       present in <dir> (V3X-Setup.exe required) AND this running exe must match
 *                       its size + sha256. Changes nothing. Exit 0 = verified, 8 = not verified.
 * Language: the GUI is English or Brazilian Portuguese (GetUserDefaultUILanguage; env V3X_LANG=en|pt
 *   overrides). --silent / --status output stays English so scripts and tests can parse it.
 * Trust (the security review H2/L3/L4): every path read from install_manifest.txt or the payload
 *   index must be a plain relative path (sdk/v3x_safe.h v3x_path_rel_ok: no '..', ':', leading
 *   separator, trailing dot/space, device names); a backup line may only name
 *   v3x/backup/version.dll or v3x/backup/version.<1-99>.dll; nothing is written or deleted through a
 *   reparse point (junction / symlink) below binaries\, and binaries\v3x itself must not be one;
 *   --purge deletes files outside binaries\v3x only when their hash matches the manifest.
 * Exit codes: 0 ok, 1 error, 2 unsupported game build, 3 game running, 4 game not found,
 *   5 not installed (uninstall without manifest), 6 damaged payload, 7 usage, 8 release not verified. */
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

#include "v3x_safe.h"          /* sdk/: V3xStr bounded builder, v3x_path_rel_ok */
#define V3X_RELEASE_SIG_IMPL   /* this unit compiles Monocypher (Ed25519) */
#include "release_sig.h"
#include "setup_parse.h"       /* payload index, meta.txt, install manifest and compat.json parsers (fuzzed) */

enum { RC_OK = 0, RC_ERR = 1, RC_UNSUPPORTED = 2, RC_RUNNING = 3, RC_NOGAME = 4, RC_NOTINST = 5,
       RC_PAYLOAD = 6, RC_USAGE = 7, RC_UNVERIFIED = 8 };

#define MANIFEST_REL L"v3x\\install_manifest.txt"
#define BACKUP_DIR_REL L"v3x\\backup"

/* ------------------------------------------------------------------ output */

static wchar_t *g_out; static size_t g_out_n, g_out_cap;
static int g_silent;
static int g_defer_log;   /* --update: main() writes install.log once, with the update-result line */

/* GUI language: 1 = Portuguese (pt-BR wording), 0 = English */
static int g_pt;
#define TR(en, pt) (g_pt ? (pt) : (en))

static void pick_language(void)
{
    char v[16]; DWORD n = GetEnvironmentVariableA("V3X_LANG", v, sizeof v);
    if (n && n < sizeof v) { g_pt = !_strnicmp(v, "pt", 2); return; }
    g_pt = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE;
}

/* _snwprintf that always terminates (truncates; never writes past cap); returns the length */
static int wfmt(wchar_t *buf, size_t cap, const wchar_t *fmt, ...)
{
    if (!cap) return 0;
    va_list ap; va_start(ap, fmt);
    int n = _vsnwprintf(buf, cap - 1, fmt, ap);
    va_end(ap);
    buf[cap - 1] = 0;
    if (n < 0 || (size_t)n >= cap) n = (int)wcslen(buf);
    return n;
}

/* wide bounded append: *len stays < cap, buf always terminated */
static void wcat(wchar_t *buf, size_t cap, size_t *len, const wchar_t *fmt, ...)
{
    if (!cap || *len >= cap - 1) return;
    va_list ap; va_start(ap, fmt);
    int n = _vsnwprintf(buf + *len, cap - 1 - *len, fmt, ap);
    va_end(ap);
    buf[cap - 1] = 0;
    if (n < 0 || (size_t)n >= cap - 1 - *len) *len = wcslen(buf);
    else *len += (size_t)n;
}

/* bounded wide copy into an ARRAY destination */
#define WCOPY(dst, src) wfmt((dst), sizeof(dst) / sizeof((dst)[0]), L"%s", (src))

static void say(const wchar_t *fmt, ...)
{
    wchar_t line[2048];
    va_list ap; va_start(ap, fmt);
    int n = _vsnwprintf(line, 2045, fmt, ap);
    va_end(ap);
    line[2045] = 0;
    if (n < 0 || n > 2045) n = (int)wcslen(line);
    /* manifest paths use '/': show them Windows-style after every "binaries\" */
    for (wchar_t *q = line; (q = wcsstr(q, L"binaries\\")) != NULL;)
        for (q += 9; *q && *q != L' ' && *q != L'('; q++) if (*q == L'/') *q = L'\\';
    line[n++] = L'\n'; line[n] = 0;
    if (g_out_n + (size_t)n + 1 > g_out_cap) {
        size_t cap = (g_out_cap + (size_t)n + 1) * 2;
        wchar_t *p = (wchar_t *)v3x_realloc_array(g_out, cap, sizeof(wchar_t));   /* overflow-checked */
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

/* 1 = ok. Fail closed: on any BCrypt error out is filled with 0xff and 0 is returned, and every
 * caller treats that as "cannot verify" (never as a match). */
static int sha256(const void *data, size_t n, uint8_t out[32])
{
    BCRYPT_ALG_HANDLE alg = NULL; BCRYPT_HASH_HANDLE h = NULL;
    int ok = 0;
    memset(out, 0xff, 32);
    if (n > 0xffffffffu) return 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)) return 0;
    if (!BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0)) {
        ok = !BCryptHashData(h, (PUCHAR)data, (ULONG)n, 0) && !BCryptFinishHash(h, out, 32, 0);
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) memset(out, 0xff, 32);
    return ok;
}

static int read_all(const wchar_t *path, uint8_t **buf, size_t *len)
{
    *buf = NULL; *len = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart > (1LL << 30)) { CloseHandle(f); return 0; }
    uint8_t *b = (uint8_t *)v3x_alloc_array((size_t)sz.QuadPart + 1, 1);
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
    hex[0] = 0;
    if (!read_all(path, &b, &n)) return 0;
    int ok = sha256(b, n, h);
    free(b);
    if (!ok) return 0;
    hex32(h, hex);
    return 1;
}

/* Moves the backup bp to vp only if its SHA-256 is want_hex (audit N14): the file is opened denying
 * writers (share read + delete), hashed through that handle and renamed while the handle is held.
 * 1 restored, -2 hash differs / no recorded hash (nothing moved), 0 error. */
static int restore_backup(const wchar_t *bp, const wchar_t *vp, const char *want_hex)
{
    if (!want_hex || strlen(want_hex) != 64) return -2;
    HANDLE f = CreateFileW(bp, GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    BY_HANDLE_FILE_INFORMATION fi;
    LARGE_INTEGER sz;
    if (!GetFileInformationByHandle(f, &fi) || (fi.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        !GetFileSizeEx(f, &sz) || sz.QuadPart > (1LL << 30)) {
        CloseHandle(f);
        return 0;
    }
    uint8_t *b = (uint8_t *)v3x_alloc_array((size_t)sz.QuadPart + 1, 1);
    if (!b) { CloseHandle(f); return 0; }
    size_t got = 0;
    while (got < (size_t)sz.QuadPart) {
        DWORD r = 0;
        if (!ReadFile(f, b + got, (DWORD)min((size_t)sz.QuadPart - got, (size_t)1 << 24), &r, NULL) || !r) break;
        got += r;
    }
    uint8_t h[32];
    char hex[65];
    int ok = got == (size_t)sz.QuadPart && sha256(b, got, h);
    free(b);
    if (!ok) { CloseHandle(f); return 0; }
    hex32(h, hex);
    if (_stricmp(hex, want_hex)) { CloseHandle(f); return -2; }
    int moved = MoveFileExW(bp, vp, MOVEFILE_WRITE_THROUGH) != 0;
    CloseHandle(f);
    return moved ? 1 : 0;
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
    if (!path[0]) return 0;
    mkdirs_for(path);
    wchar_t tmp[MAX_PATH * 2];
    int tn = _snwprintf(tmp, MAX_PATH * 2 - 1, L"%s.v3xtmp", path);
    tmp[MAX_PATH * 2 - 1] = 0;
    if (tn < 0 || tn >= MAX_PATH * 2 - 1) return 0;
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

/* out = a\b (MAX_PATH * 2 wide chars). A result that does not fit is never truncated into another
 * path: out becomes "" (every file operation on "" fails) and 0 is returned. */
static int join(wchar_t *out, const wchar_t *a, const wchar_t *b)
{
    int n = _snwprintf(out, MAX_PATH * 2 - 1, L"%s\\%s", a, b);
    out[MAX_PATH * 2 - 1] = 0;
    if (n < 0 || n >= MAX_PATH * 2 - 1) { out[0] = 0; return 0; }
    return 1;
}

static void utf8_to_w(const char *s, wchar_t *out, int cap)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, cap);
    if (n <= 0) out[0] = 0;
}

static int is_reparse(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

/* binaries\<rel> for a checked rel. 0 (out = "") when rel is not a plain relative path, the
 * result does not fit, or any existing component below bin_dir (including the file) is a reparse
 * point, so nothing is ever written or deleted through a junction or symlink. */
static int rel_to_path(const wchar_t *bin_dir, const char *rel, wchar_t *out)
{
    out[0] = 0;
    if (!rel_ok(rel)) return 0;
    wchar_t w[MAX_PATH];
    utf8_to_w(rel, w, MAX_PATH);
    if (!w[0]) return 0;
    for (wchar_t *s = w; *s; s++) if (*s == L'/') *s = L'\\';
    if (!join(out, bin_dir, w)) return 0;
    size_t base = wcslen(bin_dir) + 1;
    for (size_t i = base;; i++) {
        if (out[i] == L'\\' || out[i] == 0) {
            wchar_t keep = out[i];
            out[i] = 0;
            int rp = is_reparse(out);
            out[i] = keep;
            if (rp) { out[0] = 0; return 0; }
            if (!keep) break;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ payload */

static uint8_t *g_self; static size_t g_self_n;
static Entry *g_ent; static int g_nent;
static char g_version[64] = "?";
static Build g_builds[16]; static int g_nbuilds;
static Prof g_prof[64]; static int g_nprof;

static int load_payload(void)
{
    wchar_t self[MAX_PATH * 2];
    GetModuleFileNameW(NULL, self, MAX_PATH * 2);
    if (!read_all(self, &g_self, &g_self_n) || g_self_n < 64) return 0;
    if (!sp_payload_parse(g_self, g_self_n, sha256, &g_ent, &g_nent)) return 0;   /* setup_parse.h */
    for (int i = 0; i < g_nent; i++) {
        if (strcmp(g_ent[i].kind, "meta")) continue;
        if (!sp_meta_parse(g_ent[i].data, (size_t)g_ent[i].size, g_version, sizeof g_version, g_builds, &g_nbuilds, g_prof, &g_nprof))
            return 0;
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
    if (is_game_dir(p)) { wfmt(out, MAX_PATH * 2, L"%s", p); return 1; }
    if (n > 9 && !_wcsicmp(p + n - 9, L"\\binaries")) {
        p[n - 9] = 0;
        if (is_game_dir(p)) { wfmt(out, MAX_PATH * 2, L"%s", p); return 1; }
    }
    return 0;
}

static int try_library(const wchar_t *lib, wchar_t *out)
{
    wchar_t g[MAX_PATH * 2];
    join(g, lib, L"steamapps\\common\\Victoria 3");
    if (is_game_dir(g)) { wfmt(out, MAX_PATH * 2, L"%s", g); return 1; }
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
            wchar_t lib[MAX_PATH]; wfmt(lib, MAX_PATH, L"%c:\\%s", d, libs[i]);
            if (try_library(lib, out)) return 1;
        }
    }
    return 0;
}

/* "<TimeDateStamp>-<SizeOfImage>" exactly like src/core.c read_module_layout (game_build_id) */
static int pe_build_id(const wchar_t *exe, char out[40])
{
    snprintf(out, 40, "unknown");
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
    snprintf(out, 128, "unknown");
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
    if (!GetFullPathNameW(want, MAX_PATH * 2, wfull, NULL)) WCOPY(wfull, want);
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

/* PE helpers for is_v3x_dll: every read is bounds-checked against the file size */
static int pe_u32(const uint8_t *b, size_t n, uint64_t off, uint32_t *v)
{
    if (off > n || n - off < 4) return 0;
    memcpy(v, b + off, 4);
    return 1;
}

/* file offset of an RVA through the section table; 0 = not mapped */
static int pe_rva(const uint8_t *b, size_t n, uint32_t sec_off, uint16_t nsec, uint32_t rva, uint64_t *off)
{
    for (uint32_t i = 0; i < nsec; i++) {
        uint64_t s = (uint64_t)sec_off + 40ull * i;
        uint32_t vsz, va, rsz, raw;
        if (!pe_u32(b, n, s + 8, &vsz) || !pe_u32(b, n, s + 12, &va) || !pe_u32(b, n, s + 16, &rsz) ||
            !pe_u32(b, n, s + 20, &raw))
            return 0;
        uint32_t span = vsz > rsz ? vsz : rsz;
        if (rva >= va && (uint64_t)rva < (uint64_t)va + span) {
            uint64_t o = (uint64_t)raw + (rva - va);
            if (o >= n || rva - va >= rsz) return 0;
            *off = o;
            return 1;
        }
    }
    return 0;
}

/* A V3X loader (L4): a valid x64 PE whose export table names both data exports src/core.c has had
 * since 0.5.0, v3x_loader_api and v3x_loader_semver. A string anywhere in the file is not enough. */
static int is_v3x_dll(const wchar_t *path)
{
    uint8_t *b; size_t n;
    if (!read_all(path, &b, &n)) return 0;
    int have_api = 0, have_semver = 0;
    uint32_t lfanew, sig, ndir, exp_rva, nnames, names_rva;
    uint64_t exp_off, names_off;
    if (n < 0x40 || b[0] != 'M' || b[1] != 'Z' || !pe_u32(b, n, 0x3c, &lfanew) || !pe_u32(b, n, lfanew, &sig) ||
        sig != 0x00004550u || (uint64_t)lfanew + 24 + 112 + 8 > n)
        goto out;
    {
        uint16_t machine, nsec, optsz, magic;
        memcpy(&machine, b + lfanew + 4, 2); memcpy(&nsec, b + lfanew + 6, 2);
        memcpy(&optsz, b + lfanew + 20, 2); memcpy(&magic, b + lfanew + 24, 2);
        if (machine != 0x8664 || magic != 0x20b || nsec == 0 || nsec > 96 || optsz < 112 + 8) goto out;
        if (!pe_u32(b, n, (uint64_t)lfanew + 24 + 108, &ndir) || ndir < 1) goto out;
        if (!pe_u32(b, n, (uint64_t)lfanew + 24 + 112, &exp_rva) || !exp_rva) goto out;
        uint32_t sec_off = lfanew + 24 + optsz;
        if (!pe_rva(b, n, sec_off, nsec, exp_rva, &exp_off)) goto out;
        if (!pe_u32(b, n, exp_off + 24, &nnames) || !pe_u32(b, n, exp_off + 32, &names_rva) || nnames > 65536) goto out;
        if (!nnames || !pe_rva(b, n, sec_off, nsec, names_rva, &names_off)) goto out;
        for (uint32_t i = 0; i < nnames && !(have_api && have_semver); i++) {
            uint32_t nrva; uint64_t noff;
            if (!pe_u32(b, n, names_off + 4ull * i, &nrva) || !pe_rva(b, n, sec_off, nsec, nrva, &noff)) goto out;
            size_t max = n - (size_t)noff;
            const char *s = (const char *)b + noff;
            if (max >= 15 && !memcmp(s, "v3x_loader_api", 15)) have_api = 1;
            if (max >= 18 && !memcmp(s, "v3x_loader_semver", 18)) have_semver = 1;
        }
    }
out:
    free(b);
    return have_api && have_semver;
}

/* ------------------------------------------------------------------ manifest */

static void mf_load(const wchar_t *bin, Manifest *m)
{
    memset(m, 0, sizeof *m);
    wchar_t p[MAX_PATH * 2]; uint8_t *b; size_t n;
    join(p, bin, MANIFEST_REL);
    if (!read_all(p, &b, &n)) return;
    mf_parse((char *)b, m);   /* setup_parse.h; read_all NUL-terminates */
    free(b);
}

static int mf_save(const wchar_t *bin, Manifest *m, const char *build, const char *branch)
{
    size_t cap = 4096 + (size_t)m->n * 360;
    char *s = malloc(cap);
    if (!s) return 0;
    V3xStr w; v3x_str_init(&w, s, cap);
    SYSTEMTIME t; GetLocalTime(&t);
    v3x_str_appendf(&w,
        "# V3X install manifest, written by V3X-Setup.exe. Repair and uninstall use it: do not edit.\n"
        "# file <kind> <sha256> <path relative to binaries/>   kind: bin = program file, config = settings file we created\n"
        "v3x_manifest 1\nversion %s\ninstalled %04d-%02d-%02d %02d:%02d\ngame_build %s %s\n",
        m->version, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, build, branch);
    if (m->backup_rel[0]) v3x_str_appendf(&w, "backup %s %s\n", m->backup_sha, m->backup_rel);
    for (int i = 0; i < m->n; i++)
        v3x_str_appendf(&w, "file %s %s %s\n", m->f[i].kind, m->f[i].sha, m->f[i].rel);
    wchar_t p[MAX_PATH * 2];
    int ok = !w.truncated && join(p, bin, MANIFEST_REL) && write_all(p, s, w.len);
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
static int merge_v3x_ini_locked(const wchar_t *path)
{
    uint8_t *b; size_t n;
    if (!read_all(path, &b, &n)) return -1;
    /* M1: sized for every profile name (each < 64 bytes + "=0\r\n"); a truncated list = error */
    size_t acap = (size_t)g_nprof * 72 + 16;
    char *add = malloc(acap);
    if (!add) { free(b); return -1; }
    V3xStr as; v3x_str_init(&as, add, acap);
    int count = 0;
    for (int i = 0; i < g_nprof; i++) {
        if (g_prof[i].enabled || ini_has_plugin_key((char *)b, g_prof[i].name)) continue;
        v3x_str_appendf(&as, "%s=0\r\n", g_prof[i].name);
        count++;
    }
    size_t an = as.len;
    if (as.truncated) { free(add); free(b); return -1; }
    if (!count) { free(add); free(b); return 0; }
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
    if (!out) { free(add); free(b); return -1; }
    if (hdr_end) {
        size_t pre = (size_t)(hdr_end - (char *)b);
        memcpy(out, b, pre); k = pre;
        if (pre && out[pre - 1] != '\n') { out[k++] = '\r'; out[k++] = '\n'; }
        memcpy(out + k, add, an); k += an;
        memcpy(out + k, hdr_end, n - pre); k += n - pre;
    } else {
        memcpy(out, b, n); k = n;
        if (n && out[n - 1] != '\n') { out[k++] = '\r'; out[k++] = '\n'; }
        memcpy(out + k, "\r\n[plugins]\r\n", 13); k += 13;
        memcpy(out + k, add, an); k += an;
    }
    int ok = write_all(path, out, k);
    free(out); free(add); free(b);
    return ok ? count : -1;
}

/* the other v3x.ini writers (plugins mod_menu, doctor) hold the same named mutex around their
 * read-modify-write (mem chat #4457); a timeout writes anyway, like they do */
static int merge_v3x_ini(const wchar_t *path)
{
    HANDLE mx = CreateMutexW(NULL, FALSE, L"Local\\v3x_ini");
    DWORD w = mx ? WaitForSingleObject(mx, 10000) : WAIT_FAILED;
    int r = merge_v3x_ini_locked(path);
    if (mx) { if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) ReleaseMutex(mx); CloseHandle(mx); }
    return r;
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
    WCOPY(s->game, game);
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
    /* M1: 16 builds x (64 + 40 + 5) bytes fit; V3xStr cuts (never overruns) if that ever changes */
    static char buf[2048];
    V3xStr s; v3x_str_init(&s, buf, sizeof buf);
    for (int i = 0; i < g_nbuilds; i++)
        v3x_str_appendf(&s, "%s%s (%s)", i ? ", " : "", g_builds[i].branch, g_builds[i].id);
    return buf;
}

static void append_log(const wchar_t *bin)
{
    if (g_defer_log) return;
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

/* H2: binaries\v3x (and the folders / version.dll we write) must be real folders and files, not
 * junctions or symlinks that point somewhere else. 1 = refused (said why). */
static int links_refused(State *s)
{
    static const wchar_t *const rels[] = {L"v3x", L"v3x\\plugins", L"v3x\\backup", L"v3x\\update", L"version.dll"};
    wchar_t p[MAX_PATH * 2];
    for (size_t i = 0; i < sizeof rels / sizeof *rels; i++) {
        if (!join(p, s->bin, rels[i])) { say(L"ERROR: the game folder path is too long. Nothing was changed."); return 1; }
        if (is_reparse(p)) {
            say(L"ERROR: binaries\\%s is a link (junction or symbolic link) to another place. Setup does not write "
                L"or delete through links. Replace it with a normal folder or file, then run setup again. Nothing was changed.", rels[i]);
            return 1;
        }
    }
    return 0;
}

static void say_rejected(State *s)
{
    if (s->mf.rejected)
        say(L"warning: %d line(s) of binaries\\v3x\\install_manifest.txt were ignored (not a V3X path or hash; edited by hand or by another program)",
            s->mf.rejected);
}

static int do_install(State *s, int force)
{
    if (game_running(s->game)) {
        say(L"Victoria 3 is running from this folder. Close the game, then run setup again.");
        return RC_RUNNING;
    }
    if (links_refused(s)) return RC_ERR;
    say_rejected(s);
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
                    if (!rel_to_path(s->bin, rel, bp)) break;
                    if (!file_exists(bp)) break;
                }
                if (bp[0]) mkdirs_for(bp);
                if (!bp[0] || file_exists(bp) || !CopyFileW(p, bp, TRUE)) {
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
        if (!rel_to_path(s->bin, e->rel, p)) {
            say(L"ERROR: binaries\\%hs: path refused (a link below binaries, or too long)", e->rel); errors++; continue;
        }
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
        if (!rel_to_path(s->bin, e->rel, p)) {
            say(L"ERROR: binaries\\%hs: path refused (a link below binaries, or too long)", e->rel); errors++; continue;
        }
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
        if (!rel_to_path(s->bin, of->rel, p)) continue;   /* never followed */
        char h[65] = "";
        if (file_exists(p) && hash_file(p, h) && !strcmp(h, of->sha)) {
            if (DeleteFileW(p)) say(L"removed   binaries\\%hs (no longer part of V3X)", of->rel);
            else { say(L"could not remove binaries\\%hs, still listed", of->rel); mf_add(&nm, "bin", of->sha, of->rel); }
        }
    }

    if (g_oom) { say(L"ERROR: out of memory"); errors++; }
    if (!mf_save(s->bin, &nm, s->build, s->branch)) { say(L"ERROR: could not write binaries\\v3x\\install_manifest.txt"); errors++; }
    free(nm.f);
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
    if (links_refused(s)) return RC_ERR;
    say_rejected(s);
    wchar_t p[MAX_PATH * 2];
    int removed = 0, kept = 0, errors = 0;
    for (int i = 0; i < s->mf.n; i++) {
        MFile *f = &s->mf.f[i];
        if (!rel_to_path(s->bin, f->rel, p)) {
            say(L"kept      binaries\\%hs (path refused: a link below binaries, or too long)", f->rel);
            kept++; continue;
        }
        if (!file_exists(p)) continue;
        char h[65] = "";
        int match = hash_file(p, h) && !strcmp(h, f->sha);
        if (!match && !strcmp(f->kind, "bin") && !_stricmp(f->rel, "version.dll") && is_v3x_dll(p)) match = 1;
        /* --purge removes changed files only inside binaries\v3x; outside it (version.dll) the
         * file must still be V3X's (hash match or a V3X loader) */
        int inside_v3x = !_strnicmp(f->rel, "v3x/", 4);
        if (!match && (!purge || !inside_v3x)) {
            say(L"kept      binaries\\%hs (%s)", f->rel, strcmp(f->kind, "config") ? L"changed since install, not V3X's file any more" : L"you changed it");
            kept++; continue;
        }
        if (DeleteFileW(p)) { removed++; }
        else { say(L"ERROR: could not delete binaries\\%hs (error %lu)", f->rel, GetLastError()); errors++; }
    }
    say(L"removed %d V3X file(s)", removed);
    if (s->mf.backup_rel[0]) {
        wchar_t bp[MAX_PATH * 2], vp[MAX_PATH * 2];
        int bok = rel_to_path(s->bin, s->mf.backup_rel, bp);
        join(vp, s->bin, L"version.dll");
        if (!bok) { say(L"ERROR: backup path binaries\\%hs refused (a link below binaries)", s->mf.backup_rel); errors++; }
        else if (!file_exists(bp)) say(L"note: backup binaries\\%hs is missing, nothing to restore", s->mf.backup_rel);
        else if (file_exists(vp)) say(L"note: binaries\\version.dll exists (not V3X's), the backup stays in binaries\\%hs", s->mf.backup_rel);
        else {
            /* N14: restore only the file that was backed up (backup_sha of the manifest). The backup is
             * hashed through a handle that denies writers and is held across the move, so it cannot be
             * swapped between the check and the rename. */
            int rr = restore_backup(bp, vp, s->mf.backup_sha);
            if (rr == 1) say(L"restored  binaries\\version.dll (the file that was there before V3X)");
            else if (rr == -2) {
                say(L"kept      binaries\\%hs: it changed since install (SHA-256 differs from the manifest), not restored as binaries\\version.dll",
                    s->mf.backup_rel);
                kept++;
            } else {
                say(L"ERROR: could not restore binaries\\version.dll from binaries\\%hs", s->mf.backup_rel);
                errors++;
            }
        }
    }
    if (errors) { say(L"%d error(s); the manifest is kept so you can retry.", errors); return RC_ERR; }
    join(p, s->bin, MANIFEST_REL); DeleteFileW(p);
    join(p, s->bin, BACKUP_DIR_REL); RemoveDirectoryW(p);
    join(p, s->bin, L"v3x\\plugins"); RemoveDirectoryW(p);
    join(p, s->bin, L"v3x");
    if (purge) {
        if (is_reparse(p)) { say(L"ERROR: binaries\\v3x is a link; not deleted"); return RC_ERR; }
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


/* ------------------------------------------------------------------ doctor (offline checks)
 * --doctor: read-only checks of one game folder; ids shared with the in-game plugins/doctor
 * (mem chat #4423/#4426). Each check: level OK / WARN / PROBLEM, one plain sentence and one
 * "what to do" sentence (English + Portuguese; --doctor stdout is English), plus a machine line
 *   doctor: <id> <name> <OK|WARN|PROBLEM> <detail>
 * Exit code = number of PROBLEMs. Also writes binaries/v3x/doctor_offline.txt when binaries/v3x
 * exists (the only file it writes). */

enum { DR_OK = 0, DR_WARN = 1, DR_PROBLEM = 2 };
typedef struct {
    const char *id, *name; int level;
    wchar_t msg_en[400], msg_pt[400];
    const wchar_t *todo_en, *todo_pt;
    char detail[400];
} DrCheck;
static DrCheck g_dr[16]; static int g_ndr;
static int g_dr_repair;   /* 1 = a Repair would fix something (missing / damaged V3X files) */

static DrCheck *dr_add(const char *id, const char *name, int level, const wchar_t *todo_en, const wchar_t *todo_pt)
{
    DrCheck *c = &g_dr[g_ndr < 15 ? g_ndr++ : 15];
    memset(c, 0, sizeof *c);
    c->id = id; c->name = name; c->level = level; c->todo_en = todo_en; c->todo_pt = todo_pt;
    return c;
}

static const char *PROXY_DLLS[] = {"dinput8.dll", "winmm.dll", "dxgi.dll", "d3d11.dll", "d3d12.dll", "d3d9.dll",
                                   "dsound.dll", "winhttp.dll", "wininet.dll", "xinput1_3.dll", "xinput1_4.dll"};

static int plugin_known(const char *name)
{
    for (int i = 0; i < g_nprof; i++) if (!_stricmp(g_prof[i].name, name)) return 1;
    return 0;
}

static void do_doctor_checks(State *s)
{
    g_ndr = 0; g_dr_repair = 0;
    wchar_t p[MAX_PATH * 2];
    DrCheck *c;
    int running = game_running(s->game);

    /* D001 game build */
    if (s->supported) {
        c = dr_add("D001", "game_build", DR_OK, NULL, NULL);
        wfmt(c->msg_en, 400, L"Game version %hs is supported.", s->branch);
        wfmt(c->msg_pt, 400, L"A versão do jogo %hs é compatível.", s->branch);
    } else {
        c = dr_add("D001", "game_build", s->branch_ok ? DR_WARN : DR_PROBLEM,
                   L"Wait for a V3X update for this game version, or uninstall V3X until then.",
                   L"Espere uma atualização do V3X para esta versão do jogo, ou desinstale o V3X até lá.");
        wfmt(c->msg_en, 400, L"Game version %hs (build %hs) is not one this V3X was made for (%hs).", s->branch, s->build, supported_list());
        wfmt(c->msg_pt, 400, L"A versão do jogo %hs (build %hs) não é a que este V3X conhece (%hs).", s->branch, s->build, supported_list());
    }
    snprintf(c->detail, 400, "build=%s branch=%s", s->build, s->branch);

    /* D002 V3X files */
    join(p, s->bin, L"version.dll");
    int have_dll = file_exists(p), dll_v3x = have_dll && is_v3x_dll(p);
    if (s->installed) {
        int missing = 0, bad = 0, n = 0; char list[300] = ""; V3xStr ls; v3x_str_init(&ls, list, sizeof list);
        for (int i = 0; i < s->mf.n; i++) {
            MFile *f = &s->mf.f[i];
            if (strcmp(f->kind, "bin")) continue;
            n++;
            wchar_t fp[MAX_PATH * 2];
            char h[65] = "";
            int st = !rel_to_path(s->bin, f->rel, fp) ? 2 : !file_exists(fp) ? 1 : (!hash_file(fp, h) || strcmp(h, f->sha)) ? 2 : 0;
            if (st == 2 && !_stricmp(f->rel, "version.dll") && is_v3x_dll(fp)) st = 0;  /* a self-built loader */
            if (!st) continue;
            if (st == 1) missing++; else bad++;
            const char *b = strrchr(f->rel, '/'); b = b ? b + 1 : f->rel;
            v3x_str_appendf(&ls, "%s%s(%s)", ls.len ? "," : "", b, st == 1 ? "missing" : "changed");
        }
        if (missing || bad) {
            g_dr_repair = 1;
            c = dr_add("D002", "loader_files", DR_PROBLEM, L"Choose Repair: it copies the missing or damaged V3X files back.",
                       L"Escolha Reparar: ele copia de volta os arquivos do V3X que faltam ou estão danificados.");
            wfmt(c->msg_en, 400, L"%d V3X file(s) missing and %d damaged or changed: %hs", missing, bad, list);
            wfmt(c->msg_pt, 400, L"%d arquivo(s) do V3X faltando e %d danificado(s) ou alterado(s): %hs", missing, bad, list);
        } else {
            c = dr_add("D002", "loader_files", DR_OK, NULL, NULL);
            wfmt(c->msg_en, 400, L"All %d V3X files are present and intact (V3X %hs).", n, s->mf.version);
            wfmt(c->msg_pt, 400, L"Os %d arquivos do V3X estão presentes e intactos (V3X %hs).", n, s->mf.version);
        }
        snprintf(c->detail, 400, "files=%d missing=%d changed=%d%s%s", n, missing, bad, ls.len ? " list=" : "", list);
    } else if (dll_v3x) {
        c = dr_add("D002", "loader_files", DR_WARN, L"Run Install once: V3X then keeps a list of its files and can check and repair them.",
                   L"Rode Instalar uma vez: assim o V3X guarda a lista dos arquivos e consegue conferir e reparar.");
        WCOPY(c->msg_en, L"V3X was copied in by hand (no install list), so its files cannot be checked.");
        WCOPY(c->msg_pt, L"O V3X foi copiado à mão (sem lista de instalação), então os arquivos não podem ser conferidos.");
        snprintf(c->detail, 400, "manifest=no version_dll=v3x");
    } else {
        c = dr_add("D002", "loader_files", DR_PROBLEM, L"Choose Install to install V3X.", L"Escolha Instalar para instalar o V3X.");
        WCOPY(c->msg_en, L"V3X is not installed in this game folder.");
        WCOPY(c->msg_pt, L"O V3X não está instalado nesta pasta do jogo.");
        snprintf(c->detail, 400, "manifest=no version_dll=%s", have_dll ? "foreign" : "none");
    }

    /* D003 other proxy DLLs / a foreign version.dll */
    {
        char found[300] = ""; V3xStr fs; v3x_str_init(&fs, found, sizeof found); int nf = 0;
        if (have_dll && !dll_v3x) { v3x_str_append(&fs, "version.dll(not V3X)"); nf++; }
        for (size_t i = 0; i < sizeof PROXY_DLLS / sizeof *PROXY_DLLS; i++) {
            wchar_t w[64]; utf8_to_w(PROXY_DLLS[i], w, 64);
            join(p, s->bin, w);
            if (file_exists(p)) { v3x_str_appendf(&fs, "%s%s", fs.len ? "," : "", PROXY_DLLS[i]); nf++; }
        }
        if (nf) {
            int foreign = have_dll && !dll_v3x;
            c = dr_add("D003", "foreign_proxy", foreign ? DR_PROBLEM : DR_WARN,
                       foreign ? L"Another tool replaced V3X's version.dll: choose Repair (the other file is backed up), or remove that tool."
                               : L"If the game crashes or V3X misbehaves, move these files out of the binaries folder and test again.",
                       foreign ? L"Outra ferramenta trocou o version.dll do V3X: escolha Reparar (o outro arquivo vai para o backup) ou remova essa ferramenta."
                               : L"Se o jogo travar ou o V3X falhar, tire esses arquivos da pasta binaries e teste de novo.");
            if (foreign) g_dr_repair = 1;
            wfmt(c->msg_en, 400, L"Other mod loaders or overlays next to the game: %hs", found);
            wfmt(c->msg_pt, 400, L"Outros carregadores de mods ou overlays ao lado do jogo: %hs", found);
        } else {
            c = dr_add("D003", "foreign_proxy", DR_OK, NULL, NULL);
            WCOPY(c->msg_en, L"No other mod loader DLL next to the game.");
            WCOPY(c->msg_pt, L"Nenhuma outra DLL de carregador de mods ao lado do jogo.");
        }
        snprintf(c->detail, 400, "found=%s", nf ? found : "none");
    }

    /* D009 commit limit (page file) */
    {
        MEMORYSTATUSEX ms = {sizeof ms};
        GlobalMemoryStatusEx(&ms);
        double ram = (double)ms.ullTotalPhys / (1 << 30), lim = (double)ms.ullTotalPageFile / (1 << 30);
        int low = ms.ullTotalPageFile < ms.ullTotalPhys + ms.ullTotalPhys / 2;
        c = dr_add("D009", "pagefile", low ? DR_WARN : DR_OK,
                   low ? L"Let Windows manage the page file: Settings > System > About > Advanced system settings > Performance > Advanced > Virtual memory." : NULL,
                   low ? L"Deixe o Windows gerenciar o arquivo de paginação: Configurações > Sistema > Sobre > Configurações avançadas do sistema > Desempenho > Avançado > Memória virtual." : NULL);
        if (low) {
            wfmt(c->msg_en, 400, L"Windows memory limit is %.1f GB with %.1f GB RAM: the page file is small or off, the game can run out of memory.", lim, ram);
            wfmt(c->msg_pt, 400, L"O limite de memória do Windows é %.1f GB com %.1f GB de RAM: o arquivo de paginação é pequeno ou está desligado, o jogo pode ficar sem memória.", lim, ram);
        } else {
            wfmt(c->msg_en, 400, L"Windows memory limit %.1f GB for %.1f GB RAM is fine.", lim, ram);
            wfmt(c->msg_pt, 400, L"Limite de memória do Windows de %.1f GB para %.1f GB de RAM está bom.", lim, ram);
        }
        snprintf(c->detail, 400, "commit_limit_gb=%.1f ram_gb=%.1f", lim, ram);
    }

    /* D010 free disk space: game drive and C: */
    {
        wchar_t drives[2][MAX_PATH * 2]; int nd = 0;
        if (!GetVolumePathNameW(s->game, drives[0], MAX_PATH * 2)) WCOPY(drives[0], L"C:\\");
        nd = 1;
        if (_wcsnicmp(drives[0], L"C:\\", 3)) { WCOPY(drives[1], L"C:\\"); nd = 2; }
        int worst = DR_OK; char det[200] = ""; V3xStr ds; v3x_str_init(&ds, det, sizeof det); wchar_t lst[200] = L""; size_t wn = 0;
        for (int i = 0; i < nd; i++) {
            ULARGE_INTEGER fr; double gb = -1;
            if (GetDiskFreeSpaceExW(drives[i], &fr, NULL, NULL)) gb = (double)fr.QuadPart / (1 << 30);
            int lv = gb < 0 ? DR_OK : gb < 1 ? DR_PROBLEM : gb < 5 ? DR_WARN : DR_OK;
            if (lv > worst) worst = lv;
            v3x_str_appendf(&ds, "%s%c=%.1fGB", ds.len ? " " : "", (char)drives[i][0], gb);
            wcat(lst, 200, &wn, L"%s%c: %.1f GB", wn ? L", " : L"", drives[i][0], gb);
        }
        c = dr_add("D010", "disk_free", worst,
                   worst ? L"Free some space (at least 5 GB): Windows and the game need it for saves, logs and memory." : NULL,
                   worst ? L"Libere espaço (pelo menos 5 GB): o Windows e o jogo precisam dele para saves, logs e memória." : NULL);
        wfmt(c->msg_en, 400, L"Free disk space: %s%s", lst, worst ? L" - low" : L"");
        wfmt(c->msg_pt, 400, L"Espaço livre em disco: %s%s", lst, worst ? L" - pouco" : L"");
        snprintf(c->detail, 400, "%s", det);
    }

    /* D012 crash during the last start (loading.marker) */
    {
        uint8_t *b; size_t n; char stem[64] = "";
        join(p, s->bin, L"v3x\\loading.marker");
        if (!running && read_all(p, &b, &n)) { snprintf(stem, sizeof stem, "%s", (char *)b); free(b); trim(stem); if (!stem[0]) snprintf(stem, sizeof stem, "?"); }
        if (stem[0]) {
            c = dr_add("D012", "crash_loop", DR_PROBLEM,
                       L"Turn that plugin off (main menu > V3X, or <plugin>=0 under [plugins] in binaries\\v3x\\v3x.ini) and send binaries\\v3x\\v3x.log to the V3X developers.",
                       L"Desligue esse plugin (menu principal > V3X, ou <plugin>=0 em [plugins] no binaries\\v3x\\v3x.ini) e mande o binaries\\v3x\\v3x.log para quem faz o V3X.");
            wfmt(c->msg_en, 400, L"The game stopped while V3X was loading the plugin \"%hs\" at the last start.", stem);
            wfmt(c->msg_pt, 400, L"O jogo parou enquanto o V3X carregava o plugin \"%hs\" na última vez.", stem);
            snprintf(c->detail, 400, "plugin=%s", stem);
        } else {
            c = dr_add("D012", "crash_loop", DR_OK, NULL, NULL);
            WCOPY(c->msg_en, running ? L"The game is running, the last-start check is skipped." : L"The last start loaded every plugin.");
            WCOPY(c->msg_pt, running ? L"O jogo está aberto, a checagem da última abertura foi pulada." : L"A última abertura carregou todos os plugins.");
            snprintf(c->detail, 400, "marker=%s", running ? "skipped" : "none");
        }
    }

    /* D013 last crash (crash_logger's crash.log) */
    {
        uint8_t *b; size_t n; char line[200] = "", mod[100] = "";
        join(p, s->bin, L"v3x\\crash.log");
        if (read_all(p, &b, &n)) {
            char *hit = NULL, *q = (char *)b;
            while ((q = strstr(q, "UNHANDLED EXCEPTION")) != NULL) { hit = q; q++; }
            if (hit) {
                size_t k = 0;
                while (hit[k] && hit[k] != '\r' && hit[k] != '\n' && k < sizeof line - 1) k++;
                memcpy(line, hit, k); line[k] = 0;
                char *at = strstr(line, " at ");
                if (at) { at += 4; size_t m = 0; while (at[m] && at[m] != ' ' && at[m] != '+' && m < sizeof mod - 1) m++; memcpy(mod, at, m); mod[m] = 0; }
            }
            free(b);
        }
        if (line[0]) {
            c = dr_add("D013", "last_crash", DR_WARN,
                       L"Send binaries\\v3x\\crash.log to the V3X developers. If the module is a V3X plugin, turn that plugin off.",
                       L"Mande o binaries\\v3x\\crash.log para quem faz o V3X. Se o módulo for um plugin do V3X, desligue esse plugin.");
            wfmt(c->msg_en, 400, L"The game crashed last time, in %hs.", mod[0] ? mod : "?");
            wfmt(c->msg_pt, 400, L"O jogo travou na última vez, em %hs.", mod[0] ? mod : "?");
            snprintf(c->detail, 400, "module=%s line=%s", mod[0] ? mod : "?", line);
        } else {
            c = dr_add("D013", "last_crash", DR_OK, NULL, NULL);
            WCOPY(c->msg_en, L"No crash recorded in crash.log.");
            WCOPY(c->msg_pt, L"Nenhum travamento registrado no crash.log.");
            snprintf(c->detail, 400, "crash=none");
        }
    }

    /* D014 v3x.ini [plugins] */
    {
        uint8_t *b; size_t n;
        join(p, s->bin, L"v3x\\v3x.ini");
        if (!read_all(p, &b, &n)) {
            int inst = s->installed || dll_v3x;
            c = dr_add("D014", "ini_syntax", inst ? DR_WARN : DR_OK,
                       inst ? L"Choose Repair to create it again (every plugin's default applies until then)." : NULL,
                       inst ? L"Escolha Reparar para criar de novo (até lá vale o padrão de cada plugin)." : NULL);
            WCOPY(c->msg_en, inst ? L"binaries\\v3x\\v3x.ini is missing: every plugin found is loaded." : L"No v3x.ini (V3X is not installed).");
            WCOPY(c->msg_pt, inst ? L"Falta o binaries\\v3x\\v3x.ini: todo plugin encontrado é carregado." : L"Sem v3x.ini (o V3X não está instalado).");
            snprintf(c->detail, 400, "ini=missing");
            if (inst) g_dr_repair = 1;
        } else {
            char unk[200] = "", miss[200] = "", bad[120] = ""; V3xStr us, ms, bs; v3x_str_init(&us, unk, sizeof unk); v3x_str_init(&ms, miss, sizeof miss); v3x_str_init(&bs, bad, sizeof bad); int nu = 0, nm = 0, nb = 0, in = 0;
            for (char *line = strtok((char *)b, "\n"); line; line = strtok(NULL, "\n")) {
                trim(line);
                if (!line[0] || line[0] == '#' || line[0] == ';') continue;
                if (line[0] == '[') { char *c2 = line + 1; while (*c2 == ' ') c2++; in = !_strnicmp(c2, "plugins", 7) && (c2[7] == ']' || c2[7] == ' '); continue; }
                if (!in) continue;
                char *eq = strchr(line, '=');
                if (!eq) { v3x_str_appendf(&bs, "%s%.24s", bs.len ? "," : "", line); nb++; continue; }
                char key[64]; size_t kl = (size_t)(eq - line); while (kl && (line[kl - 1] == ' ' || line[kl - 1] == '\t')) kl--;
                if (kl >= sizeof key) kl = sizeof key - 1;
                memcpy(key, line, kl); key[kl] = 0;
                char *v = eq + 1; while (*v == ' ' || *v == '\t') v++;
                int on = !(!_stricmp(v, "0") || !_stricmp(v, "off") || !_stricmp(v, "no") || !_stricmp(v, "false"));
                wchar_t wk[64], dp[MAX_PATH * 2]; utf8_to_w(key, wk, 64);
                wfmt(dp, MAX_PATH * 2, L"%s\\v3x\\plugins\\%s.dll", s->bin, wk);
                int has = file_exists(dp);
                if (!has && !plugin_known(key)) { v3x_str_appendf(&us, "%s%s", us.len ? "," : "", key); nu++; }
                else if (!has && on) { v3x_str_appendf(&ms, "%s%s", ms.len ? "," : "", key); nm++; }
            }
            free(b);
            int lv = nm ? DR_PROBLEM : (nu || nb) ? DR_WARN : DR_OK;
            c = dr_add("D014", "ini_syntax", lv,
                       nm ? L"Choose Repair to bring the plugin files back, or set those plugins to 0 in binaries\\v3x\\v3x.ini."
                          : lv ? L"Check the spelling in binaries\\v3x\\v3x.ini under [plugins]: one line per plugin, like keep_history=1." : NULL,
                       nm ? L"Escolha Reparar para trazer os arquivos dos plugins de volta, ou ponha esses plugins em 0 no binaries\\v3x\\v3x.ini."
                          : lv ? L"Confira a escrita no binaries\\v3x\\v3x.ini em [plugins]: uma linha por plugin, como keep_history=1." : NULL);
            if (nm) g_dr_repair = 1;
            if (lv == DR_OK) { WCOPY(c->msg_en, L"v3x.ini looks fine."); WCOPY(c->msg_pt, L"O v3x.ini parece certo."); }
            else {
                wfmt(c->msg_en, 400, L"v3x.ini: %d plugin(s) turned on but their file is missing (%hs), %d unknown name(s) (%hs), %d line(s) without '=' (%hs).",
                           nm, nm ? miss : "-", nu, nu ? unk : "-", nb, nb ? bad : "-");
                wfmt(c->msg_pt, 400, L"v3x.ini: %d plugin(s) ligado(s) sem o arquivo (%hs), %d nome(s) desconhecido(s) (%hs), %d linha(s) sem '=' (%hs).",
                           nm, nm ? miss : "-", nu, nu ? unk : "-", nb, nb ? bad : "-");
            }
            snprintf(c->detail, 400, "enabled_missing=%s unknown=%s no_equals=%d", nm ? miss : "none", nu ? unk : "none", nb);
        }
    }

    /* D015 game running (offline only) */
    c = dr_add("D015", "game_running", running ? DR_WARN : DR_OK,
               running ? L"Close the game before Repair, Update or Uninstall." : NULL,
               running ? L"Feche o jogo antes de Reparar, Atualizar ou Desinstalar." : NULL);
    WCOPY(c->msg_en, running ? L"Victoria 3 is running from this folder." : L"Victoria 3 is not running.");
    WCOPY(c->msg_pt, running ? L"O Victoria 3 está aberto a partir desta pasta." : L"O Victoria 3 não está aberto.");
    snprintf(c->detail, 400, "running=%d", running);
}

static const wchar_t *dr_word(int lv) { return lv == DR_OK ? L"OK" : lv == DR_WARN ? L"WARN" : L"PROBLEM"; }
static const wchar_t *dr_word_tr(int lv) { return lv == DR_OK ? L"OK" : lv == DR_WARN ? TR(L"WARN", L"AVISO") : TR(L"PROBLEM", L"PROBLEMA"); }

static int dr_problems(void)
{
    int n = 0;
    for (int i = 0; i < g_ndr; i++) n += g_dr[i].level == DR_PROBLEM;
    return n;
}

/* --doctor: English human lines + machine lines on stdout, doctor_offline.txt; returns #PROBLEMs */
static int do_doctor(State *s)
{
    do_doctor_checks(s);
    say(L"V3X doctor (offline), V3X-Setup %hs, game folder %s", g_version, s->game);
    for (int i = 0; i < g_ndr; i++) {
        DrCheck *c = &g_dr[i];
        say(L"%-7s %s%s%s", dr_word(c->level), c->msg_en, c->todo_en ? L"  What to do: " : L"", c->todo_en ? c->todo_en : L"");
    }
    for (int i = 0; i < g_ndr; i++)
        say(L"doctor: %hs %hs %s %hs", g_dr[i].id, g_dr[i].name, dr_word(g_dr[i].level), g_dr[i].detail);
    int np = dr_problems();
    int nw = 0;
    for (int i = 0; i < g_ndr; i++) nw += g_dr[i].level == DR_WARN;
    say(L"doctor: result problems=%d warnings=%d repair=%d", np, nw, g_dr_repair);
    wchar_t dir[MAX_PATH * 2], rp[MAX_PATH * 2];
    join(dir, s->bin, L"v3x");
    if (dir_exists(dir) && g_out) {
        join(rp, dir, L"doctor_offline.txt");
        int n = WideCharToMultiByte(CP_UTF8, 0, g_out, (int)g_out_n, NULL, 0, NULL, NULL);
        char *u = malloc((size_t)n + 1);
        if (u) { WideCharToMultiByte(CP_UTF8, 0, g_out, (int)g_out_n, u, n, NULL, NULL); write_all(rp, u, (size_t)n); free(u); }
    }
    return np;
}

/* ------------------------------------------------------------------ GUI */

static const wchar_t *TITLE = L"V3X Setup";
static wchar_t g_title_ver[128];

static int pick_folder(HWND owner, wchar_t *out)
{
    for (;;) {
        BROWSEINFOW bi = {0};
        bi.hwndOwner = owner;
        bi.lpszTitle = TR(L"Select your \"Victoria 3\" game folder (the one that contains the \"binaries\" folder).\n"
                          L"Steam: right-click Victoria 3 > Manage > Browse local files.",
                          L"Escolha a pasta do jogo \"Victoria 3\" (a que tem a pasta \"binaries\").\n"
                          L"Steam: clique com o botão direito em Victoria 3 > Gerenciar > Ver arquivos locais.");
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_NONEWFOLDERBUTTON;
        PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi);
        if (!id) return 0;
        wchar_t p[MAX_PATH * 2];
        BOOL ok = SHGetPathFromIDListW(id, p);
        CoTaskMemFree(id);
        if (ok && normalize_game_dir(p, out)) return 1;
        MessageBoxW(owner, TR(L"That folder does not contain binaries\\victoria3.exe. Please choose the \"Victoria 3\" folder.",
                              L"Essa pasta não tem binaries\\victoria3.exe. Escolha a pasta \"Victoria 3\"."),
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
    c.pszExpandedControlText = TR(L"Hide details", L"Ocultar detalhes");
    c.pszCollapsedControlText = TR(L"Show details", L"Mostrar detalhes");
    c.pszFooter = footer; c.pszFooterIcon = footer ? TD_INFORMATION_ICON : NULL;
    int btn = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&c, &btn, NULL, verified))) {
        /* very old comctl32: plain message box */
        wchar_t msg[4096]; wfmt(msg, 4096, L"%s\n\n%s", main, content ? content : L"");
        btn = MessageBoxW(owner, msg, g_title_ver, MB_OKCANCEL) == IDOK ? (nb ? btns[0].nButtonID : IDOK) : IDCANCEL;
    }
    return btn;
}

static const wchar_t *SP_NOTE_EN =
    L"V3X is for single player only. Do not use it in multiplayer: uninstall it first, or rename "
    L"binaries\\version.dll (for example to version.dll.off) to start the game without V3X.";
static const wchar_t *SP_NOTE_PT =
    L"O V3X é só para um jogador. Não use no multijogador: desinstale antes, ou renomeie "
    L"binaries\\version.dll (por exemplo para version.dll.off) para abrir o jogo sem o V3X.";
#define SP_NOTE TR(SP_NOTE_EN, SP_NOTE_PT)

static int gui(const wchar_t *game_arg)
{
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    wfmt(g_title_ver, 128, L"%s %hs", TITLE, g_version);
    wchar_t game[MAX_PATH * 2] = L"";
    if (game_arg && !normalize_game_dir(game_arg, game)) game[0] = 0;
    if (!game[0] && !find_game(game)) {
        TASKDIALOG_BUTTON b[] = {{100, TR(L"Choose the game folder...\nUsually ...\\steamapps\\common\\Victoria 3",
                                          L"Escolher a pasta do jogo...\nNormalmente ...\\steamapps\\common\\Victoria 3")}};
        if (task(NULL, TD_INFORMATION_ICON, TR(L"Victoria 3 was not found automatically", L"Não achei o Victoria 3 sozinho"),
                 TR(L"Setup looked in your Steam libraries. Please show it where Victoria 3 is installed.",
                    L"Procurei nas suas bibliotecas da Steam. Mostre onde o Victoria 3 está instalado."), b, 1, 100,
                 NULL, NULL, NULL, NULL, TDCBF_CANCEL_BUTTON) != 100 || !pick_folder(NULL, game))
            return RC_NOGAME;
    }
    for (;;) {
        State s; state_read(&s, game);
        wchar_t content[4096];
        wfmt(content, 4096,
                   TR(L"Game folder:\n%s\n\nGame version: %hs  (%s)\nV3X: %s%hs",
                      L"Pasta do jogo:\n%s\n\nVersão do jogo: %hs  (%s)\nV3X: %s%hs"),
                   s.game, s.branch,
                   s.supported ? TR(L"supported", L"compatível")
                               : (s.branch_ok ? TR(L"different build: not tested", L"build diferente: não testado")
                                              : TR(L"NOT supported", L"NÃO compatível")),
                   s.installed ? TR(L"installed, version ", L"instalado, versão ") : TR(L"not installed", L"não instalado"),
                   s.installed ? s.mf.version : "");
        int en = 0, dis = 0;
        for (int i = 0; i < g_nprof; i++) { if (g_prof[i].enabled) en++; else dis++; }
        wchar_t inst[256], rep[256];
        wfmt(inst, 256, TR(L"Install V3X\nAdds version.dll and %d plugins (%d on, %d off). No game file is changed.",
                                 L"Instalar o V3X\nAdiciona version.dll e %d plugins (%d ligados, %d desligados). Nenhum arquivo do jogo é alterado."),
                   en + dis, en, dis);
        wfmt(rep, 256, L"%s", TR(L"Repair / update V3X\nRe-copies missing or changed V3X files. Your settings stay as they are.",
                                       L"Reparar / atualizar o V3X\nCopia de novo os arquivos do V3X que faltam ou mudaram. Suas configurações ficam como estão."));
        TASKDIALOG_BUTTON b[5]; int nb = 0;
        b[nb].nButtonID = 100; b[nb++].pszButtonText = s.installed ? rep : inst;
        if (s.installed) {
            b[nb].nButtonID = 102;
            b[nb++].pszButtonText = TR(L"Uninstall V3X\nRemoves V3X's files and restores what was there before.",
                                       L"Desinstalar o V3X\nRemove os arquivos do V3X e põe de volta o que havia antes.");
        }
        b[nb].nButtonID = 104;
        b[nb++].pszButtonText = TR(L"Check my install\nLooks for missing files, conflicts and other problems. Changes nothing.",
                                   L"Verificar instalação\nProcura arquivos faltando, conflitos e outros problemas. Não muda nada.");
        b[nb].nButtonID = 103; b[nb++].pszButtonText = TR(L"Choose another game folder...", L"Escolher outra pasta do jogo...");
        int btn = task(NULL, s.supported ? TD_INFORMATION_ICON : TD_WARNING_ICON,
                       s.installed ? TR(L"V3X is installed", L"O V3X está instalado")
                                   : TR(L"Install V3X for Victoria 3", L"Instalar o V3X no Victoria 3"),
                       content, b, nb, 100, NULL, NULL,
                       NULL, SP_NOTE, TDCBF_CLOSE_BUTTON);
        if (btn == 103) { wchar_t g2[MAX_PATH * 2]; if (pick_folder(NULL, g2)) WCOPY(game, g2); continue; }
        if (btn == 104) {
            g_out_n = 0; if (g_out) g_out[0] = 0;
            int np = do_doctor(&s);   /* fills g_dr, writes doctor_offline.txt */
            static wchar_t body[8192]; size_t k = 0; body[0] = 0;
            for (int i = 0; i < g_ndr && k < 8000; i++) {
                DrCheck *c = &g_dr[i];
                const wchar_t *todo = g_pt ? c->todo_pt : c->todo_en;
                wcat(body, 8192, &k, L"%s%s: %s%s%s", k ? L"\n\n" : L"", dr_word_tr(c->level),
                                        g_pt ? c->msg_pt : c->msg_en, todo ? TR(L"\n    What to do: ", L"\n    O que fazer: ") : L"",
                                        todo ? todo : L"");
            }
            wchar_t head[128];
            if (np) wfmt(head, 128, TR(L"%d problem(s) found", L"%d problema(s) encontrado(s)"), np);
            else wfmt(head, 128, L"%s", TR(L"No problems found", L"Nenhum problema encontrado"));
            TASKDIALOG_BUTTON rb[] = {{105, TR(L"Repair now\nCopies missing or damaged V3X files back.",
                                               L"Reparar agora\nCopia de volta os arquivos do V3X que faltam ou estão danificados.")}};
            int r = task(NULL, np ? TD_WARNING_ICON : TD_INFORMATION_ICON, head, body, rb, g_dr_repair ? 1 : 0, 0,
                         NULL, NULL, NULL, NULL, TDCBF_CLOSE_BUTTON);
            if (r != 105) continue;
            btn = 100;   /* Repair */
        }
        if (btn != 100 && btn != 102) return RC_OK;
        if (game_running(game)) {
            task(NULL, TD_WARNING_ICON, TR(L"Victoria 3 is running", L"O Victoria 3 está aberto"),
                 TR(L"Close the game first (the Paradox launcher may stay open), then try again.",
                    L"Feche o jogo primeiro (o launcher da Paradox pode ficar aberto) e tente de novo."), NULL, 0, 0,
                 NULL, NULL, NULL, NULL, TDCBF_OK_BUTTON);
            continue;
        }
        int rc, force = 0, purge = 0;
        if (btn == 100 && !s.supported) {
            wchar_t w[1024];
            wfmt(w, 1024, TR(L"Your game is %hs (build %hs). This V3X was made and tested for %hs.\n\n"
                                   L"Plugins check the engine code and switch themselves off where it changed, but this "
                                   L"combination was never tested. The usual fix is to wait for a V3X update.",
                                   L"Seu jogo é %hs (build %hs). Este V3X foi feito e testado para %hs.\n\n"
                                   L"Os plugins conferem o código do jogo e se desligam onde ele mudou, mas esta "
                                   L"combinação nunca foi testada. O normal é esperar uma atualização do V3X."),
                       s.branch, s.build, supported_list());
            TASKDIALOG_BUTTON fb[] = {{200, TR(L"Install anyway", L"Instalar mesmo assim")}};
            if (task(NULL, TD_WARNING_ICON, TR(L"This game version is not supported", L"Esta versão do jogo não é compatível"),
                     w, fb, 1, 0, NULL, NULL, NULL,
                     NULL, TDCBF_CANCEL_BUTTON) != 200) continue;
            force = 1;
        }
        if (btn == 102) {
            BOOL v = FALSE;
            TASKDIALOG_BUTTON ub[] = {{201, TR(L"Uninstall", L"Desinstalar")}};
            if (task(NULL, TD_WARNING_ICON, TR(L"Uninstall V3X?", L"Desinstalar o V3X?"),
                     TR(L"Removes version.dll and the V3X plugins and restores a version.dll that was there before. "
                        L"Your logs and changed settings in binaries\\v3x are kept unless you tick the box.",
                        L"Remove o version.dll e os plugins do V3X e põe de volta um version.dll que havia antes. "
                        L"Seus logs e configurações alteradas em binaries\\v3x ficam, a não ser que marque a caixa."),
                     ub, 1, 0, TR(L"Also delete logs, reports and settings (the whole binaries\\v3x folder)",
                                  L"Apagar também logs, relatórios e configurações (a pasta binaries\\v3x inteira)"), &v, NULL,
                     NULL, TDCBF_CANCEL_BUTTON) != 201) continue;
            purge = v;
        }
        g_out_n = 0; if (g_out) g_out[0] = 0;
        rc = btn == 100 ? do_install(&s, force) : do_uninstall(&s, purge);
        const wchar_t *head;
        wchar_t body[1024];
        if (rc == RC_OK && btn == 100) {
            head = TR(L"V3X is installed", L"O V3X está instalado");
            wfmt(body, 1024, L"%s", TR(
                       L"Start Victoria 3 as usual (Steam or the Paradox launcher). A small \"V3X console\" "
                       L"window opens next to the game and lists what loaded.\n\nMost plugins are installed but off. "
                       L"Turn them on or off: main menu > V3X button, or binaries\\v3x\\v3x.ini. "
                       L"Run this setup again to repair or uninstall.",
                       L"Abra o Victoria 3 como sempre (Steam ou launcher da Paradox). Uma pequena janela \"V3X console\" "
                       L"abre ao lado do jogo e mostra o que carregou.\n\nA maioria dos plugins vem instalada, mas desligada. "
                       L"Para ligar ou desligar: menu principal > botão V3X, ou binaries\\v3x\\v3x.ini. "
                       L"Rode este instalador de novo para reparar ou desinstalar."));
        } else if (rc == RC_OK) {
            head = TR(L"V3X is uninstalled", L"O V3X foi desinstalado");
            wfmt(body, 1024, L"%s", TR(L"The game starts without V3X now.", L"Agora o jogo abre sem o V3X."));
        } else {
            head = TR(L"Setup did not finish", L"A instalação não terminou");
            wfmt(body, 1024, L"%s", g_out ? g_out : L"");
        }
        task(NULL, rc == RC_OK ? TD_INFORMATION_ICON : TD_ERROR_ICON, head, body, NULL, 0, 0, NULL, NULL,
             rc == RC_OK ? g_out : NULL, rc == RC_OK && btn == 100 ? SP_NOTE : NULL, TDCBF_OK_BUTTON);
        return rc;
    }
}

/* ------------------------------------------------------------------ --verify-release (H1)
 * compat.json scanner: cj_str / cj_int / cj_assets / cj_next_asset in setup_parse.h. */

static int verify_release(const wchar_t *dir)
{
    wchar_t cp[MAX_PATH * 2], sp[MAX_PATH * 2], fp[MAX_PATH * 2];
    uint8_t *cj = NULL, *sig = NULL;
    size_t cn = 0, sn = 0;
    int rc = RC_UNVERIFIED;
    if (!join(cp, dir, L"compat.json") || !join(sp, dir, L"compat.json.sig")) { say(L"verify-release: FAILED path too long"); return rc; }
    if (!read_all(cp, &cj, &cn) || cn > (1u << 20)) { say(L"verify-release: FAILED cannot read %s", cp); goto out; }
    if (!read_all(sp, &sig, &sn) || sn > 4096) { say(L"verify-release: FAILED no signature (%s): the release is not signed", sp); goto out; }
    V3xSigResult vr;
    if (!v3x_release_verify(cj, cn, (const char *)sig, sn, &vr)) {
        say(L"verify-release: FAILED signature: %hs (key id %hs; this setup trusts %d key(s))", vr.why,
            vr.key_id[0] ? vr.key_id : "-", v3x_release_keys_configured());
        goto out;
    }
    const char *slot = vr.slot == V3X_SIG_SLOT_CURRENT ? "current" : vr.slot == V3X_SIG_SLOT_NEXT ? "next" : "DEV TEST";
    say(L"verify-release: signature ok (key id %hs, %hs key)", vr.key_id, slot);
    const char *p = (const char *)cj, *e = p + cn;
    char ver[64];
    if (!cj_str(p, e, "v3x_version", ver, sizeof ver) || strcmp(ver, g_version)) {
        say(L"verify-release: FAILED compat.json is for V3X %hs, this setup is %hs", cj_str(p, e, "v3x_version", ver, sizeof ver) ? ver : "?", g_version);
        goto out;
    }
    const char *as = cj_assets(p, e);
    if (!as) { say(L"verify-release: FAILED compat.json has no assets list"); goto out; }
    int checked = 0, self_ok = 0, bad = 0;
    for (const char *it = as + 1, *q, *oe;;) {
        int nx = cj_next_asset(&it, e, &q, &oe);
        if (nx == 0) break;
        if (nx < 0) { say(L"verify-release: FAILED malformed assets list"); goto out; }
        char name[96], hex[80];
        unsigned long long size = 0;
        if (!cj_str(q, oe, "name", name, sizeof name) || !cj_str(q, oe, "sha256", hex, sizeof hex) || !hex64_ok(hex) ||
            !cj_int(q, oe, "size", &size) || v3x_path_rel_ok_ex(name, V3X_PATH_SINGLE) != V3X_R_OK) {
            say(L"verify-release: FAILED malformed asset entry"); goto out;
        }
        wchar_t wn[96];
        utf8_to_w(name, wn, 96);
        if (!wn[0] || !join(fp, dir, wn)) { say(L"verify-release: FAILED bad asset name"); goto out; }
        int is_setup = !strcmp(name, "V3X-Setup.exe");
        if (is_setup) {   /* this exe itself must be the signed one (chain: signature -> hash -> code) */
            uint8_t h[32]; char hx[65];
            if (sha256(g_self, g_self_n, h)) { hex32(h, hx); self_ok = !strcmp(hx, hex) && (unsigned long long)g_self_n == size; }
            if (!self_ok) { say(L"verify-release: FAILED this V3X-Setup.exe is not the one compat.json names"); bad++; }
        }
        if (file_exists(fp)) {
            char hx[65] = "";
            WIN32_FILE_ATTRIBUTE_DATA fa;
            unsigned long long fsz = GetFileAttributesExW(fp, GetFileExInfoStandard, &fa)
                                         ? ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow : ~0ull;
            if (fsz != size || !hash_file(fp, hx) || strcmp(hx, hex)) { say(L"verify-release: FAILED %hs does not match compat.json", name); bad++; }
            else { say(L"verify-release: %hs ok", name); checked++; }
        } else if (is_setup) {
            say(L"verify-release: FAILED %hs is missing in %s", name, dir); bad++;
        }
    }
    if (!self_ok && !bad) { say(L"verify-release: FAILED compat.json does not list V3X-Setup.exe"); bad++; }
    if (bad) goto out;
    say(L"verify-release: ok version=%hs files=%d key=%hs", g_version, checked, vr.key_id);
    rc = RC_OK;
out:
    free(cj); free(sig);
    return rc;
}

/* ------------------------------------------------------------------ main */

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)hi; (void)prev; (void)cmdline; (void)show;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    pick_language();
    const wchar_t *game_arg = NULL;
    int uninstall = 0, purge = 0, force = 0, status = 0, update = 0, doctor = 0;
    const wchar_t *verify_dir = NULL;
    DWORD wait_pid = 0;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--silent")) g_silent = 1;
        else if (!wcscmp(argv[i], L"--uninstall")) uninstall = 1;
        else if (!wcscmp(argv[i], L"--repair") || !wcscmp(argv[i], L"--install")) uninstall = 0;
        else if (!wcscmp(argv[i], L"--purge")) purge = 1;
        else if (!wcscmp(argv[i], L"--force")) force = 1;
        else if (!wcscmp(argv[i], L"--status")) { status = 1; g_silent = 1; }
        else if (!wcscmp(argv[i], L"--game-dir") && i + 1 < argc) game_arg = argv[++i];
        else if (!wcscmp(argv[i], L"--update")) { update = 1; g_silent = 1; }
        else if (!wcscmp(argv[i], L"--doctor")) { doctor = 1; g_silent = 1; }
        else if (!wcscmp(argv[i], L"--verify-release") && i + 1 < argc) { verify_dir = argv[++i]; g_silent = 1; }
        else if (!wcscmp(argv[i], L"--wait-pid") && i + 1 < argc && (wait_pid = wcstoul(argv[i + 1], NULL, 10)) != 0) i++;
        else {
            g_silent = 1;
            say(L"usage: V3X-Setup.exe [--silent] [--game-dir <Victoria 3 folder>] [--install|--repair|--uninstall [--purge]] [--force] [--status]"
                L" [--wait-pid <pid>] [--update] [--doctor] [--verify-release <dir>]");
            flush_stdout();
            return RC_USAGE;
        }
    }
    if (update) { uninstall = 0; force = 0; status = 0; doctor = 0; g_defer_log = 1; }
    if (!load_payload()) {
        if (update) {   /* still leave a result line where the updater looks for it */
            wchar_t game[MAX_PATH * 2], bin[MAX_PATH * 2];
            say(L"ERROR: the setup file is damaged or incomplete (payload check failed). Download it again.");
            say(L"update-result: failed code=%d payload", RC_PAYLOAD);
            if (game_arg ? normalize_game_dir(game_arg, game) : find_game(game)) {
                join(bin, game, L"binaries"); g_defer_log = 0; append_log(bin);
            }
            flush_stdout();
            return 1;
        }
        if (g_silent) { say(L"ERROR: the setup file is damaged or incomplete (payload check failed). Download it again."); flush_stdout(); }
        else MessageBoxW(NULL, TR(L"This setup file is damaged or incomplete. Please download it again.",
                                  L"Este instalador está danificado ou incompleto. Baixe de novo."), TITLE, MB_OK | MB_ICONERROR);
        return RC_PAYLOAD;
    }
    if (verify_dir && !update) {
        int vrc = verify_release(verify_dir);
        flush_stdout();
        return vrc;
    }
    if (wait_pid) {
        /* the updater starts us and then the game exits: wait for it (version.dll is locked while it runs) */
        HANDLE ph = OpenProcess(SYNCHRONIZE, FALSE, wait_pid);
        if (ph) {
            DWORD ms = 600000;   /* 10 min; env V3X_WAIT_TIMEOUT_SEC shortens it (tests) */
            char tv[16]; DWORD tn = GetEnvironmentVariableA("V3X_WAIT_TIMEOUT_SEC", tv, sizeof tv);
            if (tn && tn < sizeof tv && atoi(tv) > 0) ms = (DWORD)atoi(tv) * 1000;
            DWORD w = WaitForSingleObject(ph, ms);
            CloseHandle(ph);
            if (w != WAIT_OBJECT_0) {
                if (!g_silent) {
                    MessageBoxW(NULL, TR(L"Victoria 3 did not close within 10 minutes. Nothing was changed.",
                                         L"O Victoria 3 não fechou em 10 minutos. Nada foi alterado."), TITLE, MB_OK | MB_ICONWARNING);
                    return RC_RUNNING;
                }
                say(L"process %lu did not exit within %lu s. Nothing was changed.", (unsigned long)wait_pid, (unsigned long)(ms / 1000));
                wchar_t game[MAX_PATH * 2], bin[MAX_PATH * 2];
                if (update) {
                    say(L"update-result: failed code=%d timeout", RC_RUNNING);
                    if (game_arg ? normalize_game_dir(game_arg, game) : find_game(game)) {
                        join(bin, game, L"binaries"); g_defer_log = 0; append_log(bin);
                    }
                }
                flush_stdout();
                return update ? 1 : RC_RUNNING;
            }
        }
    }
    if (!g_silent) return gui(game_arg);

    wchar_t game[MAX_PATH * 2] = L"";
    int found = game_arg ? normalize_game_dir(game_arg, game) : find_game(game);
    if (!found) {
        say(L"Victoria 3 not found%s%s. Pass --game-dir \"<...>\\steamapps\\common\\Victoria 3\".",
            game_arg ? L" in " : L"", game_arg ? game_arg : L"");
        if (update) say(L"update-result: failed code=%d nogame", RC_NOGAME);
        flush_stdout();
        return update ? 1 : RC_NOGAME;
    }
    State s; state_read(&s, game);
    int rc = RC_OK;
    if (status) do_status(&s);
    else if (doctor) rc = do_doctor(&s);
    else {
        say(L"V3X-Setup %hs: %s %s (game %hs, build %hs)", g_version, uninstall ? L"uninstall from" : L"install into",
            s.game, s.branch, s.build);
        rc = uninstall ? do_uninstall(&s, purge) : do_install(&s, force);
    }
    if (update) {
        static const wchar_t *why[] = {L"ok", L"error", L"unsupported", L"running", L"nogame", L"notinstalled", L"payload", L"usage"};
        if (rc == RC_OK) say(L"update-result: ok version=%hs", g_version);
        else say(L"update-result: failed code=%d %s", rc, (rc > 0 && rc <= RC_USAGE) ? why[rc] : L"error");
        g_defer_log = 0;
        append_log(s.bin);
        flush_stdout();
        return rc == RC_OK ? 0 : 1;
    }
    flush_stdout();
    return rc;
}
