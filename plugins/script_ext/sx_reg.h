/* sx_reg.h - new script keywords (effects / triggers) implemented in C (layer C of script_ext).
 *
 * RE notes: docs/re/script.md sections 1-3. The engine registers every keyword from a CRT static
 * initializer ("registrar"):  token = 0x3e1d6f0(registry [0x5c40a38], &{name, len, 0});
 * entry = allocator 0x43d95c0(0x18) = {vtable, description, token}; 0x3553dc0 (effect) / 0x3593dc0
 * (trigger) (token, entry). When the parser meets the keyword it calls entry vtable slot 1 Create(entry),
 * which returns a new CJominiEffect / CJominiTrigger object.
 *
 * We do the same from a post-hook of ONE vanilla registrar (the debug_log effect's), so the registration
 * runs in the engine's own static-init context:
 *   - entry vtable = copy of debug_log's entry vtable (effect: 2 slots, trigger: 3 slots) with Create
 *     replaced by a function that calls debug_log's Create and then swaps the object's vtable;
 *   - object vtable = copy of CDebugLogEffect's (26 slots) / CDebugLogTrigger's (26 slots) with Execute
 *     (slot 22) / Evaluate (slot 24) replaced. debug_log's own set-value slot (effect 25, trigger 3)
 *     stores the value text in the object (CString at +0x40 / +0x38), so `v3x_x = "text"` just works.
 * The replaced slot finds the plugin callback by the token stored at object+8 and calls it with the scope
 * reference (ctx[0]) and the value text. Fail closed: every address and every vtable shape is checked;
 * on any mismatch nothing is hooked and nothing is registered (keywords then simply do not exist).
 * Containment (audit N5, sx_guard.h): a keyword belongs to the module that contains its callback; the
 * callback runs under the per-thread fault guard (a fault switches that keyword off and logs
 * module+offset) and is never called again once its keyword is off or its module was disabled by the
 * core (V3X_EVENT_PLUGIN_DISABLED). An engine object of a keyword that is off behaves like an empty
 * effect / a false trigger. */
#ifndef SX_REG_H
#define SX_REG_H

#include "sx_engine.h"
#include "sx_guard.h"

#define SX_KW_MAX 64
#define SX_KW_NAME 64

typedef struct SxKw {
    char name[SX_KW_NAME];
    char desc[SX_KW_NAME + 40];
    void *cb;
    void *user;
    uint32_t token;
    int registered;
    HMODULE owner;          /* module containing cb */
    volatile LONG off;      /* 1 = never call cb again (fault, or owner disabled) */
    volatile LONG faults;   /* faults caught (0 or 1: the first one switches it off) */
} SxKw;

typedef struct SxEntry { const void *vt; const char *desc; uint32_t token; uint32_t pad; } SxEntry;   /* 0x18 */
typedef struct SxNameArg { const char *p; uint32_t len; uint8_t zero; uint8_t pad[3]; } SxNameArg;   /* 16 bytes with padding */

typedef uint32_t (*sx_token_fn)(void *registry, const SxNameArg *name);
typedef void *(*sx_alloc_fn)(size_t n);
typedef void *(*sx_getter_fn)(void);
typedef void (*sx_register_fn)(uint32_t token, void *entry);
typedef void *(*sx_create_fn)(void *entry);

#define SX_EFF_SLOTS 26
#define SX_TRG_SLOTS 26
#define SX_EFF_EXEC_SLOT 22
#define SX_TRG_EVAL_SLOT 24
#define SX_EFF_MSG 0x40          /* CString value of CDebugLogEffect */
#define SX_TRG_MSG 0x38          /* CString value of CDebugLogTrigger */

