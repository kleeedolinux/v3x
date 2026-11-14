/* Fuzz harness: sdk/v3x_safe.h. Checks the safety contract on arbitrary input:
 *   path:     an accepted rel never contains a ".." / "." component, ':', a leading separator or a device
 *             name, and join output = base + sep + rel (normalised), always NUL-terminated, never longer
 *             than cap-1;
 *   str:      every append keeps len < cap, buf[len] == 0, strlen(buf) == len, content = a prefix of what
 *             was appended, and a truncated result never ends inside a UTF-8 sequence that was complete;
 *   span:     a byte-code program of reads over the input never reads outside it, pos <= len always;
 *   sanitize: output has no control bytes, no C1 / bidi controls, is a NUL-terminated string < cap;
 *   audit #2 (N6, N10, 12):
 *   str/span_corrupt: builders / readers with overwritten fields (len >= cap, NULL buf, pos > len,
 *             NULL p) are refused before any arithmetic: nothing is written, nothing is read;
 *   pathw:    UTF-16 and CP_ACP rel validation + joins (Windows) obey the same rules as the UTF-8 path;
 *   scan:     bounded rel32 / rip / scan_near / xrefs over an arbitrary byte section never read outside
 *             it (the section ends at a guard page) and only return targets inside the given range;
 *             pattern_compile never accepts a non-hex byte token;
 *   ini_range: range getters return a value in [lo, hi] or the default, NULL-safe lookups;
 *   hook_arg: V3X_ARG / V3X_ARGF with a run-time index never read or write outside V3X_HookCall. */
#include <ctype.h>
#include <math.h>
#include "v3x_fuzz.h"
#ifndef _WIN32
#define V3X_SAFE_NO_WINDOWS 1   /* Linux (libFuzzer): the pure part; the Windows targets below are skipped */
#endif
#define V3X_SAFE_STRICT 1       /* every result is V3X_MUST_USE here */
#include "v3x_safe.h"
#ifdef _WIN32
#include "v3x_scan.h"
#include "v3x_ini.h"
#include "v3x_hook.h"
#endif

static int has_component(const char *p, const char *c)
{
    size_t cn = strlen(c);
    for (const char *s = p;;) {
        const char *e = s;
        while (*e && *e != '/' && *e != '\\') e++;
        if ((size_t)(e - s) == cn && !strncmp(s, c, cn)) return 1;
        if (!*e) return 0;
        s = e + 1;
    }
}

static void fuzz_path(const uint8_t *d, size_t n)
{
    const char *rel = v3x_fuzz_cstr(d, n);
    V3xResult r = v3x_path_rel_ok(rel);
    if (r == V3X_R_OK) {
        V3X_FUZZ_ASSERT(rel[0] && rel[0] != '/' && rel[0] != '\\');
        V3X_FUZZ_ASSERT(!strchr(rel, ':') && !has_component(rel, "..") && !has_component(rel, "."));
        V3X_FUZZ_ASSERT(strlen(rel) <= V3X_PATH_REL_MAX);
        size_t L = strlen(rel);
        V3X_FUZZ_ASSERT(rel[L - 1] != '.' && rel[L - 1] != ' ' && rel[L - 1] != '/' && rel[L - 1] != '\\');
    }
    (void)v3x_path_rel_ok_ex(rel, V3X_PATH_SINGLE);
    char out[300];
    size_t cap = n ? (size_t)(d[0] % sizeof out) : 0;   /* also tiny capacities */
    memset(out, 'Z', sizeof out);
    V3xResult j = v3x_path_join_checked(cap ? out : NULL, cap, "C:\\game\\v3x", rel, 0);
    if (cap) {
        size_t L = strnlen(out, cap);
        V3X_FUZZ_ASSERT(L < cap);
        if (j == V3X_R_OK) V3X_FUZZ_ASSERT(!strncmp(out, "C:\\game\\v3x\\", 12) && !strchr(out + 12, '/'));
        else V3X_FUZZ_ASSERT(out[0] == 0);
        for (size_t i = cap; i < sizeof out; i++) V3X_FUZZ_ASSERT(out[i] == 'Z');
    }
}
V3X_FUZZ_TARGET(safe_path, fuzz_path)
V3X_FUZZ_SEED(safe_path, "dir/sub/file.ini")
V3X_FUZZ_SEED(safe_path, "..\\version.dll")
V3X_FUZZ_SEED(safe_path, "plugins/con.txt")
V3X_FUZZ_SEED(safe_path, "\xC3\xA9t\xC3\xA9/COM\xC2\xB9")

