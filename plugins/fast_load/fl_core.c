#include "fl_core.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void fl_config_defaults(fl_config *c)
{
    c->timeline = 1; c->checksum_cache = 0; c->cache_verify = 1; c->db_nodes = 1; c->mesh_threads = 0; c->counters = 1; c->pump_yield = 0; c->worker_spin = 0;
}

static const char *skip_ws(const char *p, const char *e) { while (p < e && (*p == ' ' || *p == '\t')) p++; return p; }

int fl_config_parse(fl_config *c, const char *text, char *err, size_t errn)
{
    int bad = 0, line = 0;
    if (err && errn) err[0] = 0;
    for (const char *p = text; p && *p; ) {
        const char *e = strchr(p, '\n'); if (!e) e = p + strlen(p);
        line++;
        const char *s = skip_ws(p, e), *end = e;
        for (const char *q = s; q < end; q++)          /* inline comment: '#' or ';' to end of line */
            if (*q == '#' || *q == ';') { end = q; break; }
        while (end > s && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) end--;
        if (s < end && *s != '#' && *s != ';') {
            const char *eq = memchr(s, '=', (size_t)(end - s));
            char key[32] = {0}; long val = 0; int ok = 0;
            if (eq) {
                const char *k = eq; while (k > s && (k[-1] == ' ' || k[-1] == '\t')) k--;
                size_t kn = (size_t)(k - s);
                const char *v = skip_ws(eq + 1, end);
                char vb[16] = {0}; size_t vn = (size_t)(end - v);
                if (kn && kn < sizeof key && vn && vn < sizeof vb) {
                    memcpy(key, s, kn); memcpy(vb, v, vn);
                    char *ve; val = strtol(vb, &ve, 10);
                    ok = *ve == 0;
                }
            }
            int *dst = NULL, lo = 0, hi = 1;
            if (ok && !strcmp(key, "mesh_threads")) { dst = &c->mesh_threads; lo = 2; hi = 15; }
            if (ok && !strcmp(key, "pump_yield")) { dst = &c->pump_yield; lo = 1; hi = 2; }
            if (ok && !strcmp(key, "worker_spin")) { dst = &c->worker_spin; lo = 1; hi = 3; }
            if (ok) {
                if (!strcmp(key, "timeline")) dst = &c->timeline;
                else if (!strcmp(key, "checksum_cache")) dst = &c->checksum_cache;
                else if (!strcmp(key, "cache_verify")) dst = &c->cache_verify;
                else if (!strcmp(key, "db_nodes")) dst = &c->db_nodes;
                else if (!strcmp(key, "counters")) dst = &c->counters;
            }
            if (dst && (val == 0 || (val >= lo && val <= hi))) *dst = (int)val;
            else {
                if (!bad && err && errn) snprintf(err, errn, "line %d: expected <timeline|checksum_cache|cache_verify|db_nodes|counters> = 0|1, mesh_threads = 0|2..15, pump_yield = 0..2, worker_spin = 0..3", line);
                bad++;
            }
        }
        p = *e ? e + 1 : e;
    }
    return bad;
}

int fl_match(const uint8_t *code, size_t avail, size_t off, const char *pat)
{
    size_t i = off;
    for (const char *s = pat; *s; ) {
        if (*s == ' ') { s++; continue; }
        if (i >= avail) return 0;
        if (*s == '?') { s++; if (*s == '?') s++; i++; continue; }
        char hex[3] = { s[0], s[1], 0 };
        if (!s[1]) return 0;
        if (code[i] != (uint8_t)strtoul(hex, NULL, 16)) return 0;
        s += 2; i++;
    }
    return 1;
}

void fl_tl_add(fl_timeline *tl, double t_ms, uint32_t tid, const char *fmt, ...)
{
    if (tl->n >= tl->cap) { tl->dropped++; return; }
    fl_event *e = &tl->ev[tl->n++];
    e->t_ms = t_ms; e->tid = tid;
    va_list ap; va_start(ap, fmt);
    vsnprintf(e->text, sizeof e->text, fmt, ap);
    va_end(ap);
}

