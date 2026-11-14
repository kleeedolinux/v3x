/* mod_menu: enable / disable V3X plugins in game, from a "V3X" button on the main menu.
 *
 * How it works (RE notes: docs/re/gui.md):
 *   1. At load (main thread, before CRT startup) it lists the DLLs in binaries/v3x/plugins, reads the
 *      [plugins] section of binaries/v3x/v3x.ini (0 = not loaded at the next start; the core
 *      applies it) and resolves the engine's GUI function registration from the static
 *      initializer that registers the vanilla GUI function "OpenWikiUrl" (byte-checked, see
 *      mm_resolve.c). It hooks that initializer; nothing else is hooked or patched.
 *   2. When the CRT runs that initializer, the detour calls the original, then registers the void
 *      GUI functions V3XToggle0..N-1 with the same engine function and in the same context as the
 *      engine registers its own. Only if every registration succeeded it writes the generated
 *      content pack binaries/v3x/content_packs/v3x_menu/gui/frontend/frontend_main.gui (the
 *      vanilla file + a V3X button + a plugin window, mm_gen_gui). content_packs mounts it later
 *      (vfs init runs after static initialization). Any failure = no file = vanilla main menu.
 *   3. Clicking a checkbox runs GetVariableSystem.Toggle('v3x_mm_t<i>') (the tick and the
 *      "restart required" label, pure GUI) and [V3XToggle<i>]; the engine calls our exec on the
 *      GUI thread, which only counts the click. on_frame (main thread) applies the counts and
 *      rewrites the [plugins] section of v3x.ini (temp file + rename). No game script runs.
 * All changes need a restart: plugins hook the engine at load, none supports unloading.
 * Console (API v2): modmenu | modmenu toggle <plugin>. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "v3x.h"
#include "v3x_safe.h"
#include "v3x_util.h"   /* v3x_file_write_atomic: the one temp + MoveFileEx writer */
#include "mm_logic.h"
#include "mm_resolve.h"

V3X_EXPORT const char *v3x_plugin_name = "mod_menu";
V3X_PLUGIN_MANIFEST("mod_menu", "0.5.0", "v3x", "enable or disable V3X plugins from the main menu", "release/1.15.1");

#define TAG "mod_menu"

static const V3X_API *api;

/* L10 (docs/security-audit.md): log lines carry mod / file / engine text; control characters,
 * C1 controls and bidi overrides are replaced (v3x_sanitize_line) so a crafted name cannot forge
 * v3x.log lines (parsed by doctor and the installer). */
static void slog(const char *fmt, ...) V3X_PRINTF(1, 2);
static void slog(const char *fmt, ...)
{
    if (!api) return;
    char raw[1536], clean[1536];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(raw, sizeof raw, fmt, ap);
    va_end(ap);
    v3x_sanitize_line(clean, sizeof clean, raw);
    api->log(TAG, "%s", clean);
}
static mm_plugin g_list[MM_NMAX];
static int g_n;
static mm_anchors g_a;
static char g_ini[MAX_PATH], g_gui_out[MAX_PATH], g_gui_in[MAX_PATH], g_pack[MAX_PATH];
static volatile LONG g_clicks[MM_NMAX];
static int g_registered = -1;     /* -1 detour not run yet, else number of functions registered */
static int g_gui_written;
static char g_mod_override[MAX_PATH]; /* enabled mod that has its own frontend_main.gui */

/* ------------------------------------------------------------------ engine ABI (MSVC x64) */

typedef struct msvc_str { char buf[16]; uint64_t size, cap; } msvc_str;     /* std::string, SSO only */
typedef struct pdx_arr { void *ptr; int32_t cap, count; void *alloc; } pdx_arr; /* CPdxArray */
typedef struct desc_in { msvc_str text; pdx_arr args; } desc_in;            /* 0x38 bytes */
typedef void (*ctor_fn)(pdx_arr *);
typedef void *(*desc_fn)(void *out, const desc_in *in);
typedef uint8_t (*reg_fn)(const msvc_str *name, void *is_valid, void *exec, void *a4, void *a5, int a6, void *desc);
typedef uintptr_t (*any_fn)(void *, void *, void *, void *, void *, void *, void *, void *);
static any_fn g_registrar_orig;

/* exec(void*, void*, args): only count the click; returns true like the engine's void adapter. */
#define MM_EXEC(i) static uintptr_t mm_exec_##i(void *a, void *b, void *c) \
    { (void)a, (void)b, (void)c; InterlockedIncrement(&g_clicks[i]); return 1; }
