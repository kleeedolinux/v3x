/* up_logic.c - see up_logic.h. Pure C + stdio; no V3X API, no network (that is up_http_fn). */
#include "up_logic.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void up_copy(char *dst, size_t cap, const char *src)
{
    if (!cap) return;
    snprintf(dst, cap, "%s", src ? src : "");
}

static int up_ieq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) a++, b++;
    return !*a && !*b;
}

static int up_starts(const char *s, const char *prefix)
{
    return s && !strncmp(s, prefix, strlen(prefix));
}

int up_name_ok(const char *name)
{
    if (!name) return 0;
    size_t n = strlen(name);
    if (n == 0 || n > 63 || strstr(name, "..") || name[0] == '.' || name[n - 1] == '.') return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!(isalnum(c) || c == '.' || c == '-' || c == '_')) return 0;
    }
    return 1;
}

/* ================================================================ JSON scanner */

const char *up_js_ws(const char *p, const char *e)
{
    while (p && p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static const char *js_str_end(const char *p, const char *e)   /* p at '"' */
{
    if (!p || p >= e || *p != '"') return NULL;
    for (p++; p < e; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') return p + 1;
        if (c < 0x20) return NULL;
        if (c == '\\') {
            if (++p >= e) return NULL;
            if (*p == 'u') {
                for (int i = 0; i < 4; i++)
                    if (++p >= e || !isxdigit((unsigned char)*p)) return NULL;
            } else if (!strchr("\"\\/bfnrt", *p)) {
                return NULL;
            }
        }
    }
    return NULL;
}

static const char *js_num_end(const char *p, const char *e)
{
    const char *s = p;
    if (p < e && *p == '-') p++;
    if (p >= e || !isdigit((unsigned char)*p)) return NULL;
    if (*p == '0') p++;
    else while (p < e && isdigit((unsigned char)*p)) p++;
    if (p < e && *p == '.') {
        p++;
        if (p >= e || !isdigit((unsigned char)*p)) return NULL;
        while (p < e && isdigit((unsigned char)*p)) p++;
    }
    if (p < e && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < e && (*p == '+' || *p == '-')) p++;
        if (p >= e || !isdigit((unsigned char)*p)) return NULL;
        while (p < e && isdigit((unsigned char)*p)) p++;
    }
    return p > s ? p : NULL;
}

static const char *js_skip_d(const char *p, const char *e, int depth)
{
    p = up_js_ws(p, e);
    if (!p || p >= e || depth > UP_JS_DEPTH) return NULL;
    char c = *p;
    if (c == '"') return js_str_end(p, e);
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        p = up_js_ws(p + 1, e);
        if (p < e && *p == close) return p + 1;
        for (;;) {
            if (c == '{') {
                p = js_str_end(up_js_ws(p, e), e);
                p = up_js_ws(p, e);
                if (!p || p >= e || *p != ':') return NULL;
                p++;
            }
            p = js_skip_d(p, e, depth + 1);
            p = up_js_ws(p, e);
            if (!p || p >= e) return NULL;
            if (*p == close) return p + 1;
            if (*p != ',') return NULL;
            p++;
        }
    }
    if (c == '-' || isdigit((unsigned char)c)) return js_num_end(p, e);
    static const char *const lit[] = {"true", "false", "null"};
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(lit[i]);
        if ((size_t)(e - p) >= n && !memcmp(p, lit[i], n)) return p + n;
    }
    return NULL;
}

const char *up_js_skip(const char *p, const char *e) { return js_skip_d(p, e, 0); }

static void utf8_put(char *out, size_t cap, size_t *k, unsigned cp, int *overflow)
{
    char b[4];
    int n;
    if (cp < 0x80) { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | cp >> 6); b[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | cp >> 12); b[1] = (char)(0x80 | ((cp >> 6) & 63)); b[2] = (char)(0x80 | (cp & 63)); n = 3;
    } else {
        b[0] = (char)(0xF0 | cp >> 18); b[1] = (char)(0x80 | ((cp >> 12) & 63));
        b[2] = (char)(0x80 | ((cp >> 6) & 63)); b[3] = (char)(0x80 | (cp & 63)); n = 4;
    }
    if (*k + (size_t)n >= cap) { *overflow = 1; return; }
    memcpy(out + *k, b, (size_t)n);
    *k += (size_t)n;
}

static unsigned hex4(const char *p)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v = v * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
    }
    return v;
}

int up_js_string(const char *p, const char *e, char *out, size_t cap)
{
    p = up_js_ws(p, e);
    const char *end = js_str_end(p, e);
    if (!end || !out || !cap) return -1;
    size_t k = 0;
    int of = 0;
    for (const char *q = p + 1; q < end - 1 && !of; q++) {
        if (*q != '\\') {
            if (k + 1 >= cap) { of = 1; break; }
            out[k++] = *q;
            continue;
        }
        q++;
        char c = *q;
        if (c == 'u') {
            unsigned cp = hex4(q + 1);
            q += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && q + 6 < end && q[1] == '\\' && q[2] == 'u') {
                unsigned lo = hex4(q + 3);
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); q += 6; }
                else cp = '?';
            } else if (cp >= 0xD800 && cp < 0xE000) {
                cp = '?';
            }
            if (!cp) cp = '?';   /* no embedded NULs */
            utf8_put(out, cap, &k, cp, &of);
            continue;
        }
        char r = c == 'b' ? '\b' : c == 'f' ? '\f' : c == 'n' ? '\n' : c == 'r' ? '\r' : c == 't' ? '\t' : c;
        if (k + 1 >= cap) { of = 1; break; }
        out[k++] = r;
    }
    if (of) { out[0] = 0; return -1; }
    out[k] = 0;
    return (int)k;
}

int up_js_int(const char *p, const char *e, long long *out)
{
    p = up_js_ws(p, e);
    const char *end = js_num_end(p, e);
    if (!end || !out) return -1;
    int neg = *p == '-';
    if (neg) p++;
    long long v = 0;
    for (; p < end && isdigit((unsigned char)*p); p++) {
        if (v > (0x7fffffffffffffffLL - 9) / 10) return -1;
        v = v * 10 + (*p - '0');
    }
    *out = neg ? -v : v;
    return 0;
}

