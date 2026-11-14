/* fast_load core: pure helpers (no game, no Windows hooks) so test/fast_load_test.c can run offline. */
#ifndef FL_CORE_H
#define FL_CORE_H
#include <stddef.h>
#include <stdint.h>

/* <game>/binaries/v3x/fast_load.ini, "key = value" lines, '#' or ';' comments. */
typedef struct {
    int timeline;        /* 1 = install timing hooks, write v3x/load_timeline.txt (default 1) */
    int checksum_cache;  /* 1 = per-file checksum cache (default 0; see fast_load.c: fails closed) */
    int cache_verify;    /* 1 = compute both and log mismatches (default 1) */
    int db_nodes;        /* 1 = list every database node in the timeline (default 1) */
    int mesh_threads;    /* 0 = leave the engine's mesh thread setting alone (default); 2..15 = set it
                          * for the duration of the mesh loader, then restore the old value */
    int counters;        /* 1 = 1 Hz process/system CPU, I/O, hard-fault and RAM counters in the timeline
                          * until the first transition ends, then during every transition (default 1) */
    int worker_spin;     /* idle task workers re-poll without pause while any worker is busy: 0 = leave
                          * (default), 1 = pause backoff, 2 = + SwitchToThread, 3 = + Sleep(1) when long idle */
    int pump_yield;      /* loading-screen pump on the main thread (03775600) spins without sleeping:
                          * 0 = leave it (default), 1 = SwitchToThread() after each step, 2 = Sleep(1) */
} fl_config;

void fl_config_defaults(fl_config *c);
/* Returns the number of bad lines; the first error is described in err. Unknown keys are errors. */
int fl_config_parse(fl_config *c, const char *text, char *err, size_t errn);

/* 1 if code[off..] matches an IDA-style pattern ("48 8B ?? 05"), never reads past avail. */
int fl_match(const uint8_t *code, size_t avail, size_t off, const char *pat);

/* Timeline: fixed-capacity event list; extra events are counted in `dropped`. Caller locks. */
typedef struct { double t_ms; uint32_t tid; char text[132]; } fl_event;
typedef struct { fl_event *ev; int n, cap, dropped; } fl_timeline;
void fl_tl_add(fl_timeline *tl, double t_ms, uint32_t tid, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
/* Formats "  +12345.6 ms  [tid 1234]  text" lines sorted by time (stable). Returns bytes written
 * (always NUL-terminated if cap > 0, truncated if needed). */
size_t fl_tl_format(fl_timeline *tl, const char *header, char *out, size_t cap);

/* Which threads ran mesh items. Caller locks. */
typedef struct { uint32_t tid; uint32_t calls; double first_ms, last_ms; } fl_thr;
typedef struct { fl_thr t[64]; int n; uint32_t calls, overflow; } fl_thrset;
void fl_thr_hit(fl_thrset *s, uint32_t tid, double t_ms);
/* "threads=N items=M span_sum=..ms wall=..ms est_parallelism=x.x" (span = last-first item entry
 * per thread; an upper-bound proxy for busy time because the item hook is entry-only). */
size_t fl_thr_summary(const fl_thrset *s, double wall_ms, char *out, size_t cap);

/* MSVC x64 std::string {union{char buf[16]; char *ptr;}; size_t size; size_t cap;} -> out.
 * `readable` checks memory before every dereference. Returns length copied or -1. */
typedef int (*fl_readable_fn)(const void *p, size_t n);
int fl_msvc_str(const void *s, char *out, size_t cap, fl_readable_fn readable);
/* {const char *ptr; int64 len} view (engine name type used by database nodes). */
int fl_view_str(const void *v, char *out, size_t cap, fl_readable_fn readable);

#endif
