/* fast_lookup_test.c - offline tests for plugins/fast_lookup (docs/re/datastructures.md).
 *
 * Part A (no game files): fl_core.h against a fake token registry with the engine's exact lookup
 *   semantics. Every answer of the cache (measure and on) must equal the engine's; ids == size
 *   (the engine's one-past-the-end read) are never cached; an illegal slot rewrite is detected
 *   (mismatch -> disabled, still correct answers); 8 reader threads + 1 registering writer;
 *   the indent emulation against a C model of the engine routine.
 * Part B (with victoria3.exe): anchors resolve at the 1.15.1 RVAs and pass the cross-check; then
 *   the REAL engine code (exe mapped with DONT_RESOLVE_DLL_REFERENCES, registry global pointed at
 *   a fake registry in the engine layout) is run side by side with the cache / emulation:
 *   TokenGetString answers and WriteIndent stream calls must be identical. Finally the plugin is
 *   loaded against a fake API whose hook is MinHook on the mapped exe (mode=on), and calls through
 *   the hooked engine functions must give the same answers / bytes as the unhooked engine.
 *
 * Build/run (from v3x/):
 *   $CC -I$M/include -o $T/fkt.exe test/fast_lookup_test.c $M/src/buffer.c $M/src/hook.c \
 *       $M/src/trampoline.c $M/src/hde/hde64.c && $T/fkt.exe [$EXE [$T]]
 *   ($M = third_party/minhook, $T = a temp dir; without $EXE only Part A runs) */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/v3x.h"
#include "../sdk/v3x_testkit.h"
#include "MinHook.h"
#include "../plugins/fast_lookup/fast_lookup.c"

#define CHECK(c, ...) V3X_TEST_CHECK(c, __VA_ARGS__)

/* ---------------- fake std::string (MSVC layout) */
typedef struct fstr { union { char buf[16]; char *ptr; } u; int64_t size, cap; } fstr;
static fstr *fstr_new(const char *s)
{
    fstr *f = calloc(1, sizeof *f);
    size_t n = strlen(s);
    if (n <= 15) { memcpy(f->u.buf, s, n + 1); f->cap = 15; }
    else { f->u.ptr = _strdup(s); f->cap = (int64_t)n; }
    f->size = (int64_t)n;
    return f;
}

/* ---------------- fake registry in the engine layout */
typedef struct freg {
    volatile LONG lock;        /* +0: bit0 writer, readers add 2 */
    uint8_t pad[0x48 - 4];
    void **slots;              /* +0x48 */
    int32_t cap;               /* +0x50 */
    volatile int32_t size;     /* +0x54 */
    void *alloc;               /* +0x58 */
    int32_t base;              /* +0x60 */
    uint8_t tail[0x40];
} freg;
static fstr g_default_str;     /* the engine's default string */
static freg *R;
static void *g_reg_ptr;        /* the "exe global" holding the registry pointer */
static SRWLOCK g_wlock = SRWLOCK_INIT;

static void reg_init(int nstatic, int cap, int garbage_tail)
{
    R = calloc(1, sizeof *R);
    R->cap = cap;
    R->slots = calloc((size_t)cap + 8, sizeof(void *));
    if (garbage_tail) for (int i = 0; i < cap + 8; i++) R->slots[i] = (void *)(uintptr_t)(0xdead0000u + i);
    for (int i = 0; i < nstatic; i++) {
        char nm[32]; snprintf(nm, sizeof nm, i == FL_TOKEN_INDENT ? "\t" : "tok%d", i);
        R->slots[i] = (i % 7 == 3) ? NULL : fstr_new(nm);       /* some holes (never registered ids) */
    }
    R->size = nstatic;
    R->base = nstatic;
    g_reg_ptr = R;
}
/* the engine's lookup semantics (TokenGetString), with the engine's reader lock */
static void *fake_lookup(int32_t id)
{
    AcquireSRWLockShared(&g_wlock);
    void *r = &g_default_str;
    if (id >= 0 && id <= R->size) { void *p = R->slots[id]; if (p) r = p; }
    ReleaseSRWLockShared(&g_wlock);
    return r;
}
/* the registrar: appends one token in a zero-filled fresh slot */
static int32_t fake_register(const char *s)
{
    AcquireSRWLockExclusive(&g_wlock);
    int32_t id = R->size;
    if (id >= R->cap) { ReleaseSRWLockExclusive(&g_wlock); return -1; }
    R->slots[id] = NULL;
    R->size = id + 1;
    R->slots[id] = fstr_new(s);
    ReleaseSRWLockExclusive(&g_wlock);
    return id;
}

