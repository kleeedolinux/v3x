/* fl_core.h - pure logic of the fast_lookup plugin (no Windows hooks; included by the plugin and
 * by test/fast_lookup_test.c). See docs/re/datastructures.md.
 *
 * 1) Token string cache. The engine's persistent token registry (pdx_core persistence, global
 *    object at exe+0x5d13bd0, pointer to it in exe+0x5c40a38) maps token id -> std::string*.
 *    Every lookup (TokenGetString, exe+0x3e1e010) takes the registry's reader/writer SPIN lock with
 *    `lock xadd` on one shared cache line. The save writer calls it once per token from many
 *    threads at once -> the line ping-pongs between cores (8% of all exe samples in a 4 h game).
 *    Registry facts this cache relies on (1.15.1, proven from the disassembly):
 *      - +0x48 std::string** slots, +0x54 int32 size, +0x60 static-token count.
 *      - lookup(id): id < 0 or id > size -> default string (exe+0x542c980); slot NULL -> default;
 *        else the slot pointer. (id == size reads one past the end: we never cache that case.)
 *      - the only writer is the token registrar (exe+0x3e1d6f0, under the write lock). It appends:
 *        new id = dynamic count + static count + 1, grows the array zero-filled, then stores a new
 *        heap std::string* in the fresh slot. Slots are never rewritten or freed until exit.
 *    So once lookup(id) returned a non-default pointer for an id that was < size BEFORE the call,
 *    every later lookup(id) returns the same pointer. The cache stores exactly those results and
 *    re-checks id < size (a plain read of a line nobody writes during a save) on every hit.
 *
 * 2) Text-writer indent (exe+0x3e24100). Writes indent-level copies of token 17 ("\t") in chunks
 *    of 64 / len characters through the stream's vtable slot 0x38; it too takes the registry spin
 *    lock (inline) just to read token 17. The emulation below reproduces the engine's exact
 *    write calls (same bytes, same chunking) when token 17 has length 1, else it declines and the
 *    original runs. Checked against the real engine code in test/fast_lookup_test.c. */
#ifndef FL_CORE_H
#define FL_CORE_H

#include <stdint.h>
#include <string.h>

/* ---- registry / string layout (1.15.1) */
#define FL_REG_SLOTS 0x48
#define FL_REG_SIZE 0x54
#define FL_TOKEN_INDENT 17

/* MSVC std::string: 16-byte SSO buffer or heap pointer at +0, size +0x10, capacity +0x18 */
static inline const char *fl_str_data(const void *s)
{
    return *(const uint64_t *)((const uint8_t *)s + 0x18) > 0xf ? *(const char *const *)s : (const char *)s;
}
static inline int64_t fl_str_size(const void *s) { return *(const int64_t *)((const uint8_t *)s + 0x10); }

/* The engine's string view passed to the stream write slot: {ptr, int32 len, bool is_null} */
typedef struct fl_strview {
    const char *p;
    int32_t len;
    uint8_t is_null;
    uint8_t pad[3];
} fl_strview;

typedef void *(*fl_tokget_fn)(int32_t id);               /* exe+0x3e1e010 */
typedef void (*fl_write_fn)(void *stream, fl_strview *); /* stream vtable slot 0x38 */

/* ---- per-thread-ish stats: 64 stripes of one cache line each, picked by thread id */
#define FL_STRIPES 64
typedef struct fl_stripe {
    uint64_t calls, hits, misses, fills, verified, timed, cyc_orig, cyc_fast;
} __attribute__((aligned(64))) fl_stripe;

typedef struct fl_tokcache {
    void *volatile *slots;        /* cap entries, zero = not cached */
    uint32_t cap;
    const void *dflt;             /* default string returned for unknown ids */
    void *const *reg_global;      /* exe global holding the registry pointer (may be NULL until CRT init) */
    fl_tokget_fn orig;            /* the engine lookup (trampoline) */
    volatile long disabled;       /* 1 after a mismatch: forward everything */
    volatile long mismatches;
    int32_t bad_id;
    const void *bad_cached, *bad_orig;
    fl_stripe st[FL_STRIPES];
} fl_tokcache;

