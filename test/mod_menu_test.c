/* Offline test of plugins/mod_menu (no game run).
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -o $T/mmt.exe test/mod_menu_test.c
 *   $T/mmt.exe <empty temp dir> [path/to/game/gui/frontend/frontend_main.gui [path/to/victoria3.exe]]
 * A: v3x.ini [plugins] parse / rewrite.   B: plugin list.   J: content_load.json mod paths.   C: GUI generation on a synthetic file
 * (and on the real vanilla frontend_main.gui if given: anchors, balance, vanilla text preserved).
 * D: the whole plugin flow with a fake V3X_API and fake engine functions: v3x_plugin_load on a temp
 *    tree, the registrar detour registers V3XToggle<i> (SSO name layout, exec pointers, locked
 *    content_packs skipped), writes the content pack GUI, engine exec calls + on_frame rewrite
 *    v3x.ini. E (with the exe): the real anchors resolve on victoria3.exe mapped as an image (nothing
 *    executes) and the registrar is a CRT static initializer (pointer in .rdata).
 * Exit 0 = ALL OK. */
#include "../plugins/mod_menu/mm_logic.c"
#include "../plugins/mod_menu/mm_resolve.c"
#include "../plugins/mod_menu/mod_menu.c"
#include <stdarg.h>

static int fails;
static void check(int ok, const char *what) { printf("%-72s %s\n", what, ok ? "OK" : "FAIL"); fails += !ok; }

/* ------------------------------------------------------------------ A: ini */
static void part_a(void)
{
    check(mm_ini_enabled(NULL, "enhance") == 1, "A no ini -> enabled");
    const char *ini = "# V3X settings\r\nconsole=1\r\nsim_opt=0\r\n[plugins]\r\n  Sim_Opt = 0 \r\nenhance=1\r\nhello=off\r\n"
                      "[other]\r\nenhance=0\r\n";
    check(mm_ini_enabled(ini, "sim_opt") == 0, "A [plugins] key case-insensitive, spaces, CRLF -> 0");
    check(mm_ini_enabled(ini, "enhance") == 1, "A enhance=1 (and enhance=0 in [other] ignored)");
    check(mm_ini_enabled(ini, "hello") == 0, "A off = disabled");
    check(mm_ini_enabled("sim_opt=0\n", "sim_opt") == 1, "A key outside [plugins] ignored");
    check(mm_ini_enabled("[plugins]\nx=0\nx=1\n", "x") == 1, "A last occurrence wins");

    mm_plugin p[3]; int n = 0;
    n = mm_list_add(p, n, "enhance.dll"); n = mm_list_add(p, n, "sim_opt.dll"); n = mm_list_add(p, n, "hello.dll");
    p[0].enabled = 1; p[1].enabled = 0; p[2].enabled = 1;
    char out[2048];
    size_t k = mm_ini_rewrite(ini, p, n, out, sizeof out);
    check(k > 0 && strstr(out, "# V3X settings\nconsole=1\nsim_opt=0\n[plugins]\n") == out, "A rewrite keeps lines before [plugins]");
    check(strstr(out, "enhance=1\nsim_opt=0\nhello=1\n") != NULL, "A rewrite writes every plugin once");
    check(strstr(out, "[other]\nenhance=0\n") != NULL, "A rewrite keeps the following section");
    check(!strstr(out, "Sim_Opt") && !strstr(out, "\r"), "A old plugin lines and CR removed");
    check(mm_ini_enabled(out, "sim_opt") == 0 && mm_ini_enabled(out, "hello") == 1, "A rewrite round-trips");

    const char *dup = "console=0\n[plugins]\nfoo=0\n[plugins]\nbar=0\nenhance=0\n";
    k = mm_ini_rewrite(dup, p, n, out, sizeof out);
    check(k && strstr(out, "foo=0\nbar=0\n") && strstr(out, "[plugins]") == strrchr(out, '[') - 0 &&
          !strstr(strstr(out, "[plugins]") + 1, "[plugins]"), "A duplicate sections merged, unknown keys kept");
    check(mm_ini_enabled(out, "enhance") == 1 && mm_ini_enabled(out, "foo") == 0, "A merged section values");
    k = mm_ini_rewrite("console=1", p, n, out, sizeof out);
    check(k && !strncmp(out, "console=1\n\n[plugins]\n", 21), "A section appended when absent (no final newline)");
    k = mm_ini_rewrite(NULL, p, n, out, sizeof out);
    check(k && !strncmp(out, "[plugins]\n", 10), "A no ini -> new file with [plugins]");
    check(mm_ini_rewrite(ini, p, n, out, 40) == 0 && out[0] == 0, "A too small buffer -> 0");
}

