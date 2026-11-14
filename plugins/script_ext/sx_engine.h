/* sx_engine.h - run effect / trigger text through the engine's Script Runner (layer A of script_ext).
 *
 * RE notes with every address: docs/re/script.md section 5 ("Running script text at runtime").
 * Nothing here hooks or patches the engine: it resolves 8 engine functions, one global and one
 * object field from anchors (strings + byte shapes), and CALLS them the way the object explorer's
 * Script Runner (CScriptRunnerInspector, jomini_effect_runner.cpp) does:
 *
 *   effect : take the script random scope (same sequence as RunEffect 0x2df6f30) -> parse the text
 *            (0x2df4680, reader "Effect Runner", root scope type 5 = country) -> execute
 *            (0x2df4d00: game-state access 2/1, top scope, effect) -> destroy top scope + effect.
 *   trigger: random scope -> parse (0x2dfa400, "Trigger Runner") -> evaluate (0x2dfb0d0) -> destroy.
 *
 * Fail closed: every function address is cross-checked (string xref + prologue + call graph
 * between the functions), any mismatch switches the whole feature off and e->why says why.
 * Main thread only. Offline-tested in test/script_ext_test.c (fake engine functions for the flow,
 * the real exe for the anchors); NOT run inside the game. */
#ifndef SX_ENGINE_H
#define SX_ENGINE_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "v3x.h"
#include "v3x_anchor.h"
#include "v3x_game.h"
#include "v3x_mem.h"
#include "v3x_scan.h"
#include "v3x_script.h"

#define SX_SCOPE_COUNTRY 5          /* ref type of a country (ExecuteEvent 0x86d5a0 uses it for the player) */
#define SX_TEXT_MAX 32768
#define SX_TOP_SIZE 0x400           /* CJominiTopScope is 0x138 bytes (ctor 0x6ab2e0); generous */

/* ------------------------------------------------------------------ engine ABI (MSVC x64) */

typedef struct SxStr {                       /* std::string / CString, 0x20 bytes */
    union { char buf[16]; char *ptr; } u;
    uint64_t size, cap;                      /* cap > 15: heap buffer in u.ptr */
} SxStr;
typedef struct SxRef { uint16_t type; uint16_t pad[3]; uint64_t id; } SxRef;   /* scope reference, 16 bytes */
typedef struct SxRngRec {                    /* the record 0x37879e0 fills (RunEffect keeps it on its stack) */
    void *owner;                             /* +0 object at exe+0x540ae60; its +0x10 holds the owning thread id */
    uint8_t acquired;                        /* +8 1 = this call took ownership (and must release it) */
    uint8_t pad[7];
    uint64_t spare[4];
} SxRngRec;
typedef struct SxSrcLoc { uint32_t line, col; const char *file; const char *func; } SxSrcLoc;
typedef struct SxTrgRes {                    /* result of 0x2dfa400, 0x28 bytes */
    union { void *trigger; SxStr err; } u;
    uint8_t is_error;                        /* +0x20 */
    uint8_t pad[7];
} SxTrgRes;
typedef struct SxFn {                        /* MSVC std::function<void(CJominiTopScope&)> with local storage */
    const void *const *vt;                   /* impl vtable: [0] copy [1] move [2] do_call [3] type [4] delete [5] target */
    void *captured;
    uint64_t pad[5];
    void *self;                              /* +0x38: _Ptr, points to the impl (= this object) */
} SxFn;

typedef void *(*sx_eff_parse_fn)(void **out, const SxStr *text, uint32_t type, SxStr *err);
typedef void *(*sx_eff_exec_fn)(void *holder, void *top, const SxRef *root, const SxFn *init);
typedef void *(*sx_trg_parse_fn)(SxTrgRes *out, const SxStr *text, uint32_t type);
typedef uint32_t (*sx_trg_eval_fn)(void *holder, const SxRef *root, const SxFn *init);
typedef void (*sx_top_dtor_fn)(void *top);
typedef void (*sx_str_dtor_fn)(SxStr *s);
typedef void *(*sx_rng_acq_fn)(SxRngRec *rec, void *unused, const SxSrcLoc *loc);

