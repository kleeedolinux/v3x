/* v3x_safe.h - "Rust-like" safety helpers for V3X plugins and the core (header-only, C99 / C++).
 *
 * the security review "Rust-like safety plan", docs/adr/0004-sdk-safety.md. Everything here is
 * bounds-checked, never writes past the buffer it was given and reports failure instead of
 * silently wrapping. Compiles clean with -Wall -Wextra -Wconversion -Wshadow -Wformat=2 as C and C++.
 *
 *   char buf[256]; V3xStr s; v3x_str_init(&s, buf, sizeof buf);     // bounded string builder
 *   v3x_str_appendf(&s, "%s=%d ", key, val);                        // never overruns,
 *   if (v3x_str_truncated(&s)) ...                                  // sets truncated, stays NUL-terminated
 *
 *   V3xSpan r; v3x_span_init(&r, data, len);                        // checked reader over bytes
 *   uint32_t magic, n;
 *   if (v3x_span_u32le(&r, &magic) || v3x_span_u32le(&r, &n)) fail; // V3X_R_OK = 0
 *   const uint8_t *body = v3x_span_take(&r, n);                     // NULL if n > remaining
 *   if (v3x_span_failed(&r)) ...                                    // sticky: one failure = all later fail
 *
 *   size_t bytes; if (!v3x_mul_ok(count, sizeof(T), &bytes)) fail;  // checked arithmetic
 *   T *a = (T *)v3x_alloc_array(count, sizeof(T));                 // calloc with overflow check
 *
 *   if (v3x_path_join_checked(out, sizeof out, dir, rel_from_file, V3X_PATH_CHECK_REPARSE) != V3X_R_OK)
 *       refuse;                                                     // no "..", ":", "C:\", "\\?\", CON, ...
 *
 *   v3x_sanitize_line(clean, sizeof clean, mod_name);               // control chars -> '?' before logging
 *   if (!V3X_ASSERT_MAIN(api)) return;                              // logs once when called off the main thread
 *   v3x_atomic_store_int(&g_stop, 1);                               // cross-thread flags (acquire/release)
 *
 * Naming rules (ownership): *_new / *_free own memory (the caller frees with the matching _free);
 * *_view / const pointers into a caller buffer borrow (valid while the buffer is). Thread rules are
 * written next to each API entry in include/v3x.h; the annotation macros V3X_MAIN_THREAD /
 * V3X_ANY_THREAD below are documentation only.
 *
 * Define V3X_SAFE_NO_WINDOWS to use this header without <windows.h> (fuzzing on Linux); the
 * reparse-point check and V3X_ASSERT_MAIN then compile to no-ops. */
#ifndef V3X_SAFE_H
#define V3X_SAFE_H

#define V3X_SAFE_VERSION 2   /* 2: V3X_NN, invariant checks + accessors, W / CP_ACP paths, v3x_file_open_checked[W], V3X_R_EIO */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32) && !defined(V3X_SAFE_NO_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define V3X_SAFE_WIN_ 1
#else
#define V3X_SAFE_WIN_ 0
#endif
#include "v3x.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- attributes ------------------------------------------------------------------------------
 * V3X_NONNULL(i, ...)  __attribute__((nonnull)): the compiler may ASSUME the argument is non-NULL and
 *                      delete NULL checks inside the function. Use it only on functions that have no
 *                      NULL check (none in the SDK).
 * V3X_NN               per-parameter "must not be NULL" (clang _Nonnull): -Wnonnull at a call with a
 *                      literal NULL, no effect on code generation, so the SDK's runtime NULL checks
 *                      stay (a NULL that slips through still fails safely). Placed after the '*':
 *                      `const char *V3X_NN path`. Every SDK pointer parameter that must not be NULL has it.
 * V3X_MUST_USE         the result reports failure: ignoring it is a warning (-Wunused-result).
 * V3X_MUST_USE_STRICT  V3X_MUST_USE with -DV3X_SAFE_STRICT, else nothing: for APIs whose documented
 *                      use is "call many times, check one sticky flag" (V3xStr appends: s.truncated). */
#if defined(__GNUC__) || defined(__clang__)
#ifndef V3X_PRINTF   /* also defined in include/v3x.h */
#define V3X_PRINTF(fmt_idx, args_idx) __attribute__((format(printf, fmt_idx, args_idx)))
#endif
#ifndef V3X_NONNULL
#define V3X_NONNULL(...) __attribute__((nonnull(__VA_ARGS__)))
#endif
#define V3X_MUST_USE __attribute__((warn_unused_result))
#define V3X_SAFE_UNUSED_ __attribute__((unused))
#else
#ifndef V3X_PRINTF
#define V3X_PRINTF(fmt_idx, args_idx)
#endif
#ifndef V3X_NONNULL
#define V3X_NONNULL(...)
#endif
#define V3X_MUST_USE
#define V3X_SAFE_UNUSED_
#endif
#ifndef V3X_NN
#if defined(__clang__)
#define V3X_NN _Nonnull
#else
#define V3X_NN
#endif
#endif
#ifdef V3X_SAFE_STRICT
#define V3X_MUST_USE_STRICT V3X_MUST_USE
#else
#define V3X_MUST_USE_STRICT
#endif
/* V3X_NN_BEGIN / V3X_NN_END around a header that uses V3X_NN on some (not all) pointers: clang would
 * otherwise ask for a nullability qualifier on every other pointer (-Wnullability-completeness). */
#if defined(__clang__)
#define V3X_NN_BEGIN _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wnullability-completeness\"") \
    _Pragma("clang diagnostic ignored \"-Wnullability-extension\"")
#define V3X_NN_END _Pragma("clang diagnostic pop")
#else
#define V3X_NN_BEGIN
#define V3X_NN_END
#endif
V3X_NN_BEGIN
/* documentation-only thread annotations (see include/v3x.h for the rule of every API entry) */
#define V3X_MAIN_THREAD   /* call only on the game's main thread (on_frame / run_on_main / load) */
#define V3X_ANY_THREAD    /* safe from any thread */