/* ------------------------------------------------------------------ B: list */
static void part_b(void)
{
    mm_plugin p[MM_NMAX]; int n = 0;
    n = mm_list_add(p, n, "sim_opt.dll");
    n = mm_list_add(p, n, "Content_Packs.DLL");
    n = mm_list_add(p, n, "mod_menu.dll");
    n = mm_list_add(p, n, "readme.txt");
    n = mm_list_add(p, n, "SIM_OPT.dll");
    n = mm_list_add(p, n, "bad\"name.dll");
    n = mm_list_add(p, n, "alloc_tracker.dll");
    n = mm_list_add(p, n, ".dll");
    check(n == 3, "B skips mod_menu, non-dll, duplicates, unsafe names");
    mm_list_sort(p, n);
    check(!strcmp(p[0].stem, "alloc_tracker") && !strcmp(p[1].stem, "Content_Packs") && !strcmp(p[2].stem, "sim_opt"),
          "B sorted case-insensitively");
    check(p[1].locked == 1 && p[0].locked == 0 && p[2].desc[0], "B content_packs locked, descriptions found");
    int lk = 7;
    check(mm_describe("unknown_x", &lk)[0] == 0 && lk == 0, "B unknown plugin -> empty description");
    for (int i = 0; i < 40; i++) { char f[32]; snprintf(f, sizeof f, "p%02d.dll", i); n = mm_list_add(p, n, f); }
    check(n == MM_NMAX, "B capped at MM_NMAX");
    char t[64];
    mm_gui_text("a\"b[c]#d$e@f\\g{h}i\tj\xc3\xa9k", t, sizeof t);
    check(!strcmp(t, "abcdefghijk"), "B GUI text sanitized");
}

/* ------------------------------------------------------------------ C: GUI */
static int balanced(const char *t)
{
    long d = 0;
    for (long i = 0; t[i]; i++) {
        char c = t[i];
        if (c == '#') { while (t[i + 1] && t[i + 1] != '\n') i++; continue; }
        if (c == '"') { i++; while (t[i] && t[i] != '"') i++; if (!t[i]) return 0; continue; }
        if (c == '{') d++;
        else if (c == '}' && --d < 0) return 0;
    }
    return d == 0;
}

static const char k_synth[] =
    "widget = {\n\tname = \"mainmenu_panel_bottom\"\n\tsize = { 100% 100% }\n"
    "\twidget = {\n\t\thbox = {\n"
    "\t\t\tbutton_group = {\n\t\t\t\tonclick = \"[FrontEndMainView.OnLogin]\"\n\t\t\t}\n"
    "\t\t\tbutton_group = {\n\t\t\t\tonclick = \"[FrontEndMainView.OnSettings]\"\n\t\t\t\ttext = \"}#{\" # } comment {\n\t\t\t}\n"
    "\t\t}\n\t}\n\tdlc_collection = {\n\t}\n}\n\ntemplate x\n{\n}\n";

