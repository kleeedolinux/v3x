/* V3X safe hooks (API v4 api->hook_safe): a generic pass-through thunk, so plugin authors can
 * observe or wrap an engine function without writing a detour with the right prototype.
 *
 * Why: a hand-written detour must forward exactly the arguments the engine function takes. A
 * 4-argument detour on a 5-argument function hands the original garbage for argument 5 (the
 * sim_opt crash: 0xe02c90 reads a 5th stack argument). This thunk forwards everything a Win64
 * function can receive in registers or in the first 12 stack slots, whatever the prototype.
 *
 * Per hook, one SafeCtx (data, a read-write page that is never executable) and one 16-byte stub
 * (code, a page that is read-execute only: W^X, docs/adr/0003-hook-lifetime.md):
 *     mov r10, <ctx>           ; r10 is volatile and never carries an argument in the Win64 ABI
 *     jmp [r10+0x28]           ; ctx->jmp = v3x_safe_thunk (hook_safe) or the plugin's detour (raw hook)
 * All stubs of a pool page are generated while the page is still private and read-write, then the
 * page is made PAGE_EXECUTE_READ before any of them is handed out; nothing ever writes code there
 * again. MinHook redirects the target to the stub; the stub does not touch rsp, so stack walks treat
 * it as a leaf. v3x_safe_thunk (below, in .text with SEH unwind info: one `sub rsp, 0x328`):
 *   1. saves rcx, rdx, r8, r9, the 12 caller stack slots [rsp+0x28..] and xmm0-3 into a
 *      V3X_HookCall on its own frame, plus user/target/original/return address; saves the rest of
 *      the entry state (rax, r11, full ymm0-5 with AVX, else xmm0-5) in the register save area;
 *   2. calls pre(call) if set;
 *   3. unless call->skip_original: reloads all of it (the outgoing stack slots too; the args as pre
 *      left them; rax, r11, xmm4-5 and upper ymm0-5 as the caller passed them) and calls the MinHook
 *      trampoline; stores rax and xmm0 into call->ret / call->xmm_ret. With skip_original the same
 *      entry state is reloaded instead (as if the original had changed nothing but rax/xmm0);
 *   4. saves rax, rcx, rdx, r8-r11 and ymm0-5 (xmm0-5 without AVX) as the original left them,
 *      calls post(call) if set, restores all of them and returns call->ret in rax and
 *      call->xmm_ret in the low 128 bits of xmm0 (upper ymm0 as the original left it).
 * Register rule: the caller sees every register exactly as the wrapped function left it; only rax
 * and xmm0 differ, and only if a callback changed call->ret / call->xmm_ret. The game is built with
 * whole-program optimisation, so its callers may keep values in r8-r11, xmm4-5 and upper ymm halves
 * across a call to a function known not to clobber them. The original also sees the caller's
 * registers, except r10 (= ctx, set by the stub). Not covered: upper ymm6-15, AVX-512 state.
 * Non-volatile registers (rbx, rbp, rdi, rsi, r12-r15, xmm6-15) are never touched by the thunk and
 * are preserved by the C callbacks, so the original and the caller see them unchanged.
 *
 * Frame of v3x_safe_thunk (rsp after the prologue `sub rsp, 0x328`; 16-byte aligned there):
 *   +0x000 .. +0x020  home space for the calls we make
 *   +0x020 .. +0x080  outgoing stack args 5..16 for the original
 *   +0x080 .. +0x190  V3X_HookCall (0x110 bytes, offsets in include/v3x.h)
 *   +0x190 .. +0x220  ShExt: per-call state of the multi-subscriber dispatcher (below)
 *   +0x220 .. +0x260  GP save: rax, rcx, rdx, r8, r9, r10, r11, pad (entry state, then exit state)
 *   +0x260 .. +0x320  ymm0-5 save, 32 bytes each (only the low 16 used without AVX)
 *   +0x320            saved ctx (r10)
 *   +0x328            return address; +0x330 .. +0x350 caller's home space; +0x350 caller arg 5
 *
 * This file has no dependency on core.c state, so test/hook_safe_test.c includes it directly. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "MinHook.h"
#include "../include/v3x.h"

typedef struct SafeCtx {
    void *original;        /* +0x00 MinHook trampoline (never freed, docs/adr/0003-hook-lifetime.md) */
    v3x_hook_fn pre;       /* +0x08 */
    v3x_hook_fn post;      /* +0x10 */
    void *user;            /* +0x18 */
    void *target;          /* +0x20 */
    void *jmp;             /* +0x28 where the stub jumps: v3x_safe_thunk or a raw detour (atomic store) */
    uint8_t *stub;         /* +0x30 this ctx's 16-byte stub in the read-execute code page */
    uint64_t pad;          /* +0x38 */
    /* 64 bytes per ctx */
} SafeCtx;

