/* v3x_script.h - ship game script / localization / GUI files from a V3X plugin (header-only, C).
 *
 * script_ext.dll (plugins/script_ext) exports one function, `v3x_script_ext`, returning the table
 * below. A plugin writes Jomini script files (common/scripted_effects/..., events/...,
 * common/on_actions/..., localization/english/..._l_english.yml, gui/...) into a content pack under
 * <game>/binaries/v3x/content_packs/<pack>/ from its v3x_plugin_load. content_packs mounts every pack
 * when the game's VFS initializes (after all plugins loaded, before any script database is read),
 * so the engine loads those files like the files of a mod. Guide: docs/scripting.md.
 *
 *   #include "v3x_script.h"
 *   const V3X_ScriptExt *sx = v3x_script_get(api);           // NULL: script_ext.dll not installed
 *   if (sx && sx->begin_pack("my_pack") == 0) {
 *       sx->write_file("my_pack", "common/scripted_effects/my_effects.txt", text, strlen(text));
 *       sx->end_pack("my_pack");
 *   }
 *
 * Thread / timing rules:
 *   - Call it from v3x_plugin_load (main thread, before the game starts). Files written later are
 *     not read by the engine in this session (script databases are loaded once at startup).
 *   - Works before or after script_ext's own v3x_plugin_load (it needs no engine anchors), but only
 *     once the loader has loaded script_ext.dll: v3x_script_get never loads it (load order: see
 *     there). script_ext finds the content_packs folder from its own location.
 *   - Not thread-safe: one plugin thread at a time (v3x_plugin_load calls are sequential).
 *
 * Version 2 adds (docs/scripting.md): run_effect_text / eval_trigger_text (run script text once at
 * runtime, main thread) and register_effect / register_trigger (new `v3x_*` keywords implemented in C,
 * registered like vanilla ones at engine start). Both are engine calls found through anchors and
 * fail closed (features() tells). NOT verified in a running game. */
#ifndef V3X_SCRIPT_H
#define V3X_SCRIPT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "v3x.h"

#define V3X_SCRIPT_VERSION 2   /* 2: features, run_effect_text, eval_trigger_text, register_effect, register_trigger */
#define V3X_SCRIPT_DLL "script_ext.dll"
#define V3X_SCRIPT_DLL_W L"script_ext.dll"

/* return codes */
#define V3X_SCRIPT_OK 0
#define V3X_SCRIPT_EARG (-1)     /* bad pack name / path (must be relative, no "..", see below) */
#define V3X_SCRIPT_EIO (-2)      /* file system error (GetLastError in v3x.log) */
#define V3X_SCRIPT_ESTATE (-3)   /* write_file without begin_pack for that pack */
/* version 2 */
#define V3X_SCRIPT_ETHREAD (-4)  /* run/eval: not called on the game's main thread */
#define V3X_SCRIPT_EOFF (-5)     /* feature unavailable: an engine anchor failed (see v3x.log) or script_ext not loaded yet */
#define V3X_SCRIPT_ESCRIPT (-6)  /* the engine rejected the text; `err` holds its message */
#define V3X_SCRIPT_ENOGAME (-7)  /* no game loaded / no player country */
#define V3X_SCRIPT_EBUSY (-8)    /* random scope held by another thread, or a run is already in progress (re-entrant) */
#define V3X_SCRIPT_ELATE (-9)    /* register_*: the engine already registered its script keywords (call it from v3x_plugin_load) */
#define V3X_SCRIPT_EFULL (-10)   /* register_*: 64 keywords of that kind already */
#define V3X_SCRIPT_EEXIST (-11)  /* register_*: that name is already registered */
#define V3X_SCRIPT_EOWNER (-12)  /* pack calls: the pack belongs to another plugin, or the caller is disabled /
                                    outside any module */