static void gui_case(const char *v, const char *label, const char *dir)
{
    mm_plugin p[MM_NMAX]; int n = 0;
    const char *files[] = {"enhance.dll", "content_packs.dll", "sim_opt.dll", "hello.dll", "x.dll"};
    for (int i = 0; i < 5; i++) n = mm_list_add(p, n, files[i]);
    mm_list_sort(p, n);   /* content_packs enhance hello sim_opt x */
    p[3].enabled_at_start = p[3].enabled = 0;
    size_t cap = strlen(v) + 256 * 1024;
    char *o = malloc(cap);
    const char *err = NULL;
    long k = mm_gen_gui(v, p, n, o, cap, &err);
    char w[160];
    snprintf(w, sizeof w, "C %s: generated (%s)", label, err ? err : "ok"); check(k > 0, w);
    if (k <= 0) { free(o); return; }
    snprintf(w, sizeof w, "C %s: output balanced", label); check(balanced(o), w);
    /* vanilla preserved: removing the two inserted blocks gives the input back */
    const char *b1 = strstr(o, "### V3X mod_menu: generated");
    long e1 = b1 ? mm_block_end(o, (long)(b1 - o)) : -1;
    const char *b1e = e1 > 0 ? o + e1 : NULL;   /* +3 below: "}\n\n" */
    const char *b2 = strstr(o, "\t### V3X mod_menu window");
    size_t ls1 = b1 ? (size_t)(b1 - o) : 0;
    while (ls1 && o[ls1 - 1] != '\n') ls1--;
    int same = 0;
    if (b1 && b1e && b2) {
        b1e += 3;
        long tail = mm_block_end(o, (long)(b2 - o));
        const char *b2e = tail > 0 ? o + tail + 2 : NULL;   /* "}\n" */
        if (b2e) {
            char *r = malloc(cap);
            size_t m = 0;
            memcpy(r + m, o, ls1); m += ls1;
            memcpy(r + m, b1e, (size_t)(b2 - b1e)); m += (size_t)(b2 - b1e);
            strcpy(r + m, b2e);
            same = !strcmp(r, v);
            free(r);
        }
    }
    snprintf(w, sizeof w, "C %s: vanilla text unchanged outside the 2 inserted blocks", label); check(same, w);
    snprintf(w, sizeof w, "C %s: button before Settings button_group", label);
    check(b1 && strstr(o, "[FrontEndMainView.OnSettings]") > b1 && strstr(o, "[FrontEndMainView.OnLogin]") < b1, w);
    snprintf(w, sizeof w, "C %s: functions for 4 toggleable, none for content_packs", label);
    check(strstr(o, "[V3XToggle1]") && strstr(o, "[V3XToggle4]") && !strstr(o, "[V3XToggle0]") && !strstr(o, "V3XToggle5"), w);
    snprintf(w, sizeof w, "C %s: disabled-at-start plugin uses Exists, enabled uses Not(Exists)", label);
    check(strstr(o, "checked = \"[GetVariableSystem.Exists('v3x_mm_t3')]\"") &&
          strstr(o, "checked = \"[Not(GetVariableSystem.Exists('v3x_mm_t1'))]\""), w);
    snprintf(w, sizeof w, "C %s: window inside mainmenu_panel_bottom, before its last brace", label);
    long open = (long)(strstr(o, "name = \"mainmenu_panel_bottom\"") - o) - 3;
    long end = mm_block_end(o, open);
    check(b2 && end > (long)(b2 - o) && strstr(o, "name = \"v3x_mod_menu\""), w);
    if (dir) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s/frontend_main_%s.gui", dir, label);
        FILE *f = fopen(path, "wb"); if (f) { fwrite(o, 1, (size_t)k, f); fclose(f); printf("  wrote %s\n", path); }
    }
    err = NULL;
    check(mm_gen_gui(o, p, n, malloc(cap), cap, &err) < 0 && err && strstr(err, "already"), "C patched input refused");
    check(mm_gen_gui(v, p, n, o, 200, &err) < 0, "C too small buffer refused");
    free(o);
}

static void part_json(void)
{
    char m[4][64];
    const char *j = "{ \"enabledUGC\": [ {\"path\": \"X:\\\\ugc\"} ], \"enabledMods\" : [ { \"path\" : \"D:\\\\a\\/b\" },"
                    " {\"name\": \"path\", \"path\":\"C:\\\\q\\\"r\"}, {\"path\": \"bad\\u0041\"}, {\"path\": \"\"} ],"
                    " \"disabledDLC\": [ {\"path\": \"Z:\\\\dlc\"} ] }";
    int n = mm_json_mod_paths(j, m[0], sizeof m[0], 4);
    check(n == 2 && !strcmp(m[0], "D:\\a/b") && !strcmp(m[1], "C:\\q\"r"),
          "J enabledMods paths only, escapes decoded, \\u / empty skipped");
    check(mm_json_mod_paths("{\"enabledMods\": []}", m[0], sizeof m[0], 4) == 0 && mm_json_mod_paths(NULL, m[0], 64, 4) == 0 &&
          mm_json_mod_paths("{\"enabledMods\": [{\"path\": \"unterminated", m[0], sizeof m[0], 4) == 0, "J empty / NULL / truncated -> 0");
    check(mm_json_mod_paths(j, m[0], 5, 4) == 0, "J path longer than buffer skipped");
}