MM_EXEC(0) MM_EXEC(1) MM_EXEC(2) MM_EXEC(3) MM_EXEC(4) MM_EXEC(5) MM_EXEC(6) MM_EXEC(7)
MM_EXEC(8) MM_EXEC(9) MM_EXEC(10) MM_EXEC(11) MM_EXEC(12) MM_EXEC(13) MM_EXEC(14) MM_EXEC(15)
MM_EXEC(16) MM_EXEC(17) MM_EXEC(18) MM_EXEC(19) MM_EXEC(20) MM_EXEC(21) MM_EXEC(22) MM_EXEC(23)
MM_EXEC(24) MM_EXEC(25) MM_EXEC(26) MM_EXEC(27) MM_EXEC(28) MM_EXEC(29) MM_EXEC(30) MM_EXEC(31)
static void *const k_exec[MM_NMAX] = {
    mm_exec_0, mm_exec_1, mm_exec_2, mm_exec_3, mm_exec_4, mm_exec_5, mm_exec_6, mm_exec_7,
    mm_exec_8, mm_exec_9, mm_exec_10, mm_exec_11, mm_exec_12, mm_exec_13, mm_exec_14, mm_exec_15,
    mm_exec_16, mm_exec_17, mm_exec_18, mm_exec_19, mm_exec_20, mm_exec_21, mm_exec_22, mm_exec_23,
    mm_exec_24, mm_exec_25, mm_exec_26, mm_exec_27, mm_exec_28, mm_exec_29, mm_exec_30, mm_exec_31,
};

/* ------------------------------------------------------------------ files */

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (n >= 0 && n < (64 << 20)) ? (char *)malloc((size_t)n + 1) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[n] = 0; if (len) *len = (size_t)n; }
    return b;
}

static int write_file_atomic(const char *path, const char *data, size_t n)
{
    return v3x_file_write_atomic(path, data, n) == 0;
}

/* L11: every writer of the v3x inis (mod_menu, doctor fix, V3X-Setup) holds V3X_CONFIG_MUTEX
 * (sdk/v3x_util.h v3x_config_lock) around its read-modify-write. On a 5 s timeout the caller goes
 * on (and logs it): losing the user's change would be worse than a rare race. */


static void make_dirs(const char *dir)
{
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s", dir);
    for (char *s = p + 3; *s; s++)
        if (*s == '\\' || *s == '/') { char c = *s; *s = 0; CreateDirectoryA(p, NULL); *s = c; }
    CreateDirectoryA(p, NULL);
}

/* ------------------------------------------------------------------ registration (CRT init) */

static int register_all(void)
{
    ctor_fn ctor = (ctor_fn)g_a.array_ctor;
    desc_fn mk = (desc_fn)g_a.desc_make;
    reg_fn reg = (reg_fn)g_a.reg;
    int ok = 0;
    for (int i = 0; i < g_n; i++) {
        if (g_list[i].locked) continue;
        msvc_str name; memset(&name, 0, sizeof name);
        name.size = (uint64_t)snprintf(name.buf, sizeof name.buf, MM_FN_FMT, i);
        name.cap = 15;
        desc_in in; memset(&in, 0, sizeof in);
        in.text.cap = 15;
        ctor(&in.args);
        uint64_t out[16]; memset(out, 0, sizeof out);   /* description object, 0x38 bytes used */
        mk(out, &in);
        uint8_t r = reg(&name, NULL, k_exec[i], NULL, NULL, 0, out);  /* consumes `out` */
        if (in.args.ptr) slog("note: description array of %s was allocated (not freed)", name.buf);
        if (!r) { slog("engine refused GUI function %s", name.buf); return -1; }
        ok++;
    }
    return ok;
}

static void write_gui(void)
{
    size_t vn = 0;
    char *v = read_file(g_gui_in, &vn);
    if (!v) { slog("cannot read %s - no menu", g_gui_in); return; }
    size_t cap = vn + 256 * 1024;
    char *o = (char *)malloc(cap);
    const char *err = NULL;
    long n = o ? mm_gen_gui(v, g_list, g_n, o, cap, &err) : -1;
    if (n < 0) slog("frontend_main.gui not patched: %s - vanilla main menu", err ? err : "out of memory");
    else {
        char dir[MAX_PATH];
        snprintf(dir, sizeof dir, "%s\\gui\\frontend", g_pack);
        make_dirs(dir);
        g_gui_written = write_file_atomic(g_gui_out, o, (size_t)n);
        slog("%s %s (%ld bytes, %d plugins)", g_gui_written ? "wrote" : "FAILED to write", g_gui_out, n, g_n);
    }
    free(o);
    free(v);
}

