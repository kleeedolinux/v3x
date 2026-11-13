/* mod_menu pure logic: v3x.ini [plugins] section, plugin list, frontend GUI generation.
 * No Windows or engine dependencies, so test/mod_menu_test.c compiles it directly
 * (#include "../plugins/mod_menu/mm_logic.c").
 *
 * v3x.ini contract (read by the core at the next start, written by mod_menu):
 *     [plugins]
 *     enhance=1
 *     sim_opt=0          <- 0 = the core does not LoadLibrary plugins/sim_opt.dll
 * Key = DLL file name without ".dll", case-insensitive. Missing key or any value other than
 * 0/off/no/false = enabled. Lines outside [plugins] are never changed by mod_menu. */
#ifndef MM_LOGIC_H
#define MM_LOGIC_H
#include <stddef.h>

#define MM_NMAX 32          /* plugins shown in the menu (and GUI functions V3XToggle0..31) */
#define MM_STEM 48

typedef struct mm_plugin {
    char stem[MM_STEM];    /* "enhance" (file name without .dll) */
    int enabled;           /* state that will be written to v3x.ini */
    int enabled_at_start;  /* state read at this start (what the generated GUI shows as unchanged) */
    int locked;            /* 1 = cannot be toggled in the menu (the menu itself depends on it) */
    const char *desc;      /* short description, never NULL */
} mm_plugin;

/* 1 if `stem` is enabled by ini text (NULL text = no ini = enabled), 0 if [plugins] stem=0. */
int mm_ini_enabled(const char *ini, const char *stem);

/* Rewrites ini text: every line outside [plugins] is copied unchanged (CR stripped), the first
 * [plugins] section is replaced by one line per plugin (stem=0/1) followed by the keys of unknown
 * plugins that were there before; later duplicate [plugins] sections are merged into it. Without
 * a [plugins] section one is appended. Returns the output length, 0 if `cap` is too small. */
size_t mm_ini_rewrite(const char *ini, const mm_plugin *p, int n, char *out, size_t cap);

/* Adds a plugin for directory entry `file` ("enhance.dll"). Skips non-.dll names, mod_menu itself,
 * duplicates and names that are too long. Returns the new count. */
int mm_list_add(mm_plugin *list, int n, const char *file);
/* Sorts by stem (case-insensitive). */
void mm_list_sort(mm_plugin *list, int n);
/* Built-in description of a known plugin ("" if unknown); *locked = 1 for content_packs. */
const char *mm_describe(const char *stem, int *locked);

/* Copies `in` for use inside a GUI raw_text string: only printable ASCII, without the characters
 * the GUI text parser treats specially ( " [ ] # $ @ \ { } ), max cap-1 chars. */
void mm_gui_text(const char *in, char *out, size_t cap);

/* Index of the '}' closing the block opened by the first '{' at or after `open`, skipping
 * "strings" and # comments; -1 if unbalanced. */
long mm_block_end(const char *text, long open);

/* Builds the patched frontend_main.gui from the vanilla text. Anchors (each must occur exactly
 * once, else -1 and *err says which):
 *   A1 `"[FrontEndMainView.OnSettings]"` with `button_group = {` on one of the 3 lines above it:
 *      the V3X button is inserted before that button_group (same row as Account / Settings);
 *   A2 `name = "mainmenu_panel_bottom"`: the V3X window is inserted as the last child of its block.
 * Fails if the text already contains "v3x_mod_menu". Returns output length or -1. */
long mm_gen_gui(const char *vanilla, const mm_plugin *p, int n, char *out, size_t cap, const char **err);

/* Launcher content_load.json: copies the "path" strings of the "enabledMods" array (JSON escapes
 * backslash-backslash, backslash-quote and backslash-slash decoded; paths with other escapes are skipped) into out[i] (each `each` bytes), in file
 * order. Returns the count (max `max`). */
int mm_json_mod_paths(const char *json, char *out, size_t each, int max);

/* Names used by the generated GUI (shared with the plugin and the test). */
#define MM_FN_FMT   "V3XToggle%d"     /* engine GUI function, registered by mod_menu */
#define MM_VAR_OPEN "v3x_mm_open"     /* GetVariableSystem key: window open */
#define MM_VAR_FMT  "v3x_mm_t%d"      /* GetVariableSystem key: plugin i toggled since start */

#endif
