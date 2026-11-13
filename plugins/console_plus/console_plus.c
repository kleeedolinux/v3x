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
 * filtering, sorting, paging and Tab cycling are unchanged. The id completion hooks nothing; display names
 * (below, names=1) hook the matcher and Autocomplete.
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
 * Display names (names=1, docs/re/console.md section 6): `tag Prus<Tab>` -> PRU, `own Sao_Pa<Tab>`
 * -> STATE_SAO_PAULO, event titles, laws, goods, ...; numeric ids of live states, countries,
 * characters, interest groups and buildings from their names. Hooks the matcher (cuts the token off
 * the engine's own input string in place and lets our completer return exactly the matching ids) and
 * CConsole::Autocomplete (remembers the console to print "ID  (Name, context)" lines). Localization
 * and database reads stay on the main thread inside that console call; live game objects are read
 * with ReadProcessMemory.
 *
 * Config: v3x/console_plus.ini (defaults: console_plus.ini.example).
 * Console: `complete` (status), `complete <command>` (what its arguments complete), `find <text>`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "v3x.h"
#include "v3x_rtti.h"
#include "cp_logic.h"
#include "cp_names.h"
#include "cp_wild.h"

V3X_EXPORT const char *v3x_plugin_name = "console_plus";
V3X_PLUGIN_MANIFEST("console_plus", "0.5.0", "v3x", "Tab completion and wildcards for the in-game developer console", "release/1.15.1");

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
enum { E_NAME = 0x08, E_DESC = 0x90, E_EXEC = 0xb0, E_CB = 0xb8, E_USER = 0xc0, E_ARGS = 0xd0, E_NARGSTR = 10, E_SIZE = 0x210 };
enum { DB_ARR = 0x80, DB_COUNT = 0x8c, OBJ_KEY = 0x18 };

static const char FOUND_STR[] = "> (found: {} | tab: next, esc: exit)";
static const char HELP_DESC[] = "Print out all console commands";
static const char PAT_GETTER[] = "40 53 48 83 EC 40 48 8B 05 ?? ?? ?? ?? 48 85 C0 0F 85";
static const char PAT_CBCALL[] = "49 8B 85 B8 00 00 00 4D 8B 85 C0 00 00 00 48 8D 54 24 ?? 48 8D 4D ?? FF D0";
static const char PAT_KEYLOOP[] = "48 8B 9F 80 00 00 00 48 63 87 8C 00 00 00 48 8D 3C C3 48 3B DF 74 ?? 48 8B 13 48 8B CE 48 83 C2 18 E8";
static const uint8_t PUSH_PRO[] = {0x48,0x89,0x5C,0x24,0x18,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0x48,0x8B,0xFA,
                                   0x8B,0x49,0x08,0x48,0x63,0x43,0x0C,0x3B,0xC1,0x0F,0x85};
static const uint8_t PUSH_GROW[] = {0x48,0x8B,0xC8,0x48,0xC1,0xE1,0x05,0x48,0x03,0x0B,0xE8};
/* localization: CTransactionResource<CLocalizationManager> getter (1.15.1 prologue: 24 bytes, then
 * gs:[58h] at +31), the "key -> entry" lookup of the current language's hash map (FNV over the
 * 32-bit key hash, 0x30-byte robin-hood slots, entry at slot+0x10 = {key, value} as {ptr,len,flag}) */
static const uint8_t LOC_PRO[] = {0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,
                                  0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57};
static const uint8_t LOC_PRO2[] = {0x65,0x48,0x8B,0x04,0x25,0x58,0x00,0x00,0x00};
static const char PAT_LOCUSE[] = "E8 ?? ?? ?? ?? 48 8B 98 90 00 00 00 45 8B C7 49 8B D6 E8 ?? ?? ?? ?? 89 85 ?? ?? ?? ?? 48 8D 4B 38 48 8D 95 ?? ?? ?? ?? E8";
static const uint8_t LOCFIND_PRO[] = {0x0F,0xB6,0x02,0x4C,0x8B,0xC2,0x4C,0x8B,0x51,0x08,0x35,0xC5,0x9D,0x1C,0x81,0x4C,0x63,0x59,0x14};
static const uint8_t LOCFIND_MAXP[] = {0x41,0x0F,0xB6,0x41,0x18};      /* at +0x70: movzx eax,byte [r9+18h] */
static const uint8_t LOCFIND_ENT[] = {0x48,0x8D,0x48,0x10};            /* at +0x9f: lea rcx,[rax+10h] */
/* console output line: CConsole::AddOutput(this, const CString *text, bool scroll), called by
 * Autocomplete as `mov r8b,1; lea rdx,[rsp+x]; mov rcx,r13; call` */
static const uint8_t CONPRINT_CALL[] = {0x41,0xB0,0x01,0x48,0x8D,0x54,0x24};
static const uint8_t CONPRINT_PRO[] = {0x48,0x83,0xEC,0x58,0xC5,0xF9,0xEF,0xC0};
/* engine `event` completer: [db+8] event array, [db+14h] count, name CString at event+10h */
static const uint8_t EVCB_LOOP[] = {0x48,0x8B,0x5F,0x08,0x48,0x63,0x47,0x14};
static const uint8_t EVCB_NAME[] = {0x48,0x83,0xC2,0x10};
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
    /* display names (optional: without them only ids are completed) */
    void *(*loc_get)(void);      /* CLocalizationManager* (TLS transaction getter) */
    uint8_t *loc_find;           /* checked only (we iterate the map ourselves) */
    void (*con_print)(void *console, const CStr *text, uint8_t scroll);
    char names_why[160];
    /* wildcards: CConsole::ExecuteLine(this, const CString &line) -> bool */
    uint8_t *execline;
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

static int find_db_named(const V3xRtti *r, int k, const char *db, int gamedb);
static int find_db(const V3xRtti *r, int k) { return find_db_named(r, k, CP_KINDS[k].db, 1); }

/* the one function referencing the CTransactionResource object of class `mangled` */
static uint8_t *find_resource_getter(const V3xRtti *r, const char *mangled, const char *what)
{
    void *tvt = v3x_rtti_find_vtable(r, mangled);
    if (!tvt) return api->log(TAG, "  %s: no CTransactionResource vtable", what), (uint8_t *)NULL;
    void *obj = find_data_qword(r->base, r->ptr_base + ((uintptr_t)tvt - r->base));
    if (!obj) return api->log(TAG, "  %s: no resource object", what), (uint8_t *)NULL;
    uint8_t *x = api->find_xref(obj, 0), *fs = x ? api->function_start(x) : NULL;
    if (!fs) return api->log(TAG, "  %s: resource not referenced", what), (uint8_t *)NULL;
    for (int i = 1; i < 8; i++) {
        uint8_t *y = api->find_xref(obj, i);
        if (!y) break;
        if (api->function_start(y) != fs) return api->log(TAG, "  %s: resource referenced outside its getter", what), (uint8_t *)NULL;
    }
    return fs;
}

static int find_db_named(const V3xRtti *r, int k, const char *db, int gamedb)
{
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
    if (!dvt || (gamedb && !has_gamedb_base(r, dvt, db))) return api->log(TAG, "  %s: database class is not a CGameDatabase", db), 0;
    A.dbget[k] = (db_getter_fn)fs;
    A.dbvt[k] = dvt;
    return 1;
}

static int count_db_kinds(void) { int n = 0; for (int k = 1; k < CK_COUNT; k++) n += CP_KINDS[k].db != NULL; return n; }
#define CK_DB_KINDS count_db_kinds()

/* display-name anchors: localization getter + map lookup shape, console print, event database.
 * Failure only disables names (ids are still completed). */
static void find_names_anchors(const V3xRtti *r, uint8_t *ac_end)
{
#define NFAIL(...) do { snprintf(A.names_why, sizeof A.names_why, __VA_ARGS__); api->log(TAG, "display names off: %s", A.names_why); return; } while (0)
    if (find_db_named(r, CK_EVENT, "CEventDatabase", 0)) api->log(TAG, "event database found");
    uint8_t *lg = find_resource_getter(r, ".?AV?$CTransactionResource@VCLocalizationManager@@@NPdxTransactions@@", "CLocalizationManager");
    if (!lg || memcmp(lg, LOC_PRO, sizeof LOC_PRO) || memcmp(lg + 31, LOC_PRO2, sizeof LOC_PRO2)) NFAIL("localization getter not found / shape differs");
    uint8_t *u = api->pattern_scan(PAT_LOCUSE);
    if (!u || rel32(u + 1) != lg) NFAIL("localization lookup (+0x90 language, +0x38 map) not found");
    uint8_t *lf = rel32(u + sizeof PAT_LOCUSE / 3);   /* the 4 bytes after the pattern's last E8 */
    if (memcmp(lf, LOCFIND_PRO, sizeof LOCFIND_PRO) || memcmp(lf + 0x70, LOCFIND_MAXP, sizeof LOCFIND_MAXP) ||
        memcmp(lf + 0x9f, LOCFIND_ENT, sizeof LOCFIND_ENT)) NFAIL("localization map lookup shape differs");
    /* console print: the call in Autocomplete after `mov r8b,1; lea rdx,[rsp+x]; mov rcx,r13` */
    uint8_t *pf = NULL;
    for (uint8_t *p = A.f_ac; p + 16 < ac_end && !pf; p++)
        if (!memcmp(p, CONPRINT_CALL, sizeof CONPRINT_CALL) && p[8] == 0x49 && p[9] == 0x8B && p[10] == 0xCD && p[11] == 0xE8)
            pf = rel32(p + 12);
    if (!pf || memcmp(pf, CONPRINT_PRO, sizeof CONPRINT_PRO)) NFAIL("console print not found");
    /* its first call goes to the function that creates the "console_output" line widget */
    uint8_t *inner = NULL;
    for (uint8_t *p = pf; p + 5 <= pf + 0x40 && !inner; p++) if (*p == 0xE8) inner = rel32(p + 1);
    void *co = api->find_string("console_output");
    int outline = 0;
    for (int i = 0; co && inner && i < 32 && !outline; i++) {
        uint8_t *cx = api->find_xref(co, i);
        if (!cx) break;
        outline = api->function_start(cx) == inner;
    }
    if (!outline) NFAIL("console print does not add a console_output line");
    A.loc_get = (void *(*)(void))lg;
    A.loc_find = lf;
    A.con_print = (void (*)(void *, const CStr *, uint8_t))pf;
    uintptr_t b = api->image_base();
    api->log(TAG, "display names: localization getter exe+0x%llx, map lookup exe+0x%llx, console print exe+0x%llx",
             (unsigned long long)(lg - (uint8_t *)b), (unsigned long long)(lf - (uint8_t *)b), (unsigned long long)(pf - (uint8_t *)b));
#undef NFAIL
}

static void find_wild_anchors(void);
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
        find_names_anchors(&r, fe);
        v3x_rtti_free(&r);
    }
    api->log(TAG, "%d of %d databases found", found, CK_DB_KINDS);
    if (!A.dbget[CK_COUNTRY]) FAIL("country definition database not found");
    A.ok = 1;
    find_wild_anchors();
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

