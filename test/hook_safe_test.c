/* Unit test for src/safe_hook.c (api->hook_safe, API v4). No game needed.
 *   M=third_party/minhook
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -I$M/include -o $T/hst.exe \
 *       test/hook_safe_test.c $M/src/buffer.c $M/src/hook.c $M/src/trampoline.c $M/src/hde/hde64.c
 *   $T/hst.exe
 * Checks: functions with 0..8 integer args, 16 args, float/double args and returns (also mixed
 * with integers, also in the stack slots), a struct returned through the hidden pointer, pre
 * changing args, skip_original with a replacement return value, post changing the return value,
 * recursion through the hook, 4 threads at once, unhook/rehook, the callee's non-volatile
 * registers, the thunk's unwind entry, a stack walk from inside the original through the thunk,
 * bad arguments, and register preservation with 2 subscribers (entry rax/r11/xmm4-5/upper ymm reach
 * the original; every register the original leaves, r8-r11/xmm4-5/upper ymm included, reaches the
 * caller; only rax/xmm0 change when post sets the return value). Exit code 0 = ALL OK. */
#include "../src/safe_hook.c"
#include <stdio.h>
#include <math.h>

static int fails, checks;
#define CHECK(c, ...) do { int ok_ = !!(c); checks++; printf("%s: ", ok_ ? "OK  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); fails += !ok_; } while (0)

#define NOINL __attribute__((noinline))
typedef long long ll;

/* Every target has a body the optimiser cannot fold and is at least 5 bytes long. */
static volatile ll sink_;
NOINL ll f0(void) { sink_++; return 4242; }
NOINL ll f1(ll a) { sink_++; return a * 3 + 1; }
NOINL ll f2(ll a, ll b) { sink_++; return a * 3 + b * 5; }
NOINL ll f3(ll a, ll b, ll c) { sink_++; return a + b * 10 + c * 100; }
NOINL ll f4(ll a, ll b, ll c, ll d) { sink_++; return a + b * 10 + c * 100 + d * 1000; }
NOINL ll f5(ll a, ll b, ll c, ll d, ll e) { sink_++; return f4(a, b, c, d) + e * 10000; }
NOINL ll f6(ll a, ll b, ll c, ll d, ll e, ll f) { sink_++; return f5(a, b, c, d, e) + f * 100000; }
NOINL ll f7(ll a, ll b, ll c, ll d, ll e, ll f, ll g) { sink_++; return f6(a, b, c, d, e, f) + g * 1000000; }
NOINL ll f8(ll a, ll b, ll c, ll d, ll e, ll f, ll g, ll h) { sink_++; return f7(a, b, c, d, e, f, g) + h * 10000000; }
NOINL ll f16(ll a1, ll a2, ll a3, ll a4, ll a5, ll a6, ll a7, ll a8, ll a9, ll a10, ll a11, ll a12, ll a13, ll a14,
             ll a15, ll a16)
{
    sink_++;
    return a1 + 2 * a2 + 3 * a3 + 4 * a4 + 5 * a5 + 6 * a6 + 7 * a7 + 8 * a8 + 9 * a9 + 10 * a10 + 11 * a11 +
           12 * a12 + 13 * a13 + 14 * a14 + 15 * a15 + 16 * a16;
}
NOINL double fd4(double a, double b, double c, double d) { sink_++; return a + b * 2 + c * 4 + d * 8; }
NOINL float ff2(float a, float b) { sink_++; return a * b + 0.5f; }
NOINL double fmix(ll a, double b, ll c, float d, double e, ll f) { sink_++; return (double)a + b * 10 + (double)c * 100 + d * 1000 + e * 10000 + (double)f * 100000; }
NOINL void fvoid(ll *out, ll v) { sink_++; *out = v * 7; }
typedef struct { ll a, b, c; } Big;
NOINL Big fbig(ll x, ll y) { sink_++; Big r = {x, y, x + y}; return r; }
extern ll (*volatile prec)(ll);
NOINL ll frec(ll n) { sink_++; return n <= 0 ? 0 : n + prec(n - 1); }   /* recursion through the hook */
NOINL ll fnv(ll a)   /* clobbers callee-saved registers: the caller must still see its own */
{
    sink_++;
    __asm__ volatile("mov $0x1111, %%rbx\n\tmov $0x2222, %%r12\n\tmov $0x3333, %%r15" ::: "rbx", "r12", "r15");
    return a + 1;
}

