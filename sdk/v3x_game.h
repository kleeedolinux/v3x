/* v3x_game.h - "where am I?" checks for V3X plugins (header-only, C).
 *
 * Two hard-won crashes (docs/safety.md, docs/re/simulation.md, docs/re/tick_par.md):
 *   - Running game script (effects, triggers, journal pulses) outside a tick task crashed the
 *     game: the script RNG needs an ALLOW_RANDOM_IN_SCOPE owner, which only engine tasks set.
 *     A save runs as a session command on a job, outside any tick task, so script from a
 *     save_begin handler dereferenced NULL.
 *   - The owner is ONE global pointer, not per thread (exe+0x540ae80 on 1.15.1; the owner
 *     record holds the owning thread id at +0x10). Parallel ranges and 38 other functions swap
 *     it with xchg.
 *
 * v3x_script_random_allowed(&g) reads that global (fault-proof) and returns 1 only if a scope
 * is set AND it belongs to the calling thread, i.e. the engine itself would not assert
 * "Calling global random function without having called ALLOW_RANDOM_IN_SCOPE in this thread".
 * That is necessary, not sufficient, for running script: you still need the right
 * GameStateAccess and nothing else running concurrently. If it returns anything but 1, do not
 * run script; defer to a hook inside a tick task instead.
 *
 *   static V3xGame g;
 *   v3x_game_resolve(api, &g);                 // once, at load (logs nothing; check g.err)
 *   ...
 *   if (v3x_script_random_allowed(&g) == 1) run_script(); else defer();
 *
 * How the global is found (fail-closed): every function that references the assert string above
 * loads the global with `mov rdi, [rip+X]` followed by `test rdi, rdi` before the string xref
 * (1.15.1: 03787300 RandomInt, 037873e0, 037874e0, 037875e0). At least 2 of them must agree on
 * X and none may disagree, and X must lie in .data. Verified offline by test/sdk_test.c
 * against victoria3.exe 1.15.1. */
#ifndef V3X_GAME_H
#define V3X_GAME_H

#include <stdio.h>
#include "v3x.h"
#include "v3x_scan.h"
#include "v3x_mem.h"

#define V3X_GAME_RNG_ASSERT "Calling global random function without having called ALLOW_RANDOM_IN_SCOPE in this thread"

typedef struct V3xGame {
    void **rng_owner_global;   /* &global holding the script-RNG scope pointer, or NULL */
    int rng_votes;             /* functions that agreed on it */
    char err[120];             /* why resolution failed ("" = ok) */
} V3xGame;

/* Returns 0 if everything resolved, -1 otherwise (g->err says why; the checks then return -1). */
static int v3x_game_resolve(const V3X_API *api, V3xGame *g)
{
    memset(g, 0, sizeof *g);
    V3xSection text, data;
    uintptr_t base = api->image_base();
    if (!v3x_section(base, ".text", &text) || !v3x_section(base, ".data", &data)) {
        snprintf(g->err, sizeof g->err, "no .text/.data section");
        return -1;
    }
    void *s = api->find_string(V3X_GAME_RNG_ASSERT);
    if (!s) { snprintf(g->err, sizeof g->err, "RNG assert string not found"); return -1; }
    void *x[16];
    int nx = v3x_xrefs(&text, s, x, 16);
    if (nx > 16) nx = 16;
    uint8_t *found = NULL;
    int votes = 0;
    for (int i = 0; i < nx; i++) {
        uint8_t *fn = (uint8_t *)api->function_start(x[i]);
        if (!fn || (uint8_t *)x[i] - fn > 0x100) continue;
        /* mov rdi, [rip+X] (48 8B 3D X) then test rdi, rdi (48 85 FF) within 16 bytes */
        for (uint8_t *p = fn; p + 7 <= (uint8_t *)x[i]; p++) {
            if (p[0] != 0x48 || p[1] != 0x8B || p[2] != 0x3D) continue;
            if (!v3x_scan_near(p + 7, 16, "48 85 FF")) continue;
            uint8_t *t = (uint8_t *)v3x_rip_target(p, 3, 7);
            if (found && t != found) {
                snprintf(g->err, sizeof g->err, "RNG owner global ambiguous (+0x%llx vs +0x%llx)",
                         (unsigned long long)(found - (uint8_t *)base), (unsigned long long)(t - (uint8_t *)base));
                return -1;
            }
            found = t; votes++;
            break;
        }
    }
    if (votes < 2) { snprintf(g->err, sizeof g->err, "RNG owner global: %d of %d functions matched (need 2)", votes, nx); return -1; }
    if (found < data.ptr || found + 8 > data.ptr + data.size) {
        snprintf(g->err, sizeof g->err, "RNG owner global +0x%llx is not in .data", (unsigned long long)(found - (uint8_t *)base));
        return -1;
    }
    g->rng_owner_global = (void **)found;
    g->rng_votes = votes;
    return 0;
}

/* 1 = a script RNG scope is active and owned by this thread (script may use random here),
 * 0 = no scope or another thread owns it (do NOT run script), -1 = not resolved / unreadable. */
static int v3x_script_random_allowed(const V3xGame *g)
{
    uintptr_t holder, owner;
    uint32_t tid;
    if (!g || !g->rng_owner_global) return -1;
    if (!v3x_mem_read_ptr((uintptr_t)g->rng_owner_global, &holder)) return -1;
    if (!holder) return 0;
    if (!v3x_mem_read_ptr(holder, &owner) || !owner) return 0;
    if (!v3x_mem_read_u32(owner + 0x10, &tid)) return 0;
    return tid == GetCurrentThreadId();
}

/* 1 = calling thread is the game's main thread, 0 = not, -1 = unknown (loader older than API v4). */
static int v3x_on_main_thread(const V3X_API *api)
{
    if (!V3X_HAS(api, main_thread_id)) return -1;
    return api->main_thread_id() == GetCurrentThreadId();
}

#endif