_Static_assert(sizeof(SafeCtx) == 64, "SafeCtx layout");
_Static_assert(offsetof(SafeCtx, jmp) == 0x28, "SafeCtx layout (the stub's jmp [r10+0x28])");
_Static_assert(sizeof(V3X_HookCall) == 0x110, "V3X_HookCall layout (thunk offsets)");
_Static_assert(offsetof(V3X_HookCall, xmm) == 0x80, "V3X_HookCall layout");
_Static_assert(offsetof(V3X_HookCall, ret) == 0xc0, "V3X_HookCall layout");
_Static_assert(offsetof(V3X_HookCall, skip_original) == 0xc8, "V3X_HookCall layout");
_Static_assert(offsetof(V3X_HookCall, xmm_ret) == 0xd0, "V3X_HookCall layout");
_Static_assert(offsetof(V3X_HookCall, user) == 0xe0, "V3X_HookCall layout");
_Static_assert(offsetof(V3X_HookCall, return_address) == 0xf8, "V3X_HookCall layout");
_Static_assert(offsetof(V3X_HookCall, scratch) == 0x100, "V3X_HookCall layout");

void v3x_safe_thunk(void);

/* Stack-arg copies: caller slot i (i = 0..11) is at [rsp+0x350+8i]; HookCall.args[4+i] at
 * [rsp+0xa0+8i]; outgoing slot at [rsp+0x20+8i]. */
#define SH_IN(i)  "mov rax, qword ptr [rsp+0x350+8*" #i "]\n\tmov qword ptr [rsp+0xa0+8*" #i "], rax\n\t"
#define SH_OUT(i) "mov rax, qword ptr [rsp+0xa0+8*" #i "]\n\tmov qword ptr [rsp+0x20+8*" #i "], rax\n\t"

/* 1 if the OS saves the AVX state (the thunk then saves/restores full ymm0-5), 0 if not, -1 before
 * sh_init(). Set under g_sh_lock before the first hook is enabled; read by the thunk. */
volatile int v3x_sh_avx = -1;

/* Save ymm0-5 (xmm0-5 without AVX) into the vector save area. L = unique label suffix. */
#define SH_VSAVE(L)                                                                                 \
    "cmp dword ptr [rip+v3x_sh_avx], 0\n\t"                                                         \
    "jle .Lv3x_vs_sse" L "\n\t"                                                                     \
    "vmovups ymmword ptr [rsp+0x260], ymm0\n\t" "vmovups ymmword ptr [rsp+0x280], ymm1\n\t"         \
    "vmovups ymmword ptr [rsp+0x2a0], ymm2\n\t" "vmovups ymmword ptr [rsp+0x2c0], ymm3\n\t"         \
    "vmovups ymmword ptr [rsp+0x2e0], ymm4\n\t" "vmovups ymmword ptr [rsp+0x300], ymm5\n\t"         \
    "jmp .Lv3x_vs_done" L "\n"                                                                      \
    ".Lv3x_vs_sse" L ":\n\t"                                                                        \
    "movups xmmword ptr [rsp+0x260], xmm0\n\t" "movups xmmword ptr [rsp+0x280], xmm1\n\t"           \
    "movups xmmword ptr [rsp+0x2a0], xmm2\n\t" "movups xmmword ptr [rsp+0x2c0], xmm3\n\t"           \
    "movups xmmword ptr [rsp+0x2e0], xmm4\n\t" "movups xmmword ptr [rsp+0x300], xmm5\n"             \
    ".Lv3x_vs_done" L ":\n\t"
/* Reload the entry state for the original: outgoing stack args, ymm0-5 with low xmm0-3 =
 * call->xmm[] as pre left them, rcx/rdx/r8/r9 = call->args[0..3], r11 and rax as the caller passed
 * them, r10 = ctx. */
