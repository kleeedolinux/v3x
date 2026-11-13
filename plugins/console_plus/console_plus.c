/* console_plus: Tab completion for command arguments in the in-game developer console, and Tab
 * completion in the V3X console window (V3X plugin). RE notes: docs/re/console.md.
 *
 * In-game console ('~'). The engine already completes: Tab (CConsole::HandleInput, key 9) calls
 * CConsole::Autocomplete, which asks CConsoleCmdManager for candidates and shows
 * "> (found: N | tab: next, esc: exit)", Tab cycles, one match is inserted. Command NAMES are
 * always completed. For ARGUMENTS the manager calls the command's own completer
 *     void complete(const CString *previous_args, CPdxArray<CString> *out, void *user)
 * stored in the registry entry (SCommandData +0xb8, user +0xc0), then keeps the candidates that
 * contain the typed token (case-insensitive) and sorts them. In 1.15.1 only ~26 of ~300 commands
 * have one (event, mapmode, research, ai.goal, add_ideology, debug tools). So `tag SO<Tab>`,
 * `annex`, `own STATE_<Tab>` complete nothing.
 * This plugin gives every command without a completer one (main thread, on_frame, every
 * scan_frames frames while the registry grows), chosen from the engine's own argument
 * descriptions (+0xd0, e.g. "country_TAG", "province id/state region tag", "law key, country
 * tag (optional)") or from [commands] in console_plus.ini. Candidates are read inside that
 * callback, i.e. on the main thread in the engine's own console context, exactly like the
 * engine's completers do: database getter -> CGameDatabase -> T* array at +0x80, count +0x8c,
 * key CString at T+0x18 (country tags, STATE_*, law_*, goods, techs, ...), plus literal choices
 * from the description ("all/tag" -> all, "lines/ai/market/..." for `debug`). The engine's
 * filtering, sorting, paging and Tab cycling are unchanged. No engine function is hooked.
 *
 * Anchors (all must match, otherwise the in-game part does nothing and logs why):
 *   F_ac   = function referencing "> (found: {} | tab: next, esc: exit)" (CConsole::Autocomplete)
 *   getter = the only `push rbx; sub rsp,40h; mov rax,[rip+M]; test rax,rax; jnz` (manager
 *            singleton getter), called from F_ac; M = &manager pointer (read, never created)
 *   match  = the only `mov rax,[r13+0B8h]; mov r8,[r13+0C0h]; lea rdx,[rsp+x]; lea rcx,[rbp+y];
 *            call rax` (the completer call), inside a function F_ac calls
 *   push   = CPdxArray<CString>::push_back, the call in the engine's own key-listing completer
 *            loop (`mov rbx,[rdi+80h]; movsxd rax,[rdi+8Ch]; ... add rdx,18h; call push`), with
 *            its 1.15.1 prologue and grow chunk (`shl rcx,5; add rcx,[rbx]; call CString(copy)`)
 *   per kind: RTTI CTransactionResource<unique_ptr<DB>> vtable -> the .data object holding it
 *            -> the one function referencing it (getter, 1.15.1 prologue); DB vtable from RTTI
 *            with base CGameDatabase<DB,...> at offset 0 (checked again on every call)
 *   at run time: registry entries (stride 0x210) must contain "help" with its description at
 *            +0x90 and at least one engine completer at +0xb8 ("event" / "mapmode").
 * Registry writes take the manager's own lock (+0xa8, writer bit 0, readers +2) with TryEnter
 * only (never waits on the engine).
 *
 * V3X console window. The core reads lines with ReadConsoleA on its input thread; this plugin
 * hooks ReadConsoleA (all 5 arguments forwarded) and, only on the thread named
 * "v3x_console_input" and only without an input-control argument, reads the line with
 * ReadConsoleW + CONSOLE_READCONSOLE_CONTROL (wake-up on Tab). Tab completes the current word:
 * command names (from the core's `help` listing, captured once with exec_capture: it prints the
 * list once) and sub-command words written as "a|b|c" in a command's help. One match: inserted
 * with a space; several: longest common prefix, and the candidates are listed. The core starts
 * its input thread only after every plugin's v3x_plugin_load, so the hook is in place before the
 * first read (no CancelSynchronousIo: cancelling a pending console read could block forever).
 *
 * Config: v3x/console_plus.ini (defaults: console_plus.ini.example).
 * Console: `complete` (status), `complete <command>` (what its arguments complete). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "v3x.h"
#include "v3x_rtti.h"
#include "cp_logic.h"

V3X_EXPORT const char *v3x_plugin_name = "console_plus";

static const V3X_API *api;
static CpCfg g_cfg;
static DWORD g_main_tid;
#define TAG "console_plus"

/* ------------------------------------------------------------------ engine types (1.15.1) */
typedef struct { union { char buf[16]; char *ptr; } u; uint64_t size, cap; } CStr;   /* MSVC std::string */
typedef struct { void *data; int32_t cap, count; void *alloc; } CArr;               /* CPdxArray<T> */
typedef void *(*push_fn)(CArr *arr, const CStr *s);
typedef void **(*db_getter_fn)(void);
typedef void (*complete_fn)(const CStr *prev, CArr *out, void *user);

