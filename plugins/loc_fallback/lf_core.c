/* loc_fallback core (see lf_core.h). Pure logic + Win32 file I/O; test/loc_fallback_test.c includes it. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lf_core.h"

#define LF_FORMAT 1   /* bump when the output or cache format changes (forces regeneration) */

const char *const lf_langs[LF_NLANGS] = {"l_english", "l_braz_por", "l_french", "l_german", "l_polish", "l_russian",
                                         "l_spanish", "l_simp_chinese", "l_japanese", "l_korean", "l_turkish"};

int lf_lang_index(const char *id)
{
    for (int i = 0; i < LF_NLANGS; i++)
        if (id && !strcmp(id, lf_langs[i])) return i;
    return -1;
}

const char *lf_status_name(int st)
{
    switch (st) {
    case LF_DISABLED: return "disabled";
    case LF_ENGLISH: return "english (nothing to do)";
    case LF_CACHED: return "cached (inputs unchanged)";
    case LF_WRITTEN: return "regenerated";
    case LF_NOTHING: return "nothing missing";
    default: return "error";
    }
}

/* ---- tiny JSON helpers ------------------------------------------------------------------------ */

static const char *js_skip(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* decodes the JSON string starting at p (on the opening quote) into out; returns pointer after the
 * closing quote, or NULL. *bad = 1 if truncated. */
static const char *js_string(const char *p, char *out, size_t cap, int *bad)
{
    size_t k = 0;
    *bad = 0;
    if (*p != '"') return NULL;
    for (p++; *p && *p != '"'; p++) {
        unsigned c = (unsigned char)*p;
        if (c == '\\') {
            p++;
            switch (*p) {
            case '"': case '\\': case '/': c = (unsigned char)*p; break;
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                unsigned v = 0;
                for (int i = 1; i <= 4; i++) {
                    char h = p[i];
                    v <<= 4;
                    if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                    else return NULL;
                }
                p += 4;
                char u[4];
                int un = 0;
                if (v < 0x80) u[un++] = (char)v;
                else if (v < 0x800) { u[un++] = (char)(0xC0 | (v >> 6)); u[un++] = (char)(0x80 | (v & 63)); }
                else { u[un++] = (char)(0xE0 | (v >> 12)); u[un++] = (char)(0x80 | ((v >> 6) & 63)); u[un++] = (char)(0x80 | (v & 63)); }
                for (int i = 0; i < un; i++) {
                    if (k + 1 < cap) out[k++] = u[i]; else *bad = 1;
                }
                continue;
            }
            default: return NULL;
            }
        }
        if (k + 1 < cap) out[k++] = (char)c; else *bad = 1;
    }
    if (cap) out[k < cap ? k : cap - 1] = 0;
    return *p == '"' ? p + 1 : NULL;
}

int lf_json_str(const char *json, const char *key, char *out, size_t cap)
{
    char pat[80];
    if (!json || !cap) return 0;
    out[0] = 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    for (const char *p = strstr(json, pat); p; p = strstr(p + 1, pat)) {
        const char *v = js_skip(p + strlen(pat));
        if (*v != ':') continue;
        v = js_skip(v + 1);
        int bad;
        if (*v == '"' && js_string(v, out, cap, &bad) && !bad) return 1;
    }
    out[0] = 0;
    return 0;
}

int lf_lang_from_settings(const char *json, char *out, size_t cap)
{
    return lf_json_str(json, "language", out, cap) && out[0];
}

int lf_json_mod_paths(const char *json, char *out, size_t each, int max)
{
    const char *a = json ? strstr(json, "\"enabledMods\"") : NULL;
    if (!a || !each) return 0;
    a = strchr(a, '[');
    if (!a) return 0;
    int n = 0, depth = 0;
    for (const char *p = a; *p && n < max; p++) {
        if (*p == '[' || *p == '{') depth++;
        else if (*p == ']' || *p == '}') { if (--depth == 0) break; }
        else if (*p == '"') {
            char name[16];
            int bad;
            const char *q = js_string(p, name, sizeof name, &bad);
            if (!q) break;
            p = q - 1;
            if (bad || strcmp(name, "path")) continue;
            const char *v = js_skip(q);
            if (*v != ':') continue;
            v = js_skip(v + 1);
            if (*v != '"') continue;
            char *o = out + (size_t)n * each;
            q = js_string(v, o, each, &bad);
            if (!q) break;
            p = q - 1;
            if (!bad && o[0]) n++;
        }
    }
    return n;
}

/* ---- loc parser ---------------------------------------------------------------------------------- */

static int is_blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

