/* v3x_fuzz.h - harness API of the in-tree deterministic mutational fuzzer (test/fuzz/, audit action 15).
 *
 * A harness file test/fuzz/fuzz_<name>.c includes this header and the parser it tests, and registers
 * one or more targets plus seeds:
 *
 *   #include "v3x_fuzz.h"
 *   #include "../../plugins/my_plugin/my_parser.c"
 *   static void fuzz_my_parser(const uint8_t *data, size_t len)
 *   {
 *       const char *text = v3x_fuzz_cstr(data, len);    // NUL-terminated copy, the NUL is the last byte
 *       my_parse(text);                                // before a PAGE_NOACCESS guard page
 *   }
 *   V3X_FUZZ_TARGET(my_parser, fuzz_my_parser)
 *   V3X_FUZZ_SEED(my_parser, "key = value\n[section]\n")
 *
 * The driver (test/fuzz/fuzz_main.c, linked with every harness file by test/run_all.sh `t fuzz`) runs
 * each seed as-is, then FUZZ_ITERS (default 2000) mutated inputs per target: bit flips, interesting
 * bytes/integers, inserts (random or dictionary tokens: "..", "\\", "[", "=", "%n", "CON", UTF-8 cuts),
 * deletes, duplications, truncation and splices with other seeds. Deterministic: xorshift64* with a
 * fixed seed (FUZZ_SEED to change), so a failure reproduces with the same command. Seeds also come from
 * test/fuzz/corpus/<target>/ (any files). Built with -fsanitize=undefined -fsanitize-trap=undefined:
 * UB, an access violation (e.g. reading past `len` into the guard page), abort() from a harness
 * assertion or a hang (FUZZ_TIMEOUT_MS, default 10000 per input) writes the input to
 * $FUZZ_OUT/crash-<target>-<iteration>.bin (default: current directory) and exits non-zero.
 *
 * Rules for harnesses: deterministic (no time, no threads, no global state carried between calls
 * unless reset), no files outside a temp dir, no network. The input buffer is read-only and exactly
 * `len` bytes long, ending at the guard page. Check invariants with V3X_FUZZ_ASSERT (abort = crash).
 * Harnesses whose code needs no windows.h can also be built as a libFuzzer target on Linux with
 * -DV3X_FUZZ_LIBFUZZER -fsanitize=fuzzer,address (one V3X_FUZZ_TARGET per file then). */
#ifndef V3X_FUZZ_H
#define V3X_FUZZ_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*v3x_fuzz_fn)(const uint8_t *data, size_t len);

#define V3X_FUZZ_ASSERT(c)                                                                           \
    do {                                                                                             \
        if (!(c)) { fprintf(stderr, "FUZZ ASSERT %s:%d: %s\n", __FILE__, __LINE__, #c); abort(); } \
    } while (0)

#ifdef V3X_FUZZ_LIBFUZZER
/* libFuzzer: the target is the entry point; seeds come from the corpus directory. */
static inline const char *v3x_fuzz_cstr(const uint8_t *d, size_t n)
{
    static char *buf;
    free(buf);
    buf = (char *)malloc(n + 1);
    if (!buf) abort();
    if (n) memcpy(buf, d, n);
    buf[n] = 0;
    return buf;
}
static inline void *v3x_fuzz_guarded(const void *d, size_t n) { return (void *)v3x_fuzz_cstr((const uint8_t *)d, n); }
#define V3X_FUZZ_TARGET(name, fn) \
    int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) { fn(d, n); return 0; }
#define V3X_FUZZ_SEED(name, literal)
#else
void v3x_fuzz_register(const char *name, v3x_fuzz_fn fn);
void v3x_fuzz_add_seed(const char *name, const void *data, size_t len);
/* NUL-terminated copy of d[0..n) whose NUL is the last byte before a no-access guard page (freed by the
 * driver after the current input; at most 16 per input). */
const char *v3x_fuzz_cstr(const uint8_t *d, size_t n);
/* Copy of d[0..n) that ends exactly at a guard page (no NUL added). Start is aligned to 8 if n is. */
void *v3x_fuzz_guarded(const void *d, size_t n);

#define V3X_FUZZ_CAT2_(a, b) a##b
#define V3X_FUZZ_CAT_(a, b) V3X_FUZZ_CAT2_(a, b)
#define V3X_FUZZ_TARGET(name, fn)                                                                  \
    __attribute__((constructor)) static void V3X_FUZZ_CAT_(v3x_fuzz_reg_, name)(void)              \
    {                                                                                              \
        v3x_fuzz_register(#name, fn);                                                              \
    }
/* literal: a string literal (its trailing NUL is not part of the seed) */
#define V3X_FUZZ_SEED(name, literal)                                                               \
    __attribute__((constructor)) static void V3X_FUZZ_CAT_(V3X_FUZZ_CAT_(v3x_fuzz_seed_, name), __LINE__)(void) \
    {                                                                                              \
        v3x_fuzz_add_seed(#name, literal, sizeof(literal) - 1);                                    \
    }
#endif

#ifdef __cplusplus
}
#endif
#endif
