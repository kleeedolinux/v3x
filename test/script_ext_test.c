/* Offline test of plugins/script_ext (no game run).
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -Isdk -o $T/sxt.exe test/script_ext_test.c
 *   $T/sxt.exe <empty temp dir> [path/to/victoria3.exe]
 * A: pack name / relative path validation.  B: BOM rule for *.yml.
 * C: begin/write/end in a temp content_packs tree: folders, atomic overwrite, BOM, manifest, stale files
 *    of the previous session deleted, foreign files kept, errors.
 * D: v3x_plugin_load with the testkit API (root fallback to v3x_dir, console command, export table).
 * E (with the exe): the RE anchors documented in docs/re/script.md still match the exe (RTTI vtables,
 *    strings), read-only on the image mapped as data.
 * F (with the exe): layer A anchors (sx_engine_resolve) resolve uniquely to the documented addresses; broken
 *    anchors switch the feature off.  G: layer A control flow against FAKE engine functions (order of calls,
 *    random scope take/restore, error paths, thread / re-entrancy / fault cleanup, console commands).
 * H: layer C name rules, queueing, thread rule, registration flow against fake engine objects (entry and
 *    object vtables, Create swap, Execute / Evaluate wrappers).  I (with the exe): layer C anchors resolve
 *    uniquely, vtable slot counts (2 / 3 / 26 / 26) and that every slot we copy is code.
 * Exit 0 = ALL OK. Nothing here runs engine code. */
#include "v3x_testkit.h"
#include "v3x_rtti.h"
#include "../plugins/script_ext/script_ext.c"

static int file_eq(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[4096];
    size_t k = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return k == n && !memcmp(buf, data, n);
}
static int exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

static void part_a(void)
{
    V3X_TEST_CHECK(sx_valid_pack("my_pack") && sx_valid_pack("A-1.b"), "A valid pack names");
    V3X_TEST_CHECK(!sx_valid_pack("") && !sx_valid_pack("_x") && !sx_valid_pack(".x") && !sx_valid_pack("a/b") &&
                   !sx_valid_pack("a b") && !sx_valid_pack(NULL), "A bad pack names rejected");
    char long64[70]; memset(long64, 'a', 64); long64[64] = 0;
    V3X_TEST_CHECK(!sx_valid_pack(long64), "A pack name > 63 chars rejected");
    V3X_TEST_CHECK(sx_valid_rel("common/scripted_effects/x.txt") && sx_valid_rel("events\\e.txt") &&
                   sx_valid_rel("localization/english/x_l_english.yml"), "A valid relative paths");
    const char *bad[] = {"", "/abs.txt", "\\abs.txt", "C:/x.txt", "a/../b.txt", "../x", "a//b", "a/./b", "a/b.",
                         "x?.txt", "a\tb", "v3x_script_ext.manifest", "a/b ", NULL};
    int all = 1;
    for (int i = 0; bad[i]; i++) if (sx_valid_rel(bad[i])) { printf("  accepted: '%s'\n", bad[i]); all = 0; }
    V3X_TEST_CHECK(all && !sx_valid_rel(NULL), "A unsafe paths rejected (absolute, drive, .., empty parts, trailing dot, wildcards, manifest)");
    char longp[260]; memset(longp, 'a', 201); longp[201] = 0;
    V3X_TEST_CHECK(!sx_valid_rel(longp), "A path > 200 chars rejected");
}

static void part_b(void)
{
    V3X_TEST_CHECK(sx_needs_bom("l/x_l_english.yml", "l_english:", 10), "B .yml without BOM gets one");
    V3X_TEST_CHECK(!sx_needs_bom("l/x.YML", "\xEF\xBB\xBFl_english:", 13), "B .yml with BOM unchanged");
    V3X_TEST_CHECK(!sx_needs_bom("common/x.txt", "a", 1), "B .txt never gets a BOM");
}

static void part_c(const char *tmp)
{
    snprintf(sx_root, sizeof sx_root, "%s\\c_packs\\", tmp);
    CreateDirectoryA(sx_root, NULL);
    const char *eff = "v3x_test_effect = { debug_log = \"hello\" }\n";
    V3X_TEST_CHECK(sx_write_file("p1", "common/a.txt", "x", 1) == V3X_SCRIPT_ESTATE, "C write before begin_pack -> ESTATE");
    V3X_TEST_CHECK(sx_begin_pack("p1") == 0, "C begin_pack p1");
    V3X_TEST_CHECK(sx_write_file("p1", "common/scripted_effects/v3x_test.txt", eff, strlen(eff)) == 0, "C write nested file");
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%sp1\\common\\scripted_effects\\v3x_test.txt", sx_root);
    V3X_TEST_CHECK(file_eq(p, eff, strlen(eff)), "C file content exact, folders created");
    V3X_TEST_CHECK(sx_write_file("p1", "common/scripted_effects/v3x_test.txt", "b", 1) == 0 && file_eq(p, "b", 1),
                   "C overwrite replaces the file");
    char tmpf[MAX_PATH + 16]; snprintf(tmpf, sizeof tmpf, "%s.v3xtmp", p);
    V3X_TEST_CHECK(!exists(tmpf), "C no temp file left");
    V3X_TEST_CHECK(sx_write_file("p1", "localization/english/v3x_l_english.yml", "l_english:\n", 11) == 0, "C write .yml");
    snprintf(p, sizeof p, "%sp1\\localization\\english\\v3x_l_english.yml", sx_root);
    V3X_TEST_CHECK(file_eq(p, "\xEF\xBB\xBFl_english:\n", 14), "C .yml written with UTF-8 BOM");
    V3X_TEST_CHECK(sx_write_file("p1", "../escape.txt", "x", 1) == V3X_SCRIPT_EARG, "C escaping path refused");
    V3X_TEST_CHECK(sx_write_file("bad/pack", "a.txt", "x", 1) == V3X_SCRIPT_EARG, "C bad pack refused");
    V3X_TEST_CHECK(sx_end_pack("p1") == 2, "C end_pack returns 2 files (overwrite counted once)");
    snprintf(p, sizeof p, "%sp1\\v3x_script_ext.manifest", sx_root);
    const char *man = "common/scripted_effects/v3x_test.txt\nlocalization/english/v3x_l_english.yml\n";
    V3X_TEST_CHECK(file_eq(p, man, strlen(man)), "C manifest lists both files");
    V3X_TEST_CHECK(sx_write_file("p1", "x.txt", "x", 1) == V3X_SCRIPT_ESTATE, "C write after end_pack -> ESTATE");

    /* a foreign file in the pack, then a new session that writes only one file */
    char foreign[MAX_PATH]; snprintf(foreign, sizeof foreign, "%sp1\\common\\mine.txt", sx_root);
    FILE *f = fopen(foreign, "wb"); if (f) { fputs("keep", f); fclose(f); }
    /* a tampered manifest line must not delete outside the pack */
    char outside[MAX_PATH]; snprintf(outside, sizeof outside, "%s\\outside.txt", tmp);
    f = fopen(outside, "wb"); if (f) { fputs("keep", f); fclose(f); }
    f = fopen(p, "ab"); if (f) { fputs("../../outside.txt\n", f); fclose(f); }
    V3X_TEST_CHECK(sx_begin_pack("p1") == 0, "C second session begin_pack");
    snprintf(p, sizeof p, "%sp1\\common\\scripted_effects\\v3x_test.txt", sx_root);
    char p2[MAX_PATH]; snprintf(p2, sizeof p2, "%sp1\\localization\\english\\v3x_l_english.yml", sx_root);
    V3X_TEST_CHECK(!exists(p) && !exists(p2), "C files of the previous session deleted");
    V3X_TEST_CHECK(exists(foreign), "C foreign file kept");
    V3X_TEST_CHECK(exists(outside), "C '..' line in a tampered manifest ignored");
    V3X_TEST_CHECK(sx_write_file("p1", "events/v3x_events.txt", "namespace = v3x\n", 16) == 0 && sx_end_pack("p1") == 1,
                   "C second session writes 1 file");
    V3X_TEST_CHECK(sx_begin_pack("p2") == 0 && sx_end_pack("p2") == 0, "C empty pack ok");
    V3X_TEST_CHECK(sx_end_pack("nope") == V3X_SCRIPT_ESTATE, "C end_pack of unknown pack -> ESTATE");
    DeleteFileA(outside);
}