static void part_c(const char *dir, const char *vanilla_path)
{
    gui_case(k_synth, "synthetic", dir);
    mm_plugin p[1]; int n = mm_list_add(p, 0, "a.dll");
    char o[64 * 1024]; const char *err;
    char *two = malloc(2 * sizeof k_synth); strcpy(two, k_synth); strcat(two, k_synth);
    check(mm_gen_gui(two, p, n, o, sizeof o, &err) < 0 && strstr(err, "A1"), "C anchor twice -> refused");
    free(two);
    check(mm_gen_gui("widget = { }", p, n, o, sizeof o, &err) < 0 && strstr(err, "A1"), "C anchor missing -> refused");
    const char *nobg = "widget = {\n\tname = \"mainmenu_panel_bottom\"\n\tx = {\n\t\tonclick = \"[FrontEndMainView.OnSettings]\"\n\t}\n}\n";
    check(mm_gen_gui(nobg, p, n, o, sizeof o, &err) < 0 && strstr(err, "button_group"), "C no button_group above A1 -> refused");
    const char *unb = "widget = {\n\tname = \"mainmenu_panel_bottom\"\n\tbutton_group = {\n\t\tonclick = \"[FrontEndMainView.OnSettings]\"\n\t}\n";
    check(mm_gen_gui(unb, p, n, o, sizeof o, &err) < 0 && strstr(err, "balanced"), "C unbalanced block -> refused");
    const char *outside = "button_group = {\n\tonclick = \"[FrontEndMainView.OnSettings]\"\n}\nwidget = {\n\tname = \"mainmenu_panel_bottom\"\n}\n";
    check(mm_gen_gui(outside, p, n, o, sizeof o, &err) < 0 && strstr(err, "inside"), "C A1 outside panel block -> refused");
    if (vanilla_path) {
        char *v = read_file(vanilla_path, NULL);
        check(v != NULL, "C read real frontend_main.gui");
        if (v) { check(balanced(v), "C real vanilla file balanced (scanner sanity)"); gui_case(v, "vanilla", dir); free(v); }
    }
}

/* ------------------------------------------------------------------ D: plugin flow with fakes */
static char g_root[MAX_PATH], g_bin[MAX_PATH], g_v3x[MAX_PATH];
static void f_log(const char *tag, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); printf("  [%s] ", tag); vprintf(fmt, ap); printf("\n"); va_end(ap); }
static int f_print(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); printf("  > "); int k = vprintf(fmt, ap); printf("\n"); va_end(ap); return k; }
static const char *f_game_dir(void) { return g_bin; }
static const char *f_v3x_dir(void) { return g_v3x; }

/* fake image: a synthetic registrar built from the real 1.15.1 bytes */
static uint8_t *g_img; static size_t g_img_n;
static uintptr_t f_base(void) { return (uintptr_t)g_img; }
static size_t f_size(void) { return g_img_n; }
static void *f_find_string(const char *s)
{
    size_t n = strlen(s) + 1;
    for (size_t i = 0; i + n <= g_img_n; i++) if (!memcmp(g_img + i, s, n) && (i == 0 || !g_img[i - 1])) return g_img + i;
    return NULL;
}
static size_t g_fn_off;
static void *f_function_start(const void *p) { (void)p; return g_img + g_fn_off; }
static void *g_hooked; static int g_hooks;
static uintptr_t fake_registrar(void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h)
{ (void)a, (void)b, (void)c, (void)d, (void)e, (void)f, (void)g, (void)h; return 0x5a; }
static int f_hook(void *t, void *det, void **orig) { (void)det; g_hooked = t; g_hooks++; *orig = (void *)fake_registrar; return 0; }
static v3x_frame_fn g_frame; static int f_on_frame(v3x_frame_fn fn, void *u) { (void)u; g_frame = fn; return 0; }
static v3x_command_fn g_cmd; static int f_reg_cmd(const char *n, const char *h, v3x_command_fn fn, void *u)
{ (void)n, (void)h, (void)u; g_cmd = fn; return 0; }

