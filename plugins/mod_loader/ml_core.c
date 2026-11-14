/* mod_loader core helpers, see ml_core.h. Pure C, no Windows calls. */
#include "ml_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* ---- config ------------------------------------------------------------------------------- */

void ml_config_defaults(ml_config *c)
{
    memset(c, 0, sizeof *c);
    c->mode = ML_MEASURE;
    c->index_workshop = 1;
    c->index_dlc = 1;
    c->index_local = 0;
    c->index_game = 1;
    c->index_packs = 1;
    c->log_misses = 32;
    c->index_cache = 1;
    c->verify_every = 64;
    c->report = 1;
    c->skip_fatal = 0;
    c->fatal_min = 1;
}

static int parse_int(const char *v, int lo, int hi, int *out)
{
    char *e;
    long x = strtol(v, &e, 10);
    if (e == v || *e || x < lo || x > hi) return 0;
    *out = (int)x;
    return 1;
}

int ml_config_parse(ml_config *c, const char *text, char *err, size_t errn)
{
    int bad = 0, lineno = 0;
    if (err && errn) err[0] = 0;
    const char *p = text;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    while (*p) {
        char line[640];
        size_t n = strcspn(p, "\r\n");
        lineno++;
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = 0;
        p += strcspn(p, "\r\n");
        while (*p == '\r' || *p == '\n') p++;
        char *s = line, *e;
        while (*s == ' ' || *s == '\t') s++;
        char *hash = strpbrk(s, "#;");
        if (hash) *hash = 0;
        e = s + strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (!*s || *s == '[') continue;
        char *eq = strchr(s, '=');
        if (!eq) { bad++; if (err && !err[0]) snprintf(err, errn, "line %d: missing '='", lineno); continue; }
        char *k = s, *v = eq + 1;
        *eq = 0;
        e = k + strlen(k);
        while (e > k && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        while (*v == ' ' || *v == '\t') v++;
        int ok = 1;
        if (!strcmp(k, "mode")) {
            if (!strcmp(v, "off")) c->mode = ML_OFF;
            else if (!strcmp(v, "measure")) c->mode = ML_MEASURE;
            else if (!strcmp(v, "on")) c->mode = ML_ON;
            else ok = 0;
        } else if (!strcmp(k, "index_workshop")) ok = parse_int(v, 0, 1, &c->index_workshop);
        else if (!strcmp(k, "index_dlc")) ok = parse_int(v, 0, 1, &c->index_dlc);
        else if (!strcmp(k, "index_local")) ok = parse_int(v, 0, 1, &c->index_local);
        else if (!strcmp(k, "index_game")) ok = parse_int(v, 0, 1, &c->index_game);
        else if (!strcmp(k, "index_packs")) ok = parse_int(v, 0, 1, &c->index_packs);
        else if (!strcmp(k, "index_cache")) ok = parse_int(v, 0, 1, &c->index_cache);
        else if (!strcmp(k, "log_misses")) ok = parse_int(v, 0, 1024, &c->log_misses);
        else if (!strcmp(k, "verify_every")) ok = parse_int(v, 0, 1000000, &c->verify_every);
        else if (!strcmp(k, "report")) ok = parse_int(v, 0, 1, &c->report);
        else if (!strcmp(k, "skip_fatal")) ok = parse_int(v, 0, 1, &c->skip_fatal);
        else if (!strcmp(k, "fatal_min")) ok = parse_int(v, 1, 1000000, &c->fatal_min);
        else if (!strcmp(k, "skip_mods")) {
            if (strlen(v) >= sizeof c->skip_mods) ok = 0;
            else snprintf(c->skip_mods, sizeof c->skip_mods, "%s", v);
        } else {
            bad++;
            if (err && !err[0]) snprintf(err, errn, "line %d: unknown key '%s'", lineno, k);
            continue;
        }
        if (!ok) { bad++; if (err && !err[0]) snprintf(err, errn, "line %d: bad value '%s' for %s", lineno, v, k); }
    }
    return bad;
}

int ml_in_list(const char *list, const char *name)
{
    size_t nl = strlen(name);
    const char *p = list;
    while (p && *p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *e = p;
        while (*e && *e != ',') e++;
        const char *t = e;
        while (t > p && (t[-1] == ' ' || t[-1] == '\t')) t--;
        if ((size_t)(t - p) == nl && nl) {
            size_t i = 0;
            while (i < nl && lc((unsigned char)p[i]) == lc((unsigned char)name[i])) i++;
            if (i == nl) return 1;
        }
        p = e;
    }
    return 0;
}

/* ---- path rules ---------------------------------------------------------------------------- */

static int is_device(const char *c, size_t n)
{
    /* CON PRN AUX NUL COM1-9 LPT1-9, also with an extension ("nul.txt") */
    size_t b = 0;
    while (b < n && c[b] != '.') b++;
    char s[5] = {0};
    if (b != 3 && b != 4) return 0;
    for (size_t i = 0; i < b; i++) s[i] = (char)lc((unsigned char)c[i]);
    if (b == 3) return !strcmp(s, "con") || !strcmp(s, "prn") || !strcmp(s, "aux") || !strcmp(s, "nul");
    return (!memcmp(s, "com", 3) || !memcmp(s, "lpt", 3)) && s[3] >= '1' && s[3] <= '9';
}

int ml_norm(const char *name, char *out, size_t cap)
{
    if (!name || !*name || *name == '/') return -1;
    size_t n = 0, comp = 0;
    for (const char *p = name;; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '/' || ch == 0) {
            size_t len = n - comp;
            if (len == 0) return -1;
            const char *cs = out + comp;
            if ((len == 1 && cs[0] == '.') || (len == 2 && cs[0] == '.' && cs[1] == '.')) return -1;
            if (cs[len - 1] == '.' || cs[len - 1] == ' ') return -1;
            if (is_device(cs, len)) return -1;
            if (ch == 0) break;
            if (n + 1 >= cap) return -1;
            out[n++] = '/';
            comp = n;
            continue;
        }
        if (ch < 0x20 || ch >= 0x80 || strchr("\\:*?\"<>|~", ch)) return -1;
        if (n + 1 >= cap) return -1;
        out[n++] = (char)lc(ch);
    }
    out[n] = 0;
    return (int)n;
}