enum { MGR_DATA = 0x00, MGR_COUNT = 0x0c, MGR_LOCK = 0xa8 };
enum { E_NAME = 0x08, E_DESC = 0x90, E_CB = 0xb8, E_USER = 0xc0, E_ARGS = 0xd0, E_NARGSTR = 10, E_SIZE = 0x210 };
enum { DB_ARR = 0x80, DB_COUNT = 0x8c, OBJ_KEY = 0x18 };

static const char FOUND_STR[] = "> (found: {} | tab: next, esc: exit)";
static const char HELP_DESC[] = "Print out all console commands";
static const char PAT_GETTER[] = "40 53 48 83 EC 40 48 8B 05 ?? ?? ?? ?? 48 85 C0 0F 85";
static const char PAT_CBCALL[] = "49 8B 85 B8 00 00 00 4D 8B 85 C0 00 00 00 48 8D 54 24 ?? 48 8D 4D ?? FF D0";
static const char PAT_KEYLOOP[] = "48 8B 9F 80 00 00 00 48 63 87 8C 00 00 00 48 8D 3C C3 48 3B DF 74 ?? 48 8B 13 48 8B CE 48 83 C2 18 E8";
static const uint8_t PUSH_PRO[] = {0x48,0x89,0x5C,0x24,0x18,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x8B,0xFA,
                                   0x8B,0x49,0x08,0x48,0x63,0x43,0x0C,0x3B,0xC1,0x0F,0x85};
static const uint8_t PUSH_GROW[] = {0x48,0x8B,0xC8,0x48,0xC1,0xE1,0x05,0x48,0x03,0x0B,0xE8};
static const uint8_t DBG_PRO[] = {0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,
                                  0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x30,
                                  0x65,0x48,0x8B,0x04,0x25,0x58,0x00,0x00,0x00};

/* anchors */
static struct {
    int ok;                      /* in-game part usable */
    uint8_t *f_ac, *getter, *matcher, *cbcall;
    void **mgr_slot;
    push_fn push;
    db_getter_fn dbget[CK_COUNT];
    void *dbvt[CK_COUNT];
    char why[160];
} A;

/* ------------------------------------------------------------------ small helpers */
static uint8_t *rel32(uint8_t *insn_end_minus4) { int32_t d; memcpy(&d, insn_end_minus4, 4); return insn_end_minus4 + 4 + d; }

static int fn_range(const void *addr, uint8_t **s, uint8_t **e)
{
    DWORD64 image = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)addr, &image, NULL);
    if (!rf) return 0;
    *e = (uint8_t *)image + rf->EndAddress;   /* of the chunk containing addr */
    uint8_t *st = api->function_start(addr);
    if (!st) return 0;
    *s = st;
    if (st != (uint8_t *)image + rf->BeginAddress) {       /* addr in a chained chunk: use primary */
        PRUNTIME_FUNCTION r2 = RtlLookupFunctionEntry((DWORD64)st, &image, NULL);
        if (!r2) return 0;
        *e = (uint8_t *)image + r2->EndAddress;
    }
    return 1;
}

/* does [s,e) contain `call target`? */
static int calls(uint8_t *s, uint8_t *e, const uint8_t *target)
{
    for (uint8_t *p = s; p + 5 <= e; p++)
        if (*p == 0xE8 && rel32(p + 1) == target) return 1;
    return 0;
}

static const char *cstr_ptr(const CStr *s)
{
    if (!s || s->size > 4096 || s->cap < s->size) return NULL;
    if (s->cap >= 16) return s->u.ptr;
    return s->u.buf;
}

static void cstr_copy(const CStr *s, char *out, size_t n)
{
    const char *p = cstr_ptr(s);
    size_t k = p ? (size_t)s->size : 0;
    if (k >= n) k = n - 1;
    if (k) memcpy(out, p, k);
    out[k] = 0;
}

/* temp CString that the engine copies (push_back copy-constructs) and never frees */
static void cstr_temp(CStr *t, const char *s)
{
    size_t n = strlen(s);
    memset(t, 0, sizeof *t);
    if (n < 16) { memcpy(t->u.buf, s, n); t->size = n; t->cap = 15; }
    else { t->u.ptr = (char *)s; t->size = n; t->cap = n; }
}