/* pointers through a volatile table so every call goes through the (hooked) entry */
static ll (*volatile p0)(void) = f0;
static ll (*volatile p1)(ll) = f1;
static ll (*volatile p2)(ll, ll) = f2;
static ll (*volatile p3)(ll, ll, ll) = f3;
static ll (*volatile p4)(ll, ll, ll, ll) = f4;
static ll (*volatile p5)(ll, ll, ll, ll, ll) = f5;
static ll (*volatile p6)(ll, ll, ll, ll, ll, ll) = f6;
static ll (*volatile p7)(ll, ll, ll, ll, ll, ll, ll) = f7;
static ll (*volatile p8)(ll, ll, ll, ll, ll, ll, ll, ll) = f8;
static ll (*volatile p16)(ll, ll, ll, ll, ll, ll, ll, ll, ll, ll, ll, ll, ll, ll, ll, ll) = f16;
static double (*volatile pd4)(double, double, double, double) = fd4;
static float (*volatile pf2)(float, float) = ff2;
static double (*volatile pmix)(ll, double, ll, float, double, ll) = fmix;
static void (*volatile pvoid)(ll *, ll) = fvoid;
static Big (*volatile pbig)(ll, ll) = fbig;
ll (*volatile prec)(ll) = frec;
static ll (*volatile pnv)(ll) = fnv;

/* ---- callbacks */
typedef struct { volatile LONG pre, post; V3X_HookCall last_pre, last_post; } Rec;
static void rec_pre(V3X_HookCall *c) { Rec *r = c->user; InterlockedIncrement(&r->pre); r->last_pre = *c; }
static void rec_post(V3X_HookCall *c) { Rec *r = c->user; InterlockedIncrement(&r->post); r->last_post = *c; }
static void pre_double_a0(V3X_HookCall *c) { c->args[0] *= 2; }
static void pre_skip(V3X_HookCall *c) { c->skip_original = 1; c->ret = 777; }
static void post_add(V3X_HookCall *c) { c->ret += 1000; }
static volatile LONG g_scratch_ok, g_scratch_zero;
static void pre_scratch(V3X_HookCall *c) { g_scratch_zero += c->scratch[0] == 0 && c->scratch[1] == 0; c->scratch[0] = c->args[0] ^ 0x5a5a; c->scratch[1] = 7; }
static void post_scratch(V3X_HookCall *c) { g_scratch_ok += c->scratch[0] == (c->args[0] ^ 0x5a5a) && c->scratch[1] == 7; }
static void pre_set_stack(V3X_HookCall *c) { c->args[7] = 9; }            /* 8th arg lives on the stack */
static void pre_xmm(V3X_HookCall *c) { c->xmm[1].d = 100.0; }
static void post_fret(V3X_HookCall *c) { c->xmm_ret.f = c->xmm_ret.f * 2; }
static void pre_clobber_fp(V3X_HookCall *c)   /* a callback that uses float math itself */
{
    volatile double x = 3.0;
    c->reserved = (int32_t)(sqrt(x) * 1000);
}

static int hook(void *t, v3x_hook_fn pre, v3x_hook_fn post, void *u)
{
    int s = v3x_safe_hook_install(t, pre, post, u);
    if (s) printf("  install %p -> %d\n", t, s);
    return s;
}
static void unhook(void *t) { MH_DisableHook(t); MH_RemoveHook(t); }

/* ---- stack walk through the thunk */
static void *g_frames[32]; static USHORT g_nframes;
NOINL ll fwalk(ll a) { sink_++; g_nframes = RtlCaptureStackBackTrace(0, 32, g_frames, NULL); return a; }
static ll (*volatile pwalk)(ll) = fwalk;
NOINL ll walk_caller(void) { sink_++; ll r = pwalk(5); sink_++; return r; }
static ll (*volatile pwalk_caller)(void) = walk_caller;
static int frame_in(void *fn)
{
    DWORD64 img = 0;
    PRUNTIME_FUNCTION want = RtlLookupFunctionEntry((DWORD64)fn, &img, NULL);
    for (int i = 0; i < g_nframes && want; i++) {
        DWORD64 ib = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)g_frames[i], &ib, NULL);
        if (rf == want) return i;
    }
    return -1;
}