/* Database of a kind (vtable checked on every call) -> element array, count, key offset.
 * CGameDatabase: T* array +0x80, count +0x8c, key CString T+0x18; CEventDatabase: event* array
 * +0x08, count +0x14, name CString event+0x10 (layout of the engine's own `event` completer). */
static int db_list(int kind, uint8_t ***arr_out, int32_t *n_out, size_t *key_off)
{
    if (kind <= 0 || kind >= CK_COUNT || !A.dbget[kind]) return 0;
    void **slot = A.dbget[kind]();
    uint8_t *db = slot ? (uint8_t *)*slot : NULL;
    if (!db || *(void **)db != A.dbvt[kind]) { InterlockedIncrement(&g_bad_db); return 0; }
    int ev = kind == CK_EVENT;
    uint8_t **arr = *(uint8_t ***)(db + (ev ? 0x08 : DB_ARR));
    int32_t n = *(int32_t *)(db + (ev ? 0x14 : DB_COUNT));
    if (!arr || n < 0 || n > 200000) { InterlockedIncrement(&g_bad_db); return 0; }
    *arr_out = arr; *n_out = n; *key_off = ev ? 0x10 : OBJ_KEY;
    return 1;
}

static void push_db_keys(CArr *out, int kind)
{
    uint8_t **arr; int32_t n; size_t ko;
    if (!db_list(kind, &arr, &n, &ko)) return;
    for (int32_t i = 0; i < n; i++) {
        uint8_t *o = arr[i];
        if (!o) continue;
        const CStr *key = (const CStr *)(o + ko);
        if (!key->size || key->size > 255 || !cstr_ptr(key)) continue;
        A.push(out, key);
        g_pushed++;
    }
}

/* ------------------------------------------------------------------ safe reads (live game state) */
/* The game state is read without the engine's game-state lock (the console's own commands take
 * it only when they execute), so every live-object read goes through ReadProcessMemory: a stale
 * pointer gives a failed read, never an access violation. */
static int safe_read(const void *addr, void *out, size_t n, void *u)
{
    (void)u;
    SIZE_T got = 0;
    if ((uintptr_t)addr < 0x10000) return 0;
    return ReadProcessMemory(GetCurrentProcess(), addr, out, n, &got) && got == n;
}
static uint64_t rq(const void *a) { uint64_t v = 0; return safe_read(a, &v, 8, NULL) ? v : 0; }

/* MSVC string (layout 0) or {ptr, int32 len} (layout 1) at p, read safely -> key text */
static int safe_key_text(const uint8_t *p, int layout, char *out, size_t cap)
{
    uint8_t b[32];
    if (!safe_read(p, b, sizeof b, NULL)) return 0;
    size_t n; char tmp[96];
    if (layout == 0) {
        uint64_t size, c; memcpy(&size, b + 16, 8); memcpy(&c, b + 24, 8);
        if (size < 2 || size > 80 || c < size || c > 4096) return 0;
        n = (size_t)size;
        if (c < 16) memcpy(tmp, b, n);
        else { uint64_t ptr; memcpy(&ptr, b, 8); if (!safe_read((void *)(uintptr_t)ptr, tmp, n, NULL)) return 0; }
    } else {
        uint64_t ptr; int32_t len; memcpy(&ptr, b, 8); memcpy(&len, b + 8, 4);
        if (len < 2 || len > 80 || !safe_read((void *)(uintptr_t)ptr, tmp, (size_t)len, NULL)) return 0;
        n = (size_t)len;
    }
    if (n >= cap) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)tmp[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ':' || c == '-')) return 0;
    }
    memcpy(out, tmp, n); out[n] = 0;
    return 1;
}

/* ------------------------------------------------------------------ localization (current language) */
enum { LOC_LANG = 0x90, LOC_MAP = 0x38, MAP_BUCKETS = 0x08, MAP_MASK = 0x14, MAP_MAXP = 0x18,
       SLOT_SZ = 0x30, SLOT_DIST = 0x04, SLOT_ENT = 0x10, ENT_VAL = 0x10 };
/* own index over the engine's map: FNV-1a of the key -> entry (key, value as {ptr, int len}) */
static struct {
    const uint8_t **ent; uint32_t *h; uint32_t cap; int n;
    const void *lang, *buckets; uint32_t mask; int gen; char lang_name[32];
} L;

static uint32_t fnv1a(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h ? h : 1;
}

static int loc_sv(const uint8_t *sv, const char **p, size_t *n)   /* {ptr, int len, flag} */
{
    const char *s = *(const char *const *)sv;
    int32_t l = *(const int32_t *)(sv + 8);
    if (!s || l < 0 || l > 65536) return 0;
    *p = s; *n = (size_t)l;
    return 1;
}

static void loc_free(void) { free(L.ent); free(L.h); L.ent = NULL; L.h = NULL; L.cap = 0; L.n = 0; L.lang = L.buckets = NULL; }