static fl_tokcache *new_cache(uint32_t cap)
{
    fl_tokcache *c = calloc(1, sizeof *c);
    c->slots = calloc(cap, sizeof(void *));
    c->cap = cap;
    c->dflt = &g_default_str;
    c->reg_global = (void *const *)&g_reg_ptr;
    c->orig = fake_lookup;
    return c;
}
static uint64_t fake_clock(void) { static uint64_t t; return t += 7; }

static void test_semantics(int on)
{
    reg_init(200, 4000, 1);
    fl_tokcache *c = new_cache(1 << 12);
    fl_stripe *s = &c->st[0];
    int bad = 0, n = 0;
    srand(1234 + on);
    for (int round = 0; round < 40000; round++) {
        if (round % 50 == 0) { char nm[32]; snprintf(nm, sizeof nm, "dyn%d", round); fake_register(nm); }
        int32_t id = (rand() % (R->size + 6)) - 3;              /* includes <0, == size, > size */
        if (round % 97 == 0) id = R->size;                      /* the one-past-the-end read */
        void *want = fake_lookup(id);
        void *got = fl_tok_get(c, s, id, on, on ? 0 : 0, fake_clock);
        bad += got != want; n++;
    }
    CHECK(bad == 0, "%s: %d/%d answers identical to the engine lookup", on ? "on" : "measure", n - bad, n);
    CHECK(c->mismatches == 0 && !c->disabled, "%s: no mismatch, cache still enabled", on ? "on" : "measure");
    CHECK(s->fills > 100 && s->hits > 10000, "%s: cache used (fills %llu, hits %llu)", on ? "on" : "measure",
          (unsigned long long)s->fills, (unsigned long long)s->hits);
    int oob = 0;
    for (uint32_t i = (uint32_t)R->size; i < c->cap; i++) oob += c->slots[i] != NULL;
    CHECK(oob == 0, "%s: nothing cached at or beyond the registry size (one-past-the-end garbage)", on ? "on" : "measure");
    if (!on) CHECK(s->timed > 0 && s->cyc_orig > 0, "measure: timing samples taken (%llu)", (unsigned long long)s->timed);
}

static void test_violation(void)
{
    reg_init(100, 1000, 0);
    fl_tokcache *c = new_cache(1 << 12);
    fl_stripe *s = &c->st[0];
    for (int i = 0; i < 100; i++) fl_tok_get(c, s, i, 0, 0, NULL);   /* fill */
    void *old = R->slots[5];
    R->slots[5] = fstr_new("rewritten");                              /* what the proof says never happens */
    void *got = fl_tok_get(c, s, 5, 0, 0, NULL);
    CHECK(got == R->slots[5] && got != old, "measure: engine answer returned after an illegal slot rewrite");
    CHECK(c->mismatches == 1 && c->disabled && c->bad_id == 5, "measure: mismatch logged (id %d) and cache disabled", (int)c->bad_id);
    CHECK(fl_tok_get(c, s, 5, 1, 0, NULL) == R->slots[5], "disabled cache forwards to the engine in mode=on");

    reg_init(100, 1000, 0);
    c = new_cache(1 << 12); s = &c->st[0];
    for (int i = 0; i < 100; i++) fl_tok_get(c, s, i, 1, 1, NULL);
    R->slots[9] = fstr_new("rewritten9");
    got = fl_tok_get(c, s, 9, 1, 1, NULL);
    CHECK(got == R->slots[9] && c->disabled, "on + verify_every=1: rewrite detected, engine answer returned, cache off");

    reg_init(100, 1000, 0);
    c = new_cache(1 << 12); s = &c->st[0];
    for (int i = 0; i < 50; i++) fl_tok_get(c, s, i, 1, 0, NULL);
    R->size = 0;                                                      /* registrar mid-growth / teardown */
    got = fl_tok_get(c, s, 10, 1, 0, NULL);
    CHECK(got == &g_default_str, "size 0 (growth / teardown): hit path falls back to the engine (default string)");
}