#define SH_ENTRY(L)                                                                                 \
    SH_OUT(0) SH_OUT(1) SH_OUT(2) SH_OUT(3) SH_OUT(4) SH_OUT(5)                                     \
    SH_OUT(6) SH_OUT(7) SH_OUT(8) SH_OUT(9) SH_OUT(10) SH_OUT(11)                                   \
    "cmp dword ptr [rip+v3x_sh_avx], 0\n\t"                                                         \
    "jle .Lv3x_en_sse" L "\n\t"                                                                     \
    "vmovups ymm0, ymmword ptr [rsp+0x260]\n\t" "vmovups ymm1, ymmword ptr [rsp+0x280]\n\t"         \
    "vmovups ymm2, ymmword ptr [rsp+0x2a0]\n\t" "vmovups ymm3, ymmword ptr [rsp+0x2c0]\n\t"         \
    "vmovups ymm4, ymmword ptr [rsp+0x2e0]\n\t" "vmovups ymm5, ymmword ptr [rsp+0x300]\n\t"         \
    "vinsertf128 ymm0, ymm0, xmmword ptr [rsp+0x100], 0\n\t"                                        \
    "vinsertf128 ymm1, ymm1, xmmword ptr [rsp+0x110], 0\n\t"                                        \
    "vinsertf128 ymm2, ymm2, xmmword ptr [rsp+0x120], 0\n\t"                                        \
    "vinsertf128 ymm3, ymm3, xmmword ptr [rsp+0x130], 0\n\t"                                        \
    "jmp .Lv3x_en_done" L "\n"                                                                      \
    ".Lv3x_en_sse" L ":\n\t"                                                                        \
    "movups xmm0, xmmword ptr [rsp+0x100]\n\t" "movups xmm1, xmmword ptr [rsp+0x110]\n\t"           \
    "movups xmm2, xmmword ptr [rsp+0x120]\n\t" "movups xmm3, xmmword ptr [rsp+0x130]\n\t"           \
    "movups xmm4, xmmword ptr [rsp+0x2e0]\n\t" "movups xmm5, xmmword ptr [rsp+0x300]\n"             \
    ".Lv3x_en_done" L ":\n\t"                                                                       \
    "mov rcx, qword ptr [rsp+0x80]\n\t"                                                             \
    "mov rdx, qword ptr [rsp+0x88]\n\t"                                                             \
    "mov r8, qword ptr [rsp+0x90]\n\t"                                                              \
    "mov r9, qword ptr [rsp+0x98]\n\t"                                                              \
    "mov r11, qword ptr [rsp+0x250]\n\t"                                                            \
    "mov rax, qword ptr [rsp+0x220]\n\t"                                                            \
    "mov r10, qword ptr [rsp+0x320]\n\t"