size_t fl_tl_format(fl_timeline *tl, const char *header, char *out, size_t cap)
{
    if (!cap) return 0;
    /* insertion sort by time: stable, events are nearly sorted already */
    for (int i = 1; i < tl->n; i++) {
        fl_event x = tl->ev[i]; int j = i - 1;
        while (j >= 0 && tl->ev[j].t_ms > x.t_ms) { tl->ev[j + 1] = tl->ev[j]; j--; }
        tl->ev[j + 1] = x;
    }
    size_t w = 0;
#define PUT(...) do { if (w < cap) { int r = snprintf(out + w, cap - w, __VA_ARGS__); if (r > 0) w += (size_t)r; if (w >= cap) w = cap - 1; } } while (0)
    if (header) PUT("%s\n", header);
    for (int i = 0; i < tl->n; i++)
        PUT("%+10.1f ms  [tid %5u]  %s\n", tl->ev[i].t_ms, tl->ev[i].tid, tl->ev[i].text);
    if (tl->dropped) PUT("(%d events dropped: timeline full)\n", tl->dropped);
#undef PUT
    return w;
}

void fl_thr_hit(fl_thrset *s, uint32_t tid, double t_ms)
{
    s->calls++;
    for (int i = 0; i < s->n; i++)
        if (s->t[i].tid == tid) { s->t[i].calls++; s->t[i].last_ms = t_ms; return; }
    if (s->n >= (int)(sizeof s->t / sizeof s->t[0])) { s->overflow++; return; }
    s->t[s->n++] = (fl_thr){ tid, 1, t_ms, t_ms };
}

size_t fl_thr_summary(const fl_thrset *s, double wall_ms, char *out, size_t cap)
{
    double span = 0;
    for (int i = 0; i < s->n; i++) span += s->t[i].last_ms - s->t[i].first_ms;
    int r = snprintf(out, cap, "threads=%d%s items=%u span_sum=%.0fms wall=%.0fms est_parallelism=%.2f",
                     s->n, s->overflow ? "+" : "", s->calls, span, wall_ms, wall_ms > 0 ? span / wall_ms : 0.0);
    return r < 0 ? 0 : (size_t)r;
}

static int copy_bytes(const char *p, uint64_t n, char *out, size_t cap, fl_readable_fn rd)
{
    if (!cap) return -1;
    if (n > (1u << 20) || !rd(p, (size_t)n ? (size_t)n : 1)) { out[0] = 0; return -1; }
    size_t k = n < cap - 1 ? (size_t)n : cap - 1;
    memcpy(out, p, k); out[k] = 0;
    for (size_t i = 0; i < k; i++) if ((unsigned char)out[i] < 0x20) out[i] = '?';
    return (int)k;
}

int fl_msvc_str(const void *s, char *out, size_t cap, fl_readable_fn rd)
{
    if (cap) out[0] = 0;
    if (!s || !rd(s, 32)) return -1;
    const uint8_t *b = s; uint64_t size, capv;
    memcpy(&size, b + 16, 8); memcpy(&capv, b + 24, 8);
    if (size > capv || capv < 15) return -1;
    const char *p = capv > 15 ? *(const char *const *)b : (const char *)b;
    return copy_bytes(p, size, out, cap, rd);
}

int fl_view_str(const void *v, char *out, size_t cap, fl_readable_fn rd)
{
    if (cap) out[0] = 0;
    if (!v || !rd(v, 16)) return -1;
    const char *p; int64_t n;
    memcpy(&p, v, 8); memcpy(&n, (const uint8_t *)v + 8, 8);
    if (n < 0 || !p) return -1;
    return copy_bytes(p, (uint64_t)n, out, cap, rd);
}