int up_js_bool(const char *p, const char *e, int *out)
{
    p = up_js_ws(p, e);
    if (!p || !out) return -1;
    if (e - p >= 4 && !memcmp(p, "true", 4)) { *out = 1; return 0; }
    if (e - p >= 5 && !memcmp(p, "false", 5)) { *out = 0; return 0; }
    return -1;
}

int up_js_begin(up_js_it *it, const char *p, const char *e, char open)
{
    memset(it, 0, sizeof *it);
    p = up_js_ws(p, e);
    if (!p || p >= e || *p != open) { it->err = 1; return -1; }
    it->p = p + 1;
    it->e = e;
    it->first = 1;
    it->close = open == '{' ? '}' : ']';
    return 0;
}

int up_js_next(up_js_it *it, char *key, size_t kcap, const char **v)
{
    if (it->err || !it->p) return 0;
    const char *e = it->e, *p = up_js_ws(it->p, e);
    if (p >= e) { it->err = 1; return 0; }
    if (*p == it->close) { it->p = NULL; return 0; }
    if (*p == '}' || *p == ']') { it->err = 1; return 0; }   /* mismatched bracket */
    if (!it->first) {
        if (*p != ',') { it->err = 1; return 0; }
        p = up_js_ws(p + 1, e);
        if (p >= e || *p == '}' || *p == ']') { it->err = 1; return 0; }   /* trailing comma */
    }
    it->first = 0;
    if (key) {
        char tmp[256];
        const char *ke = js_str_end(p, e);
        if (!ke) { it->err = 1; return 0; }
        if (up_js_string(p, ke, tmp, sizeof tmp) < 0) tmp[0] = 0;   /* key too long: no match */
        up_copy(key, kcap, tmp);
        p = up_js_ws(ke, e);
        if (p >= e || *p != ':') { it->err = 1; return 0; }
        p = up_js_ws(p + 1, e);
    }
    const char *ve = up_js_skip(p, e);
    if (!ve) { it->err = 1; return 0; }
    *v = p;
    it->p = ve;
    return 1;
}

int up_js_get(const char *obj, const char *e, const char *key, const char **v)
{
    up_js_it it;
    if (up_js_begin(&it, obj, e, '{')) return -1;
    char k[128];
    const char *val, *hit = NULL;
    while (up_js_next(&it, k, sizeof k, &val))
        if (!hit && !strcmp(k, key)) hit = val;   /* first wins; keep scanning to validate */
    if (it.err) return -1;
    if (!hit) return 0;
    *v = hit;
    return 1;
}

/* helpers on object members */
static int js_get_str(const char *obj, const char *e, const char *key, char *out, size_t cap)
{
    const char *v;
    if (up_js_get(obj, e, key, &v) != 1) return -1;
    return up_js_string(v, e, out, cap) < 0 ? -1 : 0;
}
static int js_get_bool(const char *obj, const char *e, const char *key, int def)
{
    const char *v;
    int b;
    if (up_js_get(obj, e, key, &v) != 1 || up_js_bool(v, e, &b)) return def;
    return b;
}
static long long js_get_int(const char *obj, const char *e, const char *key, long long def)
{
    const char *v;
    long long x;
    if (up_js_get(obj, e, key, &v) != 1 || up_js_int(v, e, &x)) return def;
    return x;
}

/* ================================================================ semver */

static int pre_cmp(const char *a, const char *b)   /* semver 2.0 precedence of pre-release strings */
{
    if (!*a || !*b) return *a ? -1 : *b ? 1 : 0;   /* no pre-release > any pre-release */
    while (*a || *b) {
        if (!*a) return -1;
        if (!*b) return 1;
        const char *ae = strchr(a, '.'), *be = strchr(b, '.');
        size_t an = ae ? (size_t)(ae - a) : strlen(a), bn = be ? (size_t)(be - b) : strlen(b);
        int anum = an > 0, bnum = bn > 0;
        for (size_t i = 0; i < an; i++) if (!isdigit((unsigned char)a[i])) anum = 0;
        for (size_t i = 0; i < bn; i++) if (!isdigit((unsigned char)b[i])) bnum = 0;
        int c;
        if (anum && bnum) {
            while (an > 1 && *a == '0') a++, an--;
            while (bn > 1 && *b == '0') b++, bn--;
            c = an != bn ? (an < bn ? -1 : 1) : memcmp(a, b, an);
        } else if (anum != bnum) {
            c = anum ? -1 : 1;   /* numeric identifiers sort lower */
        } else {
            c = memcmp(a, b, an < bn ? an : bn);
            if (!c && an != bn) c = an < bn ? -1 : 1;
        }
        if (c) return c < 0 ? -1 : 1;
        a += an; b += bn;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

int up_semver_parse(const char *s, up_semver *v)
{
    if (!s || !v) return -1;
    memset(v, 0, sizeof *v);
    if (*s == 'v' || *s == 'V') s++;
    long long *f[3] = {&v->maj, &v->min, &v->pat};
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*s)) return -1;
        long long x = 0;
        int digits = 0;
        while (isdigit((unsigned char)*s)) {
            if (++digits > 9) return -1;
            x = x * 10 + (*s++ - '0');
        }
        *f[i] = x;
        if (i < 2 && *s++ != '.') return -1;
    }
    if (*s == '-') {
        s++;
        size_t k = 0;
        while (*s && *s != '+') {
            if (!isalnum((unsigned char)*s) && *s != '.' && *s != '-') return -1;
            if (k + 1 >= sizeof v->pre) return -1;
            v->pre[k++] = *s++;
        }
        v->pre[k] = 0;
        if (!k || v->pre[0] == '.' || v->pre[k - 1] == '.' || strstr(v->pre, "..")) return -1;
    }
    if (*s == '+') {
        s++;
        if (!*s) return -1;
        for (; *s; s++)
            if (!isalnum((unsigned char)*s) && *s != '.' && *s != '-') return -1;
    }
    return *s ? -1 : 0;
}