/* ---- register preservation (WPO callers keep values in volatile registers across calls)
 * Regs layout: gp[0..6] = rax, rcx, rdx, r8, r9, r10, r11; y[i] = ymm i (32 bytes) at +64+32i.
 * reg_caller(fn, in, out): loads `in` into rax..r11 and ymm0-5 (xmm0-5 without AVX), calls fn,
 * stores the registers fn's call left into `out`. reg_target: records its entry registers in
 * g_rt_entry, then leaves g_rt_exit in them and returns. */
typedef struct { uint64_t gp[8]; uint8_t y[6][32]; } Regs;
Regs g_rt_entry, g_rt_exit;
void reg_caller(void *fn, const Regs *in, Regs *out);
void reg_target(void);
#define RT_YS(base, op) \
    op " ymmword ptr [" base "+64], ymm0\n\t" op " ymmword ptr [" base "+96], ymm1\n\t" \
    op " ymmword ptr [" base "+128], ymm2\n\t" op " ymmword ptr [" base "+160], ymm3\n\t" \
    op " ymmword ptr [" base "+192], ymm4\n\t" op " ymmword ptr [" base "+224], ymm5\n\t"
#define RT_XS(base, op) \
    op " xmmword ptr [" base "+64], xmm0\n\t" op " xmmword ptr [" base "+96], xmm1\n\t" \
    op " xmmword ptr [" base "+128], xmm2\n\t" op " xmmword ptr [" base "+160], xmm3\n\t" \
    op " xmmword ptr [" base "+192], xmm4\n\t" op " xmmword ptr [" base "+224], xmm5\n\t"
#define RT_YL(base, op) \
    op " ymm0, ymmword ptr [" base "+64]\n\t" op " ymm1, ymmword ptr [" base "+96]\n\t" \
    op " ymm2, ymmword ptr [" base "+128]\n\t" op " ymm3, ymmword ptr [" base "+160]\n\t" \
    op " ymm4, ymmword ptr [" base "+192]\n\t" op " ymm5, ymmword ptr [" base "+224]\n\t"
#define RT_XL(base, op) \
    op " xmm0, xmmword ptr [" base "+64]\n\t" op " xmm1, xmmword ptr [" base "+96]\n\t" \
    op " xmm2, xmmword ptr [" base "+128]\n\t" op " xmm3, xmmword ptr [" base "+160]\n\t" \
    op " xmm4, xmmword ptr [" base "+192]\n\t" op " xmm5, xmmword ptr [" base "+224]\n\t"