/* 0 = character never answered from the index, else its lower-case form ('/' handled by the caller) */
static const unsigned char *key_table(void)
{
    static unsigned char t[256];
    static volatile int ready;
    if (!ready) {
        for (int c = 0; c < 256; c++)
            t[c] = (c < 0x20 || c >= 0x80 || strchr("\\:*?\"<>|~", c)) ? 0 : (unsigned char)lc(c);
        ready = 1;   /* idempotent: concurrent first callers write the same bytes */
    }
    return t;
}

static int comp_bad(const char *cs, size_t len)
{
    if ((len == 1 && cs[0] == '.') || (len == 2 && cs[0] == '.' && cs[1] == '.')) return 1;
    if (cs[len - 1] == '.' || cs[len - 1] == ' ') return 1;
    return (len == 3 || len >= 4) && is_device(cs, len);
}

int ml_key(const char *name, uint64_t *hash)
{
    const unsigned char *t = key_table();
    if (!name || !*name || *name == '/') return -1;
    uint64_t h = 1469598103934665603ull;
    const char *comp = name, *p = name;
    for (;; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '/' || ch == 0) {
            size_t len = (size_t)(p - comp);
            if (!len || comp_bad(comp, len)) return -1;
            if (!ch) break;
            h = (h ^ '/') * 1099511628211ull;
            comp = p + 1;
            continue;
        }
        unsigned char c = t[ch];
        if (!c) return -1;
        h = (h ^ c) * 1099511628211ull;
        if (p - name > 4000) return -1;
    }
    *hash = h | 1;
    return (int)(p - name);
}

void ml_norm_root(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    if (!cap) return;
    for (; in && *in && n + 1 < cap; in++) {
        char ch = *in == '\\' ? '/' : (char)lc((unsigned char)*in);
        if (ch == '/' && n && out[n - 1] == '/') continue;
        out[n++] = ch;
    }
    while (n > 1 && out[n - 1] == '/') n--;
    out[n] = 0;
}