typedef struct SxRegEng {
    int ok;
    char why[200];
    uint8_t *registrar;               /* the debug_log effect registrar (0x5d9110) = hook target */
    void *const *registry_global;     /* exe+0x5c40a38 */
    sx_getter_fn registry_getter;     /* 0x3e1df20 */
    sx_token_fn token_fn;             /* 0x3e1d6f0 */
    sx_alloc_fn alloc;                /* 0x43d95c0 */
    sx_register_fn eff_register;      /* 0x3553dc0 */
    sx_register_fn trg_register;      /* 0x3593dc0 */
    void *const *eff_entry_vt;        /* 0x4a77610 (2 slots) */
    void *const *trg_entry_vt;        /* 0x4a775d8 (3 slots) */
    void *const *eff_obj_vt;          /* 0x4a7e6f8 (26 slots) */
    void *const *trg_obj_vt;          /* 0x4a74788 (26 slots) */
} SxRegEng;

/* ------------------------------------------------------------------ registrations (filled by plugins at load) */

static SxKw sx_eff[SX_KW_MAX], sx_trg[SX_KW_MAX];
static int sx_neff, sx_ntrg;
static volatile LONG sx_reg_closed;       /* 1 once the engine's static initialization registered the keywords */
static int sx_reg_off;                    /* 1 = script_ext's load decided registration cannot work */
static char sx_reg_off_why[200];

/* v3x_[a-z0-9_]+, 5..63 chars. */
static int sx_valid_kw(const char *s)
{
    if (!s || strncmp(s, "v3x_", 4)) return 0;
    size_t n = strlen(s);
    if (n < 5 || n >= SX_KW_NAME) return 0;
    for (size_t i = 4; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return 0;
    }
    return 1;
}

static int sx_kw_add(SxKw *tab, int *n, const char *name, void *cb, void *user, const char *kind)
{
    if (!sx_valid_kw(name) || !cb) return V3X_SCRIPT_EARG;
    if (sx_reg_closed) return V3X_SCRIPT_ELATE;
    if (sx_reg_off) return V3X_SCRIPT_EOFF;
    for (int i = 0; i < *n; i++)
        if (!strcmp(tab[i].name, name)) return V3X_SCRIPT_EEXIST;
    if (*n >= SX_KW_MAX) return V3X_SCRIPT_EFULL;
    HMODULE owner = sx_module_of(cb);
    if (!owner || sx_module_disabled(owner)) return V3X_SCRIPT_EARG;   /* callback outside any module, or a disabled plugin */
    SxKw *k = &tab[(*n)++];
    memset(k, 0, sizeof *k);
    snprintf(k->name, sizeof k->name, "%s", name);
    snprintf(k->desc, sizeof k->desc, "V3X native %s (script_ext): %s = <value>", kind, name);
    k->cb = cb;
    k->user = user;
    k->owner = owner;
    return 0;
}

/* Switches off every keyword whose callback is in module h (the core disabled that plugin). Returns the count. */
static int sx_kw_disable_module(HMODULE h)
{
    int n = 0;
    for (int pass = 0; pass < 2; pass++) {
        SxKw *tab = pass ? sx_trg : sx_eff;
        int cnt = pass ? sx_ntrg : sx_neff;
        for (int i = 0; i < cnt; i++)
            if (tab[i].owner == h && !InterlockedExchange(&tab[i].off, 1)) n++;
    }
    return n;
}

/* ------------------------------------------------------------------ resolution */

/* Registrar of debug_log (identical shape for the effect and the trigger one; 0x5d9110 / 0x5e6250):
 * push rbx; sub rsp,40h; mov rcx,[REGISTRY]; lea rax,[NAME]; mov [rsp+20h],rax; mov dword [rsp+28h],9; mov byte
 * [rsp+2Ch],0; test rcx,rcx; jne +8; call GETTER; mov rcx,rax; vmovups xmm0,[rsp+20h]; lea rdx,[rsp+20h];
 * vmovdqa [rsp+20h],xmm0; call TOKEN; mov ecx,18h; mov ebx,eax; call ALLOC; test rax,rax; je ..;
 * lea rcx,[DESC]; mov [rax+10h],ebx; mov [rax+8],rcx; mov rdx,rax; lea rcx,[ENTRY_VT]; mov [rax],rcx; mov ecx,ebx;
 * call REGISTER; add rsp,40h; pop rbx; ret */