int up_semver_cmp(const up_semver *a, const up_semver *b)
{
    if (a->maj != b->maj) return a->maj < b->maj ? -1 : 1;
    if (a->min != b->min) return a->min < b->min ? -1 : 1;
    if (a->pat != b->pat) return a->pat < b->pat ? -1 : 1;
    return pre_cmp(a->pre, b->pre);
}

int up_version_cmp(const char *a, const char *b)
{
    up_semver x, y;
    int ox = up_semver_parse(a, &x) == 0, oy = up_semver_parse(b, &y) == 0;
    if (!ox || !oy) return ox - oy;
    return up_semver_cmp(&x, &y);
}

/* ================================================================ GitHub list + compat.json */

int up_parse_releases(const char *json, size_t n, up_catalog *cat)
{
    memset(cat, 0, sizeof *cat);
    if (!json) return -1;
    const char *e = json + n;
    up_js_it it;
    if (up_js_begin(&it, json, e, '[')) return -1;
    const char *v;
    while (up_js_next(&it, NULL, 0, &v)) {
        if (*v != '{' || cat->n >= UP_MAX_REL) continue;
        up_release *r = &cat->r[cat->n];
        memset(r, 0, sizeof *r);
        if (js_get_str(v, e, "tag_name", r->tag, sizeof r->tag)) continue;
        up_semver sv;
        const char *ver = r->tag[0] == 'v' || r->tag[0] == 'V' ? r->tag + 1 : r->tag;
        if (up_semver_parse(ver, &sv)) continue;   /* not a version tag */
        up_copy(r->version, sizeof r->version, ver);
        r->draft = js_get_bool(v, e, "draft", 1);          /* missing = treat as draft (fail closed) */
        r->prerelease = js_get_bool(v, e, "prerelease", 1);
        if (sv.pre[0]) r->prerelease = 1;
        const char *as;
        if (up_js_get(v, e, "assets", &as) == 1 && *as == '[') {
            up_js_it ai;
            const char *a;
            up_js_begin(&ai, as, e, '[');
            while (up_js_next(&ai, NULL, 0, &a)) {
                if (*a != '{') continue;
                char name[64], url[400];
                if (js_get_str(a, e, "name", name, sizeof name) || js_get_str(a, e, "browser_download_url", url, sizeof url))
                    continue;
                if (!up_url_allowed(url)) continue;
                if (!strcmp(name, "compat.json")) up_copy(r->compat_url, sizeof r->compat_url, url);
                else if (!strcmp(name, "compat.json.sig")) up_copy(r->sig_url, sizeof r->sig_url, url);
                else if (!strcmp(name, "SHA256SUMS.txt") || !strcmp(name, "SHA256SUMS"))
                    up_copy(r->sums_url, sizeof r->sums_url, url);
                else if (r->nassets < UP_MAX_ASSETS) {
                    up_asset *x = &r->assets[r->nassets++];
                    up_copy(x->name, sizeof x->name, name);
                    up_copy(x->url, sizeof x->url, url);
                    x->size = js_get_int(a, e, "size", 0);
                }
            }
        }
        cat->n++;
    }
    return it.err ? -1 : cat->n;
}

static int compat_fail(up_release *r, const char *why)
{
    r->compat = -1;
    up_copy(r->compat_err, sizeof r->compat_err, why);
    r->nbuilds = 0;
    r->setup_sha[0] = 0;
    return -1;
}