typedef struct SxEngine {
    int ok;                                  /* 1 = everything below resolved and cross-checked */
    char why[200];                           /* why not ("" = ok) */
    sx_eff_parse_fn eff_parse;               /* 0x2df4680 */
    sx_eff_exec_fn eff_exec;                 /* 0x2df4d00 */
    sx_trg_parse_fn trg_parse;               /* 0x2dfa400 */
    sx_trg_eval_fn trg_eval;                 /* 0x2dfb0d0 */
    sx_top_dtor_fn top_dtor;                 /* 0x6a7690 */
    sx_str_dtor_fn str_dtor;                 /* 0x67b890 */
    sx_rng_acq_fn rng_acquire;               /* 0x37879e0 */
    void **rng_global;                       /* exe+0x540ae80: pointer to the active SxRngRec */
    uint8_t *rng_owner;                      /* exe+0x540ae60: owner object, thread id at +0x10 */
    const uint8_t *player_root;              /* exe+0x5c3ef90: [[root]+0x608]+player_off = player country id */
    uint32_t player_off;                     /* 0xf8 */
} SxEngine;

/* ------------------------------------------------------------------ resolution */

/* RunEffect 0x2df6f30: mov [rsp+10h],rbx; push rdi; sub rsp,80h; mov rbx,rcx; xor edi,edi; srcloc {2Dh, 4} */
#define SX_P_RUNEFFECT "48 89 5C 24 10 57 48 81 EC 80 00 00 00 48 8B D9 33 FF C7 44 24 30 2D 00 00 00 C7 44 24 34 04 00 00 00"
/* RunEffect, inside: lea r8,[rsp+30h]; lea rcx,[rsp+50h]; call acquire */
#define SX_P_RE_ACQ "4C 8D 44 24 30 48 8D 4C 24 50 E8 ?? ?? ?? ??"
/* RunEffect, inside: lea rax,[G]; mov [rsp+60h],rax; lea rax,[rsp+50h]; xchg [G],rax */
#define SX_P_RE_GLOBAL "48 8D 05 ?? ?? ?? ?? 48 89 44 24 60 48 8D 44 24 50 48 87 05 ?? ?? ?? ??"
/* RunEffect, inside: mov dl,1; mov rcx,[rbx]; call RunInspector */
#define SX_P_RE_INSP "B2 01 48 8B 0B E8 ?? ?? ?? ??"
/* RunInspector 0x2df7ad0 prologue */
#define SX_P_INSP "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 20 55 41 56 41 57 48 8D AC 24 D0 FE FF FF 48 81 EC 30 02 00 00"
#define SX_P_IN_PARSE "48 8D 8D 60 01 00 00 E8 ?? ?? ?? ??"
#define SX_P_IN_STRDTOR "48 8D 4D C8 E8 ?? ?? ?? ?? 90 48 8B 4D C0"
#define SX_P_IN_EXEC "4C 8D 4C 24 48 4C 8D 44 24 38 48 8D 55 F0 48 8D 4C 24 20 E8 ?? ?? ?? ??"
#define SX_P_IN_TOPDTOR "48 8D 4D F0 E8 ?? ?? ?? ?? 48 8B 4C 24 20 0F B6 44 24 30"
/* parse functions (effect 0x2df4680 and trigger 0x2dfa400 share this prologue) */
#define SX_P_PARSE "48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D AC 24 10 FE FF FF 48 81 EC F0 02 00 00"
#define SX_P_EFF_EXEC "48 89 5C 24 08 48 89 54 24 10 57 48 81 EC F0 00 00 00 48 8B FA 48 8B D9 C7 44 24 20 00 00 00 00"
#define SX_P_TRG_EVAL "48 89 5C 24 10 48 89 74 24 18 55 57 41 56 48 8D 6C 24 90 48 81 EC 70 01 00 00 C5 F8 29 B4 24 60 01 00 00 49 8B D8 33 F6"
#define SX_P_TOPDTOR "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B D9 48 81 C1 18 01 00 00 E8"
#define SX_P_STRDTOR "40 53 48 83 EC 30 48 8B 41 18 48 8B D9 48 83 F8 0F 76 2C"
/* 0x37879e0 (random scope acquire): prologue, `lea rax,[owner]; mov [rcx],rax; mov byte [rcx+8],1`, `lock cmpxchg [rdx+10h],ecx` */
#define SX_P_RNG_ACQ "48 89 5C 24 08 48 89 7C 24 18 48 89 54 24 10 55 48 8D 6C 24 A9 48 81 EC 90 00 00 00"
#define SX_P_RNG_OWNER "48 8D 05 ?? ?? ?? ?? 48 89 01 C6 41 08 01 E8 ?? ?? ?? ?? 8B C8 C7 45 6F 00 00 00 00 48 8B 13 33 C0 F0 0F B1 4A 10"
/* ExecuteEvent 0x86d5a0: mov rdx,[root]; mov r13,[rdx+608h]; mov r8d,53Ch; mov rax,gs:[58h]; mov rcx,[rax];
 * cmp dword [r8+rcx],1; mov eax,-1; je +7; mov eax,[r13+OFF] */
