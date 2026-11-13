/* lazy_gfx core (see lg_core.h). Pure data logic + the prefetch reader threads. */
#include "lg_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ config */

void lg_config_defaults(lg_config *c)
{
    memset(c, 0, sizeof *c);
    c->mode = LG_MEASURE;
    c->csv = 1;
    c->max_records = 20000;
    c->slow_log_ms = 100;
    c->log_budget = 300;
    c->manifest = 1;
    c->manifest_game = 0;
    c->prefetch_threads = 4;
    c->prefetch_max_mb = 1024;
    c->prefetch_low_io = 0;
    c->png_cache = LG_PC_OFF;
    c->cache_max_mb = 512;
    c->cache_min_kb = 32;
    c->layout_min = 16;
    c->cache_dir[0] = 0;
}

const char *lg_mode_name(int m) { return m == LG_OFF ? "off" : m == LG_ON ? "on" : "measure"; }

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    return s;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

int lg_config_parse(lg_config *c, const char *text, char *err, size_t errn)
{
    int bad = 0, line = 0;
    if (err && errn) err[0] = 0;
    const char *p = text ? text : "";
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char buf[512];
        if (n >= sizeof buf) n = sizeof buf - 1;
        memcpy(buf, p, n); buf[n] = 0;
        p = e ? e + 1 : p + strlen(p);
        line++;
        for (char *q = buf; *q; q++)
            if ((*q == '#' || *q == ';') && (q == buf || q[-1] == ' ' || q[-1] == '\t')) { *q = 0; break; }
        char *s = trim(buf);
        if (!*s || *s == '[') continue;
        char *eq = strchr(s, '=');
        int ok = 0;
        if (eq) {
            *eq = 0;
            char *k = trim(s), *v = trim(eq + 1), *end = NULL;
            long iv = strtol(v, &end, 10);
            int isnum = *v && end && !*end;
            if (!_stricmp(k, "mode")) {
                ok = 1;
                if (!_stricmp(v, "off")) c->mode = LG_OFF;
                else if (!_stricmp(v, "measure")) c->mode = LG_MEASURE;
                else if (!_stricmp(v, "on")) c->mode = LG_ON;
                else ok = 0;
            }
#define KI(name, lo, hi) else if (!_stricmp(k, #name)) { ok = isnum; if (ok) c->name = clampi((int)iv, lo, hi); }
            KI(csv, 0, 1) KI(max_records, 0, LG_MAX_RECORDS) KI(slow_log_ms, 0, 600000) KI(log_budget, 0, 100000)
            KI(manifest, 0, 1) KI(manifest_game, 0, 1) KI(prefetch_threads, 1, 16) KI(prefetch_max_mb, 1, 65536)
            KI(prefetch_low_io, 0, 1) KI(cache_max_mb, 0, 65536) KI(cache_min_kb, 0, 1 << 20) KI(layout_min, 1, 100000)
            else if (!_stricmp(k, "png_cache")) {
                static const char *pc[] = { "off", "build", "verify", "on" };
                for (int i = 0; i < 4; i++) if (!_stricmp(v, pc[i])) { c->png_cache = i; ok = 1; }
            }
            else if (!_stricmp(k, "cache_dir")) { ok = strlen(v) < sizeof c->cache_dir; if (ok) strcpy(c->cache_dir, v); }
#undef KI
        }
        if (!ok) {
            if (!bad && err && errn) snprintf(err, errn, "line %d: '%s'", line, s);
            bad++;
        }
    }
    return bad;
}

const char *lg_stage_name(int st)
{
    static const char *n[LG_ST_N] = { "opts", "stat", "open", "read", "decode", "upload", "obj", "stamp" };
    return st >= 0 && st < LG_ST_N ? n[st] : "?";
}
const char *lg_ext_name(int e)
{
    static const char *n[LG_EXTS] = { "dds", "png", "jpg", "tga", "other" };
    return e >= 0 && e < LG_EXTS ? n[e] : "?";
}

int lg_ext_of(const char *p, size_t n)
{
    size_t i = n;
    while (i && p[i - 1] != '.' && p[i - 1] != '/' && p[i - 1] != '\\') i--;
    if (!i || p[i - 1] != '.') return 4;
    const char *e = p + i; size_t m = n - i;
    if (m == 3 && !_strnicmp(e, "dds", 3)) return 0;
    if (m == 3 && !_strnicmp(e, "png", 3)) return 1;
    if ((m == 3 && !_strnicmp(e, "jpg", 3)) || (m == 4 && !_strnicmp(e, "jpeg", 4))) return 2;
    if (m == 3 && !_strnicmp(e, "tga", 3)) return 3;
    return 4;
}