static int starts_dir(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    return n && !strncmp(s, prefix, n) && (s[n] == '/' || s[n] == 0) ? 1 : 0;
}

int ml_classify(const char *r, const char *game_norm, const char *docs_norm)
{
    char b[1024];
    if (strstr(r, "/steamapps/workshop/content/529340/")) return ML_ROOT_WORKSHOP;
    if (game_norm && *game_norm) {
        snprintf(b, sizeof b, "%s/game/dlc", game_norm);
        if (starts_dir(r, b) && strlen(r) > strlen(b)) return ML_ROOT_DLC;
        snprintf(b, sizeof b, "%s/binaries/v3x/content_packs", game_norm);
        if (starts_dir(r, b) && strlen(r) > strlen(b)) return ML_ROOT_PACK;
        static const char *inst[] = {"game", "jomini", "clausewitz", "platform_specific_game_data", NULL};
        for (int i = 0; inst[i]; i++) {
            snprintf(b, sizeof b, "%s/%s", game_norm, inst[i]);
            if (!strcmp(r, b)) return ML_ROOT_GAME;
        }
    }
    if (docs_norm && *docs_norm) {
        snprintf(b, sizeof b, "%s/mod", docs_norm);
        if (starts_dir(r, b) && strlen(r) > strlen(b)) return ML_ROOT_LOCAL;
    }
    return ML_ROOT_OTHER;
}

const char *ml_class_name(int c)
{
    static const char *n[] = {"other", "workshop", "dlc", "local", "game", "pack"};
    return c >= 0 && c <= 5 ? n[c] : "?";
}

const char *ml_root_leaf(const char *r)
{
    const char *s = strrchr(r, '/');
    return s ? s + 1 : r;
}

/* ---- set ----------------------------------------------------------------------------------- */

static uint64_t fnv(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 1099511628211ull;
    return h | 1;   /* 0 = empty slot */
}

void ml_set_free(ml_set *s)
{
    free(s->arena); free(s->offs); free(s->hash);
    memset(s, 0, sizeof *s);
}

static int grow(ml_set *s)
{
    uint32_t nc = s->cap ? s->cap * 2 : 1024;
    uint32_t *no = calloc(nc, sizeof *no);
    uint64_t *nh = calloc(nc, sizeof *nh);
    if (!no || !nh) { free(no); free(nh); return -1; }
    for (uint32_t i = 0; i < s->cap; i++) {
        if (!s->hash[i]) continue;
        uint32_t j = (uint32_t)s->hash[i] & (nc - 1);
        while (nh[j]) j = (j + 1) & (nc - 1);
        nh[j] = s->hash[i];
        no[j] = s->offs[i];
    }
    free(s->offs); free(s->hash);
    s->offs = no; s->hash = nh; s->cap = nc;
    return 0;
}

static int find_slot(const ml_set *s, const char *k, size_t n, uint64_t h, uint32_t *slot)
{
    uint32_t j = (uint32_t)h & (s->cap - 1);
    while (s->hash[j]) {
        if (s->hash[j] == h) {
            const char *e = s->arena + s->offs[j] + 1;
            if (!strncmp(e, k, n) && e[n] == 0) { *slot = j; return 1; }
        }
        j = (j + 1) & (s->cap - 1);
    }
    *slot = j;
    return 0;
}

static int add1(ml_set *s, const char *k, size_t n, int flags)
{
    uint32_t slot;
    uint64_t h = fnv(k, n);
    if (s->cap && find_slot(s, k, n, h, &slot)) return 0;
    if ((s->n + 1) * 4 >= s->cap * 3 && grow(s)) return -1;
    find_slot(s, k, n, h, &slot);
    if (s->alen + n + 2 > s->acap) {
        size_t na = s->acap ? s->acap * 2 : 65536;
        while (na < s->alen + n + 2) na *= 2;
        char *a = realloc(s->arena, na);
        if (!a) return -1;
        s->arena = a; s->acap = na;
    }
    if (s->alen > 0xFFFFFFF0u) return -1;
    s->offs[slot] = (uint32_t)s->alen;
    s->hash[slot] = h;
    s->arena[s->alen] = (char)flags;
    memcpy(s->arena + s->alen + 1, k, n);
    s->arena[s->alen + 1 + n] = 0;
    s->alen += n + 2;
    s->n++;
    if (flags & ML_SET_DIR) s->ndirs++; else s->nfiles++;
    return 1;
}