#define SX_P_PLAYER "48 8B 15 ?? ?? ?? ?? 4C 8B AA 08 06 00 00 41 B8 3C 05 00 00 65 48 8B 04 25 58 00 00 00 48 8B 08 41 83 3C 08 01 B8 FF FF FF FF 74 07 41 8B 85 ?? ?? ?? ??"
#define SX_P_EXECEVENT "48 8B C4 48 89 58 18 48 89 48 08 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 38 FE FF FF"

#define SX_WHY(e, ...) (snprintf((e)->why, sizeof (e)->why, __VA_ARGS__), (e)->ok = 0, -1)

/* The only match of `pat` in [fn, fn+n) that lies in function fn; NULL if none or several. */
static uint8_t *sx_find_in(const V3X_API *api, const uint8_t *fn, size_t n, const char *pat)
{
    V3xPattern p;
    if (!fn || !v3x_pattern_compile(pat, &p)) return NULL;
    uint8_t *hit = NULL;
    int cnt = 0;
    for (size_t i = 0; i + (size_t)p.n <= n; i++) {
        if (!v3x_pattern_match_at(&p, fn + i) || api->function_start(fn + i) != (void *)fn) continue;
        hit = (uint8_t *)fn + i;
        cnt++;
    }
    return cnt == 1 ? hit : NULL;
}

/* 1 if fn starts with the pattern */
static int sx_starts(const uint8_t *fn, const char *pat)
{
    V3xPattern p;
    return fn && v3x_pattern_compile(pat, &p) && v3x_pattern_match_at(&p, fn);
}

static int sx_in_sec(const V3xSection *s, const void *p, size_t n) { return (const uint8_t *)p >= s->ptr && (const uint8_t *)p + n <= s->ptr + s->size; }

/* call-site check: the function `a` contains a call to `target`; the pattern's E8 is at pattern offset e8 */
static void *sx_call_in(const V3X_API *api, const uint8_t *fn, const char *pat, int e8)
{
    uint8_t *m = sx_find_in(api, fn, 0x400, pat);
    return m ? v3x_rel32_target(m + e8) : NULL;
}

