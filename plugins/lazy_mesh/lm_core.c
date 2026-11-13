/* lazy_mesh core, see lm_core.h */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lm_core.h"

/* ------------------------------------------------------------------ config */

static void list_set(char dst[][LM_LIST_STR], int *n, const char *csv, int lower_path)
{
    *n = 0;
    const char *p = csv;
    while (*p && *n < LM_MAX_LIST) {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        const char *s = p;
        while (*p && *p != ',') p++;
        const char *e = p;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
        if (e > s) {
            char tmp[LM_LIST_STR];
            size_t k = (size_t)(e - s) < sizeof tmp - 1 ? (size_t)(e - s) : sizeof tmp - 1;
            memcpy(tmp, s, k); tmp[k] = 0;
            if (lower_path) lm_norm_path(tmp, dst[*n], LM_LIST_STR);
            else { for (size_t i = 0; i <= k; i++) dst[*n][i] = tmp[i]; }
            (*n)++;
        }
    }
}

void lm_config_defaults(lm_config *c)
{
    memset(c, 0, sizeof *c);
    c->mode = LM_MODE_OFF;
    c->skip_debug = 1;
    c->defer_all = 0;
    list_set(c->defer_dirs, &c->n_defer_dirs, "gfx/models/portraits/,gfx/models/military/", 1);
    list_set(c->eager_contains, &c->n_eager_contains, "bridge", 0);
    snprintf(c->eager_file, sizeof c->eager_file, "lazy_mesh_eager.txt");
    c->prefetch_after_s = 0;
    c->wait_timeout_ms = 60000;
    c->log_loads = 1;
    c->startup_from_report = 1;
    c->prefetch_on_game = 1;
    c->prefetch_all = 0;
    c->prefetch_gap_ms = 20;
    c->merge_report = 1;
}

static int parse_int(const char *v, int lo, int hi, int *out)
{
    char *end; long x = strtol(v, &end, 10);
    if (end == v || *end || x < lo || x > hi) return -1;
    *out = (int)x; return 0;
}

int lm_config_parse(lm_config *c, const char *text, char *err, size_t errn)
{
    int bad = 0, line = 0;
    if (err && errn) err[0] = 0;
    const char *p = text;
    while (*p) {
        line++;
        const char *ls = p;
        while (*p && *p != '\n') p++;
        const char *le = p;
        if (*p) p++;
        char buf[1024];
        size_t n = (size_t)(le - ls) < sizeof buf - 1 ? (size_t)(le - ls) : sizeof buf - 1;
        memcpy(buf, ls, n); buf[n] = 0;
        for (char *q = buf; *q; q++) if (*q == '#' || *q == ';') { *q = 0; break; }
        char *s = buf; while (*s == ' ' || *s == '\t' || *s == '\r') s++;
        char *e = s + strlen(s); while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
        if (!*s) continue;
        char *eq = strchr(s, '=');
        const char *why = NULL;
        if (!eq) why = "expected key = value";
        else {
            *eq = 0;
            char *k = s, *ke = eq; while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
            char *v = eq + 1; while (*v == ' ' || *v == '\t') v++;
            int iv;
            if (!*v) why = "empty value";
            else if (!strcmp(k, "mode")) {
                if (!strcmp(v, "off")) c->mode = LM_MODE_OFF;
                else if (!strcmp(v, "measure")) c->mode = LM_MODE_MEASURE;
                else if (!strcmp(v, "lazy")) c->mode = LM_MODE_LAZY;
                else why = "mode must be off|measure|lazy";
            }
            else if (!strcmp(k, "skip_debug")) { if (parse_int(v, 0, 1, &iv)) why = "0|1"; else c->skip_debug = iv; }
            else if (!strcmp(k, "defer_all")) { if (parse_int(v, 0, 1, &iv)) why = "0|1"; else c->defer_all = iv; }
            else if (!strcmp(k, "startup_from_report")) { if (parse_int(v, 0, 1, &iv)) why = "0|1"; else c->startup_from_report = iv; }
            else if (!strcmp(k, "prefetch_on_game")) { if (parse_int(v, 0, 1, &iv)) why = "0|1"; else c->prefetch_on_game = iv; }
            else if (!strcmp(k, "merge_report")) { if (parse_int(v, 0, 1, &iv)) why = "0|1"; else c->merge_report = iv; }
            else if (!strcmp(k, "prefetch_all")) { if (parse_int(v, 0, 1, &iv)) why = "0|1"; else c->prefetch_all = iv; }
            else if (!strcmp(k, "prefetch_gap_ms")) { if (parse_int(v, 0, 10000, &iv)) why = "0..10000"; else c->prefetch_gap_ms = iv; }
            else if (!strcmp(k, "log_loads")) { if (parse_int(v, 0, 1, &iv)) why = "0|1"; else c->log_loads = iv; }
            else if (!strcmp(k, "prefetch_after_s")) { if (parse_int(v, 0, 86400, &iv)) why = "0..86400"; else c->prefetch_after_s = iv; }
            else if (!strcmp(k, "wait_timeout_ms")) { if (parse_int(v, 100, 600000, &iv)) why = "100..600000"; else c->wait_timeout_ms = iv; }
            else if (!strcmp(k, "defer_dirs")) {
                if (!strcmp(v, "none")) c->n_defer_dirs = 0; else list_set(c->defer_dirs, &c->n_defer_dirs, v, 1);
            }
            else if (!strcmp(k, "eager_contains")) {
                if (!strcmp(v, "none")) c->n_eager_contains = 0; else list_set(c->eager_contains, &c->n_eager_contains, v, 0);
            }
            else if (!strcmp(k, "eager_file")) {
                if (strlen(v) >= sizeof c->eager_file || strchr(v, '/') || strchr(v, '\\') || strstr(v, ".."))
                    why = "plain file name in v3x/ expected";
                else snprintf(c->eager_file, sizeof c->eager_file, "%s", v);
            }
            else why = "unknown key";
        }
        if (why) {
            if (!bad && err && errn) snprintf(err, errn, "line %d: %s", line, why);
            bad++;
        }
    }
    return bad;
}