/* ---- results --------------------------------------------------------------------------------- */
typedef enum V3xResult {
    V3X_R_OK = 0,
    V3X_R_EARG = -1,       /* NULL / zero-capacity / otherwise invalid argument */
    V3X_R_ETRUNC = -2,     /* output did not fit (what fits was written, NUL-terminated) */
    V3X_R_EBOUNDS = -3,    /* read past the end of a span */
    V3X_R_EOVERFLOW = -4,  /* integer overflow */
    V3X_R_EPATH = -5,      /* path rejected (traversal, absolute, device name, bad character, too long) */
    V3X_R_EREPARSE = -6,   /* a component of the path is a reparse point (symlink / junction) */
    V3X_R_ENOMEM = -7,
    V3X_R_EIO = -8         /* a file system call failed (GetLastError() set) */
} V3xResult;

/* ---- checked arithmetic (1 = ok and *out set; 0 = overflow, *out = 0) ------------------------- */
#if defined(__GNUC__) || defined(__clang__)
#define V3X_SAFE_ADD_(a, b, o) __builtin_add_overflow((a), (b), (o))
#define V3X_SAFE_MUL_(a, b, o) __builtin_mul_overflow((a), (b), (o))
#endif

static V3X_SAFE_UNUSED_ V3X_MUST_USE int v3x_add_ok(size_t a, size_t b, size_t *out)
{
    size_t r;
#ifdef V3X_SAFE_ADD_
    if (V3X_SAFE_ADD_(a, b, &r)) { if (out) *out = 0; return 0; }
#else
    if (a > SIZE_MAX - b) { if (out) *out = 0; return 0; }
    r = a + b;
#endif
    if (out) *out = r;
    return 1;
}

static V3X_SAFE_UNUSED_ V3X_MUST_USE int v3x_mul_ok(size_t a, size_t b, size_t *out)
{
    size_t r;
#ifdef V3X_SAFE_MUL_
    if (V3X_SAFE_MUL_(a, b, &r)) { if (out) *out = 0; return 0; }
#else
    if (b && a > SIZE_MAX / b) { if (out) *out = 0; return 0; }
    r = a * b;
#endif
    if (out) *out = r;
    return 1;
}

static V3X_SAFE_UNUSED_ V3X_MUST_USE int v3x_add_ok_u32(uint32_t a, uint32_t b, uint32_t *out)
{
    uint32_t r;
#ifdef V3X_SAFE_ADD_
    if (V3X_SAFE_ADD_(a, b, &r)) { if (out) *out = 0; return 0; }
#else
    if (a > UINT32_MAX - b) { if (out) *out = 0; return 0; }
    r = a + b;
#endif
    if (out) *out = r;
    return 1;
}

static V3X_SAFE_UNUSED_ V3X_MUST_USE int v3x_add_ok_i64(int64_t a, int64_t b, int64_t *out)
{
    int64_t r;
#ifdef V3X_SAFE_ADD_
    if (V3X_SAFE_ADD_(a, b, &r)) { if (out) *out = 0; return 0; }
#else
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) { if (out) *out = 0; return 0; }
    r = a + b;
#endif
    if (out) *out = r;
    return 1;
}

/* calloc(n, size) with an explicit overflow check. n * size == 0 still returns a unique 1-byte
 * block (free it). NULL on overflow or out of memory. Owned: free() it. */
static V3X_SAFE_UNUSED_ V3X_MUST_USE void *v3x_alloc_array(size_t n, size_t size)
{
    size_t bytes;
    if (!v3x_mul_ok(n, size, &bytes)) return NULL;
    return calloc(bytes ? bytes : 1, 1);
}

/* realloc to n * size bytes; on overflow / OOM returns NULL and p is untouched (still owned). */
static V3X_SAFE_UNUSED_ V3X_MUST_USE void *v3x_realloc_array(void *p, size_t n, size_t size)
{
    size_t bytes;
    if (!v3x_mul_ok(n, size, &bytes)) return NULL;
    return realloc(p, bytes ? bytes : 1);
}

/* ---- UTF-8 helpers ------------------------------------------------------------------------- */

/* Length (0..len) of s[0..len) after dropping an INCOMPLETE UTF-8 sequence at its end. Complete
 * sequences and invalid bytes are kept (this only prevents cutting a codepoint in half). Only the
 * bytes from `floor` on may be dropped. */
static V3X_SAFE_UNUSED_ size_t v3x_utf8_trim_tail(const char *V3X_NN s, size_t len, size_t floor)
{
    if (!s || len <= floor) return len;
    size_t i = len, back = 0;
    while (i > floor && back < 4) {
        unsigned char c = (unsigned char)s[i - 1];
        i--; back++;
        if ((c & 0xC0u) == 0x80u) continue;        /* continuation: keep looking for the lead */
        size_t need = (c & 0x80u) == 0 ? 1u : (c & 0xE0u) == 0xC0u ? 2u : (c & 0xF0u) == 0xE0u ? 3u
                    : (c & 0xF8u) == 0xF0u ? 4u : 1u;
        return back < need ? i : len;              /* lead with too few continuations: drop it */
    }
    return len;
}

/* ---- V3xStr: bounded string builder over a caller buffer -------------------------------------
 * Every append writes at most cap-1 bytes in total and keeps buf NUL-terminated. When something
 * does not fit, as much as fits is kept (cut at a UTF-8 codepoint boundary), `truncated` is set and
 * every later append is a no-op (so the content is always a clean prefix). Appends return
 * V3X_R_OK or V3X_R_ETRUNC (V3X_R_EARG for a builder without a buffer or a corrupted one).
 *
 * The fields are PRIVATE: set them only through v3x_str_init / v3x_str_clear, read them through
 * v3x_str_cstr / v3x_str_len / v3x_str_truncated (reading s.len / s.truncated directly still works;
 * the layout is frozen). Every function first checks the invariant (buf != NULL, cap > 0,
 * len < cap) before any arithmetic: a builder whose fields were overwritten is refused (EARG,
 * truncated set), never used to compute an out-of-bounds address. */
