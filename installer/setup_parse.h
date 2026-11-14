/* setup_parse.h - the parsers of V3X-Setup.exe (installer/setup.c) that read untrusted bytes: the
 * payload index and meta.txt appended to the exe, binaries/v3x/install_manifest.txt, and the
 * compat.json scanner of --verify-release. No windows.h and no file I/O, so they are fuzzed on their
 * own (test/fuzz/fuzz_installer.c, also as a libFuzzer/ASan target on Linux in CI). Every function
 * fails closed on malformed input and never reads past the buffer it is given.
 * Needs -Iinclude -Isdk (sdk/v3x_safe.h: v3x_path_rel_ok). */
#ifndef V3X_SETUP_PARSE_H
#define V3X_SETUP_PARSE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v3x_safe.h"

#ifndef _WIN32
#include <strings.h>
#define _stricmp strcasecmp
#endif

/* SHA-256 of data; 1 ok, 0 = cannot hash (callers treat it as "no match") */
typedef int (*sp_sha256_fn)(const void *data, size_t n, uint8_t out[32]);

static void hex32(const uint8_t h[32], char out[65])
{
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = d[h[i] >> 4]; out[2 * i + 1] = d[h[i] & 15]; }
    out[64] = 0;
}

static int hex64_ok(const char *h)
{
    if (!h || strlen(h) != 64) return 0;
    for (int i = 0; i < 64; i++)
        if (!((h[i] >= '0' && h[i] <= '9') || (h[i] >= 'a' && h[i] <= 'f'))) return 0;
    return 1;
}

/* manifest / payload path check (H2): sdk/v3x_safe.h rules, plus '/' as the only separator */
static int rel_ok(const char *rel)
{
    return rel && !strchr(rel, '\\') && v3x_path_rel_ok(rel) == V3X_R_OK;
}

/* v3x/backup/version.dll or v3x/backup/version.<1..99>.dll, nothing else */
static int backup_rel_ok(const char *rel)
{
    if (!rel || strncmp(rel, "v3x/backup/version.", 19)) return 0;
    const char *t = rel + 19;
    if (!strcmp(t, "dll")) return 1;
    if (t[0] < '1' || t[0] > '9') return 0;
    size_t k = 1;
    if (t[1] >= '0' && t[1] <= '9') k = 2;
    return !strcmp(t + k, ".dll");
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t' || (unsigned char)s[i] == 0xef || (unsigned char)s[i] == 0xbb ||
           (unsigned char)s[i] == 0xbf) i++;
    if (i) memmove(s, s + i, n - i + 1);
}

/* ------------------------------------------------------------------ payload */

typedef struct { char kind[8]; uint64_t off, size; char sha[65]; char rel[260]; const uint8_t *data; } Entry;
typedef struct { char id[40]; char branch[64]; } Build;
typedef struct { char name[64]; int enabled; } Prof;

/* The payload of a whole setup file self[0..n): 64-byte footer {"V3XPAYL1", u64 index offset, u32 index
 * size, u32 count, sha256(index), "V3XEND\0\0"}, the index ("<kind> <off> <size> <sha256> <rel>" lines)
 * and the files it names, each checked against its hash. 1 ok: *ent (calloc'd, caller frees) / *nent;
 * 0 = damaged (nothing returned). */