uint64_t lg_hash(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        h = (h ^ c) * 1099511628211ull;
    }
    return h ? h : 1;
}

/* ------------------------------------------------------------------ state */

int lg_init(lg_state *s, const lg_config *c)
{
    memset(s, 0, sizeof *s);
    s->cfg = *c;
    InitializeSRWLock(&s->lock);
    QueryPerformanceFrequency(&s->freq);
    QueryPerformanceCounter(&s->t0);
    if (c->max_records > 0) {
        s->recs = calloc((size_t)c->max_records, sizeof *s->recs);
        if (!s->recs) return -1;
    }
    snprintf(s->phases[0], sizeof s->phases[0], "startup");
    s->nphases = 1;
    return 0;
}

void lg_free(lg_state *s) { free(s->recs); s->recs = NULL; s->nrecs = 0; }

double lg_now_ms(const lg_state *s)
{
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    return (double)(q.QuadPart - s->t0.QuadPart) * 1000.0 / (double)s->freq.QuadPart;
}

int lg_set_phase(lg_state *s, const char *name)
{
    AcquireSRWLockExclusive(&s->lock);
    int i = s->nphases < LG_PHASES ? s->nphases++ : LG_PHASES - 1;
    snprintf(s->phases[i], sizeof s->phases[i], "%s", name && *name ? name : "?");
    s->cur_phase = i;
    ReleaseSRWLockExclusive(&s->lock);
    return i;
}

int lg_root(lg_state *s, const void *key, const char *dir)
{
    if (!key || !dir) return 255;
    AcquireSRWLockShared(&s->lock);
    for (int i = 0; i < s->nroots; i++)
        if (s->root_key[i] == key && !strcmp(s->roots[i], dir)) { ReleaseSRWLockShared(&s->lock); return i; }
    ReleaseSRWLockShared(&s->lock);
    AcquireSRWLockExclusive(&s->lock);
    int r = 255;
    for (int i = 0; i < s->nroots; i++)
        if (s->root_key[i] == key && !strcmp(s->roots[i], dir)) { r = i; break; }
    if (r == 255 && s->nroots < LG_ROOTS && strlen(dir) < sizeof s->roots[0]) {
        r = s->nroots++;
        s->root_key[r] = key;
        strcpy(s->roots[r], dir);
    }
    ReleaseSRWLockExclusive(&s->lock);
    return r;
}

void lg_add(lg_state *s, const lg_rec *r)
{
    AcquireSRWLockExclusive(&s->lock);
    int ph = r->phase < LG_PHASES ? r->phase : LG_PHASES - 1;
    lg_agg *a = &s->agg[ph][r->main ? 1 : 0][r->ext < LG_EXTS ? r->ext : LG_EXTS - 1];
    a->n++;
    if (!r->ok) a->failed++;
    if (r->prefetched) a->prefetched++;
    a->bytes += r->bytes;
    a->total_ms += r->total_ms;
    for (int i = 0; i < LG_ST_N; i++) a->st[i] += r->st[i];
    s->total_loads++;
    if (r->main) s->main_ms += r->total_ms;
    int ti = 0;
    while (ti < s->nthr && s->thr[ti].tid != r->tid) ti++;
    if (ti == s->nthr && s->nthr < LG_THREADS) s->thr[s->nthr++].tid = r->tid;
    if (ti < s->nthr) { s->thr[ti].n++; s->thr[ti].bytes += r->bytes; s->thr[ti].ms += r->total_ms; }
    if (s->recs && s->nrecs < s->cfg.max_records) s->recs[s->nrecs++] = *r;
    else s->recs_dropped++;
    ReleaseSRWLockExclusive(&s->lock);
}

/* ------------------------------------------------------------------ report */

#define APP(...) do { if (o < n) { int k_ = snprintf(buf + o, n - o, __VA_ARGS__); if (k_ > 0) o += (size_t)k_; } } while (0)

static int cmp_rec_total(const void *a, const void *b)
{
    const lg_rec *x = *(const lg_rec *const *)a, *y = *(const lg_rec *const *)b;
    return x->total_ms < y->total_ms ? 1 : x->total_ms > y->total_ms ? -1 : 0;
}