/* (re)builds the index when the current language's map changed; 1 = usable */
static int loc_ready(void)
{
    if (!A.loc_get) return 0;
    uint8_t *mgr = (uint8_t *)A.loc_get();
    uint8_t *lang = mgr ? *(uint8_t **)(mgr + LOC_LANG) : NULL;
    if (!lang) return 0;
    const uint8_t *map = lang + LOC_MAP;
    const uint8_t *b = *(const uint8_t *const *)(map + MAP_BUCKETS);
    uint32_t mask = *(const uint32_t *)(map + MAP_MASK);
    uint8_t maxp = map[MAP_MAXP];
    if (L.ent && L.lang == lang && L.buckets == b && L.mask == mask) return 1;
    if (!b || (mask & (mask + 1)) || mask > (1u << 24)) return 0;
    loc_free();
    size_t nslots = (size_t)mask + 1 + maxp;
    uint32_t cnt = 0;
    for (size_t i = 0; i < nslots; i++) if (b[i * SLOT_SZ + SLOT_DIST]) cnt++;
    uint32_t cap = 1024;
    while (cap < cnt * 2u && cap < (1u << 26)) cap <<= 1;
    L.ent = (const uint8_t **)calloc(cap, sizeof *L.ent);
    L.h = (uint32_t *)calloc(cap, sizeof *L.h);
    if (!L.ent || !L.h) { loc_free(); return 0; }
    L.cap = cap;
    for (size_t i = 0; i < nslots; i++) {
        const uint8_t *sl = b + i * SLOT_SZ;
        if (!sl[SLOT_DIST]) continue;
        const uint8_t *e = sl + SLOT_ENT;
        const char *k; size_t kn;
        if (!loc_sv(e, &k, &kn) || !kn || kn > 255) continue;
        uint32_t h = fnv1a(k, kn), j = h & (cap - 1);
        while (L.h[j]) j = (j + 1) & (cap - 1);
        L.h[j] = h; L.ent[j] = e; L.n++;
    }
    L.lang = lang; L.buckets = b; L.mask = mask; L.gen++;
    cstr_copy((const CStr *)lang, L.lang_name, sizeof L.lang_name);
    api->log(TAG, "localization %s: %d keys indexed", L.lang_name[0] ? L.lang_name : "?", L.n);
    return 1;
}

static int loc_raw(const char *key, size_t kn, const char **v, size_t *vn)
{
    if (!L.ent || !kn) return 0;
    uint32_t h = fnv1a(key, kn), j = h & (L.cap - 1);
    for (uint32_t probes = 0; L.h[j] && probes < L.cap; probes++, j = (j + 1) & (L.cap - 1)) {
        if (L.h[j] != h) continue;
        const char *k; size_t n;
        if (loc_sv(L.ent[j], &k, &n) && n == kn && !memcmp(k, key, kn)) return loc_sv(L.ent[j] + ENT_VAL, v, vn);
    }
    return 0;
}

static int loc_resolve_cb(const char *key, size_t kn, char *out, size_t cap, void *u)
{
    (void)u;
    const char *v; size_t vn;
    if (!loc_raw(key, kn, &v, &vn)) return -1;
    if (vn >= cap) vn = cap - 1;
    memcpy(out, v, vn); out[vn] = 0;
    return (int)vn;
}

/* display name of a localization key (cleaned), "" if the key has no text */
static size_t loc_name(const char *key, char *out, size_t cap)
{
    const char *v; size_t vn;
    out[0] = 0;
    if (!loc_raw(key, strlen(key), &v, &vn)) return 0;
    return cp_loc_clean(v, vn, out, cap, loc_resolve_cb, NULL);
}

/* ------------------------------------------------------------------ name indexes */
/* One index per kind. Database kinds: built once per localization generation (the databases do
 * not change while a game runs). Live kinds (numeric ids): rebuilt when older than 5 s. Each
 * entry remembers the engine object it came from (for state -> owner and similar lookups). */
static CpNames g_nm[CK_COUNT];
static uintptr_t *g_nm_obj[CK_COUNT]; static int g_nm_objcap[CK_COUNT];
static uintptr_t *g_set[CK_COUNT]; static int *g_set_idx[CK_COUNT]; static int g_nset[CK_COUNT];
static int g_nm_gen[CK_COUNT]; static ULONGLONG g_nm_time[CK_COUNT]; static int g_nm_built[CK_COUNT];

static void names_reset(int k)
{
    cp_names_free(&g_nm[k]);
    free(g_nm_obj[k]); free(g_set[k]); free(g_set_idx[k]);
    g_nm_obj[k] = NULL; g_nm_objcap[k] = 0; g_set[k] = NULL; g_set_idx[k] = NULL; g_nset[k] = 0; g_nm_built[k] = 0;
}

static int names_add(int k, const char *id, const char *name, const char *ctx, uintptr_t obj)
{
    int i = cp_names_add(&g_nm[k], id, name, ctx);
    if (i < 0) return -1;
    if (i >= g_nm_objcap[k]) {
        int nc = g_nm[k].cap > i ? g_nm[k].cap : i + 256;
        uintptr_t *no = (uintptr_t *)realloc(g_nm_obj[k], (size_t)nc * sizeof *no);
        if (!no) { g_nm[k].n--; return -1; }
        g_nm_obj[k] = no; g_nm_objcap[k] = nc;
    }
    g_nm_obj[k][i] = obj;
    return i;
}

static int cmp_pi(const void *a, const void *b)
{
    const uintptr_t *x = (const uintptr_t *)a, *y = (const uintptr_t *)b;
    return x[0] < y[0] ? -1 : x[0] > y[0];
}

/* sorted object set of kind k (field discovery and object -> entry lookups) */
static void names_finish(int k)
{
    int n = g_nm[k].n, m = 0;
    g_nm_built[k] = 1; g_nm_gen[k] = L.gen; g_nm_time[k] = GetTickCount64();
    if (!n) return;
    uintptr_t *pairs = (uintptr_t *)malloc((size_t)n * 2 * sizeof *pairs);
    g_set[k] = (uintptr_t *)malloc((size_t)n * sizeof(uintptr_t));
    g_set_idx[k] = (int *)malloc((size_t)n * sizeof(int));
    if (!pairs || !g_set[k] || !g_set_idx[k]) { free(pairs); return; }
    for (int i = 0; i < n; i++) if (g_nm_obj[k][i]) { pairs[2 * m] = g_nm_obj[k][i]; pairs[2 * m + 1] = (uintptr_t)i; m++; }
    qsort(pairs, (size_t)m, 2 * sizeof *pairs, cmp_pi);
    for (int i = 0; i < m; i++) { g_set[k][i] = pairs[2 * i]; g_set_idx[k][i] = (int)pairs[2 * i + 1]; }
    g_nset[k] = m;
    free(pairs);
}

static const CpName *entry_of(int k, uintptr_t obj)
{
    int i = obj ? cp_ptrset_find(g_set[k], g_nset[k], obj) : -1;
    return i >= 0 ? &g_nm[k].e[g_set_idx[k][i]] : NULL;
}

static void build_db_names(int k)
{
    uint8_t **arr; int32_t n; size_t ko;
    names_reset(k);
    if (db_list(k, &arr, &n, &ko))
        for (int32_t i = 0; i < n; i++) {
            uint8_t *o = arr[i];
            if (!o) continue;
            char key[256], lk[264], name[512];
            cstr_copy((const CStr *)(o + ko), key, sizeof key);
            if (!key[0]) continue;
            if (k == CK_EVENT) { snprintf(lk, sizeof lk, "%s.t", key); loc_name(lk, name, sizeof name); }
            else loc_name(key, name, sizeof name);
            names_add(k, key, name, "", (uintptr_t)o);
        }
    names_finish(k);
}

/* ---- live game objects (numeric ids) */
/* where the console's own commands find objects by id (decoded from their OnExecute) */
static struct { int ok, conflict; const uint8_t *root; uint32_t arr, cnt; char from[48]; } g_gs[CK_COUNT];
/* discovered fields (-1 unknown, -2 not found this session) */
static struct { int def, st_region, st_owner, ig_type, ig_owner, b_type, b_state, ch_owner, ch_name[2], ch_layout[2]; } F;
static void fields_reset(void) { memset(&F, 0xff, sizeof F); }

