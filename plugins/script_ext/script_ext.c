/* script_ext: lets plugins ship game script, localization and GUI files (layer b of the script SDK).
 *
 * Exports `const V3X_ScriptExt *v3x_script_ext(void)` (sdk/v3x_script.h). Other plugins call it from
 * their v3x_plugin_load to write files into binaries/v3x/content_packs/<pack>/, which content_packs
 * mounts when the VFS initializes. No engine code is hooked, patched or called by this plugin.
 * Console: `scriptext` lists the packs written this session.
 * Engine RE for running script / new keywords: docs/re/script.md (not implemented, docs/scripting.md). */
#include <stdarg.h>
#include "v3x.h"
#include "sx_pack.h"

V3X_EXPORT const char *v3x_plugin_name = "script_ext";
V3X_PLUGIN_MANIFEST("script_ext", "0.1.0", "V3X", "ship script/loc/gui files from plugins via content packs", "*");

#define TAG "script_ext"
static const V3X_API *api;
static HMODULE g_self;

static const V3X_ScriptExt g_table = {
    (uint32_t)sizeof(V3X_ScriptExt), V3X_SCRIPT_VERSION, sx_begin_pack, sx_write_file, sx_end_pack, sx_packs_dir,
};

V3X_EXPORT const V3X_ScriptExt *v3x_script_ext(void)
{
    if (!sx_root[0]) sx_root_from_module(g_self);
    return &g_table;
}

static void log_fwd(const char *fmt, ...)
{
    if (!api) return;
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    api->log(TAG, "%s", b);
}

static void cmd_scriptext(const char *args, void *user)
{
    (void)args; (void)user;
    api->print("script_ext: content_packs dir %s", sx_root[0] ? sx_root : "(unknown)");
    int any = 0;
    for (int i = 0; i < SX_MAX_PACKS; i++) {
        if (!sx_packs[i].name[0]) continue;
        any = 1;
        api->print("  %s: %d file(s)%s", sx_packs[i].name, sx_packs[i].n, sx_packs[i].active ? " (end_pack not called)" : "");
    }
    if (!any) api->print("  no pack written this session");
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) g_self = h;   /* only remembers the handle */
    return TRUE;
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    sx_logf = log_fwd;
    if (!sx_root[0]) sx_root_from_module(g_self);
    if (!sx_root[0]) {   /* DLL not in binaries/v3x/plugins: use the loader's folder */
        snprintf(sx_root, sizeof sx_root, "%scontent_packs\\", api->v3x_dir());
        CreateDirectoryA(sx_root, NULL);
    }
    if (V3X_HAS(api, register_command)) api->register_command("scriptext", "list content packs written by plugins", cmd_scriptext, NULL);
    api->log(TAG, "ready, content packs in %s", sx_root);
    return 0;
}