static inline int32_t fl_reg_size(const fl_tokcache *c)
{
    const uint8_t *reg = c->reg_global ? (const uint8_t *)*(void *const volatile *)c->reg_global : NULL;
    return reg ? *(const volatile int32_t *)(reg + FL_REG_SIZE) : -1;
}

static inline void fl_note_mismatch(fl_tokcache *c, int32_t id, const void *cached, const void *orig)
{
    if (__sync_fetch_and_add(&c->mismatches, 1) == 0) { c->bad_id = id; c->bad_cached = cached; c->bad_orig = orig; }
    c->disabled = 1;
}

/* Stores orig's answer if it is provably permanent (see the header comment). */
static inline void fl_tok_fill(fl_tokcache *c, fl_stripe *s, int32_t id, int32_t size_before, void *v)
{
    if (v && v != c->dflt && id < size_before && (uint32_t)id < c->cap) {
        c->slots[id] = v;
        s->fills++;
    }
}

/* measure: the engine answers; the cache is filled and every cached answer is compared.
 * on:      cached answers are returned without the lock; verify_every > 0 re-checks every Nth hit
 *          (per stripe) against the engine. */
static inline void *fl_tok_get(fl_tokcache *c, fl_stripe *s, int32_t id, int on, uint32_t verify_every,
                               uint64_t (*clock)(void))
{
    uint64_t n = ++s->calls;
    if (c->disabled || id < 0 || (uint32_t)id >= c->cap) return c->orig(id);
    int32_t size = fl_reg_size(c);
    void *hit = id < size ? c->slots[id] : NULL;
    if (on && hit) {
        s->hits++;
        if (verify_every && n % verify_every == 0) {
            void *v = c->orig(id);
            s->verified++;
            if (v != hit) { fl_note_mismatch(c, id, hit, v); return v; }
        }
        return hit;
    }
    void *v;
    if (!on && clock && (n & 63) == 0) {          /* measure: time the engine call vs a cache probe */
        uint64_t t0 = clock();
        v = c->orig(id);
        uint64_t t1 = clock();
        volatile void *probe = id < fl_reg_size(c) ? c->slots[id] : NULL;
        (void)probe;
        uint64_t t2 = clock();
        s->timed++; s->cyc_orig += t1 - t0; s->cyc_fast += t2 - t1;
    } else {
        v = c->orig(id);
    }
    if (hit) {                                    /* measure mode: compare */
        s->hits++; s->verified++;
        if (v != hit) fl_note_mismatch(c, id, hit, v);
        return v;
    }
    s->misses++;
    fl_tok_fill(c, s, id, size, v);
    return v;
}

/* ---- text writer indent emulation. writer: +8 stream*, +0x10 int32 indent, +0x14 u8 binary.
 * Returns 1 if it wrote (the original must not run), 0 to let the original run. */
#define FL_W_STREAM 0x08
#define FL_W_INDENT 0x10
#define FL_W_BINARY 0x14

static inline int fl_indent(void *writer, const void *tok17, fl_write_fn (*slot38)(void *stream))
{
    uint8_t *w = (uint8_t *)writer;
    if (w[FL_W_BINARY] != 0 || *(int32_t *)(w + FL_W_INDENT) <= 0) return 0;   /* original: no-op; let it */
    if (!tok17 || fl_str_size(tok17) != 1) return 0;       /* only the 1-char case is reproduced exactly */
    const char ch = fl_str_data(tok17)[0];
    const int32_t len = (int32_t)fl_str_size(tok17), per = 64 / len;
    int32_t left = *(int32_t *)(w + FL_W_INDENT);
    char buf[0x48];
    while (left > 0) {
        int32_t k = per < left ? per : left;
        for (int32_t i = 0; i < k; i++) buf[i] = ch;      /* engine: memcpy(buf + i, data, len) for i < k */
        buf[k * len] = 0;
        void *stream = *(void **)(w + FL_W_STREAM);
        fl_strview sv;
        memset(&sv, 0, sizeof sv);
        sv.p = buf; sv.len = (int32_t)strlen(buf); sv.is_null = 0;
        slot38(stream)(stream, &sv);
        left -= k;
    }
    return 1;
}

/* default vtable accessor: (*(void ***)stream)[7] */
static inline fl_write_fn fl_stream_slot38(void *stream) { return (fl_write_fn)(*(void ***)stream)[0x38 / 8]; }

#endif