/* ------------------------------------------------------------------ names */

static uint32_t fnv(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}

static int ns_grow(lm_nameset *s)
{
    uint32_t nc = s->cap ? s->cap * 2 : 256;
    char **ns = calloc(nc, sizeof *ns);
    if (!ns) return -1;
    for (uint32_t i = 0; i < s->cap; i++) {
        char *v = s->slot[i];
        if (!v) continue;
        uint32_t h = fnv(v, strlen(v)) & (nc - 1);
        while (ns[h]) h = (h + 1) & (nc - 1);
        ns[h] = v;
    }
    free(s->slot); s->slot = ns; s->cap = nc;
    return 0;
}

int lm_nameset_add(lm_nameset *s, const char *name, size_t len)
{
    if (!len) return 0;
    if ((s->n + 1) * 2 > s->cap && ns_grow(s)) return -1;
    uint32_t h = fnv(name, len) & (s->cap - 1);
    while (s->slot[h]) {
        if (strlen(s->slot[h]) == len && !memcmp(s->slot[h], name, len)) return 0;
        h = (h + 1) & (s->cap - 1);
    }
    char *v = malloc(len + 1);
    if (!v) return -1;
    memcpy(v, name, len); v[len] = 0;
    s->slot[h] = v; s->n++;
    return 1;
}

int lm_nameset_has(const lm_nameset *s, const char *name)
{
    if (!s || !s->cap || !name) return 0;
    size_t len = strlen(name);
    uint32_t h = fnv(name, len) & (s->cap - 1);
    while (s->slot[h]) {
        if (!strcmp(s->slot[h], name)) return 1;
        h = (h + 1) & (s->cap - 1);
    }
    return 0;
}

void lm_nameset_free(lm_nameset *s)
{
    for (uint32_t i = 0; i < s->cap; i++) free(s->slot[i]);
    free(s->slot); s->slot = NULL; s->cap = s->n = 0;
}

int lm_nameset_parse(lm_nameset *s, const char *text)
{
    int added = 0;
    const char *p = text;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        if (*p == '#' || *p == ';') { while (*p && *p != '\n') p++; continue; }
        const char *s0 = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        if (lm_nameset_add(s, s0, (size_t)(p - s0)) > 0) added++;
        while (*p && *p != '\n') p++;
    }
    return added;
}