static V3X_API g_a;               /* script_ext keeps the pointer it was loaded with: must outlive part D */
static DWORD g_main_tid;
static uint32_t main_tid_fn(void) { return g_main_tid; }

static void part_d(const char *tmp)
{
    v3x_testkit_api(&g_a);
    g_main_tid = GetCurrentThreadId();
    g_a.main_thread_id = main_tid_fn;
    snprintf(v3x_tk.dir, sizeof v3x_tk.dir, "%s\\v3x\\", tmp);
    CreateDirectoryA(v3x_tk.dir, NULL);
    sx_root[0] = 0;
    int c0 = v3x_tk.commands;
    V3X_TEST_CHECK(v3x_plugin_load(&g_a) == 0, "D v3x_plugin_load returns 0");
    char want[MAX_PATH]; snprintf(want, sizeof want, "%scontent_packs\\", v3x_tk.dir);
    V3X_TEST_CHECK(!strcmp(sx_root, want) && exists(want), "D root falls back to <v3x_dir>content_packs\\ (test exe is not in plugins\\): %s", sx_root);
    V3X_TEST_CHECK(v3x_tk.commands == c0 + 3 && v3x_tk.hooks == 0 && v3x_tk.patches == 0,
                   "D three console commands (scriptext, sxrun, sxtrigger), no hooks, no patches without a game image");
    const V3X_ScriptExt *t = v3x_script_ext();
    V3X_TEST_CHECK(t && t->struct_size == sizeof(V3X_ScriptExt) && t->version == V3X_SCRIPT_VERSION && t->begin_pack &&
                   t->write_file && t->end_pack && !strcmp(t->packs_dir(), want), "D export table complete");
    V3X_TEST_CHECK(V3X_SCRIPT_VERSION == 2 && V3X_SCRIPT_HAS(t, features) && V3X_SCRIPT_HAS(t, run_effect_text) &&
                   V3X_SCRIPT_HAS(t, eval_trigger_text) && V3X_SCRIPT_HAS(t, register_effect) && V3X_SCRIPT_HAS(t, register_trigger),
                   "D version 2 fields present (appended after packs_dir)");
    V3X_TEST_CHECK(offsetof(V3X_ScriptExt, features) == offsetof(V3X_ScriptExt, packs_dir) + sizeof(void *),
                   "D version-1 fields did not move (features follows packs_dir)");
    V3X_TEST_CHECK(t->features() == 0, "D features() == 0 without a game image (everything fails closed)");
    char err[200];
    int res = 7;
    V3X_TEST_CHECK(t->run_effect_text("x = 1", err, sizeof err) == V3X_SCRIPT_EOFF && err[0], "D run_effect_text without engine -> EOFF: %s", err);
    V3X_TEST_CHECK(t->eval_trigger_text("x = 1", &res, err, sizeof err) == V3X_SCRIPT_EOFF && res == 0, "D eval_trigger_text without engine -> EOFF");
    cmd_scriptext("", NULL);
}

static void part_e(const char *exe)
{
    V3X_API a;
    if (v3x_testkit_map_exe(exe, &a)) { V3X_TEST_CHECK(0, "E map %s", exe); return; }
    V3xRtti r;
    if (v3x_rtti_build_ex(&r, (uintptr_t)v3x_tk.base, v3x_tk.size, v3x_tk.pref_base)) { V3X_TEST_CHECK(0, "E RTTI build"); return; }
    static const struct { const char *cls; unsigned long long rva; } vt[] = {
        {"CJominiEffect", 0x4a80bd8}, {"CJominiTrigger", 0x4a75328}, {"CDebugLogEffect", 0x4a7e6f8},
        {"CDebugLogTrigger", 0x4a74788}, {".?AV?$CEffectEntry@VCDebugLogEffect@@@@", 0x4a77610},
        {".?AV?$CTriggerEntry@VCDebugLogTrigger@@@@", 0x4a775d8},
    };
    for (size_t i = 0; i < sizeof vt / sizeof *vt; i++) {
        void *v = v3x_rtti_find_vtable(&r, vt[i].cls);
        V3X_TEST_CHECK(v && v3x_testkit_rva(v) == vt[i].rva, "E vtable %s at %llx (doc %llx)", vt[i].cls, v3x_testkit_rva(v), vt[i].rva);
    }
    int ne = 0, nt = 0;
    ne = v3x_rtti_enum(&r, ".?AV?$CEffectEntry@", NULL, NULL);
    nt = v3x_rtti_enum(&r, ".?AV?$CTriggerEntry@", NULL, NULL);
    V3X_TEST_CHECK(ne == 709 && nt == 1054, "E RTTI counts: %d CEffectEntry<>, %d CTriggerEntry<> (doc 709 / 1054)", ne, nt);
    v3x_rtti_free(&r);
    /* strings named in the doc, and the absence of vanilla names in the v3x_ namespace the guide reserves */
    const char *s[] = {"debug_log", "Effect Runner", "Trigger Runner", "RunEffect", "Please specify event name", SX_EFF_PATH, SX_TRG_PATH, NULL};
    for (int i = 0; s[i]; i++) {
        void *p = a.find_string(s[i]);
        void *x = p ? a.find_xref(p, 0) : NULL;
        V3X_TEST_CHECK(p && x, "E string \"%s\" present and referenced (fn %llx)", s[i], v3x_testkit_rva(x ? a.function_start(x) : NULL));
    }
    int v3x_names = 0;
    for (int k = 0; k < v3x_tk.ndata; k++)
        for (size_t i = 1; i + 4 < v3x_tk.data[k].n; i++)
            if (v3x_tk.data[k].p[i - 1] == 0 && !memcmp(v3x_tk.data[k].p + i, "v3x_", 4)) v3x_names++;
    V3X_TEST_CHECK(v3x_names == 0, "E no vanilla string starts with \"v3x_\" (%d)", v3x_names);
}

/* ------------------------------------------------------------------ F: layer A anchors on the real exe */

static int g_hide_string_active;
static const char *g_hide_string;
static void *(*g_real_find_string)(const char *);
static void *hide_find_string(const char *s)
{
    if (g_hide_string_active && !strcmp(s, g_hide_string)) return NULL;
    return g_real_find_string(s);
}
static void *no_function_start(const void *p) { (void)p; return NULL; }