#define SX_P_REGISTRAR                                                                                                        \
    "40 53 48 83 EC 40 48 8B 0D ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 44 24 20 C7 44 24 28 09 00 00 00 C6 44 24 2C 00 "    \
    "48 85 C9 75 08 E8 ?? ?? ?? ?? 48 8B C8 C5 F8 10 44 24 20 48 8D 54 24 20 C5 F9 7F 44 24 20 E8 ?? ?? ?? ?? B9 18 00 "    \
    "00 00 8B D8 E8 ?? ?? ?? ?? 48 85 C0 74 28 48 8D 0D ?? ?? ?? ?? 89 58 10 48 89 48 08 48 8B D0 48 8D 0D ?? ?? ?? ?? "    \
    "48 89 08 8B CB E8 ?? ?? ?? ?? 48 83 C4 40 5B C3"
#define SX_P_REG_FN "48 89 54 24 10 89 4C 24 08 48 83 EC 48 E8"                    /* 0x3553dc0 / 0x3593dc0 */
#define SX_P_TOKEN_FN "48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 30 FE FF FF 48 81 EC D0 02 00 00"   /* 0x3e1d6f0 */
#define SX_P_ALLOC_FN "48 89 5C 24 08 57 48 83 EC 20 65 48 8B 04 25 58 00 00 00"   /* 0x43d95c0 */
#define SX_P_GETTER_FN "48 83 EC 28 B9 40 00 00 00 65 48 8B 04 25 58 00 00 00"    /* 0x3e1df20 */
/* Create of CEffectEntry<CDebugLogEffect> / CTriggerEntry<CDebugLogTrigger>: size, then the object vtable store, then token copy */
#define SX_P_EFF_CREATE "B9 60 00 00 00 E8"
#define SX_P_TRG_CREATE "B9 58 00 00 00 E8"
#define SX_P_CREATE_VT "48 8D 05 ?? ?? ?? ?? 48 89 03"
#define SX_P_CREATE_TOK "8B 47 10 89 43 08"
#define SX_P_ZERO_RET "33 C0 C3"
#define SX_EFF_PATH "C:\\mnt\\gsg\\caligula\\caligula\\cw\\jomini\\modules\\script_system\\source\\jomini_effect_impl.cpp"
#define SX_TRG_PATH "C:\\mnt\\gsg\\caligula\\caligula\\cw\\jomini\\modules\\script_system\\source\\jomini_trigger_impl.cpp"

#define SX_RWHY(r, ...) (snprintf((r)->why, sizeof (r)->why, __VA_ARGS__), (r)->ok = 0, -1)

/* Test seam: the offline test maps the exe unrelocated, so pointers stored in it (vtable slots) are absolute
 * preferred-base addresses; it sets sx_fix to map them into the mapping and sx_is_code to the mapping's
 * .text. In the game both stay NULL (identity / v3x_mem_is_code). */
static uintptr_t (*sx_fix)(uintptr_t p);
static int (*sx_is_code)(uintptr_t p);
static uint8_t *sx_P(const void *p) { return (uint8_t *)(sx_fix ? sx_fix((uintptr_t)p) : (uintptr_t)p); }
static int sx_code(const void *p) { return sx_is_code ? sx_is_code((uintptr_t)sx_P(p)) : v3x_mem_is_code((uintptr_t)p); }

/* 1 if every one of n vtable slots is a code pointer and slot n is not (so the vtable has exactly n slots). */
static int sx_vt_slots(void *const *vt, int n)
{
    for (int i = 0; i < n; i++)
        if (!sx_code(vt[i])) return 0;
    return !sx_code(vt[n]);
}

