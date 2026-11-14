/* fast_lookup - lock-free side caches for hot engine lookups whose answers never change.
 * Results are bit-identical to vanilla: only the way an answer is fetched changes.
 * Data and proofs: docs/re/datastructures.md. Logic: fl_core.h (tested by test/fast_lookup_test.c).
 *
 * Features (each its own flag, global mode = off | measure | on):
 *   token_cache  hook TokenGetString (exe+0x3e1e010): id -> std::string* without the registry's
 *                shared spin lock. measure = the engine answers, the cache is filled and compared
 *                with every answer (any mismatch = feature off, logged).
 *   indent       hook the text writer's WriteIndent (exe+0x3e24100): writes the same tab chunks
 *                through the same stream call, reading token 17 from the cache instead of taking
 *                the spin lock. Only when token_cache is active; measure = count only.
 * Report: console command `fastlookup`, a log line after every save ("save_end" from enhance),
 * and at process exit. Nothing runs in DLL_PROCESS_DETACH. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>
#include "v3x.h"
#include "v3x_hook.h"
#include "v3x_anchor.h"
#include "v3x_ini.h"
#include "fl_core.h"

#define TAG "fast_lookup"
V3X_EXPORT const char *v3x_plugin_name = TAG;
V3X_PLUGIN_MANIFEST(TAG, "0.1.0", "v3x", "lock-free caches for immutable engine lookups (token strings)", "release/1.15.1");

/* TokenGetString: whole leaf function (no .pdata), rip disps wildcarded */
#define TOK_PAT "48 8B 15 ?? ?? ?? ?? B8 02 00 00 00 F0 0F C1 02 A8 01 74 21 F0 83 02 FE 8B 02 A8 01 74 0A 66 90 " \
                "F3 90 8B 02 A8 01 75 F8 B8 02 00 00 00 F0 0F C1 02 A8 01 75 DF 85 C9 78 1A 3B 4A 54 7F 15 48 8B " \
                "42 48 48 63 C9 48 8B 04 C8 F0 83 02 FE 48 85 C0 74 05 C3 F0 83 02 FE 48 8D 05 ?? ?? ?? ?? C3"
#define TOK_GLOBAL_DISP 3      /* mov rdx,[rip+d]: disp at +3, next ip +7 */
#define TOK_DEFAULT_DISP 0x5a  /* lea rax,[rip+d] at +0x57 */
/* WriteIndent: prologue .. the token 17 read .. the chunk loop's vtable call */
#define IND_PAT "48 8B C4 48 89 48 08 48 81 EC B8 00 00 00 80 79 14 00 4C 8B C1 0F 85 ?? ?? ?? ?? 83 79 10 00 0F 8E " \
                "?? ?? ?? ?? 48 89 58 10 BA 02 00 00 00 4C 89 60 F0 4C 89 68 E8 4C 89 78 D8 48 8B 05 ?? ?? ?? ?? F0 " \
                "0F C1 10 F6 C2 01 74 2D 66 0F 1F 44 00 00 F0 83 00 FE 8B 08 F6 C1 01 74 0E 0F 1F 44 00 00 F3 90 8B " \
                "08 F6 C1 01 75 F7 B9 02 00 00 00 F0 0F C1 08 F6 C1 01 75 D9 83 78 54 11 7D 06 F0 83 00 FE EB 14 48 " \
                "8B 48 48 48 8B 99 88 00 00 00 F0 83 00 FE 48 85 DB 75 07 48 8D 1D ?? ?? ?? ?? 4C 63 6B 10 B8 40 00 " \
                "00 00 45 8B 78 10 99 41 F7 FD"
#define IND_GLOBAL_DISP 0x3d   /* mov rax,[rip+d] at +0x3a */
#define IND_DEFAULT_DISP 0x9a  /* lea rbx,[rip+d] at +0x97 */
#define IND_SLOT_OFF 0x137     /* 4C 8B 40 38: mov r8,[rax+0x38] (stream vtable slot 0x38) */

static const uint8_t *rip_target(const uint8_t *fn, int disp_off)
{
    int32_t d;
    memcpy(&d, fn + disp_off, 4);
    return fn + disp_off + 4 + d;
}