static int sp_payload_parse(const uint8_t *self, size_t n, sp_sha256_fn sha256, Entry **ent, int *nent)
{
    *ent = NULL; *nent = 0;
    if (!self || n < 64 || !sha256) return 0;
    /* footer through a checked reader (V3xSpan): every field read is bounds-checked, errors are sticky */
    V3xSpan ft;
    v3x_span_init(&ft, self + n - 64, 64);
    const uint8_t *magic = v3x_span_take(&ft, 8);
    uint64_t ioff = 0;
    uint32_t isz = 0, cnt = 0;
    V3xResult r1 = v3x_span_u64le(&ft, &ioff), r2 = v3x_span_u32le(&ft, &isz), r3 = v3x_span_u32le(&ft, &cnt);
    const uint8_t *ihash = v3x_span_take(&ft, 32), *endtag = v3x_span_take(&ft, 8);
    if (!magic || !ihash || !endtag || r1 != V3X_R_OK || r2 != V3X_R_OK || r3 != V3X_R_OK || v3x_span_failed(&ft)) return 0;
    if (memcmp(magic, "V3XPAYL1", 8) || memcmp(endtag, "V3XEND\0\0", 8)) return 0;
    const uint64_t body = (uint64_t)n - 64;   /* bytes before the footer */
    if (ioff > body || isz > body - ioff || cnt > 4096) return 0;   /* no wrap (L3) */
    uint8_t h[32];
    if (!sha256(self + ioff, isz, h) || memcmp(h, ihash, 32)) return 0;
    char *idx = (char *)v3x_alloc_array((size_t)isz + 1, 1);
    if (!idx) return 0;
    memcpy(idx, self + ioff, isz); idx[isz] = 0;
    Entry *e0 = (Entry *)v3x_alloc_array(cnt ? cnt : 1, sizeof(Entry));
    if (!e0) { free(idx); return 0; }
    int ne = 0;
    for (char *line = strtok(idx, "\n"); line; line = strtok(NULL, "\n")) {
        if (ne >= (int)cnt) goto bad;
        Entry *e = &e0[ne];
        unsigned long long off, size;
        if (sscanf(line, "%7s %llu %llu %64s %259s", e->kind, &off, &size, e->sha, e->rel) != 5) goto bad;
        e->off = off; e->size = size;
        if (off > ioff || size > ioff - off) goto bad;   /* inside the file area, no wrap */
        if (strcmp(e->kind, "bin") && strcmp(e->kind, "ini") && strcmp(e->kind, "v3xini") && strcmp(e->kind, "meta")) goto bad;
        if (!rel_ok(e->rel)) goto bad;   /* the payload names only plain relative paths */
        e->data = self + off;
        char hx[65];
        if (!sha256(e->data, (size_t)size, h)) goto bad;
        hex32(h, hx);
        if (strcmp(hx, e->sha)) goto bad;
        ne++;
    }
    free(idx);
    if (ne != (int)cnt) { free(e0); return 0; }
    *ent = e0; *nent = ne;
    return 1;
bad:
    free(idx);
    free(e0);
    return 0;
}

/* meta.txt of the payload: "version <v>", "build <id> [<branch>]" (max 16), "plugin <name> <0|1>" (max 64).
 * Unknown lines are ignored. 1 ok, 0 out of memory. */
static int sp_meta_parse(const uint8_t *data, size_t n, char *version, size_t vcap, Build *builds, int *nbuilds,
                         Prof *prof, int *nprof)
{
    char *m = (char *)v3x_alloc_array(n + 1, 1);
    if (!m) return 0;
    if (n) memcpy(m, data, n);
    m[n] = 0;
    for (char *line = strtok(m, "\n"); line; line = strtok(NULL, "\n")) {
        char a[64], b[64], c[64];
        int k = sscanf(line, "%63s %63s %63s", a, b, c);
        if (k >= 2 && !strcmp(a, "version")) snprintf(version, vcap, "%s", b);
        else if (k >= 2 && !strcmp(a, "build") && *nbuilds < 16) {
            snprintf(builds[*nbuilds].id, 40, "%s", b);
            snprintf(builds[*nbuilds].branch, 64, "%s", k >= 3 ? c : "");
            (*nbuilds)++;
        } else if (k >= 3 && !strcmp(a, "plugin") && *nprof < 64) {
            snprintf(prof[*nprof].name, 64, "%s", b);
            prof[*nprof].enabled = atoi(c);
            (*nprof)++;
        }
    }
    free(m);
    return 1;
}

/* ------------------------------------------------------------------ manifest */

typedef struct { char kind[8]; char sha[65]; char rel[260]; } MFile;
typedef struct {
    int valid;
    char version[64];
    char backup_rel[260], backup_sha[65];
    MFile *f; int n, cap;
    int rejected;   /* lines ignored because their kind, hash or path is not acceptable */
} Manifest;

static int g_oom;   /* an allocation failed: the operation reports an error instead of half-working */

static void mf_add(Manifest *m, const char *kind, const char *sha, const char *rel)
{
    for (int i = 0; i < m->n; i++)
        if (!_stricmp(m->f[i].rel, rel)) { snprintf(m->f[i].kind, 8, "%s", kind); snprintf(m->f[i].sha, 65, "%s", sha); return; }
    if (m->n == m->cap) {
        int nc = m->cap ? m->cap * 2 : 32;
        MFile *nf = nc > 0 && nc < (1 << 20) ? (MFile *)v3x_realloc_array(m->f, (size_t)nc, sizeof(MFile)) : NULL;
        if (!nf) { g_oom = 1; return; }
        m->f = nf; m->cap = nc;
    }
    MFile *e = &m->f[m->n++];
    snprintf(e->kind, 8, "%s", kind); snprintf(e->sha, 65, "%s", sha); snprintf(e->rel, 260, "%s", rel);
}

static MFile *mf_find(Manifest *m, const char *rel)
{
    for (int i = 0; i < m->n; i++) if (!_stricmp(m->f[i].rel, rel)) return &m->f[i];
    return NULL;
}