int lg_format_report(lg_state *s, char *buf, size_t n, int top)
{
    size_t o = 0;
    if (!n) return 0;
    buf[0] = 0;
    AcquireSRWLockShared(&s->lock);
    APP("texture loads: %llu (records kept %d, dropped %d) | main-thread total %.0f ms\n",
        (unsigned long long)s->total_loads, s->nrecs, s->recs_dropped, s->main_ms);
    APP("stage split (ms, summed over loads; stat = PhysFS getRealDir probes, open = PhysFS openRead probes + open,\n"
        " read = file read, decode = DDS/PNG/JPG parse, upload = DX11 pixel buffer (CreateTexture2D), obj = texture\n"
        " object incl. stamp = second getRealDir for the file stamp)\n");
    APP("%-14s %-6s %-5s %6s %5s %8s %9s", "phase", "thread", "type", "n", "fail", "MB", "total");
    for (int i = 0; i < LG_ST_N; i++) APP(" %8s", lg_stage_name(i));
    APP(" %5s\n", "pref");
    for (int p = 0; p < s->nphases; p++)
        for (int m = 1; m >= 0; m--)
            for (int e = 0; e < LG_EXTS; e++) {
                const lg_agg *a = &s->agg[p][m][e];
                if (!a->n) continue;
                APP("%-14.14s %-6s %-5s %6u %5u %8.1f %9.0f", s->phases[p], m ? "main" : "worker", lg_ext_name(e),
                    a->n, a->failed, (double)a->bytes / 1048576.0, a->total_ms);
                for (int i = 0; i < LG_ST_N; i++) APP(" %8.0f", a->st[i]);
                APP(" %5u\n", a->prefetched);
            }
    /* per-load averages over everything, the "why" in one line */
    lg_agg all = { 0 };
    for (int p = 0; p < s->nphases; p++)
        for (int m = 0; m < 2; m++)
            for (int e = 0; e < LG_EXTS; e++) {
                const lg_agg *a = &s->agg[p][m][e];
                all.n += a->n; all.bytes += a->bytes; all.total_ms += a->total_ms;
                for (int i = 0; i < LG_ST_N; i++) all.st[i] += a->st[i];
            }
    if (all.n && all.total_ms > 0) {
        APP("share of total load time:");
        for (int i = 0; i < LG_ST_N; i++) APP(" %s %.1f%%", lg_stage_name(i), 100.0 * all.st[i] / all.total_ms);
        APP(" | avg %.2f ms/texture, %.1f KB\n", all.total_ms / all.n, (double)all.bytes / 1024.0 / all.n);
    }
    if (s->nthr) {
        APP("per thread (the loads of one thread are serial):\n");
        for (int i = 0; i < s->nthr; i++)
            APP("  tid %6lu%s n=%6u %9.0f ms %8.1f MB\n", (unsigned long)s->thr[i].tid, "", s->thr[i].n, s->thr[i].ms,
                (double)s->thr[i].bytes / 1048576.0);
    }
    /* what is left per file: fixed (path lookup: stat + open + stamp) vs per byte (read, decode, upload) */
    {
        double fx = 0, rest = 0, small_t = 0; int ns = 0, nn = 0;
        for (int i = 0; i < s->nrecs; i++) {
            const lg_rec *r = &s->recs[i];
            if (!r->complete) continue;
            nn++;
            fx += r->st[LG_ST_STAT] + r->st[LG_ST_OPEN] + r->st[LG_ST_STAMP];
            rest += r->total_ms - (r->st[LG_ST_STAT] + r->st[LG_ST_OPEN] + r->st[LG_ST_STAMP]);
            if (r->bytes < 65536) { ns++; small_t += r->total_ms; }
        }
        if (nn)
            APP("per file (kept records): path lookup (stat+open+stamp) %.2f ms, rest (opts/read/decode/upload/obj) %.2f ms;"
                " files < 64 KB: %d, %.2f ms each (= the fixed cost)\n", fx / nn, rest / nn, ns, ns ? small_t / ns : 0.0);
    }
    if (top > 0 && s->nrecs) {
        const lg_rec **v = malloc((size_t)s->nrecs * sizeof *v);
        if (v) {
            for (int i = 0; i < s->nrecs; i++) v[i] = &s->recs[i];
            qsort(v, (size_t)s->nrecs, sizeof *v, cmp_rec_total);
            APP("slowest loads (ms: total stat open read decode upload obj | KB | thread | requester RVAs):\n");
            for (int i = 0; i < top && i < s->nrecs; i++) {
                const lg_rec *r = v[i];
                APP("  %7.1f %6.1f %6.1f %6.1f %6.1f %6.1f %6.1f %8.0f %-6s %06x %06x %06x  %s\n", r->total_ms,
                    r->st[LG_ST_STAT], r->st[LG_ST_OPEN], r->st[LG_ST_READ], r->st[LG_ST_DECODE], r->st[LG_ST_UPLOAD],
                    r->st[LG_ST_OBJ], (double)r->bytes / 1024.0, r->main ? "main" : "worker", r->req[0], r->req[1],
                    r->req[2], r->path);
            }
            free(v);
        }
    }
    ReleaseSRWLockShared(&s->lock);
    return (int)(o < n ? o : n - 1);
}

