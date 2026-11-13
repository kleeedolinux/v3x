/* lazy_mesh core: pure logic (config, eager/defer rules, per-type once-state table, output format).
 * No hooks and no game access, so test/lazy_mesh_test.c can run it offline. Windows-only (Interlocked*). */
#ifndef LM_CORE_H
#define LM_CORE_H
#include <stddef.h>
#include <stdint.h>

enum { LM_MODE_OFF = 0, LM_MODE_MEASURE = 1, LM_MODE_LAZY = 2 };

#define LM_MAX_LIST 16
#define LM_LIST_STR 64

/* <game>/binaries/v3x/lazy_mesh.ini, "key = value" lines, '#' or ';' comments. Lists are comma separated. */
typedef struct {
    int mode;                 /* off | measure | lazy (default off) */
    int skip_debug;           /* lazy: defer every type whose mesh path is under gfx/models/debug/ (default 1) */
    int defer_all;            /* lazy: defer every type that no eager rule keeps (default 0) */
    int n_defer_dirs;         /* lazy: defer types whose mesh path contains one of these (normalised, lower case) */
    char defer_dirs[LM_MAX_LIST][LM_LIST_STR];
    int n_eager_contains;     /* never defer a type whose NAME contains one of these (default "bridge") */
    char eager_contains[LM_MAX_LIST][LM_LIST_STR];
    char eager_file[LM_LIST_STR];   /* v3x/<file>: one type name per line (first token), never deferred */
    int prefetch_after_s;     /* lazy: 0 = off; N = N s after the startup pass, load the remaining deferred
                               * types one by one on a background thread (safety net, gives back the RAM) */
    int wait_timeout_ms;      /* max wait for a type another thread is loading (default 60000) */
    int log_loads;            /* log every on-demand load (first 400) (default 1) */
    int startup_from_report;  /* lazy: types the previous session's report shows as first requested by entity
                               * parsing (entity_blendshape / entity_override) are loaded eagerly in the
                               * parallel pass; otherwise they load serially right after it (default 1) */
    int prefetch_on_game;     /* lazy: on the first load_transition to "Game", load the deferred types that the
                               * previous report lists as used, most-used first, on one low-priority thread (default 1) */
    int prefetch_all;         /* prefetch also deferred types never used before (default 0: keeps the RAM saving) */
    int prefetch_gap_ms;      /* pause between two prefetched types (default 20) */
    int merge_report;         /* measure/lazy: merge this session's usage into the previous lazy_mesh_used.txt
                               * instead of overwriting it (default 1) */
} lm_config;

void lm_config_defaults(lm_config *c);
/* Returns the number of bad lines; the first error is described in err. Unknown keys are errors. */
int lm_config_parse(lm_config *c, const char *text, char *err, size_t errn);

/* ---- eager name set (open addressing, owns its strings) */
typedef struct { char **slot; uint32_t cap, n; } lm_nameset;
int lm_nameset_add(lm_nameset *s, const char *name, size_t len);   /* 1 added, 0 dup/empty, -1 oom */
int lm_nameset_has(const lm_nameset *s, const char *name);
void lm_nameset_free(lm_nameset *s);
/* Lines: first token (up to whitespace/tab) is a type name; '#' and ';' lines are comments.
 * Returns the number of names added. */
int lm_nameset_parse(lm_nameset *s, const char *text);

/* "Gfx\\Models\\Debug\\x.mesh" -> "gfx/models/debug/x.mesh" (lower case, '/' separators, no leading "./") */
void lm_norm_path(const char *in, char *out, size_t cap);

/* Decision for one mesh type at the startup pass. Returns LM_EAGER_* (keep loading now) or LM_DEFER_*. */
enum {
    LM_EAGER_MODE = 0,      /* mode is not lazy */
    LM_EAGER_LISTED,        /* name is in the eager file */
    LM_EAGER_RULE,          /* name contains an eager_contains token */
    LM_EAGER_DEFAULT,       /* no defer rule matched */
    LM_EAGER_PREV,          /* previous session: first requested during entity parsing (startup_from_report) */
    LM_DEFER_DEBUG = 16,    /* skip_debug and path under gfx/models/debug/ */
    LM_DEFER_DIR,           /* path contains a defer_dirs entry */
    LM_DEFER_ALL,           /* defer_all */
};
int lm_decide(const lm_config *c, const lm_nameset *eager, const lm_nameset *prev_startup, const char *name, const char *path);
#define LM_IS_DEFER(d) ((d) >= LM_DEFER_DEBUG)

/* ---- per-type table, keyed by the CMeshType pointer. Inserts are lock-free (CAS on the key),
 * so the engine's 8 loader threads can insert concurrently. Entries are never removed. */
enum { LM_ST_EMPTY = 0, LM_ST_DEFERRED = 1, LM_ST_LOADING = 2, LM_ST_DONE = 3 };
enum { LM_SITE_NONE = 0, LM_SITE_CTOR, LM_SITE_RDATA, LM_SITE_BLEND, LM_SITE_PREFETCH, LM_SITE_CONSOLE, LM_SITE_COUNT };
const char *lm_site_name(int site);