static volatile LONG g_stop, g_tbad, g_treads;
static fl_tokcache *g_tcache;
static DWORD WINAPI reader(LPVOID p)
{
    fl_stripe *s = &g_tcache->st[(uintptr_t)p];
    uint32_t x = (uint32_t)(uintptr_t)p * 2654435761u + 1;
    while (!g_stop) {
        x = x * 1103515245u + 12345u;
        int32_t sz = R->size;
        int32_t id = (int32_t)(x % (uint32_t)(sz + 2));
        void *got = fl_tok_get(g_tcache, s, id, 1, 0, NULL);
        /* permanent truth: once a slot is set it never changes; default allowed only if unset */
        void *now = id < R->cap ? R->slots[id] : NULL;
        if (got != &g_default_str && got != now) InterlockedIncrement(&g_tbad);
        if (got == &g_default_str && now && id < sz) InterlockedIncrement(&g_tbad);
        InterlockedIncrement(&g_treads);
    }
    return 0;
}
static void test_threads(void)
{
    reg_init(300, 12000, 0);
    g_tcache = new_cache(1 << 14);
    HANDLE th[8];
    for (int i = 0; i < 8; i++) th[i] = CreateThread(NULL, 0, reader, (LPVOID)(uintptr_t)i, 0, NULL);
    for (int i = 0; i < 6000; i++) { char nm[32]; snprintf(nm, sizeof nm, "t%d", i); fake_register(nm); if (i % 200 == 0) Sleep(1); }
    g_stop = 1;
    WaitForMultipleObjects(8, th, TRUE, INFINITE);
    CHECK(g_tbad == 0 && g_treads > 10000, "8 readers + registering writer: %ld reads, %ld wrong", g_treads, g_tbad);
    CHECK(g_tcache->mismatches == 0, "threads: no mismatch");
}

/* ---------------- indent: fake writer + recording stream */
typedef struct rec { int n; int len[64]; uint8_t isnull[64]; char data[64][80]; } rec;
static rec *g_rec;
static void rec_write(void *stream, fl_strview *sv)
{
    (void)stream;
    rec *r = g_rec;
    if (r->n >= 64) { r->n++; return; }
    r->len[r->n] = sv->len; r->isnull[r->n] = sv->is_null;
    memcpy(r->data[r->n], sv->p, (size_t)(sv->len < 79 ? sv->len : 79));
    r->n++;
}
static void *g_vtbl[16];
typedef struct fstream { void **vt; } fstream;
typedef struct fwriter { void *vt; fstream *stream; int32_t indent; uint8_t binary; uint8_t pad[3]; } fwriter;

/* C model of the engine's WriteIndent (exe+0x3e24100) for token length 1 */
static void model_indent(fwriter *w, const fstr *t)
{
    if (w->binary || w->indent <= 0) return;
    int32_t len = (int32_t)t->size, per = 64 / len, left = w->indent;
    while (left > 0) {
        int32_t k = per < left ? per : left;
        char buf[0x48];
        for (int i = 0; i < k; i++) memcpy(buf + i, t->cap > 15 ? t->u.ptr : t->u.buf, (size_t)len);
        buf[k * len] = 0;
        fl_strview sv = {buf, (int32_t)strlen(buf), 0, {0}};
        rec_write(w->stream, &sv);
        left -= k;
    }
}
static int rec_eq(const rec *a, const rec *b)
{
    if (a->n != b->n) return 0;
    for (int i = 0; i < a->n && i < 64; i++)
        if (a->len[i] != b->len[i] || a->isnull[i] != b->isnull[i] || memcmp(a->data[i], b->data[i], (size_t)a->len[i])) return 0;
    return 1;
}