/* 1 if some function calls both a and b (call-graph tie between two independently found functions) */
static int sx_share_caller(const V3X_API *api, const V3xSection *text, const void *a, const void *b)
{
    void *fa[64], *fb[64];
    int na = 0, nb = 0;
    for (size_t i = 0; i + 5 <= text->size; i++) {
        if (text->ptr[i] != 0xE8) continue;
        void *t = v3x_rel32_target(text->ptr + i);
        if (t == a && na < 64) fa[na++] = api->function_start(text->ptr + i);
        else if (t == b && nb < 64) fb[nb++] = api->function_start(text->ptr + i);
    }
    for (int i = 0; i < na; i++)
        for (int k = 0; k < nb; k++)
            if (fa[i] && fa[i] == fb[k]) return 1;
    return 0;
}

/* Resolves the Script Runner functions. Returns 0 (e->ok = 1) or -1 (e->why). Logs one line per anchor. */
static int sx_engine_resolve(const V3X_API *api, const char *tag, SxEngine *e)
{
    memset(e, 0, sizeof *e);
    V3xSection text, data;
    if (!v3x_section(api->image_base(), ".text", &text) || !v3x_section(api->image_base(), ".data", &data))
        return SX_WHY(e, "no .text/.data section");
    V3xAnchor A[] = {
        {.name = "run_effect_ui", .string = "RunEffect", .pattern = SX_P_RUNEFFECT, .xref = V3X_ANCHOR_ALL_SAME},
        {.name = "eff_parse", .string = "Effect Runner", .pattern = SX_P_PARSE, .xref = V3X_ANCHOR_ALL_SAME},
        {.name = "trg_parse", .string = "Trigger Runner", .pattern = SX_P_PARSE, .xref = V3X_ANCHOR_ALL_SAME},
        {.name = "trg_eval", .string = NULL, .pattern = SX_P_TRG_EVAL, .xref = 0},
        {.name = "exec_event", .string = "Please specify event name", .pattern = SX_P_EXECEVENT, .xref = V3X_ANCHOR_ALL_SAME},
    };
    enum { A_RE, A_EP, A_TP, A_TE, A_EV, A_N };
    if (v3x_anchor_resolve_all(api, tag, A, A_N)) {
        for (int i = 0; i < A_N; i++)
            if (!A[i].result) return SX_WHY(e, "anchor %s: %s", A[i].name, A[i].err);
    }

    /* RunEffect: random scope acquire, the global, RunInspector */
    uint8_t *re = (uint8_t *)A[A_RE].result;
    uint8_t *m = sx_find_in(api, re, 0x100, SX_P_RE_ACQ);
    uint8_t *acq = m ? (uint8_t *)v3x_rel32_target(m + 10) : NULL;
    if (!acq || !sx_in_sec(&text, acq, 0x40) || !sx_starts(acq, SX_P_RNG_ACQ)) return SX_WHY(e, "random scope acquire (0x37879e0) not found / prologue differs");
    m = sx_find_in(api, re, 0x100, SX_P_RE_GLOBAL);
    uint8_t *g1 = m ? (uint8_t *)v3x_rip_target(m, 3, 7) : NULL, *g2 = m ? (uint8_t *)v3x_rip_target(m + 17, 3, 7) : NULL;
    if (!g1 || g1 != g2 || !sx_in_sec(&data, g1, 8)) return SX_WHY(e, "random scope global: lea/xchg disagree or not in .data");
    V3xGame game;
    if (v3x_game_resolve(api, &game) || game.rng_owner_global != (void **)g1)
        return SX_WHY(e, "random scope global differs from sdk/v3x_game.h (%s)", game.err[0] ? game.err : "different address");
    m = sx_find_in(api, acq, 0x80, SX_P_RNG_OWNER);
    uint8_t *own = m ? (uint8_t *)v3x_rip_target(m, 3, 7) : NULL;
    if (!own || !sx_in_sec(&data, own, 0x18)) return SX_WHY(e, "random scope owner object not found / not in .data");
    uint8_t *insp = (uint8_t *)sx_call_in(api, re, SX_P_RE_INSP, 5);
    if (!insp || !sx_in_sec(&text, insp, 0x40) || !sx_starts(insp, SX_P_INSP)) return SX_WHY(e, "RunInspector (0x2df7ad0) not found / prologue differs");

    /* RunInspector: parse, execute, top scope dtor, string dtor */
    uint8_t *ep = (uint8_t *)A[A_EP].result;
    if (sx_call_in(api, insp, SX_P_IN_PARSE, 7) != (void *)ep) return SX_WHY(e, "RunInspector does not call the Effect Runner parse");
    uint8_t *ex = (uint8_t *)sx_call_in(api, insp, SX_P_IN_EXEC, 19);
    if (!ex || !sx_starts(ex, SX_P_EFF_EXEC)) return SX_WHY(e, "effect execute (0x2df4d00) not found / prologue differs");
    uint8_t *td = (uint8_t *)sx_call_in(api, insp, SX_P_IN_TOPDTOR, 4);
    if (!td || !sx_starts(td, SX_P_TOPDTOR)) return SX_WHY(e, "top scope destructor (0x6a7690) not found / prologue differs");
    uint8_t *sd = (uint8_t *)sx_call_in(api, insp, SX_P_IN_STRDTOR, 4);
    if (!sd || !sx_starts(sd, SX_P_STRDTOR)) return SX_WHY(e, "string destructor (0x67b890) not found / prologue differs");

    /* trigger: evaluate is tied to the Trigger Runner parse by a common caller */
    if (!sx_share_caller(api, &text, A[A_TE].result, A[A_TP].result)) return SX_WHY(e, "trigger evaluate and parse have no common caller");

    /* player country: [[root]+0x608]+off, read by ExecuteEvent */
    m = sx_find_in(api, (uint8_t *)A[A_EV].result, 0x800, SX_P_PLAYER);
    const uint8_t *root = m ? (const uint8_t *)v3x_rip_target(m, 3, 7) : NULL;
    uint32_t off = 0;
    if (m) memcpy(&off, m + 0x2F, 4);
    if (!root || !sx_in_sec(&data, root, 8) || off == 0 || off > 0x1000) return SX_WHY(e, "player country id read in ExecuteEvent not found");

    e->eff_parse = (sx_eff_parse_fn)(void *)ep;
    e->eff_exec = (sx_eff_exec_fn)(void *)ex;
    e->trg_parse = (sx_trg_parse_fn)A[A_TP].result;
    e->trg_eval = (sx_trg_eval_fn)A[A_TE].result;
    e->top_dtor = (sx_top_dtor_fn)(void *)td;
    e->str_dtor = (sx_str_dtor_fn)(void *)sd;
    e->rng_acquire = (sx_rng_acq_fn)(void *)acq;
    e->rng_global = (void **)g1;
    e->rng_owner = own;
    e->player_root = root;
    e->player_off = off;
    e->ok = 1;
    e->why[0] = 0;
    return 0;
}

