/* Offline test of plugins/console_plus (no game run). #includes the plugin source.
 * Part A (logic, always): argument-kind classification on the real 1.15.1 argument descriptions,
 *   literal choices, argument index, prefix matching (case, ambiguity, longest common prefix,
 *   duplicates), line completion, `help` parsing, console_plus.ini parsing.
 * Part B (synthetic engine, always): a fake command registry (0x210-byte entries, manager lock),
 *   fake databases and a fake CPdxArray push: registry scan installs completers only where the
 *   engine has none (unless the ini says so), fails closed on a wrong layout, never waits on a
 *   held lock; the installed completer returns the right candidates per argument position and
 *   nothing off the main thread.
 * Part C (with victoria3.exe): maps the real exe as an image (LoadLibraryEx, no imports, no code
 *   run) and checks every anchor against the 1.15.1 RVAs found with tools/re_console_*.py.
 * Part D (always): spawns itself with its own (minimised) console window, types keys into it with
 *   WriteConsoleInput and checks the ReadConsoleA hook: plain read on other threads, Tab
 *   completion on the thread named "v3x_console_input" (unique, ambiguous + listing).
 * Build (from v3x/, see README for TEMP/ZIG cache variables):
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -o D:/v3x_tmp/autocomplete/cpt.exe test/console_plus_test.c
 * Run:
 *   D:/v3x_tmp/autocomplete/cpt.exe [../binaries/victoria3.exe] */
#include "../plugins/console_plus/console_plus.c"
#include <stdarg.h>

static int fails;
#define CHECK(c, ...) do { if (c) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
                           else { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* ------------------------------------------------------------------ fake API */
static uint8_t *g_base; static size_t g_size;
static uint8_t *g_text; static size_t g_text_size;
static struct { uint8_t *p; size_t n; } g_data[8]; static int g_ndata;
static int g_nhooks, g_nframe_cbs, g_ncmds_reg;

static void t_log(const char *tag, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); printf("  [%s] ", tag); vprintf(fmt, ap); printf("\n"); va_end(ap);
}
static char g_printed[8192]; static size_t g_printed_len;
static int t_print(const char *fmt, ...)
{
    char b[2048]; va_list ap; va_start(ap, fmt); int r = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("  > %s\n", b);
    size_t n = strlen(b);
    if (g_printed_len + n + 2 < sizeof g_printed) { memcpy(g_printed + g_printed_len, b, n); g_printed_len += n; g_printed[g_printed_len++] = '\n'; g_printed[g_printed_len] = 0; }
    return r;
}
static uintptr_t t_base(void) { return (uintptr_t)g_base; }
static size_t t_size(void) { return g_size; }
static const char *t_ver(void) { return "test"; }
static void *t_scan(const char *pat)
{
    uint8_t bytes[256], mask[256]; int n = 0;
    for (const char *s = pat; *s && n < 256;) {
        if (*s == ' ') { s++; continue; }
        if (*s == '?') { bytes[n] = 0; mask[n++] = 0; s++; if (*s == '?') s++; continue; }
        char hex[3] = {s[0], s[1], 0};
        bytes[n] = (uint8_t)strtoul(hex, NULL, 16); mask[n++] = 1; s += 2;
    }
    for (size_t i = 0; g_text && i + n <= g_text_size; i++) {
        int k = 0;
        while (k < n && (!mask[k] || g_text[i + k] == bytes[k])) k++;
        if (k == n) return g_text + i;
    }
    return NULL;
}
static int scan_count(const char *pat)
{
    int c = 0; uint8_t *save = g_text; size_t ss = g_text_size;
    for (;;) {
        uint8_t *p = t_scan(pat);
        if (!p) break;
        c++;
        g_text_size -= (size_t)(p + 1 - g_text); g_text = p + 1;
    }
    g_text = save; g_text_size = ss;
    return c;
}
static void *t_find_string(const char *s)
{
    size_t n = strlen(s) + 1;
    for (int r = 0; r < g_ndata; r++)
        for (size_t i = 0; i + n <= g_data[r].n; i++)
            if (g_data[r].p[i] == (uint8_t)s[0] && !memcmp(g_data[r].p + i, s, n) && (i == 0 || g_data[r].p[i - 1] == 0))
                return g_data[r].p + i;
    return NULL;
}
static void *t_find_xref(const void *target, int index)
{
    for (size_t i = 0; g_text && i + 7 <= g_text_size; i++) {
        uint8_t rex = g_text[i], op = g_text[i + 1];
        if ((rex != 0x48 && rex != 0x4C) || (op != 0x8D && op != 0x8B) || (g_text[i + 2] & 0xC7) != 0x05) continue;
        int32_t d; memcpy(&d, g_text + i + 3, 4);
        if (g_text + i + 7 + d == (const uint8_t *)target && index-- == 0) return g_text + i;
    }
    return NULL;
}
static void *t_function_start(const void *addr)
{
    DWORD64 image = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)addr, &image, NULL);
    if (!rf) return NULL;
    for (int guard = 0; guard < 32; guard++) {
        uint8_t *ui = (uint8_t *)image + rf->UnwindData;
        if (!((ui[0] >> 3) & UNW_FLAG_CHAININFO)) break;
        rf = (PRUNTIME_FUNCTION)(ui + 4 + ((ui[2] + 1) & ~1u) * 2);
    }
    return (uint8_t *)image + rf->BeginAddress;
}
static int t_hook(void *t, void *d, void **o) { (void)d; *o = t; g_nhooks++; return 0; }
static int t_unhook(void *t) { (void)t; return 0; }
static int t_patch(void *a, const void *b, size_t n) { (void)a; (void)b; (void)n; return -1; }
static int t_on_frame(v3x_frame_fn f, void *u) { (void)f; (void)u; g_nframe_cbs++; return 0; }
static char g_dir[MAX_PATH] = "D:\\v3x_tmp\\autocomplete\\";
static const char *t_dir(void) { return g_dir; }
static int t_register(const char *name, const char *help, v3x_command_fn fn, void *user) { (void)name; (void)help; (void)fn; (void)user; g_ncmds_reg++; return 0; }
static int t_run_on_main(v3x_frame_fn fn, void *user) { fn(user); return 0; }
static int t_exec(const char *line) { (void)line; return -1; }

