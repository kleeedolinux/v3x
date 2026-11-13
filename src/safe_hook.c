/* V3X safe hooks (API v4 api->hook_safe): a generic pass-through thunk, so plugin authors can
 * observe or wrap an engine function without writing a detour with the right prototype.
 *
 * Why: a hand-written detour must forward exactly the arguments the engine function takes. A
 * 4-argument detour on a 5-argument function hands the original garbage for argument 5 (the
 * sim_opt crash: 0xe02c90 reads a 5th stack argument). This thunk forwards everything a Win64
 * function can receive in registers or in the first 12 stack slots, whatever the prototype.
 *
 * Per hook, one SafeCtx in an RWX pool holds a 24-byte stub:
 *     mov r10, <ctx>           ; r10 is volatile and never carries an argument in the Win64 ABI
 *     jmp [rip+0] ; dq v3x_safe_thunk
 * MinHook redirects the target to the stub; the stub does not touch rsp, so stack walks treat it
 * as a leaf. v3x_safe_thunk (below, in .text with SEH unwind info: one `sub rsp, 0x198`):
 *   1. saves rcx, rdx, r8, r9, the 12 caller stack slots [rsp+0x28..] and xmm0-3 into a
 *      V3X_HookCall on its own frame, plus user/target/original/return address;
 *   2. calls pre(call) if set;
 *   3. unless call->skip_original: reloads all of it (the outgoing stack slots too) and calls the
 *      MinHook trampoline; stores rax and xmm0 into call->ret / call->xmm_ret;
 *   4. calls post(call) if set; returns call->ret in rax and call->xmm_ret in xmm0.
 * Non-volatile registers (rbx, rbp, rdi, rsi, r12-r15, xmm6-15) are never touched by the thunk and
 * are preserved by the C callbacks, so the original and the caller see them unchanged.
 *
 * Frame of v3x_safe_thunk (rsp after the prologue `sub rsp, 0x198`; 16-byte aligned there):
 *   +0x000 .. +0x020  home space for the calls we make
 *   +0x020 .. +0x080  outgoing stack args 5..16 for the original
 *   +0x080 .. +0x190  V3X_HookCall (0x110 bytes, offsets in include/v3x.h)
 *   +0x190            saved ctx (r10)
 *   +0x198            return address; +0x1a0 .. +0x1c0 caller's home space; +0x1c0 caller arg 5
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
    void *original;        /* +0x00 MinHook trampoline */
    v3x_hook_fn pre;       /* +0x08 */
    v3x_hook_fn post;      /* +0x10 */
    void *user;            /* +0x18 */
    void *target;          /* +0x20 */
    uint8_t stub[24];      /* +0x28 */
    /* 64 bytes per ctx */
} SafeCtx;

_Static_assert(sizeof(SafeCtx) == 64, "SafeCtx layout");
_Static_assert(offsetof(SafeCtx, stub) == 0x28, "SafeCtx layout");
_Static_assert(sizeof(V3X_HookCall) == 0x110, "V3X_HookCall layout (thunk offsets)");
_Static_assert(offsetof(V3X_HookCall, xmm) == 0x80, "V3X_HookCall layout");
_Static_assert(offsetof(V3X_HookCall, ret) == 0xc0, "V3X_HookCall layout");
_Static_assert(offsetof(V3X_HookCall, skip_original) == 0xc8, "V3X_HookCall layout");
_Static_assert(offsetof(V3X_HookCall, xmm_ret) == 0xd0, "V3X_HookCall layout");
_Static_assert(offsetof(V3X_HookCall, user) == 0xe0, "V3X_HookCall layout");
_Static_assert(offsetof(V3X_HookCall, return_address) == 0xf8, "V3X_HookCall layout");
_Static_assert(offsetof(V3X_HookCall, scratch) == 0x100, "V3X_HookCall layout");

void v3x_safe_thunk(void);

/* Stack-arg copies: caller slot i (i = 0..11) is at [rsp+0x1c0+8i]; HookCall.args[4+i] at
 * [rsp+0xa0+8i]; outgoing slot at [rsp+0x20+8i]. */
#define SH_IN(i)  "mov rax, qword ptr [rsp+0x1c0+8*" #i "]\n\tmov qword ptr [rsp+0xa0+8*" #i "], rax\n\t"
#define SH_OUT(i) "mov rax, qword ptr [rsp+0xa0+8*" #i "]\n\tmov qword ptr [rsp+0x20+8*" #i "], rax\n\t"