void lm_norm_path(const char *in, char *out, size_t cap)
{
    if (!cap) return;
    size_t k = 0;
    while (in[0] == '.' && (in[1] == '/' || in[1] == '\\')) in += 2;
    for (; *in && k + 1 < cap; in++) {
        char ch = *in;
        if (ch == '\\') ch = '/';
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        if (ch == '/' && k && out[k - 1] == '/') continue;   /* collapse "//" */
        out[k++] = ch;
    }
    out[k] = 0;
}

int lm_decide(const lm_config *c, const lm_nameset *eager, const lm_nameset *prev_startup, const char *name, const char *path)
{
    if (c->mode != LM_MODE_LAZY) return LM_EAGER_MODE;
    if (lm_nameset_has(eager, name)) return LM_EAGER_LISTED;
    if (c->startup_from_report && lm_nameset_has(prev_startup, name)) return LM_EAGER_PREV;
    for (int i = 0; i < c->n_eager_contains; i++)
        if (c->eager_contains[i][0] && strstr(name, c->eager_contains[i])) return LM_EAGER_RULE;
    char np[260];
    lm_norm_path(path ? path : "", np, sizeof np);
    if (c->skip_debug && strstr(np, "gfx/models/debug/")) return LM_DEFER_DEBUG;
    for (int i = 0; i < c->n_defer_dirs; i++)
        if (c->defer_dirs[i][0] && strstr(np, c->defer_dirs[i])) return LM_DEFER_DIR;
    if (c->defer_all) return LM_DEFER_ALL;
    return LM_EAGER_DEFAULT;
}

/* ------------------------------------------------------------------ table */

const char *lm_site_name(int site)
{
    static const char *n[LM_SITE_COUNT] = { "-", "mesh_object", "entity_override", "entity_blendshape", "prefetch", "console" };
    return site >= 0 && site < LM_SITE_COUNT ? n[site] : "?";
}

int lm_table_init(lm_table *t, uint32_t cap_pow2)
{
    memset(t, 0, sizeof *t);
    t->e = VirtualAlloc(NULL, (size_t)cap_pow2 * sizeof(lm_entry), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!t->e) return -1;
    t->cap = cap_pow2;
    return 0;
}

void lm_table_free(lm_table *t)
{
    if (t->e) VirtualFree(t->e, 0, MEM_RELEASE);
    memset(t, 0, sizeof *t);
}

static uint32_t ptr_hash(const void *p, uint32_t cap)
{
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    return (uint32_t)x & (cap - 1);
}

lm_entry *lm_table_find(const lm_table *t, const void *key)
{
    if (!t->e || !key) return NULL;
    uint32_t h = ptr_hash(key, t->cap);
    for (uint32_t i = 0; i < t->cap; i++) {
        lm_entry *e = &t->e[(h + i) & (t->cap - 1)];
        int64_t k = e->key;
        if (k == (int64_t)(uintptr_t)key) return e;
        if (k == 0) return NULL;
    }
    return NULL;
}

lm_entry *lm_table_insert(lm_table *t, const void *key, int *created)
{
    *created = 0;
    if (!t->e || !key) return NULL;
    /* keep the load factor <= 3/4 so probes stay short; beyond that the caller loads eagerly */
    uint32_t h = ptr_hash(key, t->cap);
    for (uint32_t i = 0; i < t->cap; i++) {
        lm_entry *e = &t->e[(h + i) & (t->cap - 1)];
        int64_t k = e->key;
        if (k == (int64_t)(uintptr_t)key) return e;
        if (k == 0) {
            if ((uint32_t)t->n >= t->cap / 4 * 3) { InterlockedIncrement((volatile LONG *)&t->full); return NULL; }
            int64_t prev = InterlockedCompareExchange64((volatile LONG64 *)&e->key, (LONG64)(uintptr_t)key, 0);
            if (prev == 0) { InterlockedIncrement((volatile LONG *)&t->n); e->phase = -1; *created = 1; return e; }
            if (prev == (int64_t)(uintptr_t)key) return e;
            /* someone else took this slot for another key: continue probing */
        }
    }
    InterlockedIncrement((volatile LONG *)&t->full);
    return NULL;
}

