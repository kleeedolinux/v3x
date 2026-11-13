/* console_plus: display-name search (no engine access, no Windows calls).
 * Included by console_plus.c and by test/console_plus_test.c.
 *
 *  - cp_fold: case- and accent-insensitive form of a UTF-8 text ("São Paulo" -> "sao paulo",
 *    compact form "saopaulo"); Latin-1, Latin Extended-A and the Romanian comma letters are folded
 *    to ASCII, other characters are kept byte for byte
 *  - cp_loc_clean: localized text -> plain display name (one level of $KEY$ references resolved,
 *    #style markers, [script] parts and @icon! removed)
 *  - CpNames: name index (id, display name, context such as the owner tag) with ranked search:
 *    exact name, name prefix, prefix of a later word, substring; "name@ctx" filters by context
 *  - cp_discover_field: which pointer field of a set of objects points into a known object set
 *    (used for live game objects: state -> state region, state -> owner country, ...)
 *  - cp_decode_gs: decodes the engine's own "object by numeric id" lookup in a console command
 *    (game state root -> +0x608 -> +0x120 -> slot array {.., object*} + count) */
#ifndef CP_NAMES_H
#define CP_NAMES_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ folding */
/* one char per code point U+00C0..U+017F; '?' = several letters (see cp_fold_multi), '*' = not a letter */
static const char CP_FOLD_TAB[] =
    "AAAAAA?CEEEEIIIIDNOOOOO*OUUUUY??"                          /* C0-DF */
    "aaaaaa?ceeeeiiiidnooooo*ouuuuy?y"                          /* E0-FF */
    "AaAaAa" "CcCcCcCc" "DdDd" "EeEeEeEeEe" "GgGgGgGg" "HhHh"   /* 100-127 */
    "IiIiIiIiIi" "??" "Jj" "Kkk" "LlLlLlLlLl" "NnNnNnnNn"       /* 128-14B */
    "OoOoOo" "??" "RrRrRr" "SsSsSsSs" "TtTtTt" "UuUuUuUuUuUu"   /* 14C-173 */
    "Ww" "YyY" "ZzZzZz" "s";                                    /* 174-17F */

static const char *cp_fold_multi(uint32_t cp)
{
    switch (cp) {
    case 0xC6: case 0xE6: return "ae";
    case 0xDE: case 0xFE: return "th";
    case 0xDF: return "ss";
    case 0x132: case 0x133: return "ij";
    case 0x152: case 0x153: return "oe";
    default: return NULL;
    }
}

/* decodes one UTF-8 sequence at s (n bytes left); returns its length (1 for invalid bytes) */
static size_t cp_utf8(const unsigned char *s, size_t n, uint32_t *cp)
{
    unsigned c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && n >= 2 && (s[1] & 0xC0) == 0x80) { *cp = ((c & 0x1Fu) << 6) | (s[1] & 0x3Fu); return 2; }
    if ((c & 0xF0) == 0xE0 && n >= 3 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *cp = ((c & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu); return 3;
    }
    if ((c & 0xF8) == 0xF0 && n >= 4 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *cp = ((c & 0x07u) << 18) | ((s[1] & 0x3Fu) << 12) | ((s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu); return 4;
    }
    *cp = 0xFFFD; return 1;
}

/* Folded form of s[0..n): lowercase ASCII, accents removed. compact = 0: every run of characters
 * that are neither letters nor digits becomes one blank (no leading/trailing blank);
 * compact = 1: they are dropped ("São_Paulo" -> "saopaulo"). Characters outside the folding
 * table (Cyrillic, CJK, ...) are copied unchanged. Returns the length (out is NUL-terminated). */
static size_t cp_fold(const char *s, size_t n, char *out, size_t cap, int compact)
{
    size_t o = 0;
    int pend = 0;           /* a separator is pending */
    if (!cap) return 0;
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < n && p[i];) {
        uint32_t cp; size_t k = cp_utf8(p + i, n - i, &cp);
        char buf[4]; const char *add = NULL; size_t an = 0;
        if (cp < 0x80) {
            int c = (int)cp;
            if (c >= 'A' && c <= 'Z') c += 32;
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { buf[0] = (char)c; add = buf; an = 1; }
        } else if (cp >= 0xC0 && cp <= 0x17F) {
            const char *m = cp_fold_multi(cp);
            char t = CP_FOLD_TAB[cp - 0xC0];
            if (m) { add = m; an = 2; }
            else if (t != '*' && t != '?') { buf[0] = (char)(t >= 'A' && t <= 'Z' ? t + 32 : t); add = buf; an = 1; }
        } else if (cp >= 0x218 && cp <= 0x21B) {                    /* Ș ș Ț ț */
            buf[0] = cp < 0x21A ? 's' : 't'; add = buf; an = 1;
        } else if (cp == 0xA0 || cp == 0x2019 || cp == 0x2018 || cp == 0x2013 || cp == 0x2014 || cp == 0xB7) {
            add = NULL;                                              /* nbsp, quotes, dashes: separators */
        } else if (cp != 0xFFFD) {
            add = (const char *)p + i; an = k;                       /* keep as is */
        }
        if (add) {
            if (pend && !compact && o && o + 1 < cap) out[o++] = ' ';
            pend = 0;
            for (size_t j = 0; j < an && o + 1 < cap; j++) out[o++] = add[j];
        } else pend = 1;
        i += k;
    }
    out[o] = 0;
    return o;
}