static void part_f(const char *exe)
{
    V3X_API a;
    if (v3x_testkit_map_exe(exe, &a)) { V3X_TEST_CHECK(0, "F map %s", exe); return; }
    a.main_thread_id = main_tid_fn;
    SxEngine e;
    DWORD t0 = GetTickCount();
    int rc = sx_engine_resolve(&a, "script_ext", &e);
    DWORD ms = GetTickCount() - t0;
    V3X_TEST_CHECK(rc == 0 && e.ok, "F sx_engine_resolve on the real exe (%lu ms): %s", (unsigned long)ms, e.why[0] ? e.why : "ok");
    if (rc) return;
    unsigned long long got[] = {v3x_testkit_rva((void *)e.eff_parse), v3x_testkit_rva((void *)e.eff_exec), v3x_testkit_rva((void *)e.trg_parse),
                                v3x_testkit_rva((void *)e.trg_eval), v3x_testkit_rva((void *)e.top_dtor), v3x_testkit_rva((void *)e.str_dtor),
                                v3x_testkit_rva((void *)e.rng_acquire), v3x_testkit_rva((void *)e.rng_global), v3x_testkit_rva(e.rng_owner),
                                v3x_testkit_rva(e.player_root)};
    unsigned long long want[] = {0x2df4680, 0x2df4d00, 0x2dfa400, 0x2dfb0d0, 0x6a7690, 0x67b890, 0x37879e0, 0x540ae80, 0x540ae60, 0x5c3ef90};
    const char *nm[] = {"effect parse", "effect execute", "trigger parse", "trigger evaluate", "top scope dtor", "string dtor",
                        "random scope acquire", "random scope global", "random scope owner", "player root"};
    for (int i = 0; i < 10; i++)
        V3X_TEST_CHECK(got[i] == want[i], "F %s at exe+0x%llx (doc docs/re/script.md: 0x%llx)", nm[i], got[i], want[i]);
    V3X_TEST_CHECK(e.player_off == 0xf8, "F player country id at [[root]+0x608]+0x%x (doc 0xf8)", e.player_off);

    /* uniqueness of the pattern-only anchors and of the call-site patterns */
    V3xSection text;
    v3x_section(a.image_base(), ".text", &text);
    V3X_TEST_CHECK(v3x_scan_count(&text, SX_P_TRG_EVAL, NULL) == 1, "F trigger-evaluate prologue matches exactly once in .text");
    V3X_TEST_CHECK(v3x_scan_count(&text, SX_P_EFF_EXEC, NULL) == 1 && v3x_scan_count(&text, SX_P_TOPDTOR, NULL) == 1 &&
                   v3x_scan_count(&text, SX_P_STRDTOR, NULL) == 1 && v3x_scan_count(&text, SX_P_RNG_ACQ, NULL) == 1 &&
                   v3x_scan_count(&text, SX_P_PLAYER, NULL) == 1 && v3x_scan_count(&text, SX_P_RUNEFFECT, NULL) == 1,
                   "F execute / top-scope dtor / string dtor / random acquire / player read / RunEffect prologues unique in .text");
    /* every string anchor has one function */
    const char *strs[] = {"RunEffect", "Effect Runner", "Trigger Runner", "Please specify event name", V3X_GAME_RNG_ASSERT};
    for (int i = 0; i < 5; i++) {
        void *s = a.find_string(strs[i]);
        int nx = s ? v3x_xrefs(&text, s, NULL, 0) : 0;
        V3X_TEST_CHECK(s && nx >= 1, "F string \"%.40s\" has %d code xref(s)", strs[i], nx);
    }

    /* fail closed: lose any anchor -> feature off with a reason */
    SxEngine bad;
    V3X_API b = a;
    g_real_find_string = a.find_string;
    b.find_string = hide_find_string;
    const char *hide[] = {"RunEffect", "Effect Runner", "Trigger Runner", "Please specify event name"};
    for (int i = 0; i < 4; i++) {
        g_hide_string = hide[i];
        g_hide_string_active = 1;
        int r = sx_engine_resolve(&b, "script_ext", &bad);
        g_hide_string_active = 0;
        V3X_TEST_CHECK(r != 0 && !bad.ok && bad.why[0] && !bad.eff_exec && !bad.rng_global, "F without string \"%s\": feature off (%.70s)", hide[i], bad.why);
    }
    b = a;
    b.function_start = no_function_start;
    V3X_TEST_CHECK(sx_engine_resolve(&b, "script_ext", &bad) != 0 && !bad.ok, "F no .pdata function info: feature off (%.70s)", bad.why);
    char err[120];
    int res = 5;
    V3X_TEST_CHECK(sx_run_effect(&a, &bad, "x = 1", err, sizeof err) == V3X_SCRIPT_EOFF && err[0] &&
                   sx_eval_trigger(&a, &bad, "x", &res, err, sizeof err) == V3X_SCRIPT_EOFF && res == 0,
                   "F run / eval refuse (EOFF) on a failed resolution without touching the engine");
}

/* ------------------------------------------------------------------ G: layer A flow against fake engine functions */

static SxEngine g_fake;
static uint8_t g_owner_mem[0x40] __attribute__((aligned(16)));
static void *g_rng_global_var;
static uint8_t g_g1mem[0x700], g_g2mem[0x200];
static uintptr_t g_root_val;

static struct {
    int parse_n, exec_n, tdtor_n, sdtor_n, acq_n, del_n, tparse_n, teval_n, tdel_n;
    char text[128];
    uint32_t type;
    SxRef root;
    int holder_ok, owner_ok, global_ok, fn_ok, top_ok, align_ok;
    void *top;
    int reenter_rc, reenter;
    int fault;                     /* exec simulates a hardware fault through the vectored handler's cleanup */
    int fault_global_restored;
    uint32_t fault_code;
    void *sentinel;
} F;

typedef struct { const void *const *vt; } FakeObj;
static void fake_del(void *o, int heap) { (void)o; F.del_n += heap; }
static void fake_tdel(void *o, int heap) { (void)o; F.tdel_n += heap; }
static void fake_nop(void) {}
static const void *const fake_evt[1] = {(void *)fake_del};
static const void *const fake_tvt[3] = {(void *)fake_nop, (void *)fake_nop, (void *)fake_tdel};
static FakeObj g_fobj = {fake_evt}, g_ftrg = {fake_tvt};
static char g_long_err[] = "this is a long error message from the engine, over 15 chars";

static void *fake_eff_parse(void **out, const SxStr *text, uint32_t type, SxStr *err)
{
    F.parse_n++;
    snprintf(F.text, sizeof F.text, "%s", sx_cstr(text));
    F.type = type;
    if (strstr(F.text, "BAD")) {
        *out = NULL;
        if (strstr(F.text, "LONG")) { err->u.ptr = g_long_err; err->size = strlen(g_long_err); err->cap = err->size; }
        else { strcpy(err->u.buf, "bad effect"); err->size = 10; }
        return out;
    }
    *out = &g_fobj;
    return out;
}
static void *fake_eff_exec(void *holder, void *top, const SxRef *root, const SxFn *init)
{
    F.exec_n++;
    F.top = top;
    F.root = *root;
    F.holder_ok = ((void **)holder)[0] == &g_fobj;
    F.align_ok = ((uintptr_t)top & 15) == 0;
    SxRngRec *rec = (SxRngRec *)g_rng_global_var;
    uint32_t tid;
    memcpy(&tid, g_owner_mem + 0x10, 4);
    F.owner_ok = rec && rec->owner == (void *)g_owner_mem && tid == GetCurrentThreadId();
    F.fn_ok = init->self == (void *)init && init->vt[2] != NULL;
    if (F.fn_ok) ((void (*)(void *, void *))init->vt[2])(init->self, top);   /* the engine's init callback call */
    if (F.reenter) {
        char e2[100];
        F.reenter_rc = sx_run_effect(&g_a, &g_fake, "x = 1", e2, sizeof e2);
    }
    if (F.fault) {
        sx_fault_cleanup(F.fault_code);
        F.fault_global_restored = g_rng_global_var == F.sentinel;
    }
    return top;
}
static void fake_top_dtor(void *top) { F.tdtor_n++; F.top_ok = top == F.top; }
static void fake_str_dtor(SxStr *s) { (void)s; F.sdtor_n++; }
static void *fake_rng_acq(SxRngRec *rec, void *unused, const SxSrcLoc *loc)
{
    (void)unused; (void)loc;
    F.acq_n++;
    rec->owner = g_owner_mem;
    DWORD tid = GetCurrentThreadId();
    LONG old = InterlockedCompareExchange((LONG volatile *)(g_owner_mem + 0x10), (LONG)tid, 0);
    rec->acquired = old == 0;
    return rec;
}
static void *fake_trg_parse(SxTrgRes *out, const SxStr *text, uint32_t type)
{
    F.tparse_n++;
    snprintf(F.text, sizeof F.text, "%s", sx_cstr(text));
    F.type = type;
    if (strstr(F.text, "BAD")) {
        memset(out, 0, sizeof *out);
        strcpy(out->u.err.u.buf, "bad trigger");
        out->u.err.size = 11;
        out->u.err.cap = 15;
        out->is_error = 1;
        return out;
    }
    memset(out, 0, sizeof *out);
    out->u.trigger = &g_ftrg;
    return out;
}
static uint32_t fake_trg_eval(void *holder, const SxRef *root, const SxFn *init)
{
    F.teval_n++;
    F.root = *root;
    F.holder_ok = ((void **)holder)[0] == &g_ftrg;
    SxRngRec *rec = (SxRngRec *)g_rng_global_var;
    F.owner_ok = rec && rec->owner == (void *)g_owner_mem;
    F.fn_ok = init->self == (void *)init;
    return 0x5500u | (strstr(F.text, "true") ? 1u : 0u);   /* only the low byte means anything (movzx in the engine) */
}