int lm_ensure(lm_table *t, void *key, int32_t tid, lm_load_fn load, void *user, unsigned timeout_ms, lm_entry **out)
{
    lm_entry *e = lm_table_find(t, key);
    if (out) *out = e;
    if (!e) return LM_R_UNTRACKED;
    int waited = 0;
    DWORD t0 = 0;
    for (unsigned spin = 0;; spin++) {
        LONG st = e->state;
        if (st == LM_ST_DONE || st == LM_ST_EMPTY) return waited ? LM_R_WAITED : LM_R_READY;
        if (st == LM_ST_DEFERRED) {
            if (InterlockedCompareExchange((volatile LONG *)&e->state, LM_ST_LOADING, LM_ST_DEFERRED) == LM_ST_DEFERRED) {
                InterlockedExchange((volatile LONG *)&e->owner, tid);
                load(key, user);
                InterlockedExchange((volatile LONG *)&e->owner, 0);
                InterlockedExchange((volatile LONG *)&e->state, LM_ST_DONE);
                return LM_R_LOADED;
            }
            continue;
        }
        /* LOADING */
        if (e->owner == tid) return LM_R_REENTRANT;
        if (!waited) { waited = 1; t0 = GetTickCount(); }
        else if (GetTickCount() - t0 >= timeout_ms) return LM_R_TIMEOUT;
        if (spin < 64) YieldProcessor(); else if (spin < 256) SwitchToThread(); else Sleep(1);
    }
}

void lm_note_use(lm_entry *e, int site, double now_ms, int phase)
{
    if (!e) return;
    if (InterlockedIncrement((volatile LONG *)&e->uses) == 1) {
        e->first_ms = now_ms;
        e->phase = phase;
        InterlockedExchange((volatile LONG *)&e->first_site, site);
    }
}

/* ------------------------------------------------------------------ report */

typedef struct { size_t n, cap; char *out; } sbuf;
static void sb_printf(sbuf *b, const char *fmt, ...)
{
    if (b->n + 1 >= b->cap) return;
    va_list ap; va_start(ap, fmt);
    int w = vsnprintf(b->out + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (w < 0) return;
    b->n += (size_t)w < b->cap - b->n ? (size_t)w : b->cap - b->n - 1;
}

static const char *decision_name(int d)
{
    switch (d) {
    case LM_EAGER_MODE: return "eager";
    case LM_EAGER_LISTED: return "eager:listed";
    case LM_EAGER_RULE: return "eager:rule";
    case LM_EAGER_DEFAULT: return "eager:default";
    case LM_EAGER_PREV: return "eager:prev_startup";
    case LM_DEFER_DEBUG: return "deferred:debug";
    case LM_DEFER_DIR: return "deferred:dir";
    case LM_DEFER_ALL: return "deferred:all";
    default: return "?";
    }
}

size_t lm_format_report(const lm_table *t, const lm_report_info *in, char *out, size_t cap)
{
    sbuf b = { 0, cap, out };
    if (!cap) return 0;
    out[0] = 0;
    int seen = 0, used = 0, deferred = 0, pending = 0, lazy = 0;
    double pass_ms = 0, saved_ms = 0, lazy_ms = 0;
    for (uint32_t i = 0; i < t->cap; i++) {
        const lm_entry *e = &t->e[i];
        if (!e->key) continue;
        seen++;
        if (e->uses) used++;
        if (LM_IS_DEFER(e->decision)) deferred++;
        if (e->state == LM_ST_DEFERRED) pending++;
        if (e->lazy_ms > 0) { lazy++; lazy_ms += e->lazy_ms; }
        pass_ms += e->pass_ms;
    }
    (void)saved_ms;
    static const char *modes[] = { "off", "measure", "lazy" };
    sb_printf(&b, "# V3X lazy_mesh report, mode=%s, game %s\n", modes[in->mode < 0 || in->mode > 2 ? 0 : in->mode], in->version ? in->version : "?");
    sb_printf(&b, "# types=%d used=%d deferred_at_start=%d still_unloaded=%d loaded_on_demand=%d (%.0f ms) pass_load_sum=%.0f ms table_full=%d\n",
              seen, used, deferred, pending, lazy, lazy_ms, pass_ms, (int)t->full);
    sb_printf(&b, "# A data line = a type requested at a choke point (CMeshObject ctor, entity override, entity blend shape).\n"
                  "# This file can be copied to v3x/lazy_mesh_eager.txt (only the first column is read).\n"
                  "# columns: name  first_use_s(after pass end)  phase  uses  first_site  decision  pass_ms  lazy_ms  path\n");
    const lm_prev *pv = in->prev;
    lm_nameset cur = { 0 };   /* names used in this session (to find the carried-over ones) */
    int carried = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < t->cap; i++) {
            const lm_entry *e = &t->e[i];
            const lm_prev_item *pi = (pv && e->key && e->name[0]) ? lm_prev_find(pv, e->name) : NULL;
            if (!e->key || (pass == 0) != (e->uses > 0 || pi != NULL)) continue;
            const char *ph = (e->phase >= 0 && e->phase < in->n_phases && in->phases) ? in->phases[e->phase] : "start";
            if (pass == 0) {
                int uses = (int)e->uses + (pi ? pi->uses : 0);
                int site = e->first_site;
                if (pi && (pi->site == LM_SITE_BLEND || pi->site == LM_SITE_RDATA || !e->uses)) site = pi->site;
                if (pv) lm_nameset_add(&cur, e->name, strlen(e->name));
                if (!e->uses) carried++;
                sb_printf(&b, "%s\t%.1f\t%s\t%d\t%s\t%s\t%.0f\t%.0f\t%s\n", e->name[0] ? e->name : "?",
                          e->uses ? (e->first_ms - in->pass_end_ms) / 1000.0 : -1.0, e->uses ? ph : "prev", uses, lm_site_name(site),
                          e->uses ? decision_name(e->decision) : "carried", e->pass_ms, e->lazy_ms, e->path);
            }
            else
                sb_printf(&b, "# unused %s\t%s\t%.0f\t%s\n", e->name[0] ? e->name : "?", decision_name(e->decision), e->pass_ms, e->path);
        }
        /* previous-session types not present in this session's table at all (e.g. a mod was removed) */
        if (pass == 0 && pv)
            for (int k = 0; k < pv->n; k++)
                if (!lm_nameset_has(&cur, pv->v[k].name)) {
                    carried++;
                    sb_printf(&b, "%s\t-1.0\tprev\t%d\t%s\tcarried\t0\t0\t\n", pv->v[k].name, pv->v[k].uses, lm_site_name(pv->v[k].site));
                }
    }
    if (pv) sb_printf(&b, "# merged with the previous report: %d type(s) carried over unchanged\n", carried);
    lm_nameset_free(&cur);
    return b.n;
}