/* ------------------------------------------------------------------ localization text */
/* resolve a referenced key ($KEY$): copies its raw text to out, returns its length or -1 */
typedef int (*cp_loc_fn)(const char *key, size_t kn, char *out, size_t cap, void *user);

static size_t cp_loc_clean_rec(const char *v, size_t n, char *out, size_t cap, cp_loc_fn res, void *u, int depth)
{
    size_t o = 0;
    for (size_t i = 0; i < n && v[i] && o + 1 < cap;) {
        char c = v[i];
        if (c == '$') {                                     /* $KEY$ or $KEY|format$ */
            size_t e = i + 1; while (e < n && v[e] && v[e] != '$') e++;
            if (e < n && v[e] == '$' && e > i + 1) {
                size_t kn = e - i - 1, bar = kn;
                for (size_t j = 0; j < kn; j++) if (v[i + 1 + j] == '|') { bar = j; break; }
                char sub[512];
                int sn = (depth < 2 && res) ? res(v + i + 1, bar, sub, sizeof sub, u) : -1;
                if (sn > 0) o += cp_loc_clean_rec(sub, (size_t)sn, out + o, cap - o, res, u, depth + 1);
                i = e + 1; continue;
            }
            i++; continue;
        }
        if (c == '[') {                                     /* [Scripted.Text] */
            int d = 0; size_t e = i;
            for (; e < n && v[e]; e++) { if (v[e] == '[') d++; else if (v[e] == ']' && --d == 0) break; }
            i = e < n ? e + 1 : n; continue;
        }
        if (c == '#') {                                     /* #bold, #variable; #! ends a style */
            if (i + 1 < n && v[i + 1] == '!') { i += 2; continue; }
            size_t e = i + 1;
            while (e < n && v[e] && v[e] != ' ') e++;
            i = e < n ? e + 1 : n; continue;
        }
        if (c == '@') {                                     /* @icon! */
            size_t e = i + 1; while (e < n && v[e] && v[e] != '!' && v[e] != ' ') e++;
            if (e < n && v[e] == '!') { i = e + 1; continue; }
        }
        if (c == '\\' && i + 1 < n && v[i + 1] == 'n') { out[o++] = ' '; i += 2; continue; }
        if (c == '\\' && i + 1 < n && v[i + 1] == '"') { out[o++] = '"'; i += 2; continue; }
        if (c == '\t' || c == '\r' || c == '\n') c = ' ';
        out[o++] = c; i++;
    }
    out[o] = 0;
    return o;
}