/* ------------------------------------------------------------------ anchors */
static void *find_data_qword(uintptr_t base, uint64_t value)
{
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (memcmp(s[i].Name, ".data", 6) != 0) continue;
        uint32_t n = s[i].Misc.VirtualSize < s[i].SizeOfRawData ? s[i].Misc.VirtualSize : s[i].SizeOfRawData;
        for (uint32_t o = 0; o + 8 <= n; o += 8)
            if (*(const uint64_t *)(base + s[i].VirtualAddress + o) == value) return (void *)(base + s[i].VirtualAddress + o);
    }
    return NULL;
}

/* does the class of `vt` have base `.?AV?$CGameDatabase@V<db>@@` at offset 0? */
static int has_gamedb_base(const V3xRtti *r, void *vt, const char *db)
{
    uint64_t colp = ((const uint64_t *)vt)[-1];
    if (colp < r->ptr_base || colp - r->ptr_base > r->img_size) return 0;
    const uint32_t *col = (const uint32_t *)(r->base + (colp - r->ptr_base));
    const uint32_t *chd = (const uint32_t *)(r->base + col[4]);
    uint32_t nb = chd[2];
    const uint32_t *arr = (const uint32_t *)(r->base + chd[3]);
    char want[128]; snprintf(want, sizeof want, ".?AV?$CGameDatabase@V%s@@", db);
    for (uint32_t i = 0; i < nb && i < 64; i++) {
        const uint32_t *bcd = (const uint32_t *)(r->base + arr[i]);
        const char *nm = (const char *)(r->base + bcd[0] + 16);
        if (!strncmp(nm, want, strlen(want)) && bcd[2] == 0) return 1;
    }
    return 0;
}

static int find_db(const V3xRtti *r, int k)
{
    const char *db = CP_KINDS[k].db;
    char m[256];
    snprintf(m, sizeof m, ".?AV?$CTransactionResource@V?$unique_ptr@V%s@@U?$default_delete@V%s@@@std@@@std@@@NPdxTransactions@@", db, db);
    void *tvt = v3x_rtti_find_vtable(r, m);
    if (!tvt) return api->log(TAG, "  %s: no CTransactionResource vtable", db), 0;
    void *obj = find_data_qword(r->base, r->ptr_base + ((uintptr_t)tvt - r->base));
    if (!obj) return api->log(TAG, "  %s: no resource object", db), 0;
    uint8_t *x = api->find_xref(obj, 0), *fs = x ? api->function_start(x) : NULL;
    if (!fs || memcmp(fs, DBG_PRO, sizeof DBG_PRO) != 0) return api->log(TAG, "  %s: getter shape differs (xref %p)", db, (void *)x), 0;
    for (int i = 1; i < 8; i++) {
        uint8_t *y = api->find_xref(obj, i);
        if (!y) break;
        if (api->function_start(y) != fs) return api->log(TAG, "  %s: resource referenced outside its getter", db), 0;
    }
    void *dvt = v3x_rtti_find_vtable(r, db);
    if (!dvt || !has_gamedb_base(r, dvt, db)) return api->log(TAG, "  %s: database class is not a CGameDatabase", db), 0;
    A.dbget[k] = (db_getter_fn)fs;
    A.dbvt[k] = dvt;
    return 1;
}