/* ------------------------------------------------------------------ misc */

int lm_msvc_str(const void *s, char *out, size_t cap, lm_readable_fn readable)
{
    if (!cap) return -1;
    out[0] = 0;
    if (!s || (readable && !readable(s, 32))) return -1;
    const uint8_t *b = s;
    uint64_t size, scap; memcpy(&size, b + 16, 8); memcpy(&scap, b + 24, 8);
    if (size > scap || scap > (1u << 20)) return -1;
    const char *p;
    if (scap <= 15) p = (const char *)b;
    else { memcpy(&p, b, 8); }
    size_t n = size < cap - 1 ? (size_t)size : cap - 1;
    if (n && readable && !readable(p, n)) return -1;
    memcpy(out, p, n); out[n] = 0;
    return (int)n;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int lm_match(const uint8_t *code, size_t avail, size_t off, const char *pat)
{
    size_t i = off;
    for (const char *p = pat; *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (i >= avail) return 0;
        if (p[0] == '?') { p++; if (*p == '?') p++; i++; continue; }
        int hi = hexv(p[0]), lo = p[1] ? hexv(p[1]) : -1;
        if (hi < 0 || lo < 0) return 0;
        if (code[i] != (uint8_t)(hi * 16 + lo)) return 0;
        p += 2; i++;
    }
    return 1;
}

/* ------------------------------------------------------------------ previous report */

int lm_site_from_name(const char *s)
{
    for (int i = 1; i < LM_SITE_COUNT; i++) if (!strcmp(s, lm_site_name(i))) return i;
    return LM_SITE_NONE;
}

static int prev_cmp(const void *a, const void *b) { return strcmp(((const lm_prev_item *)a)->name, ((const lm_prev_item *)b)->name); }

int lm_prev_parse(lm_prev *p, const char *text)
{
    p->v = NULL; p->n = 0;
    int cap = 0;
    const char *q = text;
    while (*q) {
        const char *ls = q;
        while (*q && *q != '\n') q++;
        const char *le = q;
        if (*q) q++;
        if (ls == le || *ls == '#' || *ls == ';' || *ls == '\r') continue;
        /* name \t first_use \t phase \t uses \t site ... (lm_format_report) */
        char line[512];
        size_t n = (size_t)(le - ls) < sizeof line - 1 ? (size_t)(le - ls) : sizeof line - 1;
        memcpy(line, ls, n); line[n] = 0;
        char *col[6] = {0}; int nc = 0;
        for (char *t = line; nc < 6;) {
            col[nc++] = t;
            char *tab = strchr(t, '\t');
            if (!tab) break;
            *tab = 0; t = tab + 1;
        }
        for (int i = 0; i < nc; i++) { char *e = col[i] + strlen(col[i]); while (e > col[i] && (e[-1] == '\r' || e[-1] == ' ')) *--e = 0; }
        if (!col[0][0] || strlen(col[0]) >= sizeof p->v->name) continue;
        if (p->n == cap) {
            int nc2 = cap ? cap * 2 : 256;
            lm_prev_item *nv = realloc(p->v, (size_t)nc2 * sizeof *nv);
            if (!nv) break;
            p->v = nv; cap = nc2;
        }
        lm_prev_item *it = &p->v[p->n++];
        snprintf(it->name, sizeof it->name, "%s", col[0]);
        it->uses = nc > 3 ? atoi(col[3]) : 1;
        if (it->uses < 1) it->uses = 1;
        it->site = nc > 4 ? lm_site_from_name(col[4]) : LM_SITE_NONE;
    }
    if (p->n > 1) qsort(p->v, (size_t)p->n, sizeof *p->v, prev_cmp);
    return p->n;
}

const lm_prev_item *lm_prev_find(const lm_prev *p, const char *name)
{
    int lo = 0, hi = p->n;
    while (lo < hi) {
        int m = (lo + hi) / 2, c = strcmp(p->v[m].name, name);
        if (!c) return &p->v[m];
        if (c < 0) lo = m + 1; else hi = m;
    }
    return NULL;
}

void lm_prev_free(lm_prev *p) { free(p->v); p->v = NULL; p->n = 0; }

int lm_prev_startup_set(const lm_prev *p, lm_nameset *out)
{
    int n = 0;
    for (int i = 0; i < p->n; i++)
        if (p->v[i].site == LM_SITE_BLEND || p->v[i].site == LM_SITE_RDATA)
            n += lm_nameset_add(out, p->v[i].name, strlen(p->v[i].name)) > 0;
    return n;
}

/* ------------------------------------------------------------------ stats */

void lm_stats_add(lm_stats *s, int site, float ms, int on_main, const char *name)
{
    if (site < 0 || site >= 8) site = 0;
    s->n[site]++; s->ms[site] += ms;
    if (on_main) { s->main_n++; s->main_ms += ms; }
    if (ms > 100) s->over_100++;
    if (ms > 500) s->over_500++;
    if (ms > s->worst_ms) {
        s->worst_ms = ms; s->worst_site = site; s->worst_main = on_main;
        snprintf(s->worst, sizeof s->worst, "%s", name ? name : "?");
    }
}

size_t lm_stats_format(const lm_stats *s, char *out, size_t cap)
{
    sbuf b = { 0, cap, out };
    if (!cap) return 0;
    out[0] = 0;
    int n = 0; double ms = 0;
    for (int i = 0; i < 8; i++) { n += s->n[i]; ms += s->ms[i]; }
    sb_printf(&b, "on-demand loads %d, %.0f ms total, main thread %d (%.0f ms), >100 ms %d, >500 ms %d, worst %.0f ms %s (%s%s)",
              n, ms, s->main_n, s->main_ms, s->over_100, s->over_500, s->worst_ms, s->worst[0] ? s->worst : "-",
              lm_site_name(s->worst_site), s->worst_main ? ", main thread" : "");
    for (int i = 1; i < LM_SITE_COUNT; i++)
        if (s->n[i]) sb_printf(&b, " | %s %d / %.0f ms", lm_site_name(i), s->n[i], s->ms[i]);
    return b.n;
}