typedef struct V3xStr {
    char *buf;      /* private: borrowed caller buffer */
    size_t cap;     /* private: its size in bytes (incl. the NUL) */
    size_t len;     /* private (read: v3x_str_len): strlen(buf) */
    int truncated;  /* private (read: v3x_str_truncated): 1 after the first append that did not fit */
} V3xStr;

/* 1 if s satisfies the invariant (usable for appends) */
static V3X_SAFE_UNUSED_ int v3x_str_valid_(const V3xStr *s)
{
    return s && s->buf && s->cap && s->len < s->cap;
}

static V3X_SAFE_UNUSED_ void v3x_str_init(V3xStr *V3X_NN s, char *buf, size_t cap)
{
    if (!s) return;
    s->buf = buf;
    s->cap = buf ? cap : 0;
    s->len = 0;
    s->truncated = 0;
    if (s->cap) buf[0] = 0;
}

static V3X_SAFE_UNUSED_ const char *v3x_str_cstr(const V3xStr *s)
{
    return v3x_str_valid_(s) ? s->buf : "";
}

static V3X_SAFE_UNUSED_ size_t v3x_str_len(const V3xStr *s) { return v3x_str_valid_(s) ? s->len : 0; }
static V3X_SAFE_UNUSED_ size_t v3x_str_cap(const V3xStr *s) { return v3x_str_valid_(s) ? s->cap : 0; }
/* 1 if an append did not fit (or the builder is unusable) */
static V3X_SAFE_UNUSED_ int v3x_str_truncated(const V3xStr *s) { return !v3x_str_valid_(s) || s->truncated; }

static V3X_SAFE_UNUSED_ void v3x_str_clear(V3xStr *V3X_NN s)
{
    if (!s) return;
    s->len = 0;
    s->truncated = 0;
    if (s->buf && s->cap) s->buf[0] = 0;
    else s->cap = 0;
}

static V3X_SAFE_UNUSED_ V3X_MUST_USE_STRICT V3xResult v3x_str_appendn(V3xStr *V3X_NN s, const char *t, size_t n)
{
    if (!v3x_str_valid_(s)) { if (s) s->truncated = 1; return V3X_R_EARG; }
    if (s->truncated) return V3X_R_ETRUNC;
    if (!t) n = 0;
    size_t room = s->cap - 1 - s->len;   /* no wrap: len < cap checked above */
    if (n <= room) {
        if (n) memmove(s->buf + s->len, t, n);   /* memmove: t may point into buf */
        s->len += n;
        s->buf[s->len] = 0;
        return V3X_R_OK;
    }
    memmove(s->buf + s->len, t, room);
    size_t start = s->len;
    s->len = v3x_utf8_trim_tail(s->buf, start + room, start);
    s->buf[s->len] = 0;
    s->truncated = 1;
    return V3X_R_ETRUNC;
}

static V3X_SAFE_UNUSED_ V3X_MUST_USE_STRICT V3xResult v3x_str_append(V3xStr *V3X_NN s, const char *t)
{
    return v3x_str_appendn(s, t, t ? strlen(t) : 0);
}

static V3X_SAFE_UNUSED_ V3X_MUST_USE_STRICT V3xResult v3x_str_appendc(V3xStr *V3X_NN s, char c)
{
    return v3x_str_appendn(s, &c, 1);
}

static V3X_SAFE_UNUSED_ V3X_PRINTF(2, 0) V3X_MUST_USE_STRICT V3xResult v3x_str_vappendf(V3xStr *V3X_NN s,
                                                                                       const char *V3X_NN fmt, va_list ap)
{
    if (!v3x_str_valid_(s)) { if (s) s->truncated = 1; return V3X_R_EARG; }
    if (s->truncated) return V3X_R_ETRUNC;
    if (!fmt) return V3X_R_OK;
    size_t start = s->len, room = s->cap - s->len;   /* room includes the NUL */
    int n = vsnprintf(s->buf + start, room, fmt, ap);
    if (n >= 0 && (size_t)n < room) { s->len = start + (size_t)n; return V3X_R_OK; }
    /* truncated (n >= room) or a CRT that returns -1 on truncation / encoding error */
    s->buf[s->cap - 1] = 0;
    size_t got = start + strlen(s->buf + start);
    s->len = v3x_utf8_trim_tail(s->buf, got, start);
    s->buf[s->len] = 0;
    s->truncated = 1;
    return V3X_R_ETRUNC;
}

