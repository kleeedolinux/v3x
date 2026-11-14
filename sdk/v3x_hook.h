/* v3x_hook.h - hooking helpers for V3X plugins (header-only, C).
 *
 * Prefer api->hook_safe (API v4): you write pre/post callbacks that receive a V3X_HookCall
 * with every argument, and the core's thunk forwards all of them (16 integer slots, xmm0-3) and
 * the return value. Use a raw api->hook detour only when you must replace the function with an
 * exact prototype, or for hot functions where the thunk's ~40 extra instructions matter.
 *
 * Rule from a crash (docs/safety.md "argument forwarding"): a raw pass-through detour must
 * forward at least as many arguments as the engine function really takes. When you do not
 * know the exact count, use V3X_PASS8 (8 integer/pointer slots: rcx, rdx, r8, r9 + 4 stack
 * slots). It does NOT preserve float args unless your detour body uses no floating point before
 * the call; check with tools/re_simopt_argcheck.py how many stack arguments a target reads.
 *
 *   static v3x_pass8_fn orig;
 *   static uint64_t my_detour(V3X_PASS8_ARGS) { count++; return orig(V3X_PASS8); }
 *   api->hook(target, (void *)my_detour, (void **)&orig);
 *
 * v3x_hook_observe(): hook_safe when the loader has it, else fail (never falls back to a
 * guessed prototype). */
#ifndef V3X_HOOK_H
#define V3X_HOOK_H

#include <string.h>
#include "v3x.h"

#define V3X_PASS8_ARGS uint64_t a0_, uint64_t a1_, uint64_t a2_, uint64_t a3_, uint64_t a4_, uint64_t a5_, \
                       uint64_t a6_, uint64_t a7_
#define V3X_PASS8 a0_, a1_, a2_, a3_, a4_, a5_, a6_, a7_
typedef uint64_t (*v3x_pass8_fn)(V3X_PASS8_ARGS);

/* 0 = hooked; -1 = loader older than API v4 (no hook_safe) or bad args; otherwise MH_STATUS. */
static int v3x_hook_observe(const V3X_API *api, void *target, v3x_hook_fn pre, v3x_hook_fn post, void *user)
{
    if (!target || !V3X_HAS(api, hook_safe)) return -1;
    return api->hook_safe(target, pre, post, user);
}

/* Typed access to V3X_HookCall arguments: V3X_ARG(call, 2, void *), V3X_ARGF(call, 1) (double).
 * Checked (audit action 12): i must be 0..V3X_HOOK_MAX_ARGS-1 (16) for V3X_ARG and 0..3 for
 * V3X_ARGF / V3X_ARGF32. A constant index out of range is a compile error (clang); a run-time index
 * out of range (or call == NULL) reads 0 instead of memory past the struct. V3X_ARG is a value;
 * V3X_ARGF / V3X_ARGF32 stay assignable (pre may change float args): an out-of-range index then
 * writes a per-thread dummy, never past the struct. To change an integer arg: call->args[i] = v. */
#define V3X_ARG(call, i, type) ((type)(uintptr_t)v3x_arg_((call), (i)))
#define V3X_ARGF(call, i) (v3x_xmm_((call), (i))->d)
#define V3X_ARGF32(call, i) (v3x_xmm_((call), (i))->f)

#if defined(__clang__)
#define V3X_HOOK_IDX_CHECK_(i, n, msg) __attribute__((diagnose_if((i) < 0 || (i) >= (n), msg, "error")))
#else
#define V3X_HOOK_IDX_CHECK_(i, n, msg)
#endif
#if defined(__cplusplus)
#define V3X_HOOK_TLS_ thread_local
#else
#define V3X_HOOK_TLS_ _Thread_local
#endif

static inline uint64_t v3x_arg_(const V3X_HookCall *call, int i)
    V3X_HOOK_IDX_CHECK_(i, V3X_HOOK_MAX_ARGS, "V3X_ARG index out of range (0..15)");
static inline uint64_t v3x_arg_(const V3X_HookCall *call, int i)
{
    return call && i >= 0 && i < V3X_HOOK_MAX_ARGS ? call->args[i] : 0;
}

static inline V3X_Xmm *v3x_xmm_(V3X_HookCall *call, int i) V3X_HOOK_IDX_CHECK_(i, 4, "V3X_ARGF index out of range (0..3)");
static inline V3X_Xmm *v3x_xmm_(V3X_HookCall *call, int i)
{
    static V3X_HOOK_TLS_ V3X_Xmm dummy;
    if (call && i >= 0 && i < 4) return &call->xmm[i];
    memset(&dummy, 0, sizeof dummy);
    return &dummy;
}

#endif