static int find_anchors(void)
{
    memset(&A, 0, sizeof A);
#define FAIL(...) do { snprintf(A.why, sizeof A.why, __VA_ARGS__); api->log(TAG, "in-game completion off: %s", A.why); return 0; } while (0)
    void *s = api->find_string(FOUND_STR);
    uint8_t *x = s ? api->find_xref(s, 0) : NULL;
    uint8_t *fe;
    if (!x || !fn_range(x, &A.f_ac, &fe)) FAIL("autocomplete UI string/function not found");
    if (api->find_xref(s, 1)) FAIL("autocomplete UI string referenced twice");
    A.getter = api->pattern_scan(PAT_GETTER);
    if (!A.getter || !calls(A.f_ac, fe, A.getter)) FAIL("command manager getter not found / not called by the UI");
    A.mgr_slot = (void **)rel32(A.getter + 9);
    A.cbcall = api->pattern_scan(PAT_CBCALL);
    uint8_t *ms, *me;
    if (!A.cbcall || !fn_range(A.cbcall, &ms, &me) || !calls(A.f_ac, fe, ms)) FAIL("completer call (+0xb8/+0xc0) not found");
    A.matcher = ms;
    uint8_t *k = api->pattern_scan(PAT_KEYLOOP);
    uint8_t *push = k ? rel32(k + 34) : NULL;   /* E8 is the pattern's last byte */
    if (!push || memcmp(push, PUSH_PRO, sizeof PUSH_PRO) != 0) FAIL("CPdxArray<CString>::push_back not found");
    uint8_t *grow = rel32(push + sizeof PUSH_PRO);
    if (memcmp(grow, PUSH_GROW, sizeof PUSH_GROW) != 0) FAIL("push_back shape differs");
    A.push = (push_fn)push;
    uintptr_t b = api->image_base();
    api->log(TAG, "autocomplete exe+0x%llx, manager getter exe+0x%llx (slot exe+0x%llx), matcher exe+0x%llx, push exe+0x%llx",
             (unsigned long long)(A.f_ac - (uint8_t *)b), (unsigned long long)(A.getter - (uint8_t *)b),
             (unsigned long long)((uint8_t *)A.mgr_slot - (uint8_t *)b), (unsigned long long)(A.matcher - (uint8_t *)b),
             (unsigned long long)(push - (uint8_t *)b));
    /* databases */
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(b + ((const IMAGE_DOS_HEADER *)b)->e_lfanew);
    uint64_t ptr_bases[2] = { b, nt->OptionalHeader.ImageBase };
    int found = 0;
    for (int pb = 0; pb < 2 && !found; pb++) {
        if (pb == 1 && ptr_bases[1] == ptr_bases[0]) break;
        V3xRtti r;
        if (v3x_rtti_build_ex(&r, b, api->image_size(), ptr_bases[pb]) != 0) continue;
        if (!v3x_rtti_find_vtable(&r, "CCountryDefinitionDatabase")) { v3x_rtti_free(&r); continue; }
        for (int kd = 1; kd < CK_COUNT; kd++) if (CP_KINDS[kd].db) found += find_db(&r, kd);
        v3x_rtti_free(&r);
    }
    api->log(TAG, "%d of %d databases found", found, CK_COUNT - 2);
    if (!A.dbget[CK_COUNTRY]) FAIL("country definition database not found");
    A.ok = 1;
    return 1;
#undef FAIL
}

/* ------------------------------------------------------------------ in-game completer */
#define CP_CMD_MAX 512
typedef struct { char name[48]; CpArg args[CP_MAX_ARGS]; int nargs; int from_ini; int replaced_engine; } CpCmd;
static CpCmd g_cmds[CP_CMD_MAX];
static int g_ncmd;
static CpPool g_pool;
static complete_fn g_event_cb; static void *g_event_user;
static volatile LONG g_calls, g_pushed, g_bad_db;
static int g_last_count = -1, g_validated, g_invalid, g_frame;

static void push_db_keys(CArr *out, int kind)
{
    if (!A.dbget[kind]) return;
    void **slot = A.dbget[kind]();
    uint8_t *db = slot ? (uint8_t *)*slot : NULL;
    if (!db || *(void **)db != A.dbvt[kind]) { InterlockedIncrement(&g_bad_db); return; }
    uint8_t **arr = *(uint8_t ***)(db + DB_ARR);
    int32_t n = *(int32_t *)(db + DB_COUNT);
    if (!arr || n < 0 || n > 200000) { InterlockedIncrement(&g_bad_db); return; }
    for (int32_t i = 0; i < n; i++) {
        uint8_t *o = arr[i];
        if (!o) continue;
        const CStr *key = (const CStr *)(o + OBJ_KEY);
        if (!key->size || key->size > 255 || !cstr_ptr(key)) continue;
        A.push(out, key);
        g_pushed++;
    }
}

/* engine ABI: void(const CString *previous_args, CPdxArray<CString> *out, void *user) */
static void cp_complete(const CStr *prev, CArr *out, void *user)
{
    CpCmd *c = (CpCmd *)user;
    if (!c || !out || GetCurrentThreadId() != g_main_tid) return;
    InterlockedIncrement(&g_calls);
    char pv[512]; cstr_copy(prev, pv, sizeof pv);
    int ai = cp_arg_index(pv);
    if (ai >= c->nargs) return;
    const CpArg *a = &c->args[ai];
    for (int i = 0; i < a->nlit; i++) {
        CStr t; cstr_temp(&t, g_pool.w[a->lit0 + i]);
        A.push(out, &t);
        g_pushed++;
    }
    if (a->kind == CK_EVENT) { if (g_event_cb) g_event_cb(prev, out, g_event_user); }
    else if (a->kind != CK_NONE) push_db_keys(out, a->kind);
}

static CpCmd *cmd_find(const char *name)
{
    for (int i = 0; i < g_ncmd; i++) if (cp_streqi(g_cmds[i].name, name)) return &g_cmds[i];
    return NULL;
}