static void fake_engine_reset(void)
{
    memset(&F, 0, sizeof F);
    memset(g_owner_mem, 0, sizeof g_owner_mem);
    F.sentinel = (void *)(uintptr_t)0x1234560;
    g_rng_global_var = F.sentinel;
    memset(g_g1mem, 0, sizeof g_g1mem);
    memset(g_g2mem, 0, sizeof g_g2mem);
    uintptr_t g2 = (uintptr_t)g_g2mem;
    memcpy(g_g1mem + 0x608, &g2, 8);
    uint32_t id = 0x1000007u;
    memcpy(g_g2mem + 0xf8, &id, 4);
    g_root_val = (uintptr_t)g_g1mem;
    sx_active = 0;
    sx_run_disabled = 0;
}
static void fake_engine_make(void)
{
    memset(&g_fake, 0, sizeof g_fake);
    g_fake.ok = 1;
    g_fake.eff_parse = fake_eff_parse; g_fake.eff_exec = fake_eff_exec; g_fake.trg_parse = fake_trg_parse; g_fake.trg_eval = fake_trg_eval;
    g_fake.top_dtor = fake_top_dtor; g_fake.str_dtor = fake_str_dtor; g_fake.rng_acquire = fake_rng_acq;
    g_fake.rng_global = &g_rng_global_var; g_fake.rng_owner = g_owner_mem;
    g_fake.player_root = (const uint8_t *)&g_root_val; g_fake.player_off = 0xf8;
    fake_engine_reset();
}
static uint32_t owner_tid(void) { uint32_t t; memcpy(&t, g_owner_mem + 0x10, 4); return t; }

typedef struct { int rc; char err[160]; int res; int trigger; } ThreadCall;
static DWORD WINAPI thread_call(LPVOID p)
{
    ThreadCall *c = (ThreadCall *)p;
    if (c->trigger) c->rc = sx_eval_trigger(&g_a, &g_fake, "true", &c->res, c->err, sizeof c->err);
    else c->rc = sx_run_effect(&g_a, &g_fake, "add_treasury = 1", c->err, sizeof c->err);
    return 0;
}