typedef struct { uint32_t id; void *p; } LiveObj;
static int live_list(int k, LiveObj **out)
{
    *out = NULL;
    if (!g_gs[k].ok || g_gs[k].conflict) return -1;
    uint8_t *root = (uint8_t *)(uintptr_t)rq(g_gs[k].root);
    uint8_t *g1 = root ? (uint8_t *)(uintptr_t)rq(root + 0x608) : NULL;
    uint8_t *gs = g1 ? (uint8_t *)(uintptr_t)rq(g1 + 0x120) : NULL;
    if (!gs) return -1;
    uint8_t *arr = (uint8_t *)(uintptr_t)rq(gs + g_gs[k].arr);
    int32_t cnt = 0;
    if (!arr || !safe_read(gs + g_gs[k].cnt, &cnt, 4, NULL) || cnt <= 0 || cnt > 4000000) return -1;
    uint64_t *slots = (uint64_t *)malloc((size_t)cnt * 16);
    LiveObj *o = (LiveObj *)malloc((size_t)cnt * sizeof *o);
    if (!slots || !o || !safe_read(arr, slots, (size_t)cnt * 16, NULL)) { free(slots); free(o); return -1; }
    int n = 0;
    for (int32_t i = 0; i < cnt; i++) if (slots[2 * i + 1]) { o[n].id = (uint32_t)i; o[n].p = (void *)(uintptr_t)slots[2 * i + 1]; n++; }
    free(slots);
    *out = o;
    return n;
}

static int discover(int *slot, LiveObj *o, int n, int target_kind, const char *what)
{
    if (*slot >= 0 || *slot == -2) return *slot;
    if (!g_nset[target_kind]) return -1;
    void **objs = (void **)malloc((size_t)n * sizeof *objs);
    if (!objs) return -1;
    for (int i = 0; i < n; i++) objs[i] = o[i].p;
    int pct = 0;
    int off = cp_discover_field(objs, n, 0x400, g_set[target_kind], g_nset[target_kind], safe_read, NULL, 60, &pct);
    free(objs);
    *slot = off >= 0 ? off : -2;
    if (off >= 0) api->log(TAG, "live objects: %s at +0x%x (%d%% of the sample)", what, off, pct);
    else api->log(TAG, "live objects: %s NOT found (best %d%%)", what, pct);
    return *slot;
}

static int ensure_names(int k);

/* character names: the two 8-aligned fields that hold localization keys most often */
static void discover_char_names(LiveObj *o, int n)
{
    static int hits[2][0x400 / 8];
    if (F.ch_name[0] != -1) return;
    memset(hits, 0, sizeof hits);
    int step = n > 128 ? n / 128 : 1, sampled = 0;
    for (int i = 0; i < n; i += step) {
        sampled++;
        for (int off = 0; off < 0x400; off += 8)
            for (int lay = 0; lay < 2; lay++) {
                char t[96]; const char *v; size_t vn;
                if (safe_key_text((uint8_t *)o[i].p + off, lay, t, sizeof t) && loc_raw(t, strlen(t), &v, &vn)) hits[lay][off / 8]++;
            }
    }
    F.ch_name[0] = F.ch_name[1] = -2;
    int best[2] = {-1, -1}, bl[2] = {0, 0}, bh[2] = {0, 0};
    for (int lay = 0; lay < 2; lay++)
        for (int k = 0; k < 0x400 / 8; k++) {
            int h = hits[lay][k];
            if (!sampled || h * 100 < sampled * 50) continue;
            if (h > bh[0]) { best[1] = best[0]; bl[1] = bl[0]; bh[1] = bh[0]; best[0] = k * 8; bl[0] = lay; bh[0] = h; }
            else if (h > bh[1]) { best[1] = k * 8; bl[1] = lay; bh[1] = h; }
        }
    if (best[0] >= 0) {
        int a = 0, b = 1;
        if (best[1] >= 0 && best[1] < best[0]) { a = 1; b = 0; }
        F.ch_name[0] = best[a]; F.ch_layout[0] = bl[a];
        F.ch_name[1] = best[b] >= 0 ? best[b] : -2; F.ch_layout[1] = bl[b];
    }
    api->log(TAG, "live objects: character name keys at %d / %d (%d sampled)", F.ch_name[0], F.ch_name[1], sampled);
}

static void build_live_names(int k)
{
    names_reset(k);
    LiveObj *o; int n = live_list(k, &o);
    if (n <= 0) { free(o); names_finish(k); return; }
    if (k != CK_N_COUNTRY) ensure_names(CK_N_COUNTRY);
    int fo = -1, fo2 = -1, tk = 0, wk = CK_N_COUNTRY;
    if (k == CK_N_COUNTRY) { tk = CK_COUNTRY; ensure_names(tk); fo = discover(&F.def, o, n, tk, "country -> definition"); }
    if (k == CK_N_STATE) { tk = CK_STATE_REGION; ensure_names(tk); fo = discover(&F.st_region, o, n, tk, "state -> state region");
                           fo2 = discover(&F.st_owner, o, n, wk, "state -> owner country"); }
    if (k == CK_N_IG) { tk = CK_IG_TYPE; ensure_names(tk); fo = discover(&F.ig_type, o, n, tk, "interest group -> type");
                        fo2 = discover(&F.ig_owner, o, n, wk, "interest group -> country"); }
    if (k == CK_N_BUILDING) { tk = CK_BUILDING; wk = CK_N_STATE; ensure_names(tk); ensure_names(wk);
                              fo = discover(&F.b_type, o, n, tk, "building -> type");
                              fo2 = discover(&F.b_state, o, n, wk, "building -> state"); }
    if (k == CK_N_CHARACTER) { fo2 = discover(&F.ch_owner, o, n, wk, "character -> country"); discover_char_names(o, n); }
    for (int i = 0; i < n; i++) {
        const uint8_t *p = (const uint8_t *)o[i].p;
        char id[16], name[512] = "", ctx[600] = "";
        snprintf(id, sizeof id, "%u", o[i].id);
        const CpName *t = (fo >= 0 && tk) ? entry_of(tk, (uintptr_t)rq(p + fo)) : NULL;
        const CpName *w = fo2 >= 0 ? entry_of(wk, (uintptr_t)rq(p + fo2)) : NULL;
        if (k == CK_N_COUNTRY) {
            if (t) { snprintf(name, sizeof name, "%s", t->name[0] ? t->name : t->id); snprintf(ctx, sizeof ctx, "%s", t->id); }
        } else if (k == CK_N_CHARACTER) {
            char a[96], b[96], na[256] = "", nb[256] = "";
            if (F.ch_name[0] >= 0 && safe_key_text(p + F.ch_name[0], F.ch_layout[0], a, sizeof a)) loc_name(a, na, sizeof na);
            if (F.ch_name[1] >= 0 && safe_key_text(p + F.ch_name[1], F.ch_layout[1], b, sizeof b)) loc_name(b, nb, sizeof nb);
            snprintf(name, sizeof name, "%s%s%s", na, na[0] && nb[0] ? " " : "", nb);
            if (w) snprintf(ctx, sizeof ctx, "%s", w->ctx);
        } else {
            if (t) snprintf(name, sizeof name, "%s", t->name[0] ? t->name : t->id);
            if (w && k == CK_N_BUILDING) snprintf(ctx, sizeof ctx, "%s %s", w->name, w->ctx);
            else if (w) snprintf(ctx, sizeof ctx, "%s", w->ctx);
        }
        names_add(k, id, name, ctx, (uintptr_t)p);
    }
    free(o);
    names_finish(k);
}

/* index of kind k (built lazily; main thread, inside the console's own completion call) */
static int ensure_names(int k)
{
    if (!g_cfg.names || k <= 0 || k >= CK_COUNT || !loc_ready()) return 0;
    if (CP_IS_NUMERIC(k)) {
        if (!g_nm_built[k] || GetTickCount64() - g_nm_time[k] > 5000) build_live_names(k);
    } else if (!g_nm_built[k] || g_nm_gen[k] != L.gen) build_db_names(k);
    return g_nm[k].n > 0;
}

static int has_names(int k) { return k > 0 && k < CK_COUNT && (CP_KINDS[k].db || k == CK_EVENT || CP_IS_NUMERIC(k)); }

/* candidates the next completer call returns instead of its usual list (set by the matcher hook
 * for a display-name match; one Tab = one matcher call = at most one completer call) */
static struct { int active; const void *rec; int n; char id[64][64]; } g_forced;
static volatile LONG g_name_tabs, g_name_hits;

