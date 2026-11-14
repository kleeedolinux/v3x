/* core_parse.h - small text parsers of the loader (src/core.c), split out like core_ini.h so they are
 * unit-tested (test/api_trust_test.c) and fuzzed (test/fuzz/fuzz_core_parse.c) on their own. Plain C99,
 * no windows.h, no state. Every input is (pointer, length) and is never read past that length.
 *
 *   cp_official_has   signed official plugin list (binaries/v3x/plugins.json, written by
 *                     installer/make_dist.py): does it list (file name, sha256)?
 *   cp_allow_has      per-user allowlist %LOCALAPPDATA%\V3X\trusted_plugins.txt: "<sha256 hex> <file>" lines
 *   cp_caps_parse     manifest capabilities "hook, patch fs ..." -> bit mask (API v6)
 *   cp_pattern_parse  strict IDA pattern "48 8B ?? 05" -> bytes + mask (audit N9) */
#ifndef V3X_CORE_PARSE_H
#define V3X_CORE_PARSE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static int cp_lower_(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }
static int cp_hexv_(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = cp_lower_(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
static int cp_ieqn_(const char *a, size_t an, const char *b, size_t bn)
{
    if (an != bn) return 0;
    for (size_t i = 0; i < an; i++)
        if (cp_lower_((unsigned char)a[i]) != cp_lower_((unsigned char)b[i])) return 0;
    return 1;
}
/* 1 if s[0..64) are 64 hex digits equal (case-insensitive) to hex64 (64 lowercase hex digits) */
static int cp_hex64_eq_(const char *s, size_t n, const char *hex64)
{
    if (n != 64) return 0;
    for (size_t i = 0; i < 64; i++)
        if (cp_hexv_((unsigned char)s[i]) < 0 || cp_lower_((unsigned char)s[i]) != hex64[i]) return 0;
    return 1;
}

/* ------------------------------------------------------------------ plugins.json (signed)
 * {"schema":1,"v3x_version":"..","plugins":[{"name":"hello","file":"plugins/hello.dll","sha256":"<64 hex>",
 *  "version":".."}, ...]}
 * The signature (plugins.json.sig, ADR 0001 format) is checked by the caller BEFORE this runs, so the
 * text is the release's own; the parser is still bounded and strict: objects of the "plugins" array are
 * scanned for "file" and "sha256" string values (no escapes allowed in them). A DLL is listed when the
 * base name of "file" equals `file` (case-insensitive) and sha256 matches. */
static const char *cp_json_str_(const char *p, const char *e, const char **v, size_t *vn)
{
    /* p at '"': the string value without escapes; returns the position after the closing quote or NULL */
    if (p >= e || *p != '"') return NULL;
    const char *s = ++p;
    while (p < e && *p != '"') {
        if (*p == '\\' || (unsigned char)*p < 0x20) return NULL;
        p++;
    }
    if (p >= e) return NULL;
    *v = s; *vn = (size_t)(p - s);
    return p + 1;
}

static int cp_official_has(const char *t, size_t n, const char *file, const char *sha_hex)
{
    const char *e = t + n, *p = t;
    size_t fl = strlen(file);
    if (!t) return 0;
    /* find "plugins" key, then '[' */
    for (;;) {
        const char *q = p;
        while (q < e && *q != '"') q++;
        if (q >= e) return 0;
        const char *v; size_t vn;
        const char *after = cp_json_str_(q, e, &v, &vn);
        if (!after) return 0;
        p = after;
        if (vn == 7 && !memcmp(v, "plugins", 7)) break;
    }
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')) p++;
    if (p >= e || *p != '[') return 0;
    p++;
    for (int guard = 0; guard < 100000; guard++) {
        while (p < e && *p != '{' && *p != ']') p++;
        if (p >= e || *p == ']') return 0;
        const char *ob = ++p, *f = NULL, *h = NULL; size_t fn = 0, hn = 0;
        while (p < e && *p != '}') {
            if (*p == '{' || *p == '[') return 0;   /* nested values are not part of the format */
            if (*p != '"') { p++; continue; }
            const char *k; size_t kn;
            const char *a = cp_json_str_(p, e, &k, &kn);
            if (!a) return 0;
            p = a;
            while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
            if (p < e && *p == ':') {
                p++;
                while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
                if (p < e && *p == '"') {
                    const char *v; size_t vn;
                    const char *b = cp_json_str_(p, e, &v, &vn);
                    if (!b) return 0;
                    p = b;
                    if (kn == 4 && !memcmp(k, "file", 4)) { f = v; fn = vn; }
                    else if (kn == 6 && !memcmp(k, "sha256", 6)) { h = v; hn = vn; }
                }
            }
        }
        (void)ob;
        if (p >= e) return 0;
        p++;   /* '}' */
        if (f && h) {
            const char *b = f + fn;
            while (b > f && b[-1] != '/' && b[-1] != '\\') b--;
            if (cp_ieqn_(b, (size_t)(f + fn - b), file, fl) && cp_hex64_eq_(h, hn, sha_hex)) return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ trusted_plugins.txt (per user)
 * One plugin per line: "<sha256: 64 hex digits> <file name>" (the name is a note for people; only the
 * hash counts). '#' starts a comment line. Written by the console command `trust <plugin>`. */
static int cp_allow_has(const char *t, size_t n, const char *sha_hex)
{
    size_t i = 0;
    if (!t) return 0;
    while (i < n) {
        size_t s = i;
        while (i < n && t[i] != '\n') i++;
        size_t e = i;
        if (i < n) i++;
        while (s < e && (t[s] == ' ' || t[s] == '\t' || t[s] == '\r')) s++;
        if (s >= e || t[s] == '#') continue;
        size_t k = s;
        while (k < e && t[k] != ' ' && t[k] != '\t' && t[k] != '\r') k++;
        if (cp_hex64_eq_(t + s, k - s, sha_hex)) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ manifest capabilities (API v6) */
#define CP_CAP_HOOK 1u
#define CP_CAP_PATCH 2u
#define CP_CAP_FS 4u
#define CP_CAP_NET 8u
#define CP_CAP_SCRIPT 16u
#define CP_CAP_EXEC 32u
#define CP_CAP_ALL 63u

/* Words separated by space, tab, ',' or ';'. Returns the mask; *unknown = number of unknown words, the
 * first one copied to first_unknown (cut). */
static uint32_t cp_caps_parse(const char *s, size_t n, int *unknown, char *first_unknown, size_t fu_cap)
{
    static const char *names[] = {"hook", "patch", "fs", "net", "script", "exec"};
    uint32_t m = 0;
    size_t i = 0;
    if (unknown) *unknown = 0;
    if (fu_cap) first_unknown[0] = 0;
    while (s && i < n) {
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == ',' || s[i] == ';')) i++;
        size_t a = i;
        while (i < n && s[i] != ' ' && s[i] != '\t' && s[i] != ',' && s[i] != ';') i++;
        if (i == a) break;
        int hit = 0;
        for (unsigned k = 0; k < 6; k++)
            if (cp_ieqn_(s + a, i - a, names[k], strlen(names[k]))) { m |= 1u << k; hit = 1; }
        if (!hit) {
            if (unknown && (*unknown)++ == 0 && fu_cap) {
                size_t c = i - a < fu_cap - 1 ? i - a : fu_cap - 1;
                memcpy(first_unknown, s + a, c);
                first_unknown[c] = 0;
            }
        }
    }
    return m;
}

/* ------------------------------------------------------------------ IDA pattern (strict, audit N9)
 * Tokens separated by spaces/tabs: two hex digits, or "?" / "??" (wildcard). Returns the byte count
 * (1..cap), 0 for an empty pattern, -1 for a bad token, -2 if it has more than cap bytes. */
static int cp_pattern_parse(const char *s, size_t n, uint8_t *bytes, uint8_t *mask, int cap)
{
    int k = 0;
    size_t i = 0;
    while (i < n) {
        while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
        size_t a = i;
        while (i < n && s[i] != ' ' && s[i] != '\t') i++;
        size_t len = i - a;
        if (!len) break;
        if (k >= cap) return -2;
        if ((len == 1 && s[a] == '?') || (len == 2 && s[a] == '?' && s[a + 1] == '?')) {
            bytes[k] = 0; mask[k] = 0; k++;
            continue;
        }
        int h = len == 2 ? cp_hexv_((unsigned char)s[a]) : -1, l = len == 2 ? cp_hexv_((unsigned char)s[a + 1]) : -1;
        if (h < 0 || l < 0) return -1;
        bytes[k] = (uint8_t)(h * 16 + l); mask[k] = 1; k++;
    }
    return k;
}

#endif