static V3xAnchor A[] = {
    {.name = "TokenGetString", .pattern = TOK_PAT},
    {.name = "WriteIndent", .pattern = IND_PAT},
};

/* both functions must read the same registry global and return the same default string, and the
 * indent loop must call stream vtable slot 0x38 */
static const char *cross_check(const uint8_t *tok, const uint8_t *ind)
{
    if (rip_target(tok, TOK_GLOBAL_DISP) != rip_target(ind, IND_GLOBAL_DISP)) return "registry globals differ";
    if (rip_target(tok, TOK_DEFAULT_DISP) != rip_target(ind, IND_DEFAULT_DISP)) return "default strings differ";
    static const uint8_t slot[4] = {0x4C, 0x8B, 0x40, 0x38};
    if (memcmp(ind + IND_SLOT_OFF, slot, 4)) return "indent stream call is not vtable slot 0x38";
    return NULL;
}

static const V3X_API *api;
static int g_mode;                 /* 0 off, 1 measure, 2 on */
static int g_want_tok, g_want_ind;
static uint32_t g_verify_every;
static fl_tokcache *g_tc;
static v3x_pass8_fn o_tok, o_ind;
static int g_tok_on, g_ind_on;
static fl_stripe g_ind_st[FL_STRIPES];   /* calls = hooked calls, hits = emulated, misses = declined */
static long g_exiting;            /* __atomic: set on the exit event, read by every detour */

static inline fl_stripe *stripe(fl_stripe *base) { return &base[(__readgsdword(0x48) >> 2) & (FL_STRIPES - 1)]; }
static uint64_t tsc(void) { return __rdtsc(); }
static void *tok_orig(int32_t id) { return (void *)o_tok((uint64_t)(uint32_t)id, 0, 0, 0, 0, 0, 0, 0); }

/* TokenGetString is a leaf that only writes rax, rcx, rdx and flags, and the engine's compiler knows
 * it: 51 of its 523 call sites keep live values in r8, r9 or r11 across the call (exe+0x1905079 keeps
 * the "ship_{}_{}_add" format pointer in r8). A plain C detour clobbers the Win64 volatiles (tok_orig
 * even zeroes r8/r9) -> NULL format string -> AV at exe+0x43c1bb6 on every job thread (1.15.1 crash,
 * 2026-11-13). So MinHook jumps to fl_tok_thunk, which keeps r8-r11 and ymm0-5 intact around the C
 * code. ymm (not xmm) because AVX C code would zero the upper halves; the game itself needs AVX2.
 * Checked by test/fast_lookup_test.c (tok_probe). */
uint64_t fl_tok_detour_c(uint32_t id) __attribute__((used));
uint64_t fl_tok_detour_c(uint32_t id)
{
    if (__atomic_load_n(&g_exiting, __ATOMIC_RELAXED)) return o_tok((uint64_t)id, 0, 0, 0, 0, 0, 0, 0);
    fl_stripe *s = stripe(g_tc->st);
    return (uint64_t)fl_tok_get(g_tc, s, (int32_t)id, g_mode == 2, g_verify_every, tsc);
}