/* engine ABI: void(const CString *previous_args, CPdxArray<CString> *out, void *user) */
static void cp_complete(const CStr *prev, CArr *out, void *user)
{
    CpCmd *c = (CpCmd *)user;
    if (!c || !out || GetCurrentThreadId() != g_main_tid) return;
    InterlockedIncrement(&g_calls);
    if (g_forced.active && g_forced.rec == c) {
        for (int i = 0; i < g_forced.n; i++) {
            CStr t; cstr_temp(&t, g_forced.id[i]);
            A.push(out, &t);
            g_pushed++;
        }
        g_forced.active = 0;
        return;
    }
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
    else if (CP_IS_NUMERIC(a->kind)) { /* numbers: only through display names (matcher hook) */ }
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

/* The event database layout used for event titles is the one the engine's own `event` completer
 * reads; if that completer does not look like that, event names are off (event ids still work). */
static int mem_has(const uint8_t *p, size_t n, const uint8_t *w, size_t wn)
{
    for (size_t i = 0; i + wn <= n; i++) if (!memcmp(p + i, w, wn)) return 1;
    return 0;
}
static void check_event_layout(const void *cb)
{
    if (!A.dbget[CK_EVENT]) return;
    const uint8_t *p = (const uint8_t *)cb;
    if (!p || !mem_has(p, 0x90, EVCB_LOOP, sizeof EVCB_LOOP) || !mem_has(p, 0x90, EVCB_NAME, sizeof EVCB_NAME)) {
        api->log(TAG, "engine event completer shape differs: event titles off");
        A.dbget[CK_EVENT] = NULL;
    }
}

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
        if (!strcmp(nm, "event") && *(void **)(e + E_CB) != (void *)cp_complete) { g_event_cb = *(complete_fn *)(e + E_CB); g_event_user = *(void **)(e + E_USER); }
    }
    int bad = names * 2 < n;                   /* most "names" are not strings: wrong layout */
    if (help != g_vlog_help || (engine_cb > 0) != g_vlog_cb || bad != g_vlog_bad) {
        g_vlog_help = help; g_vlog_cb = engine_cb > 0; g_vlog_bad = bad;
        api->log(TAG, "registry check (%d commands, %d readable names): help desc %s, engine completers %d -> %s",
                 n, names, help ? "ok" : "missing", engine_cb,
                 bad ? "layout differs, in-game completion OFF" : (help && engine_cb) ? "ok" : "not complete yet, retrying");
    }
    if (bad) return -1;
    if (help && engine_cb) check_event_layout((const void *)g_event_cb);
    return help && engine_cb ? 1 : 0;
}

/* after a game load the registry is complete: a still-failing check then means the layout differs */
static volatile LONG g_names_dirty;
static void on_load_transition(const char *name, void *data, void *user) { (void)name; (void)data; (void)user; g_loads++; InterlockedExchange(&g_names_dirty, 1); }

/* Where the console's own commands find objects of a numeric-id kind: decoded from the OnExecute
 * of commands whose FIRST argument is that kind and whose other arguments name nothing
 * (invalidate_state/_character/_country/_ig, the building wage command). Two commands that
 * disagree disable the kind (fail closed). */
static void learn_gs(uint8_t *e, const char *nm)
{
    char buf[E_NARGSTR][256]; const char *descs[E_NARGSTR]; int nd = 0;
    for (int k = 0; k < E_NARGSTR; k++) {
        cstr_copy((const CStr *)(e + E_ARGS + 0x20 * k), buf[nd], sizeof buf[nd]);
        if (buf[nd][0]) { descs[nd] = buf[nd]; nd++; }
    }
    CpArg a[CP_MAX_ARGS];
    int na = cp_classify_args(descs, nd, a, CP_MAX_ARGS, NULL);
    if (na < 1 || !CP_IS_NUMERIC(a[0].kind)) return;
    for (int k = 1; k < na; k++) if (a[k].kind != CK_NONE) return;
    int K = a[0].kind;
    uint8_t *ex = *(uint8_t **)(e + E_EXEC), *s0, *s1;
    uint8_t *b = (uint8_t *)api->image_base(), *be = b + api->image_size();
    if (ex < b || ex >= be || !fn_range(ex, &s0, &s1)) return;
    const uint8_t *root; uint32_t ao, co;
    if (!cp_decode_gs(s0, (size_t)(s1 - s0), &root, &ao, &co) || root < b || root >= be) return;
    if (!g_gs[K].ok) {
        g_gs[K].ok = 1; g_gs[K].root = root; g_gs[K].arr = ao; g_gs[K].cnt = co;
        snprintf(g_gs[K].from, sizeof g_gs[K].from, "%s", nm);
        api->log(TAG, "live %s: objects by id at [[[exe+0x%llx]+0x608]+0x120]+0x%x (count +0x%x), from `%s`", CP_KINDS[K].name,
                 (unsigned long long)(root - b), ao, co, nm);
    } else if (g_gs[K].root != root || g_gs[K].arr != ao || g_gs[K].cnt != co) {
        if (!g_gs[K].conflict) api->log(TAG, "live %s: `%s` and `%s` disagree -> names for this kind OFF", CP_KINDS[K].name, g_gs[K].from, nm);
        g_gs[K].conflict = 1;
    }
}

static void learn_player(uint8_t *e, const char *nm);
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
        if (g_cfg.names) learn_gs(e, nm);
        if (A.execline) learn_player(e, nm);
        /* the engine's `event` completer: ours delegates to it (same candidates) and adds names */
        int ev_ok = g_cfg.names && *cb && *cb == (void *)g_event_cb;
        if (*cb && !g_cfg.override_engine && ov < 0 && !ev_ok) continue;
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

/* ------------------------------------------------------------------ display names in the game console */
/* The engine's completer only gets the arguments BEFORE the token being typed, and the engine keeps
 * candidates that contain the token. To complete from a name, the matcher (CConsoleCmdManager,
 * 0x3dc2140 in 1.15.1: manager, CString input BY VALUE, result) is hooked: when the token matches
 * display names of the argument's kind, the token is cut off the matcher's input IN PLACE (so the
 * engine keeps every candidate) and our completer returns exactly the matching ids.
 * Ownership: the input is a temporary the caller copy-constructs (0x36be7a0 -> 0x678580) and the
 * matcher destroys itself (0x3dc296f: mov rcx,r15 = input; call CString dtor 0x67b890 -> mi_free).
 * So the engine must get back its OWN string object: shortening it (size + NUL within its buffer)
 * is safe, swapping in a string whose buffer lives in this DLL is not (mi_free on DLL memory =
 * the crash6 AV in mi_free). No other engine call here takes ownership: push_back 0x74c7f0 and the
 * console print 0x36bdd90 take const CString& (callers destroy theirs after the call).
 * One match: the engine replaces the token with the id. Several: they are listed as
 * "ID  (Name, context)" via the console's own output (CConsole* from the Autocomplete hook) and the
 * engine's "(found: N | tab: next ...)" cycles through the ids. Everything runs on the main
 * thread inside CConsole::Autocomplete; both hooks forward all arguments unchanged otherwise. */
typedef uint64_t (*fwd4_fn)(uint64_t, uint64_t, uint64_t, uint64_t);
static fwd4_fn o_matcher, o_autocomplete;
static void *g_console;                  /* CConsole* while Autocomplete runs (main thread) */
static int g_hooks;                      /* 1 matcher, 2 autocomplete */

static void game_print(const char *s)
{
    if (g_console && A.con_print) { CStr t; cstr_temp(&t, s); A.con_print(g_console, &t, 1); }
    else api->log(TAG, "%s", s);
}

static void name_line(char *o, size_t n, const CpName *e, const char *kind)
{
    const char *nm = e->name[0] ? e->name : "?";
    if (kind) {
        if (e->ctx[0]) snprintf(o, n, "  %s %s  %s (%s)", kind, e->id, nm, e->ctx);
        else snprintf(o, n, "  %s %s  %s", kind, e->id, nm);
    } else if (e->ctx[0]) snprintf(o, n, "  %s  (%s, %s)", e->id, nm, e->ctx);
    else snprintf(o, n, "  %s  (%s)", e->id, nm);
}

static int all_digits(const char *s) { if (!*s) return 0; for (; *s; s++) if (*s < '0' || *s > '9') return 0; return 1; }

