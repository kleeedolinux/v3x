/* map_cache core logic (see mc_core.h). */
#include "mc_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void mc_config_defaults(mc_config *c)
{
    memset(c, 0, sizeof *c);
    c->mode = MC_MEASURE;
    c->spike_ms = 250;
    c->load_log_ms = 50;
    c->load_log_mb = 8;
    c->log_budget = 2000;
    c->big_reserve_mb = 16;
    c->budget_mb = 256;
    c->max_texture_mb = 32;
    c->repeat_min = 2;
    c->max_pins = 1024;
    c->summary_sec = 300;
    c->unknown_size_kb = 1024;
}

const char *mc_mode_name(int m) { return m == MC_ON ? "on" : m == MC_MEASURE ? "measure" : "off"; }

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static int to_int(const char *v, int lo, int hi, int *out)
{
    char *end;
    long x = strtol(v, &end, 10);
    if (end == v || *trim(end) || x < lo || x > hi) return -1;
    *out = (int)x;
    return 0;
}

int mc_config_parse(mc_config *c, const char *text, char *err, size_t errn)
{
    int bad = 0;
    if (err && errn) err[0] = 0;
    const char *p = text;
    int lineno = 0;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[256];
        lineno++;
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len); line[len] = 0;
        p = nl ? nl + 1 : NULL;
        char *s = trim(line);
        char *hash = strpbrk(s, "#;");
        if (hash) *hash = 0;
        s = trim(s);
        if (!*s || *s == '[') continue;
        char *eq = strchr(s, '=');
        int ok = 0;
        if (eq) {
            *eq = 0;
            char *k = trim(s), *v = trim(eq + 1);
            ok = 1;
            if (!_stricmp(k, "mode")) {
                if (!_stricmp(v, "off") || !strcmp(v, "0")) c->mode = MC_OFF;
                else if (!_stricmp(v, "measure")) c->mode = MC_MEASURE;
                else if (!_stricmp(v, "on") || !strcmp(v, "1")) c->mode = MC_ON;
                else ok = 0;
            }
#define INTK(name, lo, hi) else if (!_stricmp(k, #name)) ok = to_int(v, lo, hi, &c->name) == 0;
            INTK(spike_ms, 1, 600000)
            INTK(load_log_ms, 0, 600000)
            INTK(load_log_mb, 0, 4096)
            INTK(log_budget, 0, 1000000)
            INTK(big_reserve_mb, 1, 4096)
            INTK(budget_mb, 0, 16384)
            INTK(max_texture_mb, 0, 4096)
            INTK(repeat_min, 1, 1000)
            INTK(max_pins, 0, MC_MAX_PINS)
            INTK(summary_sec, 0, 86400)
            INTK(unknown_size_kb, 0, 1 << 20)
#undef INTK
            else ok = 0;
        }
        if (!ok) {
            if (!bad && err && errn) snprintf(err, errn, "line %d: '%s'", lineno, s);
            bad++;
        }
    }
    return bad;
}

uint64_t mc_hash(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\\') ch = '/';
        ch = (unsigned char)tolower(ch);
        h = (h ^ ch) * 1099511628211ull;
    }
    return h ? h : 1;
}

void mc_init(mc_state *s, const mc_config *c)
{
    memset(s, 0, sizeof *s);
    s->cfg = *c;
    if (s->cfg.max_pins > MC_MAX_PINS) s->cfg.max_pins = MC_MAX_PINS;
    InitializeSRWLock(&s->lock);
}

static mc_path *path_slot(mc_state *s, uint64_t h, int create)
{
    uint32_t m = MC_MAX_PATHS - 1, i = (uint32_t)(h ^ (h >> 29)) & m;
    for (uint32_t probe = 0; probe < MC_MAX_PATHS; probe++, i = (i + 1) & m) {
        if (s->paths[i].h == h) return &s->paths[i];
        if (!s->paths[i].h) {
            if (!create) return NULL;
            if (s->npaths >= MC_MAX_PATHS * 3 / 4) { s->paths_full++; return NULL; }
            s->paths[i].h = h;
            s->npaths++;
            return &s->paths[i];
        }
    }
    return NULL;
}