/* 1 if an xref of the string `path` lies inside function `fn` */
static int sx_fn_refs_string(const V3X_API *api, const V3xSection *text, const char *path, const void *fn)
{
    void *s = api->find_string(path);
    void *x[32];
    int n = s ? v3x_xrefs(text, s, x, 32) : 0;
    if (n > 32) n = 32;
    for (int i = 0; i < n; i++)
        if (api->function_start(x[i]) == fn) return 1;
    return 0;
}

/* Decoded registrar */
typedef struct SxRegistrar {
    uint8_t *fn;
    void *const *registry_global;
    uint8_t *getter, *token, *alloc, *reg;
    void *const *entry_vt;
} SxRegistrar;

static int sx_decode_registrar(const V3xSection *data, uint8_t *fn, const void *name_str, SxRegistrar *r)
{
    if (!sx_starts(fn, SX_P_REGISTRAR)) return 0;
    if (v3x_rip_target(fn + 0xD, 3, 7) != name_str) return 0;
    r->fn = fn;
    r->registry_global = (void *const *)v3x_rip_target(fn + 6, 3, 7);
    r->getter = (uint8_t *)v3x_rel32_target(fn + 0x2B);
    r->token = (uint8_t *)v3x_rel32_target(fn + 0x44);
    r->alloc = (uint8_t *)v3x_rel32_target(fn + 0x50);
    r->entry_vt = (void *const *)v3x_rip_target(fn + 0x6B, 3, 7);
    r->reg = (uint8_t *)v3x_rel32_target(fn + 0x77);
    return sx_in_sec(data, r->registry_global, 8);
}

/* Object vtable of the debug_log object created by entry vtable slot 1 (decoded from Create). */
static void *const *sx_create_vtable(const V3X_API *api, const V3xSection *text, const uint8_t *create, int is_effect)
{
    if (!create || !sx_in_sec(text, create, 0x80)) return NULL;
    if (!v3x_scan_near(create, 0x40, is_effect ? SX_P_EFF_CREATE : SX_P_TRG_CREATE)) return NULL;
    uint8_t *m = sx_find_in(api, create, 0x80, SX_P_CREATE_VT);
    if (!m || !sx_find_in(api, create, 0x80, SX_P_CREATE_TOK)) return NULL;
    return (void *const *)v3x_rip_target(m, 3, 7);
}

