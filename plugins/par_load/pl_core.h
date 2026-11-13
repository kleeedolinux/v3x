/* pl_core.h - engine-independent part of plugins/par_load (tested offline by test/par_load_test.c).
 *
 *   - step statistics for the serial game-init steps (calls, wall ms, max) plus "inner" costs
 *     (VFSOpen, lexer-from-file, blend-shape load) attributed to the init-thread steps active at
 *     that moment (inclusive: every step on the init thread's step stack gets the time)
 *   - per-database records for the serial database ready-loop (abstractgamedatabase.cpp) and the
 *     x64 thunks that time each database callback (code generation + unwind info)
 *   - pl_forjoin: ordered fork/join (work items run on any thread, results are merged on the
 *     calling thread in index order) - the building block for parallel pure parsing
 *   - the text report
 * No engine addresses here. */
#ifndef PL_CORE_H
#define PL_CORE_H
#include <stdint.h>
#include <stdio.h>

enum { PL_MODE_OFF = 0, PL_MODE_MEASURE = 1, PL_MODE_ON = 2 };
int pl_mode_parse(const char *s);           /* "off"/"measure"/"on" (case-insensitive), else -1 */
const char *pl_mode_name(int m);

/* ---- steps */
enum { PL_IN_VFS, PL_IN_LEXER, PL_IN_BLEND, PL_IN_AREAD, PL_IN_COUNT };
#define PL_MAX_STEPS 24
#define PL_STACK_MAX 16
typedef struct {
    const char *name;
    const char *what;                        /* one-line description for the report */
    int top;                                 /* 1 = direct child of game init (counted in "other") */
    volatile long long calls, ticks, max_ticks;
    volatile long long in_ticks[PL_IN_COUNT], in_calls[PL_IN_COUNT];
} pl_step;

typedef struct {
    pl_step steps[PL_MAX_STEPS];
    int nsteps;
    /* the init thread's active steps (only that thread pushes/pops) */
    int stack[PL_STACK_MAX];
    int depth;
    uint32_t init_tid;
    /* inner costs by thread class: [0] init thread, [1] every other thread */
    volatile long long in_ticks_thr[PL_IN_COUNT][2], in_calls_thr[PL_IN_COUNT][2];
    long long freq;                          /* QPC ticks per second */
} pl_stats;

void pl_stats_init(pl_stats *s, long long qpc_freq);
int pl_step_add(pl_stats *s, const char *name, const char *what, int top);   /* index or -1 */
void pl_step_enter(pl_stats *s, int step, uint32_t tid);
void pl_step_leave(pl_stats *s, int step, uint32_t tid, long long dt_ticks);
void pl_inner_add(pl_stats *s, int kind, uint32_t tid, long long dt_ticks);

/* ---- distinct-path counter (duplicate file reads): lock-free set of 64-bit path hashes */
#define PL_PATHSET_BITS 17
typedef struct {
    volatile long long slot[1 << PL_PATHSET_BITS];
    volatile long long total, distinct, full;
} pl_pathset;
uint64_t pl_hash_path(const char *p, size_t n);        /* FNV-1a 64 of the ASCII-lowercased path, '\' = '/' */
int pl_pathset_add(pl_pathset *s, uint64_t h);          /* 1 = first time seen, 0 = seen before, -1 = set full */

/* ---- database ready-loop records */
#define PL_DB_MAX 1536
enum { PL_DB_SLOT_A = 0, PL_DB_SLOT_B = 1, PL_DB_SLOT_C = 2, PL_DB_NSLOTS = 3 };
extern const int PL_DB_SLOT_OFF[PL_DB_NSLOTS];   /* entry offsets: +0x08, +0x20, +0x28 */
typedef struct {
    char name[56];
    int list, idx, slot;                     /* which ready-loop call (1-based), entry index, PL_DB_SLOT_* */
    void *orig;                              /* the engine callback */
    volatile long long ticks, calls;
    volatile long long order;                /* 1-based order of the first call (0 = never called) */
} pl_dbrec;

typedef struct {
    pl_dbrec rec[PL_DB_MAX];
    volatile long nrec;
    volatile long long next_order;
    int nlists;
    long long list_ticks[16];
    int list_entries[16];
} pl_dbtab;

/* x64 thunk that calls dispatch(code, rdx, r8, r9) with a 16-byte aligned stack and returns its
 * rax. Layout (PL_THUNK_BYTES = 26, PL_THUNK_STRIDE = 32):
 *   48 83 EC 28         sub rsp,0x28
 *   B9 <code32>         mov ecx,code
 *   48 B8 <imm64>       mov rax,dispatch
 *   FF D0               call rax
 *   48 83 C4 28         add rsp,0x28
 *   C3                  ret                                                    */