int up_parse_compat(const char *json, size_t n, up_release *r)
{
    if (!json) return compat_fail(r, "empty compat.json");
    const char *e = json + n, *p = json;
    if (n >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    p = up_js_ws(p, e);
    if (!p || p >= e || *p != '{' || up_js_skip(p, e) == NULL) return compat_fail(r, "malformed compat.json");
    char ver[48];
    if (js_get_str(p, e, "v3x_version", ver, sizeof ver)) return compat_fail(r, "compat.json: no v3x_version");
    if (up_version_cmp(ver, r->version) != 0)
        return compat_fail(r, "compat.json: v3x_version does not match the tag");
    long long api = js_get_int(p, e, "api_version", 0);
    if (api <= 0 || api > 1000) return compat_fail(r, "compat.json: no api_version");
    r->api_version = (int)api;
    const char *gb;
    r->nbuilds = 0;
    if (up_js_get(p, e, "game_builds", &gb) != 1 || *gb != '[') return compat_fail(r, "compat.json: no game_builds");
    up_js_it it;
    const char *b;
    up_js_begin(&it, gb, e, '[');
    while (up_js_next(&it, NULL, 0, &b)) {
        if (*b != '{' || r->nbuilds >= UP_MAX_BUILDS) continue;
        up_build *x = &r->builds[r->nbuilds];
        memset(x, 0, sizeof *x);
        js_get_str(b, e, "branch", x->branch, sizeof x->branch);
        js_get_str(b, e, "build_id", x->build_id, sizeof x->build_id);
        if (x->branch[0] || x->build_id[0]) r->nbuilds++;
    }
    if (it.err) return compat_fail(r, "compat.json: malformed game_builds");
    /* assets, two layouts:
     *   installer/make_dist.py: [{"name": "V3X-Setup.exe", "size": N, "sha256": "<hex>"}, ...]
     *   older drafts:           {"setup": "V3X-Setup.exe", "sha256": {"V3X-Setup.exe": "<hex>", ...}} */
    char setup[64] = UP_SETUP_NAME;
    const char *as, *sh, *hv;
    long long csize = 0;
    r->setup_sha[0] = 0;
    int ha = up_js_get(p, e, "assets", &as);
    if (ha == 1 && *as == '{') {
        char s2[64];
        if (!js_get_str(as, e, "setup", s2, sizeof s2) && s2[0]) {
            if (!up_name_ok(s2)) return compat_fail(r, "compat.json: bad setup name");
            up_copy(setup, sizeof setup, s2);
        } else if (up_js_get(as, e, "setup", &hv) == 1) {
            return compat_fail(r, "compat.json: bad setup name");
        }
        if (up_js_get(as, e, "sha256", &sh) == 1 && *sh == '{' && up_js_get(sh, e, setup, &hv) == 1) {
            char hex[80];
            if (up_js_string(hv, e, hex, sizeof hex) == 64 && up_hex_valid(hex)) up_copy(r->setup_sha, sizeof r->setup_sha, hex);
            else return compat_fail(r, "compat.json: bad sha256");
        }
    } else if (ha == 1 && *as == '[') {
        up_js_it ai;
        const char *a;
        up_js_begin(&ai, as, e, '[');
        while (up_js_next(&ai, NULL, 0, &a)) {
            char nm[64], hex[80];
            if (*a != '{' || js_get_str(a, e, "name", nm, sizeof nm) || strcmp(nm, setup)) continue;
            if (js_get_str(a, e, "sha256", hex, sizeof hex) || strlen(hex) != 64 || !up_hex_valid(hex))
                return compat_fail(r, "compat.json: bad sha256");
            up_copy(r->setup_sha, sizeof r->setup_sha, hex);
            csize = js_get_int(a, e, "size", 0);
            if (csize < 0 || csize > (long long)UP_MAX_SETUP) return compat_fail(r, "compat.json: bad size");
        }
        if (ai.err) return compat_fail(r, "compat.json: malformed assets");
    }
    if (!up_name_ok(setup)) return compat_fail(r, "compat.json: bad setup name");
    up_copy(r->setup_name, sizeof r->setup_name, setup);
    r->setup_url[0] = 0;
    r->setup_size = 0;
    for (int i = 0; i < r->nassets; i++)
        if (!strcmp(r->assets[i].name, setup)) {
            up_copy(r->setup_url, sizeof r->setup_url, r->assets[i].url);
            r->setup_size = r->assets[i].size;
        }
    if (csize > 0) {   /* the (possibly signed) compat.json's size wins over the GitHub listing */
        if (r->setup_size > 0 && r->setup_size != csize) return compat_fail(r, "compat.json: size differs from the release asset");
        r->setup_size = csize;
    }
    r->compat = 1;
    r->compat_err[0] = 0;
    return 0;
}

int up_apply_compat(up_release *r, const char *json, size_t n, const char *sig, size_t sn)
{
    r->sig = 0;
    r->key_id[0] = 0;
    r->sig_line[0] = 0;
    r->compat_raw_n = 0;
    if (sig) {
        const char *why = "";
        char kid[17] = "";
        if (up_sig_check(json, n, sig, sn, kid, &why) < 0) {
            char m[64];
            snprintf(m, sizeof m, "compat.json signature: %s", why);
            return compat_fail(r, m);
        }
        if (n >= sizeof r->compat_raw || sn >= sizeof r->sig_line) return compat_fail(r, "compat.json: too large");
        if (up_parse_compat(json, n, r)) return -1;
        if (!r->setup_sha[0]) return compat_fail(r, "signed compat.json names no setup sha256");
        memcpy(r->compat_raw, json, n);
        r->compat_raw[n] = 0;
        r->compat_raw_n = n;
        memcpy(r->sig_line, sig, sn);
        r->sig_line[sn] = 0;
        memcpy(r->key_id, kid, sizeof kid);
        r->sig = 1;
        return 0;
    }
    return up_parse_compat(json, n, r);
}

int up_sums_find(const char *text, size_t n, const char *name, char out_hex[65])
{
    out_hex[0] = 0;
    if (!text || !name) return -1;
    const char *p = text, *e = text + n;
    while (p < e) {
        const char *le = memchr(p, '\n', (size_t)(e - p));
        if (!le) le = e;
        const char *q = p;
        while (q < le && (*q == ' ' || *q == '\t')) q++;
        if (le - q >= 66) {
            char hex[65];
            memcpy(hex, q, 64);
            hex[64] = 0;
            const char *f = q + 64;
            if (up_hex_valid(hex) && (*f == ' ' || *f == '\t')) {
                while (f < le && (*f == ' ' || *f == '\t')) f++;
                if (f < le && *f == '*') f++;
                const char *fe = le;
                while (fe > f && (fe[-1] == '\r' || fe[-1] == ' ' || fe[-1] == '\t')) fe--;
                size_t k = strlen(name);
                if ((size_t)(fe - f) == k && !memcmp(f, name, k)) {
                    for (int i = 0; i < 65; i++) out_hex[i] = (char)tolower((unsigned char)hex[i]);
                    return 0;
                }
            }
        }
        p = le + 1;
    }
    return -1;
}

/* ================================================================ selection */

int up_release_supports(const up_release *r, const char *build_id, const char *branch)
{
    if (!r || r->compat != 1) return 0;
    for (int i = 0; i < r->nbuilds; i++) {
        const up_build *b = &r->builds[i];
        if (b->build_id[0]) {
            if (build_id && up_ieq(b->build_id, build_id)) return 1;
        } else if (b->branch[0] && branch && up_ieq(b->branch, branch)) {
            return 1;
        }
    }
    return 0;
}

int up_release_candidate(const up_release *r, const up_local *l)
{
    if (r->draft || (r->prerelease && !l->beta)) return 0;
    return up_version_cmp(r->version, l->loader_version ? l->loader_version : "0.0.0") > 0 &&
           up_semver_parse(r->version, &(up_semver){0}) == 0;
}

int up_release_eligible(const up_release *r, const up_local *l)
{
    if (l->floor && l->floor[0] && up_version_cmp(r->version, l->floor) < 0) return 0;   /* rollback floor */
    return up_release_candidate(r, l) && r->compat == 1 && r->api_version >= l->api_version &&
           up_release_supports(r, l->build_id, l->branch) && r->setup_url[0] && up_hex_valid(r->setup_sha);
}

int up_release_auto_ok(const up_release *r)
{
    return r && r->compat == 1 && r->sig == 1 && up_hex_valid(r->setup_sha);
}

up_choice up_select(const up_catalog *cat, const up_local *l)
{
    up_choice c = {UP_CURRENT, -1, -1};
    for (int i = 0; cat && i < cat->n; i++) {
        const up_release *r = &cat->r[i];
        if (!up_release_candidate(r, l)) continue;
        if (c.newest < 0 || up_version_cmp(r->version, cat->r[c.newest].version) > 0) c.newest = i;
        if (up_release_eligible(r, l) && (c.best < 0 || up_version_cmp(r->version, cat->r[c.best].version) > 0))
            c.best = i;
    }
    if (!l->build_supported) c.state = c.best >= 0 ? UP_URGENT : UP_UNSUPPORTED_NOFIX;
    else if (c.best >= 0) c.state = UP_AVAILABLE;
    else if (c.newest >= 0) c.state = UP_NEWER_INCOMPATIBLE;
    return c;
}

const char *up_state_name(up_state s)
{
    switch (s) {
    case UP_OFFLINE: return "offline";
    case UP_CURRENT: return "up-to-date";
    case UP_AVAILABLE: return "update-available";
    case UP_NEWER_INCOMPATIBLE: return "newer-incompatible";
    case UP_URGENT: return "urgent";
    case UP_UNSUPPORTED_NOFIX: return "unsupported";
    default: return "unknown";
    }
}

static void builds_list(const up_release *r, char *out, size_t cap)
{
    size_t k = 0;
    out[0] = 0;
    for (int i = 0; i < r->nbuilds && k + 1 < cap; i++) {
        const up_build *b = &r->builds[i];
        int n = snprintf(out + k, cap - k, "%s%s", i ? ", " : "", b->branch[0] ? b->branch : b->build_id);
        if (n < 0) break;
        k += (size_t)n;
        if (k >= cap) { out[cap - 1] = 0; break; }
    }
    if (!out[0]) up_copy(out, cap, "no game builds listed");
}

size_t up_status_text(const up_catalog *cat, const up_local *l, up_choice c, char *out, size_t cap)
{
    char builds[200];
    const up_release *best = c.best >= 0 ? &cat->r[c.best] : NULL, *newest = c.newest >= 0 ? &cat->r[c.newest] : NULL;
    const char *game = l->branch && *l->branch ? l->branch : "?";
    int n = 0;
    switch (c.state) {
    case UP_OFFLINE:
        n = snprintf(out, cap, "V3X %s: could not check for updates (offline or GitHub unreachable).", l->loader_version);
        break;
    case UP_CURRENT:
        n = snprintf(out, cap, "V3X %s is up to date for your game (%s).", l->loader_version, game);
        break;
    case UP_AVAILABLE:
        n = snprintf(out, cap, "V3X %s is available for your game (%s); you have %s.", best->version, game, l->loader_version);
        if (newest && newest != best && n > 0 && (size_t)n < cap) {
            builds_list(newest, builds, sizeof builds);
            int m = snprintf(out + n, cap - (size_t)n, " (V3X %s exists too, but it supports only %s.)", newest->version, builds);
            n = m < 0 ? n : (size_t)m >= cap - (size_t)n ? (int)cap - 1 : n + m;   /* clamp: never past cap */
        }
        break;
    case UP_NEWER_INCOMPATIBLE:
        builds_list(newest, builds, sizeof builds);
        n = snprintf(out, cap,
                     "V3X %s exists but does NOT support your game (%s, build %s); it supports %s. Keeping %s "
                     "(it will be offered once your game is updated).",
                     newest->version, game, l->build_id ? l->build_id : "?", builds, l->loader_version);
        break;
    case UP_URGENT:
        n = snprintf(out, cap,
                     "Your game was updated (%s, build %s) and V3X %s does not support it yet: most plugins are switched "
                     "off. V3X %s supports it.",
                     game, l->build_id ? l->build_id : "?", l->loader_version, best->version);
        break;
    case UP_UNSUPPORTED_NOFIX:
        n = snprintf(out, cap,
                     "Your game was updated (%s, build %s) and no V3X release supports it yet: most plugins stay off "
                     "until one is published.",
                     game, l->build_id ? l->build_id : "?");
        break;
    default:
        n = snprintf(out, cap, "update check has not run yet.");
    }
    if (n < 0) { if (cap) out[0] = 0; return 0; }
    return (size_t)n < cap ? (size_t)n : cap ? cap - 1 : 0;
}

/* ================================================================ rate limit */

void up_local_key(const up_local *l, char *out, size_t cap)
{
    snprintf(out, cap, "%s|%d|%s|%s|%s", l->loader_version ? l->loader_version : "", l->api_version,
             l->build_id ? l->build_id : "", l->branch ? l->branch : "", l->beta ? "beta" : "stable");
}

int up_should_fetch(const up_catalog *cache, const char *key, long long now, long long interval)
{
    if (!cache || cache->attempt_at <= 0) return 1;
    if (!key || strcmp(cache->key, key)) return 1;   /* new game build / loader / channel: check now */
    if (now < cache->attempt_at) return 1;           /* clock went back */
    return now - cache->attempt_at >= interval;
}

int up_force_allowed(long long last_attempt, long long now)
{
    return last_attempt <= 0 || now < last_attempt || now - last_attempt >= UP_FORCE_MIN_S;
}

/* ================================================================ cache file */

typedef struct { char *o; size_t cap, k; int of; } wbuf;
__attribute__((format(printf, 2, 3))) static void wb_printf(wbuf *w, const char *fmt, ...)
{
    if (w->of) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->o + w->k, w->cap - w->k, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= w->cap - w->k) { w->of = 1; return; }
    w->k += (size_t)n;
}
static void wb_str(wbuf *w, const char *s)
{
    wb_printf(w, "\"");
    for (; s && *s && !w->of; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') wb_printf(w, "\\%c", c);
        else if (c < 0x20) wb_printf(w, "\\u%04x", c);
        else wb_printf(w, "%c", c);
    }
    wb_printf(w, "\"");
}