static V3X_API make_api(void)
{
    V3X_API a = {0};
    a.api_version = 3; a.struct_size = sizeof(V3X_API);
    a.log = t_log; a.image_base = t_base; a.image_size = t_size; a.game_version = t_ver;
    a.pattern_scan = t_scan; a.find_string = t_find_string; a.find_xref = t_find_xref;
    a.function_start = t_function_start; a.hook = t_hook; a.unhook = t_unhook; a.patch = t_patch;
    a.on_frame = t_on_frame; a.game_dir = t_dir; a.v3x_dir = t_dir;
    a.register_command = t_register; a.print = t_print; a.run_on_main = t_run_on_main; a.exec_command = t_exec;
    return a;
}

/* ------------------------------------------------------------------ Part A: logic */
static CpPool T_pool;
static int kinds_of(const char *desc, CpArg *out)
{
    return cp_classify_arg(desc, out, CP_MAX_ARGS, &T_pool);
}
static int lit_has(const CpArg *a, const char *w)
{
    for (int i = 0; i < a->nlit; i++) if (!strcmp(T_pool.w[a->lit0 + i], w)) return 1;
    return 0;
}

static void part_a(void)
{
    printf("== part A: completion logic\n");
    /* real 1.15.1 argument descriptions (tools/re_console_cmds.py / re_console_reg.py) */
    static const struct { const char *desc; int n; CpKind k[4]; } T[] = {
        {"country_TAG", 1, {CK_COUNTRY}},
        {"<country tag/id>", 1, {CK_COUNTRY}},
        {"country tag", 1, {CK_COUNTRY}},
        {"Country tag", 1, {CK_COUNTRY}},
        {"Native country tag", 1, {CK_COUNTRY}},
        {"country tag (optional, defaults to player)", 1, {CK_COUNTRY}},
        {"<country_TAG/province_ID>", 1, {CK_COUNTRY}},
        {"country ID", 1, {CK_NONE}},
        {"state ID", 1, {CK_NONE}},
        {"province id/state region tag", 1, {CK_STATE_REGION}},
        {"state region tag", 1, {CK_STATE_REGION}},
        {"<state region tag>", 1, {CK_STATE_REGION}},
        {"strategic region key", 1, {CK_STRATEGIC_REGION}},
        {"law key, country tag (optional)", 2, {CK_LAW, CK_COUNTRY}},
        {"<building key> <pm key> <state region key>", 3, {CK_BUILDING, CK_PRODUCTION_METHOD, CK_STATE_REGION}},
        {"<country ID> <company type key> <state ID> <building type key>", 4, {CK_NONE, CK_COMPANY_TYPE, CK_NONE, CK_BUILDING}},
        {"war goal type, country tag, state region key", 3, {CK_WAR_GOAL, CK_COUNTRY, CK_STATE_REGION}},
        {"<journal entry type>", 1, {CK_JOURNAL}},
        {"goods key", 1, {CK_GOODS}},
        {"culture", 1, {CK_CULTURE}},
        {"Ideology to add.", 1, {CK_IDEOLOGY}},
        {"<movement type key>", 1, {CK_MOVEMENT}},
        {"social hierarchy key", 1, {CK_SOCIAL_HIERARCHY}},
        {"strait key", 1, {CK_STRAIT}},
        {"event name", 1, {CK_EVENT}},
        {"amount", 1, {CK_NONE}},
        {"date in format yyyy.mm.dd.hh", 1, {CK_NONE}},
        {"<subgoal key (optional)>", 1, {CK_SUBGOAL}},
        {"country definition", 1, {CK_COUNTRY}},
        {"country type", 1, {CK_COUNTRY_TYPE}},
        {"Name of debug mode", 1, {CK_NONE}},
        {"stage name", 1, {CK_NONE}},          /* "tag" only at a word start */
        {"agriculture key", 1, {CK_NONE}},     /* "culture" only at a word start */
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        CpArg a[CP_MAX_ARGS]; int n = kinds_of(T[i].desc, a);
        int ok = n == T[i].n;
        for (int k = 0; ok && k < n; k++) ok = a[k].kind == T[i].k[k];
        if (!ok) { bad++; printf("FAIL   classify '%s': %d args, first %s\n", T[i].desc, n, n ? CP_KINDS[a[0].kind].name : "-"); }
    }
    CHECK(!bad, "classification of %d real argument descriptions", (int)(sizeof T / sizeof T[0]));

    CpArg a[CP_MAX_ARGS]; int n;
    n = kinds_of("all/tag", a);
    CHECK(n == 1 && a[0].kind == CK_COUNTRY && a[0].nlit == 1 && lit_has(&a[0], "all"), "'all/tag' -> country + literal 'all'");
    n = kinds_of("Arguments: lines/alwaysupgradecolony/ai/influence/market/alwaysreform/allmoney/alwaysdiplo/artisanchange/focusai/info/profile/pops/tooltips/assert/promotiondesc/demotiondesc/cb_use/eco/textures/fow/yesmen/wireframe/minzoom/render/color", a);
    CHECK(n == 1 && a[0].kind == CK_NONE && a[0].nlit == 26 && lit_has(&a[0], "lines") && lit_has(&a[0], "color") && lit_has(&a[0], "cb_use"),
          "`debug` argument -> 26 literal words (%d)", n ? a[0].nlit : -1);
    n = kinds_of("<0/1>", a);
    CHECK(n == 1 && a[0].nlit == 2 && lit_has(&a[0], "0") && lit_has(&a[0], "1"), "'<0/1>' -> 0 1");
    n = kinds_of("<start|stop>", a);
    CHECK(n == 1 && a[0].nlit == 2 && lit_has(&a[0], "start") && lit_has(&a[0], "stop"), "'<start|stop>' -> start stop");
    n = kinds_of("<(Optional) Failed Test Save: before/none/after>", a);
    CHECK(n == 1 && a[0].nlit == 3 && lit_has(&a[0], "before") && lit_has(&a[0], "after"), "'Failed Test Save: before/none/after' -> 3 words");
    n = kinds_of("<region_id|all>", a);
    CHECK(n == 1 && a[0].nlit == 1 && lit_has(&a[0], "all"), "'<region_id|all>' -> 'all' only (region_id is a placeholder)");
    n = kinds_of("true/false", a);
    CHECK(n == 1 && a[0].nlit == 2, "'true/false' -> 2 words");
    {
        const char *d[] = {"province id/state region tag", "country tag"};
        CpArg b[CP_MAX_ARGS]; int m = cp_classify_args(d, 2, b, CP_MAX_ARGS, &T_pool);
        CHECK(m == 2 && b[0].kind == CK_STATE_REGION && b[1].kind == CK_COUNTRY, "`own`: state_region, country");
        CHECK(cp_any_arg(b, m), "cp_any_arg");
        const char *d2[] = {"amount"};
        m = cp_classify_args(d2, 1, b, CP_MAX_ARGS, &T_pool);
        CHECK(m == 1 && !cp_any_arg(b, m), "`money amount` has nothing to complete");
    }

    CHECK(cp_arg_index("") == 0 && cp_arg_index(NULL) == 0, "arg index: '' -> 0");
    CHECK(cp_arg_index("SOK") == 1 && cp_arg_index("  SOK  ") == 1, "arg index: 'SOK' -> 1");
    CHECK(cp_arg_index("STATE_X SOK") == 2 && cp_arg_index("a\tb  c") == 3, "arg index: two/three tokens");

    /* prefix matching */
    static const char *const C[] = {"SOK", "SAR", "SAF", "sardinia", "GBR", "SOK", "Sokoto"};
    int idx[16]; char common[64];
    int m = cp_match("so", C, 7, idx, 16, common, sizeof common);
    CHECK(m == 2 && !strcmp(C[idx[0]], "SOK") && !strcmp(C[idx[1]], "Sokoto") && !strcmp(common, "soK"),
          "prefix 'so': SOK + Sokoto (duplicate SOK once), common 'soK' (typed part kept, rest from the first match) -> %d '%s'", m, common);
    m = cp_match("SA", C, 7, idx, 16, common, sizeof common);
    CHECK(m == 3 && !strcmp(common, "SA"), "prefix 'SA': 3 matches, case-insensitive (SAR SAF sardinia), common 'SA'");
    m = cp_match("sar", C, 7, idx, 16, common, sizeof common);
    CHECK(m == 2 && !strcmp(common, "sar"), "prefix 'sar': SAR + sardinia, common = what was typed: '%s'", common);
    m = cp_match("gB", C, 7, idx, 16, common, sizeof common);
    CHECK(m == 1 && !strcmp(C[idx[0]], "GBR") && !strcmp(common, "gBR"), "unique 'gB' -> GBR");
    m = cp_match("xyz", C, 7, idx, 16, common, sizeof common);
    CHECK(m == 0 && !strcmp(common, "xyz"), "no match keeps the token");
    m = cp_match("", C, 7, idx, 16, common, sizeof common);
    CHECK(m == 6, "empty token matches all distinct candidates (%d)", m);
    m = cp_match("s", C, 7, idx, 2, common, sizeof common);
    CHECK(m == 5, "match count is exact even when idx[] is small (%d)", m);
}