__asm__(
    ".intel_syntax noprefix\n"
    ".text\n"
    ".globl v3x_safe_thunk\n"
    ".def v3x_safe_thunk; .scl 2; .type 32; .endef\n"
    ".p2align 4\n"
    "v3x_safe_thunk:\n"
    ".seh_proc v3x_safe_thunk\n\t"
    "sub rsp, 0x328\n\t"
    ".seh_stackalloc 0x328\n\t"
    ".seh_endprologue\n\t"
    "mov qword ptr [rsp+0x320], r10\n\t"
    "mov qword ptr [rsp+0x220], rax\n\t"          /* entry rax, r11: not arguments, restored anyway */
    "mov qword ptr [rsp+0x250], r11\n\t"
    "mov qword ptr [rsp+0x80], rcx\n\t"
    "mov qword ptr [rsp+0x88], rdx\n\t"
    "mov qword ptr [rsp+0x90], r8\n\t"
    "mov qword ptr [rsp+0x98], r9\n\t"
    SH_IN(0) SH_IN(1) SH_IN(2) SH_IN(3) SH_IN(4) SH_IN(5)
    SH_IN(6) SH_IN(7) SH_IN(8) SH_IN(9) SH_IN(10) SH_IN(11)
    "movups xmmword ptr [rsp+0x100], xmm0\n\t"
    "movups xmmword ptr [rsp+0x110], xmm1\n\t"
    "movups xmmword ptr [rsp+0x120], xmm2\n\t"
    "movups xmmword ptr [rsp+0x130], xmm3\n\t"
    SH_VSAVE("1")                                 /* entry ymm0-5 */
    "xor eax, eax\n\t"
    "mov qword ptr [rsp+0x140], rax\n\t"          /* ret */
    "mov qword ptr [rsp+0x148], rax\n\t"          /* skip_original, reserved */
    "mov qword ptr [rsp+0x150], rax\n\t"          /* xmm_ret */
    "mov qword ptr [rsp+0x158], rax\n\t"
    "mov qword ptr [rsp+0x180], rax\n\t"          /* scratch[0..1] */
    "mov qword ptr [rsp+0x188], rax\n\t"
    "mov rax, qword ptr [r10+0x18]\n\t"
    "mov qword ptr [rsp+0x160], rax\n\t"          /* user */
    "mov rax, qword ptr [r10+0x20]\n\t"
    "mov qword ptr [rsp+0x168], rax\n\t"          /* target */
    "mov rax, qword ptr [r10]\n\t"
    "mov qword ptr [rsp+0x170], rax\n\t"          /* original */
    "mov rax, qword ptr [rsp+0x328]\n\t"
    "mov qword ptr [rsp+0x178], rax\n\t"          /* return address */
    "mov rax, qword ptr [r10+0x08]\n\t"           /* pre */
    "test rax, rax\n\t"
    "jz .Lv3x_sh_nopre\n\t"
    "lea rcx, [rsp+0x80]\n\t"
    "call rax\n"
    ".Lv3x_sh_nopre:\n\t"
    "cmp dword ptr [rsp+0x148], 0\n\t"
    "jne .Lv3x_sh_skip\n\t"
    SH_ENTRY("2")
    "call qword ptr [r10]\n\t"                    /* ctx->original, never the user-visible copy */
    "mov qword ptr [rsp+0x140], rax\n\t"
    "movups xmmword ptr [rsp+0x150], xmm0\n\t"
    "jmp .Lv3x_sh_save\n"
    ".Lv3x_sh_skip:\n\t"
    SH_ENTRY("3")                                 /* skipped: exit state = entry state */
    "\n"
    ".Lv3x_sh_save:\n\t"
    "mov qword ptr [rsp+0x220], rax\n\t"          /* exit state as the original left it */
    "mov qword ptr [rsp+0x228], rcx\n\t"
    "mov qword ptr [rsp+0x230], rdx\n\t"
    "mov qword ptr [rsp+0x238], r8\n\t"
    "mov qword ptr [rsp+0x240], r9\n\t"
    "mov qword ptr [rsp+0x248], r10\n\t"
    "mov qword ptr [rsp+0x250], r11\n\t"
    SH_VSAVE("4")
    "mov r10, qword ptr [rsp+0x320]\n\t"
    "mov rax, qword ptr [r10+0x10]\n\t"           /* post */
    "test rax, rax\n\t"
    "jz .Lv3x_sh_nopost\n\t"
    "lea rcx, [rsp+0x80]\n\t"
    "call rax\n"
    ".Lv3x_sh_nopost:\n\t"
    "mov rax, qword ptr [rsp+0x150]\n\t"          /* low 128 bits of ymm0 = call->xmm_ret */
    "mov qword ptr [rsp+0x260], rax\n\t"
    "mov rax, qword ptr [rsp+0x158]\n\t"
    "mov qword ptr [rsp+0x268], rax\n\t"
    "cmp dword ptr [rip+v3x_sh_avx], 0\n\t"
    "jle .Lv3x_sh_rsse\n\t"
    "vmovups ymm0, ymmword ptr [rsp+0x260]\n\t" "vmovups ymm1, ymmword ptr [rsp+0x280]\n\t"
    "vmovups ymm2, ymmword ptr [rsp+0x2a0]\n\t" "vmovups ymm3, ymmword ptr [rsp+0x2c0]\n\t"
    "vmovups ymm4, ymmword ptr [rsp+0x2e0]\n\t" "vmovups ymm5, ymmword ptr [rsp+0x300]\n\t"
    "jmp .Lv3x_sh_rdone\n"
    ".Lv3x_sh_rsse:\n\t"
    "movups xmm0, xmmword ptr [rsp+0x260]\n\t" "movups xmm1, xmmword ptr [rsp+0x280]\n\t"
    "movups xmm2, xmmword ptr [rsp+0x2a0]\n\t" "movups xmm3, xmmword ptr [rsp+0x2c0]\n\t"
    "movups xmm4, xmmword ptr [rsp+0x2e0]\n\t" "movups xmm5, xmmword ptr [rsp+0x300]\n"
    ".Lv3x_sh_rdone:\n\t"
    "mov rcx, qword ptr [rsp+0x228]\n\t"
    "mov rdx, qword ptr [rsp+0x230]\n\t"
    "mov r8, qword ptr [rsp+0x238]\n\t"
    "mov r9, qword ptr [rsp+0x240]\n\t"
    "mov r10, qword ptr [rsp+0x248]\n\t"
    "mov r11, qword ptr [rsp+0x250]\n\t"
    "mov rax, qword ptr [rsp+0x140]\n\t"          /* call->ret */
    "add rsp, 0x328\n\t"
    "ret\n"
    ".seh_endproc\n"
    ".att_syntax prefix\n");

/* AVX state enabled by the OS: CPUID.1:ECX OSXSAVE (bit 27) + AVX (28), XCR0 bits 1-2 (SSE, AVX). */
static void sh_init(void)
{
    if (v3x_sh_avx >= 0) return;
    unsigned a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    int avx = 0;
    if ((c & (1u << 27)) && (c & (1u << 28))) {
        unsigned lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        avx = (lo & 6) == 6;
    }
    (void)a; (void)b; (void)d;
    v3x_sh_avx = avx;
}

/* ------------------------------------------------------------------ ctx pool */

#define SH_PAGE 65536                          /* one VirtualAlloc granule = 1024 ctx records */
#define SH_MAX_PAGES 4
#define SH_STUB 16                             /* bytes per stub */
#define SH_PER (SH_PAGE / sizeof(SafeCtx))
static SafeCtx *g_sh_pages[SH_MAX_PAGES];      /* data: PAGE_READWRITE, never executable */
static uint8_t *g_sh_code[SH_MAX_PAGES];       /* stubs: PAGE_EXECUTE_READ once generated, never written again */
static unsigned g_sh_used;                     /* ctx slots handed out across all pages */
static SRWLOCK g_sh_lock = SRWLOCK_INIT;