int lf_parse(const char *buf, size_t n, lf_entry_fn fn, void *user)
{
    const char *p = buf, *end = buf + n;
    const char *lang = "";
    size_t lang_len = 0;
    int count = 0;
    if (n >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    while (p < end) {
        const char *le = memchr(p, '\n', (size_t)(end - p));
        if (!le) le = end;
        const char *s = p, *e = le;
        p = le < end ? le + 1 : end;
        while (s < e && is_blank(*s)) s++;
        while (e > s && is_blank(e[-1])) e--;
        if (s == e || *s == '#') continue;
        /* a second BOM in the middle of concatenated files */
        if (e - s >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s += 3;
        const char *k = s;
        while (s < e && *s != ':' && *s != '"' && *s != '#' && !is_blank(*s)) s++;
        if (s == e || *s != ':' || s == k) continue;
        size_t kn = (size_t)(s - k);
        s++;
        const char *ver = s;
        while (s < e && *s >= '0' && *s <= '9') s++;
        size_t vn = (size_t)(s - ver);
        while (s < e && is_blank(*s)) s++;
        if (s == e || *s == '#') {
            /* "l_xxx:" alone = language header */
            if (vn == 0 && kn > 2 && k[0] == 'l' && k[1] == '_') { lang = k; lang_len = kn; }
            continue;
        }
        if (*s != '"') continue;
        const char *v = s + 1, *q = NULL;
        /* closing quote: the last '"' after which only blanks or a '#' comment follow */
        for (const char *t = e - 1; t >= v; t--) {
            if (*t != '"') continue;
            const char *r = t + 1;
            while (r < e && is_blank(*r)) r++;
            if (r == e || *r == '#') { q = t; break; }
        }
        if (!q) continue;
        lf_entry en = {lang, lang_len, k, kn, ver, vn, v, (size_t)(q - v)};
        count++;
        if (fn) fn(user, &en);
    }
    return count;
}

/* ---- key set ------------------------------------------------------------------------------------- */

uint64_t lf_hash(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ull; }
    return h | 1;   /* 0 = empty slot */
}

static int set_grow(lf_set *set)
{
    size_t nc = set->cap ? set->cap * 2 : 4096;
    struct lf_slot *ns = calloc(nc, sizeof *ns);
    if (!ns) return -1;
    for (size_t i = 0; i < set->cap; i++) {
        if (!set->s[i].h) continue;
        size_t j = set->s[i].h & (nc - 1);
        while (ns[j].h) j = (j + 1) & (nc - 1);
        ns[j] = set->s[i];
    }
    free(set->s);
    set->s = ns;
    set->cap = nc;
    return 0;
}

int32_t *lf_set_put(lf_set *set, const char *k, size_t n, int32_t v, int *added)
{
    if (added) *added = 0;
    if ((set->count + 1) * 4 >= set->cap * 3 && set_grow(set)) return NULL;
    uint64_t h = lf_hash(k, n);
    size_t j = h & (set->cap - 1);
    for (; set->s[j].h; j = (j + 1) & (set->cap - 1))
        if (set->s[j].h == h && set->s[j].n == n && !memcmp(set->s[j].k, k, n)) return &set->s[j].v;
    set->s[j].h = h, set->s[j].k = k, set->s[j].n = (uint32_t)n, set->s[j].v = v;
    set->count++;
    if (added) *added = 1;
    return &set->s[j].v;
}

int32_t *lf_set_get(const lf_set *set, const char *k, size_t n)
{
    if (!set->cap) return NULL;
    uint64_t h = lf_hash(k, n);
    for (size_t j = h & (set->cap - 1); set->s[j].h; j = (j + 1) & (set->cap - 1))
        if (set->s[j].h == h && set->s[j].n == n && !memcmp(set->s[j].k, k, n)) return &set->s[j].v;
    return NULL;
}

void lf_set_free(lf_set *set)
{
    free(set->s);
    memset(set, 0, sizeof *set);
}

/* ---- file helpers --------------------------------------------------------------------------------- */

static wchar_t *lf_w(const char *s, wchar_t *buf, int cap)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, cap)) buf[0] = 0;
    return buf;
}

static char *read_all(const char *path, size_t *len)
{
    wchar_t w[LF_PATH];
    HANDLE h = CreateFileW(lf_w(path, w, LF_PATH), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    char *b = NULL;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart < (1ll << 30)) {
        b = malloc((size_t)sz.QuadPart + 1);
        DWORD got = 0;
        if (b && (sz.QuadPart == 0 || (ReadFile(h, b, (DWORD)sz.QuadPart, &got, NULL) && got == (DWORD)sz.QuadPart))) {
            b[sz.QuadPart] = 0;
            if (len) *len = (size_t)sz.QuadPart;
        } else { free(b); b = NULL; }
    }
    CloseHandle(h);
    return b;
}