/* registry writer lock: bit 0 = writer, readers add 2. TryEnter only. */
static int reg_lock(volatile LONG *lk)
{
    int spins = 0;
    while (InterlockedBitTestAndSet(lk, 0)) { if (++spins > 2000) return 0; YieldProcessor(); }
    spins = 0;
    while (*lk > 1) { if (++spins > 200000) { InterlockedAnd(lk, ~1); return 0; } YieldProcessor(); }
    return 1;
}
static void reg_unlock(volatile LONG *lk) { InterlockedAnd(lk, ~1); }

/* Layout check before the first write (fail closed). The registry fills up during start-up
 * (at the main menu `help`/`event`/`mapmode` may not be registered yet), so a "not yet" result is
 * retried on every scan; it never writes until the check passes.
 * Returns 1 ok, 0 not yet (retry), -1 layout really differs (names are not CStrings). */
static int g_vlog_help = -1, g_vlog_cb = -1, g_vlog_bad = -1, g_vfails_after_load, g_loads;
static int validate(uint8_t *data, int n)
{
    int help = 0, engine_cb = 0, names = 0;
    for (int i = 0; i < n; i++) {
        uint8_t *e = data + (size_t)i * E_SIZE;
        const CStr *ns = (const CStr *)(e + E_NAME);
        const char *np = cstr_ptr(ns);
        if (np && ns->size > 0 && ns->size < 64) {
            int pr = 1;
            for (uint64_t k = 0; k < ns->size; k++) if ((unsigned char)np[k] < 0x20 || (unsigned char)np[k] > 0x7e) { pr = 0; break; }
            names += pr;
        }
        char nm[64]; cstr_copy(ns, nm, sizeof nm);
        if (!strcmp(nm, "help")) {
            char d[128]; cstr_copy((const CStr *)(e + E_DESC), d, sizeof d);
            help = !strncmp(d, HELP_DESC, sizeof HELP_DESC - 1);
        }
        if ((!strcmp(nm, "event") || !strcmp(nm, "mapmode")) && *(void **)(e + E_CB)) engine_cb++;
        if (!strcmp(nm, "event")) { g_event_cb = *(complete_fn *)(e + E_CB); g_event_user = *(void **)(e + E_USER); }
    }
    int bad = names * 2 < n;                   /* most "names" are not strings: wrong layout */
    if (help != g_vlog_help || (engine_cb > 0) != g_vlog_cb || bad != g_vlog_bad) {
        g_vlog_help = help; g_vlog_cb = engine_cb > 0; g_vlog_bad = bad;
        api->log(TAG, "registry check (%d commands, %d readable names): help desc %s, engine completers %d -> %s",
                 n, names, help ? "ok" : "missing", engine_cb,
                 bad ? "layout differs, in-game completion OFF" : (help && engine_cb) ? "ok" : "not complete yet, retrying");
    }
    if (bad) return -1;
    return help && engine_cb ? 1 : 0;
}

/* after a game load the registry is complete: a still-failing check then means the layout differs */
static void on_load_transition(const char *name, void *data, void *user) { (void)name; (void)data; (void)user; g_loads++; }

static void scan_registry(void)
{
    uint8_t *mgr = (uint8_t *)*A.mgr_slot;
    if (!mgr) return;
    uint8_t *data = *(uint8_t **)(mgr + MGR_DATA);
    int n = *(int32_t *)(mgr + MGR_COUNT);
    if (!data || n <= 0 || n > 4096 || (g_validated && n == g_last_count)) return;
    volatile LONG *lk = (volatile LONG *)(mgr + MGR_LOCK);
    if (!reg_lock(lk)) return;                 /* busy: next time */
    data = *(uint8_t **)(mgr + MGR_DATA);
    n = *(int32_t *)(mgr + MGR_COUNT);
    if (!g_validated) {
        int v = validate(data, n);
        if (v <= 0) {
            if (v < 0) g_invalid = 1;
            else if (g_loads && ++g_vfails_after_load >= 5) {
                g_invalid = 1;
                api->log(TAG, "registry check still failing after a game load: in-game completion OFF");
            }
            reg_unlock(lk);
            return;
        }
        g_validated = 1;
    }
    int added = 0;
    for (int i = 0; i < n && g_ncmd < CP_CMD_MAX; i++) {
        uint8_t *e = data + (size_t)i * E_SIZE;
        void **cb = (void **)(e + E_CB), **usr = (void **)(e + E_USER);
        if (*cb == (void *)cp_complete) continue;
        char nm[48]; cstr_copy((const CStr *)(e + E_NAME), nm, sizeof nm);
        if (!nm[0]) continue;
        int ov = cp_cfg_override(&g_cfg, nm);
        if (*cb && !g_cfg.override_engine && ov < 0) continue;
        CpCmd *rec = cmd_find(nm);             /* same name registered twice: share the record */
        if (!rec) {
            CpCmd c; memset(&c, 0, sizeof c);
            snprintf(c.name, sizeof c.name, "%s", nm);
            if (ov >= 0) {
                c.from_ini = 1;
                for (int k = 0; k < g_cfg.ovr[ov].nkinds; k++) c.args[k].kind = g_cfg.ovr[ov].kinds[k];
                c.nargs = g_cfg.ovr[ov].nkinds;
            } else {
                char buf[E_NARGSTR][256]; const char *descs[E_NARGSTR]; int nd = 0;
                for (int k = 0; k < E_NARGSTR; k++) {
                    cstr_copy((const CStr *)(e + E_ARGS + 0x20 * k), buf[nd], sizeof buf[nd]);
                    if (buf[nd][0]) { descs[nd] = buf[nd]; nd++; }
                }
                c.nargs = cp_classify_args(descs, nd, c.args, CP_MAX_ARGS, &g_pool);
            }
            if (!cp_any_arg(c.args, c.nargs)) continue;
            c.replaced_engine = *cb != NULL;
            rec = &g_cmds[g_ncmd++];
            *rec = c;
        }
        *usr = rec;                            /* user first, the callback last */
        MemoryBarrier();
        *cb = (void *)cp_complete;
        added++;
        const CpCmd c = *rec;
        if (g_cfg.log_installs) {
            char ks[160]; size_t o = 0; ks[0] = 0;
            for (int k = 0; k < c.nargs && o < sizeof ks - 24; k++)
                o += snprintf(ks + o, sizeof ks - o, "%s%s%s", k ? " " : "", CP_KINDS[c.args[k].kind].name, c.args[k].nlit ? "+words" : "");
            api->log(TAG, "  %s: %s%s%s", nm, ks, c.from_ini ? " (ini)" : "", c.replaced_engine ? " (replaces engine completer)" : "");
        }
    }
    g_last_count = n;
    reg_unlock(lk);
    if (added) api->log(TAG, "registry %d commands: completion added to %d (total %d)", n, added, g_ncmd);
}