int ml_set_add(ml_set *s, const char *key, size_t len, int is_dir)
{
    int r = add1(s, key, len, is_dir ? ML_SET_DIR : 0);
    if (r < 0) return -1;
    for (size_t i = len; i-- > 0;)        /* parent directories */
        if (key[i] == '/') {
            int q = add1(s, key, i, ML_SET_DIR);
            if (q < 0) return -1;
            if (q == 0) break;               /* parents of a known dir are known */
        }
    return r;
}

int ml_set_has(const ml_set *s, const char *key, size_t len)
{
    uint32_t slot;
    return s->cap ? find_slot(s, key, len, fnv(key, len), &slot) : 0;
}

int ml_set_next(const ml_set *s, int i, const char **key, int *flags)
{
    for (; i >= 0 && (uint32_t)i < s->cap; i++)
        if (s->hash[i]) {
            *flags = s->arena[s->offs[i]];
            *key = s->arena + s->offs[i] + 1;
            return i;
        }
    return -1;
}

/* ---- content_load.json ------------------------------------------------------------------------ */

static const char *json_str(const char *p, char *out, size_t cap)   /* p at opening quote */
{
    size_t n = 0;
    if (*p != '"') return NULL;
    for (p++; *p && *p != '"'; p++) {
        unsigned c = (unsigned char)*p;
        if (c == '\\') {
            p++;
            if (!*p) return NULL;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                unsigned v = 0;
                for (int i = 1; i <= 4; i++) {
                    char h = p[i];
                    v = v * 16 + (h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 :
                                  h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0);
                    if (!h) return NULL;
                }
                p += 4;
                char u[3]; int k = 0;   /* BMP only (no surrogate pairs needed for paths here) */
                if (v < 0x80) u[k++] = (char)v;
                else if (v < 0x800) { u[k++] = (char)(0xC0 | v >> 6); u[k++] = (char)(0x80 | (v & 63)); }
                else { u[k++] = (char)(0xE0 | v >> 12); u[k++] = (char)(0x80 | ((v >> 6) & 63)); u[k++] = (char)(0x80 | (v & 63)); }
                for (int i = 0; i < k; i++) { if (n + 1 < cap) out[n++] = u[i]; }
                continue;
            }
            default: c = (unsigned char)*p;
            }
        }
        if (n + 1 < cap) out[n++] = (char)c;
    }
    if (*p != '"') return NULL;
    out[n] = 0;
    return p + 1;
}

int ml_parse_playset(const char *j, void (*cb)(const char *, void *), void *u)
{
    const char *p = strstr(j, "\"enabledMods\"");
    if (!p) return -1;
    p = strchr(p, '[');
    if (!p) return -1;
    int depth = 0, n = 0;
    char key[64], val[1024];
    for (p++; *p; ) {
        if (*p == '"') {
            const char *q = json_str(p, key, sizeof key);
            if (!q) return -1;
            p = q;
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
            if (*p == ':' && depth == 1) {
                p++;
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
                if (*p == '"') {
                    q = json_str(p, val, sizeof val);
                    if (!q) return -1;
                    p = q;
                    if (!strcmp(key, "path")) { if (cb) cb(val, u); n++; }
                }
            }
            continue;
        }
        if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') { if (depth == 0) break; depth--; }
        p++;
    }
    return n;
}

/* ---- error.log ------------------------------------------------------------------------------- */

static const char *ROOTS_DIRS[] = {"common/", "events/", "gui/", "gfx/", "localization/", "map_data/",
                                   "interface/", "fonts/", "sound/", "music/", "data_binding/", NULL};