/* ------------------------------------------------------------------ running text */

#define SX_RC_OK 0
#define SX_RC_ARG V3X_SCRIPT_EARG
#define SX_RC_THREAD V3X_SCRIPT_ETHREAD
#define SX_RC_OFF V3X_SCRIPT_EOFF

static LONG volatile sx_active;           /* 1 while a run is in progress (re-entrancy guard, fault repair) */
static struct { const SxEngine *e; SxRngRec rec; void *saved; int took, acquired; } sx_rng_state;
static void (*sx_run_log)(const char *fmt, ...);   /* optional */

static V3X_PRINTF(4, 5) int sx_fail(char *err, size_t en, int rc, const char *fmt, ...)
{
    if (err && en) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, en, fmt, ap);
        va_end(ap);
    }
    return rc;
}

static void sx_str_set(SxStr *s, const char *text, size_t n, char **heap)
{
    memset(s, 0, sizeof *s);
    *heap = NULL;
    if (n <= 15) {
        memcpy(s->u.buf, text, n);
        s->u.buf[n] = 0;
        s->size = n;
        s->cap = 15;
    } else {
        char *b = (char *)malloc(n + 1);   /* the engine only reads it (const CString *) and never frees it */
        if (!b) return;
        memcpy(b, text, n);
        b[n] = 0;
        s->u.ptr = b;
        s->size = n;
        s->cap = n;
        *heap = b;
    }
}