static void on_frame(void *u)
{
    (void)u;
    if (!A.ok || g_invalid) return;
    if (g_frame++ % g_cfg.scan_frames) return;
    scan_registry();
}

/* ------------------------------------------------------------------ V3X console window */
typedef CONSOLE_READCONSOLE_CONTROL CpReadCtl;
typedef BOOL(WINAPI *ReadConsoleA_t)(HANDLE, LPVOID, DWORD, LPDWORD, void *);
typedef HRESULT(WINAPI *GetThreadDescription_t)(HANDLE, PWSTR *);
static ReadConsoleA_t o_ReadConsoleA;
static GetThreadDescription_t p_GetThreadDescription;
static DWORD g_input_tid;
static HANDLE g_conout = INVALID_HANDLE_VALUE;
static CpHelp g_help; static int g_help_state;   /* 0 not read, 1 read */
static char g_help_text[16384]; static size_t g_help_len;
static volatile LONG g_tabs;

static int is_thread_named(HANDLE th, const wchar_t *want)
{
    PWSTR d = NULL;
    if (!p_GetThreadDescription || FAILED(p_GetThreadDescription(th, &d)) || !d) return 0;
    int r = !wcscmp(d, want);
    LocalFree(d);
    return r;
}

static int on_input_thread(void)
{
    DWORD t = GetCurrentThreadId();
    if (g_input_tid) return t == g_input_tid;
    if (is_thread_named(GetCurrentThread(), L"v3x_console_input")) { g_input_tid = t; return 1; }
    return 0;
}

static void help_sink(const char *text, void *u)
{
    (void)u;
    size_t n = strlen(text);
    if (g_help_len + n < sizeof g_help_text) { memcpy(g_help_text + g_help_len, text, n); g_help_len += n; g_help_text[g_help_len] = 0; }
}

static int v3x_cands(int argi, const char *cmd, const char *const **out, void *u)
{
    (void)u;
    if (argi == 0) { *out = g_help.names; return g_help.n; }
    if (argi == 1) { int i = cp_help_find(&g_help, cmd); if (i >= 0) { *out = g_help.subs[i]; return g_help.nsub[i]; } }
    return 0;
}

static void con_write(const char *s) { DWORD w; WriteConsoleA(g_conout, s, (DWORD)strlen(s), &w, NULL); }

/* rewrite the input line from `start`: text, then blanks over `clear` old cells */
static void con_redraw(COORD *start, const char *text, int clear)
{
    CONSOLE_SCREEN_BUFFER_INFO bi;
    SetConsoleCursorPosition(g_conout, *start);
    con_write(text);
    if (!GetConsoleScreenBufferInfo(g_conout, &bi)) return;
    COORD end = bi.dwCursorPosition;
    int w = bi.dwSize.X > 0 ? bi.dwSize.X : 80;
    int len = (int)strlen(text);
    /* the buffer may have scrolled: recompute where the line starts */
    int lines = (start->X + len) / w;
    start->Y = (SHORT)(end.Y - lines);
    if (start->Y < 0) start->Y = 0;
    int pad = clear - len;
    if (pad > 0) {
        char sp[256]; if (pad > (int)sizeof sp - 1) pad = (int)sizeof sp - 1;
        memset(sp, ' ', (size_t)pad); sp[pad] = 0;
        con_write(sp);
    }
    SetConsoleCursorPosition(g_conout, end);
}