int lg_write_csv(lg_state *s, const char *path)
{
    char tmp[MAX_PATH + 8]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    fprintf(f, "t_ms,phase,tid,main,ext,bytes,ok,complete,via_cache,prefetched,total_ms");
    for (int i = 0; i < LG_ST_N; i++) fprintf(f, ",%s_ms", lg_stage_name(i));
    for (int i = 0; i < LG_FRAMES; i++) fprintf(f, ",req%d", i);
    fprintf(f, ",img,root,path\n");
    AcquireSRWLockShared(&s->lock);
    for (int i = 0; i < s->nrecs; i++) {
        const lg_rec *r = &s->recs[i];
        fprintf(f, "%.1f,%s,%lu,%d,%s,%llu,%d,%d,%d,%d,%.3f", r->t_ms, s->phases[r->phase < LG_PHASES ? r->phase : 0],
                (unsigned long)r->tid, r->main, lg_ext_name(r->ext), (unsigned long long)r->bytes, r->ok, r->complete,
                r->via_cache, r->prefetched, r->total_ms);
        for (int k = 0; k < LG_ST_N; k++) fprintf(f, ",%.3f", r->st[k]);
        for (int k = 0; k < LG_FRAMES; k++) fprintf(f, ",0x%x", r->req[k]);
        fputc(',', f);
        for (int k = 0; r->has_img && k < LG_IMG; k++) fprintf(f, "%02x", r->img[k]);
        const char *root = r->root < s->nroots ? s->roots[r->root] : "";
        fprintf(f, ",\"%s\",\"%s\"\n", root, r->path);
    }
    ReleaseSRWLockShared(&s->lock);
    fclose(f);
    return MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}

static int default_stat(const char *abs, uint64_t *size, uint64_t *mtime)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(abs, GetFileExInfoStandard, &d) || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    *size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    *mtime = ((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
    return 1;
}

static void join_path(char *out, size_t n, const char *root, const char *rel)
{
    size_t rl = strlen(root);
    int sep = rl && (root[rl - 1] == '/' || root[rl - 1] == '\\');
    while (*rel == '/' || *rel == '\\') rel++;
    snprintf(out, n, "%s%s%s", root, sep ? "" : "/", rel);
}

int lg_write_manifest(lg_state *s, const char *path,
                      int (*stat_fn)(const char *abs, uint64_t *size, uint64_t *mtime))
{
    if (!stat_fn) stat_fn = default_stat;
    char tmp[MAX_PATH + 8]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    fprintf(f, "# lazy_gfx prefetch manifest v1: size<TAB>mtime(FILETIME hex)<TAB>virtual path<TAB>file\n");
    int written = 0;
    AcquireSRWLockShared(&s->lock);
    int cap = 1; while (cap < s->nrecs * 2 + 2) cap <<= 1;
    uint64_t *seen = calloc((size_t)cap, sizeof *seen);
    for (int i = 0; seen && i < s->nrecs; i++) {
        const lg_rec *r = &s->recs[i];
        if (!r->complete || !r->ok || r->root >= s->nroots) continue;
        uint64_t h = lg_hash(r->path, strlen(r->path));
        size_t k = (size_t)h & (size_t)(cap - 1);
        int dup = 0;
        while (seen[k]) { if (seen[k] == h) { dup = 1; break; } k = (k + 1) & (size_t)(cap - 1); }
        if (dup) continue;
        seen[k] = h;
        char abs[600]; uint64_t sz = 0, mt = 0;
        join_path(abs, sizeof abs, s->roots[r->root], r->path);
        if (!stat_fn(abs, &sz, &mt)) continue;
        fprintf(f, "%llu\t%llx\t%s\t%s\n", (unsigned long long)sz, (unsigned long long)mt, r->path, abs);
        written++;
    }
    ReleaseSRWLockShared(&s->lock);
    free(seen);
    fclose(f);
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) return -1;
    return written;
}