static void part_g(void)
{
    char err[200], pr[64];
    fake_engine_make();
    int rc = sx_run_effect(&g_a, &g_fake, "add_treasury = 1000", err, sizeof err);
    V3X_TEST_CHECK(rc == 0 && !err[0], "G run_effect ok (%d '%s')", rc, err);
    V3X_TEST_CHECK(F.parse_n == 1 && F.exec_n == 1 && F.tdtor_n == 1 && F.del_n == 1 && F.sdtor_n == 0 && F.acq_n == 1,
                   "G calls: parse 1, execute 1, top scope dtor 1, effect deleted once (slot 0, edx=1), acquire 1");
    V3X_TEST_CHECK(!strcmp(F.text, "add_treasury = 1000") && F.type == 5, "G parse got the text and root scope type 5 (country)");
    V3X_TEST_CHECK(F.root.type == 5 && F.root.id == 0x1000007u, "G root scope = player country {5, 0x%llx}", (unsigned long long)F.root.id);
    V3X_TEST_CHECK(F.holder_ok && F.fn_ok && F.top_ok && F.align_ok, "G holder[0] = effect, std::function stand-in self-pointing at +0x38, top scope aligned and destroyed with the same pointer");
    V3X_TEST_CHECK(F.owner_ok, "G during execution the random scope global points at our record and the owner thread id is ours");
    V3X_TEST_CHECK(g_rng_global_var == F.sentinel && owner_tid() == 0 && !sx_active, "G afterwards the global is restored, the owner tid released, runner idle");
    V3X_TEST_CHECK(offsetof(SxFn, self) == 0x38 && sizeof(SxFn) == 0x40 && sizeof(SxStr) == 0x20 && sizeof(SxRef) == 16 && sizeof(SxTrgRes) == 0x28,
                   "G ABI structs: std::function 0x40 with _Ptr at +0x38, CString 0x20, scope ref 16, trigger result 0x28");

    /* text lengths around the std::string SSO boundary */
    const char *t15 = "123456789012345", *t16 = "1234567890123456";
    rc = sx_run_effect(&g_a, &g_fake, t15, err, sizeof err);
    int ok15 = rc == 0 && !strcmp(F.text, t15);
    rc = sx_run_effect(&g_a, &g_fake, t16, err, sizeof err);
    V3X_TEST_CHECK(ok15 && rc == 0 && !strcmp(F.text, t16), "G 15 and 16 byte texts (SSO / heap CString) arrive intact");
    char big[3000];
    memset(big, 'a', sizeof big - 1); big[sizeof big - 1] = 0;
    rc = sx_run_effect(&g_a, &g_fake, big, err, sizeof err);
    V3X_TEST_CHECK(rc == 0 && F.parse_n == 4 && strlen(F.text) == 127, "G 3000 byte text runs (the fake keeps 127 chars)");

    /* engine rejects the text */
    fake_engine_reset();
    rc = sx_run_effect(&g_a, &g_fake, "BAD", err, sizeof err);
    V3X_TEST_CHECK(rc == V3X_SCRIPT_ESCRIPT && !strcmp(err, "bad effect") && F.sdtor_n == 1 && F.exec_n == 0 && F.del_n == 0 && F.tdtor_n == 0,
                   "G parse error -> ESCRIPT with the engine message '%s', string destroyed, nothing executed", err);
    V3X_TEST_CHECK(g_rng_global_var == F.sentinel && owner_tid() == 0 && !sx_active, "G random scope given back after a parse error");
    rc = sx_run_effect(&g_a, &g_fake, "BAD LONG", err, sizeof err);
    V3X_TEST_CHECK(rc == V3X_SCRIPT_ESCRIPT && !strcmp(err, g_long_err) && F.sdtor_n == 2, "G heap-allocated engine error message copied (and destroyed)");
    char small[12];
    rc = sx_run_effect(&g_a, &g_fake, "BAD LONG", small, sizeof small);
    V3X_TEST_CHECK(rc == V3X_SCRIPT_ESCRIPT && strlen(small) == 11, "G error message cut to the caller's buffer, NUL terminated");
    rc = sx_run_effect(&g_a, &g_fake, "BAD", NULL, 0);
    V3X_TEST_CHECK(rc == V3X_SCRIPT_ESCRIPT, "G err == NULL accepted");

    /* argument and state checks happen before the engine is touched */
    fake_engine_reset();
    V3X_TEST_CHECK(sx_run_effect(&g_a, &g_fake, NULL, err, sizeof err) == V3X_SCRIPT_EARG && sx_run_effect(&g_a, &g_fake, "", err, sizeof err) == V3X_SCRIPT_EARG,
                   "G NULL / empty text -> EARG");
    char *huge = (char *)malloc(SX_TEXT_MAX + 2);
    memset(huge, 'x', SX_TEXT_MAX + 1); huge[SX_TEXT_MAX + 1] = 0;
    V3X_TEST_CHECK(sx_run_effect(&g_a, &g_fake, huge, err, sizeof err) == V3X_SCRIPT_EARG, "G text > %d bytes -> EARG", SX_TEXT_MAX);
    free(huge);
    V3X_TEST_CHECK(F.acq_n == 0 && F.parse_n == 0, "G ... without touching the random scope or the parser");

    /* main thread only */
    ThreadCall tc = {0};
    HANDLE th = CreateThread(NULL, 0, thread_call, &tc, 0, NULL);
    WaitForSingleObject(th, 5000); CloseHandle(th);
    V3X_TEST_CHECK(tc.rc == V3X_SCRIPT_ETHREAD && tc.err[0] && F.acq_n == 0 && F.parse_n == 0, "G run_effect from another thread -> ETHREAD ('%.50s'), engine untouched", tc.err);
    tc.trigger = 1;
    th = CreateThread(NULL, 0, thread_call, &tc, 0, NULL);
    WaitForSingleObject(th, 5000); CloseHandle(th);
    V3X_TEST_CHECK(tc.rc == V3X_SCRIPT_ETHREAD && F.tparse_n == 0, "G eval_trigger from another thread -> ETHREAD");
    {
        V3X_API old = g_a;
        g_a.main_thread_id = NULL;
        V3X_TEST_CHECK(sx_run_effect(&g_a, &g_fake, "x = 1", err, sizeof err) == V3X_SCRIPT_ETHREAD, "G loader without main_thread_id (unknown) -> refused");
        g_a = old;
    }

    /* no game / no player */
    uintptr_t keep = g_root_val;
    g_root_val = 0;
    V3X_TEST_CHECK(sx_run_effect(&g_a, &g_fake, "x = 1", err, sizeof err) == V3X_SCRIPT_ENOGAME, "G no game state -> ENOGAME");
    g_root_val = keep;
    uint32_t minus1 = 0xFFFFFFFFu;
    memcpy(g_g2mem + 0xf8, &minus1, 4);
    V3X_TEST_CHECK(sx_run_effect(&g_a, &g_fake, "x = 1", err, sizeof err) == V3X_SCRIPT_ENOGAME && F.acq_n == 0, "G player id -1 -> ENOGAME, scope untouched");
    fake_engine_reset();

    /* random scope owned by someone else / by us */
    uint32_t other = GetCurrentThreadId() + 4;
    memcpy(g_owner_mem + 0x10, &other, 4);
    rc = sx_run_effect(&g_a, &g_fake, "x = 1", err, sizeof err);
    V3X_TEST_CHECK(rc == V3X_SCRIPT_EBUSY && F.acq_n == 0 && owner_tid() == other && g_rng_global_var == F.sentinel && !sx_active,
                   "G random scope held by another thread -> EBUSY without calling the engine ('%.60s')", err);
    uint32_t me = GetCurrentThreadId();
    memcpy(g_owner_mem + 0x10, &me, 4);
    rc = sx_run_effect(&g_a, &g_fake, "x = 1", err, sizeof err);
    V3X_TEST_CHECK(rc == 0 && F.acq_n == 1 && owner_tid() == me && g_rng_global_var == F.sentinel,
                   "G already inside a tick task on this thread: runs, does NOT release the scope it did not take");
    fake_engine_reset();

    /* re-entrancy */
    F.reenter = 1;
    rc = sx_run_effect(&g_a, &g_fake, "x = 1", err, sizeof err);
    V3X_TEST_CHECK(rc == 0 && F.reenter_rc == V3X_SCRIPT_EBUSY && F.parse_n == 1, "G a run started from inside a run -> EBUSY");
    fake_engine_reset();

    /* hardware fault cleanup (what the vectored handler does) */
    F.fault = 1; F.fault_code = 0xE06D7363u;
    rc = sx_run_effect(&g_a, &g_fake, "x = 1", err, sizeof err);
    V3X_TEST_CHECK(rc == 0 && !F.fault_global_restored && !sx_run_disabled, "G a C++ exception code is ignored by the fault cleanup");
    fake_engine_reset();
    F.fault = 1; F.fault_code = EXCEPTION_ACCESS_VIOLATION;
    rc = sx_run_effect(&g_a, &g_fake, "x = 1", err, sizeof err);
    V3X_TEST_CHECK(F.fault_global_restored && owner_tid() == 0 && sx_run_disabled && !sx_active,
                   "G access violation during the run: random scope restored at once, tid released, running disabled");
    V3X_TEST_CHECK(sx_run_effect(&g_a, &g_fake, "x = 1", err, sizeof err) == V3X_SCRIPT_EOFF && sx_eval_trigger(&g_a, &g_fake, "true", &(int){0}, err, sizeof err) == V3X_SCRIPT_EOFF,
                   "G after a fault both run and eval are refused until restart ('%.60s')", err);
    fake_engine_reset();
    EXCEPTION_RECORD er = {0};
    EXCEPTION_POINTERS ep = {&er, NULL};
    er.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    V3X_TEST_CHECK(sx_veh(&ep) == EXCEPTION_CONTINUE_SEARCH && !sx_run_disabled, "G vectored handler outside a run: does nothing, never handles the exception");
    F.fault = 0;

    /* triggers */
    fake_engine_reset();
    int res = -1;
    rc = sx_eval_trigger(&g_a, &g_fake, "true", &res, err, sizeof err);
    V3X_TEST_CHECK(rc == 0 && res == 1 && F.tparse_n == 1 && F.teval_n == 1 && F.tdel_n == 1 && F.type == 5 && F.root.type == 5 && F.root.id == 0x1000007u &&
                   F.holder_ok && F.owner_ok && F.fn_ok, "G eval_trigger true: parse type 5, evaluate with root {5, player}, trigger deleted (slot 2)");
    V3X_TEST_CHECK(g_rng_global_var == F.sentinel && owner_tid() == 0 && !sx_active, "G trigger: random scope given back");
    rc = sx_eval_trigger(&g_a, &g_fake, "false_thing", &res, err, sizeof err);
    V3X_TEST_CHECK(rc == 0 && res == 0, "G eval_trigger false (garbage in the upper bits of eax ignored)");
    rc = sx_eval_trigger(&g_a, &g_fake, "BAD", &res, err, sizeof err);
    V3X_TEST_CHECK(rc == V3X_SCRIPT_ESCRIPT && !strcmp(err, "bad trigger") && res == 0 && F.sdtor_n == 1 && F.teval_n == 2 && F.tdel_n == 2,
                   "G trigger parse error -> ESCRIPT '%s', string destroyed, not evaluated", err);
    V3X_TEST_CHECK(sx_eval_trigger(&g_a, &g_fake, "true", NULL, err, sizeof err) == V3X_SCRIPT_EARG, "G result == NULL -> EARG");

    /* console commands go through run_on_main (the testkit runs it at once) */
    fake_engine_reset();
    g_eng = g_fake;
    g_eng_state = 2;
    g_loaded = 1;
    int p0 = v3x_tk.checks;
    (void)p0; (void)pr;
    cmd_sxrun("   add_treasury = 5", NULL);
    V3X_TEST_CHECK(F.parse_n == 1 && !strcmp(F.text, "add_treasury = 5") && F.exec_n == 1, "G sxrun <text> runs the effect (leading blanks stripped)");
    cmd_sxrun("", NULL);
    cmd_sxtrigger("", NULL);
    V3X_TEST_CHECK(F.parse_n == 1 && F.tparse_n == 0, "G sxrun / sxtrigger without text print usage and run nothing");
    cmd_sxtrigger("true", NULL);
    V3X_TEST_CHECK(F.tparse_n == 1 && F.teval_n == 1, "G sxtrigger <text> evaluates");
    const V3X_ScriptExt *t = v3x_script_ext();
    V3X_TEST_CHECK(t->run_effect_text("add_treasury = 6", err, sizeof err) == 0 && F.exec_n == 2 &&
                   t->eval_trigger_text("true", &res, err, sizeof err) == 0 && res == 1 && (t->features() & V3X_SCRIPT_F_RUN),
                   "G table entry points work and features() reports F_RUN");
    cmd_scriptext("", NULL);
    memset(&g_eng, 0, sizeof g_eng);
    g_eng_state = 0;
    fake_engine_reset();
}

/* ------------------------------------------------------------------ H: layer C against fake engine objects */

static int g_cb_n;
static V3X_ScriptScope g_cb_scope;
static char g_cb_value[64];
static void *g_cb_user;
static void cb_effect(const V3X_ScriptScope *s, const char *v, void *u) { g_cb_n++; g_cb_scope = *s; snprintf(g_cb_value, sizeof g_cb_value, "%s", v); g_cb_user = u; }
static int cb_trigger(const V3X_ScriptScope *s, const char *v, void *u) { g_cb_n++; g_cb_scope = *s; snprintf(g_cb_value, sizeof g_cb_value, "%s", v); g_cb_user = u; return v[0] == 'y'; }

static void *g_fake_registry_val;
static int g_fake_token_n, g_fake_reg_n, g_fake_alloc_n;
static SxEntry *g_fake_entries[8];
static uint32_t g_fake_regtok[8];
static void fake_code0(void) {}
static void fake_code1(void) {}
static void *fake_registry_getter(void) { static int r; return &r; }
static uint32_t fake_token(void *registry, const SxNameArg *n)
{
    (void)registry;
    g_fake_token_n++;
    uint32_t h = 100;
    for (uint32_t i = 0; i < n->len; i++) h = h * 31 + (uint8_t)n->p[i];
    return h;
}
static void *fake_alloc(size_t n) { g_fake_alloc_n++; return calloc(1, n); }
static void fake_eff_register(uint32_t tok, void *entry) { g_fake_regtok[g_fake_reg_n] = tok; g_fake_entries[g_fake_reg_n++] = (SxEntry *)entry; }
static void fake_trg_register(uint32_t tok, void *entry) { g_fake_regtok[g_fake_reg_n] = tok; g_fake_entries[g_fake_reg_n++] = (SxEntry *)entry; }

