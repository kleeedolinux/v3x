/* Driver of the in-tree mutational fuzzer (test/fuzz/v3x_fuzz.h has the harness API and the rules).
 *
 *   python -m ziglang cc -target x86_64-windows-gnu -O1 -fsanitize=undefined -fsanitize-trap=undefined \
 *       -Iinclude -Isdk -o fz.exe test/fuzz/fuzz_<name>.c test/fuzz/fuzz_main.c
 *   fz.exe [target ...]          all targets of this exe when none is named
 * Env: FUZZ_ITERS (mutated inputs per target, default 2000), FUZZ_SEED (default 0x5eed0003),
 *      FUZZ_MAXLEN (default 16384), FUZZ_TIMEOUT_MS (default 10000), FUZZ_OUT (crash files, default "."),
 *      FUZZ_CORPUS (default test/fuzz/corpus; files in <dir>/<target>/ are extra seeds),
 *      FUZZ_REPRO=<file>: run only that input through the named target (reproduce a crash).
 * Output: one line per target and a final "N checks, M failed" (a check = one target run clean).
 * Exit code 0 = no crash. A crash/UB trap/hang exits 99/98 after writing the input. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v3x_fuzz.h"

#define MAX_TARGETS 64
#define MAX_SEEDS 512
#define MAX_GUARDED 16

static struct { const char *name; v3x_fuzz_fn fn; } g_t[MAX_TARGETS];
static int g_nt;
static struct { const char *name; uint8_t *d; size_t n; } g_s[MAX_SEEDS];
static int g_ns;

void v3x_fuzz_register(const char *name, v3x_fuzz_fn fn)
{
    if (g_nt < MAX_TARGETS) { g_t[g_nt].name = name; g_t[g_nt].fn = fn; g_nt++; }
}

void v3x_fuzz_add_seed(const char *name, const void *data, size_t len)
{
    if (g_ns >= MAX_SEEDS) return;
    uint8_t *c = (uint8_t *)malloc(len ? len : 1);
    if (!c) return;
    if (len) memcpy(c, data, len);
    g_s[g_ns].name = name; g_s[g_ns].d = c; g_s[g_ns].n = len;
    g_ns++;
}

/* ---- guarded allocations: data ends at a PAGE_NOACCESS page */
static struct { void *base; size_t size; } g_g[MAX_GUARDED];
static int g_ng;
static size_t g_page = 4096;