static V3X_SAFE_UNUSED_ V3X_PRINTF(2, 3) V3X_MUST_USE_STRICT V3xResult v3x_str_appendf(V3xStr *V3X_NN s, const char *V3X_NN fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    V3xResult r = v3x_str_vappendf(s, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- V3xSpan: checked reader over a byte range (borrowed) ------------------------------------
 * Every read checks the remaining length. The first failing read sets err (sticky): it and every
 * later read fail, outputs are zeroed, take returns NULL. Integers are little-endian, unaligned OK.
 * The fields are PRIVATE (set only by v3x_span_init; read through v3x_span_pos / v3x_span_len /
 * v3x_span_failed / v3x_span_remaining). Every function first checks the invariant (pos <= len,
 * p != NULL when len > 0): a span whose fields were overwritten fails (err set), it is never used to
 * form an out-of-range pointer. Read results are V3X_MUST_USE: check each one, or check
 * v3x_span_failed() once after a sequence and cast the reads to (void). */
typedef struct V3xSpan {
    const uint8_t *p;   /* private: borrowed */
    size_t len;         /* private */
    size_t pos;         /* private */
    int err;            /* private (read: v3x_span_failed) */
} V3xSpan;

/* 1 if the span is usable: invariant holds and no error yet. Sets err when the invariant is broken. */
static V3X_SAFE_UNUSED_ int v3x_span_ok_(V3xSpan *r)
{
    if (!r) return 0;
    if (r->err) return 0;
    if (r->pos > r->len || (!r->p && r->len)) { r->err = 1; return 0; }
    return 1;
}

static V3X_SAFE_UNUSED_ void v3x_span_init(V3xSpan *V3X_NN r, const void *p, size_t len)
{
    if (!r) return;
    r->p = (const uint8_t *)p;
    r->len = p ? len : 0;
    r->pos = 0;
    r->err = 0;
}

static V3X_SAFE_UNUSED_ size_t v3x_span_remaining(const V3xSpan *r)
{
    return r && !r->err && r->pos <= r->len && (r->p || !r->len) ? r->len - r->pos : 0;
}
static V3X_SAFE_UNUSED_ size_t v3x_span_pos(const V3xSpan *r) { return r && r->pos <= r->len ? r->pos : 0; }
static V3X_SAFE_UNUSED_ size_t v3x_span_len(const V3xSpan *r) { return r && (r->p || !r->len) ? r->len : 0; }
/* 1 after the first failed read (or for a NULL / corrupted span) */
static V3X_SAFE_UNUSED_ int v3x_span_failed(const V3xSpan *r)
{
    return !r || r->err || r->pos > r->len || (!r->p && r->len);
}

/* n bytes at the read position (a view into the span), or NULL (err set) if fewer remain.
 * take(0) returns the current position (non-NULL unless the span is empty and has no data). */
static V3X_SAFE_UNUSED_ V3X_MUST_USE const uint8_t *v3x_span_take(V3xSpan *V3X_NN r, size_t n)
{
    if (!v3x_span_ok_(r)) return NULL;
    if (n > r->len - r->pos || (!r->p && n)) { r->err = 1; return NULL; }
    const uint8_t *q = r->p ? r->p + r->pos : NULL;
    r->pos += n;
    return q;
}

static V3X_SAFE_UNUSED_ V3X_MUST_USE V3xResult v3x_span_skip(V3xSpan *V3X_NN r, size_t n)
{
    if (!v3x_span_ok_(r)) return V3X_R_EBOUNDS;
    if (n > r->len - r->pos) { r->err = 1; return V3X_R_EBOUNDS; }
    r->pos += n;
    return V3X_R_OK;
}

/* a sub-span of the next n bytes (consumed from r); an empty span with err=1 if they do not fit */
static V3X_SAFE_UNUSED_ V3X_MUST_USE V3xSpan v3x_span_sub(V3xSpan *V3X_NN r, size_t n)
{
    V3xSpan s;
    const uint8_t *q = v3x_span_take(r, n);
    v3x_span_init(&s, q, q ? n : 0);
    s.err = (!r || r->err) ? 1 : 0;
    return s;
}

static V3X_SAFE_UNUSED_ V3X_MUST_USE V3xResult v3x_span_u8(V3xSpan *V3X_NN r, uint8_t *out)
{
    const uint8_t *q = v3x_span_take(r, 1);
    if (out) *out = q ? q[0] : 0;
    return q ? V3X_R_OK : V3X_R_EBOUNDS;
}

static V3X_SAFE_UNUSED_ V3X_MUST_USE V3xResult v3x_span_u16le(V3xSpan *V3X_NN r, uint16_t *out)
{
    const uint8_t *q = v3x_span_take(r, 2);
    if (out) *out = q ? (uint16_t)(q[0] | (uint16_t)(q[1] << 8)) : 0;
    return q ? V3X_R_OK : V3X_R_EBOUNDS;
}

static V3X_SAFE_UNUSED_ V3X_MUST_USE V3xResult v3x_span_u32le(V3xSpan *V3X_NN r, uint32_t *out)
{
    const uint8_t *q = v3x_span_take(r, 4);
    if (out) *out = q ? ((uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24) : 0;
    return q ? V3X_R_OK : V3X_R_EBOUNDS;
}

static V3X_SAFE_UNUSED_ V3X_MUST_USE V3xResult v3x_span_u64le(V3xSpan *V3X_NN r, uint64_t *out)
{
    const uint8_t *q = v3x_span_take(r, 8);
    uint64_t v = 0;
    if (q)
        for (int i = 7; i >= 0; i--) v = v << 8 | q[i];
    if (out) *out = v;
    return q ? V3X_R_OK : V3X_R_EBOUNDS;
}

/* ---- paths ------------------------------------------------------------------------------------
 * A "relative path" from an untrusted source (manifest, cache, ini, mod file) is accepted only if:
 * 1..V3X_PATH_REL_MAX bytes; components separated by '/' or '\' (no leading or trailing separator,
 * no empty component); every component 1..255 bytes, not "." or "..", no ':' (drive, stream), no
 * control character or any of < > " | ? *, not ending in '.' or ' ', and its stem (before the first
 * '.', trailing spaces ignored) is not a DOS device name (CON PRN AUX NUL CONIN$ CONOUT$ COM0-9
 * LPT0-9, also COM/LPT + superscript 1-3), case-insensitive. Flag V3X_PATH_SINGLE: one component only. */
#define V3X_PATH_REL_MAX 512
#define V3X_PATH_CHECK_REPARSE 1u   /* join: refuse if base\c1, base\c1\c2, ... is a reparse point (Windows) */
#define V3X_PATH_SINGLE 2u          /* rel must be a single file name */

static V3X_SAFE_UNUSED_ int v3x_ascii_ieq_(const char *a, size_t n, const char *b)
{
    size_t m = strlen(b);
    if (n != m) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'a' && x <= 'z') x = (unsigned char)(x - 32u);
        if (y >= 'a' && y <= 'z') y = (unsigned char)(y - 32u);
        if (x != y) return 0;
    }
    return 1;
}

static V3X_SAFE_UNUSED_ int v3x_path_reserved_(const char *c, size_t n)
{
    size_t stem = 0;
    while (stem < n && c[stem] != '.') stem++;
    while (stem > 0 && c[stem - 1] == ' ') stem--;
    static const char *const names[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (v3x_ascii_ieq_(c, stem, names[i])) return 1;
    if (stem >= 4 && (v3x_ascii_ieq_(c, 3, "COM") || v3x_ascii_ieq_(c, 3, "LPT"))) {
        if (stem == 4 && c[3] >= '0' && c[3] <= '9') return 1;
        /* superscript 1, 2, 3 in UTF-8: C2 B9, C2 B2, C2 B3 */
        if (stem == 5 && (unsigned char)c[3] == 0xC2 &&
            ((unsigned char)c[4] == 0xB9 || (unsigned char)c[4] == 0xB2 || (unsigned char)c[4] == 0xB3))
            return 1;
    }
    return 0;
}

static V3X_SAFE_UNUSED_ V3X_MUST_USE V3xResult v3x_path_rel_ok_ex(const char *V3X_NN rel, unsigned flags)
{
    if (!rel) return V3X_R_EARG;
    size_t n = strlen(rel);
    if (n == 0 || n > V3X_PATH_REL_MAX) return V3X_R_EPATH;
    size_t i = 0;
    while (i <= n) {
        size_t j = i;
        while (j < n && rel[j] != '/' && rel[j] != '\\') j++;
        size_t cn = j - i;
        const char *c = rel + i;
        if (cn == 0 || cn > 255) return V3X_R_EPATH;            /* leading / trailing / doubled separator */
        if ((cn == 1 && c[0] == '.') || (cn == 2 && c[0] == '.' && c[1] == '.')) return V3X_R_EPATH;
        for (size_t k = 0; k < cn; k++) {
            unsigned char ch = (unsigned char)c[k];
            if (ch < 0x20 || ch == 0x7F || ch == ':' || ch == '<' || ch == '>' || ch == '"' || ch == '|' ||
                ch == '?' || ch == '*')
                return V3X_R_EPATH;
        }
        if (c[cn - 1] == '.' || c[cn - 1] == ' ') return V3X_R_EPATH;
        if (v3x_path_reserved_(c, cn)) return V3X_R_EPATH;
        if (j < n && (flags & V3X_PATH_SINGLE)) return V3X_R_EPATH;
        i = j + 1;
    }
    return V3X_R_OK;
}

static V3X_SAFE_UNUSED_ V3X_MUST_USE V3xResult v3x_path_rel_ok(const char *V3X_NN rel)
{
    return v3x_path_rel_ok_ex(rel, 0);
}

#if V3X_SAFE_WIN_
/* ---- Windows: wide paths, code pages, handle-based reparse checks ------------------------------
 * Text encodings: the W functions take UTF-16 (what the file system uses: always exact). The char
 * functions that touch the file system (v3x_path_join_checked with V3X_PATH_CHECK_REPARSE,
 * v3x_file_open_checked) take the caller's ANSI code page (CP_ACP), like CreateFileA and
 * api->v3x_dir() / api->game_dir(). UTF-8 text (manifests, ini values) -> v3x_widen(CP_UTF8, ...) and
 * the W function. Limits of v3x_path_rel_ok_ex (512 bytes / 255 per component) count UTF-8 bytes.
 *
 * Reparse checks open every existing prefix with FILE_FLAG_OPEN_REPARSE_POINT and read its attributes
 * through the handle (no GetFileAttributes name lookup). A check alone is check-then-use: to close
 * the race use v3x_file_open_checked[W] (re-verifies the opened handle) or v3x_file_write_atomic_in[W]
 * (sdk/v3x_util.h: holds the directory while it writes). */
#define V3X_PATH_WMAX 2048   /* wchar_t capacity of the joined paths built internally */

/* src (code page cp, e.g. CP_ACP or CP_UTF8; n bytes or SIZE_MAX = until the NUL) -> out (cap wchar_t,
 * NUL-terminated). Invalid sequences are refused. Returns V3X_R_OK, EARG, ETRUNC or EPATH (bad bytes). */
static V3X_SAFE_UNUSED_ V3X_MUST_USE V3xResult v3x_widen(unsigned cp, const char *V3X_NN src, size_t n,
                                                       wchar_t *V3X_NN out, size_t cap)
{
    if (!out || !cap) return V3X_R_EARG;
    out[0] = 0;
    if (!src) return V3X_R_EARG;
    size_t len = n == SIZE_MAX ? strlen(src) : strnlen(src, n);
    if (len == 0) return V3X_R_OK;
    if (len > 0x7FFFFFF0u || cap > 0x7FFFFFF0u) return V3X_R_ETRUNC;
    int need = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, src, (int)len, NULL, 0);
    if (need <= 0) return V3X_R_EPATH;
    if ((size_t)need >= cap) return V3X_R_ETRUNC;
    if (MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, src, (int)len, out, (int)cap) != need) { out[0] = 0; return V3X_R_EPATH; }
    out[need] = 0;
    return V3X_R_OK;
}