static uintptr_t hk_registrar(void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h)
{
    uintptr_t r = g_registrar_orig(a, b, c, d, e, f, g, h);
    if (g_registered == -1) {
        g_registered = register_all();
        if (g_registered >= 0) {
            slog("registered %d GUI functions (" MM_FN_FMT "..)", g_registered, 0);
            write_gui();
        }
    }
    return r;
}

/* ------------------------------------------------------------------ toggles (main thread) */

static void save_ini(void)
{
    HANDLE lk = v3x_config_lock(5000);
    if (!lk) slog("v3x.ini lock (Local\\v3x_ini) busy for 5 s: saving anyway");
    char *old = read_file(g_ini, NULL);
    size_t cap = (old ? strlen(old) : 0) + 4096 + (size_t)g_n * 64;
    char *o = (char *)malloc(cap);
    size_t n = o ? mm_ini_rewrite(old, g_list, g_n, o, cap) : 0;
    int ok = n && write_file_atomic(g_ini, o, n);
    v3x_config_unlock(lk);
    slog("%s %s", ok ? "saved" : "FAILED to save", g_ini);
    free(o);
    free(old);
}

static void on_frame(void *user)
{
    (void)user;
    int changed = 0;
    for (int i = 0; i < g_n; i++) {
        LONG c = InterlockedExchange(&g_clicks[i], 0);
        if (!(c & 1) || g_list[i].locked) continue;
        g_list[i].enabled = !g_list[i].enabled;
        slog("%s -> %s at the next start", g_list[i].stem, g_list[i].enabled ? "enabled" : "disabled");
        changed = 1;
    }
    if (changed) save_ini();
}

static void cmd_modmenu(const char *args, void *user)
{
    (void)user;
    if (!strncmp(args, "toggle ", 7)) {
        const char *s = args + 7;
        while (*s == ' ') s++;
        for (int i = 0; i < g_n; i++)
            if (!_stricmp(s, g_list[i].stem)) {
                if (g_list[i].locked) { api->print("%s cannot be disabled here (the menu needs it)", s); return; }
                InterlockedIncrement(&g_clicks[i]);
                api->print("%s: toggle queued (applied on the next frame; the main menu tick shows it after a restart)", s);
                return;
            }
        api->print("unknown plugin '%s'", s);
        return;
    }
    api->print("mod_menu: GUI functions %d, menu file %s", g_registered, g_gui_written ? "written" : "NOT written");
    if (g_mod_override[0]) api->print("  menu hidden by mod %s (it overrides frontend_main.gui)", g_mod_override);
    for (int i = 0; i < g_n; i++)
        api->print("  %-16s now %-3s next start %-3s %s%s", g_list[i].stem, g_list[i].enabled_at_start ? "on" : "off",
                   g_list[i].enabled ? "on" : "off", g_list[i].desc, g_list[i].locked ? " [locked]" : "");
}

/* ------------------------------------------------------------------ load */

static uint8_t *text_section(size_t *n)
{
    uint8_t *base = (uint8_t *)api->image_base();
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
        if (!memcmp(s->Name, ".text", 5)) { *n = s->Misc.VirtualSize; return base + s->VirtualAddress; }
    return NULL;
}

/* Finds and checks the OpenWikiUrl registrar. Returns 1 and fills g_a. */
static int resolve(void)
{
    const uint8_t *name = (const uint8_t *)api->find_string("OpenWikiUrl");
    size_t tn = 0;
    uint8_t *text = text_section(&tn);
    if (!name || !text) { g_a.err = "string OpenWikiUrl / .text not found"; return 0; }
    g_a.err = "no vmovsd reference to OpenWikiUrl";
    for (int k = 0; k < 8; k++) {
        uint8_t *ref = mm_find_vmovsd_ref(text, tn, name, k);
        if (!ref) break;
        uint8_t *fn = (uint8_t *)api->function_start(ref);
        if (fn && ref - fn < 0x100 && mm_check_registrar(fn, 0x180, name, &g_a)) { g_a.registrar = fn; return 1; }
    }
    return 0;
}

