/* lazy_gfx core: config, per-texture load records (stage split), aggregation per load phase /
 * thread class / file type, report + CSV formatting, the prefetch manifest and the prefetch
 * worker. No engine access here (the plugin, lazy_gfx.c, owns every hook), so all of this is
 * testable offline (test/lazy_gfx_test.c). One SRWLOCK guards lg_state. */
#ifndef LG_CORE_H
#define LG_CORE_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

enum { LG_OFF = 0, LG_MEASURE = 1, LG_ON = 2 };
enum { LG_PC_OFF = 0, LG_PC_BUILD = 1, LG_PC_VERIFY = 2, LG_PC_ON = 3 };

typedef struct lg_config {
    int mode;               /* off | measure | on (= measure + prefetch) */
    int csv;                /* write v3x/lazy_gfx_textures.csv (one line per texture load) */
    int max_records;        /* records kept for the CSV (aggregates count everything) */
    int slow_log_ms;        /* log a single texture load >= this (0 = off) */
    int log_budget;         /* max slow-load lines per session */
    int manifest;           /* write v3x/lazy_gfx_prefetch.txt when the first Game state begins */
    int manifest_game;      /* 0 = textures read before the first Game load (written when it begins),
                               1 = also those of the first Game load (written when it ends) */
    int prefetch_threads;   /* mode=on: reader threads (1..16) */
    int prefetch_max_mb;    /* mode=on: stop after reading this many MB */
    int prefetch_low_io;    /* mode=on: THREAD_MODE_BACKGROUND_BEGIN (very low I/O priority) */
    int png_cache;          /* LG_PC_*: processed-texture cache for png/jpg/tga (independent of mode) */
    int cache_max_mb;       /* bytes written to the cache per session */
    int cache_min_kb;       /* only sources at least this big */
    int layout_min;         /* plain 2D DDS decodes that must confirm the img layout before the cache acts */
    char cache_dir[260];    /* "" = <v3x>/cache/lazy_gfx/ */
} lg_config;

#define LG_FRAMES 6         /* requester return addresses (exe RVAs) above CreateTexture */
#define LG_PATH 200
#define LG_ROOTS 96
#define LG_PHASES 16
#define LG_EXTS 5           /* dds png jpg tga other */
#define LG_MAX_RECORDS 65536
#define LG_THREADS 64
#define LG_IMG 80             /* bytes of the decoded-image struct captured after decode (layout not RE'd) */

enum { LG_ST_OPTS, LG_ST_STAT, LG_ST_OPEN, LG_ST_READ, LG_ST_DECODE, LG_ST_UPLOAD, LG_ST_OBJ, LG_ST_STAMP, LG_ST_N };

typedef struct lg_rec {
    double t_ms;                 /* start, ms since lg_init */
    double total_ms;             /* first stage start .. object created */
    float st[LG_ST_N];           /* ms per stage (see lg_stage_name) */
    uint64_t bytes;              /* file size (decode input) */
    uint32_t tid;
    uint32_t req[LG_FRAMES];     /* requester chain: return addresses above CreateTexture (RVA, 0 = none) */
    uint8_t main, ok, phase, ext, root, via_cache, prefetched, complete, has_img;
    uint8_t img[LG_IMG];         /* decode's output struct (CreateTexture rbp-0x80) right after decode */
    char path[LG_PATH];
} lg_rec;

typedef struct lg_agg {
    uint32_t n, failed, prefetched;
    uint64_t bytes;
    double total_ms, st[LG_ST_N];
} lg_agg;

typedef struct lg_pf_item { char *path; uint64_t size, mtime; uint64_t vh; volatile LONG state; } lg_pf_item;
enum { LG_PF_TODO = 0, LG_PF_BUSY, LG_PF_DONE, LG_PF_SKIP };

typedef struct lg_prefetch {
    lg_pf_item *items; int n, cap;
    volatile LONG next;          /* next item index to take */
    volatile LONG stop;
    volatile LONG64 bytes_read;
    volatile LONG done, skipped, failed;
    double ms;                   /* wall time of the whole run */
    uint64_t max_bytes;
} lg_prefetch;

typedef struct lg_state {
    lg_config cfg;
    SRWLOCK lock;
    LARGE_INTEGER freq, t0;
    lg_rec *recs; int nrecs, recs_dropped;
    lg_agg agg[LG_PHASES][2][LG_EXTS];   /* [phase][main][ext] */
    char phases[LG_PHASES][32]; int nphases, cur_phase;
    char roots[LG_ROOTS][260]; const void *root_key[LG_ROOTS]; int nroots;
    uint64_t total_loads; double main_ms;
    struct { uint32_t tid, n; uint64_t bytes; double ms; } thr[LG_THREADS]; int nthr;
} lg_state;

void lg_config_defaults(lg_config *c);
/* "key = value" lines, '#'/';' comments, [sections] ignored. Returns bad line count (first in err). */
int lg_config_parse(lg_config *c, const char *text, char *err, size_t errn);
const char *lg_mode_name(int mode);
const char *lg_stage_name(int st);
const char *lg_ext_name(int ext);
int lg_ext_of(const char *path, size_t n);
uint64_t lg_hash(const char *s, size_t n);      /* case-insensitive FNV-1a, '\\' == '/', never 0 */

int lg_init(lg_state *s, const lg_config *c);   /* 0 ok, -1 no memory */
void lg_free(lg_state *s);
double lg_now_ms(const lg_state *s);
/* Starts a new phase (load state name); returns its index (the last slot is reused when full). */
int lg_set_phase(lg_state *s, const char *name);
/* Interns a VFS root dir (key = the engine's dirName pointer). Returns index (255 = unknown). */
int lg_root(lg_state *s, const void *key, const char *dir);
/* Adds a finished record (copied) and aggregates it. */
void lg_add(lg_state *s, const lg_rec *r);

/* Report text (stage split per phase/thread/type, top slow paths). Returns length. */
int lg_format_report(lg_state *s, char *buf, size_t n, int top);
/* CSV with a header line; one row per kept record. Returns 0 ok. */
int lg_write_csv(lg_state *s, const char *path);
/* Prefetch manifest: unique paths of complete records (every thread), in load order, resolved to <root>/<path>; stat_fn returns 1 + size/mtime if the file exists (NULL =
 * GetFileAttributesExA). Returns the number of lines written or -1. */
int lg_write_manifest(lg_state *s, const char *path,
                      int (*stat_fn)(const char *abs, uint64_t *size, uint64_t *mtime));

/* Prefetch (mode=on). Load a manifest; run reads it on nthreads threads and returns when all are
 * done or pf->stop is set (blocking; the plugin calls it from its own thread). */
int lg_pf_load(lg_prefetch *pf, const char *manifest_path, uint64_t max_bytes);
void lg_pf_free(lg_prefetch *pf);
int lg_pf_run(lg_prefetch *pf, int nthreads, int low_io);
/* 1 if the manifest had virtual path hash vh and it was read before now. */
int lg_pf_was_read(const lg_prefetch *pf, uint64_t vh);

#endif