/* 1 if s[0..n) is valid UTF-8 (shortest form not checked: enough for the cut property) */
static int utf8_valid(const char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        size_t k = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!k || i + k > n) return 0;
        for (size_t j = 1; j < k; j++) if (((unsigned char)s[i + j] & 0xC0) != 0x80) return 0;
        i += k;
    }
    return 1;
}

/* byte 0 = cap, then chunks: [op][len][bytes...] ; op&3: 0 append, 1 appendn, 2 appendc, 3 appendf("%s|%.*s") */
static void fuzz_str(const uint8_t *d, size_t n)
{
    if (!n) return;
    size_t cap = d[0];
    char store[256 + 1];
    memset(store, 'Z', sizeof store);
    V3xStr s;
    v3x_str_init(&s, cap ? store : NULL, cap);
    /* one v3x_fuzz_cstr copy per chunk: the drivers allow 16 copies per input and abort beyond
     * (test/fuzz/fuzz_main.c MAX_GUARDED, which also spends one on the input itself;
     * .github/scripts/libfuzzer_adapter.c MAX_COPIES), so at most 15 chunks */
    for (size_t i = 1, chunks = 0; i + 1 < n && chunks < 15; chunks++) {
        uint8_t op = d[i], ln = d[i + 1];
        i += 2;
        size_t k = ln < n - i ? ln : n - i;
        const char *chunk = v3x_fuzz_cstr(d + i, k);   /* may contain NULs: append stops there */
        i += k;
        size_t clen = strlen(chunk);
        size_t before = s.len;
        int was_trunc = s.truncated;
        switch (op & 3) {
        case 0: (void)v3x_str_append(&s, chunk); break;
        case 1: (void)v3x_str_appendn(&s, chunk, clen); break;
        case 2: (void)v3x_str_appendc(&s, chunk[0] ? chunk[0] : 'x'); clen = 1; break;
        default: (void)v3x_str_appendf(&s, "%s|%.*s", chunk, (int)(op >> 2), chunk); clen = 0; break;
        }
        if (cap) {
            V3X_FUZZ_ASSERT(s.len < cap && s.buf[s.len] == 0 && strlen(s.buf) == s.len);
            V3X_FUZZ_ASSERT(s.len >= before);
            if (was_trunc) V3X_FUZZ_ASSERT(s.len == before);
            if ((op & 3) < 2 && !was_trunc) {
                /* the new bytes are a prefix of the chunk */
                V3X_FUZZ_ASSERT(!memcmp(s.buf + before, chunk, s.len - before));
                if (!s.truncated) V3X_FUZZ_ASSERT(s.len - before == clen);
                else if (utf8_valid(chunk, clen)) V3X_FUZZ_ASSERT(utf8_valid(s.buf + before, s.len - before));   /* no half codepoint */
            }
        }
    }
    for (size_t i = cap; i < sizeof store; i++) V3X_FUZZ_ASSERT(store[i] == 'Z');
}
V3X_FUZZ_TARGET(safe_str, fuzz_str)
V3X_FUZZ_SEED(safe_str, "\x10\x00\x05hello\x01\x04\xE2\x82\xAC!\x03\x02%n\x02\x01x")
V3X_FUZZ_SEED(safe_str, "\x06\x00\x08\xF0\x9F\x98\x80\xF0\x9F\x98\x80")