static const char HELP_TEXT[] =
    "24 commands (type a command and press Enter):\n"
    "  help       list commands, 'help <name>' for one\n"
    "  plugins    list loaded plugins\n"
    "  profile    profile on|off|report|reset - devtools sampling profiler\n"
    "  lazymesh   lazy_mesh: status | dump (write v3x/lazy_mesh_used.txt) | load <type name>\n"
    "  lean       lean [on|off]: debug_mode without its debug-only GUI/tooltips (enhance lean_debug)\n"
    "  log        write a line to v3x.log\n"
    "  mapcache   map_cache: status | report (write v3x/map_cache_report.txt) | flush (release pins)\n"
    "  complete   console_plus: completion status\n";

static void part_a2(void)
{
    printf("== part A2: V3X console (help parsing, line completion, ini)\n");
    static CpHelp h;
    cp_parse_help(HELP_TEXT, &h);
    CHECK(h.n == 8 && !strcmp(h.name[0], "help") && !strcmp(h.name[7], "complete"), "help parse: 8 commands (%d)", h.n);
    int ip = cp_help_find(&h, "PROFILE"), il = cp_help_find(&h, "lazymesh"), ilean = cp_help_find(&h, "lean");
    CHECK(ip >= 0 && h.nsub[ip] == 4 && !strcmp(h.sub[ip][0], "on") && !strcmp(h.sub[ip][3], "reset"), "profile sub-words on|off|report|reset");
    CHECK(il >= 0 && h.nsub[il] == 3 && !strcmp(h.sub[il][0], "status") && !strcmp(h.sub[il][2], "load"), "lazymesh sub-words status dump load (not 'type')");
    CHECK(ilean >= 0 && h.nsub[ilean] == 2, "lean sub-words on off");
    CHECK(cp_help_find(&h, "log") >= 0 && h.nsub[cp_help_find(&h, "log")] == 0, "log: no sub-words");

    CpResult r;
    cp_complete_line("pro", v3x_cands, NULL, &r);   /* uses g_help */
    CHECK(r.nmatch == 0, "no help captured yet -> nothing");
    g_help = h;
    cp_complete_line("pro", v3x_cands, NULL, &r);
    CHECK(r.nmatch == 1 && !strcmp(r.line, "profile ") && r.changed, "'pro' -> 'profile '");
    cp_complete_line("l", v3x_cands, NULL, &r);
    CHECK(r.nmatch == 3 && !strcmp(r.line, "l") && !r.changed, "'l' ambiguous (lazymesh lean log), line unchanged");
    cp_complete_line("la", v3x_cands, NULL, &r);
    CHECK(r.nmatch == 1 && !strcmp(r.line, "lazymesh "), "'la' -> 'lazymesh '");
    cp_complete_line("le", v3x_cands, NULL, &r);
    CHECK(r.nmatch == 1 && !strcmp(r.line, "lean "), "'le' -> 'lean '");
    cp_complete_line("PROFILE r", v3x_cands, NULL, &r);
    CHECK(r.nmatch == 2 && !strcmp(r.line, "PROFILE re"), "'PROFILE r' -> common 're' (report/reset): '%s'", r.line);
    cp_complete_line("profile rep", v3x_cands, NULL, &r);
    CHECK(r.nmatch == 1 && !strcmp(r.line, "profile report "), "'profile rep' -> 'profile report '");
    cp_complete_line("  mapcache  f", v3x_cands, NULL, &r);
    CHECK(r.nmatch == 1 && !strcmp(r.line, "  mapcache  flush "), "leading/double blanks kept: '%s'", r.line);
    cp_complete_line("profile on x", v3x_cands, NULL, &r);
    CHECK(r.nmatch == 0 && !strcmp(r.line, "profile on x"), "third word: nothing");
    cp_complete_line("", v3x_cands, NULL, &r);
    CHECK(r.nmatch == 8 && !strcmp(r.line, ""), "empty line lists all commands");
    memset(&g_help, 0, sizeof g_help);

    CpCfg c;
    cp_cfg_defaults(&c);
    CHECK(c.enabled && c.game_console && c.v3x_console && !c.override_engine && c.scan_frames == 120, "ini defaults");
    cp_parse_ini("# comment\n[console_plus]\nv3x_console = 0\noverride_engine=1\nscan_frames=3\nbogus=1\n"
                 "[commands]\nown = state_region country\nTag=country\nresearch = tech\nfoo = nosuchkind\nmoney = none\n", &c);
    CHECK(!c.v3x_console && c.override_engine && c.scan_frames == 10, "ini values (scan_frames clamped to 10)");
    int io = cp_cfg_override(&c, "OWN"), it = cp_cfg_override(&c, "tag");
    CHECK(c.novr == 4 && io >= 0 && c.ovr[io].nkinds == 2 && c.ovr[io].kinds[0] == CK_STATE_REGION && c.ovr[io].kinds[1] == CK_COUNTRY,
          "[commands] own = state_region country");
    CHECK(it >= 0 && c.ovr[it].kinds[0] == CK_COUNTRY && cp_cfg_override(&c, "foo") < 0, "Tag=country (case), unknown kind rejected");
    CHECK(c.bad_lines == 2, "2 bad lines (bogus key, unknown kind): %d", c.bad_lines);
    FILE *ex = fopen("plugins/console_plus/console_plus.ini.example", "rb");
    if (ex) {
        static char t[16384]; size_t k = fread(t, 1, sizeof t - 1, ex); t[k] = 0; fclose(ex);
        CpCfg d, e; cp_cfg_defaults(&d); cp_cfg_defaults(&e); cp_parse_ini(t, &e);
        CHECK(!memcmp(&d, &e, sizeof d), "console_plus.ini.example parses to exactly the built-in defaults (bad lines %d)", e.bad_lines);
    } else printf("(ini example check skipped: run from v3x/)\n");
}