/* Parses install_manifest.txt (text is modified: strtok). m must be zeroed. Lines whose kind, hash or path
 * is not acceptable are counted in m->rejected and never followed (H2). */
static void mf_parse(char *text, Manifest *m)
{
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        trim(line);
        char a[16], x[260], y[260], z[260];
        int k = sscanf(line, "%15s %259s %259s %259s", a, x, y, z);
        if (k >= 2 && !strcmp(a, "v3x_manifest")) m->valid = 1;
        else if (k >= 2 && !strcmp(a, "version")) snprintf(m->version, 64, "%s", x);
        else if (k >= 3 && !strcmp(a, "backup")) {
            /* H2: only our own backup names; anything else would move an arbitrary file to version.dll */
            if (hex64_ok(x) && backup_rel_ok(y)) { snprintf(m->backup_sha, 65, "%s", x); snprintf(m->backup_rel, 260, "%s", y); }
            else m->rejected++;
        } else if (k >= 4 && !strcmp(a, "file")) {
            /* H2: kind, hash and path checked; a line that fails is ignored (never followed) */
            if ((!strcmp(x, "bin") || !strcmp(x, "config")) && hex64_ok(y) && rel_ok(z)) mf_add(m, x, y, z);
            else m->rejected++;
        }
    }
}

/* ------------------------------------------------------------------ compat.json (--verify-release, H1)
 * compat.json is written by installer/make_dist.py and signed by tools/sign_release.py, so its
 * layout is fixed: {"schema":1,"v3x_version":"..",...,"assets":[{"name":"..","size":N,"sha256":".."},..]}.
 * The scanner below reads only that shape (strings without escapes, objects without nesting) and
 * fails closed on anything else; it never reads past the buffer. */

/* value of "key": "<string>" in p..e (first occurrence); 1 ok */
static int cj_str(const char *p, const char *e, const char *key, char *out, size_t cap)
{
    size_t kn = strlen(key);
    for (const char *q = p; q + kn + 2 < e; q++) {
        if (*q != '"' || memcmp(q + 1, key, kn) || q[kn + 1] != '"') continue;
        const char *v = q + kn + 2;
        while (v < e && (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')) v++;
        if (v >= e || *v != ':') return 0;
        v++;
        while (v < e && (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')) v++;
        if (v >= e || *v != '"') return 0;
        v++;
        size_t k = 0;
        while (v < e && *v != '"') {
            if (*v == '\\' || (unsigned char)*v < 0x20 || k + 1 >= cap) return 0;
            out[k++] = *v++;
        }
        if (v >= e) return 0;
        out[k] = 0;
        return 1;
    }
    return 0;
}

static int cj_int(const char *p, const char *e, const char *key, unsigned long long *out)
{
    size_t kn = strlen(key);
    for (const char *q = p; q + kn + 2 < e; q++) {
        if (*q != '"' || memcmp(q + 1, key, kn) || q[kn + 1] != '"') continue;
        const char *v = q + kn + 2;
        while (v < e && (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')) v++;
        if (v >= e || *v != ':') return 0;
        v++;
        while (v < e && (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')) v++;
        unsigned long long x = 0;
        int digits = 0;
        while (v < e && *v >= '0' && *v <= '9') {
            if (++digits > 15) return 0;
            x = x * 10 + (unsigned)(*v++ - '0');
        }
        if (!digits) return 0;
        *out = x;
        return 1;
    }
    return 0;
}

/* The '[' of the "assets" array in p..e, NULL if there is none (only blanks and ':' between the key
 * and the '['). */
static const char *cj_assets(const char *p, const char *e)
{
    const char *as = NULL;
    for (const char *q = p; q + 8 < e; q++)
        if (!memcmp(q, "\"assets\"", 8)) { as = q + 8; break; }
    while (as && as < e && *as != '[') { if (*as != ' ' && *as != ':' && *as != '\r' && *as != '\n' && *as != '\t') as = NULL; else as++; }
    return as && as < e ? as : NULL;
}

/* Next asset object of the list at *q (call with *q = cj_assets() + 1). 1 = an object [*ob, *oe) with
 * *oe at its '}' (q advanced past it), 0 = end of the list, -1 = malformed. */
static int cj_next_asset(const char **q, const char *e, const char **ob, const char **oe)
{
    const char *s = *q;
    while (s < e && *s != ']' && *s != '{') s++;
    if (s >= e) return -1;
    if (*s == ']') { *q = s; return 0; }
    const char *t = s + 1;
    while (t < e && *t != '}' && *t != '{' && *t != '[') t++;
    if (t >= e || *t != '}') return -1;
    *ob = s; *oe = t;
    *q = t + 1;
    return 1;
}

#endif