static void test_indent_model(void)
{
    g_vtbl[7] = (void *)rec_write;
    fstream st = {g_vtbl};
    fstr *tab = fstr_new("\t"), *two = fstr_new("  ");
    int bad = 0;
    rec a, b;
    for (int ind = -2; ind <= 300; ind++) {
        for (int bin = 0; bin < 2; bin++) {
            fwriter w = {0, &st, ind, (uint8_t)bin, {0}};
            memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
            g_rec = &a; model_indent(&w, tab);
            g_rec = &b; int did = fl_indent(&w, tab, fl_stream_slot38);
            if (!did) { /* declined: the original runs, which must then be a no-op here */ if (a.n) bad++; }
            else if (!rec_eq(&a, &b)) bad++;
        }
    }
    CHECK(bad == 0, "indent emulation == engine model for indent -2..300, text and binary");
    fwriter w = {0, &st, 5, 0, {0}};
    CHECK(fl_indent(&w, two, fl_stream_slot38) == 0, "indent declines for a 2-char token (engine leaves stack bytes)");
    CHECK(fl_indent(&w, NULL, fl_stream_slot38) == 0, "indent declines without token");
    CHECK(fl_indent(&w, &g_default_str, fl_stream_slot38) == 0, "indent declines for the default (empty) string");
}

/* ---------------- Part B: the real engine code */
static uint8_t *X;   /* exe mapped executable (DONT_RESOLVE_DLL_REFERENCES) */
#define RVA_TOK 0x3e1e010
#define RVA_IND 0x3e24100
#define RVA_GLOBAL 0x5c40a38
#define RVA_DEFAULT 0x542c980

static freg *engine_reg(int nstatic, int cap)
{
    freg *r = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    r->cap = cap;
    r->slots = calloc((size_t)cap + 8, sizeof(void *));
    for (int i = 0; i < nstatic; i++) {
        char nm[32]; snprintf(nm, sizeof nm, i == FL_TOKEN_INDENT ? "\t" : "etok%d", i);
        r->slots[i] = (i % 5 == 2 && i != FL_TOKEN_INDENT) ? NULL : fstr_new(nm);
    }
    r->size = nstatic; r->base = nstatic;
    return r;
}
typedef void *(*eng_tok_fn)(int32_t);
typedef void (*eng_ind_fn)(void *writer);
static eng_tok_fn E_tok;      /* the engine function as called by the engine (hooked after B4 starts) */
static void *eng_tok_orig(int32_t id) { return E_tok(id); }

/* informational: 8 threads x N lookups through E_tok (engine spin lock vs hooked cache) */
static volatile LONG g_bgo;
static DWORD WINAPI bench_thr(LPVOID p)
{
    uint32_t x = (uint32_t)(uintptr_t)p + 7;
    uintptr_t acc = 0;
    while (!g_bgo) YieldProcessor();
    for (int i = 0; i < 2000000; i++) { x = x * 1103515245u + 12345u; acc += (uintptr_t)E_tok((int32_t)(x % 390u)); }
    return (DWORD)acc;
}
static double bench(int nthr)
{
    HANDLE th[16];
    LARGE_INTEGER f, a, b;
    g_bgo = 0;
    for (int i = 0; i < nthr; i++) th[i] = CreateThread(NULL, 0, bench_thr, (LPVOID)(uintptr_t)i, 0, NULL);
    Sleep(20);
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    g_bgo = 1;
    WaitForMultipleObjects(nthr, th, TRUE, INFINITE);
    QueryPerformanceCounter(&b);
    for (int i = 0; i < nthr; i++) CloseHandle(th[i]);
    return (double)(b.QuadPart - a.QuadPart) * 1e9 / (double)f.QuadPart / (2000000.0 * nthr);
}

/* ---------------- register probe: the engine's own callers of TokenGetString keep live values in
 * r8/r9/r11 across the call (the compiler knows the leaf only writes rax, rcx, rdx; 51 of 523 call
 * sites, e.g. exe+0x1905079 keeps the "ship_{}_{}_add" format pointer in r8 -> the 1.15.1 crash at
 * exe+0x43c1bb6 with a NULL format string when a C detour clobbered r8). tok_probe(fn, id, out) loads
 * sentinels into r8-r11 and ymm0-5, calls fn(id) and stores what is left in them to out[0..15]
 * (out[4+2i], out[5+2i] = low 128 bits of xmm i). */