static int g_ctor_calls, g_reg_calls, g_reg_bad, g_reg_fail_at = -1;
static void *g_reg_exec[MM_NMAX]; static char g_reg_names[MM_NMAX][16];
static void fake_ctor(pdx_arr *a) { a->ptr = NULL; a->cap = a->count = 0; a->alloc = (void *)0x1234; g_ctor_calls++; }
static void *fake_desc(void *out, const desc_in *in) { memcpy(out, in, sizeof *in); return out; }
static uint8_t fake_reg(const msvc_str *name, void *iv, void *exec, void *a4, void *a5, int a6, void *desc)
{
    const desc_in *d = desc;
    if (name->cap != 15 || name->size != strlen(name->buf) || iv || a4 || a5 || a6 || d->text.cap != 15 ||
        d->text.size || d->args.alloc != (void *)0x1234) g_reg_bad++;
    if (g_reg_calls < MM_NMAX) { snprintf(g_reg_names[g_reg_calls], 16, "%s", name->buf); g_reg_exec[g_reg_calls] = exec; }
    return g_reg_calls++ != g_reg_fail_at;
}

static void put(const char *rel, const char *text)
{
    char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\%s", g_root, rel);
    FILE *f = fopen(p, "wb"); if (f) { fputs(text, f); fclose(f); }
}

static void build_fake_image(void)
{
    /* layout: [0x800] "OpenWikiUrl\0"; [0x1000] registrar; [0x2000] reg; [0x2100] ctor; [0x2200] desc; [0x2300] exec */
    g_img_n = 0x3000; g_img = calloc(1, g_img_n);
    memcpy(g_img + 0x800, "OpenWikiUrl", 12);
    static const uint8_t body[] = {   /* 1.15.1 bytes 0x2cd091.., rel32s patched below */
        0x48,0x8d,0x4d,0xe7,0xe8,0,0,0,0,                     /* +0x00 lea rcx,[rbp-19h]; call ctor */
        0xc5,0xfb,0x10,0x05,0,0,0,0,                          /* +0x09 vmovsd xmm0,[name] */
        0x48,0x8d,0x55,0xc7,0x48,0x8d,0x4d,0x1f,0xe8,0,0,0,0, /* +0x11 desc */
        0x48,0x89,0x44,0x24,0x30,0x89,0x5c,0x24,0x28,0x48,0x89,0x5c,0x24,0x20,0x45,0x33,0xc9,
        0x4c,0x8d,0x05,0,0,0,0,0x33,0xd2,0x48,0x8d,0x4d,0xff,0xe8,0,0,0,0, /* +0x1e reg */
        0xc3};
    size_t f = 0x1000; memcpy(g_img + f, body, sizeof body);
#define REL(at, to) do { int32_t d_ = (int32_t)((to) - ((at) + 4)); memcpy(g_img + (at), &d_, 4); } while (0)
    REL(f + 0x05, 0x2100); REL(f + 0x0d, 0x800); REL(f + 0x1a, 0x2200); REL(f + 0x1e + 20, 0x2300); REL(f + 0x1e + 31, 0x2000);
    memcpy(g_img + 0x2000, B_REG, sizeof B_REG);
    memcpy(g_img + 0x2100, B_CTOR, sizeof B_CTOR);
    memcpy(g_img + 0x2200, B_DESC, sizeof B_DESC);
    static const uint8_t ex[] = {0x48,0x83,0xec,0x38,0x4d,0x8b,0xc8,0x48,0x8d,0x0d,0,0,0,0,0xe8,0,0,0,0,0x48,0x83,0xc4,0x38,0xc3};
    memcpy(g_img + 0x2300, ex, sizeof ex);
    /* minimal PE headers so text_section() finds .text = whole fake image */
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_img; dos->e_lfanew = 0x40;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_img + 0x40);
    nt->FileHeader.NumberOfSections = 1; nt->FileHeader.SizeOfOptionalHeader = sizeof nt->OptionalHeader;
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    memcpy(s->Name, ".text", 5); s->VirtualAddress = 0; s->Misc.VirtualSize = (DWORD)g_img_n;
    g_fn_off = f;
}