__asm__(
    ".intel_syntax noprefix\n"
    ".text\n"
    ".globl reg_caller\n"
    ".def reg_caller; .scl 2; .type 32; .endef\n"
    ".p2align 4\n"
    "reg_caller:\n"
    ".seh_proc reg_caller\n\t"
    "push rbx\n\t" ".seh_pushreg rbx\n\t"
    "push rsi\n\t" ".seh_pushreg rsi\n\t"
    "sub rsp, 0x28\n\t" ".seh_stackalloc 0x28\n\t"
    ".seh_endprologue\n\t"
    "mov rbx, rcx\n\t"
    "mov rsi, r8\n\t"
    "cmp dword ptr [rip+v3x_sh_avx], 0\n\t"
    "jle 1f\n\t"
    RT_YL("rdx", "vmovups")
    "jmp 2f\n"
    "1:\n\t"
    RT_XL("rdx", "movups")
    "2:\n\t"
    "mov rax, qword ptr [rdx]\n\t" "mov rcx, qword ptr [rdx+8]\n\t" "mov r8, qword ptr [rdx+24]\n\t"
    "mov r9, qword ptr [rdx+32]\n\t" "mov r10, qword ptr [rdx+40]\n\t" "mov r11, qword ptr [rdx+48]\n\t"
    "mov rdx, qword ptr [rdx+16]\n\t"
    "call rbx\n\t"
    "mov qword ptr [rsi], rax\n\t" "mov qword ptr [rsi+8], rcx\n\t" "mov qword ptr [rsi+16], rdx\n\t"
    "mov qword ptr [rsi+24], r8\n\t" "mov qword ptr [rsi+32], r9\n\t" "mov qword ptr [rsi+40], r10\n\t"
    "mov qword ptr [rsi+48], r11\n\t"
    "cmp dword ptr [rip+v3x_sh_avx], 0\n\t"
    "jle 3f\n\t"
    RT_YS("rsi", "vmovups")
    "vzeroupper\n\t"
    "jmp 4f\n"
    "3:\n\t"
    RT_XS("rsi", "movups")
    "4:\n\t"
    "add rsp, 0x28\n\t"
    "pop rsi\n\t"
    "pop rbx\n\t"
    "ret\n"
    ".seh_endproc\n"
    ".globl reg_target\n"
    ".def reg_target; .scl 2; .type 32; .endef\n"
    ".p2align 4\n"
    "reg_target:\n\t"
    ".byte 0x0f, 0x1f, 0x44, 0x00, 0x00\n\t"     /* 5-byte nop: room for MinHook's jmp */
    "mov qword ptr [rip+g_rt_entry], rax\n\t"
    "lea rax, [rip+g_rt_entry]\n\t"
    "mov qword ptr [rax+8], rcx\n\t" "mov qword ptr [rax+16], rdx\n\t" "mov qword ptr [rax+24], r8\n\t"
    "mov qword ptr [rax+32], r9\n\t" "mov qword ptr [rax+40], r10\n\t" "mov qword ptr [rax+48], r11\n\t"
    "cmp dword ptr [rip+v3x_sh_avx], 0\n\t"
    "jle 1f\n\t"
    RT_YS("rax", "vmovups")
    "lea rax, [rip+g_rt_exit]\n\t"
    RT_YL("rax", "vmovups")
    "jmp 2f\n"
    "1:\n\t"
    RT_XS("rax", "movups")
    "lea rax, [rip+g_rt_exit]\n\t"
    RT_XL("rax", "movups")
    "2:\n\t"
    "mov rcx, qword ptr [rax+8]\n\t" "mov rdx, qword ptr [rax+16]\n\t" "mov r8, qword ptr [rax+24]\n\t"
    "mov r9, qword ptr [rax+32]\n\t" "mov r10, qword ptr [rax+40]\n\t" "mov r11, qword ptr [rax+48]\n\t"
    "mov rax, qword ptr [rax]\n\t"
    "ret\n"
    ".att_syntax prefix\n");
static void (*volatile preg_target)(void) = reg_target;