uint64_t tok_probe(void *fn, int32_t id, uint64_t *out);
__asm__(
    ".intel_syntax noprefix\n"
    ".text\n"
    ".globl tok_probe\n"
    ".def tok_probe; .scl 2; .type 32; .endef\n"
    ".p2align 4\n"
    "tok_probe:\n"
    ".seh_proc tok_probe\n\t"
    "push rbx\n\t"
    ".seh_pushreg rbx\n\t"
    "push rsi\n\t"
    ".seh_pushreg rsi\n\t"
    "sub rsp, 0x28\n\t"
    ".seh_stackalloc 0x28\n\t"
    ".seh_endprologue\n\t"
    "mov rbx, rcx\n\t"
    "mov rsi, r8\n\t"
    "mov ecx, edx\n\t"
    "mov r8, 0x1111111111111108\n\t"
    "mov r9, 0x2222222222222209\n\t"
    "mov r10, 0x333333333333330a\n\t"
    "mov r11, 0x444444444444440b\n\t"
    "mov rax, 0x5555555555555500\n\t" "vmovq xmm0, rax\n\t" "vpbroadcastq ymm0, xmm0\n\t"
    "mov rax, 0x5555555555555501\n\t" "vmovq xmm1, rax\n\t" "vpbroadcastq ymm1, xmm1\n\t"
    "mov rax, 0x5555555555555502\n\t" "vmovq xmm2, rax\n\t" "vpbroadcastq ymm2, xmm2\n\t"
    "mov rax, 0x5555555555555503\n\t" "vmovq xmm3, rax\n\t" "vpbroadcastq ymm3, xmm3\n\t"
    "mov rax, 0x5555555555555504\n\t" "vmovq xmm4, rax\n\t" "vpbroadcastq ymm4, xmm4\n\t"
    "mov rax, 0x5555555555555505\n\t" "vmovq xmm5, rax\n\t" "vpbroadcastq ymm5, xmm5\n\t"
    "call rbx\n\t"
    "mov qword ptr [rsi], r8\n\t"
    "mov qword ptr [rsi+8], r9\n\t"
    "mov qword ptr [rsi+16], r10\n\t"
    "mov qword ptr [rsi+24], r11\n\t"
    "vmovdqu xmmword ptr [rsi+32], xmm0\n\t"
    "vmovdqu xmmword ptr [rsi+48], xmm1\n\t"
    "vmovdqu xmmword ptr [rsi+64], xmm2\n\t"
    "vmovdqu xmmword ptr [rsi+80], xmm3\n\t"
    "vmovdqu xmmword ptr [rsi+96], xmm4\n\t"
    "vmovdqu xmmword ptr [rsi+112], xmm5\n\t"
    "vextracti128 xmm0, ymm0, 1\n\t"
    "vmovq qword ptr [rsi+128], xmm0\n\t"
    "vextracti128 xmm5, ymm5, 1\n\t"
    "vmovq qword ptr [rsi+136], xmm5\n\t"
    "vzeroupper\n\t"
    "add rsp, 0x28\n\t"
    "pop rsi\n\t"
    "pop rbx\n\t"
    "ret\n"
    ".seh_endproc\n"
    ".att_syntax prefix\n");

/* 1 if every probed register survived the call */
static int regs_kept(const uint64_t *o)
{
    static const uint64_t want[4] = {0x1111111111111108ull, 0x2222222222222209ull, 0x333333333333330aull, 0x444444444444440bull};
    for (int i = 0; i < 4; i++) if (o[i] != want[i]) return 0;
    for (int i = 0; i < 6; i++)
        if (o[4 + 2 * i] != 0x5555555555555500ull + (uint64_t)i || o[5 + 2 * i] != 0x5555555555555500ull + (uint64_t)i) return 0;
    return o[16] == 0x5555555555555500ull && o[17] == 0x5555555555555505ull;   /* upper ymm0 / ymm5 */
}

static void probe_regs(const char *what, int32_t id, void *want)
{
    uint64_t o[18];
    memset(o, 0, sizeof o);
    void *got = (void *)tok_probe((void *)E_tok, id, o);
    CHECK(got == want && regs_kept(o), "%s id %d: answer ok %d, r8 %016llx r9 %016llx r10 %016llx r11 %016llx xmm0 %016llx "
          "ymm5hi %016llx (engine callers rely on r8-r11 + vector regs surviving)", what, (int)id, got == want,
          (unsigned long long)o[0], (unsigned long long)o[1], (unsigned long long)o[2], (unsigned long long)o[3],
          (unsigned long long)o[4], (unsigned long long)o[17]);
}