/* ------------------------------------------------------------------ Part B: synthetic engine */
#define FN 8
static uint8_t F_entries[FN][E_SIZE];
static struct { uint8_t *data; int32_t cap, count; uint8_t pad[0xa8 - 16]; volatile LONG lock; uint8_t pad2[0x40]; } F_mgr;
static void *F_mgr_ptr;
static char F_out[4096][32]; static int F_nout;
static void *fake_push(CArr *arr, const CStr *s)
{
    (void)arr;
    if (F_nout < 4096) cstr_copy(s, F_out[F_nout++], 32);
    return NULL;
}
/* two fake CGameDatabases: countries and state regions */
static void *F_vt_country[4], *F_vt_state[4];
typedef struct { void *vt; uint8_t pad[0x10]; CStr key; } FObj;    /* key at +0x18 */
static FObj F_c[3], F_s[2];
static FObj *F_cp[3] = {&F_c[0], &F_c[1], &F_c[2]}, *F_sp[2] = {&F_s[0], &F_s[1]};
static struct { void *vt; uint8_t pad[0x78]; FObj **arr; int32_t cap, count; } F_dbc, F_dbs;
static struct { void *db; uint8_t has; } F_slotc, F_slots;
static void **fake_get_country(void) { return (void **)&F_slotc; }
static void **fake_get_state(void) { return (void **)&F_slots; }
static int F_event_calls;
static void fake_event_cb(const CStr *prev, CArr *out, void *user) { (void)prev; (void)user; F_event_calls++; CStr t; cstr_temp(&t, "debug.1"); fake_push(out, &t); }
static void fake_mapmode_cb(const CStr *prev, CArr *out, void *user) { (void)prev; (void)out; (void)user; }

static void set_cstr(CStr *s, const char *v) { cstr_temp(s, v); }
static void make_entry(int i, const char *name, const char *desc, const char *a0, const char *a1, void *cb)
{
    uint8_t *e = F_entries[i];
    memset(e, 0, E_SIZE);
    set_cstr((CStr *)(e + E_NAME), name);
    set_cstr((CStr *)(e + E_DESC), desc);
    for (int k = 0; k < E_NARGSTR; k++) set_cstr((CStr *)(e + E_ARGS + 0x20 * k), "");
    if (a0) set_cstr((CStr *)(e + E_ARGS), a0);
    if (a1) set_cstr((CStr *)(e + E_ARGS + 0x20), a1);
    *(void **)(e + E_CB) = cb;
}