/* frame: entry rsp = 8 mod 16; 4 pushes + 0xe8 -> 16-aligned; +0x00 home, +0x20 ymm0-5 (6 x 32) */
uint64_t fl_tok_thunk(void);
__asm__(
    ".intel_syntax noprefix\n"
    ".text\n"
    ".globl fl_tok_thunk\n"
    ".def fl_tok_thunk; .scl 2; .type 32; .endef\n"
    ".p2align 4\n"
    "fl_tok_thunk:\n"
    ".seh_proc fl_tok_thunk\n\t"
    "push r8\n\t"
    ".seh_pushreg r8\n\t"
    "push r9\n\t"
    ".seh_pushreg r9\n\t"
    "push r10\n\t"
    ".seh_pushreg r10\n\t"
    "push r11\n\t"
    ".seh_pushreg r11\n\t"
    "sub rsp, 0xe8\n\t"
    ".seh_stackalloc 0xe8\n\t"
    ".seh_endprologue\n\t"
    "vmovdqu ymmword ptr [rsp+0x20], ymm0\n\t"
    "vmovdqu ymmword ptr [rsp+0x40], ymm1\n\t"
    "vmovdqu ymmword ptr [rsp+0x60], ymm2\n\t"
    "vmovdqu ymmword ptr [rsp+0x80], ymm3\n\t"
    "vmovdqu ymmword ptr [rsp+0xa0], ymm4\n\t"
    "vmovdqu ymmword ptr [rsp+0xc0], ymm5\n\t"
    "call fl_tok_detour_c\n\t"
    "vmovdqu ymm0, ymmword ptr [rsp+0x20]\n\t"
    "vmovdqu ymm1, ymmword ptr [rsp+0x40]\n\t"
    "vmovdqu ymm2, ymmword ptr [rsp+0x60]\n\t"
    "vmovdqu ymm3, ymmword ptr [rsp+0x80]\n\t"
    "vmovdqu ymm4, ymmword ptr [rsp+0xa0]\n\t"
    "vmovdqu ymm5, ymmword ptr [rsp+0xc0]\n\t"
    "add rsp, 0xe8\n\t"
    "pop r11\n\t"
    "pop r10\n\t"
    "pop r9\n\t"
    "pop r8\n\t"
    "ret\n"
    ".seh_endproc\n"
    ".att_syntax prefix\n");

static uint64_t ind_detour(V3X_PASS8_ARGS)
{
    fl_stripe *s = stripe(g_ind_st);
    FL_INC(s->calls);
    if (g_mode == 2 && !__atomic_load_n(&g_exiting, __ATOMIC_RELAXED) && !fl_disabled(g_tc)) {
        void *t17 = fl_tok_get(g_tc, stripe(g_tc->st), FL_TOKEN_INDENT, 1, g_verify_every, NULL);
        if (fl_indent((void *)a0_, t17, fl_stream_slot38)) { FL_INC(s->hits); return 0; }
    }
    FL_INC(s->misses);
    return o_ind(V3X_PASS8);
}

static void sum(const fl_stripe *st, fl_stripe *o)
{
    memset(o, 0, sizeof *o);
    for (int i = 0; i < FL_STRIPES; i++) {
#define FL_RD(f) __atomic_load_n(&st[i].f, __ATOMIC_RELAXED)
        o->calls += FL_RD(calls); o->hits += FL_RD(hits); o->misses += FL_RD(misses); o->fills += FL_RD(fills);
        o->verified += FL_RD(verified); o->timed += FL_RD(timed); o->cyc_orig += FL_RD(cyc_orig); o->cyc_fast += FL_RD(cyc_fast);
#undef FL_RD
    }
}

static void report(const char *why, int to_console)
{
    if (!g_tc) return;
    fl_stripe t, d;
    sum(g_tc->st, &t);
    sum(g_ind_st, &d);
    long mism = __atomic_load_n(&g_tc->mismatches, __ATOMIC_ACQUIRE);
    char line[640];
    snprintf(line, sizeof line,
             "[%s] mode=%s token_cache=%s: calls %llu, cache hits %llu (%.1f%%), misses %llu, filled %llu, compared %llu, "
             "MISMATCHES %ld%s; engine lookup %.0f cyc vs cache %.0f cyc (%llu timed) | indent=%s: calls %llu, emulated %llu, "
             "engine %llu",
             why, g_mode == 2 ? "on" : g_mode == 1 ? "measure" : "off", g_tok_on ? (fl_disabled(g_tc) ? "DISABLED" : "active") : "no",
             (unsigned long long)t.calls, (unsigned long long)t.hits, t.calls ? 100.0 * (double)t.hits / (double)t.calls : 0.0,
             (unsigned long long)t.misses, (unsigned long long)t.fills, (unsigned long long)t.verified, mism,
             mism ? " (cache turned off)" : "", t.timed ? (double)t.cyc_orig / (double)t.timed : 0.0,
             t.timed ? (double)t.cyc_fast / (double)t.timed : 0.0, (unsigned long long)t.timed, g_ind_on ? "active" : "no",
             (unsigned long long)d.calls, (unsigned long long)d.hits, (unsigned long long)d.misses);
    api->log(TAG, "%s", line);
    if (mism && __atomic_load_n(&g_tc->bad_ready, __ATOMIC_ACQUIRE))
        api->log(TAG, "first mismatch: id %d cached %p engine %p", (int)g_tc->bad_id, g_tc->bad_cached, g_tc->bad_orig);
    if (to_console && api->print) api->print("%s", line);
}

