/* mod_loader core: pure helpers (config, path rules, per-root file index, playset / error.log parsers),
 * no hooks, so test/mod_loader_test.c runs them offline. See docs/re/modloading.md. */
#ifndef ML_CORE_H
#define ML_CORE_H
#include <stddef.h>
#include <stdint.h>

enum { ML_OFF = 0, ML_MEASURE = 1, ML_ON = 2 };

/* <game>/binaries/v3x/mod_loader.ini, "key = value", '#'/';' comments, [sections] ignored. */
typedef struct {
    int mode;            /* off | measure (default: count probes, verify the index, change nothing) | on */
    int index_workshop;  /* 1: index roots under steamapps/workshop/content/529340/ (default 1) */
    int index_dlc;       /* 1: index <game>/game/dlc/<x> roots (default 1) */
    int index_local;     /* 1: index Documents/.../Victoria 3/mod/<x> roots (default 0: mods being edited) */
    int verify_every;    /* mode=on: every Nth skipped miss is re-checked with the real call; a mismatch
                            turns the cache off for the session (default 64, 0 = never) */
    int report;          /* 1: write v3x/mod_report.txt after every load transition (default 1) */
    int skip_fatal;      /* 1: hide mods that had >= fatal_min parse errors in the previous run (default 0 = warn) */
    int fatal_min;       /* default 1 */
    char skip_mods[512]; /* comma separated mod folder names (workshop ids) to hide (default empty) */
} ml_config;

void ml_config_defaults(ml_config *c);
/* Returns the number of bad lines; first error in err. Unknown keys are errors. */
int ml_config_parse(ml_config *c, const char *text, char *err, size_t errn);
/* 1 if `name` (case-insensitive) is in the comma list */
int ml_in_list(const char *list, const char *name);

/* PhysFS archive-relative name -> index key (ASCII lower-case). Returns the key length, or -1 when the
 * name must NOT be answered from an index because Windows could resolve it to a different on-disk
 * name: empty, leading '/', '\\' ':' '*' '?' '"' '<' '>' '|' '~', bytes < 0x20 or >= 0x80, empty /
 * "." / ".." components, components ending in '.' or ' ', DOS device names, too long. */
int ml_norm(const char *name, char *out, size_t cap);

/* Root path (PhysFS DIR opaque, e.g. "D:/x/y\\") -> lower-case, '/' separators, no trailing '/'. */
void ml_norm_root(const char *in, char *out, size_t cap);

enum { ML_ROOT_OTHER = 0, ML_ROOT_WORKSHOP, ML_ROOT_DLC, ML_ROOT_LOCAL };
/* game_norm / docs_norm: ml_norm_root of <game> (folder that contains game/) and of the
 * Documents/Paradox Interactive/Victoria 3 folder (may be ""). */
int ml_classify(const char *root_norm, const char *game_norm, const char *docs_norm);
const char *ml_class_name(int c);
/* last path component of a normalized root ("3385002128") */
const char *ml_root_leaf(const char *root_norm);

/* Hash set of index keys (files and their parent directories). Not thread-safe while building;
 * read-only afterwards. */
typedef struct {
    char *arena; size_t alen, acap;
    uint32_t *offs; uint64_t *hash; uint32_t cap, n;
    uint32_t nfiles, ndirs;
} ml_set;
#define ML_SET_DIR 1
void ml_set_free(ml_set *s);
/* adds key (and, for files and dirs, every parent dir). Returns 1 added, 0 present, -1 out of memory */
int ml_set_add(ml_set *s, const char *key, size_t len, int is_dir);
int ml_set_has(const ml_set *s, const char *key, size_t len);
/* iterate: returns next index >= i holding an entry, or -1; *key / *flags set */
int ml_set_next(const ml_set *s, int i, const char **key, int *flags);

/* content_load.json: calls cb(path_utf8) for every enabledMods[].path in order. Returns count or -1. */
int ml_parse_playset(const char *json, void (*cb)(const char *path, void *u), void *u);

/* error.log: one entry per "[hh:mm:ss][file.cpp:N]: msg" line (continuation lines belong to it).
 * Returns 1 and fills path (first game-relative path in the entry, lower-case, "" if none) and kind,
 * advances *pos; 0 at end. */
enum { ML_ERR_OTHER = 0, ML_ERR_PARSE, ML_ERR_DUPKEY, ML_ERR_LOC };
int ml_errlog_next(const char *text, size_t *pos, char *path, size_t pathn, int *kind);

#endif