static complete_fn entry_cb(int i) { return *(complete_fn *)(F_entries[i] + E_CB); }
static void *entry_user(int i) { return *(void **)(F_entries[i] + E_USER); }
static int run_cb(int i, const char *prev)
{
    CStr p; cstr_temp(&p, prev);
    F_nout = 0;
    complete_fn f = entry_cb(i);
    if (f) f(&p, (CArr *)&F_out, entry_user(i));
    return F_nout;
}
static int out_has(const char *w) { for (int i = 0; i < F_nout; i++) if (!strcmp(F_out[i], w)) return 1; return 0; }

static void reset_plugin_state(void)
{
    memset(g_cmds, 0, sizeof g_cmds); g_ncmd = 0; memset(&g_pool, 0, sizeof g_pool);
    g_last_count = -1; g_validated = 0; g_invalid = 0; g_loads = 0; g_vfails_after_load = 0; g_vlog_help = g_vlog_cb = g_vlog_bad = -1; g_frame = 0; g_event_cb = NULL; g_event_user = NULL;
    g_calls = g_pushed = g_bad_db = 0;
}

static void setup_engine(void)
{
    memset(&A, 0, sizeof A);
    A.ok = 1; A.mgr_slot = &F_mgr_ptr; A.push = fake_push;
    A.dbget[CK_COUNTRY] = fake_get_country; A.dbvt[CK_COUNTRY] = F_vt_country;
    A.dbget[CK_STATE_REGION] = fake_get_state; A.dbvt[CK_STATE_REGION] = F_vt_state;
    const char *tags[3] = {"SOK", "GBR", "SAR"}, *st[2] = {"STATE_SOKOTO", "STATE_LONDON"};
    for (int i = 0; i < 3; i++) { F_c[i].vt = (void *)1; set_cstr(&F_c[i].key, tags[i]); }
    for (int i = 0; i < 2; i++) { F_s[i].vt = (void *)1; set_cstr(&F_s[i].key, st[i]); }
    F_dbc.vt = F_vt_country; F_dbc.arr = F_cp; F_dbc.count = 3;
    F_dbs.vt = F_vt_state; F_dbs.arr = F_sp; F_dbs.count = 2;
    F_slotc.db = &F_dbc; F_slotc.has = 1; F_slots.db = &F_dbs; F_slots.has = 1;
    make_entry(0, "help", "Print out all console commands or a specific command description.", "command name", NULL, NULL);
    make_entry(1, "event", "Executes an event", "event name", "<country_TAG/province_ID>", (void *)fake_event_cb);
    make_entry(2, "tag", "Switch control to another country", "country_TAG", NULL, NULL);
    make_entry(3, "own", "Change the owner of specified province or state region", "province id/state region tag", "country tag", NULL);
    make_entry(4, "money", "Adds specified amount of money", "amount", NULL, NULL);
    make_entry(5, "mapmode", "Switches to a given mapmode", "mapmode", NULL, (void *)fake_mapmode_cb);
    make_entry(6, "testevent", "Tests an event", "event name", "<country_TAG/province_ID>", NULL);
    make_entry(7, "disable_ai", "Disables AI", "all/tag", NULL, NULL);
    F_mgr.data = (uint8_t *)F_entries; F_mgr.cap = FN; F_mgr.count = FN; F_mgr.lock = 0;
    F_mgr_ptr = NULL;
    reset_plugin_state();
    cp_cfg_defaults(&g_cfg);
    g_cfg.log_installs = 0;
}

