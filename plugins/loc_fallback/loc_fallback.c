/* loc_fallback: English fallback for localization keys missing in the player's language.
 *
 * Many mods ship only localization/english/<x>_l_english.yml; in another language the game then shows
 * the raw keys. At plugin load (before the game's CRT/WinMain, so before the VFS is built and long
 * before localization is read) this computes every English key of the enabled mods that no file of
 * the player's language defines (vanilla or any mod) and writes them, English text verbatim, to
 *     binaries/v3x/content_packs/v3x_loc_fallback/localization/<lang>/zz_v3x_fallback_l_<lang>.yml
 * plugins/content_packs mounts that folder above game/ but below mods, and the file only holds keys
 * nobody else defines in that language, so every real translation still wins. Data only: no engine
 * hook. Details and engine facts: docs/re/localization.md. Logic: lf_core.c (offline test:
 * test/loc_fallback_test.c).
 *
 * Config binaries/v3x/loc_fallback.ini ([loc_fallback], every key optional):
 *   enabled=1            0 = delete the generated files and do nothing
 *   also_fill_vanilla=0  1 = also fill vanilla English keys missing in the language
 *   language=            force a language id (l_braz_por, ...); default = pdx_settings.json
 * Console: `locfallback` (summary), `locfallback rebuild` (regenerate now; used at the next start). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "v3x.h"
#include "v3x_ini.h"
#include "lf_core.h"

#define TAG "loc_fallback"

V3X_EXPORT const char *v3x_plugin_name = "loc_fallback";
V3X_PLUGIN_MANIFEST("loc_fallback", "1.0.0", "V3X", "English fallback for localization keys missing in your language",
                    "*");

static const V3X_API *api;
static lf_paths g_paths;
static lf_cfg g_cfg;
static lf_result g_res;
static int g_have_docs;

static void slashes(char *s)
{
    for (; *s; s++) if (*s == '\\') *s = '/';
}

/* Documents/Paradox Interactive/Victoria 3: the shell's Documents folder (follows OneDrive/redirect),
 * else %USERPROFILE%/Documents, else %OneDrive%/Documents. */
static int find_docs(char *out, size_t cap)
{
    typedef HRESULT(WINAPI * sgfp_fn)(HWND, int, HANDLE, DWORD, LPWSTR);
    wchar_t w[MAX_PATH];
    char base[LF_PATH], t[LF_PATH + 64];
    HMODULE sh = LoadLibraryA("shell32.dll");
    sgfp_fn f = sh ? (sgfp_fn)(void *)GetProcAddress(sh, "SHGetFolderPathW") : NULL;
    if (f && f(NULL, 0x0005 /* CSIDL_PERSONAL */, NULL, 0, w) == 0 &&
        WideCharToMultiByte(CP_UTF8, 0, w, -1, base, sizeof base, NULL, NULL)) {
        snprintf(t, sizeof t, "%s/Paradox Interactive/Victoria 3/content_load.json", base);
        if (MultiByteToWideChar(CP_UTF8, 0, t, -1, w, MAX_PATH) && GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES) {
            snprintf(out, cap, "%s/Paradox Interactive/Victoria 3", base);
            slashes(out);
            return 1;
        }
    }
    for (int i = 0; i < 2; i++) {
        DWORD k = GetEnvironmentVariableW(i ? L"OneDrive" : L"USERPROFILE", w, MAX_PATH);
        if (!k || k >= MAX_PATH || !WideCharToMultiByte(CP_UTF8, 0, w, -1, base, sizeof base, NULL, NULL)) continue;
        snprintf(t, sizeof t, "%s/Documents/Paradox Interactive/Victoria 3/content_load.json", base);
        if (MultiByteToWideChar(CP_UTF8, 0, t, -1, w, MAX_PATH) && GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES) {
            snprintf(out, cap, "%s/Documents/Paradox Interactive/Victoria 3", base);
            slashes(out);
            return 1;
        }
    }
    return 0;
}