static void sx_str_init_empty(SxStr *s) { memset(s, 0, sizeof *s); s->cap = 15; }

/* message of an engine string into dst (cut, never faults) */
static void sx_str_copy(char *dst, size_t n, const SxStr *s)
{
    if (!dst || !n) return;
    dst[0] = 0;
    const char *p = s->cap > 15 ? s->u.ptr : s->u.buf;
    size_t len = s->size < n - 1 ? (size_t)s->size : n - 1;
    if (len && v3x_mem_read((uintptr_t)p, dst, len)) dst[len] = 0;
    else dst[0] = 0;
}

/* std::function stand-in: init callback that does nothing (the Script Runner's own lambda records
 * the run in its UI history). The engine only calls vt[2] with (impl, CJominiTopScope&). */
static void *sx_fn_copy(void *self, void *dst) { (void)self; return dst; }
static const void *sx_fn_type(void *self) { (void)self; return NULL; }
static void sx_fn_call(void *self, void *top) { (void)self; (void)top; }
static void sx_fn_delete(void *self, int heap) { (void)self; (void)heap; }
static void *sx_fn_target(void *self) { return self; }
static const void *const sx_fn_vt[6] = {(void *)sx_fn_copy, (void *)sx_fn_copy, (void *)sx_fn_call, (void *)sx_fn_type,
                                        (void *)sx_fn_delete, (void *)sx_fn_target};

static void sx_fn_make(SxFn *f)
{
    memset(f, 0, sizeof *f);
    f->vt = sx_fn_vt;
    f->self = f;
}

/* The player's country as the root scope. 0 = ok. */
static int sx_player_ref(const SxEngine *e, SxRef *ref, char *err, size_t en)
{
    uintptr_t g1 = 0, g2 = 0;
    uint32_t id = 0;
    memset(ref, 0, sizeof *ref);
    if (!v3x_mem_read_ptr((uintptr_t)e->player_root, &g1) || !g1 || !v3x_mem_read_ptr(g1 + 0x608, &g2) || !g2)
        return sx_fail(err, en, V3X_SCRIPT_ENOGAME, "no game loaded (game state not available)");
    if (!v3x_mem_read_u32(g2 + e->player_off, &id) || id == 0xFFFFFFFFu)
        return sx_fail(err, en, V3X_SCRIPT_ENOGAME, "no player country (game not started?)");
    ref->type = SX_SCOPE_COUNTRY;
    ref->id = id;
    return 0;
}

/* Takes the script random scope exactly like RunEffect 0x2df6f30: acquire (cmpxchg of the owner tid at
 * owner+0x10), then xchg the global to point at our record. Refuses instead of letting the engine assert
 * when another thread owns it. */
static int sx_rng_enter(const SxEngine *e, char *err, size_t en)
{
    uint32_t cur = 0, tid = GetCurrentThreadId();
    memset(&sx_rng_state, 0, sizeof sx_rng_state);
    sx_rng_state.e = e;
    if (!v3x_mem_read_u32((uintptr_t)e->rng_owner + 0x10, &cur))
        return sx_fail(err, en, V3X_SCRIPT_EOFF, "script random scope owner not readable");
    if (cur != 0 && cur != tid)
        return sx_fail(err, en, V3X_SCRIPT_EBUSY, "script random scope is held by thread %u (a tick task is running); try again", cur);
    SxSrcLoc loc = {0x2d, 4, "script_ext", "script_ext"};
    e->rng_acquire(&sx_rng_state.rec, NULL, &loc);
    if (sx_rng_state.rec.owner != (void *)e->rng_owner) {
        if (sx_rng_state.rec.acquired) InterlockedExchange((LONG volatile *)(e->rng_owner + 0x10), 0);
        return sx_fail(err, en, V3X_SCRIPT_EOFF, "random scope acquire returned an unexpected owner");
    }
    sx_rng_state.acquired = sx_rng_state.rec.acquired;
    sx_rng_state.saved = InterlockedExchangePointer((void *volatile *)e->rng_global, &sx_rng_state.rec);
    sx_rng_state.took = 1;
    return 0;
}