/* Plain display name from a raw localization value; returns its length (0 = nothing usable). */
static size_t cp_loc_clean(const char *v, size_t n, char *out, size_t cap, cp_loc_fn res, void *u)
{
    if (!cap) return 0;
    size_t k = cp_loc_clean_rec(v, n, out, cap, res, u, 0);
    /* collapse blanks, trim */
    size_t o = 0; int sp = 1;
    for (size_t i = 0; i < k; i++) {
        if (out[i] == ' ') { if (!sp) out[o++] = ' '; sp = 1; }
        else { out[o++] = out[i]; sp = 0; }
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
    return o;
}

/* ------------------------------------------------------------------ name index */
typedef struct CpArenaBlk { struct CpArenaBlk *next; size_t used, cap; char data[1]; } CpArenaBlk;
typedef struct { CpArenaBlk *head; size_t bytes; } CpArena;

static char *cp_arena_dup(CpArena *a, const char *s, size_t n)
{
    if (!a->head || a->head->used + n + 1 > a->head->cap) {
        size_t cap = n + 1 > 65536 ? n + 1 : 65536;
        CpArenaBlk *b = (CpArenaBlk *)malloc(sizeof *b + cap);
        if (!b) return NULL;
        b->next = a->head; b->used = 0; b->cap = cap;
        a->head = b; a->bytes += cap;
    }
    char *d = a->head->data + a->head->used;
    memcpy(d, s, n); d[n] = 0;
    a->head->used += n + 1;
    return d;
}

static void cp_arena_free(CpArena *a)
{
    while (a->head) { CpArenaBlk *n = a->head->next; free(a->head); a->head = n; }
    a->bytes = 0;
}

typedef struct {
    const char *id;     /* what the command takes: "PRU", "STATE_SAO_PAULO", "123" */
    const char *name;   /* display name, UTF-8 */
    const char *ctx;    /* context, e.g. owner tag ("" if none) */
    const char *fc;     /* folded compact name */
    const char *ff;     /* folded name with blanks */
    const char *fid;    /* folded compact id */
    const char *fctx;   /* folded compact context */
} CpName;

typedef struct { CpName *e; int n, cap; CpArena ar; } CpNames;

static int cp_names_add(CpNames *x, const char *id, const char *name, const char *ctx)
{
    if (!id || !*id) return -1;
    if (!name) name = "";
    if (!ctx) ctx = "";
    if (x->n == x->cap) {
        int nc = x->cap ? x->cap * 2 : 256;
        CpName *ne = (CpName *)realloc(x->e, (size_t)nc * sizeof *ne);
        if (!ne) return -1;
        x->e = ne; x->cap = nc;
    }
    char fc[512], ff[512], fi[256], fx[256];
    size_t nl = strlen(name), cl = strlen(ctx), il = strlen(id);
    size_t a = cp_fold(name, nl, fc, sizeof fc, 1), b = cp_fold(name, nl, ff, sizeof ff, 0);
    size_t c = cp_fold(id, il, fi, sizeof fi, 1), d = cp_fold(ctx, cl, fx, sizeof fx, 1);
    CpName *e = &x->e[x->n];
    e->id = cp_arena_dup(&x->ar, id, il);
    e->name = cp_arena_dup(&x->ar, name, nl);
    e->ctx = cp_arena_dup(&x->ar, ctx, cl);
    e->fc = cp_arena_dup(&x->ar, fc, a);
    e->ff = cp_arena_dup(&x->ar, ff, b);
    e->fid = cp_arena_dup(&x->ar, fi, c);
    e->fctx = cp_arena_dup(&x->ar, fx, d);
    if (!e->id || !e->name || !e->ctx || !e->fc || !e->ff || !e->fid || !e->fctx) return -1;
    return x->n++;
}

static void cp_names_free(CpNames *x)
{
    free(x->e); x->e = NULL; x->n = x->cap = 0;
    cp_arena_free(&x->ar);
}

/* score of a folded name for the folded query: 0 exact, 1 prefix, 2 a later word starts with it
 * (words of ff, compared compactly: "rio grande do sul" matches "grandedo"), 3 substring
 * (queries of 2+ characters), -1 no match */
static int cp_name_score(const char *fc, const char *ff, const char *qc)
{
    size_t ql = strlen(qc);
    if (!ql) return -1;
    if (!strcmp(fc, qc)) return 0;
    if (!strncmp(fc, qc, ql)) return 1;
    /* word starts: the compact text from each word start of ff */
    size_t ci = 0;           /* position in fc that corresponds to the word start */
    for (const char *p = ff; *p; p++) {
        if (*p == ' ') continue;
        if ((p == ff || p[-1] == ' ') && ci > 0 && !strncmp(fc + ci, qc, ql)) return 2;
        ci++;
    }
    if (ql >= 2 && strstr(fc, qc)) return 3;
    return -1;
}

typedef struct { int idx; int score; } CpHit;

#define CP_Q_IDS 1      /* also match the id ("STATE_SAO" -> STATE_SAO_PAULO) */

/* Ranked search. query = "name" or "name@ctx" (ctx: part of the folded context, e.g. the owner
 * tag); "@ctx" alone lists everything of that context. Returns the number of matches; out[]
 * holds the best `max` of them, ordered by score, then shorter name, then id. */
static int cp_names_query(const CpNames *x, const char *query, int flags, CpHit *out, int max)
{
    char q[256], qc[256], cc[128];
    size_t qn = strlen(query);
    if (qn >= sizeof q) qn = sizeof q - 1;
    memcpy(q, query, qn); q[qn] = 0;
    char *at = strchr(q, '@');
    cc[0] = 0;
    if (at) { *at = 0; cp_fold(at + 1, strlen(at + 1), cc, sizeof cc, 1); }
    cp_fold(q, strlen(q), qc, sizeof qc, 1);
    size_t ccl = strlen(cc);
    if (!qc[0] && !ccl) return 0;
    int total = 0, kept = 0;
    for (int i = 0; i < x->n; i++) {
        const CpName *e = &x->e[i];
        if (ccl && !strstr(e->fctx, cc)) continue;
        int s;
        if (!qc[0]) s = 4;
        else {
            s = cp_name_score(e->fc, e->ff, qc);
            if (flags & CP_Q_IDS) {
                int si = !strcmp(e->fid, qc) ? 0 : !strncmp(e->fid, qc, strlen(qc)) ? 1 : (strlen(qc) >= 2 && strstr(e->fid, qc)) ? 3 : -1;
                if (si >= 0 && (s < 0 || si < s)) s = si;
            }
            if (s < 0) continue;
        }
        total++;
        if (max <= 0) continue;
        /* insertion into the kept top list */
        int pos = kept;
        while (pos > 0) {
            const CpHit *h = &out[pos - 1];
            const CpName *o = &x->e[h->idx];
            int better = s < h->score;
            if (s == h->score) {
                size_t a = strlen(e->name), b = strlen(o->name);
                better = a < b || (a == b && strcmp(e->id, o->id) < 0);
            }
            if (!better) break;
            pos--;
        }
        if (pos >= max) continue;
        int last = kept < max ? kept : max - 1;
        for (int k = last; k > pos; k--) out[k] = out[k - 1];
        out[pos].idx = i; out[pos].score = s;
        if (kept < max) kept++;
    }
    return total;
}

/* ------------------------------------------------------------------ pointer sets / field discovery */
static int cp_cmp_uptr(const void *a, const void *b)
{
    uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
    return x < y ? -1 : x > y;
}

static void cp_ptrset_sort(uintptr_t *v, int n) { if (n > 1) qsort(v, (size_t)n, sizeof *v, cp_cmp_uptr); }

/* index of v in the sorted set or -1 */
static int cp_ptrset_find(const uintptr_t *v, int n, uintptr_t x)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (v[m] == x) return m;
        if (v[m] < x) lo = m + 1; else hi = m - 1;
    }
    return -1;
}