static void list_plugins(void)
{
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%splugins\\*.dll", api->v3x_dir());
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        int before = g_n;
        g_n = mm_list_add(g_list, g_n, fd.cFileName);
        if (g_n == before && g_n == MM_NMAX) slog("more than %d plugins: %s not shown", MM_NMAX, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    mm_list_sort(g_list, g_n);
    char *ini = read_file(g_ini, NULL);
    for (int i = 0; i < g_n; i++)
        g_list[i].enabled = g_list[i].enabled_at_start = mm_ini_enabled(ini, g_list[i].stem);
    free(ini);
}

/* Paradox mods are mounted by the engine AFTER the content packs and above them (docs/re/vfs.md), so a
 * mod with its own gui/frontend/frontend_main.gui hides the generated menu. The mod list is not
 * readable through the VFS when the menu is generated (CRT init, VFS not built yet), so this reads
 * the launcher's content_load.json and only warns. */
static void check_mods(void)
{
    char env[MAX_PATH], path[MAX_PATH * 2];
    const char *roots[2] = {"USERPROFILE", "OneDrive"};
    char *json = NULL;
    for (int r = 0; r < 2 && !json; r++) {
        DWORD k = GetEnvironmentVariableA(roots[r], env, sizeof env);
        if (!k || k >= sizeof env) continue;
        snprintf(path, sizeof path, "%s\\Documents\\Paradox Interactive\\Victoria 3\\content_load.json", env);
        json = read_file(path, NULL);
    }
    if (!json) { slog("content_load.json not found - mod override check skipped"); return; }
    static char mods[256][MAX_PATH];
    int n = mm_json_mod_paths(json, mods[0], sizeof mods[0], 256);
    free(json);
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof path, "%s\\gui\\frontend\\frontend_main.gui", mods[i]);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
        snprintf(g_mod_override, sizeof g_mod_override, "%s", mods[i]);
        slog("WARNING: enabled mod %s has its own frontend_main.gui; mods are mounted above content packs, "
                      "so the V3X main-menu button will not show (use the console: modmenu toggle <plugin>)", mods[i]);
    }
    slog("%d enabled mods checked, %s", n, g_mod_override[0] ? "main menu overridden by a mod" : "none overrides the main menu");
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    snprintf(g_ini, sizeof g_ini, "%sv3x.ini", api->v3x_dir());
    snprintf(g_pack, sizeof g_pack, "%scontent_packs\\v3x_menu", api->v3x_dir());
    snprintf(g_gui_out, sizeof g_gui_out, "%s\\gui\\frontend\\frontend_main.gui", g_pack);
    snprintf(g_gui_in, sizeof g_gui_in, "%s..\\game\\gui\\frontend\\frontend_main.gui", api->game_dir());
    /* never leave last session's menu behind: it is written again only after registration worked */
    DeleteFileA(g_gui_out);

    list_plugins();
    check_mods();
    int have_packs = 0;
    for (int i = 0; i < g_n; i++) if (!_stricmp(g_list[i].stem, "content_packs") && g_list[i].enabled) have_packs = 1;
    slog("%d plugins listed, ini %s", g_n, g_ini);
    if (!have_packs) { slog("content_packs missing or disabled - the menu cannot be shown, nothing hooked"); return 0; }

    if (!resolve()) { slog("GUI function registration not found (%s) - vanilla main menu", g_a.err); return 0; }
    uintptr_t base = api->image_base();
    slog("registrar rva %llx, register %llx, desc %llx, array ctor %llx",
             (unsigned long long)((uintptr_t)g_a.registrar - base), (unsigned long long)((uintptr_t)g_a.reg - base),
             (unsigned long long)((uintptr_t)g_a.desc_make - base), (unsigned long long)((uintptr_t)g_a.array_ctor - base));
    int st = api->hook(g_a.registrar, (void *)hk_registrar, (void **)&g_registrar_orig);
    if (st) { slog("hook failed (%d) - vanilla main menu", st); return 0; }
    api->on_frame(on_frame, NULL);
    if (api->api_version >= 2 && api->struct_size >= offsetof(V3X_API, register_command) + sizeof(void *))
        api->register_command("modmenu", "mod_menu: list plugins; 'modmenu toggle <plugin>' flips it for the next start",
                              cmd_modmenu, NULL);
    return 0;
}