size_t up_cache_write(const up_catalog *c, char *out, size_t cap)
{
    wbuf w = {out, cap, 0, 0};
    if (!out || !cap) return 0;
    wb_printf(&w, "{\"schema\":1,\"checked_at\":%lld,\"attempt_at\":%lld,\"key\":", c->checked_at, c->attempt_at);
    wb_str(&w, c->key);
    wb_printf(&w, ",\"releases\":[");
    for (int i = 0; i < c->n; i++) {
        const up_release *r = &c->r[i];
        wb_printf(&w, "%s\n{\"tag\":", i ? "," : "");
        wb_str(&w, r->tag);
        wb_printf(&w, ",\"version\":");
        wb_str(&w, r->version);
        wb_printf(&w, ",\"draft\":%s,\"prerelease\":%s,\"compat\":%d,\"compat_err\":", r->draft ? "true" : "false",
                  r->prerelease ? "true" : "false", r->compat);
        wb_str(&w, r->compat_err);
        wb_printf(&w, ",\"api_version\":%d,\"setup_name\":", r->api_version);
        wb_str(&w, r->setup_name);
        wb_printf(&w, ",\"setup_url\":");
        wb_str(&w, r->setup_url);
        wb_printf(&w, ",\"setup_sha\":");
        wb_str(&w, r->setup_sha);
        wb_printf(&w, ",\"setup_size\":%lld,\"sig_url\":", r->setup_size);
        wb_str(&w, r->sig_url);
        if (r->sig == 1) {   /* re-verified on every read; the cache never says "signed" by itself */
            wb_printf(&w, ",\"compat_raw\":");
            wb_str(&w, r->compat_raw);
            wb_printf(&w, ",\"compat_sig\":");
            wb_str(&w, r->sig_line);
        }
        wb_printf(&w, ",\"game_builds\":[");
        for (int j = 0; j < r->nbuilds; j++) {
            wb_printf(&w, "%s{\"branch\":", j ? "," : "");
            wb_str(&w, r->builds[j].branch);
            wb_printf(&w, ",\"build_id\":");
            wb_str(&w, r->builds[j].build_id);
            wb_printf(&w, "}");
        }
        wb_printf(&w, "]}");
    }
    wb_printf(&w, "]}\n");
    return w.of ? 0 : w.k;
}