/* safe read of n bytes at addr into out; 1 = ok */
typedef int (*cp_read_fn)(const void *addr, void *out, size_t n, void *user);

#define CP_DISC_MAXOFF 0x800
/* For up to 256 of the objects, which 8-aligned offset below max_off holds a pointer into `set`
 * (sorted) most often? Returns that offset if at least min_pct % of the sampled objects have it
 * (smallest offset on ties), else -1. *pct receives the hit rate. */
static int cp_discover_field(void *const *objs, int nobj, size_t max_off, const uintptr_t *set, int nset,
                             cp_read_fn rd, void *user, int min_pct, int *pct)
{
    static uint64_t buf[CP_DISC_MAXOFF / 8];
    static int hits[CP_DISC_MAXOFF / 8];
    if (pct) *pct = 0;
    if (max_off > CP_DISC_MAXOFF) max_off = CP_DISC_MAXOFF;
    if (nobj <= 0 || nset <= 0) return -1;
    memset(hits, 0, sizeof hits);
    int step = nobj > 256 ? nobj / 256 : 1, sampled = 0;
    for (int i = 0; i < nobj; i += step) {
        if (!objs[i] || !rd(objs[i], buf, max_off, user)) continue;
        sampled++;
        for (size_t k = 0; k < max_off / 8; k++)
            if (buf[k] && cp_ptrset_find(set, nset, (uintptr_t)buf[k]) >= 0) hits[k]++;
    }
    if (!sampled) return -1;
    int best = -1;
    for (size_t k = 0; k < max_off / 8; k++) if (hits[k] && (best < 0 || hits[k] > hits[best])) best = (int)k;
    if (best < 0) return -1;
    int p = hits[best] * 100 / sampled;
    if (pct) *pct = p;
    return p >= min_pct ? best * 8 : -1;
}