static void sx_rng_leave(void)
{
    const SxEngine *e = sx_rng_state.e;
    if (!e || !sx_rng_state.took) return;
    *e->rng_global = sx_rng_state.saved;
    if (sx_rng_state.acquired) {
        LONG old = InterlockedExchange((LONG volatile *)(e->rng_owner + 0x10), 0);
        if ((DWORD)old != GetCurrentThreadId() && sx_run_log) sx_run_log("random scope release: owner was thread %lu, not this thread", (unsigned long)old);
    }
    sx_rng_state.took = 0;
}

/* Fault cleanup. A fault inside the engine call is caught by the loader's SEH guard, which then disables
 * this plugin's callbacks: nobody could give the random scope back, and the tick threads would hit
 * "Random should not be acquired by any other thread". So a vectored handler (called before any frame
 * handler, installed once, last in the chain so crash_logger sees the fault first) restores the scope the
 * moment a hardware fault happens on the running thread, and switches script running off for the rest of
 * the session. It never handles the exception (returns CONTINUE_SEARCH). C++ exceptions are ignored. */
static LONG volatile sx_run_disabled;
static DWORD sx_run_tid;

static int sx_fault_cleanup(DWORD code)
{
    if (!sx_active || sx_run_tid != GetCurrentThreadId()) return 0;
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_IN_PAGE_ERROR: case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION: case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        break;
    default:
        return 0;
    }
    sx_rng_leave();
    sx_run_disabled = 1;
    sx_active = 0;
    if (sx_run_log) sx_run_log("FAULT %08lx during a script run: random scope released, script running disabled until restart", (unsigned long)code);
    return 1;
}