static void part_d(const char *dir, const char *vanilla_path)
{
    snprintf(g_root, sizeof g_root, "%s\\flow", dir);
    char p[MAX_PATH];
    const char *dirs[] = {"", "\\binaries", "\\binaries\\v3x", "\\binaries\\v3x\\plugins", "\\game", "\\game\\gui", "\\game\\gui\\frontend"};
    for (int i = 0; i < 7; i++) { snprintf(p, sizeof p, "%s%s", g_root, dirs[i]); CreateDirectoryA(p, NULL); }
    snprintf(g_bin, sizeof g_bin, "%s\\binaries\\", g_root);
    snprintf(g_v3x, sizeof g_v3x, "%s\\binaries\\v3x\\", g_root);
    const char *dlls[] = {"enhance", "content_packs", "mod_menu", "sim_opt", "hello"};
    for (int i = 0; i < 5; i++) { snprintf(p, sizeof p, "binaries\\v3x\\plugins\\%s.dll", dlls[i]); put(p, "x"); }
    put("binaries\\v3x\\v3x.ini", "# V3X settings\nconsole=1\n[plugins]\nsim_opt=0\n");
    char *v = vanilla_path ? read_file(vanilla_path, NULL) : NULL;
    put("game\\gui\\frontend\\frontend_main.gui", v ? v : k_synth);
    free(v);
    put("stale.txt", "");
    /* stale generated file from a previous session must be deleted at load */
    snprintf(p, sizeof p, "%s\\binaries\\v3x\\content_packs", g_root); CreateDirectoryA(p, NULL);
    snprintf(p, sizeof p, "%s\\binaries\\v3x\\content_packs\\v3x_menu", g_root); make_dirs(p);
    snprintf(p, sizeof p, "%s\\binaries\\v3x\\content_packs\\v3x_menu\\gui\\frontend", g_root); make_dirs(p);
    put("binaries\\v3x\\content_packs\\v3x_menu\\gui\\frontend\\frontend_main.gui", "stale");

    build_fake_image();
    V3X_API fa; memset(&fa, 0, sizeof fa);
    fa.api_version = 3; fa.struct_size = sizeof fa;
    fa.log = f_log; fa.image_base = f_base; fa.image_size = f_size; fa.find_string = f_find_string;
    fa.function_start = f_function_start; fa.hook = f_hook; fa.on_frame = f_on_frame;
    fa.game_dir = f_game_dir; fa.v3x_dir = f_v3x_dir; fa.register_command = f_reg_cmd; fa.print = f_print;
    /* fake user profile: content_load.json with two mods, the second overrides frontend_main.gui */
    {
        const char *d2[] = {"\\user", "\\user\\Documents", "\\user\\Documents\\Paradox Interactive",
                            "\\user\\Documents\\Paradox Interactive\\Victoria 3", "\\modA", "\\modB", "\\modB\\gui",
                            "\\modB\\gui\\frontend"};
        for (int i = 0; i < 8; i++) { snprintf(p, sizeof p, "%s%s", g_root, d2[i]); CreateDirectoryA(p, NULL); }
        put("modB\\gui\\frontend\\frontend_main.gui", "widget = {}");
        char json[2048], ra[MAX_PATH * 2] = "", rb[MAX_PATH * 2] = "";
        snprintf(p, sizeof p, "%s\\modA", g_root);
        for (char *s = p, *o = ra; *s; s++) { if (*s == '\\') *o++ = '\\'; *o++ = *s; *o = 0; }
        snprintf(p, sizeof p, "%s\\modB", g_root);
        for (char *s = p, *o = rb; *s; s++) { if (*s == '\\') *o++ = '\\'; *o++ = *s; *o = 0; }
        snprintf(json, sizeof json, "{\n  \"enabledMods\": [\n    {\n      \"path\": \"%s\"\n    },\n    {\n      \"path\": \"%s\"\n    }\n  ],\n"
                 "  \"disabledDLC\": [],\n  \"enabledUGC\": []\n}\n", ra, rb);
        put("user\\Documents\\Paradox Interactive\\Victoria 3\\content_load.json", json);
        snprintf(p, sizeof p, "%s\\user", g_root);
        SetEnvironmentVariableA("USERPROFILE", p);
    }
    check(v3x_plugin_load(&fa) == 0, "D load returns 0");
    check(strstr(g_mod_override, "modB") && !strstr(g_mod_override, "modA"), "D enabled mod overriding frontend_main.gui detected");
    check(GetFileAttributesA(g_gui_out) == INVALID_FILE_ATTRIBUTES, "D stale generated GUI deleted at load");
    check(g_n == 4 && !strcmp(g_list[0].stem, "content_packs") && !strcmp(g_list[3].stem, "sim_opt"), "D 4 plugins listed (mod_menu excluded)");
    check(g_list[3].enabled_at_start == 0 && g_list[1].enabled_at_start == 1, "D ini state read (sim_opt=0)");
    check(g_hooks == 1 && g_hooked == g_img + g_fn_off, "D registrar resolved on the fake image and hooked");
    check(g_a.reg == g_img + 0x2000 && g_a.array_ctor == g_img + 0x2100 && g_a.desc_make == g_img + 0x2200 &&
          g_a.engine_exec == g_img + 0x2300, "D register/ctor/desc/exec resolved");
    check(g_frame && g_cmd, "D on_frame + console command registered");
    /* engine functions -> fakes (the fake image is not executable) */
    g_a.array_ctor = (uint8_t *)fake_ctor; g_a.desc_make = (uint8_t *)fake_desc; g_a.reg = (uint8_t *)fake_reg;
    check(hk_registrar(0, 0, 0, 0, 0, 0, 0, 0) == 0x5a, "D detour returns the original's value");
    check(g_registered == 3 && g_reg_calls == 3 && g_ctor_calls == 3 && g_reg_bad == 0, "D 3 functions registered, ABI fields as engine");
    check(!strcmp(g_reg_names[0], "V3XToggle1") && !strcmp(g_reg_names[2], "V3XToggle3") &&
          g_reg_exec[0] == k_exec[1] && g_reg_exec[2] == k_exec[3], "D names/exec match list indices (content_packs skipped)");
    hk_registrar(0, 0, 0, 0, 0, 0, 0, 0);
    check(g_reg_calls == 3, "D second detour call registers nothing");
    char *gui = read_file(g_gui_out, NULL);
    check(gui && strstr(gui, "[V3XToggle3]") && strstr(gui, "v3x_mod_menu") && g_gui_written, "D content pack GUI written");
    free(gui);
    /* clicks: sim_opt twice (no change) then once; hello once */
    uintptr_t (*ex)(void *, void *, void *) = (uintptr_t (*)(void *, void *, void *))k_exec[3];
    check(ex(0, 0, 0) == 1, "D exec returns true");
    ex(0, 0, 0);
    g_frame(NULL);
    char *ini = read_file(g_ini, NULL);
    check(ini && strstr(ini, "sim_opt=0") && !strstr(ini, "enhance=1"), "D even clicks -> no change, ini not rewritten");
    free(ini);
    ex(0, 0, 0);
    ((uintptr_t (*)(void *, void *, void *))k_exec[2])(0, 0, 0);
    g_frame(NULL);
    ini = read_file(g_ini, NULL);
    check(ini && mm_ini_enabled(ini, "sim_opt") == 1 && mm_ini_enabled(ini, "hello") == 0 &&
          mm_ini_enabled(ini, "enhance") == 1 && strstr(ini, "console=1\n"), "D clicks -> v3x.ini rewritten, other lines kept");
    free(ini);
    g_cmd("toggle Enhance", NULL); g_cmd("toggle content_packs", NULL); g_cmd("toggle nope", NULL);
    g_frame(NULL);
    ini = read_file(g_ini, NULL);
    check(ini && mm_ini_enabled(ini, "enhance") == 0 && mm_ini_enabled(ini, "content_packs") == 1, "D console toggle; locked stays on");
    free(ini);
    g_cmd("", NULL);
    snprintf(p, sizeof p, "%s.tmp", g_ini);
    check(GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES, "D no temp file left");

    /* failure: engine refuses a registration -> no GUI file */
    DeleteFileA(g_gui_out); g_registered = -1; g_gui_written = 0; g_reg_calls = 0; g_reg_fail_at = 1;
    hk_registrar(0, 0, 0, 0, 0, 0, 0, 0);
    check(g_registered == -1 && GetFileAttributesA(g_gui_out) == INVALID_FILE_ATTRIBUTES, "D refused registration -> no GUI file (vanilla menu)");
    /* failure: anchors broken -> nothing hooked */
    g_hooks = 0; g_n = 0; g_img[g_fn_off + 0x1e + 30] = 0x90;
    check(v3x_plugin_load(&fa) == 0 && g_hooks == 0, "D broken registrar shape -> no hook");
    /* content_packs disabled -> nothing hooked */
    g_img[g_fn_off + 0x1e + 30] = 0xE8; g_n = 0;
    put("binaries\\v3x\\v3x.ini", "[plugins]\ncontent_packs=0\n");
    v3x_plugin_load(&fa);
    check(g_hooks == 0, "D content_packs disabled -> no hook");
}

