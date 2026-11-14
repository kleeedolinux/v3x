/* Fuzz harness: plugins/mod_menu/mm_logic.c - the v3x.ini [plugins] reader / rewriter, the GUI text
 * escaper, the block matcher, the frontend_main.gui patcher and the launcher JSON reader.
 * Invariants: the rewritten ini states exactly the requested enabled flags for every listed plugin;
 * outputs stay within their caps; mm_gui_text output has none of the GUI-special characters;
 * mm_block_end returns -1 or the index of a '}' inside the text. */
#include "v3x_fuzz.h"
#include "../../plugins/mod_menu/mm_logic.c"

static void fuzz_ini(const uint8_t *d, size_t n)
{
    const char *t = v3x_fuzz_cstr(d, n);
    static mm_plugin p[4];
    static const char *const stems[4] = {"console_plus", "fast_load", "content_packs", "hello"};
    for (int i = 0; i < 4; i++) {
        memset(&p[i], 0, sizeof p[i]);
        snprintf(p[i].stem, sizeof p[i].stem, "%s", stems[i]);
        p[i].enabled = n > (size_t)i ? d[i] & 1 : i & 1;
        p[i].desc = "";
        (void)mm_ini_enabled(t, stems[i]);
    }
    size_t cap = strlen(t) + 4096;
    char *o = (char *)malloc(cap);
    V3X_FUZZ_ASSERT(o != NULL);
    size_t k = mm_ini_rewrite(t, p, 4, o, cap);
    if (k) {
        V3X_FUZZ_ASSERT(k < cap && o[k] == 0 && strlen(o) == k);
        for (int i = 0; i < 4; i++) V3X_FUZZ_ASSERT(mm_ini_enabled(o, stems[i]) == p[i].enabled);
    }
    char small[64];
    (void)mm_ini_rewrite(t, p, 4, small, sizeof small);   /* too small: 0, never an overrun */
    free(o);
    char g[48];
    mm_gui_text(t, g, sizeof g);
    V3X_FUZZ_ASSERT(strlen(g) < sizeof g && !strpbrk(g, "\"[]#$@\\{}"));
    static char paths[8 * 128];
    int m = mm_json_mod_paths(t, paths, 128, 8);
    V3X_FUZZ_ASSERT(m >= 0 && m <= 8);
    for (int i = 0; i < m; i++) V3X_FUZZ_ASSERT(memchr(paths + i * 128, 0, 128) != NULL);
}
V3X_FUZZ_TARGET(mm_ini, fuzz_ini)
V3X_FUZZ_SEED(mm_ini, "\x01\x00\x01\x00console=0\r\n[plugins]\r\nconsole_plus = 0 ; off\r\nghost=1\r\n[log]\r\nlevel=info\r\n[PLUGINS]\r\nhello=1\r\n")
V3X_FUZZ_SEED(mm_ini, "{\"enabledMods\":[{\"path\":\"C:\\\\m\\\\a\"},{\"path\":\"x\\u0041\"}]}")

static void fuzz_gui(const uint8_t *d, size_t n)
{
    const char *t = v3x_fuzz_cstr(d, n);
    long len = (long)strlen(t);
    for (long open = 0; open < len && open < 64; open += 7) {
        long e = mm_block_end(t, open);
        V3X_FUZZ_ASSERT(e == -1 || (e >= open && e < len && t[e] == '}'));
    }
    static mm_plugin p[2];
    memset(p, 0, sizeof p);
    snprintf(p[0].stem, sizeof p[0].stem, "console_plus"); p[0].enabled = 1; p[0].desc = "Tab \"completion\" [x]";
    snprintf(p[1].stem, sizeof p[1].stem, "content_packs"); p[1].locked = 1; p[1].desc = "";
    size_t cap = (size_t)len * 2 + 65536;
    char *o = (char *)malloc(cap);
    V3X_FUZZ_ASSERT(o != NULL);
    const char *err = NULL;
    long k = mm_gen_gui(t, p, 2, o, cap, &err);
    if (k >= 0) V3X_FUZZ_ASSERT((size_t)k < cap && strlen(o) == (size_t)k && strstr(o, "v3x_mod_menu"));
    else V3X_FUZZ_ASSERT(err != NULL);
    free(o);
}
V3X_FUZZ_TARGET(mm_gui, fuzz_gui)
V3X_FUZZ_SEED(mm_gui, "window = {\n  name = \"mainmenu_panel_bottom\"\n  flowcontainer = {\n    button_group = {\n      # c { \n      button = { text = \"x}\" }\n"
                      "      onclick = \"[FrontEndMainView.OnSettings]\"\n    }\n  }\n}\n")