/* program: byte i = op, operands from the following bytes; reads go over the WHOLE input as a span */
static void fuzz_span(const uint8_t *d, size_t n)
{
    V3xSpan r;
    v3x_span_init(&r, d, n);
    uint8_t b; uint16_t h; uint32_t w; uint64_t q;
    for (size_t pc = 0; pc < n && pc < 512; pc++) {
        size_t before = r.pos;
        switch (d[pc] % 8) {
        case 0: (void)v3x_span_u8(&r, &b); break;
        case 1: (void)v3x_span_u16le(&r, &h); break;
        case 2: (void)v3x_span_u32le(&r, &w); break;
        case 3: (void)v3x_span_u64le(&r, &q); break;
        case 4: {
            const uint8_t *p = v3x_span_take(&r, pc + 1 < n ? d[pc + 1] : 0);
            if (p) { volatile uint8_t sink = 0; for (size_t i = 0; i < (pc + 1 < n ? d[pc + 1] : 0); i++) sink ^= p[i]; (void)sink; }
            break;
        }
        case 5: (void)v3x_span_skip(&r, pc + 1 < n ? (size_t)d[pc + 1] * 3 : SIZE_MAX); break;
        case 6: {
            V3xSpan sub = v3x_span_sub(&r, pc + 1 < n ? d[pc + 1] % 32 : 1);
            while (v3x_span_u8(&sub, &b) == V3X_R_OK) {}
            V3X_FUZZ_ASSERT(sub.pos <= sub.len);
            break;
        }
        default: (void)v3x_span_remaining(&r); break;
        }
        V3X_FUZZ_ASSERT(r.pos <= r.len && r.len == n && r.pos >= before);
        if (r.err) { V3X_FUZZ_ASSERT(v3x_span_remaining(&r) == 0 && v3x_span_take(&r, 0) == NULL); v3x_span_init(&r, d, n); }
    }
}
V3X_FUZZ_TARGET(safe_span, fuzz_span)
V3X_FUZZ_SEED(safe_span, "\x00\x01\x02\x03\x04\x10\x05\x02\x06\x08\x07")

static void fuzz_sanitize(const uint8_t *d, size_t n)
{
    const char *src = v3x_fuzz_cstr(d, n);
    char out[64];
    size_t cap = n ? 1 + d[0] % sizeof out : sizeof out;
    size_t L = v3x_sanitize_line(out, cap, src);
    V3X_FUZZ_ASSERT(L < cap && strlen(out) == L);
    for (size_t i = 0; i < L; i++) {
        unsigned char c = (unsigned char)out[i];
        V3X_FUZZ_ASSERT(c >= 0x20 && c != 0x7F);
        if (c == 0xC2 && i + 1 < L) V3X_FUZZ_ASSERT(!((unsigned char)out[i + 1] >= 0x80 && (unsigned char)out[i + 1] <= 0x9F));
        if (c == 0xE2 && i + 2 < L) {
            unsigned char c1 = (unsigned char)out[i + 1], c2 = (unsigned char)out[i + 2];
            V3X_FUZZ_ASSERT(!((c1 == 0x80 && c2 >= 0xAA && c2 <= 0xAE) || (c1 == 0x81 && c2 >= 0xA6 && c2 <= 0xA9)));
        }
    }
    (void)v3x_sanitize_linen(out, sizeof out, (const char *)d, n);   /* not NUL-terminated source, bounded by n */
}
V3X_FUZZ_TARGET(safe_sanitize, fuzz_sanitize)
V3X_FUZZ_SEED(safe_sanitize, "ok\nFAKE [12:00][core] x\r\t\x1b[31m a\xC2\x85 b\xE2\x80\xAE c\xE2\x81\xA6 d\xC3\xA9")