static void cmd_report(const char *args, void *u) { (void)args; (void)u; report("command", 1); }
static void on_save_end(const char *n, void *d, void *u) { (void)n; (void)d; (void)u; report("save_end", 0); }
static void on_exit_ev(const char *n, void *d, void *u)
{
    (void)n; (void)d; (void)u;
    __atomic_store_n(&g_exiting, 1, __ATOMIC_RELAXED);   /* static destructors may free the registry: forward everything from now on */
    report("exit", 0);
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    V3xIni ini;
    v3x_ini_load_plugin(&ini, api, TAG);
    char mode[16];
    v3x_ini_str(&ini, TAG, "mode", "measure", mode, sizeof mode);
    g_mode = !_stricmp(mode, "on") ? 2 : !_stricmp(mode, "measure") ? 1 : 0;
    g_want_tok = v3x_ini_int(&ini, TAG, "token_cache", 1);
    g_want_ind = v3x_ini_int(&ini, TAG, "indent", 1);
    int verify = v3x_ini_int(&ini, TAG, "verify_every", 0);
    int cap = v3x_ini_int(&ini, TAG, "max_tokens", 1 << 20);
    v3x_ini_free(&ini);
    g_verify_every = verify > 0 ? (uint32_t)verify : 0;
    if (cap < 4096) cap = 4096;
    if (cap > (1 << 24)) cap = 1 << 24;
    if (!g_mode || !g_want_tok) { api->log(TAG, "mode=%s token_cache=%d: nothing hooked", mode, g_want_tok); return 0; }

    if (v3x_anchor_resolve_all(api, TAG, A, 2) != 0) { api->log(TAG, "anchors failed - inactive"); return 0; }
    const char *why = cross_check(A[0].result, A[1].result);
    if (why) { api->log(TAG, "cross-check failed (%s) - inactive", why); return 0; }

    g_tc = VirtualAlloc(NULL, sizeof *g_tc, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    void *slots = VirtualAlloc(NULL, (size_t)cap * sizeof(void *), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_tc || !slots) { api->log(TAG, "out of memory - inactive"); return 0; }
    g_tc->slots = slots;
    g_tc->cap = (uint32_t)cap;
    g_tc->reg_global = (void *const *)rip_target(A[0].result, TOK_GLOBAL_DISP);
    g_tc->dflt = rip_target(A[0].result, TOK_DEFAULT_DISP);

    int rc = api->hook(A[0].result, (void *)fl_tok_thunk, (void **)&o_tok);
    if (rc) { api->log(TAG, "hook TokenGetString failed (%d) - inactive", rc); return 0; }
    g_tc->orig = tok_orig;
    g_tok_on = 1;
    if (g_want_ind) {
        rc = api->hook(A[1].result, (void *)ind_detour, (void **)&o_ind);
        if (rc) api->log(TAG, "hook WriteIndent failed (%d) - indent off", rc);
        else g_ind_on = 1;
    }
    if (api->api_version >= 3) {
        api->on_event("save_end", on_save_end, NULL);
        api->on_event("process_exit", on_exit_ev, NULL);
    }
    if (api->api_version >= 2) api->register_command("fastlookup", "fast_lookup: token cache stats", cmd_report, NULL);
    api->log(TAG, "ready: mode=%s token_cache=1 indent=%d verify_every=%u max_tokens=%d (registry global exe+0x%llx)",
             g_mode == 2 ? "on" : "measure", g_ind_on, g_verify_every, cap,
             (unsigned long long)((uintptr_t)g_tc->reg_global - api->image_base()));
    return 0;
}
