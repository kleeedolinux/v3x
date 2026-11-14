/* pl_core.c - see pl_core.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include "pl_core.h"

int pl_mode_parse(const char *s)
{
    if (!s) return -1;
    if (!_stricmp(s, "off") || !strcmp(s, "0")) return PL_MODE_OFF;
    if (!_stricmp(s, "measure")) return PL_MODE_MEASURE;
    if (!_stricmp(s, "on") || !strcmp(s, "1")) return PL_MODE_ON;
    return -1;
}
const char *pl_mode_name(int m) { return m == PL_MODE_ON ? "on" : m == PL_MODE_MEASURE ? "measure" : "off"; }

static long long qpc_now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
long long (*pl_now)(void) = qpc_now;

/* ------------------------------------------------------------------ steps */

void pl_stats_init(pl_stats *s, long long qpc_freq)
{
    memset(s, 0, sizeof *s);
    s->freq = qpc_freq > 0 ? qpc_freq : 1;
}

int pl_step_add(pl_stats *s, const char *name, const char *what, int top)
{
    if (s->nsteps >= PL_MAX_STEPS) return -1;
    pl_step *p = &s->steps[s->nsteps];
    memset(p, 0, sizeof *p);
    p->name = name; p->what = what; p->top = top;
    return s->nsteps++;
}

void pl_step_enter(pl_stats *s, int step, uint32_t tid)
{
    if (step < 0 || step >= s->nsteps) return;
    if (tid == s->init_tid && s->depth < PL_STACK_MAX) s->stack[s->depth++] = step;
}

void pl_step_leave(pl_stats *s, int step, uint32_t tid, long long dt)
{
    if (step < 0 || step >= s->nsteps) return;
    pl_step *p = &s->steps[step];
    InterlockedIncrement64(&p->calls);
    InterlockedAdd64(&p->ticks, dt);
    for (;;) {
        long long m = p->max_ticks;
        if (dt <= m || InterlockedCompareExchange64(&p->max_ticks, dt, m) == m) break;
    }
    if (tid == s->init_tid && s->depth > 0 && s->stack[s->depth - 1] == step) s->depth--;
}

void pl_inner_add(pl_stats *s, int kind, uint32_t tid, long long dt)
{
    if (kind < 0 || kind >= PL_IN_COUNT) return;
    int other = tid != s->init_tid;
    InterlockedAdd64(&s->in_ticks_thr[kind][other], dt);
    InterlockedIncrement64(&s->in_calls_thr[kind][other]);
    if (other) return;
    /* the stack is only written by this (the init) thread: no lock needed */
    int seen[PL_MAX_STEPS] = {0};
    for (int i = 0; i < s->depth; i++) {
        int st = s->stack[i];
        if (st < 0 || st >= s->nsteps || seen[st]) continue;   /* a recursive step counts once */
        seen[st] = 1;
        s->steps[st].in_ticks[kind] += dt;
        s->steps[st].in_calls[kind]++;
    }
}

/* ------------------------------------------------------------------ path set */

uint64_t pl_hash_path(const char *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        uint8_t ch = (uint8_t)p[i];
        if (ch >= 'A' && ch <= 'Z') ch = (uint8_t)(ch + 32);
        if (ch == 0x5C) ch = '/';
        h = (h ^ ch) * 1099511628211ull;
    }
    return h ? h : 1;                                   /* 0 = empty slot */
}

int pl_pathset_add(pl_pathset *s, uint64_t h)
{
    InterlockedIncrement64(&s->total);
    if (!h) h = 1;
    size_t mask = ((size_t)1 << PL_PATHSET_BITS) - 1, i = (size_t)(h ^ (h >> 29)) & mask;
    for (size_t probe = 0; probe < 256; probe++, i = (i + 1) & mask) {
        long long cur = s->slot[i];
        if (cur == (long long)h) return 0;
        if (cur == 0) {
            long long prev = InterlockedCompareExchange64(&s->slot[i], (long long)h, 0);
            if (prev == 0) { InterlockedIncrement64(&s->distinct); return 1; }
            if (prev == (long long)h) return 0;
        }
    }
    InterlockedIncrement64(&s->full);
    return -1;
}