static void make_dirs(const char *dir)
{
    char tmp[LF_PATH];
    wchar_t w[LF_PATH];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *s = tmp + 3; *s; s++) {
        if (*s != '/' && *s != '\\') continue;
        char c = *s;
        *s = 0;
        CreateDirectoryW(lf_w(tmp, w, LF_PATH), NULL);
        *s = c;
    }
    CreateDirectoryW(lf_w(tmp, w, LF_PATH), NULL);
}

static int write_atomic(const char *path, const char *data, size_t n)
{
    char tmp[LF_PATH + 8];
    wchar_t wt[LF_PATH + 8], wp[LF_PATH];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    HANDLE h = CreateFileW(lf_w(tmp, wt, LF_PATH + 8), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD put = 0;
    BOOL ok = n == 0 || WriteFile(h, data, (DWORD)n, &put, NULL);
    CloseHandle(h);
    if (!ok || put != (DWORD)n || !MoveFileExW(wt, lf_w(path, wp, LF_PATH), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(wt);
        return -1;
    }
    return 0;
}

static void lang_folder(const char *lang, char *out, size_t cap) { snprintf(out, cap, "%s", lang + 2); }

static void out_path(const char *packs, const char *lang, char *out, size_t cap)
{
    char f[32];
    lang_folder(lang, f, sizeof f);
    snprintf(out, cap, "%s/" LF_PACK "/localization/%s/" LF_OUT_PREFIX "%s.yml", packs, f, lang);
}

int lf_clean(const char *packs, const char *keep)
{
    char p[LF_PATH];
    wchar_t w[LF_PATH];
    int n = 0;
    for (int i = 0; i < LF_NLANGS; i++) {
        if (keep && !strcmp(keep, lf_langs[i])) continue;
        out_path(packs, lf_langs[i], p, sizeof p);
        if (DeleteFileW(lf_w(p, w, LF_PATH))) n++;
    }
    return n;
}

/* ---- roots and files --------------------------------------------------------------------------- */

enum { RK_BASE, RK_PACK, RK_DLC, RK_MOD };

typedef struct lf_root { char path[LF_PATH]; int kind, mod; } lf_root;
typedef struct lf_file { char *full, *rel; int root, cls; uint64_t size, mtime; char *data; size_t dlen; } lf_file;

typedef struct lf_ctx {
    const lf_cfg *cfg;
    lf_result *r;
    char lang[32];
    lf_root *roots; int nroots, roots_cap;
    lf_file *files; int nfiles, files_cap;
    char **bufs; int nbufs, bufs_cap;
    lf_set present, cand;
    struct lf_rec { const char *k, *ver, *val; uint32_t kn, vn, valn; int root, rep; } *recs;
    int nrecs, recs_cap;
    int cur_root, cur_rep, hash_only;
    uint64_t *bh; size_t nbh, bh_cap;   /* sorted hashes of vanilla keys in the language (key cache) */
} lf_ctx;

#define GROW(arr, n, cap) \
    ((n) < (cap) || (((cap) = (cap) ? (cap) * 2 : 64), ((arr) = realloc((arr), sizeof *(arr) * (size_t)(cap))) != NULL))

static void add_root(lf_ctx *c, const char *path, int kind, int mod)
{
    if (!GROW(c->roots, c->nroots, c->roots_cap)) return;
    lf_root *r = &c->roots[c->nroots++];
    snprintf(r->path, sizeof r->path, "%s", path);
    for (char *s = r->path; *s; s++) if (*s == '\\') *s = '/';
    size_t n = strlen(r->path);
    while (n && r->path[n - 1] == '/') r->path[--n] = 0;
    r->kind = kind, r->mod = mod;
}

/* language class of a lower-case file name: index of the rightmost l_<lang> in it, -1 if none */
static int name_class(const char *lname)
{
    int best = -1;
    const char *bp = NULL;
    for (int i = 0; i < LF_NLANGS; i++) {
        const char *f = strstr(lname, lf_langs[i]), *last = NULL;
        for (; f; f = strstr(f + 1, lf_langs[i])) last = f;
        if (last && (!bp || last > bp)) bp = last, best = i;
    }
    return best;
}

static void walk(lf_ctx *c, int root, const char *dir, const char *rel, int depth)
{
    char pat[LF_PATH], sub[LF_PATH], srel[LF_PATH];
    wchar_t w[LF_PATH];
    WIN32_FIND_DATAW fd;
    if (depth > 16) return;
    snprintf(pat, sizeof pat, "%s/*", dir);
    HANDLE h = FindFirstFileExW(lf_w(pat, w, LF_PATH), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char name[MAX_PATH * 3];
        if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL)) continue;
        snprintf(sub, sizeof sub, "%s/%s", dir, name);
        snprintf(srel, sizeof srel, "%s/%s", rel, name);
        for (char *s = srel; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { walk(c, root, sub, srel, depth + 1); continue; }
        size_t ln = strlen(srel);
        if (ln < 4 || strcmp(srel + ln - 4, ".yml")) continue;
        if (!GROW(c->files, c->nfiles, c->files_cap)) break;
        lf_file *f = &c->files[c->nfiles++];
        memset(f, 0, sizeof *f);
        f->full = _strdup(sub);
        f->rel = _strdup(srel);
        f->root = root;
        const char *base = strrchr(srel, '/');
        f->cls = name_class(base ? base + 1 : srel);
        f->size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        f->mtime = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int cmp_rel_root(const void *a, const void *b)
{
    const lf_file *x = a, *y = b;
    int d = strcmp(x->rel, y->rel);
    return d ? d : x->root - y->root;
}

static int cmp_root_rel(const void *a, const void *b)
{
    const lf_file *x = a, *y = b;
    return x->root != y->root ? x->root - y->root : strcmp(x->rel, y->rel);
}

static int is_replace(const char *rel) { return strstr(rel, "/replace/") != NULL; }

/* ---- parse callbacks ------------------------------------------------------------------------------ */

static void on_entry(void *user, const lf_entry *e)
{
    lf_ctx *c = user;
    lf_root *rt = &c->roots[c->cur_root];
    if (e->lang_len == strlen(c->lang) && !memcmp(e->lang, c->lang, e->lang_len)) {
        if (c->hash_only) {
            if (GROW(c->bh, c->nbh, c->bh_cap)) c->bh[c->nbh++] = lf_hash(e->key, e->key_len);
            return;
        }
        int added;
        lf_set_put(&c->present, e->key, e->key_len, 0, &added);
        if (rt->mod >= 0) c->r->mods[rt->mod].lang_keys++;
        return;
    }
    if (c->hash_only || e->lang_len != 9 || memcmp(e->lang, "l_english", 9)) return;
    int fillable = rt->kind == RK_MOD || rt->kind == RK_PACK || c->cfg->also_fill_vanilla;
    if (!fillable) return;
    if (rt->mod >= 0) c->r->mods[rt->mod].en_keys++;
    int added;
    int32_t *slot = lf_set_put(&c->cand, e->key, e->key_len, c->nrecs, &added);
    if (!slot) return;
    if (!added) {
        struct lf_rec *o = &c->recs[*slot];
        c->r->dup_english++;
        /* replace/ beats normal, then the higher-priority root; first definition otherwise */
        if (!(c->cur_rep > o->rep || (c->cur_rep == o->rep && c->cur_root > o->root))) return;
        o->k = e->key, o->kn = (uint32_t)e->key_len, o->ver = e->ver, o->vn = (uint32_t)e->ver_len;
        o->val = e->val, o->valn = (uint32_t)e->val_len, o->root = c->cur_root, o->rep = c->cur_rep;
        return;
    }
    if (!GROW(c->recs, c->nrecs, c->recs_cap)) { *slot = -1; return; }
    struct lf_rec *o = &c->recs[c->nrecs++];
    o->k = e->key, o->kn = (uint32_t)e->key_len, o->ver = e->ver, o->vn = (uint32_t)e->ver_len;
    o->val = e->val, o->valn = (uint32_t)e->val_len, o->root = c->cur_root, o->rep = c->cur_rep;
}

/* ---- output buffer --------------------------------------------------------------------------------- */

typedef struct ob { char *p; size_t n, cap; int oom; } ob;
static void ob_put(ob *o, const char *s, size_t n)
{
    if (o->oom) return;
    if (o->n + n + 1 > o->cap) {
        size_t nc = o->cap ? o->cap * 2 : 1 << 16;
        while (nc < o->n + n + 1) nc *= 2;
        char *np = realloc(o->p, nc);
        if (!np) { o->oom = 1; return; }
        o->p = np, o->cap = nc;
    }
    memcpy(o->p + o->n, s, n);
    o->n += n;
    o->p[o->n] = 0;
}
static void ob_puts(ob *o, const char *s) { ob_put(o, s, strlen(s)); }

/* comment-safe copy of a display name (no newlines) */
static void ob_name(ob *o, const char *s)
{
    for (; *s; s++) ob_put(o, (*s == '\n' || *s == '\r') ? " " : s, 1);
}

/* ---- cache --------------------------------------------------------------------------------------- */

static uint64_t hmix(uint64_t h, const void *d, size_t n)
{
    const unsigned char *s = d;
    for (size_t i = 0; i < n; i++) { h ^= s[i]; h *= 1099511628211ull; }
    return h;
}

/* cache: "v3x_loc_fallback <fmt>\nhash <hex>\nlang <id>\nout <size>\ntotal <n>\nvanilla <n>\nmod <i> <filled>\n..." */
static int cache_hit(const lf_ctx *c, const char *cache, uint64_t hash, const char *outp, lf_result *r)
{
    char *t = read_all(cache, NULL);
    if (!t) return 0;
    int fmt = 0, total = -1, van = 0;
    unsigned long long h = 0, osz = 0;
    char lang[32] = "";
    int ok = sscanf(t, "v3x_loc_fallback %d\nhash %llx\nlang %31s\nout %llu\ntotal %d\nvanilla %d", &fmt, &h, lang, &osz,
                    &total, &van) == 6 && fmt == LF_FORMAT && h == hash && !strcmp(lang, c->lang) && total >= 0;
    if (ok) {
        wchar_t w[LF_PATH];
        WIN32_FILE_ATTRIBUTE_DATA a;
        int have = GetFileAttributesExW(lf_w(outp, w, LF_PATH), GetFileExInfoStandard, &a);
        uint64_t sz = have ? (((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow) : 0;
        ok = total ? (have && sz == osz) : !have;
    }
    if (ok) {
        r->total_filled = total;
        r->vanilla_filled = van;
        for (const char *m = strstr(t, "\nmod "); m; m = strstr(m + 1, "\nmod ")) {
            int i, f;
            if (sscanf(m, "\nmod %d %d", &i, &f) == 2 && i >= 0 && i < r->nmods) r->mods[i].filled = f;
        }
    }
    free(t);
    return ok;
}

static void cache_write(const char *cache, uint64_t hash, const char *lang, uint64_t osz, const lf_result *r)
{
    ob o = {0};
    char line[160];
    snprintf(line, sizeof line, "v3x_loc_fallback %d\nhash %016llx\nlang %s\nout %llu\ntotal %d\nvanilla %d\n", LF_FORMAT,
             (unsigned long long)hash, lang, (unsigned long long)osz, r->total_filled, r->vanilla_filled);
    ob_puts(&o, line);
    for (int i = 0; i < r->nmods; i++) {
        snprintf(line, sizeof line, "mod %d %d %s\n", i, r->mods[i].filled, r->mods[i].id);
        ob_puts(&o, line);
    }
    if (!o.oom) write_atomic(cache, o.p, o.n);
    free(o.p);
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* <cache>.keys = u64 hash of the vanilla file list, u64 count, count x u64 key hashes. The file is a
 * cache (not trusted): sizes are checked, and the sorted / unique order bh_has relies on is restored.
 * Returns a malloc'd array (count in *out_n), or NULL when it does not match or is corrupt. */
static uint64_t *lf_keys_parse(const char *kb, size_t kn, uint64_t vh, size_t *out_n)
{
    uint64_t hdr[2];
    *out_n = 0;
    if (!kb || kn < 16 || (kn - 16) % 8) return NULL;
    memcpy(hdr, kb, 16);
    if (hdr[0] != vh || hdr[1] != (kn - 16) / 8) return NULL;
    size_t n = (size_t)hdr[1];
    uint64_t *b = (uint64_t *)malloc(kn - 16 + 8);
    if (!b) return NULL;
    if (n) memcpy(b, kb + 16, n * 8);
    qsort(b, n, sizeof *b, cmp_u64);
    size_t u = 0;
    for (size_t i = 0; i < n; i++) if (!u || b[u - 1] != b[i]) b[u++] = b[i];
    *out_n = u;
    return b;
}

static int bh_has(const lf_ctx *c, const char *k, size_t n)
{
    if (!c->nbh) return 0;
    uint64_t h = lf_hash(k, n);
    size_t lo = 0, hi = c->nbh;
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (c->bh[m] < h) lo = m + 1; else hi = m;
    }
    return lo < c->nbh && c->bh[lo] == h;
}

static int is_present(const lf_ctx *c, const char *k, size_t n) { return lf_set_get(&c->present, k, n) || bh_has(c, k, n); }

/* number of English candidates no file of the language defines so far */
static int count_missing(const lf_ctx *c)
{
    int n = 0;
    for (int i = 0; i < c->nrecs; i++) n += !is_present(c, c->recs[i].k, c->recs[i].kn);
    return n;
}

/* Reads the files of the given root kinds on up to 8 threads (cold file opens cost ~10 ms each, mostly
 * waiting: AV scan / disk), so the sequential parse below finds them in memory. */
typedef struct pf_job { lf_ctx *c; int k1, k2; volatile LONG next; } pf_job;
static DWORD WINAPI pf_thread(void *u)
{
    pf_job *j = u;
    for (;;) {
        LONG i = InterlockedIncrement(&j->next) - 1;
        if (i >= j->c->nfiles) return 0;
        lf_file *f = &j->c->files[i];
        int k = j->c->roots[f->root].kind;
        if ((k == j->k1 || k == j->k2) && !f->data) f->data = read_all(f->full, &f->dlen);
    }
}
static void prefetch(lf_ctx *c, int k1, int k2)
{
    pf_job j = {c, k1, k2, 0};
    HANDLE th[8];
    int n = 0;
    for (; n < 8; n++) if (!(th[n] = CreateThread(NULL, 64 * 1024, pf_thread, &j, 0, NULL))) break;
    pf_thread(&j);
    if (n) WaitForMultipleObjects((DWORD)n, th, TRUE, 30000);
    for (int i = 0; i < n; i++) CloseHandle(th[i]);
}

static void parse_file(lf_ctx *c, lf_result *r, int i)
{
    size_t n = c->files[i].dlen;
    char *b = c->files[i].data;
    c->files[i].data = NULL;
    if (!b) b = read_all(c->files[i].full, &n);
    if (!b) return;
    c->cur_root = c->files[i].root;
    c->cur_rep = is_replace(c->files[i].rel);
    lf_parse(b, n, on_entry, c);
    r->nparsed++;
    if (c->hash_only) { free(b); return; }   /* hashes copied, text not needed */
    if (!GROW(c->bufs, c->nbufs, c->bufs_cap)) return;   /* OOM: keys point into b, so leak it rather than free */
    c->bufs[c->nbufs++] = b;
}

/* ---- main ------------------------------------------------------------------------------------------- */

static void ctx_free(lf_ctx *c)
{
    for (int i = 0; i < c->nfiles; i++) free(c->files[i].full), free(c->files[i].rel), free(c->files[i].data);
    for (int i = 0; i < c->nbufs; i++) free(c->bufs[i]);
    free(c->files), free(c->bufs), free(c->roots), free(c->recs), free(c->bh);
    lf_set_free(&c->present);
    lf_set_free(&c->cand);
}

static double now_ms(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void mod_ident(const char *path, lf_modsum *m)
{
    char p[LF_PATH];
    const char *b = path + strlen(path);
    while (b > path && (b[-1] == '/' || b[-1] == '\\')) b--;
    const char *e = b;
    while (b > path && b[-1] != '/' && b[-1] != '\\') b--;
    snprintf(m->id, sizeof m->id, "%.*s", (int)(e - b), b);
    snprintf(p, sizeof p, "%s/.metadata/metadata.json", path);
    char *j = read_all(p, NULL);
    if (!j || !lf_json_str(j, "name", m->name, sizeof m->name)) snprintf(m->name, sizeof m->name, "%s", m->id);
    free(j);
}

int lf_run(const lf_paths *p, const lf_cfg *cfg, lf_result *r)
{
    lf_ctx c;
    char path[LF_PATH * 2];
    double t0 = now_ms();
    memset(r, 0, sizeof *r);
    memset(&c, 0, sizeof c);
    c.cfg = cfg, c.r = r;
    r->status = LF_ERROR;

    /* 1. language */
    if (p->lang[0]) snprintf(c.lang, sizeof c.lang, "%s", p->lang);
    else {
        snprintf(path, sizeof path, "%s/pdx_settings.json", p->docs);
        char *j = read_all(path, NULL);
        if (!j || !lf_lang_from_settings(j, c.lang, sizeof c.lang)) snprintf(c.lang, sizeof c.lang, "l_english");
        free(j);
    }
    snprintf(r->lang, sizeof r->lang, "%s", c.lang);
    if (!cfg->enabled) { lf_clean(p->packs, NULL); r->status = LF_DISABLED; goto done; }
    if (!strcmp(c.lang, "l_english")) { lf_clean(p->packs, NULL); r->status = LF_ENGLISH; goto done; }
    if (lf_lang_index(c.lang) < 0) { snprintf(r->err, sizeof r->err, "unknown language '%s'", c.lang); goto done; }
    lf_clean(p->packs, c.lang);
    out_path(p->packs, c.lang, r->out_path, sizeof r->out_path);

    /* 2. roots, lowest priority first (mount order: each later mount is prepended = searched first) */
    static const char *base[] = {"clausewitz", "jomini", "game"};
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s/%s", p->game_root, base[i]);
        add_root(&c, path, RK_BASE, -1);
    }
    {   /* other content packs (alphabetical; this plugin's own pack is the output, never an input) */
        char pat[LF_PATH + 4], names[64][MAX_PATH];
        wchar_t w[LF_PATH + 4];
        WIN32_FIND_DATAW fd;
        int n = 0;
        snprintf(pat, sizeof pat, "%s/*", p->packs);
        HANDLE h = FindFirstFileW(lf_w(pat, w, LF_PATH + 4), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' || fd.cFileName[0] == L'_') continue;
                if (n < 64 && WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, names[n], MAX_PATH, NULL, NULL) &&
                    _stricmp(names[n], LF_PACK))
                    n++;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        qsort(names, (size_t)n, sizeof names[0], (int (*)(const void *, const void *))_stricmp);
        for (int i = 0; i < n; i++) {
            snprintf(path, sizeof path, "%s/%s", p->packs, names[i]);
            add_root(&c, path, RK_PACK, -1);
        }
        snprintf(pat, sizeof pat, "%s/game/dlc/*", p->game_root);
        n = 0;
        h = FindFirstFileW(lf_w(pat, w, LF_PATH + 4), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
                if (n < 64 && WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, names[n], MAX_PATH, NULL, NULL)) n++;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        qsort(names, (size_t)n, sizeof names[0], (int (*)(const void *, const void *))_stricmp);
        for (int i = 0; i < n; i++) {
            snprintf(path, sizeof path, "%s/game/dlc/%s", p->game_root, names[i]);
            add_root(&c, path, RK_DLC, -1);
        }
    }
    {
        snprintf(path, sizeof path, "%s/content_load.json", p->docs);
        char *j = read_all(path, NULL);
        static char mods[LF_MAXMODS][LF_PATH];
        int n = j ? lf_json_mod_paths(j, mods[0], sizeof mods[0], LF_MAXMODS) : 0;
        free(j);
        for (int i = 0; i < n; i++) {
            mod_ident(mods[i], &r->mods[i]);
            add_root(&c, mods[i], RK_MOD, i);
        }
        r->nmods = n;
    }
    r->nroots = c.nroots;

    /* 3. files; the same relative path resolves to the highest root (VFS override) */
    for (int i = 0; i < c.nroots; i++) {
        snprintf(path, sizeof path, "%s/localization", c.roots[i].path);
        walk(&c, i, path, "localization", 0);
    }
    if (c.nfiles) qsort(c.files, (size_t)c.nfiles, sizeof *c.files, cmp_rel_root);
    int target = lf_lang_index(c.lang), w = 0;
    for (int i = 0; i < c.nfiles; i++) {
        int last = i + 1 == c.nfiles || strcmp(c.files[i].rel, c.files[i + 1].rel);
        lf_file f = c.files[i];
        int k = c.roots[f.root].kind;
        int need = f.cls < 0 || f.cls == target ||
                   (f.cls == 0 && (k == RK_MOD || k == RK_PACK || cfg->also_fill_vanilla));
        if (last && need) c.files[w++] = f;
        else free(f.full), free(f.rel);
    }
    c.nfiles = w;
    r->nfiles = w;
    if (c.nfiles) qsort(c.files, (size_t)c.nfiles, sizeof *c.files, cmp_root_rel);

    uint64_t hash = 1469598103934665603ull;
    int fmt = LF_FORMAT;
    hash = hmix(hash, &fmt, sizeof fmt);
    hash = hmix(hash, c.lang, strlen(c.lang) + 1);
    hash = hmix(hash, &cfg->also_fill_vanilla, sizeof cfg->also_fill_vanilla);
    for (int i = 0; i < c.nroots; i++) hash = hmix(hash, c.roots[i].path, strlen(c.roots[i].path) + 1);
    for (int i = 0; i < c.nfiles; i++) {
        hash = hmix(hash, c.files[i].rel, strlen(c.files[i].rel) + 1);
        hash = hmix(hash, &c.files[i].root, sizeof c.files[i].root);
        hash = hmix(hash, &c.files[i].size, sizeof c.files[i].size);
        hash = hmix(hash, &c.files[i].mtime, sizeof c.files[i].mtime);
    }
    if (!cfg->force && p->cache[0] && cache_hit(&c, p->cache, hash, r->out_path, r)) {
        r->status = LF_CACHED;
        goto done;
    }

    /* 4. parse: mods and packs first; vanilla (clausewitz/jomini/game/DLC) only if English candidates
     * are still missing. Without also_fill_vanilla only vanilla's keys in the language matter: their
     * 64-bit hashes are cached in <cache>.keys, keyed by the vanilla files' paths/sizes/mtimes. */
    prefetch(&c, RK_MOD, RK_PACK);
    for (int i = 0; i < c.nfiles; i++) {
        int k = c.roots[c.files[i].root].kind;
        if (k == RK_MOD || k == RK_PACK) parse_file(&c, r, i);
    }
    if (cfg->also_fill_vanilla) {
        prefetch(&c, RK_BASE, RK_DLC);
        for (int i = 0; i < c.nfiles; i++) {
            int k = c.roots[c.files[i].root].kind;
            if (k == RK_BASE || k == RK_DLC) parse_file(&c, r, i);
        }
    } else if (count_missing(&c)) {
        uint64_t vh = 1469598103934665603ull;
        vh = hmix(vh, &fmt, sizeof fmt);
        vh = hmix(vh, c.lang, strlen(c.lang) + 1);
        for (int i = 0; i < c.nfiles; i++) {
            int k = c.roots[c.files[i].root].kind;
            if (k != RK_BASE && k != RK_DLC) continue;
            const char *rp = c.roots[c.files[i].root].path;
            vh = hmix(vh, rp, strlen(rp) + 1);
            vh = hmix(vh, c.files[i].rel, strlen(c.files[i].rel) + 1);
            vh = hmix(vh, &c.files[i].size, sizeof c.files[i].size);
            vh = hmix(vh, &c.files[i].mtime, sizeof c.files[i].mtime);
        }
        char kp[LF_PATH + 8];
        snprintf(kp, sizeof kp, "%s.keys", p->cache);
        size_t kn = 0;
        char *kb = p->cache[0] && !cfg->force ? read_all(kp, &kn) : NULL;
        uint64_t hdr[2];
        size_t kcount = 0;
        uint64_t *kh = lf_keys_parse(kb, kn, vh, &kcount);
        if (kh) {
            c.bh = kh;
            c.nbh = c.bh_cap = kcount;
            r->vanilla_keys_cached = 1;
        } else {
            c.hash_only = 1;
            prefetch(&c, RK_BASE, RK_DLC);
            for (int i = 0; i < c.nfiles; i++) {
                int k = c.roots[c.files[i].root].kind;
                if (k == RK_BASE || k == RK_DLC) parse_file(&c, r, i);
            }
            c.hash_only = 0;
            if (c.nbh) qsort(c.bh, c.nbh, sizeof *c.bh, cmp_u64);
            size_t u = 0;
            for (size_t i = 0; i < c.nbh; i++) if (!u || c.bh[u - 1] != c.bh[i]) c.bh[u++] = c.bh[i];
            c.nbh = u;
            if (p->cache[0]) {
                char *wb = malloc(16 + u * 8);
                if (wb) {
                    hdr[0] = vh, hdr[1] = u;
                    memcpy(wb, hdr, 16);
                    if (u) memcpy(wb + 16, c.bh, u * 8);
                    write_atomic(kp, wb, 16 + u * 8);
                    free(wb);
                }
            }
        }
        free(kb);
    }
    r->present_keys = (int)(c.present.count + c.nbh);
    r->english_keys = (int)c.cand.count;

    /* 5. missing keys, grouped by root in priority order */
    ob o = {0};
    ob_puts(&o, "\xEF\xBB\xBF");
    ob_puts(&o, c.lang);
    ob_puts(&o, ":\n # Generated by V3X loc_fallback: English text for keys that no mod or vanilla file defines in ");
    ob_puts(&o, c.lang);
    ob_puts(&o, ".\n # Rewritten at game start when inputs change - do not edit. Disable: binaries/v3x/loc_fallback.ini enabled=0\n");
    for (int rt = 0; rt < c.nroots; rt++) {
        int header = 0;
        for (int i = 0; i < c.nrecs; i++) {
            struct lf_rec *e = &c.recs[i];
            if (e->root != rt || is_present(&c, e->k, e->kn)) continue;
            if (!header) {
                header = 1;
                ob_puts(&o, " # ---- ");
                if (c.roots[rt].mod >= 0) {
                    ob_name(&o, r->mods[c.roots[rt].mod].name);
                    ob_puts(&o, " (");
                    ob_puts(&o, r->mods[c.roots[rt].mod].id);
                    ob_puts(&o, ")");
                } else ob_name(&o, c.roots[rt].path);
                ob_puts(&o, "\n");
            }
            ob_puts(&o, " ");
            ob_put(&o, e->k, e->kn);
            ob_puts(&o, ":");
            ob_put(&o, e->ver, e->vn);
            ob_puts(&o, " \"");
            ob_put(&o, e->val, e->valn);
            ob_puts(&o, "\"\n");
            r->total_filled++;
            if (c.roots[rt].mod >= 0) r->mods[c.roots[rt].mod].filled++;
            else r->vanilla_filled++;
        }
    }
    uint64_t osz = 0;
    if (o.oom) { snprintf(r->err, sizeof r->err, "out of memory"); free(o.p); goto done; }
    wchar_t wp[LF_PATH];
    if (r->total_filled) {
        char dir[LF_PATH];
        snprintf(dir, sizeof dir, "%s", r->out_path);
        *strrchr(dir, '/') = 0;
        make_dirs(dir);
        if (write_atomic(r->out_path, o.p, o.n)) {
            snprintf(r->err, sizeof r->err, "cannot write %s (error %lu)", r->out_path, GetLastError());
            free(o.p);
            goto done;
        }
        osz = o.n;
        r->status = LF_WRITTEN;
    } else {
        DeleteFileW(lf_w(r->out_path, wp, LF_PATH));
        r->status = LF_NOTHING;
    }
    free(o.p);
    if (p->cache[0]) cache_write(p->cache, hash, c.lang, osz, r);

done:
    ctx_free(&c);
    r->ms = now_ms() - t0;
    return r->status;
}