static uint8_t *guard_alloc(size_t n)
{
    if (g_ng >= MAX_GUARDED) { fprintf(stderr, "fuzz: more than %d guarded buffers per input\n", MAX_GUARDED); abort(); }
    size_t body = (n + g_page - 1) / g_page * g_page;
    if (!body) body = g_page;
    uint8_t *b = (uint8_t *)VirtualAlloc(NULL, body + g_page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old;
    if (!b || !VirtualProtect(b + body, g_page, PAGE_NOACCESS, &old)) { fprintf(stderr, "fuzz: VirtualAlloc failed\n"); abort(); }
    g_g[g_ng].base = b; g_g[g_ng].size = body + g_page;
    g_ng++;
    return b + body - n;
}

static void guard_free_all(void)
{
    for (int i = 0; i < g_ng; i++) VirtualFree(g_g[i].base, 0, MEM_RELEASE);
    g_ng = 0;
}

const char *v3x_fuzz_cstr(const uint8_t *d, size_t n)
{
    char *p = (char *)guard_alloc(n + 1);
    if (n) memcpy(p, d, n);
    p[n] = 0;
    return p;
}

void *v3x_fuzz_guarded(const void *d, size_t n)
{
    uint8_t *p = guard_alloc(n);
    if (n) memcpy(p, d, n);
    return p;
}

/* ---- deterministic PRNG: xorshift64* */
static uint64_t g_rng;
static uint64_t rnd(void)
{
    g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1Dull;
}
static size_t rndn(size_t n) { return n ? (size_t)(rnd() % n) : 0; }

static const uint8_t k_bytes[] = {0, 1, 0x7F, 0x80, 0xFF, '\n', '\r', '\t', ' ', '=', '[', ']', ';', '#', '\\', '/', '.',
                                  ':', '"', '\'', '%', '*', '?', '<', '>', '|', '@', '$', 0xC2, 0xE2, 0xF0, 0xBF};
static const char *const k_tokens[] = {"..", "../", "..\\", "\\\\?\\", "C:\\", "\r\n", "[", "]", "=", " = ", "%s", "%n",
                                       "%999999d", "CON", "NUL", "COM1", "aux.txt", "\xC2", "\xE2\x80\xAE", "\xF0\x9F\x98",
                                       "0x", "-1", "4294967295", "18446744073709551615", "9223372036854775808", "1e999",
                                       "nan", "[plugins]", "[log]", "true", "off", "\"", "{", "}", "\\u0000", ".?AV", "@@"};
static const uint32_t k_ints[] = {0, 1, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000u,
                                  0xFFFFFFFFu, 0xFFFFFFFEu, 24, 4096};

static size_t g_maxlen = 16384;

/* mutates buf[0..n) in place (cap bytes available); returns the new length */
static size_t mutate(uint8_t *buf, size_t n, size_t cap, const char *target)
{
    int rounds = 1 + (int)rndn(6);
    for (int r = 0; r < rounds; r++) {
        switch (rndn(10)) {
        case 0:   /* bit flip */
            if (n) buf[rndn(n)] ^= (uint8_t)(1u << rndn(8));
            break;
        case 1:   /* interesting byte */
            if (n) buf[rndn(n)] = k_bytes[rndn(sizeof k_bytes)];
            break;
        case 2:   /* random bytes inserted */
        case 3: { /* token inserted */
            const char *tok = NULL;
            uint8_t rb[16];
            size_t k;
            if (rndn(2)) { tok = k_tokens[rndn(sizeof k_tokens / sizeof k_tokens[0])]; k = strlen(tok); }
            else { k = 1 + rndn(16); for (size_t i = 0; i < k; i++) rb[i] = (uint8_t)rnd(); }
            if (n + k > cap) break;
            size_t at = rndn(n + 1);
            memmove(buf + at + k, buf + at, n - at);
            memcpy(buf + at, tok ? (const uint8_t *)tok : rb, k);
            n += k;
            break;
        }
        case 4: { /* delete a range */
            if (!n) break;
            size_t at = rndn(n), k = 1 + rndn(n - at < 64 ? n - at : 64);
            memmove(buf + at, buf + at + k, n - at - k);
            n -= k;
            break;
        }
        case 5: { /* duplicate a range */
            if (!n) break;
            size_t at = rndn(n), k = 1 + rndn(n - at < 256 ? n - at : 256);
            if (n + k > cap) break;
            size_t to = rndn(n + 1);
            uint8_t tmp[256];
            memcpy(tmp, buf + at, k);
            memmove(buf + to + k, buf + to, n - to);
            memcpy(buf + to, tmp, k);
            n += k;
            break;
        }
        case 6:   /* truncate */
            if (n) n = rndn(n + 1);
            break;
        case 7: { /* interesting little-endian integer */
            if (n < 4) break;
            uint32_t v = k_ints[rndn(sizeof k_ints / sizeof k_ints[0])];
            if (rndn(4) == 0) v = (uint32_t)n - (uint32_t)rndn(8);
            size_t at = rndn(n - 3);
            if (rndn(2)) memcpy(buf + at, &v, 4);
            else { uint16_t h = (uint16_t)v; memcpy(buf + at, &h, 2); }
            break;
        }
        case 8: { /* splice: a random piece of another seed of this target */
            int cand[MAX_SEEDS], nc = 0;
            for (int i = 0; i < g_ns; i++) if (!strcmp(g_s[i].name, target) && g_s[i].n) cand[nc++] = i;
            if (!nc) break;
            int s = cand[rndn((size_t)nc)];
            size_t from = rndn(g_s[s].n), k = 1 + rndn(g_s[s].n - from);
            size_t at = rndn(n + 1);
            if (at + k > cap) k = cap - at;
            memcpy(buf + at, g_s[s].d + from, k);
            if (at + k > n) n = at + k;
            break;
        }
        default:  /* byte arithmetic */
            if (n) { size_t at = rndn(n); buf[at] = (uint8_t)(buf[at] + (uint8_t)(rndn(33) - 16)); }
            break;
        }
    }
    return n;
}

/* ---- crash reporting */
static const char *g_cur_target = "?";
static long g_cur_iter = -1;
static const uint8_t *g_cur_data;
static size_t g_cur_len;
static volatile LONG g_progress;
static char g_outdir[MAX_PATH] = ".";

static void dump_input(const char *why, int code)
{
    char p[MAX_PATH + 128];
    snprintf(p, sizeof p, "%s\\crash-%s-%ld.bin", g_outdir, g_cur_target, g_cur_iter);
    FILE *f = fopen(p, "wb");
    if (f) { if (g_cur_len) fwrite(g_cur_data, 1, g_cur_len, f); fclose(f); }
    fprintf(stdout, "\nFUZZ CRASH: target %s iteration %ld (%zu bytes): %s\n  input saved: %s\n  reproduce: FUZZ_REPRO=%s <exe> %s\n0 checks, 1 failed\n",
            g_cur_target, g_cur_iter, g_cur_len, why, p, p, g_cur_target);
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), (UINT)code);
}