static LONG CALLBACK sx_veh(EXCEPTION_POINTERS *ep)
{
    sx_fault_cleanup(ep->ExceptionRecord->ExceptionCode);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void *sx_veh_handle;
static void sx_veh_install(void)
{
    if (!sx_veh_handle) sx_veh_handle = AddVectoredExceptionHandler(0, sx_veh);
}

/* Runs effect text with the player's country as root. Returns 0 or a V3X_SCRIPT_E*; err gets the reason. */
static int sx_run_effect(const V3X_API *api, const SxEngine *e, const char *text, char *err, size_t en)
{
    if (err && en) err[0] = 0;
    if (!e || !e->ok) return sx_fail(err, en, SX_RC_OFF, "script runner unavailable: %s", e && e->why[0] ? e->why : "not resolved");
    if (sx_run_disabled) return sx_fail(err, en, SX_RC_OFF, "script running was disabled after a fault in an earlier run");
    size_t n = text ? strlen(text) : 0;
    if (!n || n > SX_TEXT_MAX) return sx_fail(err, en, SX_RC_ARG, "effect text must be 1..%d bytes", SX_TEXT_MAX);
    if (v3x_on_main_thread(api) != 1) return sx_fail(err, en, SX_RC_THREAD, "run_effect_text must be called on the main thread (use api->run_on_main)");
    SxRef ref;
    int rc = sx_player_ref(e, &ref, err, en);
    if (rc) return rc;
    if (InterlockedCompareExchange(&sx_active, 1, 0)) return sx_fail(err, en, V3X_SCRIPT_EBUSY, "a script run is already in progress (re-entrant call)");
    sx_run_tid = GetCurrentThreadId();

    SxStr txt, perr;
    char *heap = NULL;
    sx_str_set(&txt, text, n, &heap);
    if (n > 15 && !heap) { sx_active = 0; return sx_fail(err, en, V3X_SCRIPT_EIO, "out of memory"); }
    sx_str_init_empty(&perr);
    rc = sx_rng_enter(e, err, en);
    if (rc) { sx_active = 0; free(heap); return rc; }

    void *eff = NULL;
    e->eff_parse(&eff, &txt, SX_SCOPE_COUNTRY, &perr);
    if (!eff) {
        sx_str_copy(err, en, &perr);
        if (err && en && !err[0]) snprintf(err, en, "script error (no message)");
        e->str_dtor(&perr);
        rc = V3X_SCRIPT_ESCRIPT;
    } else {
        _Alignas(16) uint8_t top[SX_TOP_SIZE];
        void *holder[4] = {eff, 0, 0, 0};            /* {effect*, ...}: execute reads [0] only */
        SxFn fn;
        memset(top, 0, sizeof top);
        sx_fn_make(&fn);
        holder[1] = (void *)(uintptr_t)SX_SCOPE_COUNTRY;
        e->eff_exec(holder, top, &ref, &fn);
        e->top_dtor(top);
        (*(void (***)(void *, int))eff)[0](eff, 1);   /* deleting destructor, as RunInspector does */
        rc = 0;
    }
    sx_rng_leave();
    sx_active = 0;
    free(heap);
    return rc;
}

/* Evaluates trigger text with the player's country as root. *result = 0/1. Same return codes. */
static int sx_eval_trigger(const V3X_API *api, const SxEngine *e, const char *text, int *result, char *err, size_t en)
{
    if (err && en) err[0] = 0;
    if (result) *result = 0;
    if (!e || !e->ok) return sx_fail(err, en, SX_RC_OFF, "script runner unavailable: %s", e && e->why[0] ? e->why : "not resolved");
    if (sx_run_disabled) return sx_fail(err, en, SX_RC_OFF, "script running was disabled after a fault in an earlier run");
    size_t n = text ? strlen(text) : 0;
    if (!n || n > SX_TEXT_MAX || !result) return sx_fail(err, en, SX_RC_ARG, "trigger text must be 1..%d bytes and result must not be NULL", SX_TEXT_MAX);
    if (v3x_on_main_thread(api) != 1) return sx_fail(err, en, SX_RC_THREAD, "eval_trigger_text must be called on the main thread (use api->run_on_main)");
    SxRef ref;
    int rc = sx_player_ref(e, &ref, err, en);
    if (rc) return rc;
    if (InterlockedCompareExchange(&sx_active, 1, 0)) return sx_fail(err, en, V3X_SCRIPT_EBUSY, "a script run is already in progress (re-entrant call)");
    sx_run_tid = GetCurrentThreadId();

    SxStr txt;
    char *heap = NULL;
    sx_str_set(&txt, text, n, &heap);
    if (n > 15 && !heap) { sx_active = 0; return sx_fail(err, en, V3X_SCRIPT_EIO, "out of memory"); }
    rc = sx_rng_enter(e, err, en);
    if (rc) { sx_active = 0; free(heap); return rc; }

    SxTrgRes res;
    memset(&res, 0, sizeof res);
    e->trg_parse(&res, &txt, SX_SCOPE_COUNTRY);
    if (res.is_error || !res.u.trigger) {
        if (res.is_error) {
            sx_str_copy(err, en, &res.u.err);
            e->str_dtor(&res.u.err);
        }
        if (err && en && !err[0]) snprintf(err, en, "script error (no message)");
        rc = V3X_SCRIPT_ESCRIPT;
    } else {
        SxFn fn;
        sx_fn_make(&fn);
        void *trg = res.u.trigger;
        *result = (e->trg_eval(&res, &ref, &fn) & 0xFF) != 0;
        (*(void (***)(void *, int))trg)[2](trg, 1);   /* deleting destructor, slot 2 of CJominiTrigger */
        rc = 0;
    }
    sx_rng_leave();
    sx_active = 0;
    free(heap);
    return rc;
}

#endif