uint32_t mc_record_load(mc_state *s, uint64_t h, const char *name, size_t n, uint64_t bytes, double ms, int via_cache)
{
    uint32_t loads = 1;
    AcquireSRWLockExclusive(&s->lock);
    mc_path *p = path_slot(s, h, 1);
    if (p) {
        if (!p->name[0] && name) {
            size_t k = n < MC_NAME - 1 ? n : MC_NAME - 1;
            if (n > k) { name += n - k; }            /* keep the tail (file name) of very long paths */
            memcpy(p->name, name, k); p->name[k] = 0;
        }
        loads = ++p->loads;
        p->via_cache += via_cache != 0;
        if (bytes) p->bytes = bytes;
        p->ms_total += ms;
        if (ms > p->ms_max) p->ms_max = ms;
    }
    s->loads++;
    s->load_ms += ms;
    s->bytes_read += bytes;
    s->f_loads++;
    s->f_bytes += bytes;
    s->f_ms += ms;
    if (loads > 1) { s->reloads++; s->reload_ms += ms; s->f_reloads++; }
    if (ms > s->f_slow_ms) {
        s->f_slow_ms = ms;
        if (p) memcpy(s->f_slow, p->name, sizeof s->f_slow);
        else snprintf(s->f_slow, sizeof s->f_slow, "%.*s", (int)(n < MC_NAME - 1 ? n : MC_NAME - 1), name ? name : "?");
    }
    ReleaseSRWLockExclusive(&s->lock);
    return loads;
}

void mc_record_gol(mc_state *s, int miss)
{
    AcquireSRWLockExclusive(&s->lock);
    s->gol_calls++;
    s->gol_misses += miss != 0;
    ReleaseSRWLockExclusive(&s->lock);
}

int mc_should_pin(const mc_state *s, uint32_t loads, uint64_t bytes)
{
    const mc_config *c = &s->cfg;
    if (c->mode != MC_ON || c->max_pins <= 0 || c->budget_mb <= 0) return 0;
    if (loads < (uint32_t)c->repeat_min) return 0;
    if (bytes > (uint64_t)c->max_texture_mb << 20) return 0;
    if (bytes > (uint64_t)c->budget_mb << 20) return 0;
    return 1;
}

static uint64_t charge(const mc_state *s, uint64_t bytes)
{
    return bytes ? bytes : (uint64_t)s->cfg.unknown_size_kb << 10;
}

int mc_pin_add(mc_state *s, void *tex, uint64_t h, uint64_t bytes, uint64_t now, void **replaced)
{
    *replaced = NULL;
    if (!tex) return 0;
    int r = 0;
    AcquireSRWLockExclusive(&s->lock);
    int same_path = -1;
    for (int i = 0; i < s->npins; i++) {
        if (s->pins[i].tex == tex) { s->pins[i].last = now; goto out; }
        if (s->pins[i].h == h) same_path = i;
    }
    if (same_path >= 0) {          /* the engine made a new texture object for a pinned path: swap */
        *replaced = s->pins[same_path].tex;
        s->pin_bytes -= s->pins[same_path].bytes;
        s->pins[same_path] = s->pins[--s->npins];
        s->pins_replaced++;
    }
    if (s->npins >= MC_MAX_PINS || s->npins >= s->cfg.max_pins + 64) goto out;   /* hard cap; collect trims */
    mc_pin *p = &s->pins[s->npins++];
    p->tex = tex; p->h = h; p->bytes = charge(s, bytes); p->last = now;
    s->pin_bytes += p->bytes;
    s->pins_added++;
    r = 1;
out:
    ReleaseSRWLockExclusive(&s->lock);
    return r;
}