static int is_stamp(const char *s)   /* "[hh:mm:ss]" */
{
    for (int i = 0; i < 10; i++) if (!s[i]) return 0;   /* never read past the terminator (found by fuzz_mod_loader) */
    return s[0] == '[' && s[3] == ':' && s[6] == ':' && s[9] == ']';
}

int ml_errlog_next(const char *t, size_t *pos, char *path, size_t pathn, int *kind)
{
    size_t p = *pos;
    while (t[p] && !is_stamp(t + p)) {           /* skip to the next entry */
        while (t[p] && t[p] != '\n') p++;
        if (t[p]) p++;
    }
    if (!t[p]) { *pos = p; return 0; }
    size_t s = p;
    do {                                       /* entry = this line + continuation lines */
        while (t[p] && t[p] != '\n') p++;
        if (t[p]) p++;
    } while (t[p] && !is_stamp(t + p));
    *pos = p;
    size_t n = p - s;
    path[0] = 0;
    *kind = ML_ERR_OTHER;
    for (size_t i = s; i < p; i++) {
        if (i > s && (unsigned char)t[i - 1] > ' ' && t[i - 1] != '\'' && t[i - 1] != '"' && t[i - 1] != '(' &&
            t[i - 1] != ',')
            continue;
        for (int r = 0; ROOTS_DIRS[r]; r++) {
            size_t rl = strlen(ROOTS_DIRS[r]);
            if (i + rl <= p && !strncmp(t + i, ROOTS_DIRS[r], rl)) {
                size_t k = 0;
                for (size_t q = i; q < p && k + 1 < pathn; q++) {
                    char c = t[q];
                    if ((unsigned char)c <= ' ' || c == ':' || c == '\'' || c == '"' || c == ',' || c == '(' ||
                        c == ')' || c == ']')
                        break;
                    path[k++] = (char)lc((unsigned char)c);
                }
                while (k && path[k - 1] == '.') k--;
                path[k] = 0;
                goto found;
            }
        }
    }
found:;
    /* kind from the source location / message (first line only) */
    size_t e1 = s;
    while (e1 < s + n && t[e1] != '\n') e1++;
    char line[512];
    size_t ln = e1 - s < sizeof line - 1 ? e1 - s : sizeof line - 1;
    memcpy(line, t + s, ln);
    line[ln] = 0;
    if (strstr(line, "persistent_reader") || strstr(line, "Unexpected token") || strstr(line, "Expected '"))
        *kind = ML_ERR_PARSE;
    else if (strstr(line, "Duplicated key") || strstr(line, "Duplicated event ID") || strstr(line, "Duplicate"))
        *kind = ML_ERR_DUPKEY;
    else if (strstr(line, "localiz") || strstr(line, "loc key"))
        *kind = ML_ERR_LOC;
    return 1;
}

/* ---- compact index ------------------------------------------------------------------------------- */

uint64_t ml_hash(const char *key, size_t len) { return fnv(key, len); }

int ml_hv_push(ml_hv *v, uint64_t h)
{
    if (v->n == v->cap) {
        uint32_t nc = v->cap ? v->cap * 2 : 256;
        uint64_t *p = realloc(v->h, (size_t)nc * sizeof *p);
        if (!p) return -1;
        v->h = p; v->cap = nc;
    }
    v->h[v->n++] = h;
    return 0;
}

static int radix_sort(uint64_t *a, uint32_t n)   /* LSD, 4 x 16 bit; 0 ok, -1 oom */
{
    uint64_t *tmp = malloc((size_t)n * sizeof *tmp);
    uint32_t *cnt = malloc(65536 * sizeof *cnt);
    if (!tmp || !cnt) { free(tmp); free(cnt); return -1; }
    for (int sh = 0; sh < 64; sh += 16) {
        memset(cnt, 0, 65536 * sizeof *cnt);
        for (uint32_t i = 0; i < n; i++) cnt[(a[i] >> sh) & 0xFFFF]++;
        uint32_t sum = 0;
        for (uint32_t i = 0; i < 65536; i++) { uint32_t c = cnt[i]; cnt[i] = sum; sum += c; }
        for (uint32_t i = 0; i < n; i++) tmp[cnt[(a[i] >> sh) & 0xFFFF]++] = a[i];
        memcpy(a, tmp, (size_t)n * sizeof *a);
    }
    free(tmp); free(cnt);
    return 0;
}