static void part_b(void)
{
    printf("== part B: synthetic registry + databases\n");
    g_main_tid = GetCurrentThreadId();
    setup_engine();
    scan_registry();
    CHECK(g_ncmd == 0 && !g_validated, "no manager yet -> nothing (manager is never created by the plugin)");
    F_mgr_ptr = &F_mgr;
    F_mgr.lock = 1;                               /* engine writer holds the lock */
    scan_registry();
    CHECK(g_ncmd == 0 && F_mgr.lock == 1, "lock held by the engine -> skipped, lock untouched");
    F_mgr.lock = 2;                               /* one reader active */
    scan_registry();
    CHECK(g_ncmd == 0 && F_mgr.lock == 2, "reader active -> skipped (writer bit released): lock=%ld", (long)F_mgr.lock);
    F_mgr.lock = 0;
    scan_registry();
    CHECK(g_validated && F_mgr.lock == 0, "registry validated, lock released");
    CHECK(g_ncmd == 4, "4 commands got a completer: tag own testevent disable_ai (%d)", g_ncmd);
    CHECK(entry_cb(1) == fake_event_cb && entry_cb(5) == fake_mapmode_cb, "engine completers (event, mapmode) untouched");
    CHECK(entry_cb(4) == NULL && entry_cb(0) == NULL, "money/help (nothing to complete) untouched");
    CHECK(entry_cb(2) == cp_complete && entry_cb(3) == cp_complete, "tag/own -> console_plus completer");
    CHECK(g_event_cb == fake_event_cb, "engine `event` completer remembered");

    int n = run_cb(2, "");
    CHECK(n == 3 && out_has("SOK") && out_has("GBR") && out_has("SAR"), "tag <Tab>: 3 country tags");
    n = run_cb(3, "");
    CHECK(n == 2 && out_has("STATE_SOKOTO"), "own <Tab>: state regions");
    n = run_cb(3, "STATE_SOKOTO");
    CHECK(n == 3 && out_has("SOK"), "own STATE_SOKOTO <Tab>: country tags");
    n = run_cb(3, "STATE_SOKOTO SOK");
    CHECK(n == 0, "own: no third argument");
    n = run_cb(6, "");
    CHECK(n == 1 && out_has("debug.1") && F_event_calls == 1, "testevent <Tab>: delegated to the engine's event completer");
    n = run_cb(6, "debug.1");
    CHECK(n == 3, "testevent debug.1 <Tab>: country tags");
    n = run_cb(7, "");
    CHECK(n == 4 && out_has("all") && out_has("SOK"), "disable_ai <Tab>: 'all' + tags");

    /* wrong database object -> nothing, counted */
    F_dbc.vt = F_vt_state;
    n = run_cb(2, "");
    CHECK(n == 0 && g_bad_db == 1, "database vtable mismatch -> no candidates (fail closed)");
    F_dbc.vt = F_vt_country;
    /* bad keys skipped */
    F_c[1].key.size = 5000;
    n = run_cb(2, "");
    CHECK(n == 2 && !out_has("GBR"), "implausible key CString skipped");
    set_cstr(&F_c[1].key, "GBR");

    /* off the main thread: nothing */
    g_main_tid = GetCurrentThreadId() + 1;
    n = run_cb(2, "");
    CHECK(n == 0, "completer on another thread -> nothing");
    g_main_tid = GetCurrentThreadId();

    /* rescan with the same count: nothing new; registry grows: only the new entry */
    int before = g_ncmd;
    scan_registry();
    CHECK(g_ncmd == before, "same count -> no rescan");
    F_mgr.count = FN - 1;
    make_entry(4, "annex", "Annexes a country", "<country tag/id>", NULL, NULL);
    scan_registry();
    CHECK(g_ncmd == before + 1 && entry_cb(4) == cp_complete, "changed registry: new `annex` gets a completer");
    n = run_cb(4, "");
    CHECK(n == 3, "annex <Tab>: tags");

    /* entries moved (array realloc): copies keep the completer, nothing re-added */
    static uint8_t moved[FN][E_SIZE];
    memcpy(moved, F_entries, sizeof moved);
    F_mgr.data = (uint8_t *)moved; F_mgr.count = FN;
    scan_registry();
    CHECK(g_ncmd == before + 1, "moved entries: no duplicates (%d)", g_ncmd);
    F_mgr.data = (uint8_t *)F_entries;

    /* ini override + override_engine */
    setup_engine();
    F_mgr_ptr = &F_mgr;
    cp_parse_ini("[commands]\nmoney = country\nmapmode = state_region\n", &g_cfg);
    scan_registry();
    CHECK(entry_cb(4) == cp_complete && entry_cb(5) == cp_complete, "ini: money and mapmode (replaces engine completer) completed");
    n = run_cb(5, "");
    CHECK(n == 2 && out_has("STATE_LONDON"), "mapmode <Tab> (ini): state regions");
    CHECK(entry_cb(1) == fake_event_cb, "event still the engine's");
    setup_engine();
    F_mgr_ptr = &F_mgr;
    g_cfg.override_engine = 1;
    scan_registry();
    CHECK(entry_cb(1) == cp_complete && entry_cb(5) == fake_mapmode_cb, "override_engine=1: event replaced; mapmode (no kind) kept");

    /* registry not complete yet (main menu): retried, nothing written, then works */
    setup_engine();
    F_mgr_ptr = &F_mgr;
    set_cstr((CStr *)(F_entries[0] + E_DESC), "something else");
    *(void **)(F_entries[1] + E_CB) = NULL; *(void **)(F_entries[5] + E_CB) = NULL;
    scan_registry();
    CHECK(!g_invalid && !g_validated && g_ncmd == 0 && entry_cb(2) == NULL && F_mgr.lock == 0, "help/completers missing -> not yet: nothing written, not off");
    scan_registry(); on_frame(NULL);
    CHECK(!g_invalid && g_ncmd == 0, "same count: still retried, still nothing");
    set_cstr((CStr *)(F_entries[0] + E_DESC), "Print out all console commands or a specific command description.");
    *(void **)(F_entries[1] + E_CB) = (void *)fake_event_cb; *(void **)(F_entries[5] + E_CB) = (void *)fake_mapmode_cb;
    scan_registry();
    CHECK(g_validated && g_ncmd == 4 && entry_cb(2) == cp_complete, "registry complete later (same count) -> validated, installed");
    /* still failing after a game load -> off after 5 more scans */
    setup_engine();
    F_mgr_ptr = &F_mgr;
    set_cstr((CStr *)(F_entries[0] + E_DESC), "something else");
    on_load_transition("load_transition", NULL, NULL);
    for (int i = 0; i < 4; i++) scan_registry();
    CHECK(!g_invalid, "after a load: 4 failed checks, still retrying");
    scan_registry();
    CHECK(g_invalid && g_ncmd == 0 && entry_cb(2) == NULL, "5th failed check after a load -> off, nothing written");
    on_frame(NULL);
    CHECK(g_ncmd == 0, "off stays off");
    /* names that are not strings: layout really differs -> off at once */
    setup_engine();
    F_mgr_ptr = &F_mgr;
    for (int i = 0; i < FN; i++) { CStr *ns = (CStr *)(F_entries[i] + E_NAME); ns->size = 0x7fffffff; }
    scan_registry();
    CHECK(g_invalid && g_ncmd == 0, "unreadable names -> layout differs, off immediately");

    /* on_frame cadence */
    setup_engine();
    F_mgr_ptr = &F_mgr;
    g_cfg.scan_frames = 10;
    for (int i = 0; i < 9; i++) { g_frame = i + 1; on_frame(NULL); }
    CHECK(g_ncmd == 0, "on_frame scans only every scan_frames frames");
    g_frame = 0; on_frame(NULL);
    CHECK(g_ncmd == 4, "frame 0 scans");

    /* `complete` command output */
    g_printed_len = 0; g_printed[0] = 0;
    cmd_complete("own", NULL);
    CHECK(strstr(g_printed, "arg 1: state_region") && strstr(g_printed, "arg 2: country"), "`complete own` prints the kinds");
    g_printed_len = 0;
    cmd_complete("disable_ai", NULL);
    CHECK(strstr(g_printed, "country + words: all") != NULL, "`complete disable_ai`: country + words: all");
    g_printed_len = 0;
    cmd_complete("", NULL);
    CHECK(strstr(g_printed, "4 commands completed") != NULL, "`complete` status");
}