static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep)
{
    char why[96];
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    snprintf(why, sizeof why, "exception 0x%08lx%s", c,
             c == EXCEPTION_ILLEGAL_INSTRUCTION ? " (UBSan trap / ud2)" : c == EXCEPTION_ACCESS_VIOLATION ? " (access violation)" : "");
    dump_input(why, 99);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void on_abort(int sig) { (void)sig; dump_input("abort() (harness assertion)", 99); }

static DWORD g_timeout_ms = 10000;
static DWORD WINAPI watchdog(void *arg)
{
    (void)arg;
    LONG last = -1;
    for (;;) {
        Sleep(g_timeout_ms);
        LONG now = g_progress;
        if (now == last) dump_input("hang (no progress within FUZZ_TIMEOUT_MS)", 98);
        last = now;
    }
}

static long env_num(const char *name, long def)
{
    char b[64];
    DWORD n = GetEnvironmentVariableA(name, b, sizeof b);
    return n && n < sizeof b ? strtol(b, NULL, 0) : def;
}

static uint64_t hash_name(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ull;
    return h;
}

static void load_corpus(const char *root, const char *target)
{
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\%s\\*", root, target);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s\\%s\\%s", root, target, fd.cFileName);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        static uint8_t b[1 << 20];
        size_t n = fread(b, 1, g_maxlen < sizeof b ? g_maxlen : sizeof b, f);
        fclose(f);
        v3x_fuzz_add_seed(target, b, n);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void run_one(int t, long iter, const uint8_t *d, size_t n)
{
    g_cur_iter = iter;
    uint8_t *g = (uint8_t *)v3x_fuzz_guarded(d, n);   /* the harness input itself ends at a guard page */
    g_cur_data = g; g_cur_len = n;
    g_t[t].fn(g, n);
    guard_free_all();
    InterlockedIncrement(&g_progress);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_page = si.dwPageSize;
    SetUnhandledExceptionFilter(on_crash);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    signal(SIGABRT, on_abort);
    long iters = env_num("FUZZ_ITERS", 2000);
    uint64_t seed = (uint64_t)env_num("FUZZ_SEED", 0x5eed0003);
    g_maxlen = (size_t)env_num("FUZZ_MAXLEN", 16384);
    if (g_maxlen < 64) g_maxlen = 64;
    g_timeout_ms = (DWORD)env_num("FUZZ_TIMEOUT_MS", 10000);
    DWORD on = GetEnvironmentVariableA("FUZZ_OUT", g_outdir, sizeof g_outdir);
    if (!on || on >= sizeof g_outdir) strcpy(g_outdir, ".");
    char corpus[MAX_PATH] = "test\\fuzz\\corpus", repro[MAX_PATH] = "";
    DWORD cn = GetEnvironmentVariableA("FUZZ_CORPUS", corpus, sizeof corpus);
    if (!cn || cn >= sizeof corpus) strcpy(corpus, "test\\fuzz\\corpus");
    DWORD rn = GetEnvironmentVariableA("FUZZ_REPRO", repro, sizeof repro);
    if (rn >= sizeof repro) repro[0] = 0;
    HANDLE wd = CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    if (wd) CloseHandle(wd);

    uint8_t *buf = (uint8_t *)malloc(g_maxlen);
    if (!buf) return 2;
    int checks = 0, failed = 0;
    for (int t = 0; t < g_nt; t++) {
        int want = argc < 2;
        for (int a = 1; a < argc; a++) if (!strcmp(argv[a], g_t[t].name)) want = 1;
        if (!want) continue;
        g_cur_target = g_t[t].name;
        if (repro[0]) {
            FILE *f = fopen(repro, "rb");
            if (!f) { printf("cannot open %s\n", repro); return 2; }
            size_t n = fread(buf, 1, g_maxlen, f);
            fclose(f);
            run_one(t, 0, buf, n);
            printf("fuzz %-24s repro of %s: no crash\n", g_t[t].name, repro);
            checks++;
            continue;
        }
        load_corpus(corpus, g_t[t].name);
        int seeds[MAX_SEEDS], ns = 0;
        for (int i = 0; i < g_ns; i++) if (!strcmp(g_s[i].name, g_t[t].name)) seeds[ns++] = i;
        g_rng = (seed ^ hash_name(g_t[t].name)) | 1;
        DWORD t0 = GetTickCount();
        run_one(t, -1, (const uint8_t *)"", 0);   /* empty input */
        for (int i = 0; i < ns; i++) run_one(t, -2 - i, g_s[seeds[i]].d, g_s[seeds[i]].n < g_maxlen ? g_s[seeds[i]].n : g_maxlen);
        for (long it = 0; it < iters; it++) {
            size_t n = 0;
            if (ns) {
                int s = seeds[rndn((size_t)ns)];
                n = g_s[s].n < g_maxlen ? g_s[s].n : g_maxlen;
                memcpy(buf, g_s[s].d, n);
            }
            n = mutate(buf, n, g_maxlen, g_t[t].name);
            run_one(t, it, buf, n);
        }
        printf("fuzz %-24s %ld iterations, %d seeds, 0 crashes (%lu ms)\n", g_t[t].name, iters, ns,
               (unsigned long)(GetTickCount() - t0));
        checks++;
    }
    if (!checks) { printf("no targets matched (%d registered)\n0 checks, 1 failed\n", g_nt); return 1; }
    printf("%d checks, %d failed\n", checks, failed);
    return failed;
}
