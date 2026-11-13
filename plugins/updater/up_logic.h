/* up_logic.h - the updater's pure logic (no V3X API, no network): JSON scanner, semver, GitHub release
 * list + compat.json parsing, compatibility-aware selection, rate limit, cache file, SHA-256, the
 * setup command line. Network goes through an up_http_fn so test/updater_test.c runs offline.
 * Format of compat.json: docs/updating.md (agreed with installer/make_dist.py). */
#ifndef UP_LOGIC_H
#define UP_LOGIC_H

#include <stddef.h>
#include <stdint.h>

#define UP_REPO_API "https://api.github.com/repos/kleeedolinux/v3x/releases?per_page=30"
#define UP_ASSET_PREFIX "https://github.com/kleeedolinux/v3x/releases/download/"
#define UP_MAX_REL 32
#define UP_MAX_BUILDS 16
#define UP_MAX_ASSETS 8
#define UP_MAX_COMPAT_FETCH 10           /* compat.json downloads per check (newest candidates first) */
#define UP_MAX_JSON (4u << 20)           /* API / compat.json / SHA256SUMS bodies */
#define UP_MAX_SETUP (64u << 20)         /* V3X-Setup.exe */
#define UP_INTERVAL_S (6 * 3600)         /* automatic checks: at most once per 6 h */
#define UP_FORCE_MIN_S 60                /* `update check`: at most once per minute */

/* ---- JSON scanner (read-only, bounded, no allocation). p..e is the input; all functions return NULL /
 * -1 on malformed input and never read past e. Nesting deeper than UP_JS_DEPTH = malformed. */
#define UP_JS_DEPTH 32
const char *up_js_ws(const char *p, const char *e);
const char *up_js_skip(const char *p, const char *e);   /* end of the value at p, or NULL */
int up_js_string(const char *p, const char *e, char *out, size_t cap);   /* length, -1 = not a string,
                                                                         malformed or does not fit */
int up_js_int(const char *p, const char *e, long long *out);   /* 0 ok (integer part of a number) */
int up_js_bool(const char *p, const char *e, int *out);        /* 0 ok (true/false) */
typedef struct up_js_it { const char *p, *e; int first, err; char close; } up_js_it;
int up_js_begin(up_js_it *it, const char *p, const char *e, char open);   /* p at '[' or '{'; 0 ok */
int up_js_next(up_js_it *it, char *key, size_t kcap, const char **v);    /* 1 = element (key filled for
                                                                         objects; long keys truncated), 0 = end, it->err on malformed */
int up_js_get(const char *obj, const char *e, const char *key, const char **v);   /* 1 found, 0 no, -1 bad */

/* ---- semver: [v]MAJOR.MINOR.PATCH[-pre][+build] */
typedef struct up_semver { long long maj, min, pat; char pre[40]; } up_semver;
int up_semver_parse(const char *s, up_semver *v);   /* 0 ok */
int up_semver_cmp(const up_semver *a, const up_semver *b);   /* <0, 0, >0 (build metadata ignored) */
int up_version_cmp(const char *a, const char *b);   /* both must parse; unparsable sorts lowest */

/* ---- catalog (the release list + each candidate's compat.json) */
typedef struct up_build { char branch[64]; char build_id[40]; } up_build;
typedef struct up_asset { char name[64]; char url[400]; long long size; } up_asset;
typedef struct up_release {
    char tag[48], version[48];
    int draft, prerelease;
    int compat;            /* 1 = compat.json parsed and valid, 0 = not fetched, -1 = missing / invalid */
    char compat_err[64];
    int api_version;
    int nbuilds;
    up_build builds[UP_MAX_BUILDS];
    char setup_name[64], setup_url[400], setup_sha[65];
    long long setup_size;
    char compat_url[400], sums_url[400];
    int nassets;           /* transient (GitHub list), not cached */
    up_asset assets[UP_MAX_ASSETS];
} up_release;
typedef struct up_catalog {
    long long checked_at;  /* unix seconds of the last successful check (0 = never) */
    long long attempt_at;  /* unix seconds of the last attempt, successful or not */
    char key[200];         /* up_local_key() of the last successful check */
    int n;
    up_release r[UP_MAX_REL];
} up_catalog;

int up_parse_releases(const char *json, size_t n, up_catalog *cat);   /* GitHub list; count or -1 */
int up_parse_compat(const char *json, size_t n, up_release *r);       /* 0 ok, -1 (r->compat_err) */
int up_sums_find(const char *text, size_t n, const char *name, char out_hex[65]);   /* 0 ok */