/* ------------------------------------------------------------------ Part C: real exe */
static void part_c(const char *exe)
{
    printf("== part C: %s\n", exe);
    HMODULE m = LoadLibraryExA(exe, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!m) { printf("FAIL cannot map %s (%lu)\n", exe, GetLastError()); fails++; return; }
    g_base = (uint8_t *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    g_size = nt->OptionalHeader.SizeOfImage;
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (!memcmp(s->Name, ".text", 5)) { g_text = g_base + s->VirtualAddress; g_text_size = s->Misc.VirtualSize; }
        else if ((!memcmp(s->Name, ".rdata", 6) || !memcmp(s->Name, ".data", 5)) && g_ndata < 8)
            { g_data[g_ndata].p = g_base + s->VirtualAddress; g_data[g_ndata++].n = s->Misc.VirtualSize; }
    }
#define RVA(p) ((unsigned long long)((uint8_t *)(p) - g_base))
    CHECK(scan_count(PAT_GETTER) == 1, "manager getter pattern unique");
    CHECK(scan_count(PAT_CBCALL) == 1, "completer call pattern unique");
    CHECK(scan_count(PAT_KEYLOOP) == 2, "key-listing loop pattern: 2 engine completers (mapmode, research)");
    int ok = find_anchors();
    CHECK(ok && A.ok, "find_anchors");
    CHECK(RVA(A.f_ac) == 0x36be560, "CConsole::Autocomplete exe+0x%llx", RVA(A.f_ac));
    CHECK(RVA(A.getter) == 0x3dc0e00 && RVA(A.mgr_slot) == 0x5c72730, "manager getter exe+0x%llx, slot exe+0x%llx", RVA(A.getter), RVA(A.mgr_slot));
    CHECK(RVA(A.matcher) == 0x3dc2140 && RVA(A.cbcall) == 0x3dc24cd, "matcher exe+0x%llx, completer call exe+0x%llx", RVA(A.matcher), RVA(A.cbcall));
    CHECK(RVA(A.push) == 0x74c7f0, "CPdxArray<CString>::push_back exe+0x%llx", RVA(A.push));
    static const struct { CpKind k; unsigned long long rva; } G[] = {
        {CK_COUNTRY, 0x7d0f00}, {CK_STATE_REGION, 0x79e6b0}, {CK_STRATEGIC_REGION, 0x79e530}, {CK_LAW, 0x7d0180},
        {CK_GOODS, 0x7cc400}, {CK_JOURNAL, 0x7d0000}, {CK_BUILDING, 0x7ce800}, {CK_PRODUCTION_METHOD, 0x7d9070},
        {CK_CULTURE, 0x7cc700}, {CK_RELIGION, 0x7d9670}, {CK_IDEOLOGY, 0x7cf580}, {CK_POP_TYPE, 0x7d8440},
        {CK_DECREE, 0x7d4c90}, {CK_TECH, 0x7d1b10}, {CK_MOVEMENT, 0x7d82c0}, {CK_COUNTRY_TYPE, 0x7d4810},
        {CK_WAR_GOAL, 0x7ccb80}, {CK_COMPANY_TYPE, 0x7cec80}, {CK_SOCIAL_HIERARCHY, 0x7d9af0}, {CK_STRAIT, 0x7ce680},
        {CK_IG_TYPE, 0x7cf700}, {CK_SUBGOAL, 0x7d79c0},
    };
    int good = 0;
    for (size_t i = 0; i < sizeof G / sizeof G[0]; i++) {
        int g = A.dbget[G[i].k] && RVA(A.dbget[G[i].k]) == G[i].rva && A.dbvt[G[i].k];
        if (!g) printf("FAIL   %s getter exe+0x%llx (want 0x%llx)\n", CP_KINDS[G[i].k].name, A.dbget[G[i].k] ? RVA(A.dbget[G[i].k]) : 0ull, G[i].rva);
        good += g;
    }
    CHECK(good == (int)(sizeof G / sizeof G[0]), "%d/%d database getters at their 1.15.1 RVAs, DB vtables found", good, (int)(sizeof G / sizeof G[0]));
    CHECK(A.dbvt[CK_COUNTRY] && RVA(A.dbvt[CK_COUNTRY]) == 0x4687050, "CCountryDefinitionDatabase vtable exe+0x%llx", A.dbvt[CK_COUNTRY] ? RVA(A.dbvt[CK_COUNTRY]) : 0ull);

    /* plugin load against the real image: game part on, V3X console off (ini), no hooks in game code */
    FILE *f = fopen("D:/v3x_tmp/autocomplete/console_plus.ini", "w");
    if (f) { fputs("[console_plus]\nv3x_console=0\n", f); fclose(f); }
    g_nhooks = g_nframe_cbs = g_ncmds_reg = 0;
    int rc = v3x_plugin_load(api);
    CHECK(rc == 0 && A.ok && g_nframe_cbs == 1 && g_nhooks == 0 && g_ncmds_reg == 1,
          "v3x_plugin_load: rc %d, on_frame %d, hooks %d (none: the in-game part hooks nothing), `complete` registered", rc, g_nframe_cbs, g_nhooks);
    remove("D:/v3x_tmp/autocomplete/console_plus.ini");
}