#define PL_THUNK_BYTES 26
#define PL_THUNK_STRIDE 32
#define PL_THUNK_PROLOG 4
void pl_thunk_emit(uint8_t *dst, uint32_t code, const void *dispatch);
/* Allocates `n` thunks (code i -> dispatch(i, ...)) in one RWX block and registers unwind info for
 * them (RtlAddFunctionTable). Returns the first thunk or NULL. pl_thunk_at(i) = i-th thunk. */
uint8_t *pl_thunk_pool_create(int n, const void *dispatch);
void *pl_thunk_at(int i);
int pl_thunk_pool_size(void);

/* The dispatcher par_load installs: times tab->rec[code] around a call of rec->orig(0, a1, a2, a3). */
void pl_db_bind(pl_dbtab *t);
uint64_t pl_db_dispatch(uint64_t code, uint64_t a1, uint64_t a2, uint64_t a3);
extern long long (*pl_now)(void);           /* QPC reader (tests may replace it) */

/* ---- ordered fork/join */
typedef void (*pl_work_fn)(int i, void *ctx);      /* runs on any thread; writes only item i's result */
typedef void (*pl_merge_fn)(int i, void *ctx);     /* runs on the calling thread, i = 0..n-1 in order */
/* Runs work(0..n-1) on `threads` threads (the caller is one of them; threads <= 1 = serial on the
 * caller) and then merge(i) for i = 0..n-1 in order on the caller. Returns the number of helper
 * threads that really started (0 = everything ran serially). */
int pl_forjoin(int n, int threads, pl_work_fn work, pl_merge_fn merge, void *ctx);

/* ---- blend-shape file prefetch (OS file-cache warm-up; the engine still reads every byte itself)
 * Recording (init thread only): every pdx_asset read inside the blend-shape step, in engine order,
 * with the root directory the engine resolved (PHYSFS_getRealDir result of the blend loader's own
 * existence check; a base-mesh read takes the root of the next blend-shape check of the same call).
 * File: binaries/v3x/par_load_prefetch.txt, one line per read: "<size>\t<vfs path>\t<native path>".
 * Next run: a background thread reads those native files sequentially, at most `window` bytes ahead
 * of the engine's position (= number of blend-step reads the engine has started), and discards the
 * bytes. No engine memory, object or file handle is touched. */
#define PL_PF_MAX 16384
typedef struct {
    char *vfs, *native;                      /* malloc'd */
    uint64_t vfs_hash;
    long long size;
    volatile long state;                     /* 0 = not read, 1 = reading, 2 = done, 3 = failed */
} pl_pf_ent;
typedef struct {
    pl_pf_ent e[PL_PF_MAX];
    int n;
    /* recording */
    int pending_root;                        /* index of the first entry still waiting for a root (-1 none) */
    char last_root[520];
    /* run */
    volatile long engine_pos;                /* engine reads started (set by the hook) */
    volatile long stop;
    long long window;
    volatile long long bytes_read, files_read, files_failed, content_hash;
    /* engine side */
    volatile long long hit, inflight, miss, mismatch, beyond;
} pl_pf;

void pl_pf_reset(pl_pf *p);
/* recorder: one engine read (vfs path) / one resolved root (ends with or without a separator) */
void pl_pf_rec_read(pl_pf *p, const char *vfs, size_t n);
void pl_pf_rec_root(pl_pf *p, const char *root);
/* Composes native paths for entries still missing a root (using the last root) and writes the file
 * (sizes from the file system; entries whose native file does not exist are written with size -1).
 * Returns the number of lines written, -1 on error. `resolved` (may be NULL) = entries that exist. */
int pl_pf_save(pl_pf *p, const char *path, int *resolved);
int pl_pf_load(pl_pf *p, const char *path);          /* entries loaded (0 if missing), -1 on error */
/* The prefetch loop (run it on its own thread). Returns files read. content_hash = FNV-1a over every
 * byte read, in order (tests compare it with the files' real content). */
long long pl_pf_run(pl_pf *p);
/* Engine side: the engine starts read number `k` (0-based) of the blend step with this vfs path. */
void pl_pf_engine_read(pl_pf *p, long k, uint64_t vfs_hash);
uint64_t pl_fnv_bytes(uint64_t h, const void *b, size_t n);

/* ---- report */
void pl_report(FILE *f, const pl_stats *s, const pl_dbtab *t, const pl_pathset *ps, const char *header, int top_db);

#endif