/* Callbacks that wreck every volatile register (C code may use any of them, CRT memcpy uses ymm). */
static const float g_junk = -1234.5f;
static void clobber_regs(void)
{
    __asm__ volatile("mov $0xdead1, %%rcx\n\tmov $0xdead2, %%rdx\n\tmov $0xdead3, %%r8\n\tmov $0xdead4, %%r9\n\t"
                     "mov $0xdead5, %%r10\n\tmov $0xdead6, %%r11\n\t"
                     ::: "rcx", "rdx", "r8", "r9", "r10", "r11");
    if (v3x_sh_avx > 0)
        __asm__ volatile("vbroadcastss %0, %%ymm0\n\tvbroadcastss %0, %%ymm1\n\tvbroadcastss %0, %%ymm2\n\t"
                         "vbroadcastss %0, %%ymm3\n\tvbroadcastss %0, %%ymm4\n\tvbroadcastss %0, %%ymm5\n\t"
                         :: "m"(g_junk) : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5");
    else
        __asm__ volatile("movss %0, %%xmm0\n\tshufps $0, %%xmm0, %%xmm0\n\tmovaps %%xmm0, %%xmm1\n\t"
                         "movaps %%xmm0, %%xmm2\n\tmovaps %%xmm0, %%xmm3\n\tmovaps %%xmm0, %%xmm4\n\tmovaps %%xmm0, %%xmm5\n\t"
                         :: "m"(g_junk) : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5");
}
static volatile LONG g_rg_calls;
static void rg_cb(V3X_HookCall *c) { (void)c; InterlockedIncrement(&g_rg_calls); clobber_regs(); }
static int g_rg_setret;
static void rg_post_ret(V3X_HookCall *c)
{
    InterlockedIncrement(&g_rg_calls); clobber_regs();
    if (g_rg_setret) { c->ret = 0x5151; memset(c->xmm_ret.b, 0x61, 16); }
}
static void rg_pre_skip(V3X_HookCall *c) { InterlockedIncrement(&g_rg_calls); clobber_regs(); c->skip_original = 1; c->ret = 777; }

static void rg_fill(Regs *r, uint64_t seed)
{
    memset(r, 0, sizeof *r);
    for (int i = 0; i < 7; i++) r->gp[i] = seed * 0x1000 + 0x11 * (uint64_t)(i + 1);
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 32; j++) r->y[i][j] = (uint8_t)(seed * 16 + i * 32 + j + 1);
}
/* Compares GP regs (mask of gp indices) and ymm 0-5 (low 16 bytes; all 32 with AVX). */
static int rg_same(const Regs *a, const Regs *b, unsigned gpmask, unsigned ymask, int *what)
{
    for (int i = 0; i < 7; i++) if ((gpmask >> i & 1) && a->gp[i] != b->gp[i]) { *what = i; return 0; }
    size_t n = v3x_sh_avx > 0 ? 32 : 16;
    for (int i = 0; i < 6; i++) if ((ymask >> i & 1) && memcmp(a->y[i], b->y[i], n)) { *what = 100 + i; return 0; }
    return 1;
}

/* ---- threads */
static Rec g_thr;
static volatile LONG g_thr_bad;
static DWORD WINAPI thr(void *arg)
{
    ll base = (ll)(uintptr_t)arg;
    for (int i = 0; i < 20000; i++) {
        ll a = base + i;
        if (p5(a, 1, 2, 3, 4) != a + 10 + 200 + 3000 + 40000) InterlockedIncrement(&g_thr_bad);
    }
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(MH_Initialize() == MH_OK, "MH_Initialize");

    /* unwind info of the thunk itself */
    DWORD64 ib = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)v3x_safe_thunk_address() + 8, &ib, NULL);
    CHECK(rf && (void *)(ib + rf->BeginAddress) == v3x_safe_thunk_address(), "thunk has a .pdata unwind entry");

    /* bad args */
    CHECK(v3x_safe_hook_install(NULL, rec_pre, NULL, NULL) == -1, "NULL target refused");
    CHECK(v3x_safe_hook_install((void *)f0, NULL, NULL, NULL) == -1, "no pre and no post refused");

    Rec r;
    /* 0..8 args, observe only: result unchanged, args captured */