static void fuzz_math(const uint8_t *d, size_t n)
{
    if (n < 16) return;
    uint64_t a, b;
    memcpy(&a, d, 8); memcpy(&b, d + 8, 8);
    size_t o;
    if (v3x_add_ok((size_t)a, (size_t)b, &o)) V3X_FUZZ_ASSERT(o == (size_t)a + (size_t)b && o >= (size_t)a);
    else V3X_FUZZ_ASSERT(o == 0 && (size_t)a > SIZE_MAX - (size_t)b);
    if (v3x_mul_ok((size_t)a, (size_t)b, &o)) V3X_FUZZ_ASSERT(!a || o / (size_t)a == (size_t)b);
    else V3X_FUZZ_ASSERT(o == 0 && a && (size_t)b > SIZE_MAX / (size_t)a);
    int64_t i;
    if (v3x_add_ok_i64((int64_t)a, (int64_t)b, &i)) V3X_FUZZ_ASSERT((uint64_t)i == a + b);
    if (v3x_mul_ok((size_t)a, (size_t)b, &o) && o < (1u << 20)) {   /* small: really allocate */
        void *p = v3x_alloc_array((size_t)a, (size_t)b);
        V3X_FUZZ_ASSERT(p);
        free(p);
    } else if (!v3x_mul_ok((size_t)a, (size_t)b, &o)) {
        V3X_FUZZ_ASSERT(v3x_alloc_array((size_t)a, (size_t)b) == NULL);
    }
}
V3X_FUZZ_TARGET(safe_math, fuzz_math)
V3X_FUZZ_SEED(safe_math, "\xff\xff\xff\xff\xff\xff\xff\xff\x02\x00\x00\x00\x00\x00\x00\x00")
V3X_FUZZ_SEED(safe_math, "\x10\x00\x00\x00\x00\x00\x00\x00\x08\x00\x00\x00\x00\x00\x00\x00")

/* ---- audit #2: corrupted builder / reader fields (N10 invariants) */
/* d[0] = cap (0..63), d[1] = len (0..127), d[2] bit0 = NULL buf, bit1 = truncated; rest = text */
static void fuzz_str_corrupt(const uint8_t *d, size_t n)
{
    if (n < 3) return;
    char store[64 + 16];
    memset(store, 'Z', sizeof store);
    V3xStr s;
    size_t cap = d[0] % 64;
    s.buf = (d[2] & 1) ? NULL : store;
    s.cap = cap;
    s.len = d[1] % 128;
    s.truncated = (d[2] & 2) ? 1 : 0;
    int valid = s.buf && s.cap && s.len < s.cap;
    const char *t = v3x_fuzz_cstr(d + 3, n - 3);
    V3xResult r = v3x_str_append(&s, t);
    if (!valid) {
        V3X_FUZZ_ASSERT(r == V3X_R_EARG && v3x_str_truncated(&s) && !strcmp(v3x_str_cstr(&s), ""));
        for (size_t i = 0; i < sizeof store; i++) V3X_FUZZ_ASSERT(store[i] == 'Z');   /* nothing written */
    } else if (d[2] & 2) {   /* already truncated: a no-op */
        V3X_FUZZ_ASSERT(r == V3X_R_ETRUNC && s.len == d[1] % 128u);
        for (size_t i = 0; i < sizeof store; i++) V3X_FUZZ_ASSERT(store[i] == 'Z');
    } else {
        V3X_FUZZ_ASSERT(s.len < cap && store[s.len] == 0);
        for (size_t i = cap; i < sizeof store; i++) V3X_FUZZ_ASSERT(store[i] == 'Z');
    }
    r = v3x_str_appendf(&s, "%s", t);
    (void)r;
    for (size_t i = cap; i < sizeof store; i++) V3X_FUZZ_ASSERT(store[i] == 'Z');
}
V3X_FUZZ_TARGET(safe_str_corrupt, fuzz_str_corrupt)
V3X_FUZZ_SEED(safe_str_corrupt, "\x08\x20\x00hello")
V3X_FUZZ_SEED(safe_str_corrupt, "\x08\x03\x00hello world")
V3X_FUZZ_SEED(safe_str_corrupt, "\x08\x00\x01x")