/* Allocates data page `page` and its code page: every stub i is generated up front
 * ("mov r10, &ctx[i]; jmp [r10+0x28]"), then the code page becomes read-execute. 0 / -1. */
static int sh_new_page(unsigned page)
{
    SafeCtx *d = (SafeCtx *)VirtualAlloc(NULL, SH_PAGE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint8_t *code = (uint8_t *)VirtualAlloc(NULL, SH_PER * SH_STUB, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD old;
    if (!d || !code) goto fail;
    for (unsigned i = 0; i < SH_PER; i++) {
        uint8_t *p = code + (size_t)i * SH_STUB;
        uint64_t ctx = (uint64_t)(uintptr_t)&d[i];
        p[0] = 0x49; p[1] = 0xBA; memcpy(p + 2, &ctx, 8);                         /* mov r10, imm64 */
        p[10] = 0x41; p[11] = 0xFF; p[12] = 0x62; p[13] = (uint8_t)offsetof(SafeCtx, jmp);   /* jmp [r10+0x28] */
        p[14] = p[15] = 0xCC;                                                    /* int3 padding */
        d[i].stub = p;
    }
    if (!VirtualProtect(code, SH_PER * SH_STUB, PAGE_EXECUTE_READ, &old)) goto fail;
    FlushInstructionCache(GetCurrentProcess(), code, SH_PER * SH_STUB);
    g_sh_pages[page] = d;
    g_sh_code[page] = code;
    return 0;
fail:
    if (d) VirtualFree(d, 0, MEM_RELEASE);
    if (code) VirtualFree(code, 0, MEM_RELEASE);   /* never published: nobody can execute it */
    return -1;
}

static SafeCtx *sh_alloc(void)
{
    unsigned page = g_sh_used / (unsigned)SH_PER, slot = g_sh_used % (unsigned)SH_PER;
    if (page >= SH_MAX_PAGES) return NULL;
    if (!g_sh_pages[page] && sh_new_page(page)) return NULL;
    g_sh_used++;
    return &g_sh_pages[page][slot];
}

/* Resets a ctx (keeps its stub) and points the stub at `jmp`. */
static void sh_ctx_init(SafeCtx *c, void *jmp)
{
    uint8_t *stub = c->stub;
    memset(c, 0, sizeof *c);
    c->stub = stub;
    __atomic_store_n(&c->jmp, jmp, __ATOMIC_RELEASE);
}

/* Installs the hook. Returns 0 or an MH_STATUS (MH_ERROR_MEMORY_ALLOC when the pool is full or
 * cannot be allocated), -1 on bad arguments. On failure the ctx slot is kept unused for reuse. */
int v3x_safe_hook_install(void *target, v3x_hook_fn pre, v3x_hook_fn post, void *user)
{
    if (!target || (!pre && !post)) return -1;
    AcquireSRWLockExclusive(&g_sh_lock);
    sh_init();
    SafeCtx *c = sh_alloc();
    if (!c) { ReleaseSRWLockExclusive(&g_sh_lock); return MH_ERROR_MEMORY_ALLOC; }
    sh_ctx_init(c, (void *)v3x_safe_thunk);
    c->pre = pre; c->post = post; c->user = user; c->target = target;
    MH_STATUS s = MH_CreateHook(target, c->stub, &c->original);
    if (s == MH_OK) {
        s = MH_EnableHook(target);
        if (s != MH_OK) MH_RemoveHook(target);   /* never enabled: no thread can be in it */
    }
    if (s != MH_OK) { sh_ctx_init(c, NULL); g_sh_used--; }   /* last slot handed out: give it back */
    ReleaseSRWLockExclusive(&g_sh_lock);
    return (int)s;
}

/* For tests: the thunk's code address (to check its unwind entry). */
void *v3x_safe_thunk_address(void) { return (void *)v3x_safe_thunk; }

/* ------------------------------------------------------------------ hook registry (core.c uses this)
 *
 * Several plugins may want the same engine function. MinHook allows one hook per target, so the
 * core keeps one record per target and decides here:
 *   - hook_safe: any number of subscribers (up to V3X_HOOK_MAX_SUBSCRIBERS) share ONE thunk. The
 *     record's SafeCtx calls sh_disp_pre / sh_disp_post, which run the subscribers like nested
 *     wrappers ("onion"): pre in registration order, the original, post in reverse order. Each
 *     subscriber gets its own call->user and its own call->scratch (kept in the ShExt frame area).
 *     A pre that sets skip_original stops the chain: later subscribers see nothing of this call,
 *     the original is not called, and the post callbacks of the subscribers up to and including
 *     the skipping one still run (the first one to skip wins).
 *   - raw hook (api->hook): exclusive. A target with a raw hook or with hook_safe subscribers
 *     refuses the other kind with V3X_EHOOKBUSY (busy_owner = the module holding it, NULL if it was
 *     hooked outside this registry).
 *   - remove(owner, target) removes only that owner's raw hook or subscriptions.
 * Concurrency: registration takes g_sh_lock. The subscriber list is an immutable ShList snapshot
 * published with one release store; the dispatcher loads it once per call (acquire) and keeps it in
 * the frame, so pre and post of one call always see the same subscribers. Snapshots and records are
 * never freed (a thread may still be inside the thunk).
 * Lifetime (docs/adr/0003-hook-lifetime.md, audit M2): a hook that was ever enabled is NEVER removed
 * from MinHook at runtime (MH_RemoveHook frees the trampoline another thread may be about to call).
 * Raw hooks go through the record's stub too (ctx->jmp = the detour, *original = the record's
 * trampoline), so unhook only DISABLES (original bytes restored, trampoline and stub stay valid for
 * threads in flight) and a later hook_safe or raw hook on the same target re-points ctx->jmp and
 * re-enables the same MinHook entry. MH_RemoveHook is used only on a hook that was never enabled. */

typedef struct ShSub { v3x_hook_fn pre, post; void *user; void *owner; } ShSub;
struct ShRec;
typedef struct ShList {
    struct ShRec *rec;
    int n;
    int pad;
    ShSub sub[V3X_HOOK_MAX_SUBSCRIBERS];
} ShList;
typedef struct ShRec {
    void *target;
    SafeCtx *ctx;              /* this target's stub + trampoline; allocated once, reused, never freed */
    ShList *list;              /* current subscribers (immutable snapshot) or NULL; atomic */
    void *raw_owner;           /* owner of a raw hook, NULL = none */
    int created;               /* our stub hook exists in MinHook (stays until process exit) */
    int enabled;               /* ... and is enabled */
} ShRec;
/* Per-call dispatcher state, in the thunk frame right after the V3X_HookCall (+0x190). */
typedef struct ShExt {
    ShList *list;
    int32_t entered;           /* subscribers whose pre stage ran (post runs for these) */
    int32_t pad;
    uint64_t scratch[V3X_HOOK_MAX_SUBSCRIBERS][2];
} ShExt;
_Static_assert(sizeof(ShExt) == 0x90, "ShExt must fit the thunk frame (+0x190 .. +0x220)");

#define SH_MAX_RECS 2048
static ShRec g_sh_recs[SH_MAX_RECS];
static int g_sh_nrecs;

static void sh_disp_pre(V3X_HookCall *c)
{
    ShExt *x = (ShExt *)(c + 1);
    ShList *l = __atomic_load_n(&((ShRec *)c->user)->list, __ATOMIC_ACQUIRE);
    x->list = l;
    x->entered = 0;
    c->user = NULL;
    if (!l) return;
    memset(x->scratch, 0, sizeof x->scratch[0] * (size_t)l->n);
    for (int i = 0; i < l->n; i++) {
        x->entered = i + 1;
        if (l->sub[i].pre) {
            c->user = l->sub[i].user;
            c->scratch[0] = c->scratch[1] = 0;
            l->sub[i].pre(c);
            x->scratch[i][0] = c->scratch[0];
            x->scratch[i][1] = c->scratch[1];
        }
        if (c->skip_original) break;   /* first skip wins: inner subscribers never see this call */
    }
}

static void sh_disp_post(V3X_HookCall *c)
{
    ShExt *x = (ShExt *)(c + 1);
    ShList *l = x->list;
    for (int i = x->entered - 1; i >= 0; i--) {
        if (!l->sub[i].post) continue;
        c->user = l->sub[i].user;
        c->scratch[0] = x->scratch[i][0];
        c->scratch[1] = x->scratch[i][1];
        l->sub[i].post(c);
    }
}

static ShRec *sh_find(void *target, int create)
{
    for (int i = 0; i < g_sh_nrecs; i++)
        if (g_sh_recs[i].target == target) return &g_sh_recs[i];
    if (!create || g_sh_nrecs >= SH_MAX_RECS) return NULL;
    ShRec *r = &g_sh_recs[g_sh_nrecs++];
    memset(r, 0, sizeof *r);
    r->target = target;
    return r;
}

static ShList *sh_list_copy(ShRec *r, const ShList *old)
{
    ShList *l = (ShList *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *l);
    if (!l) return NULL;
    if (old) *l = *old;
    l->rec = r;
    return l;
}

static void sh_publish(ShRec *r, ShList *l) { __atomic_store_n(&r->list, l, __ATOMIC_RELEASE); }

/* hook_safe for `owner` (non-NULL, e.g. the calling module). 0, -1 bad args, MH_ERROR_ALREADY_CREATED
 * (the same owner/pre/post/user is already subscribed), MH_ERROR_MEMORY_ALLOC (subscriber/record/ctx
 * pool full), V3X_EHOOKBUSY (raw-hooked; *busy_owner = its owner or NULL), or another MH_STATUS. */
int v3x_hook_add_safe(void *owner, void *target, v3x_hook_fn pre, v3x_hook_fn post, void *user, void **busy_owner)
{
    if (busy_owner) *busy_owner = NULL;
    if (!owner || !target || (!pre && !post)) return -1;
    int s = MH_OK, n;
    AcquireSRWLockExclusive(&g_sh_lock);
    sh_init();
    ShRec *r = sh_find(target, 1);
    ShList *old = r ? r->list : NULL, *nl = NULL;
    if (!r) { s = MH_ERROR_MEMORY_ALLOC; goto out; }
    if (r->raw_owner) { if (busy_owner) *busy_owner = r->raw_owner; s = V3X_EHOOKBUSY; goto out; }
    n = old ? old->n : 0;
    for (int i = 0; i < n; i++)
        if (old->sub[i].owner == owner && old->sub[i].pre == pre && old->sub[i].post == post && old->sub[i].user == user) {
            s = MH_ERROR_ALREADY_CREATED; goto out;
        }
    if (n >= V3X_HOOK_MAX_SUBSCRIBERS) { s = MH_ERROR_MEMORY_ALLOC; goto out; }
    if (!r->ctx) {
        SafeCtx *c = sh_alloc();
        if (!c) { s = MH_ERROR_MEMORY_ALLOC; goto out; }
        sh_ctx_init(c, NULL);
        r->ctx = c;
    }
    if (!r->enabled) {                 /* disabled: no new call enters the stub; safe to (re)point it */
        r->ctx->pre = sh_disp_pre; r->ctx->post = sh_disp_post; r->ctx->user = r; r->ctx->target = target;
        __atomic_store_n(&r->ctx->jmp, (void *)v3x_safe_thunk, __ATOMIC_RELEASE);
    }
    if (!(nl = sh_list_copy(r, old))) { s = MH_ERROR_MEMORY_ALLOC; goto out; }
    nl->sub[n].pre = pre; nl->sub[n].post = post; nl->sub[n].user = user; nl->sub[n].owner = owner;
    nl->n = n + 1;
    if (!r->created) {
        s = MH_CreateHook(target, r->ctx->stub, &r->ctx->original);
        if (s == MH_ERROR_ALREADY_CREATED) s = V3X_EHOOKBUSY;   /* hooked outside the registry */
        if (s != MH_OK) { HeapFree(GetProcessHeap(), 0, nl); goto out; }
        r->created = 1;
    }
    sh_publish(r, nl);                 /* before enabling: the first call already sees it */
    if (!r->enabled) {
        s = MH_EnableHook(target);
        if (s != MH_OK) { sh_publish(r, old); goto out; }   /* nl may have been read: not freed */
        r->enabled = 1;
    }
out:
    ReleaseSRWLockExclusive(&g_sh_lock);
    return s;
}

/* Raw MinHook detour for `owner`: exclusive. 0, -1 bad args, V3X_EHOOKBUSY (*busy_owner = holder,
 * NULL = hooked outside the registry) or an MH_STATUS. */
int v3x_hook_add_raw(void *owner, void *target, void *detour, void **original, void **busy_owner)
{
    if (busy_owner) *busy_owner = NULL;
    if (!owner) return -1;
    int s;
    AcquireSRWLockExclusive(&g_sh_lock);
    ShRec *r = target ? sh_find(target, 0) : NULL;
    if (r && r->raw_owner) { if (busy_owner) *busy_owner = r->raw_owner; s = V3X_EHOOKBUSY; goto out; }
    if (r && r->list && r->list->n) { if (busy_owner) *busy_owner = r->list->sub[0].owner; s = V3X_EHOOKBUSY; goto out; }
    if (!target || !detour) { s = -1; goto out; }
    if (!r && !(r = sh_find(target, 1))) { s = MH_ERROR_MEMORY_ALLOC; goto out; }
    if (!r->ctx) {
        SafeCtx *c = sh_alloc();
        if (!c) { s = MH_ERROR_MEMORY_ALLOC; goto out; }
        sh_ctx_init(c, NULL);
        r->ctx = c;
    }
    if (r->enabled) MH_DisableHook(target);   /* unreachable (subscribers / raw owner refused above); defensive */
    r->enabled = 0;
    /* the hook is disabled here: point the stub at the detour, then (re)enable the same entry */
    r->ctx->pre = r->ctx->post = NULL; r->ctx->user = NULL; r->ctx->target = target;
    __atomic_store_n(&r->ctx->jmp, detour, __ATOMIC_RELEASE);
    if (!r->created) {
        s = MH_CreateHook(target, r->ctx->stub, &r->ctx->original);
        if (s == MH_ERROR_ALREADY_CREATED) { s = V3X_EHOOKBUSY; goto out; }   /* hooked outside the registry */
        if (s != MH_OK) goto out;
        r->created = 1;
    }
    if (original) *original = r->ctx->original;   /* before enabling: the detour may run at once */
    s = MH_EnableHook(target);
    if (s != MH_OK) {
        if (original) *original = NULL;
        goto out;
    }
    r->enabled = 1;
    r->raw_owner = owner;
out:
    ReleaseSRWLockExclusive(&g_sh_lock);
    return s;
}

/* unhook for `owner`: removes its raw hook, or all of its hook_safe subscriptions on `target` (the
 * thunk is disabled when the last subscriber leaves). V3X_EHOOKBUSY (*busy_owner set) if only other
 * owners hold the target; MH_ERROR_NOT_CREATED if nothing is subscribed. A target unknown to the
 * registry is only disabled (MH_DisableHook). Nothing is ever MH_RemoveHook'ed here: a removed
 * hook frees the trampoline a thread may still be about to call (audit M2). */
int v3x_hook_remove(void *owner, void *target, void **busy_owner)
{
    if (busy_owner) *busy_owner = NULL;
    int s = MH_OK;
    AcquireSRWLockExclusive(&g_sh_lock);
    ShRec *r = target ? sh_find(target, 0) : NULL;
    if (!r) {
        s = MH_DisableHook(target);
    } else if (r->raw_owner) {
        if (r->raw_owner != owner) { if (busy_owner) *busy_owner = r->raw_owner; s = V3X_EHOOKBUSY; }
        else {
            s = r->enabled ? MH_DisableHook(target) : MH_OK;   /* restores the original bytes; trampoline kept */
            if (s == MH_OK) { r->enabled = 0; r->raw_owner = NULL; }
        }
    } else if (!r->list || !r->list->n) {
        s = MH_ERROR_NOT_CREATED;
    } else {
        ShList *old = r->list, *nl = sh_list_copy(r, NULL);
        if (!nl) { s = MH_ERROR_MEMORY_ALLOC; goto out; }
        for (int i = 0; i < old->n; i++)
            if (old->sub[i].owner != owner) nl->sub[nl->n++] = old->sub[i];
        if (nl->n == old->n) {
            HeapFree(GetProcessHeap(), 0, nl);
            if (busy_owner) *busy_owner = old->sub[0].owner;
            s = V3X_EHOOKBUSY;
        } else if (nl->n == 0) {
            HeapFree(GetProcessHeap(), 0, nl);
            s = r->enabled ? MH_DisableHook(target) : MH_OK;   /* restores the original bytes */
            if (s == MH_OK) { r->enabled = 0; sh_publish(r, NULL); }
        } else {
            sh_publish(r, nl);
        }
    }
out:
    ReleaseSRWLockExclusive(&g_sh_lock);
    return s;
}

/* Number of hook_safe subscribers on `target` (0 = none), -1 if it has a raw hook. For logs/tests. */
int v3x_hook_subscribers(void *target)
{
    AcquireSRWLockShared(&g_sh_lock);
    ShRec *r = sh_find(target, 0);
    int n = !r ? 0 : r->raw_owner ? -1 : r->list ? r->list->n : 0;
    ReleaseSRWLockShared(&g_sh_lock);
    return n;
}

/* Removes everything `owner` holds (raw hooks and hook_safe subscriptions) on every target, like
 * calling v3x_hook_remove for each. For the loader's rollback of a plugin whose v3x_plugin_load failed
 * or whose callback faulted. Returns the number of targets released. */
int v3x_hook_remove_owner(void *owner)
{
    void *targets[SH_MAX_RECS];
    int n = 0, released = 0;
    if (!owner) return 0;
    AcquireSRWLockShared(&g_sh_lock);
    for (int i = 0; i < g_sh_nrecs; i++) {
        ShRec *r = &g_sh_recs[i];
        int mine = r->raw_owner == owner;
        ShList *l = r->list;
        for (int k = 0; !mine && l && k < l->n; k++) mine = l->sub[k].owner == owner;
        if (mine) targets[n++] = r->target;
    }
    ReleaseSRWLockShared(&g_sh_lock);
    for (int i = 0; i < n; i++)
        if (v3x_hook_remove(owner, targets[i], NULL) == MH_OK) released++;
    return released;
}