int up_cache_read(const char *json, size_t n, up_catalog *c)
{
    memset(c, 0, sizeof *c);
    if (!json) return -1;
    const char *e = json + n, *p = up_js_ws(json, e);
    if (!p || p >= e || *p != '{' || !up_js_skip(p, e) || js_get_int(p, e, "schema", 0) != 1) return -1;
    c->checked_at = js_get_int(p, e, "checked_at", 0);
    c->attempt_at = js_get_int(p, e, "attempt_at", 0);
    js_get_str(p, e, "key", c->key, sizeof c->key);
    const char *rs, *v;
    if (up_js_get(p, e, "releases", &rs) != 1 || *rs != '[') { memset(c, 0, sizeof *c); return -1; }
    up_js_it it;
    up_js_begin(&it, rs, e, '[');
    while (up_js_next(&it, NULL, 0, &v)) {
        if (*v != '{' || c->n >= UP_MAX_REL) continue;
        up_release *r = &c->r[c->n];
        memset(r, 0, sizeof *r);
        if (js_get_str(v, e, "tag", r->tag, sizeof r->tag) || js_get_str(v, e, "version", r->version, sizeof r->version))
            continue;
        r->draft = js_get_bool(v, e, "draft", 1);
        r->prerelease = js_get_bool(v, e, "prerelease", 1);
        r->compat = (int)js_get_int(v, e, "compat", 0);
        if (r->compat != 1 && r->compat != -1) r->compat = 0;
        js_get_str(v, e, "compat_err", r->compat_err, sizeof r->compat_err);
        r->api_version = (int)js_get_int(v, e, "api_version", 0);
        js_get_str(v, e, "setup_name", r->setup_name, sizeof r->setup_name);
        js_get_str(v, e, "setup_url", r->setup_url, sizeof r->setup_url);
        js_get_str(v, e, "setup_sha", r->setup_sha, sizeof r->setup_sha);
        js_get_str(v, e, "sig_url", r->sig_url, sizeof r->sig_url);
        r->setup_size = js_get_int(v, e, "setup_size", 0);
        if (r->setup_url[0] && !up_url_allowed(r->setup_url)) r->setup_url[0] = 0;
        if (r->sig_url[0] && !up_url_allowed(r->sig_url)) r->sig_url[0] = 0;
        if (!up_hex_valid(r->setup_sha)) r->setup_sha[0] = 0;
        if (!up_name_ok(r->setup_name)) up_copy(r->setup_name, sizeof r->setup_name, UP_SETUP_NAME);   /* M4 */
        const char *gb, *b;
        if (up_js_get(v, e, "game_builds", &gb) == 1 && *gb == '[') {
            up_js_it bi;
            up_js_begin(&bi, gb, e, '[');
            while (up_js_next(&bi, NULL, 0, &b)) {
                if (*b != '{' || r->nbuilds >= UP_MAX_BUILDS) continue;
                up_build *x = &r->builds[r->nbuilds];
                js_get_str(b, e, "branch", x->branch, sizeof x->branch);
                js_get_str(b, e, "build_id", x->build_id, sizeof x->build_id);
                if (x->branch[0] || x->build_id[0]) r->nbuilds++;
            }
        }
        /* a cached signed compat.json counts only if its signature still verifies; then every field
         * that matters (builds, api, hash, size) comes from it, not from the editable cache */
        static char raw[UP_MAX_COMPAT_RAW], sg[128];
        int rn = js_get_str(v, e, "compat_raw", raw, sizeof raw) ? -1 : (int)strlen(raw);
        int sn = js_get_str(v, e, "compat_sig", sg, sizeof sg) ? -1 : (int)strlen(sg);
        if (rn > 0 && sn > 0) {
            char url[400];
            long long lsize = r->setup_size;
            up_copy(url, sizeof url, r->setup_url);
            r->nbuilds = 0;
            r->setup_sha[0] = 0;
            r->setup_size = 0;
            if (up_apply_compat(r, raw, (size_t)rn, sg, (size_t)sn) == 0) {
                if (!r->setup_url[0]) up_copy(r->setup_url, sizeof r->setup_url, url);
                if (lsize > 0 && r->setup_size > 0 && lsize != r->setup_size) r->setup_url[0] = 0;
            }
        } else {
            r->sig = 0;
        }
        c->n++;
    }
    if (it.err) { memset(c, 0, sizeof *c); return -1; }
    return 0;
}