/* ---- `find <text>`: every kind, best matches first */
typedef struct { int k, idx, score; } FindHit;
static int cmp_find(const void *a, const void *b)
{
    const FindHit *x = (const FindHit *)a, *y = (const FindHit *)b;
    if (x->score != y->score) return x->score - y->score;
    size_t lx = strlen(g_nm[x->k].e[x->idx].name), ly = strlen(g_nm[y->k].e[y->idx].name);
    if (lx != ly) return lx < ly ? -1 : 1;
    return x->k - y->k;
}

/* builds missing indexes only when `build` (in-game, inside the console's completion call) */
static void find_text(const char *q, int build, void (*out)(const char *))
{
    static FindHit hits[CK_COUNT * 24];
    int nh = 0, total = 0, kinds = 0;
    char ln[700];
    while (*q == ' ') q++;
    if (!*q) { out("find <text>: ids and display names of every kind (name@TAG narrows by owner)"); return; }
    for (int k = 1; k < CK_COUNT; k++) {
        if (!has_names(k)) continue;
        if (build) ensure_names(k);
        if (!g_nm_built[k] || !g_nm[k].n) continue;
        kinds++;
        CpHit h[24];
        int t = cp_names_query(&g_nm[k], q, CP_Q_IDS, h, 24);
        total += t;
        for (int i = 0; i < t && i < 24; i++) { hits[nh].k = k; hits[nh].idx = h[i].idx; hits[nh].score = h[i].score; nh++; }
    }
    if (!kinds) { out(build ? "find: no names available (no game loaded, or display names are off)" :
                      "find: nothing indexed yet - press Tab once in the game console (or use `find` there)"); return; }
    qsort(hits, (size_t)nh, sizeof *hits, cmp_find);
    snprintf(ln, sizeof ln, "find '%s': %d match%s", q, total, total == 1 ? "" : "es");
    out(ln);
    for (int i = 0; i < nh && i < 30; i++) { name_line(ln, sizeof ln, &g_nm[hits[i].k].e[hits[i].idx], CP_KINDS[hits[i].k].name); out(ln); }
    if (total > 30) { snprintf(ln, sizeof ln, "  ... %d more (more letters, or name@TAG)", total - 30); out(ln); }
}

static void print_v3x(const char *s) { api->print("%s", s); }

/* V3X console command (input thread): searches what is already indexed. Index builds stay inside
 * the game console's own completion call on the main thread. */
static void cmd_find_v3x(const char *args, void *u) { (void)u; find_text(args ? args : "", 0, print_v3x); }

/* Called with the matcher's input (engine-owned, the matcher destroys it). On a name match the
 * token is cut off in place - size shortened, NUL written inside the engine's own buffer, capacity
 * and pointer untouched - and 1 is returned; 0 = input unchanged. */
static int names_pre_match(CStr *in)
{
    g_forced.active = 0;
    if (!g_cfg.names || !in || GetCurrentThreadId() != g_main_tid) return 0;
    char *ip = (char *)cstr_ptr(in);
    if (!ip || in->size >= 1024) return 0;         /* must fit `line` untruncated */
    if (InterlockedExchange(&g_names_dirty, 0)) {
        for (int k = 1; k < CK_COUNT; k++) names_reset(k);
        fields_reset();
    }
    char line[1024];
    cstr_copy(in, line, sizeof line);
    char *sp = strchr(line, ' ');
    if (!sp) return 0;
    char cmd[64]; size_t cl = (size_t)(sp - line);
    if (cl == 0 || cl >= sizeof cmd) return 0;
    memcpy(cmd, line, cl); cmd[cl] = 0;
    if (cp_streqi(cmd, "find") && !cmd_find("find")) { find_text(sp + 1, 1, game_print); return 0; }
    CpCmd *c = cmd_find(cmd);
    if (!c) return 0;
    size_t n = strlen(line), ts = n;
    while (ts > cl && line[ts - 1] != ' ') ts--;
    const char *token = line + ts;
    char prev[1024]; size_t pl = ts > cl + 1 ? ts - cl - 1 : 0;
    memcpy(prev, line + cl + 1, pl); prev[pl] = 0;
    int ai = cp_arg_index(prev);
    if (ai >= c->nargs || strlen(token) < 2) return 0;
    int k = c->args[ai].kind;
    if (!has_names(k) || (CP_IS_NUMERIC(k) && all_digits(token))) return 0;
    InterlockedIncrement(&g_name_tabs);
    if (!ensure_names(k)) return 0;
    CpHit h[64];
    int total = cp_names_query(&g_nm[k], token, 0, h, 64);
    char ln[700];
    if (!total) {
        /* "own São Pa": a name with blanks spans several tokens; list it, the line stays */
        size_t st[CP_MAX_ARGS + 1]; int nt = 0;      /* token starts after the command */
        for (size_t i = cl + 1; i < n && nt <= CP_MAX_ARGS; i++)
            if (line[i] != ' ' && line[i - 1] == ' ') st[nt++] = i;
        for (int j = 2; j <= 4 && j <= nt && ai - j + 1 >= 0; j++) {
            int k2 = c->args[ai - j + 1].kind;
            size_t ps = st[nt - j];
            if (!has_names(k2) || !ensure_names(k2)) continue;
            int t2 = cp_names_query(&g_nm[k2], line + ps, 0, h, 15);
            if (!t2 || h[0].score > 1) continue;
            game_print("names with blanks: write them with _ or without blanks (Sao_Pa, SaoPa):");
            for (int i = 0; i < t2 && i < 15; i++) { name_line(ln, sizeof ln, &g_nm[k2].e[h[i].idx], NULL); game_print(ln); }
            return 0;
        }
        return 0;
    }
    /* ids that contain the token (what the engine alone would offer) first, then the names */
    g_forced.n = 0; g_forced.rec = c;
    if (!CP_IS_NUMERIC(k))
        for (int i = 0; i < g_nm[k].n && g_forced.n < 32; i++)
            if (cp_containsi(g_nm[k].e[i].id, strlen(g_nm[k].e[i].id), token))
                snprintf(g_forced.id[g_forced.n++], sizeof g_forced.id[0], "%s", g_nm[k].e[i].id);
    int shown = 0;
    for (int i = 0; i < total && i < 64 && g_forced.n < 64; i++) {
        const CpName *e = &g_nm[k].e[h[i].idx];
        int dup = 0;
        for (int j = 0; j < g_forced.n; j++) if (!strcmp(g_forced.id[j], e->id)) { dup = 1; break; }
        if (!dup) snprintf(g_forced.id[g_forced.n++], sizeof g_forced.id[0], "%s", e->id);
    }
    g_forced.active = 1;
    InterlockedIncrement(&g_name_hits);
    if (g_forced.n > 1) {
        for (int i = 0; i < total && i < 15; i++) { name_line(ln, sizeof ln, &g_nm[k].e[h[i].idx], NULL); game_print(ln); shown++; }
        if (total > shown) { snprintf(ln, sizeof ln, "  ... %d more (more letters, or name@TAG)", total - shown); game_print(ln); }
    }
    /* the matcher sees the line without the token: the engine keeps all our candidates.
     * In place: ts <= size <= cap, the buffer stays the engine's (SSO or its heap block). */
    ip[ts] = 0;
    in->size = ts;
    return 1;
}

static uint64_t hk_matcher(uint64_t mgr, uint64_t input, uint64_t result, uint64_t r9)
{
    names_pre_match((CStr *)(uintptr_t)input);          /* may shorten the engine's string in place */
    uint64_t r = o_matcher(mgr, input, result, r9);     /* always the engine's own object */
    g_forced.active = 0;
    return r;
}

static uint64_t hk_autocomplete(uint64_t console, uint64_t mode, uint64_t r8, uint64_t r9)
{
    void *was = g_console;
    if (GetCurrentThreadId() == g_main_tid) g_console = (void *)(uintptr_t)console;
    uint64_t r = o_autocomplete(console, mode, r8, r9);
    g_console = was;
    return r;
}

/* ------------------------------------------------------------------ country wildcards on Enter */
/* `add_liberty_desire * -100` runs the line once per subject country (of anyone), `M:*` once per
 * subject of the player's country, `TAG:*` per subject of TAG, `ALL` per country - in an argument
 * the command describes as a country. CConsole::ExecuteLine (0x36bf740 in 1.15.1, `bool (this,
 * const CString &line)`, called per queued line by 0x36c0070) is hooked: for such a line it calls
 * the original once per matching tag with the rewritten line - the same path as a typed line
 * (history, echo, result output, the command's own checks and game-state lock) - and prints one
 * summary line. The line argument is const: the engine copies it (history 0x301a470 copy-
 * constructs, the tokenizer reads) and never frees it, so the per-tag lines may live here.
 * Countries: the live country list (display-name index of country ids, tag from the definition),
 * the full id at country+0x18 and the overlord from the engine's GetOverlord map (decoded from its
 * code, read with ReadProcessMemory). The player: [[root]+0x608]+OFF as the console commands read
 * it. Only the main thread, nothing is written to the game. */