static int sx_reg_resolve(const V3X_API *api, SxRegEng *r)
{
    memset(r, 0, sizeof *r);
    V3xSection text, data, rdata;
    if (!v3x_section(api->image_base(), ".text", &text) || !v3x_section(api->image_base(), ".data", &data) ||
        !v3x_section(api->image_base(), ".rdata", &rdata))
        return SX_RWHY(r, "no .text/.data/.rdata section");
    void *name = api->find_string("debug_log");
    if (!name) return SX_RWHY(r, "string \"debug_log\" not found");
    void *x[16];
    int nx = v3x_xrefs(&text, name, x, 16);
    if (nx > 16) nx = 16;
    SxRegistrar eff = {0}, trg = {0};
    int have_e = 0, have_t = 0;
    for (int i = 0; i < nx; i++) {
        uint8_t *fn = (uint8_t *)api->function_start(x[i]);
        SxRegistrar d;
        if (!fn || (uint8_t *)x[i] - fn > 0x40 || !sx_decode_registrar(&data, fn, name, &d)) continue;
        void *const *vt = d.entry_vt;
        if (!sx_in_sec(&rdata, vt, 3 * 8) || !sx_code(vt[0]) || !sx_code(vt[1])) continue;
        uint8_t *create = sx_P(vt[1]);
        int is_eff = v3x_scan_near(create, 0x40, SX_P_EFF_CREATE) != NULL, is_trg = v3x_scan_near(create, 0x40, SX_P_TRG_CREATE) != NULL;
        if (is_eff == is_trg) continue;
        if (is_eff) { eff = d; have_e++; } else { trg = d; have_t++; }
    }
    if (have_e != 1 || have_t != 1) return SX_RWHY(r, "debug_log registrars: %d effect, %d trigger (need 1 each)", have_e, have_t);
    if (eff.getter != trg.getter || eff.token != trg.token || eff.alloc != trg.alloc || eff.registry_global != trg.registry_global)
        return SX_RWHY(r, "effect and trigger registrars disagree on registry / token / allocator");
    if (!sx_starts(eff.getter, SX_P_GETTER_FN) || !sx_starts(eff.token, SX_P_TOKEN_FN) || !sx_starts(eff.alloc, SX_P_ALLOC_FN))
        return SX_RWHY(r, "token registry / token / allocator prologue differs");
    if (!sx_starts(eff.reg, SX_P_REG_FN) || !sx_starts(trg.reg, SX_P_REG_FN) || eff.reg == trg.reg)
        return SX_RWHY(r, "effect / trigger register function prologue differs");

    void *const *eov = sx_create_vtable(api, &text, sx_P(eff.entry_vt[1]), 1);
    void *const *tov = sx_create_vtable(api, &text, sx_P(trg.entry_vt[1]), 0);
    if (!eov || !tov || !sx_in_sec(&rdata, eov, (SX_EFF_SLOTS + 1) * 8) || !sx_in_sec(&rdata, tov, (SX_TRG_SLOTS + 1) * 8))
        return SX_RWHY(r, "debug_log object vtables not found in Create");
    if (!sx_vt_slots(eov, SX_EFF_SLOTS) || !sx_vt_slots(tov, SX_TRG_SLOTS)) return SX_RWHY(r, "object vtable slot count differs from 26");
    /* entry vtables: effect 2 slots, trigger 3 slots (slot 2 = `xor eax,eax; ret`) */
    if (!sx_vt_slots(eff.entry_vt, 2)) return SX_RWHY(r, "effect entry vtable is not 2 slots");
    if (!sx_code(trg.entry_vt[2]) || !sx_starts(sx_P(trg.entry_vt[2]), SX_P_ZERO_RET) || sx_code(trg.entry_vt[3]))
        return SX_RWHY(r, "trigger entry vtable is not 3 slots with slot 2 = xor eax,eax");
    /* the slots we replace / rely on: Execute logs with the effect path string, Evaluate with the trigger one;
     * the set-value slots store the string into +0x40 / +0x38 (add rcx,imm8; call assign) */
    if (!sx_fn_refs_string(api, &text, SX_EFF_PATH, sx_P(eov[SX_EFF_EXEC_SLOT]))) return SX_RWHY(r, "effect vtable slot 22 is not the debug_log Execute");
    if (!sx_fn_refs_string(api, &text, SX_TRG_PATH, sx_P(tov[SX_TRG_EVAL_SLOT]))) return SX_RWHY(r, "trigger vtable slot 24 is not the debug_log Evaluate");
    if (!v3x_scan_near(sx_P(eov[25]), 0x40, "48 83 C1 40 E8")) return SX_RWHY(r, "effect vtable slot 25 does not assign to +0x40");
    if (!v3x_scan_near(sx_P(tov[3]), 0x40, "48 83 C1 38 E8")) return SX_RWHY(r, "trigger vtable slot 3 does not assign to +0x38");

    r->registrar = eff.fn;
    r->registry_global = eff.registry_global;
    r->registry_getter = (sx_getter_fn)(void *)eff.getter;
    r->token_fn = (sx_token_fn)(void *)eff.token;
    r->alloc = (sx_alloc_fn)(void *)eff.alloc;
    r->eff_register = (sx_register_fn)(void *)eff.reg;
    r->trg_register = (sx_register_fn)(void *)trg.reg;
    r->eff_entry_vt = eff.entry_vt;
    r->trg_entry_vt = trg.entry_vt;
    r->eff_obj_vt = eov;
    r->trg_obj_vt = tov;
    r->ok = 1;
    r->why[0] = 0;
    return 0;
}