/* ------------------------------------------------------------------ thunks */

const int PL_DB_SLOT_OFF[PL_DB_NSLOTS] = {0x08, 0x20, 0x28};

void pl_thunk_emit(uint8_t *d, uint32_t code, const void *dispatch)
{
    uint64_t a = (uint64_t)(uintptr_t)dispatch;
    uint8_t *p = d;
    *p++ = 0x48; *p++ = 0x83; *p++ = 0xEC; *p++ = 0x28;              /* sub rsp,0x28 */
    *p++ = 0xB9; memcpy(p, &code, 4); p += 4;                         /* mov ecx,code */
    *p++ = 0x48; *p++ = 0xB8; memcpy(p, &a, 8); p += 8;               /* mov rax,imm64 */
    *p++ = 0xFF; *p++ = 0xD0;                                         /* call rax */
    *p++ = 0x48; *p++ = 0x83; *p++ = 0xC4; *p++ = 0x28;               /* add rsp,0x28 */
    *p++ = 0xC3;                                                      /* ret */
    while (p < d + PL_THUNK_STRIDE) *p++ = 0xCC;
}

static uint8_t *g_pool;
static int g_pool_n;

uint8_t *pl_thunk_pool_create(int n, const void *dispatch)
{
    if (g_pool) return n <= g_pool_n ? g_pool : NULL;
    if (n <= 0) return NULL;
    size_t code = (size_t)n * PL_THUNK_STRIDE;
    size_t tab = (size_t)n * sizeof(RUNTIME_FUNCTION);
    size_t total = code + tab + 64;
    uint8_t *b = VirtualAlloc(NULL, total, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!b) return NULL;
    for (int i = 0; i < n; i++) pl_thunk_emit(b + (size_t)i * PL_THUNK_STRIDE, (uint32_t)i, dispatch);
    /* one shared UNWIND_INFO: version 1, no flags, prolog 4 bytes, 1 code: UWOP_ALLOC_SMALL 0x28 */
    uint8_t *ui = b + code + tab;
    ui[0] = 1; ui[1] = PL_THUNK_PROLOG; ui[2] = 1; ui[3] = 0;
    ui[4] = PL_THUNK_PROLOG; ui[5] = (uint8_t)(2 | (((0x28 - 8) / 8) << 4)); ui[6] = 0; ui[7] = 0;
    RUNTIME_FUNCTION *rf = (RUNTIME_FUNCTION *)(b + code);
    for (int i = 0; i < n; i++) {
        rf[i].BeginAddress = (DWORD)(i * PL_THUNK_STRIDE);
        rf[i].EndAddress = (DWORD)(i * PL_THUNK_STRIDE + PL_THUNK_BYTES);
        rf[i].UnwindData = (DWORD)(ui - b);
    }
    if (!RtlAddFunctionTable(rf, (DWORD)n, (DWORD64)(uintptr_t)b)) {
        VirtualFree(b, 0, MEM_RELEASE);
        return NULL;
    }
    FlushInstructionCache(GetCurrentProcess(), b, code);
    g_pool = b; g_pool_n = n;
    return b;
}
void *pl_thunk_at(int i) { return g_pool && i >= 0 && i < g_pool_n ? g_pool + (size_t)i * PL_THUNK_STRIDE : NULL; }
int pl_thunk_pool_size(void) { return g_pool_n; }

static pl_dbtab *g_tab;
void pl_db_bind(pl_dbtab *t) { g_tab = t; }