/* v3x_path_rel_ok_ex for a UTF-16 rel (converted to UTF-8, unpaired surrogates refused). */
static V3X_SAFE_UNUSED_ V3X_MUST_USE V3xResult v3x_path_rel_ok_exW(const wchar_t *V3X_NN rel, unsigned flags)
{
    if (!rel) return V3X_R_EARG;
    size_t n = wcsnlen(rel, V3X_PATH_REL_MAX + 1);
    if (n == 0 || n > V3X_PATH_REL_MAX) return V3X_R_EPATH;
    char u8[V3X_PATH_REL_MAX * 3 + 1];
    int k = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, rel, (int)n, u8, (int)sizeof u8 - 1, NULL, NULL);
    if (k <= 0) return V3X_R_EPATH;
    u8[k] = 0;
    return v3x_path_rel_ok_ex(u8, flags);
}

/* v3x_path_rel_ok_ex for a rel in code page cp (DBCS-safe: validated after conversion). */
static V3X_SAFE_UNUSED_ V3X_MUST_USE V3xResult v3x_path_rel_ok_cp(unsigned cp, const char *V3X_NN rel, unsigned flags)
{
    if (!rel) return V3X_R_EARG;
    size_t n = strnlen(rel, V3X_PATH_REL_MAX + 1);
    if (n == 0 || n > V3X_PATH_REL_MAX) return V3X_R_EPATH;
    wchar_t w[V3X_PATH_REL_MAX + 1];
    if (v3x_widen(cp, rel, n, w, sizeof w / sizeof w[0]) != V3X_R_OK) return V3X_R_EPATH;
    return v3x_path_rel_ok_exW(w, flags);
}