/* features() bits */
#define V3X_SCRIPT_F_RUN 1u         /* run_effect_text / eval_trigger_text can work (all engine anchors resolved) */
#define V3X_SCRIPT_F_REGISTER 2u    /* register_effect / register_trigger armed: keywords will be registered at engine start */
#define V3X_SCRIPT_F_REGISTERED 4u  /* the engine's static initialization has run and the keywords are registered */

/* Scope a native keyword runs in: a copy of the engine's 16-byte scope reference. type 5 = country,
 * id = the object's id (for a country: the id the console commands use). Other type numbers are not named. */
typedef struct V3X_ScriptScope {
    uint16_t type;
    uint16_t reserved[3];
    uint64_t id;
} V3X_ScriptScope;
#define V3X_SCOPE_COUNTRY 5

/* Native effect: `v3x_name = value` in script. `value` is the text after '=' (token or quoted string,
 * no quotes), "" if none. Called on WHATEVER thread the engine executes the effect on (tick worker
 * threads included): keep it short, thread-safe, no V3X API calls except log.
 * Lifetimes: `scope` and `value` are BORROWED for the duration of the call only (they point into the
 * engine's effect object / a stack copy): copy what you need, never keep the pointers. `user` is
 * yours and must stay valid for the rest of the session (keywords cannot be unregistered). */
typedef void (*v3x_script_effect_fn)(const V3X_ScriptScope *scope, const char *value, void *user);
/* Native trigger: `v3x_name = value`. Return non-zero = true. Same thread and lifetime rules as the
 * effect callback; may be evaluated many times per day by AI/tooltips: it must be pure and fast. */
typedef int (*v3x_script_trigger_fn)(const V3X_ScriptScope *scope, const char *value, void *user);

typedef struct V3X_ScriptExt {
    uint32_t struct_size;        /* sizeof(V3X_ScriptExt) of the DLL; check before newer fields */
    uint32_t version;            /* V3X_SCRIPT_VERSION */
    /* Starts (re)writing pack `pack` (letters, digits, '_', '-', '.', 1..63 chars, not starting with
     * '_' or '.'): creates the folder and deletes the files this function wrote into that pack in an
     * earlier session (listed in <pack>/v3x_script_ext.manifest), so removed files do not linger.
     * Files you did not write through script_ext are never deleted. */
    int (*begin_pack)(const char *pack);
    /* Writes one file: relpath relative to the pack root, '/' or '\\' separators, no "..", no drive,
     * no leading separator, max 200 chars. Missing folders are created; the file is replaced
     * atomically (temp file + rename). A *.yml file without a UTF-8 BOM gets one (the engine needs it
     * for localization). Returns V3X_SCRIPT_OK or an error above. Limit: at most 512 files per pack
     * are recorded in the pack manifest; files beyond that are written (v3x.log says so) but NOT deleted by the next
     * begin_pack (they can linger): keep a pack below 512 files (split into several packs). */
    int (*write_file)(const char *pack, const char *relpath, const void *data, size_t n);
    /* Writes the manifest of the pack. Returns the number of files written, or an error. */
    int (*end_pack)(const char *pack);
    /* Absolute content_packs folder with trailing backslash ("" if unknown). */
    const char *(*packs_dir)(void);

    /* ---- version 2 (check V3X_SCRIPT_HAS(t, field) first) ---- */
    /* Bit mask of V3X_SCRIPT_F_*; meaningful once script_ext's own v3x_plugin_load has run (call it from
     * on_frame / a command, not from your own v3x_plugin_load: 0 there if script_ext loads after you). */
    uint32_t (*features)(void);
    /* Runs effect script text (e.g. "add_treasury = 1000") once, root scope = the player's country, through
     * the engine's Script Runner. MAIN THREAD only (api->run_on_main). Returns V3X_SCRIPT_OK or an error;
     * `err` (may be NULL) gets the reason. OK means "the engine ran it": execution errors (wrong scope,
     * missing field) are logged by the engine in game.log, not returned. Not for multiplayer. */
    int (*run_effect_text)(const char *text, char *err, size_t errlen);
    /* Evaluates trigger text (e.g. "is_ai = no") once, root = the player's country; *result = 0/1. Main thread. */
    int (*eval_trigger_text)(const char *text, int *result, char *err, size_t errlen);
    /* New script keywords written in C. Only from v3x_plugin_load (before the engine starts). `name` must be
     * v3x_[a-z0-9_]+ (max 63 chars). Returns V3X_SCRIPT_OK (queued: it is registered when the engine runs its
     * static initializers; check features() & V3X_SCRIPT_F_REGISTERED later) or an error. */
    int (*register_effect)(const char *name, v3x_script_effect_fn cb, void *user);
    int (*register_trigger)(const char *name, v3x_script_trigger_fn cb, void *user);
} V3X_ScriptExt;