typedef uint64_t (*fn4)(uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t pl_db_dispatch(uint64_t code, uint64_t a1, uint64_t a2, uint64_t a3)
{
    pl_dbrec *r = &g_tab->rec[code];
    if (!r->order) r->order = InterlockedIncrement64(&g_tab->next_order);
    long long t0 = pl_now();
    /* integer-only before the call: xmm argument registers are passed through untouched */
    uint64_t ret = ((fn4)r->orig)(0, a1, a2, a3);
    InterlockedAdd64(&r->ticks, pl_now() - t0);
    InterlockedIncrement64(&r->calls);
    return ret;
}

/* ------------------------------------------------------------------ fork/join */

typedef struct { volatile long next; int n; pl_work_fn work; void *ctx; } fj_t;

static void fj_run(fj_t *j)
{
    for (;;) {
        long i = InterlockedIncrement(&j->next) - 1;
        if (i >= j->n) break;
        j->work((int)i, j->ctx);
    }
}
static DWORD WINAPI fj_thread(LPVOID p) { fj_run((fj_t *)p); return 0; }

int pl_forjoin(int n, int threads, pl_work_fn work, pl_merge_fn merge, void *ctx)
{
    if (n <= 0) return 0;
    fj_t j = {0, n, work, ctx};
    HANDLE h[64];
    int started = 0;
    if (threads > 65) threads = 65;
    if (threads > n) threads = n;
    for (int k = 0; k + 1 < threads; k++) {
        h[started] = CreateThread(NULL, 256 * 1024, fj_thread, &j, 0, NULL);
        if (h[started]) started++;
    }
    fj_run(&j);                                  /* the caller works too */
    if (started) WaitForMultipleObjects((DWORD)started, h, TRUE, INFINITE);
    for (int k = 0; k < started; k++) CloseHandle(h[k]);
    if (merge)
        for (int i = 0; i < n; i++) merge(i, ctx);
    return started;
}

/* ------------------------------------------------------------------ blend-shape prefetch */

uint64_t pl_fnv_bytes(uint64_t h, const void *b, size_t n)
{
    const uint8_t *p = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

void pl_pf_reset(pl_pf *p)
{
    for (int i = 0; i < p->n; i++) { free(p->e[i].vfs); free(p->e[i].native); }
    memset(p, 0, sizeof *p);
    p->pending_root = -1;
    p->window = 64ll << 20;
    p->content_hash = (long long)1469598103934665603ull;
}

static char *join_root(const char *root, const char *vfs)
{
    size_t a = strlen(root), b = strlen(vfs);
    char *o = malloc(a + b + 2);
    if (!o) return NULL;
    memcpy(o, root, a);
    while (*vfs == '/' || *vfs == 0x5C) { vfs++; b--; }
    if (a && o[a - 1] != '/' && o[a - 1] != 0x5C) o[a++] = 0x5C;
    memcpy(o + a, vfs, b + 1);
    for (char *c = o; *c; c++) if (*c == '/') *c = 0x5C;
    return o;
}

static int root_fresh;
void pl_pf_rec_read(pl_pf *p, const char *vfs, size_t n)
{
    if (p->n >= PL_PF_MAX || !n || n > 400) return;
    pl_pf_ent *e = &p->e[p->n];
    memset(e, 0, sizeof *e);
    e->vfs = malloc(n + 1);
    if (!e->vfs) return;
    memcpy(e->vfs, vfs, n); e->vfs[n] = 0;
    e->vfs_hash = pl_hash_path(vfs, n);
    e->size = -1;
    if (root_fresh && p->last_root[0]) { e->native = join_root(p->last_root, e->vfs); root_fresh = 0; }
    else if (p->pending_root < 0) p->pending_root = p->n;
    p->n++;
}

void pl_pf_rec_root(pl_pf *p, const char *root)
{
    size_t rl = root ? strlen(root) : 0;
    if (!rl || rl >= sizeof p->last_root) return;
    memcpy(p->last_root, root, rl + 1);
    if (p->pending_root >= 0)
        for (int i = p->pending_root; i < p->n; i++)
            if (!p->e[i].native) p->e[i].native = join_root(root, p->e[i].vfs);
    p->pending_root = -1;
    root_fresh = 1;
}

static long long file_size_u8(const char *path)
{
    wchar_t w[1040];
    if (!path || !MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1040)) return -1;
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(w, GetFileExInfoStandard, &d) || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return -1;
    return ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

int pl_pf_save(pl_pf *p, const char *path, int *resolved)
{
    if (p->pending_root >= 0 && p->last_root[0])
        for (int i = p->pending_root; i < p->n; i++)
            if (!p->e[i].native) p->e[i].native = join_root(p->last_root, p->e[i].vfs);
    p->pending_root = -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# par_load blend-shape prefetch list: size<TAB>vfs path<TAB>native path (engine read order)\n");
    int ok = 0;
    for (int i = 0; i < p->n; i++) {
        long long sz = file_size_u8(p->e[i].native);
        ok += sz >= 0;
        fprintf(f, "%lld\t%s\t%s\n", sz, p->e[i].vfs, p->e[i].native ? p->e[i].native : "");
    }
    fclose(f);
    if (resolved) *resolved = ok;
    return p->n;
}

static int pl_is_sep(char c) { return c == '/' || c == 0x5C; }

/* "X:\" + more, no ".." component, no ':' after the drive, no control chars, no doubled separator */
static int pl_abs_drive_path(const char *s)
{
    if (!s || !((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) || s[1] != ':' || !pl_is_sep(s[2])) return 0;
    for (const char *c = s; *c; c++) {
        if ((unsigned char)*c < 0x20 || (c > s + 1 && *c == ':') || strchr("<>\"|?*", *c)) return 0;
        if (c > s && pl_is_sep(c[-1]) && pl_is_sep(*c)) return 0;
        if (c > s && c[0] == '.' && c[1] == '.' && pl_is_sep(c[-1]) && (!c[2] || pl_is_sep(c[2]))) return 0;
    }
    return 1;
}

int pl_pf_add_root(pl_pf *p, const char *dir)
{
    size_t n = dir ? strlen(dir) : 0;
    if (p->nroots >= PL_PF_ROOTS || n < 3 || n >= sizeof p->roots[0] || !pl_abs_drive_path(dir)) return 0;
    char *r = p->roots[p->nroots];
    memcpy(r, dir, n + 1);
    while (n > 3 && pl_is_sep(r[n - 1])) r[--n] = 0;      /* no trailing separator except "X:\" */
    p->nroots++;
    return 1;
}

int pl_pf_path_ok(const pl_pf *p, const char *native)
{
    if (!pl_abs_drive_path(native)) return 0;
    size_t m = strlen(native);
    for (int r = 0; r < p->nroots; r++) {
        const char *root = p->roots[r];
        size_t n = strlen(root);
        if (m <= n) continue;
        int ok = 1;
        for (size_t i = 0; i < n && ok; i++) {
            char a = native[i], b = root[i];
            if (pl_is_sep(a) && pl_is_sep(b)) continue;
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            ok = a == b;
        }
        if (ok && (pl_is_sep(root[n - 1]) || pl_is_sep(native[n]))) return 1;   /* component boundary */
    }
    return 0;
}

int pl_pf_load(pl_pf *p, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[2200];
    while (fgets(line, sizeof line, f) && p->n < PL_PF_MAX) {
        if (line[0] == '#') continue;
        char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t2) continue;
        *t1 = 0; *t2 = 0;
        char *nat = t2 + 1, *nl = nat + strcspn(nat, "\r\n");
        *nl = 0;
        pl_pf_ent *e = &p->e[p->n];
        memset(e, 0, sizeof *e);
        e->size = _atoi64(line);
        e->vfs = _strdup(t1 + 1);
        e->native = *nat && pl_pf_path_ok(p, nat) ? _strdup(nat) : NULL;   /* outside the roots: never opened */
        if (!e->vfs) break;
        e->vfs_hash = pl_hash_path(e->vfs, strlen(e->vfs));
        p->n++;
    }
    fclose(f);
    return p->n;
}

long long pl_pf_run(pl_pf *p)
{
    long long *ps = calloc((size_t)p->n + 1, sizeof *ps);
    if (!ps) return 0;
    for (int i = 0; i < p->n; i++) ps[i + 1] = ps[i] + (p->e[i].size > 0 ? p->e[i].size : 0);
    static uint8_t buf[1 << 20];
    uint64_t h = (uint64_t)p->content_hash;
#define PL_STOPPED() __atomic_load_n(&p->stop, __ATOMIC_ACQUIRE)
#define PL_POS() __atomic_load_n(&p->engine_pos, __ATOMIC_ACQUIRE)
    for (int i = 0; i < p->n && !PL_STOPPED(); i++) {
        pl_pf_ent *e = &p->e[i];
        if (e->size < 0 || !e->native || !pl_pf_path_ok(p, e->native)) { __atomic_store_n(&e->state, 3, __ATOMIC_RELEASE); continue; }
        for (;;) {                                   /* stay at most `window` bytes ahead of the engine */
            long pos = PL_POS();
            if (PL_STOPPED() || pos > i || i == pos || ps[i] + e->size - ps[pos < p->n ? pos : p->n] <= p->window) break;
            Sleep(1);
        }
        if (PL_STOPPED()) break;
        if (PL_POS() > i) { __atomic_store_n(&e->state, 3, __ATOMIC_RELEASE); continue; }  /* the engine is already past it */
        wchar_t w[1040];
        HANDLE fh = INVALID_HANDLE_VALUE;
        __atomic_store_n(&e->state, 1, __ATOMIC_RELEASE);
        if (MultiByteToWideChar(CP_UTF8, 0, e->native, -1, w, 1040))
            fh = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                             FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (fh == INVALID_HANDLE_VALUE) { __atomic_store_n(&e->state, 3, __ATOMIC_RELEASE); InterlockedIncrement64(&p->files_failed); continue; }
        DWORD got;
        while (ReadFile(fh, buf, sizeof buf, &got, NULL) && got) {
            h = pl_fnv_bytes(h, buf, got);
            InterlockedAdd64(&p->bytes_read, got);
        }
        CloseHandle(fh);
        __atomic_store_n(&e->state, 2, __ATOMIC_RELEASE);
        InterlockedIncrement64(&p->files_read);
    }
    p->content_hash = (long long)h;
    free(ps);
    return p->files_read;
}

void pl_pf_engine_read(pl_pf *p, long k, uint64_t vfs_hash)
{
    __atomic_store_n(&p->engine_pos, k + 1, __ATOMIC_RELEASE);
    if (k >= p->n) { InterlockedIncrement64(&p->beyond); return; }
    if (p->e[k].vfs_hash != vfs_hash) { InterlockedIncrement64(&p->mismatch); return; }
    long st = __atomic_load_n(&p->e[k].state, __ATOMIC_ACQUIRE);
    if (st == 2) InterlockedIncrement64(&p->hit);
    else if (st == 1) InterlockedIncrement64(&p->inflight);
    else InterlockedIncrement64(&p->miss);
}

/* ------------------------------------------------------------------ report */

static double ms(const pl_stats *s, long long t) { return (double)t * 1000.0 / (double)s->freq; }

typedef struct { int list, idx; long long t[PL_DB_NSLOTS]; long long tot, order; const char *name; } dbrow;
static int dbrow_cmp(const void *a, const void *b)
{
    long long x = ((const dbrow *)a)->tot, y = ((const dbrow *)b)->tot;
    return x < y ? 1 : x > y ? -1 : 0;
}

void pl_report(FILE *f, const pl_stats *s, const pl_dbtab *t, const pl_pathset *ps, const char *header, int top_db)
{
    (void)ps;   /* kept in the signature: distinct-path numbers are printed by the caller */
    static const char *INNAME[PL_IN_COUNT] = {"vfs_open", "lexer_file", "blendshape", "asset_read"};
    fprintf(f, "# par_load report%s%s\n", header ? " - " : "", header ? header : "");
    fprintf(f, "# wall ms per step on the thread that ran it; 'inner' = time inside VFSOpen / lexer-from-file /\n"
               "# blend-shape loads made by the init thread while the step was active (inclusive).\n");
    fprintf(f, "%-14s %6s %10s %9s | %10s %7s | %10s %6s | %10s %6s | %s\n", "step", "calls", "total_ms", "max_ms",
            "vfs_ms", "vfs_n", "lexer_ms", "lex_n", "blend_ms", "bl_n", "what");
    long long top_sum = 0, init_ticks = 0;
    for (int i = 0; i < s->nsteps; i++) {
        const pl_step *p = &s->steps[i];
        if (i == 0) init_ticks = p->ticks;
        else if (p->top) top_sum += p->ticks;
        fprintf(f, "%-14s %6lld %10.1f %9.1f | %10.1f %7lld | %10.1f %6lld | %10.1f %6lld | %s\n", p->name, p->calls,
                ms(s, p->ticks), ms(s, p->max_ticks), ms(s, p->in_ticks[PL_IN_VFS]), p->in_calls[PL_IN_VFS],
                ms(s, p->in_ticks[PL_IN_LEXER]), p->in_calls[PL_IN_LEXER], ms(s, p->in_ticks[PL_IN_BLEND]),
                p->in_calls[PL_IN_BLEND], p->what ? p->what : "");
    }
    if (s->nsteps)
        fprintf(f, "%-14s %6s %10.1f %9s   (game init minus the timed top-level steps: mesh pass, map, misc)\n", "other", "",
                ms(s, init_ticks - top_sum), "");
    fprintf(f, "\ninner totals: %-12s %12s %9s %12s %9s\n", "", "init_thr_ms", "init_n", "other_ms", "other_n");
    for (int k = 0; k < PL_IN_COUNT; k++)
        fprintf(f, "  %-24s %12.1f %9lld %12.1f %9lld\n", INNAME[k], ms(s, s->in_ticks_thr[k][0]), s->in_calls_thr[k][0],
                ms(s, s->in_ticks_thr[k][1]), s->in_calls_thr[k][1]);
    if (!t) return;
    fprintf(f, "\n# database ready-loop (abstractgamedatabase.cpp): %d call(s), %ld timed callbacks\n", t->nlists, t->nrec);
    for (int l = 0; l < t->nlists && l < 16; l++)
        fprintf(f, "  list %d: %d entries, %.1f ms\n", l + 1, t->list_entries[l], ms(s, t->list_ticks[l]));
    int n = (int)t->nrec;
    dbrow *rows = calloc((size_t)(n ? n : 1), sizeof *rows);
    if (!rows) return;
    int nr = 0;
    long long all = 0;
    for (int i = 0; i < n; i++) {
        const pl_dbrec *r = &t->rec[i];
        int k = 0;
        while (k < nr && !(rows[k].list == r->list && rows[k].idx == r->idx)) k++;
        if (k == nr) { rows[nr].list = r->list; rows[nr].idx = r->idx; rows[nr].name = r->name; nr++; }
        if (r->slot >= 0 && r->slot < PL_DB_NSLOTS) rows[k].t[r->slot] += r->ticks;
        rows[k].tot += r->ticks; all += r->ticks;
        if (r->order && (!rows[k].order || r->order < rows[k].order)) rows[k].order = r->order;
    }
    qsort(rows, (size_t)nr, sizeof *rows, dbrow_cmp);
    fprintf(f, "  %d databases, %.1f ms in their callbacks; top %d:\n", nr, ms(s, all), top_db);
    fprintf(f, "  %-44s %4s %5s %10s %10s %10s %10s %6s\n", "database", "list", "order", "total_ms", "+08_ms", "+20_ms",
            "+28_ms", "share");
    for (int i = 0; i < nr && i < top_db; i++)
        fprintf(f, "  %-44.44s %4d %5lld %10.1f %10.1f %10.1f %10.1f %5.1f%%\n", rows[i].name, rows[i].list, rows[i].order,
                ms(s, rows[i].tot), ms(s, rows[i].t[0]), ms(s, rows[i].t[1]), ms(s, rows[i].t[2]),
                all ? 100.0 * (double)rows[i].tot / (double)all : 0.0);
    free(rows);
}
