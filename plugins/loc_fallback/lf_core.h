/* loc_fallback core: Paradox localization parser, missing-key computation and fallback file writer.
 * No engine dependencies (only Win32 file APIs), so test/loc_fallback_test.c #includes lf_core.c.
 *
 * Pipeline (lf_run): language from pdx_settings.json -> search-path roots (clausewitz, jomini, game,
 * content packs, DLC, enabled mods in content_load.json order; later = higher priority, like the VFS)
 * -> every localization/ .yml, same relative path resolved to the highest root (VFS override) ->
 * parse files of the player's language (all roots) and English (mods; vanilla too with
 * also_fill_vanilla) -> English keys no root defines in the player's language are written to
 * <packs>/v3x_loc_fallback/localization/<folder>/zz_v3x_fallback_l_<lang>.yml. Inputs (paths, sizes,
 * mtimes, config) are hashed into the cache file; an unchanged hash with the output in place skips
 * parsing. docs/re/localization.md has the engine facts this relies on. */
#ifndef LF_CORE_H
#define LF_CORE_H
#include <stddef.h>
#include <stdint.h>

#define LF_PATH 1024
#define LF_MAXMODS 256
#define LF_PACK "v3x_loc_fallback"
#define LF_OUT_PREFIX "zz_v3x_fallback_"

/* Paradox language ids (game/localization/languages.yml); folder = id without "l_". */
extern const char *const lf_langs[];
#define LF_NLANGS 11
/* index into lf_langs or -1 */
int lf_lang_index(const char *id);

/* "language" string of pdx_settings.json (System block) into out; 0 if absent. */
int lf_lang_from_settings(const char *json, char *out, size_t cap);

/* content_load.json: "path" strings of the "enabledMods" array, in order (JSON escapes \\ \" \/ decoded,
 * \uXXXX to UTF-8). Returns count (max `max`). */
int lf_json_mod_paths(const char *json, char *out, size_t each, int max);

/* Value of the first top-level-ish "key": "string" pair in a JSON text (mod metadata name). */
int lf_json_str(const char *json, const char *key, char *out, size_t cap);

/* One parsed line. lang = current header ("l_english"), may be empty before any header. */
typedef struct lf_entry {
    const char *lang; size_t lang_len;
    const char *key; size_t key_len;
    const char *ver; size_t ver_len;   /* digits after ':' (may be empty) */
    const char *val; size_t val_len;   /* between the quotes, verbatim (escapes kept) */
} lf_entry;
typedef void (*lf_entry_fn)(void *user, const lf_entry *e);

/* Tolerant Paradox loc YAML parser: optional UTF-8 BOM, CRLF/LF, '#' comments (whole line or after the
 * value), header lines "l_xxx:" (any indent, nothing after), entries `key:0 "text"`, `key: "text"`,
 * `key:"text"`; the value ends at the last '"' followed only by blanks or a '#' comment (so unescaped and
 * \" quotes inside stay). Lines without a closing quote or with a space inside the key are skipped.
 * Returns the number of entries passed to fn. */
int lf_parse(const char *buf, size_t n, lf_entry_fn fn, void *user);

/* ---- key set (open addressing, keys are borrowed pointers) ----------------------------------- */
typedef struct lf_set { struct lf_slot { uint64_t h; const char *k; uint32_t n; int32_t v; } *s; size_t cap, count; } lf_set;
uint64_t lf_hash(const char *s, size_t n);
/* returns the slot value pointer (inserting v when absent; *added = 1 then). NULL on OOM. */
int32_t *lf_set_put(lf_set *set, const char *k, size_t n, int32_t v, int *added);
int32_t *lf_set_get(const lf_set *set, const char *k, size_t n);
void lf_set_free(lf_set *set);

/* ---- pipeline ---------------------------------------------------------------------------------- */
typedef struct lf_cfg {
    int enabled;            /* 0 = remove generated files and stop */
    int also_fill_vanilla;  /* 1 = also fill vanilla English keys missing in the language */
    int force;              /* 1 = ignore the cache */
} lf_cfg;

typedef struct lf_paths {
    char game_root[LF_PATH];  /* folder holding game/, jomini/, clausewitz/ (trailing slash optional) */
    char docs[LF_PATH];       /* Documents/Paradox Interactive/Victoria 3 (pdx_settings.json, content_load.json) */
    char packs[LF_PATH];      /* binaries/v3x/content_packs */
    char cache[LF_PATH];      /* cache file */
    char lang[32];            /* "" = read pdx_settings.json, else forced language id */
} lf_paths;

enum { LF_ERROR = -1, LF_DISABLED = 0, LF_ENGLISH, LF_CACHED, LF_WRITTEN, LF_NOTHING };

typedef struct lf_modsum { char id[64]; char name[96]; int filled, en_keys, lang_keys; } lf_modsum;

typedef struct lf_result {
    int status;
    char lang[32];
    char out_path[LF_PATH];
    int nmods, nroots, nfiles, nparsed, total_filled, present_keys, english_keys, dup_english;
    double ms;
    lf_modsum mods[LF_MAXMODS];   /* one per enabled mod, content_load.json order */
    int vanilla_filled;
    int vanilla_keys_cached;     /* 1 = vanilla keys of the language came from <cache>.keys */
    char err[160];
} lf_result;

int lf_run(const lf_paths *p, const lf_cfg *c, lf_result *r);
const char *lf_status_name(int st);

/* Removes every zz_v3x_fallback_l_*.yml this plugin may have written except for language `keep`
 * (NULL = all). Returns the number deleted. */
int lf_clean(const char *packs, const char *keep);

#endif