/* ------------------------------------------------------------------ prefetch */

void lg_pf_free(lg_prefetch *pf)
{
    for (int i = 0; i < pf->n; i++) free(pf->items[i].path);
    free(pf->items);
    memset(pf, 0, sizeof *pf);
}

int lg_pf_load(lg_prefetch *pf, const char *manifest_path, uint64_t max_bytes)
{
    memset(pf, 0, sizeof *pf);
    pf->max_bytes = max_bytes;
    FILE *f = fopen(manifest_path, "rb");
    if (!f) return -1;
    char line[1400];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\r' || line[0] == '\n') continue;
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
        char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL, *t3 = t2 ? strchr(t2 + 1, '\t') : NULL;
        if (!t3 || !t3[1]) continue;
        *t1 = *t2 = *t3 = 0;
        if (pf->n == pf->cap) {
            int nc = pf->cap ? pf->cap * 2 : 256;
            lg_pf_item *ni = realloc(pf->items, (size_t)nc * sizeof *ni);
            if (!ni) break;
            pf->items = ni; pf->cap = nc;
        }
        lg_pf_item *it = &pf->items[pf->n];
        memset(it, 0, sizeof *it);
        it->size = strtoull(line, NULL, 10);
        it->mtime = strtoull(t1 + 1, NULL, 16);
        it->vh = lg_hash(t2 + 1, strlen(t2 + 1));
        it->path = _strdup(t3 + 1);
        if (!it->path) break;
        pf->n++;
    }
    fclose(f);
    return pf->n;
}

typedef struct { lg_prefetch *pf; int low_io; } pf_arg;

static DWORD WINAPI pf_thread(void *p)
{
    pf_arg *a = p;
    lg_prefetch *pf = a->pf;
    if (a->low_io) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const DWORD BUF = 1 << 20;
    uint8_t *buf = VirtualAlloc(NULL, BUF, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) return 1;
    for (;;) {
        if (pf->stop) break;
        LONG i = InterlockedIncrement(&pf->next) - 1;
        if (i >= pf->n) break;
        lg_pf_item *it = &pf->items[i];
        if (pf->max_bytes && (uint64_t)pf->bytes_read >= pf->max_bytes) { it->state = LG_PF_SKIP; InterlockedIncrement(&pf->skipped); continue; }
        InterlockedExchange(&it->state, LG_PF_BUSY);
        WIN32_FILE_ATTRIBUTE_DATA d;
        uint64_t sz = 0, mt = 0;
        if (GetFileAttributesExA(it->path, GetFileExInfoStandard, &d)) {
            sz = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
            mt = ((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
        }
        if (!sz || sz != it->size || mt != it->mtime) {   /* gone or changed since the manifest: skip */
            InterlockedExchange(&it->state, LG_PF_SKIP); InterlockedIncrement(&pf->skipped); continue;
        }
        HANDLE h = CreateFileA(it->path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (h == INVALID_HANDLE_VALUE) { InterlockedExchange(&it->state, LG_PF_SKIP); InterlockedIncrement(&pf->failed); continue; }
        DWORD got = 0; uint64_t total = 0;
        while (!pf->stop && ReadFile(h, buf, BUF, &got, NULL) && got) total += got;
        CloseHandle(h);
        InterlockedExchangeAdd64(&pf->bytes_read, (LONG64)total);
        InterlockedExchange(&it->state, LG_PF_DONE);
        InterlockedIncrement(&pf->done);
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    return 0;
}

int lg_pf_run(lg_prefetch *pf, int nthreads, int low_io)
{
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 16) nthreads = 16;
    LARGE_INTEGER fq, a, b; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&a);
    pf_arg arg = { pf, low_io };
    HANDLE th[16]; int nt = 0;
    for (int i = 0; i < nthreads; i++) {
        th[nt] = CreateThread(NULL, 0, pf_thread, &arg, 0, NULL);
        if (th[nt]) nt++;
    }
    if (!nt) pf_thread(&arg);
    if (nt) WaitForMultipleObjects((DWORD)nt, th, TRUE, INFINITE);
    for (int i = 0; i < nt; i++) CloseHandle(th[i]);
    QueryPerformanceCounter(&b);
    pf->ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)fq.QuadPart;
    return pf->done;
}

int lg_pf_was_read(const lg_prefetch *pf, uint64_t vh)
{
    for (int i = 0; pf && i < pf->n; i++)
        if (pf->items[i].vh == vh) return pf->items[i].state == LG_PF_DONE;
    return 0;
}