int mc_pin_touch(mc_state *s, void *tex, uint64_t now)
{
    int r = 0;
    AcquireSRWLockExclusive(&s->lock);
    for (int i = 0; i < s->npins; i++)
        if (s->pins[i].tex == tex) {
            s->pins[i].last = now;
            s->pin_hits++;
            mc_path *p = path_slot(s, s->pins[i].h, 0);
            if (p) p->pin_hits++;
            r = 1;
            break;
        }
    ReleaseSRWLockExclusive(&s->lock);
    return r;
}

int mc_pin_collect(mc_state *s, void **victims, int maxv)
{
    int n = 0;
    AcquireSRWLockExclusive(&s->lock);
    uint64_t budget = (uint64_t)s->cfg.budget_mb << 20;
    while (n < maxv && s->npins > 0 && (s->pin_bytes > budget || s->npins > s->cfg.max_pins)) {
        int lru = 0;
        for (int i = 1; i < s->npins; i++)
            if (s->pins[i].last < s->pins[lru].last) lru = i;
        victims[n++] = s->pins[lru].tex;
        s->pin_bytes -= s->pins[lru].bytes;
        s->pins[lru] = s->pins[--s->npins];
        s->pins_evicted++;
    }
    ReleaseSRWLockExclusive(&s->lock);
    return n;
}

int mc_pin_take_all(mc_state *s, void **victims, int maxv)
{
    int n = 0;
    AcquireSRWLockExclusive(&s->lock);
    while (n < maxv && s->npins > 0) {
        victims[n++] = s->pins[s->npins - 1].tex;
        s->pin_bytes -= s->pins[s->npins - 1].bytes;
        s->npins--;
    }
    if (!s->npins) s->pin_bytes = 0;
    ReleaseSRWLockExclusive(&s->lock);
    return n;
}

void mc_big_record(mc_state *s, uint32_t ret_rva, uint64_t n)
{
    AcquireSRWLockExclusive(&s->lock);
    int i;
    for (i = 0; i < s->nbig; i++) if (s->big[i].ret_rva == ret_rva) break;
    if (i == s->nbig) {
        if (s->nbig >= MC_MAX_BIG) { s->big_other += n; ReleaseSRWLockExclusive(&s->lock); return; }
        memset(&s->big[i], 0, sizeof s->big[i]);
        s->big[i].ret_rva = ret_rva;
        s->nbig++;
    }
    s->big[i].count++;
    s->big[i].bytes += n;
    if (n > s->big[i].max) s->big[i].max = n;
    ReleaseSRWLockExclusive(&s->lock);
}

void mc_frame_take(mc_state *s, mc_frame_snap *o)
{
    AcquireSRWLockExclusive(&s->lock);
    o->loads = s->f_loads; o->reloads = s->f_reloads;
    o->mb = (double)s->f_bytes / 1048576.0; o->ms = s->f_ms; o->slow_ms = s->f_slow_ms;
    memcpy(o->slow, s->f_slow, sizeof o->slow);
    s->f_loads = s->f_reloads = 0; s->f_bytes = 0; s->f_ms = s->f_slow_ms = 0; s->f_slow[0] = 0;
    ReleaseSRWLockExclusive(&s->lock);
}

static int cmp_ms(const void *a, const void *b)
{
    const mc_path *x = *(const mc_path *const *)a, *y = *(const mc_path *const *)b;
    return x->ms_total < y->ms_total ? 1 : x->ms_total > y->ms_total ? -1 : 0;
}

static int cmp_big(const void *a, const void *b)
{
    const mc_big *x = a, *y = b;
    return x->bytes < y->bytes ? 1 : x->bytes > y->bytes ? -1 : 0;
}

#define APPEND(...) do { if (k < n) { int w_ = snprintf(buf + k, n - k, __VA_ARGS__); if (w_ > 0) k += (size_t)w_; if (k > n) k = n; } } while (0)

