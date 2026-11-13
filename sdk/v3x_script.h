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
 *   - Works before or after script_ext's own v3x_plugin_load (it needs no engine anchors): it finds
 *     the content_packs folder from the location of script_ext.dll (<game>/binaries/v3x/plugins/).
 *   - Not thread-safe: one plugin thread at a time (v3x_plugin_load calls are sequential).
 *
 * What it does NOT do (yet): run effect/trigger text at runtime, or add new script keywords
 * implemented in C. See docs/scripting.md "Not available". */
#ifndef V3X_SCRIPT_H
#define V3X_SCRIPT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "v3x.h"

#define V3X_SCRIPT_VERSION 1
#define V3X_SCRIPT_DLL "script_ext.dll"

/* return codes */
#define V3X_SCRIPT_OK 0
#define V3X_SCRIPT_EARG (-1)     /* bad pack name / path (must be relative, no "..", see below) */
#define V3X_SCRIPT_EIO (-2)      /* file system error (GetLastError in v3x.log) */
#define V3X_SCRIPT_ESTATE (-3)   /* write_file without begin_pack for that pack */

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
     * for localization). Returns V3X_SCRIPT_OK or an error above. */
    int (*write_file)(const char *pack, const char *relpath, const void *data, size_t n);
    /* Writes the manifest of the pack. Returns the number of files written, or an error. */
    int (*end_pack)(const char *pack);
    /* Absolute content_packs folder with trailing backslash ("" if unknown). */
    const char *(*packs_dir)(void);
} V3X_ScriptExt;

typedef const V3X_ScriptExt *(*v3x_script_ext_fn)(void);

/* script_ext's table, or NULL. Loads <v3x_dir>/plugins/script_ext.dll if it is not loaded yet (the
 * loader then finds it already loaded and still calls its v3x_plugin_load). */
static const V3X_ScriptExt *v3x_script_get(const V3X_API *api)
{
    HMODULE h = GetModuleHandleA(V3X_SCRIPT_DLL);
    if (!h && api && api->v3x_dir) {
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%splugins\\%s", api->v3x_dir(), V3X_SCRIPT_DLL);
        if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) h = LoadLibraryA(p);
    }
    v3x_script_ext_fn f = h ? (v3x_script_ext_fn)(void *)GetProcAddress(h, "v3x_script_ext") : NULL;
    const V3X_ScriptExt *t = f ? f() : NULL;
    return t && t->struct_size >= offsetof(V3X_ScriptExt, packs_dir) ? t : NULL;
}

#endif