static void test_engine(const char *exe)
{
    V3X_API ta;
    if (v3x_testkit_map_exe(exe, &ta) != 0) { CHECK(0, "map %s", exe); return; }
    int failed = v3x_anchor_resolve_all(&ta, TAG, A, 2);
    CHECK(failed == 0, "anchors resolve on the real exe");
    if (failed) return;
    CHECK(v3x_testkit_rva(A[0].result) == RVA_TOK && v3x_testkit_rva(A[1].result) == RVA_IND,
          "TokenGetString rva 0x%llx, WriteIndent rva 0x%llx (1.15.1: 0x3e1e010 / 0x3e24100)",
          v3x_testkit_rva(A[0].result), v3x_testkit_rva(A[1].result));
    CHECK(cross_check(A[0].result, A[1].result) == NULL, "cross-check: same registry global + default string, slot 0x38");
    CHECK(v3x_testkit_rva(rip_target(A[0].result, TOK_GLOBAL_DISP)) == RVA_GLOBAL &&
          v3x_testkit_rva(rip_target(A[0].result, TOK_DEFAULT_DISP)) == RVA_DEFAULT,
          "registry global exe+0x%llx, default string exe+0x%llx",
          v3x_testkit_rva(rip_target(A[0].result, TOK_GLOBAL_DISP)), v3x_testkit_rva(rip_target(A[0].result, TOK_DEFAULT_DISP)));

    HMODULE h = LoadLibraryExA(exe, NULL, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(h != NULL, "exe mapped executable");
    if (!h) return;
    X = (uint8_t *)h;
    freg *er = engine_reg(400, 2000);
    *(void **)(X + RVA_GLOBAL) = er;
    E_tok = (eng_tok_fn)(X + RVA_TOK);
    eng_ind_fn E_ind = (eng_ind_fn)(X + RVA_IND);
    void *dflt = X + RVA_DEFAULT;
    probe_regs("unhooked engine", 5, E_tok(5));

    /* B2: real TokenGetString vs cache (measure, then on) */
    fl_tokcache *c = calloc(1, sizeof *c);
    c->slots = calloc(1 << 12, sizeof(void *)); c->cap = 1 << 12; c->dflt = dflt;
    c->reg_global = (void *const *)(X + RVA_GLOBAL); c->orig = eng_tok_orig;
    int bad = 0, n = 0, ndef = 0;
    for (int pass = 0; pass < 3; pass++)
        for (int32_t id = -3; id <= er->size + 3; id++) {
            void *want = E_tok(id);
            void *got = fl_tok_get(c, &c->st[0], id, pass > 0, pass == 2 ? 3 : 0, NULL);
            bad += want != got; n++; ndef += want == dflt;
        }
    CHECK(bad == 0 && n > 1000, "real TokenGetString == cache for %d lookups (%d default answers)", n, ndef);
    CHECK(c->mismatches == 0 && c->st[0].hits > 500, "real engine: cache hits %llu, no mismatch", (unsigned long long)c->st[0].hits);
    CHECK(er->lock == 0, "engine left its spin lock released");

    /* B3: real WriteIndent vs emulation */
    g_vtbl[7] = (void *)rec_write;
    fstream st = {g_vtbl};
    rec ra, rb;
    bad = 0;
    void *t17 = E_tok(FL_TOKEN_INDENT);
    for (int ind = -1; ind <= 200; ind++)
        for (int bin = 0; bin < 2; bin++) {
            fwriter w = {0, &st, ind, (uint8_t)bin, {0}};
            memset(&ra, 0, sizeof ra); memset(&rb, 0, sizeof rb);
            g_rec = &ra; E_ind(&w);
            g_rec = &rb;
            if (!fl_indent(&w, t17, fl_stream_slot38)) { if (ra.n) bad++; }
            else if (!rec_eq(&ra, &rb)) bad++;
        }
    CHECK(bad == 0, "real WriteIndent == emulation (same write calls, bytes, chunking) for indent -1..200");
    {   /* a 64+ indent produces 2+ chunks in the engine too */
        fwriter w = {0, &st, 130, 0, {0}};
        memset(&ra, 0, sizeof ra); g_rec = &ra; E_ind(&w);
        printf("  engine indent 130: %d calls, len %d %d %d\n", ra.n, ra.len[0], ra.len[1], ra.len[2]);
        CHECK(ra.n == 3 && ra.len[0] == 64 && ra.len[1] == 64 && ra.len[2] == 2, "engine indent 130 = chunks 64+64+2");
    }

    /* B4: the plugin end to end, MinHook on the mapped exe, mode=on */
    char dir[MAX_PATH], ini[MAX_PATH];
    GetTempPathA(sizeof dir, dir);
    snprintf(ini, sizeof ini, "%sfast_lookup.ini", dir);
    FILE *f = fopen(ini, "w");
    if (f) { fputs("mode = on\ntoken_cache = 1\nindent = 1\nverify_every = 0\nmax_tokens = 65536\n", f); fclose(f); }
    V3X_API pa;
    v3x_testkit_use_image(X);
    v3x_testkit_api(&pa);
    snprintf(v3x_tk.dir, sizeof v3x_tk.dir, "%s", dir);
    pa.hook = NULL;
    CHECK(MH_Initialize() == MH_OK, "MinHook init");
    extern int mh_hook(void *t, void *d, void **o);
    pa.hook = mh_hook;
    /* reference answers BEFORE hooking */
    double e1 = bench(1), e8 = bench(8);
    static void *ref[2100];
    for (int32_t id = 0; id < 2100; id++) ref[id] = E_tok(id - 50);
    CHECK(v3x_plugin_load(&pa) == 0 && g_tok_on && g_ind_on && g_mode == 2, "plugin loads in mode=on, both hooks in place");
    bad = 0;
    for (int pass = 0; pass < 3; pass++)
        for (int32_t id = 0; id < 2100; id++) {
            void *g = E_tok(id - 50);
            if (g != ref[id] && bad++ < 3) printf("  diff id %d: hooked %p engine %p (size %d)\n", id - 50, g, ref[id], er->size);
        }
    /* the crash reproducer: hooked TokenGetString must keep every register its leaf original keeps */
    probe_regs("hooked mode=on miss", er->size + 2, E_tok(er->size + 2));
    probe_regs("hooked mode=on hit", 7, ref[57]);
    probe_regs("hooked mode=on bad id", -1, ref[49]);
    g_mode = 1;   /* the lead's crashing config: measure */
    probe_regs("hooked mode=measure", 9, ref[59]);
    probe_regs("hooked mode=measure bad id", 1 << 20, ref[0]);
    g_mode = 2;
    fl_stripe t; sum(g_tc->st, &t);
    CHECK(bad == 0 && t.hits > 500, "hooked TokenGetString == unhooked engine (3 x 2100 ids; %llu cache hits)", (unsigned long long)t.hits);
    /* the plugin's indent through the hooked engine function vs the reference recording */
    bad = 0;
    for (int ind = 0; ind <= 140; ind += 7) {
        fwriter w = {0, &st, ind, 0, {0}};
        memset(&rb, 0, sizeof rb); g_rec = &rb; E_ind(&w);
        memset(&ra, 0, sizeof ra); g_rec = &ra; model_indent(&w, (const fstr *)t17);
        bad += !rec_eq(&ra, &rb);
    }
    fl_stripe d; sum(g_ind_st, &d);
    CHECK(bad == 0 && d.hits > 10, "hooked WriteIndent writes the engine's bytes (%llu emulated)", (unsigned long long)d.hits);
    /* a new registration after the cache filled: engine registers id = size, fresh slot */
    int32_t nid = er->size;
    er->slots[nid] = fstr_new("late_token");
    er->size = nid + 1;
    CHECK(E_tok(nid) == er->slots[nid], "token registered after filling is answered (not a stale default)");
    report("test", 1);
    double c1 = bench(1), c8 = bench(8);
    printf("bench (ns per lookup, wall / thread-calls): engine 1 thr %.1f, 8 thr %.1f | cache 1 thr %.1f, 8 thr %.1f\n", e1, e8, c1, c8);
    CHECK(g_tc->mismatches == 0, "no mismatch end to end");
    MH_DisableHook(MH_ALL_HOOKS);
}

int mh_hook(void *t, void *d, void **o)
{
    int rc = MH_CreateHook(t, d, o);
    return rc ? rc : MH_EnableHook(t);
}

int main(int argc, char **argv)
{
    test_semantics(0);
    test_semantics(1);
    test_violation();
    test_threads();
    test_indent_model();
    if (argc > 1) test_engine(argv[1]);
    else printf("(no exe given: Part B skipped)\n");
    return v3x_testkit_done();
}