/* fake debug_log vtables: [-1] = "RTTI" word, 26 code slots, then a data word */
static int g_orig_exec_n, g_orig_eval_n, g_dummy_data;
static void fake_orig_exec(void *self, void **ctx) { (void)self; (void)ctx; g_orig_exec_n++; }
static uint8_t fake_orig_eval(void *self, void **ctx) { (void)self; (void)ctx; g_orig_eval_n++; return 1; }
static void *g_fev[1 + 27], *g_ftv[1 + 27], *g_feev[1 + 3], *g_fterv[1 + 4];
static void *fake_orig_ecreate(void *entry)
{
    uint8_t *o = (uint8_t *)calloc(1, 0x60);
    *(void **)o = &g_fev[1];
    memcpy(o + 8, (uint8_t *)entry + 0x10, 4);
    SxStr *s = (SxStr *)(o + SX_EFF_MSG);
    s->cap = 15;
    return o;
}
static void *fake_orig_tcreate(void *entry)
{
    uint8_t *o = (uint8_t *)calloc(1, 0x58);
    *(void **)o = &g_ftv[1];
    memcpy(o + 8, (uint8_t *)entry + 0x10, 4);
    SxStr *s = (SxStr *)(o + SX_TRG_MSG);
    s->cap = 15;
    return o;
}
static void fake_dtor_slot(void) {}
static uint8_t fake_ret0[4] = {0x33, 0xC0, 0xC3, 0};

static void reg_state_reset(void)
{
    memset(sx_eff, 0, sizeof sx_eff);
    memset(sx_trg, 0, sizeof sx_trg);
    sx_neff = sx_ntrg = 0;
    sx_reg_closed = 0;
    sx_reg_off = 0;
    g_cb_n = 0;
}

static DWORD WINAPI thread_reg(LPVOID p)
{
    int *rc = (int *)p;
    *rc = v3x_script_ext()->register_effect("v3x_from_thread", cb_effect, NULL);
    return 0;
}