__asm__(
    ".intel_syntax noprefix\n"
    ".text\n"
    ".globl v3x_safe_thunk\n"
    ".def v3x_safe_thunk; .scl 2; .type 32; .endef\n"
    ".p2align 4\n"
    "v3x_safe_thunk:\n"
    ".seh_proc v3x_safe_thunk\n\t"
    "sub rsp, 0x198\n\t"
    ".seh_stackalloc 0x198\n\t"
    ".seh_endprologue\n\t"
    "mov qword ptr [rsp+0x190], r10\n\t"
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
    "mov rax, qword ptr [rsp+0x198]\n\t"
    "mov qword ptr [rsp+0x178], rax\n\t"          /* return address */
    "mov rax, qword ptr [r10+0x08]\n\t"           /* pre */
    "test rax, rax\n\t"
    "jz .Lv3x_sh_nopre\n\t"
    "lea rcx, [rsp+0x80]\n\t"
    "call rax\n"
    ".Lv3x_sh_nopre:\n\t"
    "cmp dword ptr [rsp+0x148], 0\n\t"
    "jne .Lv3x_sh_skip\n\t"
    SH_OUT(0) SH_OUT(1) SH_OUT(2) SH_OUT(3) SH_OUT(4) SH_OUT(5)
    SH_OUT(6) SH_OUT(7) SH_OUT(8) SH_OUT(9) SH_OUT(10) SH_OUT(11)
    "movups xmm0, xmmword ptr [rsp+0x100]\n\t"
    "movups xmm1, xmmword ptr [rsp+0x110]\n\t"
    "movups xmm2, xmmword ptr [rsp+0x120]\n\t"
    "movups xmm3, xmmword ptr [rsp+0x130]\n\t"
    "mov rcx, qword ptr [rsp+0x80]\n\t"
    "mov rdx, qword ptr [rsp+0x88]\n\t"
    "mov r8, qword ptr [rsp+0x90]\n\t"
    "mov r9, qword ptr [rsp+0x98]\n\t"
    "mov r10, qword ptr [rsp+0x190]\n\t"
    "call qword ptr [r10]\n\t"                    /* ctx->original, never the user-visible copy */
    "mov qword ptr [rsp+0x140], rax\n\t"
    "movups xmmword ptr [rsp+0x150], xmm0\n"
    ".Lv3x_sh_skip:\n\t"
    "mov r10, qword ptr [rsp+0x190]\n\t"
    "mov rax, qword ptr [r10+0x10]\n\t"           /* post */
    "test rax, rax\n\t"
    "jz .Lv3x_sh_nopost\n\t"
    "lea rcx, [rsp+0x80]\n\t"
    "call rax\n"
    ".Lv3x_sh_nopost:\n\t"
    "mov rax, qword ptr [rsp+0x140]\n\t"
    "movups xmm0, xmmword ptr [rsp+0x150]\n\t"
    "add rsp, 0x198\n\t"
    "ret\n"
    ".seh_endproc\n"
    ".att_syntax prefix\n");

/* ------------------------------------------------------------------ ctx pool */

#define SH_PAGE 65536                          /* one VirtualAlloc granule = 1024 hooks */
#define SH_MAX_PAGES 4
static SafeCtx *g_sh_pages[SH_MAX_PAGES];
static unsigned g_sh_used;                     /* ctx slots handed out across all pages */
static SRWLOCK g_sh_lock = SRWLOCK_INIT;

static SafeCtx *sh_alloc(void)
{
    const unsigned per = SH_PAGE / sizeof(SafeCtx);
    unsigned page = g_sh_used / per, slot = g_sh_used % per;
    if (page >= SH_MAX_PAGES) return NULL;
    if (!g_sh_pages[page]) {
        g_sh_pages[page] = (SafeCtx *)VirtualAlloc(NULL, SH_PAGE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!g_sh_pages[page]) return NULL;
    }
    g_sh_used++;
    return &g_sh_pages[page][slot];
}

static void sh_write_stub(SafeCtx *c)
{
    uint8_t *p = c->stub;
    uint64_t ctx = (uint64_t)(uintptr_t)c, th = (uint64_t)(uintptr_t)v3x_safe_thunk;
    p[0] = 0x49; p[1] = 0xBA; memcpy(p + 2, &ctx, 8);                  /* mov r10, imm64 */
    p[10] = 0xFF; p[11] = 0x25; memset(p + 12, 0, 4);                  /* jmp [rip+0] */
    memcpy(p + 16, &th, 8);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof c->stub);
}

/* Installs the hook. Returns 0 or an MH_STATUS (MH_ERROR_MEMORY_ALLOC when the pool is full or
 * cannot be allocated), -1 on bad arguments. On failure the ctx slot is kept unused for reuse. */
int v3x_safe_hook_install(void *target, v3x_hook_fn pre, v3x_hook_fn post, void *user)
{
    if (!target || (!pre && !post)) return -1;
    AcquireSRWLockExclusive(&g_sh_lock);
    SafeCtx *c = sh_alloc();
    if (!c) { ReleaseSRWLockExclusive(&g_sh_lock); return MH_ERROR_MEMORY_ALLOC; }
    memset(c, 0, sizeof *c);
    c->pre = pre; c->post = post; c->user = user; c->target = target;
    sh_write_stub(c);
    MH_STATUS s = MH_CreateHook(target, c->stub, &c->original);
    if (s == MH_OK) {
        s = MH_EnableHook(target);
        if (s != MH_OK) MH_RemoveHook(target);
    }
    if (s != MH_OK) { memset(c, 0, sizeof *c); g_sh_used--; }   /* last slot handed out: give it back */
    ReleaseSRWLockExclusive(&g_sh_lock);
    return (int)s;
}

/* For tests: the thunk's code address (to check its unwind entry). */
void *v3x_safe_thunk_address(void) { return (void *)v3x_safe_thunk; }