/* ================================================================ network flow */

int up_url_allowed(const char *url)
{
    if (!url) return 0;
    for (const char *p = url; *p; p++)
        if ((unsigned char)*p <= ' ' || *p == '"' || *p == '\\' || (unsigned char)*p >= 0x7f) return 0;
    if (strstr(url, "..")) return 0;
    return up_starts(url, UP_ASSET_PREFIX) || up_starts(url, "https://api.github.com/repos/kleeedolinux/v3x/");
}

__attribute__((format(printf, 3, 4))) static void errf(char *err, size_t cap, const char *fmt, ...)
{
    if (!err || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static int http_get(up_http_fn get, void *ctx, const char *url, size_t max, char **body, size_t *len)
{
    *body = NULL;
    *len = 0;
    if (!up_url_allowed(url)) return -2;
    int st = get(ctx, url, max, body, len);
    if (st != 200) {
        free(*body);
        *body = NULL;
        *len = 0;
    } else if (!*body) {
        st = -3;
    }
    return st;
}

int up_fetch_catalog(up_http_fn get, void *ctx, const up_local *l, long long now, up_catalog *cat, char *err,
                     size_t errcap)
{
    static up_catalog fresh;   /* large; the plugin calls this from one thread, tests from main */
    char *body;
    size_t len;
    if (err && errcap) err[0] = 0;
    int st = http_get(get, ctx, UP_REPO_API, UP_MAX_JSON, &body, &len);
    if (st != 200) {
        if (st < 0) errf(err, errcap, "network error (%d)", st);
        else errf(err, errcap, "GitHub answered HTTP %d", st);
        return -1;
    }
    int n = up_parse_releases(body, len, &fresh);
    free(body);
    if (n < 0) { errf(err, errcap, "malformed release list"); return -1; }
    /* compat.json of the newest candidates (older ones can never be selected) */
    for (int fetched = 0; fetched < UP_MAX_COMPAT_FETCH;) {
        int pick = -1;
        for (int i = 0; i < fresh.n; i++)
            if (fresh.r[i].compat == 0 && up_release_candidate(&fresh.r[i], l) &&
                (pick < 0 || up_version_cmp(fresh.r[i].version, fresh.r[pick].version) > 0))
                pick = i;
        if (pick < 0) break;
        up_release *r = &fresh.r[pick];
        if (!r->compat_url[0]) { compat_fail(r, "no compat.json asset"); continue; }
        fetched++;
        st = http_get(get, ctx, r->compat_url, UP_MAX_JSON, &body, &len);
        if (st != 200) { compat_fail(r, st < 0 ? "compat.json: network error" : "compat.json: HTTP error"); continue; }
        char *sbody = NULL;
        size_t slen = 0;
        if (r->sig_url[0]) {
            int ss = http_get(get, ctx, r->sig_url, 4096, &sbody, &slen);
            if (ss != 200) {   /* listed but not readable: do not fall back to "unsigned" */
                free(body);
                compat_fail(r, "compat.json.sig: download failed");
                continue;
            }
        }
        up_apply_compat(r, body, len, sbody, slen);
        free(body);
        free(sbody);
        if (r->compat == 1 && !r->sig && !r->setup_sha[0] && r->sums_url[0]) {
            st = http_get(get, ctx, r->sums_url, UP_MAX_JSON, &body, &len);
            if (st == 200) up_sums_find(body, len, r->setup_name, r->setup_sha);
            free(body);
        }
    }
    for (int i = 0; i < fresh.n; i++)
        if (fresh.r[i].compat == 0) compat_fail(&fresh.r[i], "not checked");
    fresh.checked_at = fresh.attempt_at = now;
    up_local_key(l, fresh.key, sizeof fresh.key);
    memcpy(cat, &fresh, sizeof fresh);
    return 0;
}

int up_download_setup(up_http_fn get, void *ctx, const up_release *r, char **data, size_t *len, char *err,
                      size_t errcap)
{
    *data = NULL;
    *len = 0;
    if (!r->setup_url[0] || !up_hex_valid(r->setup_sha)) { errf(err, errcap, "no verifiable setup in %s", r->tag); return -1; }
    char *body;
    size_t n;
    int st = http_get(get, ctx, r->setup_url, UP_MAX_SETUP, &body, &n);
    if (st != 200) { errf(err, errcap, "download failed (%d)", st); return -1; }
    if (r->setup_size > 0 && (long long)n != r->setup_size) {
        errf(err, errcap, "size mismatch (%zu bytes, release says %lld)", n, r->setup_size);
        free(body);
        return -1;
    }
    char hex[65];
    up_sha256_hex(body, n, hex);
    if (!up_hex_eq(hex, r->setup_sha)) {
        errf(err, errcap, "SHA-256 mismatch (got %.16s..., expected %.16s...)", hex, r->setup_sha);
        free(body);
        return -1;
    }
    *data = body;
    *len = n;
    return 0;
}

/* ================================================================ SHA-256 (FIPS 180-4) */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(up_sha256 *s, const uint8_t *b)
{
    uint32_t w[64], a, bb, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; bb = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += bb; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void up_sha256_init(up_sha256 *s)
{
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof iv);
    s->n = 0;
    s->k = 0;
}

void up_sha256_update(up_sha256 *s, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    s->n += n;
    while (n) {
        size_t t = 64 - s->k < n ? 64 - s->k : n;
        memcpy(s->buf + s->k, p, t);
        s->k += t; p += t; n -= t;
        if (s->k == 64) { sha_block(s, s->buf); s->k = 0; }
    }
}

void up_sha256_final(up_sha256 *s, uint8_t out[32])
{
    uint64_t bits = s->n * 8;
    uint8_t pad = 0x80, z = 0, len[8];
    up_sha256_update(s, &pad, 1);
    while (s->k != 56) up_sha256_update(s, &z, 1);
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    up_sha256_update(s, len, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24); out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8); out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

static void to_hex(const uint8_t d[32], char out[65])
{
    static const char x[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = x[d[i] >> 4]; out[2 * i + 1] = x[d[i] & 15]; }
    out[64] = 0;
}

void up_sha256_hex(const void *data, size_t n, char out[65])
{
    up_sha256 s;
    uint8_t d[32];
    up_sha256_init(&s);
    up_sha256_update(&s, data, n);
    up_sha256_final(&s, d);
    to_hex(d, out);
}

int up_sha256_file(const char *path, char out[65])
{
    out[0] = 0;
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) return -1;
    up_sha256 s;
    uint8_t d[32];
    char *buf = (char *)malloc(65536);   /* heap: may run on the updater thread and at process exit */
    if (!buf) { fclose(f); return -1; }
    up_sha256_init(&s);
    size_t k;
    while ((k = fread(buf, 1, 65536, f)) > 0) up_sha256_update(&s, buf, k);
    int bad = ferror(f);
    fclose(f);
    free(buf);
    if (bad) return -1;
    up_sha256_final(&s, d);
    to_hex(d, out);
    return 0;
}

int up_hex_valid(const char *h)
{
    if (!h) return 0;
    for (int i = 0; i < 64; i++)
        if (!isxdigit((unsigned char)h[i])) return 0;
    return h[64] == 0;
}

int up_hex_eq(const char *a, const char *b)
{
    if (!up_hex_valid(a) || !up_hex_valid(b)) return 0;
    for (int i = 0; i < 64; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

/* ================================================================ applying */

int up_game_root(const char *game_dir, char *out, size_t cap)
{
    if (!game_dir || !cap) return -1;
    size_t n = strlen(game_dir);
    while (n && (game_dir[n - 1] == '\\' || game_dir[n - 1] == '/')) n--;
    if (n >= 8 && (n == 8 || game_dir[n - 9] == '\\' || game_dir[n - 9] == '/') && !_strnicmp(game_dir + n - 8, "binaries", 8)) {
        n -= 8;
        while (n && (game_dir[n - 1] == '\\' || game_dir[n - 1] == '/')) n--;
    }
    if (!n || n >= cap) return -1;
    if (n == 2 && game_dir[1] == ':') return -1;   /* a bare drive is never the game folder */
    memcpy(out, game_dir, n);
    out[n] = 0;
    return 0;
}

int up_append_arg(char *out, size_t cap, const char *arg)
{
    size_t k = strlen(out);
    int need_q = !*arg || strpbrk(arg, " \t\"") != NULL;
    char tmp[1200];
    size_t t = 0;
#define PUT(c) do { if (t + 1 >= sizeof tmp) return -1; tmp[t++] = (c); } while (0)
    if (k) PUT(' ');
    if (!need_q) {
        for (const char *p = arg; *p; p++) PUT(*p);
    } else {
        PUT('"');
        for (const char *p = arg;; p++) {
            size_t bs = 0;
            while (*p == '\\') { bs++; p++; }
            if (!*p) { for (size_t i = 0; i < bs * 2; i++) PUT('\\'); break; }
            if (*p == '"') { for (size_t i = 0; i < bs * 2 + 1; i++) PUT('\\'); PUT('"'); }
            else { for (size_t i = 0; i < bs; i++) PUT('\\'); PUT(*p); }
        }
        PUT('"');
    }
#undef PUT
    if (k + t + 1 > cap) return -1;
    memcpy(out + k, tmp, t);
    out[k + t] = 0;
    return 0;
}

int up_build_cmdline(char *out, size_t cap, const char *setup_exe, const char *game_root, unsigned long pid)
{
    char p[24];
    if (!out || !cap || !setup_exe || !*setup_exe || !game_root || !*game_root) return -1;
    out[0] = 0;
    snprintf(p, sizeof p, "%lu", pid);
    const char *args[] = {setup_exe, "--update", "--silent", "--game-dir", game_root, "--wait-pid", p};
    for (size_t i = 0; i < sizeof args / sizeof *args; i++)
        if (up_append_arg(out, cap, args[i])) { out[0] = 0; return -1; }
    return 0;
}

int up_install_result(const char *text, size_t n, char *detail, size_t cap)
{
    const char *tag = "update-result:", *hit = NULL;
    size_t tl = strlen(tag);
    if (cap) detail[0] = 0;
    if (!text) return -1;
    for (const char *p = text; p < text + n;) {
        const char *le = memchr(p, '\n', (size_t)(text + n - p));
        if (!le) le = text + n;
        if ((size_t)(le - p) >= tl && !memcmp(p, tag, tl)) hit = p;
        p = le + 1;
    }
    if (!hit) return -1;
    const char *q = hit + tl, *le = memchr(hit, '\n', (size_t)(text + n - hit));
    if (!le) le = text + n;
    while (q < le && *q == ' ') q++;
    while (le > q && (le[-1] == '\r' || le[-1] == ' ')) le--;
    int ok = (le - q) >= 2 && !memcmp(q, "ok", 2) && (le - q == 2 || q[2] == ' ');
    const char *d = ok ? q + 2 : q;
    while (d < le && *d == ' ') d++;
    if (ok && le - d > 8 && !memcmp(d, "version=", 8)) d += 8;
    size_t k = (size_t)(le - d) < cap ? (size_t)(le - d) : cap ? cap - 1 : 0;
    if (cap) { memcpy(detail, d, k); detail[k] = 0; }
    return ok;
}