/* ------------------------------------------------------------------ Part D: V3X console Tab (child with its own console) */
static void key(INPUT_RECORD *r, int *n, WCHAR ch, WORD vk)
{
    for (int down = 1; down >= 0; down--) {
        INPUT_RECORD *k = &r[(*n)++];
        memset(k, 0, sizeof *k);
        k->EventType = KEY_EVENT;
        k->Event.KeyEvent.bKeyDown = down; k->Event.KeyEvent.wRepeatCount = 1;
        k->Event.KeyEvent.wVirtualKeyCode = vk; k->Event.KeyEvent.uChar.UnicodeChar = ch;
    }
}
static void type_str(INPUT_RECORD *r, int *n, const char *s)
{
    for (; *s; s++) {
        WORD vk = (*s >= 'a' && *s <= 'z') ? (WORD)(*s - 32) : (*s == ' ' ? VK_SPACE : 0);
        key(r, n, (WCHAR)*s, vk);
    }
}
static int console_child(const char *outfile)
{
    FILE *res = fopen(outfile, "w");
    if (!res) return 3;
    HANDLE in = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    g_conout = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (in == INVALID_HANDLE_VALUE || g_conout == INVALID_HANDLE_VALUE) { fprintf(res, "no console\n"); fclose(res); return 4; }
    SetConsoleMode(in, ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_INSERT_MODE | ENABLE_EXTENDED_FLAGS);
    p_GetThreadDescription = (GetThreadDescription_t)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    typedef HRESULT(WINAPI * SetThreadDescription_t)(HANDLE, PCWSTR);
    SetThreadDescription_t sd = (SetThreadDescription_t)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadDescription");
    o_ReadConsoleA = (ReadConsoleA_t)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "ReadConsoleA");
    cp_parse_help(HELP_TEXT, &g_help); g_help_state = 1;
    char buf[2048]; DWORD n = 0;
    static INPUT_RECORD r[256]; int nr = 0; DWORD w;
    /* 1: not the V3X input thread -> plain ReadConsoleA (Tab is just a character) */
    type_str(r, &nr, "pro"); key(r, &nr, L'\t', VK_TAB); key(r, &nr, L'\r', VK_RETURN);
    WriteConsoleInputW(in, r, (DWORD)nr, &w);
    BOOL ok = hk_ReadConsoleA(in, buf, sizeof buf - 1, &n, NULL); buf[ok ? n : 0] = 0;
    fprintf(res, "plain=[%s]\n", buf);
    /* 2: named thread: "pro<Tab>rep<Tab>" -> "profile report " */
    if (sd) sd(GetCurrentThread(), L"v3x_console_input");
    g_input_tid = 0;
    nr = 0;
    type_str(r, &nr, "pro"); key(r, &nr, L'\t', VK_TAB); type_str(r, &nr, "rep"); key(r, &nr, L'\t', VK_TAB); key(r, &nr, L'\r', VK_RETURN);
    WriteConsoleInputW(in, r, (DWORD)nr, &w);
    ok = hk_ReadConsoleA(in, buf, sizeof buf - 1, &n, NULL); buf[ok ? n : 0] = 0;
    fprintf(res, "tab=[%s] tabs=%ld\n", buf, (long)g_tabs);
    /* 3: ambiguous "l<Tab>" lists, then "e<Tab>" -> "lean " */
    nr = 0;
    type_str(r, &nr, "l"); key(r, &nr, L'\t', VK_TAB); type_str(r, &nr, "e"); key(r, &nr, L'\t', VK_TAB); type_str(r, &nr, "on"); key(r, &nr, L'\r', VK_RETURN);
    WriteConsoleInputW(in, r, (DWORD)nr, &w);
    ok = hk_ReadConsoleA(in, buf, sizeof buf - 1, &n, NULL); buf[ok ? n : 0] = 0;
    fprintf(res, "amb=[%s] tabs=%ld\n", buf, (long)g_tabs);
    /* screen: the candidate list was printed */
    CONSOLE_SCREEN_BUFFER_INFO bi; GetConsoleScreenBufferInfo(g_conout, &bi);
    char scr[4096] = {0}; DWORD rd = 0; COORD c0 = {0, 0};
    DWORD cells = (DWORD)bi.dwSize.X * (DWORD)(bi.dwCursorPosition.Y + 1);
    if (cells > sizeof scr - 1) cells = sizeof scr - 1;
    ReadConsoleOutputCharacterA(g_conout, scr, cells, c0, &rd);
    fprintf(res, "list=%d\n", strstr(scr, "lazymesh") && strstr(scr, "lean") && strstr(scr, "log") ? 1 : 0);
    fclose(res);
    return 0;
}

static void part_d(const char *self)
{
    printf("== part D: V3X console Tab completion in a child process with its own console\n");
    const char *rf = "D:/v3x_tmp/autocomplete/console_child.txt";
    remove(rf);
    char cmd[MAX_PATH * 2]; snprintf(cmd, sizeof cmd, "\"%s\" --console-child %s", self, rf);
    STARTUPINFOA si; PROCESS_INFORMATION pi; memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_MINIMIZE;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) { CHECK(0, "spawn child (%lu)", GetLastError()); return; }
    DWORD wr = WaitForSingleObject(pi.hProcess, 20000);
    if (wr != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 9);
    DWORD ec = 99; GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    CHECK(wr == WAIT_OBJECT_0 && ec == 0, "child finished (exit %lu)", ec);
    char t[2048] = {0};
    FILE *f = fopen(rf, "r"); if (f) { size_t k = fread(t, 1, sizeof t - 1, f); t[k] = 0; fclose(f); }
    printf("%s", t);
    CHECK(strstr(t, "plain=[pro\tprofile") == NULL && strstr(t, "plain=[pro") != NULL, "other thread: plain read (no completion)");
    CHECK(strstr(t, "tab=[profile report \r\n]") != NULL || strstr(t, "tab=[profile report \n]") != NULL, "V3X input thread: 'pro<Tab>rep<Tab>' -> 'profile report '");
    CHECK(strstr(t, "amb=[lean on") != NULL, "'l<Tab>' (ambiguous) then 'e<Tab>on' -> 'lean on'");
    CHECK(strstr(t, "list=1") != NULL, "ambiguous Tab listed lazymesh lean log on screen");
    remove(rf);
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "--console-child")) { static V3X_API ca; ca = make_api(); api = &ca; return console_child(argv[2]); }
    static V3X_API a;
    a = make_api();
    api = &a;
    part_a();
    part_a2();
    part_b();
    if (argc > 1) part_c(argv[1]);
    else printf("(part C skipped: no victoria3.exe given)\n");
    part_d(argv[0]);
    printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
    return fails ? 1 : 0;
}