/* 1 if the table (struct_size) has `field` (version 2 fields). */
#define V3X_SCRIPT_HAS(t, field)     ((t) && (t)->struct_size >= offsetof(V3X_ScriptExt, field) + sizeof((t)->field) && (t)->field)

typedef const V3X_ScriptExt *(*v3x_script_ext_fn)(void);

/* script_ext's table, or NULL when script_ext.dll is not loaded (not installed, disabled with
 * [plugins] script_ext = 0, skipped by the crash guard or the build gate) or its table is too small.
 *
 * It NEVER loads the DLL itself (audit N4/N11: that would bypass the loader's enable switch, crash
 * guard, manifest and build gate). It looks for the module loaded from the SAME folder as the calling
 * plugin (<v3x>/plugins/script_ext.dll: full-path match with the caller's own module path in UTF-16,
 * no ANSI loss), so an unrelated "script_ext.dll" elsewhere in the process is never used.
 *
 * Load order: the loader loads plugins in directory order (FindFirstFile: alphabetical on NTFS). A
 * plugin that sorts BEFORE script_ext gets NULL in its own v3x_plugin_load: call v3x_script_get
 * again later (first on_frame / a command). Keyword registration (register_effect /
 * register_trigger) must happen before the engine's static initialization, pack files before the
 * game's VFS initializes (V3X_SCRIPT_ELATE / docs/scripting.md).
 *
 * Table check (N11): struct_size must cover every version 1 field (through packs_dir) and those four
 * functions must be non-NULL; check version 2 fields with V3X_SCRIPT_HAS. The table belongs to
 * script_ext.dll and stays valid for the session (plugins are never unloaded). */
static const V3X_ScriptExt *v3x_script_get(const V3X_API *api)
{
    (void)api;   /* kept for source compatibility: the folder comes from the calling module */
    HMODULE self = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(const void *)&v3x_script_get, &self) || !self)
        return NULL;
    static const wchar_t dll[] = V3X_SCRIPT_DLL_W;
    wchar_t p[1024];
    const DWORD cap = (DWORD)(sizeof p / sizeof p[0]);
    DWORD n = GetModuleFileNameW(self, p, cap);
    if (n == 0 || n >= cap) return NULL;
    while (n > 0 && p[n - 1] != L'\\' && p[n - 1] != L'/') n--;   /* keep the folder incl. its separator */
    if (n == 0 || n + sizeof dll / sizeof dll[0] > cap) return NULL;
    memcpy(p + n, dll, sizeof dll);
    HMODULE h = GetModuleHandleW(p);   /* full path: only the module loaded from exactly this file */
    if (!h) return NULL;
    v3x_script_ext_fn f = (v3x_script_ext_fn)(void *)GetProcAddress(h, "v3x_script_ext");
    const V3X_ScriptExt *t = f ? f() : NULL;
    if (!t || t->struct_size < offsetof(V3X_ScriptExt, packs_dir) + sizeof(t->packs_dir)) return NULL;
    if (!t->begin_pack || !t->write_file || !t->end_pack || !t->packs_dir) return NULL;
    return t;
}

#endif
