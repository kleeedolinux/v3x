/* libFuzzer adapter for the in-tree harnesses (test/fuzz/fuzz_*.c, API test/fuzz/v3x_fuzz.h), used by the
 * Linux job of .github/workflows/ci.yml. Instead of test/fuzz/fuzz_main.c (Windows: guard pages via
 * VirtualAlloc, own mutator) the harness file is linked with this file and
 * clang -fsanitize=fuzzer,address,undefined: libFuzzer mutates with coverage guidance, ASan finds
 * out-of-bounds reads/writes (every input and every v3x_fuzz_cstr/guarded copy is its own heap block,
 * so a read one byte past it is reported), UBSan the rest. Harness files keep several targets; this
 * adapter picks one at start:
 *   FUZZ_TARGET=<name> ./fz [libFuzzer flags] [corpus dirs]   fuzz that target
 *   FUZZ_LIST=1 ./fz                                          print the target names, exit 0
 *   FUZZ_TARGET=<name> FUZZ_DUMP_SEEDS=<dir> ./fz             write the target's V3X_FUZZ_SEEDs into dir, exit 0
 * Only harnesses whose code needs no windows.h can be built here. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "v3x_fuzz.h"

#define MAX_TARGETS 64
#define MAX_SEEDS 512
#define MAX_COPIES 16

static struct { const char *name; v3x_fuzz_fn fn; } g_t[MAX_TARGETS];
static int g_nt;
static struct { const char *name; const void *d; size_t n; } g_s[MAX_SEEDS];
static int g_ns;
static v3x_fuzz_fn g_fn;
static void *g_copies[MAX_COPIES];
static int g_ncopies;

void v3x_fuzz_register(const char *name, v3x_fuzz_fn fn)
{
    if (g_nt < MAX_TARGETS) { g_t[g_nt].name = name; g_t[g_nt].fn = fn; g_nt++; }
}

void v3x_fuzz_add_seed(const char *name, const void *data, size_t len)
{
    if (g_ns < MAX_SEEDS) { g_s[g_ns].name = name; g_s[g_ns].d = data; g_s[g_ns].n = len; g_ns++; }
}

static void *copy_of(const void *d, size_t n, int nul)
{
    if (g_ncopies >= MAX_COPIES) { fprintf(stderr, "fuzz: more than %d copies per input\n", MAX_COPIES); abort(); }
    size_t sz = n + (size_t)nul;
    uint8_t *p = (uint8_t *)malloc(sz ? sz : 1);   /* exact size: ASan reports a read one byte past it */
    if (!p) abort();
    if (n) memcpy(p, d, n);
    if (nul) p[n] = 0;
    g_copies[g_ncopies++] = p;
    return p;
}

const char *v3x_fuzz_cstr(const uint8_t *d, size_t n) { return (const char *)copy_of(d, n, 1); }
void *v3x_fuzz_guarded(const void *d, size_t n) { return copy_of(d, n, 0); }

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    if (getenv("FUZZ_LIST")) {
        for (int i = 0; i < g_nt; i++) printf("%s\n", g_t[i].name);
        exit(0);
    }
    const char *want = getenv("FUZZ_TARGET");
    for (int i = 0; want && i < g_nt; i++)
        if (!strcmp(g_t[i].name, want)) g_fn = g_t[i].fn;
    if (!g_fn) {
        fprintf(stderr, "fuzz: set FUZZ_TARGET to one of:");
        for (int i = 0; i < g_nt; i++) fprintf(stderr, " %s", g_t[i].name);
        fprintf(stderr, "\n");
        exit(2);
    }
    const char *dump = getenv("FUZZ_DUMP_SEEDS");
    if (dump) {
        mkdir(dump, 0755);
        int k = 0;
        for (int i = 0; i < g_ns; i++) {
            if (strcmp(g_s[i].name, want)) continue;
            char path[4096];
            snprintf(path, sizeof path, "%s/seed-%03d", dump, k++);
            FILE *f = fopen(path, "wb");
            if (!f) { fprintf(stderr, "fuzz: cannot write %s (%s)\n", path, strerror(errno)); exit(2); }
            if (g_s[i].n) fwrite(g_s[i].d, 1, g_s[i].n, f);
            fclose(f);
        }
        printf("%d seed(s) of %s written to %s\n", k, want, dump);
        exit(0);
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    g_fn(data, size);
    for (int i = 0; i < g_ncopies; i++) free(g_copies[i]);
    g_ncopies = 0;
    return 0;
}