int mc_format_summary(mc_state *s, char *buf, size_t n, int top)
{
    size_t k = 0;
    if (!n) return 0;
    buf[0] = 0;
    AcquireSRWLockShared(&s->lock);
    APPEND("mode=%s | CreateTexture (disk reads): %llu, %.1f MB, %.0f ms | repeated reads of an already-loaded path: %llu, %.0f ms\n",
           mc_mode_name(s->cfg.mode), (unsigned long long)s->loads, (double)s->bytes_read / 1048576.0, s->load_ms,
           (unsigned long long)s->reloads, s->reload_ms);
    APPEND("GetOrLoadTexture: %llu calls, %llu misses | pins: %d (%.1f MB of %d MB), added %llu, evicted %llu, replaced %llu, pin hits %llu | paths %d%s\n",
           (unsigned long long)s->gol_calls, (unsigned long long)s->gol_misses, s->npins, (double)s->pin_bytes / 1048576.0,
           s->cfg.budget_mb, (unsigned long long)s->pins_added, (unsigned long long)s->pins_evicted,
           (unsigned long long)s->pins_replaced, (unsigned long long)s->pin_hits, s->npaths, s->paths_full ? " (table full)" : "");
    const mc_path **v = malloc(sizeof *v * (size_t)(s->npaths ? s->npaths : 1));
    int nv = 0;
    if (v) {
        for (int i = 0; i < MC_MAX_PATHS; i++) if (s->paths[i].h) v[nv++] = &s->paths[i];
        qsort(v, (size_t)nv, sizeof *v, cmp_ms);
        APPEND("top paths by total CreateTexture time:\n    #   loads via_cache  pin_hits   MB(last)   ms_total   ms_max  path\n");
        for (int i = 0; i < nv && i < top; i++)
            APPEND("  %3d %7u %9u %9u %10.1f %10.0f %8.0f  %s\n", i + 1, v[i]->loads, v[i]->via_cache, v[i]->pin_hits,
                   (double)v[i]->bytes / 1048576.0, v[i]->ms_total, v[i]->ms_max, v[i]->name);
        free(v);
    }
    if (s->nbig) {
        mc_big b[MC_MAX_BIG];
        memcpy(b, s->big, sizeof b[0] * (size_t)s->nbig);
        qsort(b, (size_t)s->nbig, sizeof b[0], cmp_big);
        APPEND("big byte-array reserves (>= %d MB) by return address (generic reserve, alloc site 0x84800b):\n", s->cfg.big_reserve_mb);
        for (int i = 0; i < s->nbig && i < top; i++)
            APPEND("  ret %08x  count %6u  total %10.1f MB  max %8.1f MB\n", b[i].ret_rva, b[i].count,
                   (double)b[i].bytes / 1048576.0, (double)b[i].max / 1048576.0);
        if (s->big_other) APPEND("  (other callers: %.1f MB)\n", (double)s->big_other / 1048576.0);
    }
    ReleaseSRWLockShared(&s->lock);
    return (int)k;
}

int mc_msvc_str(const void *str, char *out, size_t outn, int (*readable)(const void *, size_t))
{
    if (!outn) return -1;
    out[0] = 0;
    if (!str || (readable && !readable(str, 0x20))) return -1;
    const uint8_t *b = str;
    uint64_t size, cap;
    memcpy(&size, b + 0x10, 8);
    memcpy(&cap, b + 0x18, 8);
    if (size > 4096 || cap < size || cap < 15) return -1;
    const char *p;
    if (cap > 15) { memcpy(&p, b, sizeof p); if (!p || (readable && !readable(p, size + 1))) return -1; }
    else p = (const char *)b;
    size_t k = size < outn - 1 ? (size_t)size : outn - 1;
    if (size > k) p += size - k;          /* keep the tail */
    memcpy(out, p, k);
    out[k] = 0;
    return (int)k;
}