#define OBS(tgt, call, expect, nargs, ...)                                                               \
    do {                                                                                                 \
        memset(&r, 0, sizeof r);                                                                         \
        CHECK(hook((void *)tgt, rec_pre, rec_post, &r) == 0, "hook " #tgt);                              \
        ll got = call;                                                                                   \
        ll want[] = {0, ##__VA_ARGS__};                                                                  \
        int args_ok = 1;                                                                                 \
        for (int i = 0; i < nargs; i++) args_ok &= r.last_pre.args[i] == (uint64_t)want[i + 1];          \
        CHECK(got == (expect) && r.pre == 1 && r.post == 1 && args_ok && r.last_post.ret == (uint64_t)(expect) && \
                  r.last_pre.target == (void *)tgt && r.last_pre.user == &r && r.last_pre.original,     \
              #tgt " %d args: ret %lld (want %lld), pre %ld post %ld, args %s", nargs, got, (ll)(expect),  \
              r.pre, r.post, args_ok ? "ok" : "WRONG");                                                  \
        unhook((void *)tgt);                                                                             \
    } while (0)
    OBS(f0, p0(), 4242, 0);
    OBS(f1, p1(7), 22, 1, 7);
    OBS(f2, p2(7, 11), 76, 2, 7, 11);
    OBS(f3, p3(1, 2, 3), 321, 3, 1, 2, 3);
    OBS(f4, p4(1, 2, 3, 4), 4321, 4, 1, 2, 3, 4);
    OBS(f5, p5(1, 2, 3, 4, 5), 54321, 5, 1, 2, 3, 4, 5);
    OBS(f6, p6(1, 2, 3, 4, 5, 6), 654321, 6, 1, 2, 3, 4, 5, 6);
    OBS(f7, p7(1, 2, 3, 4, 5, 6, 7), 7654321, 7, 1, 2, 3, 4, 5, 6, 7);
    OBS(f8, p8(1, 2, 3, 4, 5, 6, 7, 8), 87654321, 8, 1, 2, 3, 4, 5, 6, 7, 8);
    {
        ll e = 0; for (int i = 1; i <= 16; i++) e += (ll)i * (100 + i);
        OBS(f16, p16(101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116), e, 16,
            101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116);
    }
    /* the return address the callback sees is inside main */
    {
        memset(&r, 0, sizeof r);
        hook((void *)f1, rec_pre, NULL, &r);
        p1(1);
        DWORD64 b1 = 0, b2 = 0;
        PRUNTIME_FUNCTION a = RtlLookupFunctionEntry((DWORD64)r.last_pre.return_address, &b1, NULL);
        PRUNTIME_FUNCTION m = RtlLookupFunctionEntry((DWORD64)(void *)main, &b2, NULL);
        CHECK(a && a == m, "return_address is in the caller (main)");
        unhook((void *)f1);
    }

    /* floats */
    memset(&r, 0, sizeof r);
    CHECK(hook((void *)fd4, rec_pre, rec_post, &r) == 0, "hook fd4");
    double d = pd4(1.5, 2.25, 3.0, -4.0);
    CHECK(d == 1.5 + 4.5 + 12.0 - 32.0 && r.last_pre.xmm[0].d == 1.5 && r.last_pre.xmm[3].d == -4.0 &&
              r.last_post.xmm_ret.d == d, "double x4: %g, xmm args captured, xmm_ret %g", d, r.last_post.xmm_ret.d);
    unhook((void *)fd4);
    CHECK(hook((void *)fd4, pre_xmm, NULL, NULL) == 0, "hook fd4 (pre changes xmm1)");
    d = pd4(1.0, 2.0, 3.0, 4.0);
    CHECK(d == 1.0 + 200.0 + 12.0 + 32.0, "pre xmm1 = 100 forwarded: %g", d);
    unhook((void *)fd4);
    CHECK(hook((void *)ff2, pre_clobber_fp, post_fret, NULL) == 0, "hook ff2 (callbacks use fp math)");
    float f = pf2(3.0f, 4.0f);
    CHECK(f == (3.0f * 4.0f + 0.5f) * 2, "float args survive an fp-using pre; post doubles float ret: %g", f);
    unhook((void *)ff2);
    memset(&r, 0, sizeof r);
    CHECK(hook((void *)fmix, rec_pre, rec_post, &r) == 0, "hook fmix");
    d = pmix(1, 2.0, 3, 4.0f, 5.0, 6);   /* xmm1, xmm3; arg 5 (double) and 6 (int) on the stack */
    double dw = 1 + 20.0 + 300 + 4000.0 + 50000.0 + 600000.0;
    double s5; memcpy(&s5, &r.last_pre.args[4], 8);
    CHECK(d == dw && r.last_pre.args[0] == 1 && r.last_pre.xmm[1].d == 2.0 && r.last_pre.args[2] == 3 &&
              r.last_pre.xmm[3].f == 4.0f && s5 == 5.0 && r.last_pre.args[5] == 6,
          "mixed int/float/stack-double: %g (want %g)", d, dw);
    unhook((void *)fmix);

    /* modify / skip / post */
    CHECK(hook((void *)f2, pre_double_a0, NULL, NULL) == 0, "hook f2 (pre doubles a0)");
    CHECK(p2(7, 11) == 14 * 3 + 55, "pre-modified register arg forwarded");
    unhook((void *)f2);
    CHECK(hook((void *)f8, pre_set_stack, post_add, NULL) == 0, "hook f8 (pre sets arg 8, post +1000)");
    CHECK(p8(1, 2, 3, 4, 5, 6, 7, 8) == 97654321 + 1000, "pre-modified stack arg forwarded, post changed ret");
    unhook((void *)f8);
    sink_ = 0;
    CHECK(hook((void *)f3, pre_skip, NULL, NULL) == 0, "hook f3 (skip)");
    ll before = sink_;
    CHECK(p3(1, 2, 3) == 777 && sink_ == before, "skip_original: original not run, replacement ret returned");
    unhook((void *)f3);
    ll out = 0;
    memset(&r, 0, sizeof r);
    CHECK(hook((void *)fvoid, rec_pre, rec_post, &r) == 0, "hook fvoid");
    pvoid(&out, 6);
    CHECK(out == 42 && r.pre == 1 && r.post == 1, "void fn with out-pointer");
    unhook((void *)fvoid);
    memset(&r, 0, sizeof r);
    CHECK(hook((void *)fbig, rec_pre, NULL, &r) == 0, "hook fbig");
    Big b = pbig(10, 20);
    CHECK(b.a == 10 && b.b == 20 && b.c == 30 && r.last_pre.args[1] == 10 && r.last_pre.args[2] == 20,
          "struct return via hidden pointer (args shifted by one)");
    unhook((void *)fbig);

    /* scratch: zeroed per call, carried from pre to post (also across nested calls) */
    CHECK(hook((void *)f1, pre_scratch, post_scratch, NULL) == 0, "hook f1 (scratch)");
    for (int i = 0; i < 5; i++) p1(i);
    CHECK(g_scratch_ok == 5 && g_scratch_zero == 5, "scratch zeroed at entry and kept pre -> post (%ld/%ld)", g_scratch_ok, g_scratch_zero);
    unhook((void *)f1);

    /* recursion */
    memset(&r, 0, sizeof r);
    CHECK(hook((void *)frec, rec_pre, rec_post, &r) == 0, "hook frec");
    CHECK(prec(50) == 1275 && r.pre == 51 && r.post == 51, "recursion: 51 nested calls (pre %ld post %ld)", r.pre, r.post);
    unhook((void *)frec);

    /* callee-saved registers clobbered by the original are restored by it; caller's survive */
    CHECK(hook((void *)fnv, rec_pre, rec_post, &r) == 0, "hook fnv");
    {
        ll v1 = 0, v2 = 0;
        __asm__ volatile(
            "mov $0xAAAA, %%rbx\n\tmov $0xBBBB, %%r12\n\t"
            "mov $5, %%rcx\n\tsub $32, %%rsp\n\tcall *%2\n\tadd $32, %%rsp\n\t"
            "mov %%rbx, %0\n\tmov %%r12, %1"
            : "=r"(v1), "=r"(v2)
            : "r"(pnv)
            : "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "rbx", "r12", "memory", "xmm0", "xmm1", "xmm2",
              "xmm3", "xmm4", "xmm5");
        CHECK(v1 == 0xAAAA && v2 == 0xBBBB, "non-volatile rbx/r12 preserved across the hooked call");
    }
    unhook((void *)fnv);

    /* stack walk from inside the original passes through the thunk to the caller */
    g_nframes = 0;
    pwalk_caller();
    int i_plain = frame_in((void *)walk_caller);
    CHECK(i_plain >= 0, "unhooked stack walk finds walk_caller (frame %d)", i_plain);
    CHECK(hook((void *)fwalk, rec_pre, NULL, &r) == 0, "hook fwalk");
    g_nframes = 0;
    pwalk_caller();
    int i_th = frame_in(v3x_safe_thunk_address()), i_c = frame_in((void *)walk_caller), i_m = frame_in((void *)main);
    CHECK(i_th >= 0 && i_c > i_th && i_m > i_c, "hooked stack walk: thunk at %d, walk_caller at %d, main at %d", i_th, i_c, i_m);
    unhook((void *)fwalk);

    /* registers: entry state reaches the original, exit state reaches the caller (2 subscribers) */
    {
        Regs in, out; int w = -1;
        static int own1, own2;
        rg_fill(&in, 1); rg_fill(&g_rt_exit, 2);
        memset(&out, 0, sizeof out);
        reg_caller((void *)preg_target, &in, &out);
        CHECK(rg_same(&g_rt_entry, &in, 0x5f, 0x3f, &w) && rg_same(&out, &g_rt_exit, 0x7f, 0x3f, &w),
              "register test harness, unhooked (avx %d, diff %d)", v3x_sh_avx, w);
        CHECK(v3x_hook_add_safe(&own1, (void *)reg_target, rg_cb, rg_cb, NULL, NULL) == 0 &&
              v3x_hook_add_safe(&own2, (void *)reg_target, rg_cb, rg_post_ret, NULL, NULL) == 0,
              "hook_safe reg_target with 2 subscribers");
        g_rg_calls = 0; g_rg_setret = 0; w = -1;
        memset(&g_rt_entry, 0, sizeof g_rt_entry); memset(&out, 0, sizeof out);
        reg_caller((void *)preg_target, &in, &out);
        CHECK(g_rg_calls == 4 && rg_same(&g_rt_entry, &in, 0x5f, 0x3f, &w),
              "original sees caller's rax/rcx/rdx/r8/r9/r11/ymm0-5 after clobbering pre callbacks (calls %ld, diff %d)",
              g_rg_calls, w);
        w = -1;
        CHECK(rg_same(&out, &g_rt_exit, 0x7f, 0x3f, &w),
              "caller sees rax/rcx/rdx/r8-r11/xmm4-5/full ymm0-5 as the original left them (diff %d)", w);
        g_rg_setret = 1; w = -1;
        memset(&out, 0, sizeof out);
        reg_caller((void *)preg_target, &in, &out);
        uint8_t x61[16]; memset(x61, 0x61, 16);
        int ok = rg_same(&out, &g_rt_exit, 0x7e, 0x3e, &w) && out.gp[0] == 0x5151 && !memcmp(out.y[0], x61, 16) &&
                 (v3x_sh_avx <= 0 || !memcmp(out.y[0] + 16, g_rt_exit.y[0] + 16, 16));
        CHECK(ok, "post sets ret/xmm_ret: only rax and low xmm0 change (diff %d)", w);
        g_rg_setret = 0;
        CHECK(v3x_hook_remove(&own2, (void *)reg_target, NULL) == 0 &&
              v3x_hook_add_safe(&own2, (void *)reg_target, rg_pre_skip, rg_cb, NULL, NULL) == 0, "resubscribe with skip");
        g_rg_calls = 0; w = -1;
        memset(&g_rt_entry, 0, sizeof g_rt_entry); memset(&out, 0, sizeof out);
        reg_caller((void *)preg_target, &in, &out);
        CHECK(g_rg_calls == 4 && g_rt_entry.gp[1] == 0 && out.gp[0] == 777 && rg_same(&out, &in, 0x5e, 0x3e, &w),
              "skip_original: rax = ret, other registers = caller's entry values (calls %ld, diff %d)", g_rg_calls, w);
        CHECK(v3x_hook_remove(&own1, (void *)reg_target, NULL) == 0 && v3x_hook_remove(&own2, (void *)reg_target, NULL) == 0,
              "unsubscribe reg_target");
    }

    /* threads */
    memset(&g_thr, 0, sizeof g_thr);
    CHECK(hook((void *)f5, rec_pre, NULL, &g_thr) == 0, "hook f5 for threads");
    HANDLE th[4];
    for (int i = 0; i < 4; i++) th[i] = CreateThread(NULL, 0, thr, (void *)(uintptr_t)(i * 1000000), 0, NULL);
    WaitForMultipleObjects(4, th, TRUE, INFINITE);
    CHECK(g_thr_bad == 0 && g_thr.pre == 80000, "4 threads x 20000 calls: %ld wrong, %ld pre calls", g_thr_bad, g_thr.pre);
    unhook((void *)f5);
    CHECK(p5(1, 2, 3, 4, 5) == 54321, "unhooked f5 works");

    /* hook twice -> MH_ERROR_ALREADY_CREATED, slot reused afterwards */
    unsigned used0 = g_sh_used;
    CHECK(hook((void *)f1, rec_pre, NULL, &r) == 0, "hook f1");
    CHECK(v3x_safe_hook_install((void *)f1, rec_pre, NULL, &r) == MH_ERROR_ALREADY_CREATED && g_sh_used == used0 + 1,
          "second hook on the same target refused, its slot returned");
    unhook((void *)f1);
    CHECK(p1(2) == 7, "f1 after unhook");

    printf("%d checks, %d failed\n%s\n", checks, fails, fails ? "FAILED" : "ALL OK");
    return fails ? 1 : 0;
}
