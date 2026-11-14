/* Fuzz harness: sdk/v3x_safe.h. Checks the safety contract on arbitrary input:
 *   path:     an accepted rel never contains a ".." / "." component, ':', a leading separator or a device
 *             name, and join output = base + sep + rel (normalised), always NUL-terminated, never longer
 *             than cap-1;
 *   str:      every append keeps len < cap, buf[len] == 0, strlen(buf) == len, content = a prefix of what
 *             was appended, and a truncated result never ends inside a UTF-8 sequence that was complete;
 *   span:     a byte-code program of reads over the input never reads outside it, pos <= len always;
 *   sanitize: output has no control bytes, no C1 / bidi controls, is a NUL-terminated string < cap. */
#include "v3x_fuzz.h"
#define V3X_SAFE_NO_WINDOWS 1   /* the pure part; the reparse check is covered by test/sdk_test.c */
#include "v3x_safe.h"

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
    for (size_t i = 1; i + 1 < n;) {
        uint8_t op = d[i], ln = d[i + 1];
        i += 2;
        size_t k = ln < n - i ? ln : n - i;
        const char *chunk = v3x_fuzz_cstr(d + i, k);   /* may contain NULs: append stops there */
        i += k;
        size_t clen = strlen(chunk);
        size_t before = s.len;
        int was_trunc = s.truncated;
        switch (op & 3) {
        case 0: v3x_str_append(&s, chunk); break;
        case 1: v3x_str_appendn(&s, chunk, clen); break;
        case 2: v3x_str_appendc(&s, chunk[0] ? chunk[0] : 'x'); clen = 1; break;
        default: v3x_str_appendf(&s, "%s|%.*s", chunk, (int)(op >> 2), chunk); clen = 0; break;
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
        case 0: v3x_span_u8(&r, &b); break;
        case 1: v3x_span_u16le(&r, &h); break;
        case 2: v3x_span_u32le(&r, &w); break;
        case 3: v3x_span_u64le(&r, &q); break;
        case 4: {
            const uint8_t *p = v3x_span_take(&r, pc + 1 < n ? d[pc + 1] : 0);
            if (p) { volatile uint8_t sink = 0; for (size_t i = 0; i < (pc + 1 < n ? d[pc + 1] : 0); i++) sink ^= p[i]; (void)sink; }
            break;
        }
        case 5: v3x_span_skip(&r, pc + 1 < n ? (size_t)d[pc + 1] * 3 : SIZE_MAX); break;
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