/* the input is the span's data (ends at a guard page); d[0] picks the corruption */
static void fuzz_span_corrupt(const uint8_t *d, size_t n)
{
    if (!n) return;
    V3xSpan r;
    v3x_span_init(&r, d, n);
    switch (d[0] % 4) {
    case 0: r.pos = n + 1 + (n > 1 ? d[1] : 0); break;   /* pos > len */
    case 1: r.p = NULL; break;                          /* len > 0, no data */
    case 2: r.pos = n; break;                           /* at the end: valid */
    default: break;
    }
    int valid = r.pos <= r.len && (r.p || !r.len);
    uint64_t q = 1;
    V3xResult rr = v3x_span_u64le(&r, &q);
    if (!valid) V3X_FUZZ_ASSERT(rr == V3X_R_EBOUNDS && q == 0 && v3x_span_failed(&r) && v3x_span_take(&r, 0) == NULL);
    const uint8_t *t = v3x_span_take(&r, n > 2 ? d[2] : 1);
    if (t) V3X_FUZZ_ASSERT(t >= d && t <= d + n);
    V3X_FUZZ_ASSERT(v3x_span_remaining(&r) <= n);
}
V3X_FUZZ_TARGET(safe_span_corrupt, fuzz_span_corrupt)
V3X_FUZZ_SEED(safe_span_corrupt, "\x00\x05\x02" "abcdefgh")
V3X_FUZZ_SEED(safe_span_corrupt, "\x01\x00\x01" "abcdefgh")
V3X_FUZZ_SEED(safe_span_corrupt, "\x03\x00\x04" "abcdefgh")

#ifdef _WIN32
/* ---- N6: UTF-16 and CP_ACP paths */
static int has_component_w(const wchar_t *p, const wchar_t *c)
{
    size_t cn = wcslen(c);
    for (const wchar_t *s = p;;) {
        const wchar_t *e = s;
        while (*e && *e != L'/' && *e != L'\\') e++;
        if ((size_t)(e - s) == cn && !wcsncmp(s, c, cn)) return 1;
        if (!*e) return 0;
        s = e + 1;
    }
}

/* input = UTF-16LE rel (d[0] also picks the output capacity) */
static void fuzz_pathw(const uint8_t *d, size_t n)
{
    static wchar_t base[MAX_PATH];
    if (!base[0]) {   /* a base that does not exist: the reparse walk stops at its first component */
        wchar_t t[MAX_PATH];
        GetTempPathW(MAX_PATH, t);
        _snwprintf(base, MAX_PATH, L"%lsv3x_fuzz_none_%lu", t, (unsigned long)GetCurrentProcessId());
        base[MAX_PATH - 1] = 0;
    }
    size_t wn = n / 2 > 600 ? 600 : n / 2;
    wchar_t rel[601];
    memcpy(rel, d, wn * 2);
    rel[wn] = 0;
    V3xResult r = v3x_path_rel_ok_exW(rel, 0);
    if (r == V3X_R_OK) {
        V3X_FUZZ_ASSERT(rel[0] && rel[0] != L'/' && rel[0] != L'\\' && !wcschr(rel, L':'));
        V3X_FUZZ_ASSERT(!has_component_w(rel, L"..") && !has_component_w(rel, L"."));
        size_t L = wcslen(rel);
        V3X_FUZZ_ASSERT(L <= V3X_PATH_REL_MAX && rel[L - 1] != L'.' && rel[L - 1] != L' ' && rel[L - 1] != L'/' && rel[L - 1] != L'\\');
        for (size_t i = 0; i < L; i++) V3X_FUZZ_ASSERT(rel[i] >= 0x20 && rel[i] != 0x7F);
    }
    wchar_t out[700];
    size_t cap = n ? 1 + (size_t)d[0] * 3 % (sizeof out / sizeof out[0]) : 0;
    for (size_t i = 0; i < sizeof out / sizeof out[0]; i++) out[i] = L'Z';
    V3xResult j = v3x_path_join_checkedW(cap ? out : NULL, cap, base, rel, (n & 1) ? V3X_PATH_CHECK_REPARSE : 0);
    if (cap) {
        size_t L = wcsnlen(out, cap);
        V3X_FUZZ_ASSERT(L < cap);
        if (j == V3X_R_OK) {
            size_t bl = wcslen(base);
            V3X_FUZZ_ASSERT(r == V3X_R_OK && !wcsncmp(out, base, bl) && out[bl] == L'\\' && !wcschr(out + bl, L'/'));
        } else V3X_FUZZ_ASSERT(out[0] == 0);
        for (size_t i = cap; i < sizeof out / sizeof out[0]; i++) V3X_FUZZ_ASSERT(out[i] == L'Z');
    }
    /* the same bytes as CP_ACP and UTF-8 text */
    const char *a = v3x_fuzz_cstr(d, n);
    V3xResult ra = v3x_path_rel_ok_cp(CP_ACP, a, 0);
    char ao[300];
    V3xResult ja = v3x_path_join_checked(ao, sizeof ao, "C:\\game\\v3x", a, 0);
    V3X_FUZZ_ASSERT(strnlen(ao, sizeof ao) < sizeof ao);
    if (ja == V3X_R_OK) V3X_FUZZ_ASSERT(ra == V3X_R_OK && !strncmp(ao, "C:\\game\\v3x\\", 12));
    wchar_t w[16];
    V3xResult rw = v3x_widen(CP_UTF8, a, SIZE_MAX, w, sizeof w / sizeof w[0]);
    V3X_FUZZ_ASSERT(wcsnlen(w, 16) < 16);
    if (rw != V3X_R_OK) V3X_FUZZ_ASSERT(w[0] == 0);
}
V3X_FUZZ_TARGET(safe_pathw, fuzz_pathw)
V3X_FUZZ_SEED(safe_pathw, "d\0i\0r\0/\0\xE9\0t\0\xE9\0.\0t\0x\0t\0")
V3X_FUZZ_SEED(safe_pathw, ".\0.\0\\\0x\0")
V3X_FUZZ_SEED(safe_pathw, "a\0\x00\xD8" "b\0")
V3X_FUZZ_SEED(safe_pathw, "C\0O\0N\0.\0t\0x\0t\0")