static void report(int to_console)
{
    int (*out)(const char *, ...) = to_console ? api->print : NULL;
    char line[320];
    const lf_result *r = &g_res;
    snprintf(line, sizeof line, "language %s: %s in %.0f ms - %d key(s) filled (%d enabled mods, %d roots, %d files%s)",
             r->lang, lf_status_name(r->status), r->ms, r->total_filled, r->nmods, r->nroots, r->nfiles,
             r->status == LF_WRITTEN || r->status == LF_NOTHING ? ", parsed" : "");
    if (out) out("%s", line); else api->log(TAG, "%s", line);
    if (r->status == LF_WRITTEN || r->status == LF_NOTHING) {
        snprintf(line, sizeof line, "  %d %s keys present, %d English candidate keys (%d duplicate definitions)",
                 r->present_keys, r->lang, r->english_keys, r->dup_english);
        if (out) out("%s", line); else api->log(TAG, "%s", line);
    }
    if (r->err[0]) { if (out) out("  ERROR: %s", r->err); else api->log(TAG, "ERROR: %s", r->err); }
    for (int i = 0; i < r->nmods; i++) {
        if (!r->mods[i].filled) continue;
        snprintf(line, sizeof line, "  %5d  %s (%s)", r->mods[i].filled, r->mods[i].name, r->mods[i].id);
        if (out) out("%s", line); else api->log(TAG, "%s", line);
    }
    if (r->vanilla_filled) { if (out) out("  %5d  vanilla", r->vanilla_filled); else api->log(TAG, "  %5d  vanilla", r->vanilla_filled); }
    if (r->total_filled) { if (out) out("  -> %s", r->out_path); else api->log(TAG, "-> %s", r->out_path); }
}

static void run(int force)
{
    lf_cfg c = g_cfg;
    c.force = force;
    if (!g_have_docs) {
        memset(&g_res, 0, sizeof g_res);
        g_res.status = LF_ERROR;
        snprintf(g_res.err, sizeof g_res.err, "Documents/Paradox Interactive/Victoria 3/content_load.json not found");
        return;
    }
    lf_run(&g_paths, &c, &g_res);
}

static void cmd(const char *args, void *user)
{
    (void)user;
    if (!_strnicmp(args, "rebuild", 7)) {
        run(1);
        api->print("loc_fallback: rebuilt (the game reads it at the next start)");
    }
    report(1);
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    V3xIni ini;
    v3x_ini_load_plugin(&ini, api, "loc_fallback");
    g_cfg.enabled = v3x_ini_bool(&ini, "loc_fallback", "enabled", 1);
    g_cfg.also_fill_vanilla = v3x_ini_bool(&ini, "loc_fallback", "also_fill_vanilla", 0);
    v3x_ini_str(&ini, "loc_fallback", "language", "", g_paths.lang, sizeof g_paths.lang);
    v3x_ini_free(&ini);

    snprintf(g_paths.game_root, sizeof g_paths.game_root, "%s..", api->game_dir());
    snprintf(g_paths.packs, sizeof g_paths.packs, "%scontent_packs", api->v3x_dir());
    snprintf(g_paths.cache, sizeof g_paths.cache, "%sloc_fallback.cache", api->v3x_dir());
    slashes(g_paths.game_root), slashes(g_paths.packs), slashes(g_paths.cache);
    g_have_docs = find_docs(g_paths.docs, sizeof g_paths.docs);
    if (!g_have_docs) lf_clean(g_paths.packs, NULL);

    run(0);
    report(0);
    char cp[LF_PATH];
    snprintf(cp, sizeof cp, "%splugins/content_packs.dll", api->v3x_dir());
    if (g_res.total_filled && GetFileAttributesA(cp) == INVALID_FILE_ATTRIBUTES)
        api->log(TAG, "WARNING: plugins/content_packs.dll not found - the fallback file will not be mounted");
    if (V3X_HAS(api, register_command))
        api->register_command("locfallback", "loc_fallback: English keys filled into your language; 'locfallback rebuild'", cmd, NULL);
    return 0;
}