static int cells_between(COORD a, COORD b, int w) { return (b.Y - a.Y) * w + (b.X - a.X); }

/* ReadConsoleA replacement for the V3X input thread: line editing by the console, Tab wakes us */
static BOOL tab_read(HANDLE in, char *out, DWORD outsz, LPDWORD nread)
{
    static wchar_t wb[2048];
    char line[1024];
    DWORD init = 0;
    CONSOLE_SCREEN_BUFFER_INFO bi;
    COORD start = {0, 0};
    if (GetConsoleScreenBufferInfo(g_conout, &bi)) start = bi.dwCursorPosition;
    for (;;) {
        CpReadCtl ctl = { sizeof ctl, init, 1u << '\t', 0 };
        DWORD got = 0;
        if (!ReadConsoleW(in, wb, (DWORD)(sizeof wb / sizeof wb[0]) - 1, &got, &ctl)) return FALSE;
        DWORD tab = got;
        for (DWORD i = 0; i < got; i++) if (wb[i] == L'\t') { tab = i; break; }
        if (tab == got) {                       /* Enter (or EOF): hand the line to the core */
            int n = WideCharToMultiByte(GetConsoleCP(), 0, wb, (int)got, out, (int)outsz - 1, NULL, NULL);
            if (n < 0) n = 0;
            *nread = (DWORD)n;
            return TRUE;
        }
        InterlockedIncrement(&g_tabs);
        COORD cur = start;
        if (GetConsoleScreenBufferInfo(g_conout, &bi)) cur = bi.dwCursorPosition;
        int w = bi.dwSize.X > 0 ? bi.dwSize.X : 80;
        int shown = cells_between(start, cur, w);
        /* prefix (up to the Tab) and the rest of the line after it */
        int pn = WideCharToMultiByte(GetConsoleCP(), 0, wb, (int)tab, line, (int)sizeof line - 1, NULL, NULL);
        line[pn > 0 ? pn : 0] = 0;
        char rest[512]; int rn = 0;
        if (got > tab + 1) rn = WideCharToMultiByte(GetConsoleCP(), 0, wb + tab + 1, (int)(got - tab - 1), rest, (int)sizeof rest - 1, NULL, NULL);
        rest[rn > 0 ? rn : 0] = 0;
        if (g_help_state == 0 && api->api_version >= 3 && api->exec_capture) {
            g_help_state = 1;
            con_write("\n");
            api->exec_capture("help", help_sink, NULL);   /* prints the list once */
            cp_parse_help(g_help_text, &g_help);
            if (GetConsoleScreenBufferInfo(g_conout, &bi)) start = bi.dwCursorPosition;
            shown = 0;
        }
        CpResult r;
        cp_complete_line(line, v3x_cands, NULL, &r);
        if (r.nmatch > 1) {
            /* list the candidates under the line, then start a fresh input line */
            SetConsoleCursorPosition(g_conout, cur);
            char lst[2048]; size_t o = 0;
            o += snprintf(lst + o, sizeof lst - o, "\n");
            int shown_m = r.nmatch < 64 ? r.nmatch : 64;
            for (int i = 0; i < shown_m && o < sizeof lst - 40; i++) o += snprintf(lst + o, sizeof lst - o, "%s  ", r.cands[r.idx[i]]);
            if (r.nmatch > shown_m) o += snprintf(lst + o, sizeof lst - o, "... (%d)", r.nmatch);
            snprintf(lst + o, sizeof lst - o, "\n");
            con_write(lst);
            if (GetConsoleScreenBufferInfo(g_conout, &bi)) start = bi.dwCursorPosition;
            shown = 0;
        }
        char nl[1536];
        snprintf(nl, sizeof nl, "%s%s", r.line, rest);
        con_redraw(&start, r.line, shown);
        if (rest[0]) { con_write(rest); }
        int wn = MultiByteToWideChar(GetConsoleCP(), 0, nl, -1, wb, (int)(sizeof wb / sizeof wb[0]) - 1);
        init = wn > 0 ? (DWORD)(wn - 1) : 0;
    }
}