static void part_h_fake(void)
{
    /* ---- names */
    V3X_TEST_CHECK(sx_valid_kw("v3x_hello") && sx_valid_kw("v3x_a") && sx_valid_kw("v3x_add_gold_2") && sx_valid_kw("v3x_0"), "H valid keyword names");
    char l62[80], l63[80], l64[80];
    memset(l62, 'a', 79); memcpy(l62, "v3x_", 4); l62[62] = 0;
    memset(l63, 'a', 79); memcpy(l63, "v3x_", 4); l63[63] = 0;
    memset(l64, 'a', 79); memcpy(l64, "v3x_", 4); l64[64] = 0;
    V3X_TEST_CHECK(sx_valid_kw(l62) && sx_valid_kw(l63) && !sx_valid_kw(l64), "H name length limit: 63 chars ok, 64 refused");
    const char *bad[] = {"", "v3x_", "v3x", "hello", "debug_log", "V3X_hello", "v3x_Hello", "v3x_he llo", "v3x_he-llo", "v3x_h\xc3\xa9", "x_v3x_a", " v3x_a", "v3x_a ", "add_gold", NULL};
    int allbad = 1;
    for (int i = 0; bad[i]; i++) if (sx_valid_kw(bad[i])) { printf("  accepted: '%s'\n", bad[i]); allbad = 0; }
    V3X_TEST_CHECK(allbad && !sx_valid_kw(NULL), "H names without the v3x_ prefix, empty, uppercase, blanks, '-' or non-ASCII refused (vanilla names cannot be shadowed)");

    /* ---- queueing through the exported table. g_loaded = 0 plays "a plugin that loads before script_ext's own
     * v3x_plugin_load ran": the keyword is only queued (no anchors needed yet). */
    int was_loaded = g_loaded;
    g_loaded = 0;
    reg_state_reset();
    const V3X_ScriptExt *t = v3x_script_ext();
    int rc1 = t->register_effect("v3x_hello", cb_effect, (void *)1), rc2 = t->register_trigger("v3x_is_rich", cb_trigger, (void *)2);
    V3X_TEST_CHECK(rc1 == 0 && rc2 == 0 && sx_neff == 1 && sx_ntrg == 1, "H register_effect / register_trigger queue keywords (without a game image the hook is not armed)");
    V3X_TEST_CHECK(t->register_effect("v3x_hello", cb_effect, NULL) == V3X_SCRIPT_EEXIST && t->register_trigger("v3x_is_rich", cb_trigger, NULL) == V3X_SCRIPT_EEXIST,
                   "H duplicate name of the same kind -> EEXIST");
    V3X_TEST_CHECK(t->register_trigger("v3x_hello", cb_trigger, NULL) == 0 && sx_ntrg == 2, "H the same name as effect and trigger is allowed (separate registries)");
    V3X_TEST_CHECK(t->register_effect("hello", cb_effect, NULL) == V3X_SCRIPT_EARG && t->register_effect("v3x_x", NULL, NULL) == V3X_SCRIPT_EARG &&
                   t->register_trigger(NULL, cb_trigger, NULL) == V3X_SCRIPT_EARG && t->register_effect("debug_log", cb_effect, NULL) == V3X_SCRIPT_EARG,
                   "H bad name / NULL callback -> EARG");
    reg_state_reset();
    int full = 0;
    for (int i = 0; i < SX_KW_MAX + 3; i++) {
        char nm[32];
        snprintf(nm, sizeof nm, "v3x_k%d", i);
        int r = t->register_effect(nm, cb_effect, NULL);
        if (i < SX_KW_MAX && r != 0) full = -1;
        if (i >= SX_KW_MAX && r != V3X_SCRIPT_EFULL) full = -2;
    }
    V3X_TEST_CHECK(full == 0 && sx_neff == SX_KW_MAX, "H %d keywords per kind, the next ones -> EFULL", SX_KW_MAX);
    reg_state_reset();
    sx_reg_closed = 1;
    V3X_TEST_CHECK(t->register_effect("v3x_late", cb_effect, NULL) == V3X_SCRIPT_ELATE && t->register_trigger("v3x_late", cb_trigger, NULL) == V3X_SCRIPT_ELATE && sx_neff == 0,
                   "H after the engine's static init -> ELATE (only from v3x_plugin_load)");
    reg_state_reset();
    sx_reg_off = 1;
    V3X_TEST_CHECK(t->register_effect("v3x_off", cb_effect, NULL) == V3X_SCRIPT_EOFF, "H registration switched off by a failed anchor -> EOFF");
    reg_state_reset();
    int trc = 0;
    HANDLE th = CreateThread(NULL, 0, thread_reg, &trc, 0, NULL);
    WaitForSingleObject(th, 5000); CloseHandle(th);
    V3X_TEST_CHECK(trc == V3X_SCRIPT_ETHREAD && sx_neff == 0, "H register_effect from another thread (after load) -> ETHREAD");
    /* after script_ext's own load, a registration needs the anchors at once: without a game image it fails closed */
    g_loaded = 1;
    reg_state_reset();
    g_reg_tried = 0; g_hooked = 0;
    int off_rc = t->register_effect("v3x_noimage", cb_effect, NULL);
    V3X_TEST_CHECK(off_rc == V3X_SCRIPT_EOFF && sx_reg_off && sx_neff == 0 && sx_reg_off_why[0] && !g_hooked && v3x_tk.hooks == 0,
                   "H registration after load with unresolvable anchors -> EOFF, queue dropped, no hook ('%.60s')", sx_reg_off_why);
    V3X_TEST_CHECK(t->register_trigger("v3x_again", cb_trigger, NULL) == V3X_SCRIPT_EOFF, "H ... and stays off");
    V3X_TEST_CHECK((t->features() & (V3X_SCRIPT_F_REGISTER | V3X_SCRIPT_F_REGISTERED)) == 0, "H features(): registration not armed");
    g_reg_tried = 0;
    g_loaded = 0;

    /* ---- the registration flow against fake engine objects */
    for (int i = 0; i < 26; i++) { g_fev[1 + i] = (void *)(i & 1 ? fake_code0 : fake_code1); g_ftv[1 + i] = (void *)(i & 1 ? fake_code1 : fake_code0); }
    g_fev[0] = (void *)(uintptr_t)0xC0111; g_ftv[0] = (void *)(uintptr_t)0xC0222;
    g_fev[1 + SX_EFF_EXEC_SLOT] = (void *)fake_orig_exec;
    g_ftv[1 + SX_TRG_EVAL_SLOT] = (void *)fake_orig_eval;
    g_fev[1 + 26] = &g_dummy_data; g_ftv[1 + 26] = &g_dummy_data;
    g_feev[0] = (void *)(uintptr_t)0xC0333; g_feev[1] = (void *)fake_dtor_slot; g_feev[2] = (void *)fake_orig_ecreate; g_feev[3] = &g_dummy_data;
    g_fterv[0] = (void *)(uintptr_t)0xC0444; g_fterv[1] = (void *)fake_dtor_slot; g_fterv[2] = (void *)fake_orig_tcreate; g_fterv[3] = (void *)fake_ret0; g_fterv[4] = &g_dummy_data;
    SxRegEng r;
    memset(&r, 0, sizeof r);
    r.ok = 1;
    r.registry_global = (void *const *)&g_fake_registry_val;
    r.registry_getter = fake_registry_getter;
    r.token_fn = fake_token;
    r.alloc = fake_alloc;
    r.eff_register = fake_eff_register;
    r.trg_register = fake_trg_register;
    r.eff_entry_vt = &g_feev[1]; r.trg_entry_vt = &g_fterv[1];
    r.eff_obj_vt = &g_fev[1]; r.trg_obj_vt = &g_ftv[1];
    V3X_TEST_CHECK(sx_vt_slots(r.eff_obj_vt, 26) && sx_vt_slots(r.trg_obj_vt, 26) && !sx_vt_slots(r.eff_obj_vt, 25) && !sx_vt_slots(r.eff_obj_vt, 27) &&
                   sx_vt_slots(r.eff_entry_vt, 2) && !sx_vt_slots(r.eff_entry_vt, 1),
                   "H slot counting: exact count required (n-1 and n+1 fail)");
    V3X_TEST_CHECK(sx_reg_build(&r) == 0, "H sx_reg_build");
    int same = 1;
    for (int i = 0; i < 26; i++) if (i != SX_EFF_EXEC_SLOT && sx_eff_obj_arr[1 + i] != g_fev[1 + i]) same = 0;
    for (int i = 0; i < 26; i++) if (i != SX_TRG_EVAL_SLOT && sx_trg_obj_arr[1 + i] != g_ftv[1 + i]) same = 0;
    V3X_TEST_CHECK(same && sx_eff_obj_arr[0] == g_fev[0] && sx_trg_obj_arr[0] == g_ftv[0] && sx_eff_obj_arr[1 + SX_EFF_EXEC_SLOT] == (void *)sx_eff_execute &&
                   sx_trg_obj_arr[1 + SX_TRG_EVAL_SLOT] == (void *)sx_trg_evaluate,
                   "H object vtables = copy of debug_log's (26 slots + RTTI word) with only slot 22 / slot 24 replaced");
    V3X_TEST_CHECK(sx_eff_ent_arr[0] == g_feev[0] && sx_eff_ent_arr[1] == g_feev[1] && sx_eff_ent_arr[2] == (void *)sx_eff_create &&
                   sx_trg_ent_arr[0] == g_fterv[0] && sx_trg_ent_arr[1] == g_fterv[1] && sx_trg_ent_arr[2] == (void *)sx_trg_create && sx_trg_ent_arr[3] == g_fterv[3],
                   "H entry vtables = copy (effect 2 slots, trigger 3 slots incl. slot 2 'xor eax,eax') with only Create replaced");

    reg_state_reset();
    t->register_effect("v3x_hello", cb_effect, (void *)11);
    t->register_effect("v3x_other", cb_effect, (void *)12);
    t->register_trigger("v3x_is_rich", cb_trigger, (void *)13);
    int n = sx_register_all(&r, NULL);
    V3X_TEST_CHECK(n == 3 && g_fake_token_n == 3 && g_fake_alloc_n == 3 && g_fake_reg_n == 3, "H sx_register_all: token + allocate + register once per keyword (%d)", n);
    V3X_TEST_CHECK(g_fake_entries[0]->vt == &sx_eff_ent_arr[1] && g_fake_entries[2]->vt == &sx_trg_ent_arr[1] && g_fake_entries[0]->token == g_fake_regtok[0] &&
                   g_fake_entries[0]->token == sx_eff[0].token && g_fake_entries[0]->desc == sx_eff[0].desc && !strncmp(g_fake_entries[0]->desc, "V3X native effect", 17) &&
                   g_fake_entries[0]->token != g_fake_entries[1]->token,
                   "H entry {vtable, description, token} filled like the vanilla registrar, distinct tokens");
    V3X_TEST_CHECK(sizeof(SxEntry) == 0x18 && offsetof(SxEntry, desc) == 8 && offsetof(SxEntry, token) == 0x10, "H entry layout 0x18 bytes {vtable, desc +8, token +0x10}");

    /* Create -> swapped object -> Execute / Evaluate wrappers */
    void *(*cr)(void *) = (void *(*)(void *))((void *const *)g_fake_entries[0]->vt)[1];
    uint8_t *obj = (uint8_t *)cr(g_fake_entries[0]);
    V3X_TEST_CHECK(obj && *(void **)obj == &sx_eff_obj_arr[1] && !memcmp(obj + 8, &g_fake_entries[0]->token, 4),
                   "H entry Create returns debug_log's object (token at +8) with our vtable");
    SxStr *msg = (SxStr *)(obj + SX_EFF_MSG);
    strcpy(msg->u.buf, "hello"); msg->size = 5;
    SxRef ref = {5, {0}, 0x1000007u};
    void *ctx[3] = {&ref, NULL, NULL};
    void (*exec)(void *, void **) = (void (*)(void *, void **))((void *const *)*(void **)obj)[SX_EFF_EXEC_SLOT];
    exec(obj, ctx);
    V3X_TEST_CHECK(g_cb_n == 1 && g_cb_scope.type == 5 && g_cb_scope.id == 0x1000007u && !strcmp(g_cb_value, "hello") && g_cb_user == (void *)11 && g_orig_exec_n == 0,
                   "H Execute calls the plugin callback with scope {5, id}, value 'hello', its user pointer (debug_log's own Execute not run)");
    char longv[] = "a value text longer than fifteen chars";
    msg->u.ptr = longv; msg->size = strlen(longv); msg->cap = msg->size;
    exec(obj, ctx);
    V3X_TEST_CHECK(g_cb_n == 2 && !strcmp(g_cb_value, longv), "H value stored as a heap CString is read too");
    uint8_t *obj2 = (uint8_t *)cr(g_fake_entries[1]);
    exec(obj2, ctx);
    V3X_TEST_CHECK(g_cb_user == (void *)12, "H a second keyword reaches its own callback (token lookup)");
    uint8_t *ghost = (uint8_t *)fake_orig_ecreate(g_fake_entries[0]);
    *(uint32_t *)(ghost + 8) = 0xDEAD;
    *(void **)ghost = &sx_eff_obj_arr[1];
    int before = g_cb_n;
    exec(ghost, ctx);
    V3X_TEST_CHECK(g_cb_n == before, "H an object whose token is not registered calls nothing");
    exec(obj, NULL);
    V3X_TEST_CHECK(g_cb_n == before + 1 && g_cb_scope.type == 0 && g_cb_scope.id == 0, "H a NULL context gives an empty scope instead of crashing");

    void *(*tcr)(void *) = (void *(*)(void *))((void *const *)g_fake_entries[2]->vt)[1];
    uint8_t *tobj = (uint8_t *)tcr(g_fake_entries[2]);
    V3X_TEST_CHECK(tobj && *(void **)tobj == &sx_trg_obj_arr[1], "H trigger entry Create returns the object with our vtable");
    SxStr *tmsg = (SxStr *)(tobj + SX_TRG_MSG);
    strcpy(tmsg->u.buf, "yes"); tmsg->size = 3;
    uint8_t (*ev)(void *, void **) = (uint8_t (*)(void *, void **))((void *const *)*(void **)tobj)[SX_TRG_EVAL_SLOT];
    uint8_t y = ev(tobj, ctx);
    strcpy(tmsg->u.buf, "no");
    uint8_t nn = ev(tobj, ctx);
    V3X_TEST_CHECK(y == 1 && nn == 0 && g_cb_user == (void *)13 && g_orig_eval_n == 0, "H Evaluate returns the plugin callback's truth value (debug_log's Evaluate not run)");
    V3X_TEST_CHECK(g_cb_scope.type == 5 && g_cb_scope.id == 0x1000007u, "H trigger callback got the scope");
    reg_state_reset();
    g_loaded = was_loaded;
}