/* ---- 12: bounded decoders over an arbitrary section */
static void fuzz_scan(const uint8_t *d, size_t n)
{
    if (n < 4) return;
    uint8_t *sec_b = (uint8_t *)v3x_fuzz_guarded(d, n);   /* the section ends at a guard page */
    V3xSection sec = {sec_b, n}, half = {sec_b, n / 2};
    for (size_t i = 0; i < n; i++) {
        void *t = v3x_rel32_target_in(&sec, sec_b + i, NULL);
        if (t) V3X_FUZZ_ASSERT((uint8_t *)t >= sec_b && (uint8_t *)t < sec_b + n);
        t = v3x_rel32_target_in(&half, sec_b + i, &sec);
        if (t) V3X_FUZZ_ASSERT(i < n / 2 && (uint8_t *)t < sec_b + n);
        t = v3x_rip_target_in(&sec, sec_b + i, d[i] % 8, d[(i + 1) % n] % 16, &half);
        if (t) V3X_FUZZ_ASSERT((uint8_t *)t >= sec_b && (uint8_t *)t < sec_b + n / 2);
    }
    (void)v3x_xrefs(&sec, sec_b, NULL, 0);
    (void)v3x_callers(&sec, sec_b + n / 2, NULL, 0);
    /* pattern from the input text */
    const char *pat = v3x_fuzz_cstr(d, n > 64 ? 64 : n);
    V3xPattern p;
    int pn = v3x_pattern_compile(pat, &p);
    V3X_FUZZ_ASSERT(pn >= 0 && pn <= 256);
    if (pn) {   /* every accepted token is ?/?? or two hex digits */
        for (const char *c = pat; *c;) {
            if (*c == ' ') { c++; continue; }
            if (*c == '?') { c++; if (*c == '?') c++; continue; }
            V3X_FUZZ_ASSERT(isxdigit((unsigned char)c[0]) && isxdigit((unsigned char)c[1]));
            c += 2;
        }
        void *hit = v3x_scan_near_in(&sec, sec_b + n / 3, (size_t)d[0] * 4, pat);
        if (hit) V3X_FUZZ_ASSERT((uint8_t *)hit >= sec_b + n / 3 && (uint8_t *)hit + pn <= sec_b + n);
        (void)v3x_scan_count(&sec, pat, NULL);
    }
}
V3X_FUZZ_TARGET(safe_scan, fuzz_scan)
V3X_FUZZ_SEED(safe_scan, "\xE8\x10\x00\x00\x00\x48\x8B\x05\xF0\xFF\xFF\xFF\x0F\x84\x01\x00\x00\x00")
V3X_FUZZ_SEED(safe_scan, "48 8B ?? ? 05 E8 ?? ?? ?? ??")
V3X_FUZZ_SEED(safe_scan, "+F -1 0x 4G")
V3X_FUZZ_SEED(safe_scan, "\xE8\xFB\xFF\xFF\xFF\xE9\x00\x00\x00\x80")