typedef struct {
    volatile int64_t key;        /* type pointer, 0 = free */
    volatile int32_t state;      /* LM_ST_* */
    volatile int32_t owner;      /* thread id while LOADING */
    volatile int32_t uses;       /* choke-point requests seen */
    volatile int32_t first_site; /* LM_SITE_* of the first request */
    int32_t decision;            /* lm_decide() result at the pass */
    int32_t phase;               /* load phase index at the first request (-1 = none yet) */
    double first_ms;             /* ms (plugin clock) of the first request, 0 = none */
    float pass_ms;               /* Load time during the startup pass (0 if deferred) */
    float lazy_ms;               /* Load time on demand (0 if never) */
    char name[72];
    char path[112];
} lm_entry;

typedef struct { lm_entry *e; uint32_t cap; volatile int32_t n, full; } lm_table;

int lm_table_init(lm_table *t, uint32_t cap_pow2);   /* 0 ok, -1 oom */
void lm_table_free(lm_table *t);
lm_entry *lm_table_find(const lm_table *t, const void *key);
/* Insert or find. *created = 1 if this call claimed the slot (fill name/path/state after).
 * Returns NULL when the table is full (caller must then load eagerly). */
lm_entry *lm_table_insert(lm_table *t, const void *key, int *created);

/* Make sure a deferred type is loaded before the caller uses it.
 *   UNTRACKED  key not in the table: engine-managed, nothing to do
 *   READY      already loaded (or eager)
 *   LOADED     this call ran load(key) (state DEFERRED -> LOADING -> DONE)
 *   WAITED     another thread was loading it; returned after it finished
 *   REENTRANT  this thread is the one loading it (call from inside load) -> do not wait
 *   TIMEOUT    waited timeout_ms for another thread; returns anyway (logged by the caller) */
enum { LM_R_TIMEOUT = -1, LM_R_UNTRACKED = 0, LM_R_READY, LM_R_LOADED, LM_R_WAITED, LM_R_REENTRANT };
typedef void (*lm_load_fn)(void *key, void *user);
int lm_ensure(lm_table *t, void *key, int32_t tid, lm_load_fn load, void *user, unsigned timeout_ms, lm_entry **out);

/* Record a choke-point request (measure + lazy statistics). first_ms/site/phase are set once. */
void lm_note_use(lm_entry *e, int site, double now_ms, int phase);

/* Output file: used types as data lines (name first, so the file can be used as the eager file),
 * unused ones as "# unused" comment lines. Returns bytes written (NUL-terminated, truncated if needed). */
struct lm_prev;
/* prev (may be NULL): the previous report as loaded at startup. The written file is the UNION: a type used in
 * this session gets uses = previous + current (an entity-parse first site from before is kept, so the
 * startup set never shrinks); a type used only before is carried over as a data line (decision "carried"). */
typedef struct { int mode; const char *version; double pass_end_ms; const char *const *phases; int n_phases;
                 const struct lm_prev *prev; } lm_report_info;
size_t lm_format_report(const lm_table *t, const lm_report_info *info, char *out, size_t cap);

/* MSVC x64 std::string {union{char buf[16]; char *ptr;}; size_t size; size_t cap;} -> out (truncated).
 * `readable` checks memory before every dereference (may be NULL = trust). Returns length or -1. */
typedef int (*lm_readable_fn)(const void *p, size_t n);
int lm_msvc_str(const void *s, char *out, size_t cap, lm_readable_fn readable);

/* 1 if code[off..] matches an IDA-style pattern ("48 8B ?? 05"), never reads past avail. */
int lm_match(const uint8_t *code, size_t avail, size_t off, const char *pat);

/* ---- previous session's report (lazy_mesh_used.txt data lines). Sorted by name for lookup. */
typedef struct { char name[72]; int uses; int site; } lm_prev_item;
typedef struct lm_prev { lm_prev_item *v; int n; } lm_prev;
int lm_site_from_name(const char *s);                  /* "mesh_object" -> LM_SITE_CTOR, unknown -> LM_SITE_NONE */
int lm_prev_parse(lm_prev *p, const char *text);      /* returns the number of data lines read */
const lm_prev_item *lm_prev_find(const lm_prev *p, const char *name);
void lm_prev_free(lm_prev *p);
/* names whose first site was entity parsing (blend shape / override) -> set; returns how many */
int lm_prev_startup_set(const lm_prev *p, lm_nameset *out);

/* ---- per-session on-demand load statistics (caller serialises lm_stats_add) */
typedef struct {
    int n[8]; double ms[8];              /* per LM_SITE_* */
    int main_n; double main_ms;          /* loads that ran on the main thread (visible hitches) */
    int over_100, over_500;
    float worst_ms; int worst_site, worst_main; char worst[72];
} lm_stats;
void lm_stats_add(lm_stats *s, int site, float ms, int on_main, const char *name);
size_t lm_stats_format(const lm_stats *s, char *out, size_t cap);

#endif