/* ------------------------------------------------------------------ our vtables and wrappers */

static const SxRegEng *sx_reg_eng;
static void *sx_eff_obj_arr[1 + SX_EFF_SLOTS], *sx_trg_obj_arr[1 + SX_TRG_SLOTS];
static void *sx_eff_ent_arr[1 + 2], *sx_trg_ent_arr[1 + 3];
static sx_create_fn sx_eff_create_orig, sx_trg_create_orig;

static const char *sx_cstr(const SxStr *s) { return s->cap > 15 ? (s->u.ptr ? s->u.ptr : "") : s->u.buf; }

static SxKw *sx_kw_find(SxKw *tab, int n, uint32_t token)
{
    for (int i = 0; i < n; i++)
        if (tab[i].registered && tab[i].token == token) return &tab[i];
    return NULL;
}

static void sx_scope_of(void **ctx, V3X_ScriptScope *sc)
{
    memset(sc, 0, sizeof *sc);
    const uint8_t *ref = ctx ? (const uint8_t *)ctx[0] : NULL;
    if (ref) {
        memcpy(&sc->type, ref, 2);
        memcpy(&sc->id, ref + 8, 8);
    }
}

/* CEffectEntry vtable slot 1 replacement */
static void *sx_eff_create(void *entry)
{
    void *o = sx_eff_create_orig(entry);
    if (o) *(void *const **)o = &sx_eff_obj_arr[1];
    return o;
}
static void *sx_trg_create(void *entry)
{
    void *o = sx_trg_create_orig(entry);
    if (o) *(void *const **)o = &sx_trg_obj_arr[1];
    return o;
}

/* A keyword's callback faulted: switch it off and log where (once per keyword). Normal thread context. */
static void sx_kw_fault(SxKw *k, const SxGuard *g, const char *kind)
{
    InterlockedIncrement(&k->faults);
    if (InterlockedExchange(&k->off, 1)) return;
    if (!sx_run_log) return;
    char at[160], own[128];
    sx_where(g->addr, at, sizeof at);
    sx_module_name(k->owner, own, sizeof own);
    if (g->code == EXCEPTION_ACCESS_VIOLATION)
        sx_run_log("FAULT 0xc0000005 (%s 0x%llx) at %s in the callback of %s %s (owner %s): keyword switched off until restart",
                   g->info[0] == 1 ? "write" : g->info[0] == 8 ? "execute" : "read", (unsigned long long)g->info[1], at, kind, k->name, own[0] ? own : "?");
    else
        sx_run_log("FAULT 0x%08lx at %s in the callback of %s %s (owner %s): keyword switched off until restart",
                   (unsigned long)g->code, at, kind, k->name, own[0] ? own : "?");
}

/* 1 if k may be called (not off, owner not disabled); switches it off when its owner was disabled */
static int sx_kw_callable(SxKw *k)
{
    if (!k || k->off) return 0;
    if (sx_module_disabled(k->owner)) { InterlockedExchange(&k->off, 1); return 0; }
    return 1;
}

/* CJominiEffect slot 22: void Execute(this, const Context *ctx); ctx[0] = scope reference */
static void sx_eff_execute(void *self, void **ctx)
{
    uint32_t tok;
    memcpy(&tok, (uint8_t *)self + 8, 4);
    SxKw *k = sx_kw_find(sx_eff, sx_neff, tok);
    if (!sx_kw_callable(k)) return;
    V3X_ScriptScope sc;
    sx_scope_of(ctx, &sc);
    SxGuard g;
    if (sx_guard_effect(&g, (v3x_script_effect_fn)k->cb, &sc, sx_cstr((const SxStr *)((uint8_t *)self + SX_EFF_MSG)), k->user))
        sx_kw_fault(k, &g, "effect");
}

