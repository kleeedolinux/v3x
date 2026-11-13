/* map_cache core: config, per-path load statistics, texture keep-alive pins (LRU + byte budget),
 * big-reserve attribution and per-frame spike accounting. Pure data logic, no engine access:
 * the plugin (map_cache.c) does every engine call (refcount add/release) itself, so this file is
 * fully testable offline (test/map_cache_test.c). One SRWLOCK guards all of mc_state. */
#ifndef MC_CORE_H
#define MC_CORE_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

enum { MC_OFF = 0, MC_MEASURE = 1, MC_ON = 2 };

typedef struct mc_config {
    int mode;              /* off | measure | on */
    int spike_ms;          /* log a main-thread gap (frame) >= this, with the texture loads inside it */
    int load_log_ms;       /* log a single CreateTexture call >= this many ms ... */
    int load_log_mb;       /* ... or reading a file >= this many MB (0 = off) */
    int log_budget;        /* max individual load lines per session */
    int big_reserve_mb;    /* attribute generic byte-array reserves >= this (the 0x84800b alloc site) */
    int budget_mb;         /* mode=on: total bytes (file sizes) kept alive by pins */
    int max_texture_mb;    /* mode=on: never pin a texture whose file is larger than this */
    int repeat_min;        /* mode=on: pin a path once it has been loaded from disk this many times */
    int max_pins;          /* mode=on: pin count cap (<= MC_MAX_PINS) */
    int summary_sec;       /* write the summary (log + v3x/map_cache_report.txt) every N s (0 = off) */
    int unknown_size_kb;   /* bytes charged to a pin whose file size could not be measured */
} mc_config;

#define MC_MAX_PATHS 8192          /* power of two */
#define MC_MAX_PINS  2048
#define MC_MAX_BIG   64
#define MC_NAME      120

typedef struct mc_path {
    uint64_t h;                    /* 0 = empty slot */
    char name[MC_NAME];
    uint32_t loads;                /* CreateTexture calls for this path (disk reads) */
    uint32_t via_cache;            /* ... of which came from GetOrLoadTexture (cache miss) */
    uint32_t pin_hits;             /* GetOrLoadTexture calls answered by a texture we kept alive */
    uint64_t bytes;                /* last measured file size */
    double ms_total, ms_max;
} mc_path;

typedef struct mc_pin { void *tex; uint64_t h, bytes, last; } mc_pin;
typedef struct mc_big { uint32_t ret_rva, count; uint64_t bytes, max; } mc_big;

typedef struct mc_frame_snap {
    int loads, reloads;
    double mb, ms, slow_ms;
    char slow[MC_NAME];
} mc_frame_snap;

typedef struct mc_state {
    mc_config cfg;
    SRWLOCK lock;
    mc_path paths[MC_MAX_PATHS]; int npaths, paths_full;
    mc_pin pins[MC_MAX_PINS]; int npins; uint64_t pin_bytes;
    mc_big big[MC_MAX_BIG]; int nbig; uint64_t big_other;
    /* totals */
    uint64_t loads, reloads, gol_calls, gol_misses, pin_hits, pins_added, pins_evicted, pins_replaced, bytes_read;
    double load_ms, reload_ms;
    /* current frame (all threads) */
    int f_loads, f_reloads; uint64_t f_bytes; double f_ms, f_slow_ms; char f_slow[MC_NAME];
} mc_state;

void mc_config_defaults(mc_config *c);
/* "key=value" lines, '#' or ';' comments. Returns the number of bad lines (first one in err). */
int mc_config_parse(mc_config *c, const char *text, char *err, size_t errn);
const char *mc_mode_name(int mode);

/* case-insensitive FNV-1a over the path with '\\' == '/'; never 0 */
uint64_t mc_hash(const char *s, size_t n);

void mc_init(mc_state *s, const mc_config *c);

/* Record one CreateTexture call (a disk read). Returns this path's load count including this one. */
uint32_t mc_record_load(mc_state *s, uint64_t h, const char *name, size_t n, uint64_t bytes, double ms, int via_cache);
/* GetOrLoadTexture finished (hit = no CreateTexture inside) */
void mc_record_gol(mc_state *s, int miss);

/* mode=on policy: pin after the repeat_min-th disk read, within max_texture_mb */
int mc_should_pin(const mc_state *s, uint32_t loads, uint64_t bytes);
/* Add a pin for tex (path h). Returns 1 = caller must add one reference, 0 = already pinned / table
 * full / disabled. *replaced = a previously pinned texture of the same path that the caller must
 * release (NULL if none). */
int mc_pin_add(mc_state *s, void *tex, uint64_t h, uint64_t bytes, uint64_t now, void **replaced);
/* tex returned from the cache: if pinned, refresh LRU and count a pin hit. Returns 1 if pinned. */
int mc_pin_touch(mc_state *s, void *tex, uint64_t now);
/* Remove least-recently-used pins until within budget/max_pins. Returns count written to victims
 * (caller releases one reference of each). */
int mc_pin_collect(mc_state *s, void **victims, int maxv);
/* Remove every pin (caller releases them). */
int mc_pin_take_all(mc_state *s, void **victims, int maxv);

void mc_big_record(mc_state *s, uint32_t ret_rva, uint64_t n);
/* Take and reset the current frame's accumulators. */
void mc_frame_take(mc_state *s, mc_frame_snap *out);
/* Human-readable summary (top paths by total load time, pins, big reserves). Returns length. */
int mc_format_summary(mc_state *s, char *buf, size_t n, int top);

/* Read an MSVC std::string (SSO 16, size +0x10, cap +0x18) into out. readable(p, n) validates
 * memory (may be NULL = trust). Returns length or -1. */
int mc_msvc_str(const void *str, char *out, size_t outn, int (*readable)(const void *, size_t));

#endif