static int cmp_u64(const void *x, const void *y)
{
    uint64_t a = *(const uint64_t *)x, b = *(const uint64_t *)y;
    return a < b ? -1 : a > b;
}

void ml_hv_finish(ml_hv *v)
{
    if (!v->n) return;
    if (v->n < 4096 || radix_sort(v->h, v->n)) qsort(v->h, v->n, sizeof *v->h, cmp_u64);
    uint32_t k = 1;
    for (uint32_t i = 1; i < v->n; i++)
        if (v->h[i] != v->h[k - 1]) v->h[k++] = v->h[i];
    v->n = k;
    uint64_t *p = realloc(v->h, (size_t)k * sizeof *p);   /* shrink: exactly 8 bytes per key */
    if (p) { v->h = p; v->cap = k; }
}

int ml_hv_has(const ml_hv *v, uint64_t h)
{
    if (!v->n) return 0;
    const uint64_t *a = v->h;
    int64_t lo = 0, hi = (int64_t)v->n - 1;
    for (int it = 0; it < 4 && lo < hi; it++) {          /* interpolation steps (hashes are uniform) */
        if (h < a[lo] || h > a[hi]) return 0;
        uint64_t span = a[hi] - a[lo];
        int64_t pos = lo + (int64_t)(((unsigned __int128)(h - a[lo]) * (uint64_t)(hi - lo)) / span);
        if (a[pos] == h) return 1;
        if (a[pos] < h) lo = pos + 1; else hi = pos - 1;
    }
    while (lo <= hi) {                                   /* binary fallback */
        int64_t m = lo + (hi - lo) / 2;
        if (a[m] == h) return 1;
        if (a[m] < h) lo = m + 1; else hi = m - 1;
    }
    return 0;
}

void ml_hv_free(ml_hv *v) { free(v->h); memset(v, 0, sizeof *v); }

int ml_dirs_add(ml_dirs *d, const char *rel, uint64_t mtime)
{
    size_t l = strlen(rel);
    if (l > 0xFFFF) return -1;
    if (d->n == d->cap) {
        uint32_t nc = d->cap ? d->cap * 2 : 64;
        uint32_t *o = realloc(d->off, nc * sizeof *o);
        if (!o) return -1;
        d->off = o;
        uint64_t *m = realloc(d->mt, nc * sizeof *m);
        if (!m) return -1;
        d->mt = m; d->cap = nc;
    }
    if (d->nlen + l + 1 > d->ncap) {
        size_t nc = d->ncap ? d->ncap * 2 : 4096;
        while (nc < d->nlen + l + 1) nc *= 2;
        char *p = realloc(d->names, nc);
        if (!p) return -1;
        d->names = p; d->ncap = nc;
    }
    if (d->nlen > 0xFFFFFFF0u) return -1;
    d->off[d->n] = (uint32_t)d->nlen;
    d->mt[d->n++] = mtime;
    memcpy(d->names + d->nlen, rel, l + 1);
    d->nlen += l + 1;
    return 0;
}

const char *ml_dirs_name(const ml_dirs *d, uint32_t i) { return d->names + d->off[i]; }
void ml_dirs_free(ml_dirs *d) { free(d->names); free(d->off); free(d->mt); memset(d, 0, sizeof *d); }

static const char IX_MAGIC[8] = {'V', '3', 'X', 'M', 'L', 'I', 'X', 0};

typedef struct { uint8_t *p; size_t n, cap; } wbuf;
static void wput(wbuf *b, const void *src, size_t n)
{
    if (b->p && b->n + n <= b->cap) memcpy(b->p + b->n, src, n);
    b->n += n;
}