/* ------------------------------------------------------------------ E: real exe */
static void part_e(const char *exe)
{
    HMODULE h = LoadLibraryExA(exe, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    check(h != NULL, "E map victoria3.exe as image");
    if (!h) return;
    uint8_t *base = (uint8_t *)((uintptr_t)h & ~(uintptr_t)3);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    uint8_t *text = NULL; size_t tn = 0; uint8_t *rd = NULL; size_t rdn = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (!memcmp(s->Name, ".text", 5)) { text = base + s->VirtualAddress; tn = s->Misc.VirtualSize; }
        if (!memcmp(s->Name, ".rdata", 6)) { rd = base + s->VirtualAddress; rdn = s->Misc.VirtualSize; }
    }
    const uint8_t *name = NULL;
    for (size_t i = 0; rd && i + 12 <= rdn; i++) if (!memcmp(rd + i, "OpenWikiUrl", 12) && !rd[i - 1]) { name = rd + i; break; }
    check(name != NULL, "E string OpenWikiUrl");
    int hits = 0; uint8_t *ref = NULL;
    for (int k = 0; k < 8; k++) { uint8_t *r = mm_find_vmovsd_ref(text, tn, name, k); if (!r) break; if (!ref) ref = r; hits++; }
    printf("  vmovsd refs to OpenWikiUrl: %d, first rva %#llx\n", hits, ref ? (unsigned long long)(ref - base) : 0ull);
    check(hits == 1, "E exactly one vmovsd reference");
    IMAGE_DATA_DIRECTORY *ex = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    IMAGE_RUNTIME_FUNCTION_ENTRY *pd = (IMAGE_RUNTIME_FUNCTION_ENTRY *)(base + ex->VirtualAddress);
    size_t np = ex->Size / sizeof *pd; uint8_t *fn = NULL;
    uint32_t rva = ref ? (uint32_t)(ref - base) : 0;
    for (size_t i = 0; i < np; i++) if (pd[i].BeginAddress <= rva && rva < pd[i].EndAddress) { fn = base + pd[i].BeginAddress; break; }
    mm_anchors a; memset(&a, 0, sizeof a);
    int ok = fn && mm_check_registrar(fn, 0x180, name, &a);
#define R(x) ((x) ? (unsigned long long)((uint8_t *)(x) - base) : 0ull)
    printf("  registrar %#llx reg %#llx desc %#llx ctor %#llx exec %#llx %s\n", R(fn), R(a.reg), R(a.desc_make),
           R(a.array_ctor), R(a.engine_exec), a.err ? a.err : "");
    check(ok, "E registrar byte checks pass");
    check(R(fn) == 0x2cd050 && R(a.reg) == 0x796350 && R(a.desc_make) == 0x794c20 && R(a.array_ctor) == 0x678200 &&
          R(a.engine_exec) == 0x2090a00, "E RVAs match docs/re/gui.md (1.15.1)");
    /* registrar is a CRT initializer: its absolute address (preferred base, image not relocated) is in .rdata */
    uint64_t want = nt->OptionalHeader.ImageBase + R(fn); int ptrs = 0;
    for (size_t i = 0; rd && i + 8 <= rdn; i += 8) if (!memcmp(rd + i, &want, 8)) ptrs++;
    check(ptrs == 1, "E one .rdata pointer to the registrar (static initializer table)");
    /* the register function is shared: many registrars call it */
    int calls = 0;
    for (size_t i = 0; i + 5 <= tn; i++)
        if (text[i] == 0xE8) { int32_t d; memcpy(&d, text + i + 1, 4); if (text + i + 5 + d == a.reg) calls++; }
    printf("  call sites of register: %d\n", calls);
    check(calls >= 40, "E register is the shared void-function registrar (>= 40 callers)");
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: %s <empty temp dir> [frontend_main.gui [victoria3.exe]]\n", argv[0]); return 2; }
    part_a();
    part_b();
    part_json();
    part_c(argv[1], argc > 2 ? argv[2] : NULL);
    part_d(argv[1], argc > 2 ? argv[2] : NULL);
    if (argc > 3) part_e(argv[3]);
    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails ? 1 : 0;
}