/* 0 = p exists and is not a reparse point, 1 = it is one, -1 = it does not exist, -2 = cannot tell */
static V3X_SAFE_UNUSED_ int v3x_reparse_state_w_(const wchar_t *p)
{
    HANDLE h = CreateFileW(p, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? -1 : -2;
    }
    BY_HANDLE_FILE_INFORMATION fi;
    BOOL ok = GetFileInformationByHandle(h, &fi);
    CloseHandle(h);
    if (!ok) return -2;
    return (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? 1 : 0;
}

/* 1 if a prefix of path longer than path[0..from) that ends at a separator, or the whole path, is a
 * reparse point (or cannot be checked: fail closed); 0 if none is (a missing component ends the
 * walk: nothing below it exists). path[0..from) (your trusted base) is not checked. */
static V3X_SAFE_UNUSED_ V3X_MUST_USE int v3x_path_has_reparseW(const wchar_t *V3X_NN path, size_t from)
{
    if (!path) return 1;
    size_t len = wcsnlen(path, V3X_PATH_WMAX);
    if (len >= V3X_PATH_WMAX || from > len) return 1;
    wchar_t w[V3X_PATH_WMAX];
    memcpy(w, path, (len + 1) * sizeof w[0]);
    for (size_t i = from; i <= len; i++) {
        if (i < len && w[i] != L'\\' && w[i] != L'/') continue;
        if (i == 0 || i == from) continue;   /* empty prefix / the base itself */
        wchar_t keep = w[i];
        w[i] = 0;
        int st = v3x_reparse_state_w_(w);
        w[i] = keep;
        if (st == -1) return 0;
        if (st != 0) return 1;
    }
    return 0;
}

/* v3x_path_has_reparseW for a char path in code page cp (from = byte offset). */
static V3X_SAFE_UNUSED_ V3X_MUST_USE int v3x_path_has_reparse_cp(unsigned cp, const char *V3X_NN path, size_t from)
{
    wchar_t w[V3X_PATH_WMAX];
    if (!path || from > strlen(path) || v3x_widen(cp, path, SIZE_MAX, w, V3X_PATH_WMAX) != V3X_R_OK) return 1;
    int wfrom = from ? MultiByteToWideChar(cp, 0, path, (int)from, NULL, 0) : 0;
    if (wfrom < 0 || (from && wfrom == 0)) return 1;
    return v3x_path_has_reparseW(w, (size_t)wfrom);
}

/* out = base + '\' (if base does not end in a separator) + rel ('/' -> '\'), all UTF-16. rel must pass
 * v3x_path_rel_ok_exW(rel, flags). base is trusted (your own directory; L"" = rel alone). out must
 * not overlap base or rel. V3X_PATH_CHECK_REPARSE: refuse if base\c1, base\c1\c2, ... is a reparse
 * point. On any error out = L"" (when cap > 0). */
static V3X_SAFE_UNUSED_ V3X_MUST_USE V3xResult v3x_path_join_checkedW(wchar_t *V3X_NN out, size_t cap,
                                                                      const wchar_t *V3X_NN base,
                                                                      const wchar_t *V3X_NN rel, unsigned flags)
{
    if (!out || !cap) return V3X_R_EARG;
    out[0] = 0;
    if (!base) return V3X_R_EARG;
    V3xResult r = v3x_path_rel_ok_exW(rel, flags);
    if (r != V3X_R_OK) return r;
    size_t bl = wcsnlen(base, cap), rl = wcslen(rel);   /* rl <= V3X_PATH_REL_MAX (validated) */
    if (bl >= cap) return V3X_R_ETRUNC;
    size_t sep = (bl && base[bl - 1] != L'\\' && base[bl - 1] != L'/') ? 1u : 0u;
    if (cap - bl - sep <= rl) return V3X_R_ETRUNC;       /* bl + sep + rl + NUL must fit */
    memmove(out, base, bl * sizeof *out);
    if (sep) out[bl] = L'\\';
    size_t from = bl + sep;
    for (size_t i = 0; i < rl; i++) out[from + i] = (rel[i] == L'/' || rel[i] == L'\\') ? L'\\' : rel[i];
    out[from + rl] = 0;
    if ((flags & V3X_PATH_CHECK_REPARSE) && v3x_path_has_reparseW(out, from)) { out[0] = 0; return V3X_R_EREPARSE; }
    return V3X_R_OK;
}
#endif

/* out = base + separator (if base does not end in one) + rel (separators normalised to '\' on
 * Windows, '/' elsewhere). rel must pass v3x_path_rel_ok_ex(rel, flags) (on Windows checked as CP_ACP
 * text, see v3x_path_rel_ok_cp). base is trusted (your own directory) and may be "" (then out = rel).
 * On Windows base and rel are in the ANSI code page (CP_ACP: api->v3x_dir(), the *A functions) and
 * the reparse check converts with CP_ACP; for UTF-8 text use v3x_path_join_checkedW. On any error
 * out = "" (when cap > 0). out must not overlap base or rel. */
static V3X_SAFE_UNUSED_ V3X_MUST_USE V3xResult v3x_path_join_checked(char *V3X_NN out, size_t cap,
                                                                     const char *V3X_NN base,
                                                                     const char *V3X_NN rel, unsigned flags)
{
    if (!out || !cap) return V3X_R_EARG;
    out[0] = 0;
    if (!base) return V3X_R_EARG;
#if V3X_SAFE_WIN_
    V3xResult r = v3x_path_rel_ok_cp(CP_ACP, rel, flags);
#else
    V3xResult r = v3x_path_rel_ok_ex(rel, flags);
#endif
    if (r != V3X_R_OK) return r;
#if defined(_WIN32)
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    V3xStr s;
    v3x_str_init(&s, out, cap);
    size_t bl = strlen(base);
    (void)v3x_str_appendn(&s, base, bl);
    if (bl && base[bl - 1] != '/' && base[bl - 1] != '\\') (void)v3x_str_appendc(&s, sep);
    size_t from = s.len;
    for (const char *p = rel; *p; p++) (void)v3x_str_appendc(&s, (*p == '/' || *p == '\\') ? sep : *p);
    if (s.truncated) { out[0] = 0; return V3X_R_ETRUNC; }
#if V3X_SAFE_WIN_
    if ((flags & V3X_PATH_CHECK_REPARSE) && v3x_path_has_reparse_cp(CP_ACP, out, from)) { out[0] = 0; return V3X_R_EREPARSE; }
#else
    (void)from;
#endif
    return V3X_R_OK;
}

#if V3X_SAFE_WIN_
/* 1 if a and b (final paths) are equal ignoring case */
static V3X_SAFE_UNUSED_ int v3x_path_ieq_w_(const wchar_t *a, const wchar_t *b)
{
    return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
}

/* GetFinalPathNameByHandleW into out (cap wchar_t) without a trailing separator. 0 = ok. */
static V3X_SAFE_UNUSED_ int v3x_final_path_w_(HANDLE h, wchar_t *out, size_t cap)
{
    DWORD n = GetFinalPathNameByHandleW(h, out, (DWORD)cap, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n == 0 || n >= cap) return -1;
    while (n > 0 && out[n - 1] == L'\\') out[--n] = 0;
    return 0;
}

/* Deletes the file behind h (opened by us, ideally with DELETE access). Best effort. */
static V3X_SAFE_UNUSED_ void v3x_delete_by_handle_(HANDLE h)
{
    FILE_DISPOSITION_INFO del;
    del.DeleteFile = TRUE;
    if (SetFileInformationByHandle(h, FileDispositionInfo, &del, sizeof del)) return;
    HANDLE hd = ReOpenFile(h, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_FLAG_OPEN_REPARSE_POINT);
    if (hd == INVALID_HANDLE_VALUE) return;
    (void)SetFileInformationByHandle(hd, FileDispositionInfo, &del, sizeof del);
    CloseHandle(hd);
}

/* Opens base\rel race-free with respect to reparse points: rel is validated, its chain checked, the
 * file opened with FILE_FLAG_OPEN_REPARSE_POINT (the last component is never followed), and then the
 * OPENED HANDLE is verified: not a reparse point and its final path == final(base)\rel. A component
 * swapped for a junction / symlink between the check and the open is caught there: the handle is
 * closed (a file this call created is deleted first) and V3X_R_EREPARSE returned.
 * access/share/disposition/attrs are CreateFileW's (attrs may add FILE_FLAG_BACKUP_SEMANTICS to open
 * a directory). *out = the handle (CloseHandle it) or INVALID_HANDLE_VALUE. V3X_R_EIO: an open
 * failed (GetLastError() tells why). An 8.3 short name in rel never verifies (refused). */
static V3X_SAFE_UNUSED_ V3X_MUST_USE V3xResult v3x_file_open_checkedW(const wchar_t *V3X_NN base,
                                                                      const wchar_t *V3X_NN rel, DWORD access,
                                                                      DWORD share, DWORD disposition, DWORD attrs,
                                                                      HANDLE *V3X_NN out)
{
    if (!out) return V3X_R_EARG;
    *out = INVALID_HANDLE_VALUE;
    if (!base || !base[0]) return V3X_R_EARG;
    wchar_t path[V3X_PATH_WMAX], fb[V3X_PATH_WMAX], fh[V3X_PATH_WMAX];
    V3xResult r = v3x_path_join_checkedW(path, V3X_PATH_WMAX, base, rel, V3X_PATH_CHECK_REPARSE);
    if (r != V3X_R_OK) return r;
    HANDLE hb = CreateFileW(base, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (hb == INVALID_HANDLE_VALUE) return V3X_R_EIO;
    int bad = v3x_final_path_w_(hb, fb, V3X_PATH_WMAX);
    CloseHandle(hb);
    if (bad) return V3X_R_EIO;
    if (disposition == CREATE_NEW) access |= DELETE;   /* to remove our own new file if it fails the check */
    HANDLE h = CreateFileW(path, access, share, NULL, disposition, attrs | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) return V3X_R_EIO;
    DWORD cerr = GetLastError();
    int created = disposition == CREATE_NEW ||
                  ((disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS) && cerr != ERROR_ALREADY_EXISTS);
    BY_HANDLE_FILE_INFORMATION fi;
    int ok = GetFileInformationByHandle(h, &fi) && !(fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
             v3x_final_path_w_(h, fh, V3X_PATH_WMAX) == 0;
    if (ok) {   /* expected = final(base) + '\' + rel with '\' separators */
        size_t fl = wcslen(fb), rl = wcslen(rel);
        ok = fl + 1 + rl < V3X_PATH_WMAX;
        if (ok) {
            fb[fl] = L'\\';
            for (size_t i = 0; i < rl; i++) fb[fl + 1 + i] = rel[i] == L'/' ? L'\\' : rel[i];
            fb[fl + 1 + rl] = 0;
            ok = v3x_path_ieq_w_(fh, fb);
        }
    }
    if (ok) { *out = h; return V3X_R_OK; }
    if (created) v3x_delete_by_handle_(h);
    CloseHandle(h);
    return V3X_R_EREPARSE;
}

/* v3x_file_open_checkedW with base and rel in the ANSI code page (CP_ACP). */
static V3X_SAFE_UNUSED_ V3X_MUST_USE V3xResult v3x_file_open_checked(const char *V3X_NN base, const char *V3X_NN rel,
                                                                     DWORD access, DWORD share, DWORD disposition,
                                                                     DWORD attrs, HANDLE *V3X_NN out)
{
    if (!out) return V3X_R_EARG;
    *out = INVALID_HANDLE_VALUE;
    if (!base || !rel) return V3X_R_EARG;
    wchar_t wb[V3X_PATH_WMAX], wr[V3X_PATH_REL_MAX + 1];
    if (v3x_widen(CP_ACP, base, SIZE_MAX, wb, V3X_PATH_WMAX) != V3X_R_OK) return V3X_R_EPATH;
    if (strnlen(rel, V3X_PATH_REL_MAX + 1) > V3X_PATH_REL_MAX ||
        v3x_widen(CP_ACP, rel, SIZE_MAX, wr, V3X_PATH_REL_MAX + 1) != V3X_R_OK)
        return V3X_R_EPATH;
    return v3x_file_open_checkedW(wb, wr, access, share, disposition, attrs, out);
}
#endif

/* ---- log line sanitising ----------------------------------------------------------------------
 * Copies src into dst (cap bytes incl. NUL) replacing every ASCII control character (0x00-0x1F,
 * 0x7F; tab and newline too), C1 control (U+0080-U+009F) and bidi override / isolate (U+202A-U+202E,
 * U+2066-U+2069) with '?', so text from mods cannot forge log lines or reorder them on screen.
 * Other bytes (UTF-8) are copied; the result is cut at a codepoint boundary. Returns strlen(dst).
 * n = maximum source bytes to read (SIZE_MAX = until the NUL). */
static V3X_SAFE_UNUSED_ size_t v3x_sanitize_linen(char *V3X_NN dst, size_t cap, const char *src, size_t n)
{
    if (!dst || !cap) return 0;
    V3xStr s;
    v3x_str_init(&s, dst, cap);
    for (size_t i = 0; src && i < n && src[i] && !s.truncated;) {
        unsigned char c = (unsigned char)src[i];
        unsigned char c1 = i + 1 < n ? (unsigned char)src[i + 1] : 0;
        unsigned char c2 = (c1 && i + 2 < n) ? (unsigned char)src[i + 2] : 0;
        if (c < 0x20 || c == 0x7F) { (void)v3x_str_appendc(&s, '?'); i++; continue; }
        if (c == 0xC2 && c1 >= 0x80 && c1 <= 0x9F) { (void)v3x_str_appendc(&s, '?'); i += 2; continue; }
        if (c == 0xE2 && ((c1 == 0x80 && c2 >= 0xAA && c2 <= 0xAE) || (c1 == 0x81 && c2 >= 0xA6 && c2 <= 0xA9))) {
            (void)v3x_str_appendc(&s, '?');
            i += 3;
            continue;
        }
        (void)v3x_str_appendc(&s, (char)c);
        i++;
    }
    if (s.truncated) {   /* appendc cannot see a split codepoint: trim it here */
        s.len = v3x_utf8_trim_tail(dst, s.len, 0);
        dst[s.len] = 0;
    }
    return s.len;
}

static V3X_SAFE_UNUSED_ size_t v3x_sanitize_line(char *V3X_NN dst, size_t cap, const char *src)
{
    return v3x_sanitize_linen(dst, cap, src, SIZE_MAX);
}

/* ---- atomics (sequentially consistent; for flags, counters and pointers shared across threads) */
#if defined(__GNUC__) || defined(__clang__)
static V3X_SAFE_UNUSED_ int v3x_atomic_load_int(const volatile int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static V3X_SAFE_UNUSED_ void v3x_atomic_store_int(volatile int *p, int v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
/* 1 if *p was `expected` and is now `desired`; 0 otherwise (then *p is unchanged) */
static V3X_SAFE_UNUSED_ int v3x_atomic_cas_int(volatile int *p, int expected, int desired)
{
    return __atomic_compare_exchange_n(p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 1 : 0;
}
/* returns the NEW value */
static V3X_SAFE_UNUSED_ int v3x_atomic_add_int(volatile int *p, int d) { return __atomic_add_fetch(p, d, __ATOMIC_SEQ_CST); }
static V3X_SAFE_UNUSED_ void *v3x_atomic_load_ptr(void *const volatile *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static V3X_SAFE_UNUSED_ void v3x_atomic_store_ptr(void *volatile *p, void *v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static V3X_SAFE_UNUSED_ int v3x_atomic_cas_ptr(void *volatile *p, void *expected, void *desired)
{
    return __atomic_compare_exchange_n(p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 1 : 0;
}
#elif V3X_SAFE_WIN_
static V3X_SAFE_UNUSED_ int v3x_atomic_load_int(const volatile int *p) { return (int)InterlockedCompareExchange((volatile LONG *)p, 0, 0); }
static V3X_SAFE_UNUSED_ void v3x_atomic_store_int(volatile int *p, int v) { InterlockedExchange((volatile LONG *)p, v); }
static V3X_SAFE_UNUSED_ int v3x_atomic_cas_int(volatile int *p, int e, int d) { return InterlockedCompareExchange((volatile LONG *)p, d, e) == e; }
static V3X_SAFE_UNUSED_ int v3x_atomic_add_int(volatile int *p, int d) { return (int)InterlockedExchangeAdd((volatile LONG *)p, d) + d; }
static V3X_SAFE_UNUSED_ void *v3x_atomic_load_ptr(void *const volatile *p) { return InterlockedCompareExchangePointer((void *volatile *)p, NULL, NULL); }
static V3X_SAFE_UNUSED_ void v3x_atomic_store_ptr(void *volatile *p, void *v) { InterlockedExchangePointer(p, v); }
static V3X_SAFE_UNUSED_ int v3x_atomic_cas_ptr(void *volatile *p, void *e, void *d) { return InterlockedCompareExchangePointer(p, d, e) == e; }
#endif

/* ---- main-thread assertion ------------------------------------------------------------------------
 * V3X_ASSERT_MAIN(api): 1 if the caller runs on the game's main thread (or the loader is too old to
 * say: API < v4), else 0 and an ERROR line in v3x.log (at most 8 per translation unit).
 *   if (!V3X_ASSERT_MAIN(api)) return;   // in code that touches game state */
#define V3X_ASSERT_MAIN(api) v3x_assert_main_((api), __FILE__, __LINE__)
static V3X_SAFE_UNUSED_ int v3x_assert_main_(const V3X_API *api, const char *file, int line)
{
#if V3X_SAFE_WIN_
    if (!V3X_HAS(api, main_thread_id)) return 1;
    uint32_t main_id = api->main_thread_id();
    if (!main_id || main_id == (uint32_t)GetCurrentThreadId()) return 1;
    static volatile int reported;
    if (v3x_atomic_add_int(&reported, 1) <= 8 && api->log)
        api->log("v3x_safe", "ERROR: %s:%d must run on the main thread (called on thread %lu)", file ? file : "?",
                 line, (unsigned long)GetCurrentThreadId());
    return 0;
#else
    (void)api; (void)file; (void)line;
    return 1;
#endif
}

V3X_NN_END
#ifdef __cplusplus
}
#endif
#endif