/* ---- selection */
typedef struct up_local {
    const char *loader_version;   /* running loader, "0.5.0" */
    int api_version;              /* running loader's V3X_API_VERSION */
    const char *build_id;         /* api->game_build_id() */
    const char *branch;           /* api->game_version() */
    int beta;                     /* [update] channel = beta: pre-releases too */
    int build_supported;          /* 1 = the running loader declares this game build (not gated) */
} up_local;
typedef enum up_state {
    UP_UNKNOWN = 0,          /* not checked yet */
    UP_OFFLINE = 1,          /* the check failed and there is no cached result */
    UP_CURRENT = 2,          /* up to date */
    UP_AVAILABLE = 3,        /* a newer release supports this game build */
    UP_NEWER_INCOMPATIBLE = 4,   /* newer releases exist, none supports this game build: not offered */
    UP_URGENT = 5,           /* this game build is unsupported by the running V3X (plugins gated off)
                                and a release that supports it exists */
    UP_UNSUPPORTED_NOFIX = 6 /* this game build is unsupported and no release supports it yet */
} up_state;
typedef struct up_choice { up_state state; int best, newest; } up_choice;   /* indexes into the catalog, -1 = none */
int up_release_supports(const up_release *r, const char *build_id, const char *branch);
int up_release_candidate(const up_release *r, const up_local *l);   /* newer than local, channel ok */
int up_release_eligible(const up_release *r, const up_local *l);    /* candidate + compatible + verifiable */
up_choice up_select(const up_catalog *cat, const up_local *l);
const char *up_state_name(up_state s);
size_t up_status_text(const up_catalog *cat, const up_local *l, up_choice c, char *out, size_t cap);

/* ---- rate limit */
void up_local_key(const up_local *l, char *out, size_t cap);
int up_should_fetch(const up_catalog *cache, const char *key, long long now, long long interval);
int up_force_allowed(long long last_attempt, long long now);

/* ---- cache file (binaries/v3x/update_cache.json): our own JSON, read back with the scanner */
size_t up_cache_write(const up_catalog *c, char *out, size_t cap);   /* length, 0 = does not fit */
int up_cache_read(const char *json, size_t n, up_catalog *c);        /* 0 ok, -1 malformed (c zeroed) */

/* ---- network through a function pointer (WinHTTP in the plugin, a fake in the test).
 * Returns the HTTP status (200 = *body is malloc'd, NUL-terminated, *len bytes; caller frees) or < 0
 * for a transport failure / body larger than max. */
typedef int (*up_http_fn)(void *ctx, const char *url, size_t max, char **body, size_t *len);
int up_url_allowed(const char *url);   /* only the V3X repository's API and release downloads */
/* Fetches the release list and the compat.json (and SHA256SUMS.txt if needed) of the candidates.
 * 0 ok (cat replaced, checked_at = attempt_at = now, key set), -1 failure (err; cat untouched). */
int up_fetch_catalog(up_http_fn get, void *ctx, const up_local *l, long long now, up_catalog *cat, char *err,
                     size_t errcap);
/* Downloads r's setup into memory and verifies size + SHA-256. 0 ok (*data malloc'd), -1 (err). */
int up_download_setup(up_http_fn get, void *ctx, const up_release *r, char **data, size_t *len, char *err,
                      size_t errcap);

/* ---- SHA-256 */
typedef struct up_sha256 { uint32_t h[8]; uint64_t n; uint8_t buf[64]; size_t k; } up_sha256;
void up_sha256_init(up_sha256 *s);
void up_sha256_update(up_sha256 *s, const void *data, size_t n);
void up_sha256_final(up_sha256 *s, uint8_t out[32]);
void up_sha256_hex(const void *data, size_t n, char out[65]);
int up_sha256_file(const char *path, char out[65]);   /* 0 ok */
int up_hex_valid(const char *h);                      /* 64 hex digits */
int up_hex_eq(const char *a, const char *b);          /* both valid and equal, case-insensitive */

/* ---- applying */
/* ".../Victoria 3/binaries/" -> ".../Victoria 3" (no trailing slash). 0 ok. */
int up_game_root(const char *game_dir, char *out, size_t cap);
/* Appends arg quoted for CommandLineToArgvW / the MSVC CRT. 0 ok, -1 does not fit. */
int up_append_arg(char *out, size_t cap, const char *arg);
/* "<setup>" --update --silent --game-dir "<root>" --wait-pid <pid>. 0 ok. */
int up_build_cmdline(char *out, size_t cap, const char *setup_exe, const char *game_root, unsigned long pid);
/* Last "update-result:" line of install.log: 1 = ok (detail = version), 0 = failed (detail = rest), -1 none. */
int up_install_result(const char *text, size_t n, char *detail, size_t cap);

#endif