static const char PAT_EXECQ[] = "48 8B D3 48 8B CF E8 ?? ?? ?? ?? 84 C0 75 ?? 38 87 59 0D 00 00 74 ?? 48 83 C3 20 48 3B DE 75";
static const char OVL_MSG[] = "%s has no overlord. 'make_independent' requires an overlord";
#define CP_WILD_MAX 1024
static struct {
    int ok;                                /* overlord map decoded (+ execline found) */
    const uint8_t *root; uint32_t cnt, keys, vals;
    int ply_ok, ply_conflict; uint32_t ply_off; char ply_from[48];
    char why[160];
    volatile LONG lines, runs;
} W;
static fwd4_fn o_execline;

static void find_wild_anchors(void)
{
#define WFAIL(...) do { snprintf(W.why, sizeof W.why, __VA_ARGS__); api->log(TAG, "wildcards off: %s", W.why); A.execline = NULL; return; } while (0)
    memset((void *)&W, 0, sizeof W);
    A.execline = NULL;
    if (!g_cfg.wildcards) { snprintf(W.why, sizeof W.why, "wildcards=0"); return; }
    uint8_t *b = (uint8_t *)api->image_base(), *be = b + api->image_size();
    uint8_t *q = api->pattern_scan(PAT_EXECQ);
    uint8_t *xl = q ? rel32(q + 7) : NULL, *xs, *xe;
    if (!xl || xl < b || xl >= be || api->function_start(xl) != xl || !fn_range(xl, &xs, &xe) || !calls(xs, xe, A.getter))
        WFAIL("console line execution (queued-lines loop -> ExecuteLine) not found");
    void *msg = api->find_string(OVL_MSG);
    if (!msg) WFAIL("make_independent overlord check not found");
    int found = 0;
    for (int i = 0; i < 8 && !found; i++) {
        uint8_t *x = api->find_xref(msg, i), *s0, *s1;
        if (!x) break;
        if (!fn_range(x, &s0, &s1)) continue;
        for (uint8_t *p = s0; p + 5 <= s1 && !found; p++) {
            if (*p != 0xE8) continue;
            uint8_t *t = rel32(p + 1);
            if (t < b || t + 0x80 > be) continue;
            const uint8_t *root; uint32_t c, k, v;
            if (cp_decode_overlord(t, 0x80, &root, &c, &k, &v) && root >= b && root < be) {
                W.root = root; W.cnt = c; W.keys = k; W.vals = v; found = 1;
                api->log(TAG, "wildcards: GetOverlord exe+0x%llx: game state [[[exe+0x%llx]+0x608]+0x120], ids < +0x%x, keys +0x%x, values +0x%x",
                         (unsigned long long)(t - b), (unsigned long long)(root - b), c, k, v);
            }
        }
    }
    if (!found) WFAIL("GetOverlord shape differs");
    A.execline = xl;
    W.ok = 1;
    api->log(TAG, "wildcards: console ExecuteLine exe+0x%llx", (unsigned long long)(xl - b));
#undef WFAIL
}

/* player country id offset, decoded from the OnExecute of every command that reads it */
static void learn_player(uint8_t *e, const char *nm)
{
    if (!W.ok) return;
    uint8_t *ex = *(uint8_t **)(e + E_EXEC), *s0, *s1;
    uint8_t *b = (uint8_t *)api->image_base(), *be = b + api->image_size();
    if (ex < b || ex >= be || !fn_range(ex, &s0, &s1)) return;
    const uint8_t *root; uint32_t off;
    if (!cp_decode_player(s0, (size_t)(s1 - s0), &root, &off) || root != W.root) return;
    if (!W.ply_ok) {
        W.ply_ok = 1; W.ply_off = off;
        snprintf(W.ply_from, sizeof W.ply_from, "%s", nm);
        api->log(TAG, "wildcards: player country id at [[exe+0x%llx]+0x608]+0x%x, from `%s`", (unsigned long long)(root - b), off, nm);
    } else if (off != W.ply_off && !W.ply_conflict) {
        W.ply_conflict = 1;
        api->log(TAG, "wildcards: `%s` and `%s` read the player country differently -> M:* OFF", W.ply_from, nm);
    }
}

static uint32_t read_overlord(const uint8_t *gs, uint32_t id)
{
    uint32_t idx = id & 0xffffff; int32_t cnt = 0, kv[2]; uint8_t v[8];
    if (!safe_read(gs + W.cnt, &cnt, 4, NULL) || cnt <= 0 || idx >= (uint32_t)cnt) return CP_NO_ID;
    const uint8_t *keys = (const uint8_t *)(uintptr_t)rq(gs + W.keys);
    if (!keys || !safe_read(keys + (size_t)idx * 8, kv, 8, NULL) || (uint32_t)kv[0] != id || kv[1] < 0) return CP_NO_ID;
    const uint8_t *vals = (const uint8_t *)(uintptr_t)rq(gs + W.vals);
    if (!vals || !safe_read(vals + (size_t)kv[1] * 8, v, 8, NULL) || !v[4]) return CP_NO_ID;
    uint32_t ov; memcpy(&ov, v, 4);
    return ov;
}

/* live countries with tag, full id and overlord; -1 = not readable (no game, names off, layout) */
static int wild_countries(CpCountry **out, uint32_t *player, char *why, size_t wn)
{
    *out = NULL; *player = CP_NO_ID;
    if (!g_cfg.names || !A.loc_get) { snprintf(why, wn, "the country list needs display names (names=1)"); return -1; }
    if (!ensure_names(CK_N_COUNTRY)) { snprintf(why, wn, "no live countries (no game loaded?)"); return -1; }
    const uint8_t *root = (const uint8_t *)(uintptr_t)rq(W.root);
    const uint8_t *g1 = root ? (const uint8_t *)(uintptr_t)rq(root + 0x608) : NULL;
    const uint8_t *gs = g1 ? (const uint8_t *)(uintptr_t)rq(g1 + 0x120) : NULL;
    if (!gs) { snprintf(why, wn, "game state not readable"); return -1; }
    if (W.ply_ok && !W.ply_conflict) { uint32_t pid; if (safe_read(g1 + W.ply_off, &pid, 4, NULL)) *player = pid; }
    int n = g_nm[CK_N_COUNTRY].n, m = 0, bad = 0;
    CpCountry *c = (CpCountry *)calloc((size_t)(n > 0 ? n : 1), sizeof *c);
    if (!c) { snprintf(why, wn, "out of memory"); return -1; }
    for (int i = 0; i < n; i++) {
        const CpName *e = &g_nm[CK_N_COUNTRY].e[i];
        const uint8_t *obj = (const uint8_t *)g_nm_obj[CK_N_COUNTRY][i];
        uint32_t id;
        if (!obj || !e->ctx[0] || strlen(e->ctx) >= sizeof c[0].tag || !safe_read(obj + 0x18, &id, 4, NULL)) continue;
        if ((id & 0xffffff) != (uint32_t)strtoul(e->id, NULL, 10)) { bad++; continue; }
        snprintf(c[m].tag, sizeof c[m].tag, "%s", e->ctx);
        c[m].id = id;
        c[m].overlord = read_overlord(gs, id);
        m++;
    }
    if (bad > m) { free(c); snprintf(why, wn, "country ids at +0x18 do not match the id slots (%d of %d)", bad, bad + m); return -1; }
    *out = c;
    return m;
}

static void con_out(void *con, const char *s)
{
    if (con && A.con_print) { CStr t; cstr_temp(&t, s); A.con_print(con, &t, 1); }
    api->log(TAG, "%s", s);
}