/* ------------------------------------------------------------------ I: layer C anchors on the real exe */

static uintptr_t fix_ptr(uintptr_t p) { return p ? (uintptr_t)v3x_testkit_ptr(p) : 0; }
static int mapped_is_code(uintptr_t p) { return p >= (uintptr_t)v3x_tk.text && p < (uintptr_t)v3x_tk.text + v3x_tk.text_size; }

static void part_i(const char *exe)
{
    V3X_API a;
    if (v3x_testkit_map_exe(exe, &a)) { V3X_TEST_CHECK(0, "I map %s", exe); return; }
    sx_fix = fix_ptr;
    sx_is_code = mapped_is_code;
    SxRegEng r;
    DWORD t0 = GetTickCount();
    int rc = sx_reg_resolve(&a, &r);
    DWORD ms = GetTickCount() - t0;
    V3X_TEST_CHECK(rc == 0 && r.ok, "I sx_reg_resolve on the real exe (%lu ms): %s", (unsigned long)ms, r.why[0] ? r.why : "ok");
    if (rc) { sx_fix = NULL; sx_is_code = NULL; return; }
    unsigned long long got[] = {v3x_testkit_rva(r.registrar), v3x_testkit_rva(r.registry_global), v3x_testkit_rva((void *)r.registry_getter), v3x_testkit_rva((void *)r.token_fn),
                                v3x_testkit_rva((void *)r.alloc), v3x_testkit_rva((void *)r.eff_register), v3x_testkit_rva((void *)r.trg_register),
                                v3x_testkit_rva(r.eff_entry_vt), v3x_testkit_rva(r.trg_entry_vt), v3x_testkit_rva(r.eff_obj_vt), v3x_testkit_rva(r.trg_obj_vt)};
    unsigned long long want[] = {0x5d9110, 0x5c40a38, 0x3e1df20, 0x3e1d6f0, 0x43d95c0, 0x3553dc0, 0x3593dc0, 0x4a77610, 0x4a775d8, 0x4a7e6f8, 0x4a74788};
    const char *nm[] = {"hook target = debug_log effect registrar", "token registry global", "registry getter", "name -> token", "engine allocator", "effect register",
                        "trigger register", "CEffectEntry<CDebugLogEffect> vtable", "CTriggerEntry<CDebugLogTrigger> vtable", "CDebugLogEffect vtable", "CDebugLogTrigger vtable"};
    for (int i = 0; i < 11; i++) V3X_TEST_CHECK(got[i] == want[i], "I %s at exe+0x%llx (doc 0x%llx)", nm[i], got[i], want[i]);

    /* uniqueness: exactly two registrars reference "debug_log" (one effect, one trigger) */
    V3xSection text;
    v3x_section(a.image_base(), ".text", &text);
    void *s = a.find_string("debug_log");
    V3X_TEST_CHECK(s && v3x_xrefs(&text, s, NULL, 0) == 2, "I string \"debug_log\" has exactly 2 code xrefs (effect + trigger registrar)");
    V3X_TEST_CHECK(sx_starts((uint8_t *)r.registry_getter, SX_P_GETTER_FN) && sx_starts((uint8_t *)r.token_fn, SX_P_TOKEN_FN) && sx_starts((uint8_t *)r.alloc, SX_P_ALLOC_FN) &&
                   sx_starts((uint8_t *)r.eff_register, SX_P_REG_FN) && sx_starts((uint8_t *)r.trg_register, SX_P_REG_FN) && r.eff_register != r.trg_register,
                   "I getter / token / allocator / register functions pass their prologue checks (they are reached from the unique debug_log registrars, not by pattern)");
    /* vtable slot counts: exact */
    V3X_TEST_CHECK(sx_vt_slots(r.eff_entry_vt, 2) && !sx_vt_slots(r.eff_entry_vt, 1) && !sx_vt_slots(r.eff_entry_vt, 3), "I CEffectEntry vtable: exactly 2 slots");
    V3X_TEST_CHECK(sx_vt_slots(r.trg_entry_vt, 3) && !sx_vt_slots(r.trg_entry_vt, 2) && !sx_vt_slots(r.trg_entry_vt, 4) &&
                   sx_starts(sx_P(r.trg_entry_vt[2]), SX_P_ZERO_RET), "I CTriggerEntry vtable: exactly 3 slots, slot 2 = xor eax,eax; ret");
    V3X_TEST_CHECK(sx_vt_slots(r.eff_obj_vt, 26) && !sx_vt_slots(r.eff_obj_vt, 25) && !sx_vt_slots(r.eff_obj_vt, 27), "I CDebugLogEffect vtable: exactly 26 slots");
    V3X_TEST_CHECK(sx_vt_slots(r.trg_obj_vt, 26) && !sx_vt_slots(r.trg_obj_vt, 25) && !sx_vt_slots(r.trg_obj_vt, 27), "I CDebugLogTrigger vtable: exactly 26 slots");
    V3X_TEST_CHECK(v3x_testkit_rva(sx_P(r.eff_obj_vt[SX_EFF_EXEC_SLOT])) == 0x3558890 && v3x_testkit_rva(sx_P(r.trg_obj_vt[SX_TRG_EVAL_SLOT])) == 0x35a4360 &&
                   v3x_testkit_rva(sx_P(r.eff_obj_vt[25])) == 0x194dfe0 && v3x_testkit_rva(sx_P(r.trg_obj_vt[3])) == 0x35a4810 &&
                   v3x_testkit_rva(sx_P(r.eff_entry_vt[1])) == 0x35f2810 && v3x_testkit_rva(sx_P(r.trg_entry_vt[1])) == 0x35f6860,
                   "I slots: effect Execute (22) 0x3558890, trigger Evaluate (24) 0x35a4360, set-value 0x194dfe0 / 0x35a4810, Create 0x35f2810 / 0x35f6860");
    V3X_TEST_CHECK(sx_reg_build(&r) == 0 && sx_eff_obj_arr[1 + SX_EFF_EXEC_SLOT] == (void *)sx_eff_execute, "I sx_reg_build on the real vtables");
    sx_fix = NULL; sx_is_code = NULL;

    /* fail closed */
    SxRegEng bad;
    V3X_API b = a;
    g_real_find_string = a.find_string;
    b.find_string = hide_find_string;
    g_hide_string = "debug_log";
    g_hide_string_active = 1;
    int r1 = sx_reg_resolve(&b, &bad);
    g_hide_string_active = 0;
    V3X_TEST_CHECK(r1 != 0 && !bad.ok && !bad.registrar && !bad.eff_register && bad.why[0], "I without the \"debug_log\" string: registration off (%.70s)", bad.why);
    b = a;
    b.function_start = no_function_start;
    V3X_TEST_CHECK(sx_reg_resolve(&b, &bad) != 0 && !bad.ok, "I no .pdata function info: registration off (%.70s)", bad.why);
    sx_fix = fix_ptr;
    sx_is_code = NULL;    /* pointers in the unrelocated image are not code in this process: the vtable shape check must refuse */
    V3X_TEST_CHECK(sx_reg_resolve(&a, &bad) != 0 && !bad.ok, "I vtable slots that are not code pointers: registration off (%.70s)", bad.why);
    sx_fix = NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: %s <empty temp dir> [victoria3.exe]\n", argv[0]); return 2; }
    part_a();
    part_b();
    part_c(argv[1]);
    part_d(argv[1]);
    part_g();
    part_h_fake();
    if (argc > 2) { part_e(argv[2]); part_f(argv[2]); part_i(argv[2]); }
    return v3x_testkit_done();
}