/* CJominiTrigger slot 24: bool Evaluate(this, const Context *ctx); a keyword that is off evaluates false */
static uint8_t sx_trg_evaluate(void *self, void **ctx)
{
    uint32_t tok;
    memcpy(&tok, (uint8_t *)self + 8, 4);
    SxKw *k = sx_kw_find(sx_trg, sx_ntrg, tok);
    if (!sx_kw_callable(k)) return 0;
    V3X_ScriptScope sc;
    sx_scope_of(ctx, &sc);
    SxGuard g;
    volatile int res = 0;
    if (sx_guard_trigger(&g, (v3x_script_trigger_fn)k->cb, &sc, sx_cstr((const SxStr *)((uint8_t *)self + SX_TRG_MSG)), k->user, &res)) {
        sx_kw_fault(k, &g, "trigger");
        return 0;
    }
    return res != 0;
}

/* Builds the four vtables from the engine's debug_log ones. 0 = ok. */
static int sx_reg_build(const SxRegEng *r)
{
    if (!r || !r->ok) return -1;
    sx_reg_eng = r;
    sx_eff_obj_arr[0] = r->eff_obj_vt[-1];
    memcpy(&sx_eff_obj_arr[1], r->eff_obj_vt, SX_EFF_SLOTS * sizeof(void *));
    sx_eff_obj_arr[1 + SX_EFF_EXEC_SLOT] = (void *)sx_eff_execute;
    sx_trg_obj_arr[0] = r->trg_obj_vt[-1];
    memcpy(&sx_trg_obj_arr[1], r->trg_obj_vt, SX_TRG_SLOTS * sizeof(void *));
    sx_trg_obj_arr[1 + SX_TRG_EVAL_SLOT] = (void *)sx_trg_evaluate;
    sx_eff_create_orig = (sx_create_fn)r->eff_entry_vt[1];
    sx_trg_create_orig = (sx_create_fn)r->trg_entry_vt[1];
    sx_eff_ent_arr[0] = r->eff_entry_vt[-1];
    sx_eff_ent_arr[1] = r->eff_entry_vt[0];
    sx_eff_ent_arr[2] = (void *)sx_eff_create;
    sx_trg_ent_arr[0] = r->trg_entry_vt[-1];
    sx_trg_ent_arr[1] = r->trg_entry_vt[0];
    sx_trg_ent_arr[2] = (void *)sx_trg_create;
    sx_trg_ent_arr[3] = r->trg_entry_vt[2];
    return 0;
}

/* Registers the queued keywords exactly like a vanilla registrar. Returns the number registered.
 * Called once, from the post-hook of the debug_log effect registrar (engine static init, main thread). */
static int sx_register_all(const SxRegEng *r, void (*logf)(const char *fmt, ...))
{
    int done = 0;
    for (int pass = 0; pass < 2; pass++) {
        SxKw *tab = pass ? sx_trg : sx_eff;
        int n = pass ? sx_ntrg : sx_neff;
        for (int i = 0; i < n; i++) {
            SxKw *k = &tab[i];
            SxNameArg na;
            memset(&na, 0, sizeof na);
            na.p = k->name;
            na.len = (uint32_t)strlen(k->name);
            void *reg = *r->registry_global;
            if (!reg) reg = r->registry_getter();
            uint32_t tok = r->token_fn(reg, &na);
            SxEntry *en = (SxEntry *)r->alloc(sizeof(SxEntry));
            if (!en) { if (logf) logf("%s %s: engine allocator returned NULL, not registered", pass ? "trigger" : "effect", k->name); continue; }
            en->vt = pass ? &sx_trg_ent_arr[1] : &sx_eff_ent_arr[1];
            en->desc = k->desc;
            en->token = tok;
            en->pad = 0;
            k->token = tok;
            k->registered = 1;          /* before the engine can create objects of it */
            (pass ? r->trg_register : r->eff_register)(tok, en);
            done++;
            if (logf) logf("registered %s %s (token %u)", pass ? "trigger" : "effect", k->name, (unsigned)tok);
        }
    }
    return done;
}

#endif