static BOOL WINAPI hk_ReadConsoleA(HANDLE in, LPVOID buf, DWORD n, LPDWORD nread, void *ctl)
{
    if (ctl || !buf || !nread || n < 4 || g_conout == INVALID_HANDLE_VALUE || !on_input_thread())
        return o_ReadConsoleA(in, buf, n, nread, ctl);
    return tab_read(in, (char *)buf, n, nread);
}

static int install_v3x_console(void)
{
    p_GetThreadDescription = (GetThreadDescription_t)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    if (!p_GetThreadDescription) return api->log(TAG, "V3X console: GetThreadDescription missing, Tab completion off"), 0;
    void *rc = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "ReadConsoleA");
    if (!rc) return 0;
    g_conout = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (g_conout == INVALID_HANDLE_VALUE) return api->log(TAG, "V3X console: no console window, Tab completion off"), 0;
    int h = api->hook(rc, (void *)hk_ReadConsoleA, (void **)&o_ReadConsoleA);
    if (h != 0) { CloseHandle(g_conout); g_conout = INVALID_HANDLE_VALUE; return api->log(TAG, "V3X console: hook failed (%d)", h), 0; }
    api->log(TAG, "V3X console: Tab completion on");
    return 1;
}

/* ------------------------------------------------------------------ V3X command `complete` */
static void cmd_complete(const char *args, void *u)
{
    (void)u;
    if (args && *args) {
        const CpCmd *c = cmd_find(args);
        if (!c) { api->print("%s: no completion installed (unknown, no argument kinds, or engine's own completer)", args); return; }
        api->print("%s%s:", c->name, c->from_ini ? " (from console_plus.ini)" : "");
        for (int k = 0; k < c->nargs; k++) {
            char w[512]; size_t o = 0; w[0] = 0;
            for (int i = 0; i < c->args[k].nlit && o < sizeof w - 32; i++) o += snprintf(w + o, sizeof w - o, " %s", g_pool.w[c->args[k].lit0 + i]);
            api->print("  arg %d: %s%s%s", k + 1, CP_KINDS[c->args[k].kind].name, w[0] ? " + words:" : "", w);
        }
        return;
    }
    char st[200];
    if (!A.ok) snprintf(st, sizeof st, "OFF (%s)", A.why[0] ? A.why : "disabled in ini");
    else snprintf(st, sizeof st, "%s", g_invalid ? "OFF (registry layout differs)" : g_validated ? "on" : "waiting for the command registry");
    api->print("console_plus: in-game %s, %d commands completed, %ld completer calls, %ld candidates pushed, %ld bad db reads",
               st, g_ncmd, (long)g_calls, (long)g_pushed, (long)g_bad_db);
    char ks[1024]; size_t o = 0; ks[0] = 0;
    for (int k = 1; k < CK_COUNT; k++)
        if (CP_KINDS[k].db) o += snprintf(ks + o, sizeof ks - o, " %s%s", CP_KINDS[k].name, A.dbget[k] ? "" : "(missing)");
    api->print("  kinds:%s event(%s)", ks, g_event_cb ? "engine" : "missing");
    api->print("  V3X console Tab completion: %s, %ld Tabs", g_conout != INVALID_HANDLE_VALUE ? "on" : "off", (long)g_tabs);
    char cs[2048]; o = 0; cs[0] = 0;
    for (int i = 0; i < g_ncmd && o < sizeof cs - 64; i++) o += snprintf(cs + o, sizeof cs - o, "%s ", g_cmds[i].name);
    if (g_ncmd) api->print("  commands: %s", cs);
}

/* ------------------------------------------------------------------ load */
static void load_cfg(void)
{
    cp_cfg_defaults(&g_cfg);
    char p[MAX_PATH]; snprintf(p, sizeof p, "%sconsole_plus.ini", api->v3x_dir());
    FILE *f = fopen(p, "rb");
    if (!f) return;
    static char text[32768];
    size_t n = fread(text, 1, sizeof text - 1, f); fclose(f);
    text[n] = 0;
    cp_parse_ini(text, &g_cfg);
    if (g_cfg.bad_lines) api->log(TAG, "console_plus.ini: %d lines ignored", g_cfg.bad_lines);
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    g_main_tid = GetCurrentThreadId();
    load_cfg();
    if (!g_cfg.enabled) { api->log(TAG, "disabled in console_plus.ini"); return 0; }
    int any = 0;
    if (g_cfg.game_console && find_anchors()) {
        api->on_frame(on_frame, NULL);
        if (api->api_version >= 3 && api->on_event) api->on_event("load_transition", on_load_transition, NULL);
        any = 1;
    }
    if (g_cfg.v3x_console) any |= install_v3x_console();
    if (api->api_version >= 2 && api->register_command)
        api->register_command("complete", "console_plus: completion status; 'complete <game command>' shows its argument kinds", cmd_complete, NULL);
    return any ? 0 : 1;
}