/* ------------------------------------------------------------------ "object by id" decoding */
/* The engine's console commands look objects up by numeric id like this (invalidate_state 1.15.1):
 *   48 8B 05 rel32        mov rax,[rip+root]          game state root pointer
 *   48 8B 88 08 06 00 00  mov rcx,[rax+608h]          (any registers)
 *   48 8B 81 20 01 00 00  mov rax,[rcx+120h]          game state G
 *   3B 98 3C 1C 00 00     cmp ebx,[rax+1C3Ch]         count
 *   73 xx ...             jae
 *   48 8B 80 30 1C 00 00  mov rax,[rax+1C30h]         slot array (16-byte slots)
 *   48 8B 4C C8 08        mov rcx,[rax+rcx*8+8]       object = slot+8
 * The rip-relative root load may come a few instructions before. Returns 1 and the root address
 * (absolute, = code address based), array and count offsets. */
static int cp_modrm_disp(const uint8_t *p, size_t left, uint32_t *disp, size_t *len)
{
    /* p at the ModRM byte of a reg,[base+disp] form without SIB; returns 0 if not that shape */
    if (left < 2) return 0;
    uint8_t m = p[0], mod = m >> 6, rm = m & 7;
    if (rm == 4 || mod == 0 || mod == 3) return 0;
    if (mod == 1) { *disp = p[1]; *len = 2; return 1; }
    if (left < 5) return 0;
    memcpy(disp, p + 1, 4); *len = 5; return 1;
}

static int cp_decode_gs(const uint8_t *code, size_t n, const uint8_t **root, uint32_t *arr_off, uint32_t *cnt_off)
{
    for (size_t i = 0; i + 40 < n; i++) {
        /* mov r64,[r64+608h] */
        if (!((code[i] == 0x48 || code[i] == 0x4C || code[i] == 0x49 || code[i] == 0x4D) && code[i + 1] == 0x8B)) continue;
        uint32_t d; size_t l;
        if (!cp_modrm_disp(code + i + 2, n - i - 2, &d, &l) || d != 0x608 || l != 5) continue;
        /* root: rip-relative mov within the 24 bytes before */
        const uint8_t *r = NULL;
        for (size_t b = i >= 24 ? i - 24 : 0; b + 7 <= i; b++)
            if ((code[b] == 0x48 || code[b] == 0x4C) && code[b + 1] == 0x8B && (code[b + 2] & 0xC7) == 0x05) {
                int32_t rel; memcpy(&rel, code + b + 3, 4);
                r = code + b + 7 + rel;
            }
        if (!r) continue;
        /* next mov r64,[r64+120h] within 24 bytes */
        size_t j = i + 2 + l, k;
        int ok = 0;
        for (k = j; k + 7 <= n && k < j + 24; k++)
            if ((code[k] == 0x48 || code[k] == 0x4C || code[k] == 0x49 || code[k] == 0x4D) && code[k + 1] == 0x8B &&
                cp_modrm_disp(code + k + 2, n - k - 2, &d, &l) && d == 0x120 && l == 5) { ok = 1; break; }
        if (!ok) continue;
        j = k + 2 + l;
        /* cmp r32,[r64+count] (3B, optional REX) */
        uint32_t cnt = 0; ok = 0;
        for (k = j; k + 3 <= n && k < j + 12; k++) {
            size_t o = k + ((code[k] & 0xF0) == 0x40);
            if (code[o] == 0x3B && cp_modrm_disp(code + o + 1, n - o - 1, &cnt, &l)) { ok = 1; j = o + 1 + l; break; }
        }
        if (!ok || (cnt & 0xF) != 0xC) continue;
        /* mov r64,[r64+array] with array = count - 0xC, then the slot load "C8 08" */
        ok = 0;
        for (k = j; k + 7 <= n && k < j + 24; k++)
            if ((code[k] == 0x48 || code[k] == 0x4C || code[k] == 0x49 || code[k] == 0x4D) && code[k + 1] == 0x8B &&
                cp_modrm_disp(code + k + 2, n - k - 2, &d, &l) && d == cnt - 0xC) { ok = 1; j = k + 2 + l; break; }
        if (!ok) continue;
        ok = 0;
        for (k = j; k + 5 <= n && k < j + 8; k++)
            if ((code[k] == 0x48 || code[k] == 0x4C || code[k] == 0x49 || code[k] == 0x4D) && code[k + 1] == 0x8B &&
                (code[k + 2] & 0xC7) == 0x44 && code[k + 3] == 0xC8 && code[k + 4] == 0x08) { ok = 1; break; }
        if (!ok) continue;
        *root = r; *arr_off = cnt - 0xC; *cnt_off = cnt;
        return 1;
    }
    return 0;
}

#endif
