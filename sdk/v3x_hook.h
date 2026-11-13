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

/* Typed access to V3X_HookCall arguments: V3X_ARG(call, 2, void *), V3X_ARGF(call, 1) (double). */
#define V3X_ARG(call, i, type) ((type)(uintptr_t)(call)->args[i])
#define V3X_ARGF(call, i) ((call)->xmm[i].d)
#define V3X_ARGF32(call, i) ((call)->xmm[i].f)

#endif