/* ---- 12: ini range getters (d[0..1] = lo/hi selectors, rest = text) */
static void fuzz_ini_range(const uint8_t *d, size_t n)
{
    if (n < 2) return;
    V3xIni ini;
    const char *text = v3x_fuzz_cstr(d + 2, n - 2);
    if (v3x_ini_parse(&ini, text) != 0) { v3x_ini_free(&ini); return; }
    long long lo = (int8_t)d[0], hi = lo + d[1];
    for (int i = 0; i < ini.n && i < 32; i++) {
        const char *sec = ini.kv[i].section, *key = ini.kv[i].key;
        long long v = v3x_ini_int_range(&ini, sec, key, 12345, lo, hi);
        V3X_FUZZ_ASSERT(v == 12345 || (v >= lo && v <= hi));
        double dv = v3x_ini_double_range(&ini, sec, key, -0.5, (double)lo, (double)hi);
        V3X_FUZZ_ASSERT(dv == -0.5 || (isfinite(dv) && dv >= (double)lo && dv <= (double)hi));
        char out[8];
        V3X_FUZZ_ASSERT(strlen(v3x_ini_str(&ini, sec, key, NULL, out, 1 + d[1] % sizeof out)) < sizeof out);
        (void)v3x_ini_bool(&ini, sec, key, 0);
        (void)v3x_ini_int(&ini, NULL, key, 0);
    }
    V3X_FUZZ_ASSERT(v3x_ini_get(&ini, "x", NULL) == NULL && v3x_ini_int(NULL, "x", "y", 9) == 9);
    v3x_ini_free(&ini);
}
V3X_FUZZ_TARGET(safe_ini_range, fuzz_ini_range)
V3X_FUZZ_SEED(safe_ini_range, "\x00\x0a[s]\na = 5\nb = 11\nc = -1\nd = 99999999999999999999\ne = 1e999\nf = nan\n")
V3X_FUZZ_SEED(safe_ini_range, "\xf6\x14k=0x7fffffffffffffff\nk2 = -0x8000000000000000\n")

/* ---- 12: checked V3X_ARG / V3X_ARGF with a run-time index (the struct ends at a guard page) */
static void fuzz_hook_arg(const uint8_t *d, size_t n)
{
    static const V3X_HookCall zero;
    V3X_HookCall *call = (V3X_HookCall *)v3x_fuzz_guarded(&zero, sizeof zero);
    for (size_t i = 0; i < n; i++) {
        int idx = (int8_t)d[i];
        call->args[i % 16] = (uint64_t)i;
        uint64_t v = V3X_ARG(call, idx, uint64_t);
        V3X_FUZZ_ASSERT(idx >= 0 && idx < 16 ? v == call->args[idx] : v == 0);
        V3X_ARGF(call, idx) = (double)i;
        if (idx >= 0 && idx < 4) V3X_FUZZ_ASSERT(call->xmm[idx].d == (double)i);
    }
}
V3X_FUZZ_TARGET(safe_hook_arg, fuzz_hook_arg)
V3X_FUZZ_SEED(safe_hook_arg, "\x00\x0f\x10\xff\x80\x7f\x03\x04")
#endif