size_t ml_ixfile_write(const ml_ixroot *const *r, int n, uint8_t *out, size_t cap)
{
    wbuf b = {out, 0, cap};
    uint32_t v = ML_IXFILE_VERSION, nr = (uint32_t)n;
    wput(&b, IX_MAGIC, 8); wput(&b, &v, 4); wput(&b, &nr, 4);
    for (int i = 0; i < n; i++) {
        uint16_t l = (uint16_t)strlen(r[i]->root);
        wput(&b, &l, 2); wput(&b, r[i]->root, l);
        wput(&b, &r[i]->dirs.n, 4);
        for (uint32_t k = 0; k < r[i]->dirs.n; k++) {
            const char *nm = ml_dirs_name(&r[i]->dirs, k);
            uint16_t dl = (uint16_t)strlen(nm);
            wput(&b, &dl, 2); wput(&b, nm, dl); wput(&b, &r[i]->dirs.mt[k], 8);
        }
        wput(&b, &r[i]->hv.n, 4);
        wput(&b, r[i]->hv.h, (size_t)r[i]->hv.n * 8);
    }
    if (!out) return b.n + 8;
    if (b.n + 8 > cap) return 0;
    uint64_t sum = fnv((const char *)out, b.n);
    memcpy(out + b.n, &sum, 8);
    return b.n + 8;
}

void ml_ixfile_free(ml_ixroot *r, int n)
{
    if (!r) return;
    for (int i = 0; i < n; i++) { ml_dirs_free(&r[i].dirs); ml_hv_free(&r[i].hv); }
    free(r);
}

int ml_ixfile_read(const uint8_t *buf, size_t len, ml_ixroot **out)
{
    *out = NULL;
    if (len < 24 || memcmp(buf, IX_MAGIC, 8)) return -1;
    uint64_t sum;
    memcpy(&sum, buf + len - 8, 8);
    if (fnv((const char *)buf, len - 8) != sum) return -1;
    size_t end = len - 8, p = 8;
    uint32_t ver, nr;
    memcpy(&ver, buf + p, 4); memcpy(&nr, buf + p + 4, 4); p += 8;
    if (ver != ML_IXFILE_VERSION || nr > 4096) return -1;
    ml_ixroot *r = calloc(nr ? nr : 1, sizeof *r);
    if (!r) return -1;
    char *nm = malloc(0x10001);
    if (!nm) { free(r); return -1; }
#define NEED(k) do { if (end - p < (size_t)(k)) goto bad; } while (0)
    for (uint32_t i = 0; i < nr; i++) {
        uint16_t l;
        NEED(2); memcpy(&l, buf + p, 2); p += 2;
        if (l >= sizeof r[i].root) goto bad;
        NEED(l); memcpy(r[i].root, buf + p, l); r[i].root[l] = 0; p += l;
        uint32_t nd;
        NEED(4); memcpy(&nd, buf + p, 4); p += 4;
        for (uint32_t k = 0; k < nd; k++) {
            uint16_t dl; uint64_t mt;
            NEED(2); memcpy(&dl, buf + p, 2); p += 2;
            NEED((size_t)dl + 8); memcpy(nm, buf + p, dl); nm[dl] = 0; p += dl;
            memcpy(&mt, buf + p, 8); p += 8;
            if (ml_dirs_add(&r[i].dirs, nm, mt)) goto bad;
        }
        uint32_t nh;
        NEED(4); memcpy(&nh, buf + p, 4); p += 4;
        if (nh > (end - p) / 8) goto bad;
        if (nh) {
            r[i].hv.h = malloc((size_t)nh * 8);
            if (!r[i].hv.h) goto bad;
            memcpy(r[i].hv.h, buf + p, (size_t)nh * 8);
            r[i].hv.n = r[i].hv.cap = nh;
            p += (size_t)nh * 8;
            for (uint32_t k = 1; k < nh; k++) if (r[i].hv.h[k] <= r[i].hv.h[k - 1]) goto bad;
        }
    }
#undef NEED
    if (p != end) goto bad;
    free(nm);
    *out = r;
    return (int)nr;
bad:
    free(nm);
    ml_ixfile_free(r, (int)nr);
    return -1;
}