static int wild_argok(const char *cmd, int argi, const char *tok, size_t tn, void *u)
{
    const CpCmd *c = (const CpCmd *)u; (void)cmd;
    if (argi >= c->nargs || c->args[argi].kind != CK_COUNTRY) return 0;
    for (int i = 0; i < c->args[argi].nlit; i++) {
        const char *w = g_pool.w[c->args[argi].lit0 + i];
        if (strlen(w) == tn && !_strnicmp(w, tok, tn)) return 0;     /* a literal word of the command */
    }
    return 1;
}

static const char *wild_what(const CpWild *w, int one, char *b, size_t n)
{
    const char *s = one ? "subject" : "subjects";
    switch (w->k) {
    case CW_SUBJECTS: return s;
    case CW_SUBJECTS_OF_PLAYER: snprintf(b, n, "%s of the player", s); return b;
    case CW_SUBJECTS_OF_TAG: snprintf(b, n, "%s of %s", s, w->tag); return b;
    default: return one ? "country" : "countries";
    }
}

/* 1 = the line had a wildcard and was handled here (*ret = ExecuteLine's result), 0 = not ours */
static int wild_exec(uint64_t con, const CStr *in, uint64_t r8, uint64_t r9, uint64_t *ret)
{
    char line[1024], cmd[64], msg[300], wb[40];
    const char *p = cstr_ptr(in);
    if (!p || !in->size || in->size >= sizeof line) return 0;
    cstr_copy(in, line, sizeof line);
    if (!strchr(line, '*') && !strstr(line, "ALL") && !strstr(line, "all") && !strstr(line, "All")) return 0;
    size_t i = 0;
    while (line[i] == ' ') i++;
    size_t cs = i;
    while (line[i] && line[i] != ' ') i++;
    if (i == cs || i - cs >= sizeof cmd) return 0;
    memcpy(cmd, line + cs, i - cs); cmd[i - cs] = 0;
    CpCmd *c = cmd_find(cmd);
    if (!c) return 0;
    size_t ts = 0, te = 0; CpWild w;
    int r = cp_wild_scan(line, wild_argok, c, &ts, &te, &w);
    if (!r) return 0;
    *ret = 0;
    InterlockedIncrement(&W.lines);
    if (r < 0) { con_out((void *)(uintptr_t)con, "console_plus: one wildcard (* M:* TAG:* ALL) per line - nothing run"); return 1; }
    CpCountry *cl; uint32_t player; char why[160];
    int n = wild_countries(&cl, &player, why, sizeof why);
    if (n < 0) { snprintf(msg, sizeof msg, "console_plus: %.*s: %s - nothing run", (int)(te - ts), line + ts, why); con_out((void *)(uintptr_t)con, msg); return 1; }
    static int idx[CP_WILD_MAX];
    int m = cp_wild_expand(&w, cl, n, player, idx, CP_WILD_MAX);
    if (m < 0) {
        if (w.k == CW_SUBJECTS_OF_PLAYER) snprintf(msg, sizeof msg, "console_plus: M:*: player country not known%s - nothing run", W.ply_ok ? "" : " (no command reads it)");
        else snprintf(msg, sizeof msg, "console_plus: %s:*: no such country - nothing run", w.tag);
        con_out((void *)(uintptr_t)con, msg);
        free(cl);
        return 1;
    }
    int ok = 0, run = 0;
    for (int k = 0; k < m; k++) {
        char one[1100];
        if (!cp_wild_line(line, ts, te, cl[idx[k]].tag, one, sizeof one)) continue;
        CStr t; cstr_temp(&t, one);                  /* const CString&: the engine only copies it */
        uint64_t res = o_execline(con, (uint64_t)(uintptr_t)&t, r8, r9);
        run++;
        ok += (uint8_t)res != 0;
    }
    InterlockedExchangeAdd(&W.runs, run);
    if (!m) snprintf(msg, sizeof msg, "console_plus: %s: no %s - nothing run", cmd, wild_what(&w, 0, wb, sizeof wb));
    else snprintf(msg, sizeof msg, "console_plus: ran %s for %d %s (%d ok)", cmd, run, wild_what(&w, run == 1, wb, sizeof wb), ok);
    con_out((void *)(uintptr_t)con, msg);
    free(cl);
    *ret = ok > 0;
    return 1;
}

static uint64_t hk_execline(uint64_t con, uint64_t line, uint64_t r8, uint64_t r9)
{
    uint64_t ret = 0;
    if (W.ok && GetCurrentThreadId() == g_main_tid && line && wild_exec(con, (const CStr *)(uintptr_t)line, r8, r9, &ret))
        return ret & 0xff;
    return o_execline(con, line, r8, r9);
}

static void install_wild_hook(void)
{
    if (!A.execline) return;
    int h = api->hook(A.execline, (void *)hk_execline, (void **)&o_execline);
    if (h != 0) { api->log(TAG, "wildcards off: ExecuteLine hook failed (%d)", h); A.execline = NULL; W.ok = 0; return; }
    api->log(TAG, "wildcards on: * (subjects) M:* (player's subjects) TAG:* ALL in country arguments");
}

static void install_name_hooks(void)
{
    if (!g_cfg.names) return;
    if (!A.loc_get) { api->log(TAG, "display names off: %s", A.names_why[0] ? A.names_why : "anchors missing"); return; }
    int h = api->hook(A.matcher, (void *)hk_matcher, (void **)&o_matcher);
    if (h != 0) { api->log(TAG, "display names off: matcher hook failed (%d)", h); A.loc_get = NULL; return; }
    g_hooks |= 1;
    if (api->hook(A.f_ac, (void *)hk_autocomplete, (void **)&o_autocomplete) == 0) g_hooks |= 2;
    else api->log(TAG, "Autocomplete hook failed: name lists go to v3x.log instead of the game console");
    api->log(TAG, "display names on (matcher hook%s)", g_hooks & 2 ? " + console output" : "");
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
    if (!g_cfg.names) api->print("  display names: off (names=0)");
    else if (!A.loc_get) api->print("  display names: OFF (%s)", A.names_why[0] ? A.names_why : "anchors missing");
    else {
        char ns[1400]; size_t o2 = 0; ns[0] = 0;
        for (int k = 1; k < CK_COUNT && o2 < sizeof ns - 40; k++)
            if (g_nm_built[k]) o2 += snprintf(ns + o2, sizeof ns - o2, " %s=%d", CP_KINDS[k].name, g_nm[k].n);
        api->print("  display names: on (hooks %s), language %s, %d localization keys, %ld name Tabs, %ld completed from names",
                   g_hooks == 3 ? "matcher+console" : g_hooks ? "matcher" : "none", L.lang_name[0] ? L.lang_name : "(not read yet)", L.n,
                   (long)g_name_tabs, (long)g_name_hits);
        api->print("  indexed:%s", ns[0] ? ns : " nothing yet (first Tab on a name builds the index)");
        char gs[400]; size_t o3 = 0; gs[0] = 0;
        for (int k = CK_N_STATE; k <= CK_N_BUILDING; k++)
            o3 += snprintf(gs + o3, sizeof gs - o3, " %s:%s", CP_KINDS[k].name, g_gs[k].conflict ? "conflict" : g_gs[k].ok ? "ok" : "unknown");
        api->print("  live objects:%s", gs);
    }
    if (!A.execline) api->print("  wildcards: off (%s)", W.why[0] ? W.why : "no in-game part");
    else api->print("  wildcards: on (* M:* TAG:* ALL in country arguments), player id %s, %ld lines expanded, %ld commands run",
                    W.ply_conflict ? "conflict" : W.ply_ok ? "known" : "not decoded yet", (long)W.lines, (long)W.runs);
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
    fields_reset();
    if (g_cfg.game_console && find_anchors()) {
        api->on_frame(on_frame, NULL);
        install_name_hooks();
        install_wild_hook();
        if (api->api_version >= 3 && api->on_event) api->on_event("load_transition", on_load_transition, NULL);
        any = 1;
    }
    if (g_cfg.v3x_console) any |= install_v3x_console();
    if (api->api_version >= 2 && api->register_command)
        api->register_command("complete", "console_plus: completion status; 'complete <game command>' shows its argument kinds", cmd_complete, NULL);
    if (api->api_version >= 2 && api->register_command && g_cfg.names)
        api->register_command("find", "console_plus: find <text> - ids + display names of countries, states, events, laws, ... (name@TAG narrows)", cmd_find_v3x, NULL);
    return any ? 0 : 1;
}
