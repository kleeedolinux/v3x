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
 * Part A3 (always): display names: accent folding, localization text cleaning, ranked name search
 *   (exact / prefix / word / substring, name@ctx), field discovery, decoding of the engine's
 *   "object by numeric id" lookups.
 * Part B2 (always): fake localization map, databases and live game state: name -> id through the
 *   matcher pre-step (tag Prus -> PRU, own sao_pa -> STATE_SAO_PAULO, event titles, state /
 *   country / character ids, ambiguity listing in the console, find), hook wrapper.
 * Part D (always): spawns itself with its own (minimised) console window, types keys into it with
 *   WriteConsoleInput and checks the ReadConsoleA hook: plain read on other threads, Tab
 *   completion on the thread named "v3x_console_input" (unique, ambiguous + listing).
 * Build (from v3x/, see README for TEMP/ZIG cache variables):
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -o $V3X_TMP/cpt.exe test/console_plus_test.c
 * Run:
 *   $V3X_TMP/cpt.exe [../binaries/victoria3.exe]
 * Scratch files go to $V3X_TMP/namecomplete/ (else %TEMP%, see test/test_tmp.h). Part D prints a SKIP
 * line (not a failure) when the child cannot get a console (no window station, e.g. a service). */
#include "../plugins/console_plus/console_plus.c"
#include <stdarg.h>
#include "test_tmp.h"

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
static char g_dir[MAX_PATH];   /* <scratch>/namecomplete/, set in main */
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
        {"country ID", 1, {CK_N_COUNTRY}},
        {"state ID", 1, {CK_N_STATE}},
        {"character ID", 1, {CK_N_CHARACTER}},
        {"interest group ID", 1, {CK_N_IG}},
        {"<building ID> <new wage rate> (optional)", 2, {CK_N_BUILDING, CK_NONE}},
        {"<province ID>", 1, {CK_NONE}},
        {"Source Formation ID", 1, {CK_NONE}},
        {"container name or id", 1, {CK_NONE}},
        {"<event id, default oldest pending>", 2, {CK_EVENT, CK_NONE}},
        {"province id/state region tag", 1, {CK_STATE_REGION}},
        {"state region tag", 1, {CK_STATE_REGION}},
        {"<state region tag>", 1, {CK_STATE_REGION}},
        {"strategic region key", 1, {CK_STRATEGIC_REGION}},
        {"law key, country tag (optional)", 2, {CK_LAW, CK_COUNTRY}},
        {"<building key> <pm key> <state region key>", 3, {CK_BUILDING, CK_PRODUCTION_METHOD, CK_STATE_REGION}},
        {"<country ID> <company type key> <state ID> <building type key>", 4, {CK_N_COUNTRY, CK_COMPANY_TYPE, CK_N_STATE, CK_BUILDING}},
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
    CHECK(c.enabled && c.game_console && c.v3x_console && !c.override_engine && c.scan_frames == 120 && c.names, "ini defaults (names=1)");
    cp_parse_ini("# comment\n[console_plus]\nv3x_console = 0\noverride_engine=1\nscan_frames=3\nbogus=1\nnames=0\n"
                 "[commands]\nown = state_region country\nTag=country\nresearch = tech\nfoo = nosuchkind\nmoney = none\n", &c);
    CHECK(!c.v3x_console && c.override_engine && c.scan_frames == 10 && !c.names, "ini values (scan_frames clamped to 10, names=0)");
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

/* ------------------------------------------------------------------ Part A3: display-name logic */
static int t_read(const void *a, void *o, size_t n, void *u) { (void)u; return safe_read(a, o, n, NULL); }
static int t_res(const char *k, size_t kn, char *out, size_t cap, void *u)
{
    (void)u;
    if (kn == 3 && !memcmp(k, "PRU", 3)) { snprintf(out, cap, "Prussia"); return 7; }
    if (kn == 4 && !memcmp(k, "LOOP", 4)) { snprintf(out, cap, "$LOOP$x"); return 7; }
    return -1;
}

static void part_a3(void)
{
    printf("== part A3: display names (folding, cleaning, ranking, discovery, id lookup decoding)\n");
    char f[256];
    cp_fold("S\xC3\xA3o Paulo", 10, f, sizeof f, 0);
    CHECK(!strcmp(f, "sao paulo"), "fold 'São Paulo' -> '%s'", f);
    cp_fold("S\xC3\xA3o_Paulo", 10, f, sizeof f, 1);
    CHECK(!strcmp(f, "saopaulo"), "fold compact 'São_Paulo' -> '%s'", f);
    cp_fold("Pr\xC3\xBAs", 5, f, sizeof f, 1);
    CHECK(!strcmp(f, "prus"), "fold 'Prús' -> '%s'", f);
    const char *all = "\xC3\x81\xC3\xA7\xC3\xA3\xC3\xA9\xC3\xB1\xC3\xB6\xC3\x98\xC3\x9F\xC3\x86\xC5\x81\xC5\x91\xC5\xA0\xC5\xBE\xC8\x98\xC8\x9B\xC4\xB0";  /* Áçãéñ öØßÆ Łő Šž Șț İ */
    cp_fold(all, strlen(all), f, sizeof f, 1);
    CHECK(!strcmp(f, "acaenoossaeloszsti"), "fold Latin-1/Ext-A/Romanian: '%s'", f);
    cp_fold("\xD0\x9C\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0", 12, f, sizeof f, 1);   /* Москва: kept as is */
    CHECK(!strcmp(f, "\xD0\x9C\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0"), "non-Latin text kept byte for byte");
    cp_fold("  Rio-Grande  do\tSul! ", 22, f, sizeof f, 0);
    CHECK(!strcmp(f, "rio grande do sul"), "separators collapse: '%s'", f);

    char c[256];
    cp_loc_clean("#bold Grand Duchy#! of $PRU$", 28, c, sizeof c, t_res, NULL);
    CHECK(!strcmp(c, "Grand Duchy of Prussia"), "loc clean: style markers, $KEY$ resolved: '%s'", c);
    cp_loc_clean("[ROOT.GetCountry.GetName] @gold! Gold\\nMine", 43, c, sizeof c, t_res, NULL);
    CHECK(!strcmp(c, "Gold Mine"), "loc clean: [script] and @icon! dropped, \\n -> blank: '%s'", c);
    cp_loc_clean("$LOOP$", 6, c, sizeof c, t_res, NULL);
    CHECK(!strcmp(c, "xx"), "loc clean: self reference stops after 2 levels: '%s'", c);
    cp_loc_clean("$NOPE$", 6, c, sizeof c, t_res, NULL);
    CHECK(!c[0], "loc clean: unknown reference -> empty");

    static CpNames x; memset(&x, 0, sizeof x);
    cp_names_add(&x, "PRU", "Prussia", "");
    cp_names_add(&x, "BRZ", "Brasil", "");
    cp_names_add(&x, "STATE_SAO_PAULO", "S\xC3\xA3o Paulo", "BRZ");
    cp_names_add(&x, "STATE_EAST_PRUSSIA", "East Prussia", "PRU");
    cp_names_add(&x, "STATE_RIO_GRANDE_DO_SUL", "Rio Grande do Sul", "BRZ");
    cp_names_add(&x, "STATE_RIO", "Rio de Janeiro", "BRZ");
    cp_names_add(&x, "SPA", "Spain", "");
    CHECK(x.n == 7, "index: 7 names");
    CpHit h[8];
    int t = cp_names_query(&x, "Pr\xC3\xBAs", 0, h, 8);
    CHECK(t == 2 && !strcmp(x.e[h[0].idx].id, "PRU") && h[0].score == 1 && !strcmp(x.e[h[1].idx].id, "STATE_EAST_PRUSSIA") && h[1].score == 2,
          "'Prús': PRU (prefix) before East Prussia (word) -> %d", t);
    t = cp_names_query(&x, "brasil", 0, h, 8);
    CHECK(t == 1 && !strcmp(x.e[h[0].idx].id, "BRZ") && h[0].score == 0, "'brasil' -> BRZ (exact)");
    t = cp_names_query(&x, "S\xC3\xA3o Pa", 0, h, 8);
    CHECK(t == 1 && !strcmp(x.e[h[0].idx].id, "STATE_SAO_PAULO"), "'São Pa' -> STATE_SAO_PAULO");
    t = cp_names_query(&x, "saopa", 0, h, 8);
    CHECK(t == 1 && !strcmp(x.e[h[0].idx].id, "STATE_SAO_PAULO"), "'saopa' (no blank) -> STATE_SAO_PAULO");
    t = cp_names_query(&x, "rio", 0, h, 8);
    CHECK(t == 2 && !strcmp(x.e[h[0].idx].id, "STATE_RIO"), "'rio': 2 (shorter name first: Rio de Janeiro) -> %s", t ? x.e[h[0].idx].id : "-");
    t = cp_names_query(&x, "grandedo", 0, h, 8);
    CHECK(t == 1 && h[0].score == 2, "'grandedo': word start, compact");
    t = cp_names_query(&x, "ain", 0, h, 8);
    CHECK(t == 1 && !strcmp(x.e[h[0].idx].id, "SPA") && h[0].score == 3, "'ain': substring (Spain)");
    t = cp_names_query(&x, "p", 0, h, 8);
    CHECK(t == 3, "1 letter: prefixes / word starts only, no substrings (%d)", t);
    t = cp_names_query(&x, "rio@brz", 0, h, 8);
    CHECK(t == 2, "'rio@brz': context filter");
    t = cp_names_query(&x, "@pru", 0, h, 8);
    CHECK(t == 1 && !strcmp(x.e[h[0].idx].id, "STATE_EAST_PRUSSIA"), "'@pru': everything of context PRU");
    t = cp_names_query(&x, "state_rio", 0, h, 8);
    CHECK(t == 0, "ids are not names without CP_Q_IDS");
    t = cp_names_query(&x, "state_rio", CP_Q_IDS, h, 8);
    CHECK(t == 2 && !strcmp(x.e[h[0].idx].id, "STATE_RIO") && h[0].score == 0, "CP_Q_IDS: 'state_rio' -> STATE_RIO exact, + RIO_GRANDE prefix");
    t = cp_names_query(&x, "s", 0, h, 1);
    CHECK(t >= 1 && h[0].idx >= 0, "top-1 list with more matches (%d)", t);
    t = cp_names_query(&x, "", 0, h, 8);
    CHECK(t == 0, "empty query -> nothing");
    cp_names_free(&x);
    CHECK(x.n == 0 && !x.ar.head, "index freed");

    /* field discovery on fake objects */
    static uint64_t objs[20][64], targets[5][4];
    uintptr_t set[5]; void *op[20];
    for (int i = 0; i < 5; i++) set[i] = (uintptr_t)targets[i];
    cp_ptrset_sort(set, 5);
    memset(objs, 0, sizeof objs);
    for (int i = 0; i < 20; i++) { op[i] = objs[i]; objs[i][3] = (uint64_t)(uintptr_t)targets[i % 5]; objs[i][9] = 12345; }
    for (int i = 0; i < 6; i++) objs[i][1] = (uint64_t)(uintptr_t)targets[0];       /* a rarer field */
    int pct = 0;
    int off = cp_discover_field(op, 20, 0x200, set, 5, t_read, NULL, 60, &pct);
    CHECK(off == 0x18 && pct == 100, "discover: field +0x18 points into the set in 100%% (got %d, %d%%)", off, pct);
    for (int i = 0; i < 12; i++) objs[i][3] = 0;
    off = cp_discover_field(op, 20, 0x200, set, 5, t_read, NULL, 60, &pct);
    CHECK(off == -1 && pct == 40, "discover: best field below 60%% -> -1 (%d%%)", pct);
    CHECK(cp_ptrset_find(set, 5, set[2]) == 2 && cp_ptrset_find(set, 5, 1) == -1, "pointer set lookup");

    /* id lookup decoding: synthetic copies of the 1.15.1 shapes */
    static uint8_t code[128];
    static const uint8_t st[] = {0x48,0x8B,0x05,0x10,0x00,0x00,0x00, 0x48,0x8B,0x88,0x08,0x06,0x00,0x00, 0x48,0x8B,0x81,0x20,0x01,0x00,0x00,
                                 0x3B,0x98,0x3C,0x1C,0x00,0x00, 0x73,0x17, 0x48,0x8B,0xCB, 0x48,0x03,0xC9, 0x48,0x8B,0x80,0x30,0x1C,0x00,0x00,
                                 0x48,0x8B,0x4C,0xC8,0x08};
    memset(code, 0xCC, sizeof code); memcpy(code, st, sizeof st);
    const uint8_t *root; uint32_t ao = 0, co = 0;
    int d = cp_decode_gs(code, sizeof code, &root, &ao, &co);
    CHECK(d && root == code + 7 + 0x10 && ao == 0x1c30 && co == 0x1c3c, "decode state lookup: array +0x%x count +0x%x", ao, co);
    static const uint8_t cty[] = {0x48,0x8B,0x05,0x00,0x01,0x00,0x00, 0x48,0x8B,0x88,0x08,0x06,0x00,0x00, 0x48,0x8B,0x81,0x20,0x01,0x00,0x00,
                                  0x3B,0x58,0x3C, 0x73,0x14, 0x48,0x8B,0xCB, 0x48,0x03,0xC9, 0x48,0x8B,0x40,0x30, 0x48,0x8B,0x4C,0xC8,0x08};
    memset(code, 0xCC, sizeof code); memcpy(code, cty, sizeof cty);
    d = cp_decode_gs(code, sizeof code, &root, &ao, &co);
    CHECK(d && ao == 0x30 && co == 0x3c, "decode country lookup (disp8): array +0x%x count +0x%x", ao, co);
    code[24] = 0x90; code[25] = 0x90; code[22] = 0x58; code[23] = 0x38;  /* count 0x38: not ...C */
    CHECK(!cp_decode_gs(code, sizeof code, &root, &ao, &co), "decode rejects a count offset that is not array+0xC");
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
    g_cfg.names = 0;          /* part B: ids only (as before display names); part B2 turns them on */
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

/* ------------------------------------------------------------------ Part B2: display names on fake engine data */
typedef struct { const char *p; int32_t len; uint8_t flag; uint8_t pad[3]; } FSv;                         /* {ptr,len,flag} */
typedef struct { uint32_t pad0; uint8_t dist; uint8_t pad1[3]; uint32_t hash; uint32_t pad2; FSv key, val; } FSlot;   /* 0x30 */
static FSlot T_slots[64 + 4];
static struct { CStr name; uint8_t pad[0x38 - sizeof(CStr)]; struct { void *x; FSlot *buckets; int32_t a; uint32_t mask; uint8_t maxp; uint8_t pad[7]; } map; } T_lang;
static uint8_t T_locmgr[0x100];
static void *fake_loc_get(void) { return T_locmgr; }
static int T_nloc;
static void loc_put(const char *k, const char *v)
{
    int i = (T_nloc * 7) % 64; T_nloc++;
    while (T_slots[i].dist) i = (i + 1) % 64;
    T_slots[i].dist = 1;
    T_slots[i].key.p = k; T_slots[i].key.len = (int32_t)strlen(k);
    T_slots[i].val.p = v; T_slots[i].val.len = (int32_t)strlen(v);
}
/* name fixture databases */
static FObj N_c[3], N_s[3];
static FObj *N_cp[3] = {&N_c[0], &N_c[1], &N_c[2]}, *N_sp[3] = {&N_s[0], &N_s[1], &N_s[2]};
static struct { void *vt; uint8_t pad[0x78]; FObj **arr; int32_t cap, count; } N_dbc, N_dbs;
static struct { void *db; uint8_t has; } N_slotc, N_slots, N_slote;
static void *N_vt_c[2], *N_vt_s[2], *N_vt_e[2];
static void **n_get_c(void) { return (void **)&N_slotc; }
static void **n_get_s(void) { return (void **)&N_slots; }
static void **n_get_e(void) { return (void **)&N_slote; }
typedef struct { uint8_t pad[0x10]; CStr name; } FEv;
static FEv N_ev[2]; static FEv *N_evp[2] = {&N_ev[0], &N_ev[1]};
static struct { void *vt; FEv **arr; int32_t cap, count; } N_dbe;      /* arr +8, count +0x14 */
/* live game state */
static uint8_t T_G[0x2000];
static struct { uint8_t pad[0x608]; void *g1; } T_root;
static struct { uint8_t pad[0x120]; void *g; } T_g1;
static void *T_rootvar;
static uint64_t T_cobj[3][128], T_sobj[4][128], T_chobj[2][128];
static uint64_t T_cslots[8], T_sslots[8], T_chslots[4];
static CStr T_chname[2][2];
/* console output */
static char T_con[8192]; static size_t T_conlen;
static void fake_con_print(void *con, const CStr *t, uint8_t scroll)
{
    (void)con; (void)scroll;
    char b[700]; cstr_copy(t, b, sizeof b);
    size_t n = strlen(b);
    if (T_conlen + n + 2 < sizeof T_con) { memcpy(T_con + T_conlen, b, n); T_conlen += n; T_con[T_conlen++] = '\n'; T_con[T_conlen] = 0; }
}
/* fake engine allocator (stands in for mimalloc): its own heap + a list of live blocks. The engine
 * frees a CString it owns with it; freeing anything it did not allocate is a provenance error
 * (in the game: mi_free on plugin memory = AV, crash6). */
static HANDLE T_eheap; static void *T_eblk[256]; static int T_neblk, T_bad_free, T_good_free;
static void *eng_alloc(size_t n)
{
    if (!T_eheap) T_eheap = HeapCreate(0, 0, 0);
    void *p = HeapAlloc(T_eheap, 0, n);
    if (T_neblk < 256) T_eblk[T_neblk++] = p;
    return p;
}
static void eng_free(void *p)
{
    for (int i = 0; i < T_neblk; i++)
        if (T_eblk[i] == p) { T_eblk[i] = T_eblk[--T_neblk]; HeapFree(T_eheap, 0, p); T_good_free++; return; }
    T_bad_free++;                                 /* not ours: the real engine would crash here */
}
/* engine CString copy ctor 0x678580 (heap via the engine allocator) and dtor 0x67b890 */
static void eng_cstr(CStr *s, const char *v)
{
    size_t n = strlen(v);
    memset(s, 0, sizeof *s);
    if (n < 16) { memcpy(s->u.buf, v, n + 1); s->size = n; s->cap = 15; }
    else { s->u.ptr = (char *)eng_alloc(n + 1); memcpy(s->u.ptr, v, n + 1); s->size = n; s->cap = n; }
}
static void eng_cstr_dtor(CStr *s) { if (s->cap > 15) eng_free(s->u.ptr); memset(s, 0, sizeof *s); }

/* the real matcher takes the input BY VALUE and destroys it before returning (0x3dc296f) */
static const CStr *T_seen_input; static char T_seen_text[256];
static uint64_t fake_matcher(uint64_t m, uint64_t in, uint64_t r, uint64_t r9)
{
    (void)m; (void)r; (void)r9;
    T_seen_input = (const CStr *)(uintptr_t)in;
    cstr_copy(T_seen_input, T_seen_text, sizeof T_seen_text);
    eng_cstr_dtor((CStr *)(uintptr_t)in);
    return 0x1234;
}

/* the real CConsole::ExecuteLine takes const CString& and only copies it (history, tokens); a line
 * that is not one of the engine allocator's blocks is one console_plus built */
static char T_exec[8][128]; static int T_nexec, T_pass, T_exec_con_ok = 1, T_exec_r8r9_ok = 1;
static uint64_t fake_execline(uint64_t con, uint64_t line, uint64_t r8, uint64_t r9)
{
    const CStr *l = (const CStr *)(uintptr_t)line;
    if (con != 0x77) T_exec_con_ok = 0;
    if (r8 != 5 || r9 != 6) T_exec_r8r9_ok = 0;
    if (l->cap > 15) {
        for (int i = 0; i < T_neblk; i++) if (T_eblk[i] == l->u.ptr) { T_pass++; break; }
    } else T_pass++;
    if (T_nexec < 8) cstr_copy(l, T_exec[T_nexec], sizeof T_exec[0]);
    T_nexec++;
    return 0x101;                                /* al = 1 (ok), upper bits not part of the bool */
}

static void names_full_reset(void)
{
    for (int k = 0; k < CK_COUNT; k++) names_reset(k);
    loc_free(); L.gen = 0; L.lang_name[0] = 0;
    memset(g_gs, 0, sizeof g_gs);
    fields_reset();
    memset(&g_forced, 0, sizeof g_forced);
    g_name_tabs = g_name_hits = 0;
}

/* runs the matcher pre-step on an engine-allocated copy of `line` (as Autocomplete passes it);
 * returns the text the engine's matcher would see ("" = unchanged). Checks that the engine's string
 * object keeps its buffer (pointer, capacity) and is freed only through the engine allocator. */
static int T_pre_kept = 1;
static const char *pre(const char *line)
{
    static char seen[256];
    CStr in; eng_cstr(&in, line);
    CStr was = in;
    int x = names_pre_match(&in);
    if (in.cap != was.cap || (was.cap > 15 && in.u.ptr != was.u.ptr) || in.size > was.size) T_pre_kept = 0;
    if (!x && memcmp(&in, &was, sizeof in)) T_pre_kept = 0;
    if (x) cstr_copy(&in, seen, sizeof seen);
    eng_cstr_dtor(&in);
    return x ? seen : "";
}
static int forced_is(int n, const char *a, const char *b)
{
    if (!g_forced.active || g_forced.n != n) return 0;
    if (a && strcmp(g_forced.id[0], a)) return 0;
    if (b && strcmp(g_forced.id[1], b)) return 0;
    return 1;
}

static void part_b2(void)
{
    printf("== part B2: display names (fake localization, databases, live game state)\n");
    setup_engine();
    names_full_reset();
    g_cfg.names = 1;
    F_mgr_ptr = &F_mgr;
    make_entry(4, "invalidate_state", "Invalidate the modifier on the state with the specified ID", "state ID", NULL, NULL);
    make_entry(6, "invalidate_character", "Invalidate the modifier on the character with the specified ID", "character ID", NULL, NULL);
    make_entry(7, "invalidate_country", "Invalidate the modifier on the country with the specified ID", "country ID", NULL, NULL);
    /* localization */
    memset(T_slots, 0, sizeof T_slots); T_nloc = 0;
    set_cstr(&T_lang.name, "l_english");
    T_lang.map.buckets = T_slots; T_lang.map.mask = 63; T_lang.map.maxp = 0;
    *(void **)(T_locmgr + 0x90) = &T_lang;
    loc_put("PRU", "Prussia"); loc_put("BRZ", "Brasil"); loc_put("GBR", "$GBR_NAME$"); loc_put("GBR_NAME", "Great Britain");
    loc_put("STATE_SAO_PAULO", "S\xC3\xA3o Paulo"); loc_put("STATE_BAHIA", "Bahia"); loc_put("STATE_EAST_PRUSSIA", "East Prussia");
    loc_put("rev.1.t", "The #bold Revolution#!");
    loc_put("name_otto", "Otto"); loc_put("name_von_bismarck_x", "von Bismarck");
    loc_put("name_helmuth", "Helmuth"); loc_put("name_moltke_the_elder", "von Moltke");
    A.loc_get = fake_loc_get;
    A.con_print = fake_con_print;
    g_console = (void *)1;
    /* databases */
    const char *ct[3] = {"PRU", "BRZ", "GBR"}, *sr[3] = {"STATE_SAO_PAULO", "STATE_BAHIA", "STATE_EAST_PRUSSIA"};
    for (int i = 0; i < 3; i++) { N_c[i].vt = (void *)1; set_cstr(&N_c[i].key, ct[i]); N_s[i].vt = (void *)1; set_cstr(&N_s[i].key, sr[i]); }
    N_dbc.vt = N_vt_c; N_dbc.arr = N_cp; N_dbc.count = 3; N_slotc.db = &N_dbc; N_slotc.has = 1;
    N_dbs.vt = N_vt_s; N_dbs.arr = N_sp; N_dbs.count = 3; N_slots.db = &N_dbs; N_slots.has = 1;
    set_cstr(&N_ev[0].name, "rev.1"); set_cstr(&N_ev[1].name, "rev.2");
    N_dbe.vt = N_vt_e; N_dbe.arr = N_evp; N_dbe.count = 2; N_slote.db = &N_dbe; N_slote.has = 1;
    A.dbget[CK_COUNTRY] = n_get_c; A.dbvt[CK_COUNTRY] = N_vt_c;
    A.dbget[CK_STATE_REGION] = n_get_s; A.dbvt[CK_STATE_REGION] = N_vt_s;
    /* live objects: countries 0 PRU, (1 empty), 2 BRZ, 3 GBR; states 0 Sao Paulo/BRZ, 1 Bahia/BRZ,
     * 2 East Prussia/PRU, 3 Sao Paulo/GBR (a split state region); characters 0 Otto von Bismarck, 1 Helmuth von Moltke (PRU) */
    T_rootvar = &T_root; T_root.g1 = &T_g1; T_g1.g = T_G;
    memset(T_cobj, 0, sizeof T_cobj); memset(T_sobj, 0, sizeof T_sobj); memset(T_chobj, 0, sizeof T_chobj);
    for (int i = 0; i < 3; i++) T_cobj[i][9] = (uint64_t)(uintptr_t)&N_c[i];          /* definition at +0x48 */
    T_cslots[1] = (uint64_t)(uintptr_t)T_cobj[0]; T_cslots[3] = 0; T_cslots[5] = (uint64_t)(uintptr_t)T_cobj[1]; T_cslots[7] = (uint64_t)(uintptr_t)T_cobj[2];
    *(uint64_t **)(T_G + 0x30) = T_cslots; *(int32_t *)(T_G + 0x3c) = 4;
    int sreg[4] = {0, 1, 2, 0}, sown[4] = {1, 1, 0, 2};
    for (int i = 0; i < 4; i++) {
        T_sobj[i][12] = (uint64_t)(uintptr_t)&N_s[sreg[i]];                           /* region at +0x60 */
        T_sobj[i][20] = (uint64_t)(uintptr_t)T_cobj[sown[i]];                         /* owner at +0xa0 */
        T_sslots[2 * i + 1] = (uint64_t)(uintptr_t)T_sobj[i];
    }
    *(uint64_t **)(T_G + 0x1c30) = T_sslots; *(int32_t *)(T_G + 0x1c3c) = 4;
    const char *chn[2][2] = {{"name_otto", "name_von_bismarck_x"}, {"name_helmuth", "name_moltke_the_elder"}};
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) { set_cstr(&T_chname[i][j], chn[i][j]); memcpy((uint8_t *)T_chobj[i] + 0x20 + 0x20 * j, &T_chname[i][j], sizeof(CStr)); }
        T_chobj[i][16] = (uint64_t)(uintptr_t)T_cobj[0];                               /* country at +0x80 */
        T_chslots[2 * i + 1] = (uint64_t)(uintptr_t)T_chobj[i];
    }
    *(uint64_t **)(T_G + 0x1300) = T_chslots; *(int32_t *)(T_G + 0x130c) = 2;
    int kk[3] = {CK_N_COUNTRY, CK_N_STATE, CK_N_CHARACTER}; uint32_t ao[3] = {0x30, 0x1c30, 0x1300};
    for (int i = 0; i < 3; i++) { g_gs[kk[i]].ok = 1; g_gs[kk[i]].root = (const uint8_t *)&T_rootvar; g_gs[kk[i]].arr = ao[i]; g_gs[kk[i]].cnt = ao[i] + 0xc; }

    scan_registry();
    A.dbget[CK_EVENT] = n_get_e; A.dbvt[CK_EVENT] = N_vt_e;     /* (validate checked the fake engine completer's shape) */
    CHECK(g_validated && entry_cb(1) == cp_complete, "names=1: the engine `event` completer is replaced by ours (it delegates)");
    CHECK(entry_cb(4) == cp_complete && entry_cb(6) == cp_complete && entry_cb(7) == cp_complete, "numeric-id commands get a completer");
    CHECK(entry_cb(5) == fake_mapmode_cb, "mapmode still the engine's");
    int n = run_cb(4, "");
    CHECK(n == 0, "invalidate_state <Tab> without a name: no candidates (numbers come only from names)");
    n = run_cb(1, "");
    CHECK(n == 1 && out_has("debug.1"), "event <Tab>: still the engine's own list (delegated)");

    T_conlen = 0; T_con[0] = 0;
    const char *s = pre("tag Prus");
    CHECK(!strcmp(s, "tag ") && forced_is(1, "PRU", NULL) && !T_con[0], "'tag Prus' -> matcher sees 'tag ', candidate PRU, nothing listed");
    CHECK(L.n == 12 && !strcmp(L.lang_name, "l_english"), "localization index: %d keys, language %s", L.n, L.lang_name);
    n = run_cb(2, "");
    CHECK(n == 1 && out_has("PRU") && !g_forced.active, "completer returns exactly PRU, forced list consumed");
    n = run_cb(2, "");
    CHECK(n == 3, "next completer call: the normal tag list again");
    s = pre("tag Pr\xC3\xBAs");
    CHECK(forced_is(1, "PRU", NULL), "'tag Prús' (accent) -> PRU");
    s = pre("tag great");
    CHECK(forced_is(1, "GBR", NULL), "'tag great' -> GBR ($GBR_NAME$ reference resolved)");
    s = pre("tag BRASIL");
    CHECK(forced_is(1, "BRZ", NULL), "'tag BRASIL' -> BRZ");
    s = pre("tag r");
    CHECK(!s[0] && !g_forced.active, "1-letter token: engine as before");
    s = pre("tag xyzzy");
    CHECK(!s[0] && !g_forced.active, "no name match: engine as before");
    s = pre("tag PRU");
    CHECK(forced_is(1, "PRU", NULL), "typing the id: still that id");
    s = pre("own sao_pa");
    CHECK(!strcmp(s, "own ") && forced_is(1, "STATE_SAO_PAULO", NULL), "'own sao_pa' -> STATE_SAO_PAULO");
    s = pre("own STATE_SAO_PAULO bra");
    CHECK(!strcmp(s, "own STATE_SAO_PAULO ") && forced_is(1, "BRZ", NULL), "second argument: 'bra' -> BRZ (country)");
    T_conlen = 0; T_con[0] = 0;
    s = pre("own S\xC3\xA3o Pa");
    CHECK(!s[0] && !g_forced.active && strstr(T_con, "without blanks") && strstr(T_con, "STATE_SAO_PAULO  (S\xC3\xA3o Paulo)"),
          "'own São Pa' (blank inside the name): listed with a hint, line unchanged");
    s = pre("event revol");
    CHECK(forced_is(1, "rev.1", NULL), "'event revol' -> rev.1 (event title 'The Revolution')");
    F_event_calls = 0;
    n = run_cb(1, "");
    CHECK(n == 1 && out_has("rev.1") && F_event_calls == 0, "event completer returns only rev.1");
    T_conlen = 0; T_con[0] = 0;
    s = pre("event rev");
    CHECK(forced_is(2, "rev.1", "rev.2") && strstr(T_con, "rev.1  (The Revolution)"), "'event rev': ids containing it + titles, listed");
    g_forced.active = 0;

    s = pre("invalidate_state bahia");
    CHECK(!strcmp(s, "invalidate_state ") && forced_is(1, "1", NULL), "'invalidate_state bahia' -> state id 1");
    n = run_cb(4, "");
    CHECK(n == 1 && out_has("1"), "completer returns '1'");
    T_conlen = 0; T_con[0] = 0;
    s = pre("invalidate_state sao");
    CHECK(forced_is(2, "0", "3") && strstr(T_con, "0  (S\xC3\xA3o Paulo, BRZ)") && strstr(T_con, "3  (S\xC3\xA3o Paulo, GBR)"),
          "'invalidate_state sao': split state region -> 2 ids listed with owners");
    s = pre("invalidate_state sao@gbr");
    CHECK(forced_is(1, "3", NULL), "'sao@gbr' -> 3");
    s = pre("invalidate_state 12");
    CHECK(!s[0] && !g_forced.active, "digits: engine as before");
    s = pre("invalidate_country brasil");
    CHECK(forced_is(1, "2", NULL), "'invalidate_country brasil' -> country id 2");
    s = pre("invalidate_character bismarck");
    CHECK(forced_is(1, "0", NULL), "'invalidate_character bismarck' -> character 0 (name keys discovered at +0x%x/+0x%x)", F.ch_name[0], F.ch_name[1]);
    CHECK(F.def == 0x48 && F.st_region == 0x60 && F.st_owner == 0xa0 && F.ch_owner == 0x80 && F.ch_name[0] == 0x20 && F.ch_name[1] == 0x40,
          "discovered fields: def +0x%x, region +0x%x, owner +0x%x, char country +0x%x", F.def, F.st_region, F.st_owner, F.ch_owner);
    s = pre("invalidate_character otto@pru");
    CHECK(forced_is(1, "0", NULL), "'otto@pru' -> 0");

    /* find */
    T_conlen = 0; T_con[0] = 0;
    s = pre("find pau");
    CHECK(!s[0] && strstr(T_con, "state_region STATE_SAO_PAULO  S\xC3\xA3o Paulo") && strstr(T_con, "state_id 0  S\xC3\xA3o Paulo (BRZ)"),
          "in-game 'find pau'+Tab lists state region and states");
    g_printed_len = 0; g_printed[0] = 0;
    cmd_find_v3x("brasil", NULL);
    CHECK(strstr(g_printed, "country BRZ  Brasil") && strstr(g_printed, "country_id 2  Brasil (BRZ)"), "V3X `find brasil` (cached indexes)");
    g_printed_len = 0; g_printed[0] = 0;
    cmd_find_v3x("", NULL);
    CHECK(strstr(g_printed, "find <text>") != NULL, "V3X `find` without text: usage");

    /* hook wrapper, other threads, names off, load transition */
    CHECK(T_pre_kept, "matcher pre-step: the engine's string keeps its buffer/capacity (only size + NUL change)");
    o_matcher = fake_matcher;
    int bad0 = T_bad_free, good0 = T_good_free;
    CStr in; eng_cstr(&in, "tag Prus");                 /* SSO input */
    uint64_t r = hk_matcher(1, (uint64_t)(uintptr_t)&in, 2, 3);
    CHECK(r == 0x1234 && T_seen_input == &in && !strcmp(T_seen_text, "tag ") && !g_forced.active, "hk_matcher: engine's own input shortened in place, result returned, forced cleared");
    eng_cstr(&in, "invalidate_state bahia");           /* heap input (> 15 chars), cut to 17 chars: still heap */
    r = hk_matcher(1, (uint64_t)(uintptr_t)&in, 2, 3);
    CHECK(T_seen_input == &in && !strcmp(T_seen_text, "invalidate_state "), "hk_matcher: heap input shortened in place ('%s')", T_seen_text);
    eng_cstr(&in, "own STATE_SAO_PAULO bra");
    hk_matcher(1, (uint64_t)(uintptr_t)&in, 2, 3);
    CHECK(T_seen_input == &in && !strcmp(T_seen_text, "own STATE_SAO_PAULO "), "hk_matcher: second argument, heap input shortened in place");
    eng_cstr(&in, "tag xyzzy");
    hk_matcher(1, (uint64_t)(uintptr_t)&in, 2, 3);
    CHECK(T_seen_input == &in && !strcmp(T_seen_text, "tag xyzzy"), "hk_matcher: no name match -> the engine's own input, unchanged");
    CHECK(T_bad_free == bad0 && T_good_free == good0 + 2 && T_neblk == 0,
          "pointer provenance: engine freed only its own blocks (%d foreign frees, %d own, %d leaked)", T_bad_free - bad0, T_good_free - good0, T_neblk);
    {   /* the provenance check itself: a string whose buffer is plugin memory is caught */
        static char plugin_buf[64] = "invalidate_state ";
        CStr x; memset(&x, 0, sizeof x); x.u.ptr = plugin_buf; x.size = 17; x.cap = 17;
        int b = T_bad_free; eng_cstr_dtor(&x);
        CHECK(T_bad_free == b + 1, "provenance check catches a plugin-owned buffer handed to the engine (the crash6 pattern)");
    }
    CHECK(T_bad_free == bad0 + 1, "no foreign frees anywhere in part B2 (pre-step + hook)");
    g_main_tid = GetCurrentThreadId() + 1;
    s = pre("tag Prus");
    CHECK(!s[0], "off the main thread: nothing");
    g_main_tid = GetCurrentThreadId();
    g_cfg.names = 0;
    s = pre("tag Prus");
    CHECK(!s[0], "names=0: nothing");
    g_cfg.names = 1;
    int before = g_nm[CK_COUNTRY].n;
    loc_put("SPA", "Spain");                     /* engine map changed: same buckets -> no rebuild */
    on_load_transition("load_transition", NULL, NULL);
    s = pre("tag Prus");
    CHECK(forced_is(1, "PRU", NULL) && g_nm[CK_COUNTRY].n == before, "after a load transition the indexes are rebuilt");
    g_printed_len = 0; g_printed[0] = 0;
    cmd_complete("", NULL);
    CHECK(strstr(g_printed, "display names: on") && strstr(g_printed, "language l_english"), "`complete` shows the display-name status");
    /* country wildcards on Enter: ExecuteLine hook against the fake game state */
    {
        static int32_t T_keys[4][2], T_vals[3][2];
        uint32_t idPRU = 0 | 0x01000000u, idBRZ = 2 | 0x02000000u, idGBR = 3 | 0x01000000u;
        T_cobj[0][3] = idPRU; T_cobj[1][3] = idBRZ; T_cobj[2][3] = idGBR;              /* full id at +0x18 */
        T_keys[0][0] = (int32_t)idPRU; T_keys[0][1] = 0; T_keys[1][0] = 7; T_keys[1][1] = 0;
        T_keys[2][0] = (int32_t)idBRZ; T_keys[2][1] = 1; T_keys[3][0] = (int32_t)idGBR; T_keys[3][1] = 2;
        T_vals[0][0] = -1; T_vals[0][1] = 1;                     /* PRU: independent */
        T_vals[1][0] = (int32_t)idGBR; T_vals[1][1] = 1;         /* BRZ: subject of GBR */
        T_vals[2][0] = (int32_t)idPRU; T_vals[2][1] = 1;         /* GBR: subject of PRU */
        *(int32_t **)(T_G + 0x1f58) = &T_keys[0][0]; *(int32_t *)(T_G + 0x1f64) = 4; *(int32_t **)(T_G + 0x1f70) = &T_vals[0][0];
        *(uint32_t *)((uint8_t *)&T_g1 + 0xf8) = idPRU;          /* player = PRU */
        memset((void *)&W, 0, sizeof W);
        W.ok = 1; W.root = (const uint8_t *)&T_rootvar; W.cnt = 0x1f64; W.keys = 0x1f58; W.vals = 0x1f70; W.ply_ok = 1; W.ply_off = 0xf8;
        A.execline = (uint8_t *)1;
        o_execline = fake_execline;
        /* the engine's add_liberty_desire and disable_ai as console_plus classifies them */
        const char *ld[2] = {"Country tag", "Liberty Desire Value optional (default=10)"}, *da[1] = {"all/tag"};
        CpCmd *lc = &g_cmds[g_ncmd++]; memset(lc, 0, sizeof *lc); snprintf(lc->name, sizeof lc->name, "add_liberty_desire");
        lc->nargs = cp_classify_args(ld, 2, lc->args, CP_MAX_ARGS, &g_pool);
        CpCmd *dc = &g_cmds[g_ncmd++]; memset(dc, 0, sizeof *dc); snprintf(dc->name, sizeof dc->name, "disable_ai");
        dc->nargs = cp_classify_args(da, 1, dc->args, CP_MAX_ARGS, &g_pool);
        CHECK(lc->nargs >= 1 && lc->args[0].kind == CK_COUNTRY, "add_liberty_desire: first argument is a country");
        CHECK(dc->nargs >= 1 && dc->args[0].kind == CK_COUNTRY && dc->args[0].nlit >= 1, "disable_ai: country + literal word(s)");

        int bad0 = T_bad_free;
        T_conlen = 0; T_con[0] = 0; T_nexec = 0;
        CStr ln; eng_cstr(&ln, "add_liberty_desire * -100");
        uint64_t r = hk_execline(0x77, (uint64_t)(uintptr_t)&ln, 5, 6);
        CHECK(r == 1 && T_nexec == 2 && !strcmp(T_exec[0], "add_liberty_desire BRZ -100") && !strcmp(T_exec[1], "add_liberty_desire GBR -100"),
              "'add_liberty_desire * -100' -> runs for BRZ, GBR (%d lines: %s | %s)", T_nexec, T_exec[0], T_nexec > 1 ? T_exec[1] : "");
        CHECK(T_exec_con_ok && T_exec_r8r9_ok, "each run: same console object and extra registers forwarded");
        CHECK(strstr(T_con, "ran add_liberty_desire for 2 subjects (2 ok)") != NULL, "summary line in the game console: %s", T_con);
        CHECK(!strcmp(cstr_ptr(&ln), "add_liberty_desire * -100") && ln.size == 25, "the engine's own line is untouched");
        eng_cstr_dtor(&ln);
        T_nexec = 0; T_conlen = 0; T_con[0] = 0;
        eng_cstr(&ln, "add_liberty_desire M:* -100");
        hk_execline(0x77, (uint64_t)(uintptr_t)&ln, 5, 6);
        CHECK(T_nexec == 1 && !strcmp(T_exec[0], "add_liberty_desire GBR -100") && strstr(T_con, "for 1 subject of the player (1 ok)"), "'M:*' (player PRU) -> GBR only");
        eng_cstr_dtor(&ln);
        T_nexec = 0;
        eng_cstr(&ln, "add_liberty_desire gbr:* 5");
        hk_execline(0x77, (uint64_t)(uintptr_t)&ln, 5, 6);
        CHECK(T_nexec == 1 && !strcmp(T_exec[0], "add_liberty_desire BRZ 5"), "'gbr:*' -> BRZ");
        eng_cstr_dtor(&ln);
        T_nexec = 0;
        eng_cstr(&ln, "add_liberty_desire ALL");
        r = hk_execline(0x77, (uint64_t)(uintptr_t)&ln, 5, 6);
        CHECK(r == 1 && T_nexec == 3 && !strcmp(T_exec[0], "add_liberty_desire PRU"), "'ALL' -> every live country (%d)", T_nexec);
        eng_cstr_dtor(&ln);
        T_nexec = 0; T_conlen = 0; T_con[0] = 0;
        eng_cstr(&ln, "add_liberty_desire XYZ:* 5");
        r = hk_execline(0x77, (uint64_t)(uintptr_t)&ln, 5, 6);
        CHECK(r == 0 && T_nexec == 0 && strstr(T_con, "XYZ:*: no such country"), "'XYZ:*' -> message, nothing run");
        eng_cstr_dtor(&ln);
        T_nexec = 0; T_conlen = 0; T_con[0] = 0;
        eng_cstr(&ln, "add_liberty_desire BRZ:*");
        r = hk_execline(0x77, (uint64_t)(uintptr_t)&ln, 5, 6);
        CHECK(r == 0 && T_nexec == 0 && strstr(T_con, "no subjects of BRZ"), "'BRZ:*' (no subjects) -> message, nothing run");
        eng_cstr_dtor(&ln);
        T_nexec = 0; T_pass = 0;
        eng_cstr(&ln, "add_liberty_desire GBR -100");
        hk_execline(0x77, (uint64_t)(uintptr_t)&ln, 5, 6);
        CHECK(T_nexec == 1 && T_pass == 1, "no wildcard -> the engine's line passed through unchanged");
        eng_cstr_dtor(&ln);
        T_nexec = 0; T_pass = 0;
        eng_cstr(&ln, "disable_ai all");
        hk_execline(0x77, (uint64_t)(uintptr_t)&ln, 5, 6);
        CHECK(T_nexec == 1 && T_pass == 1, "'disable_ai all': 'all' is the command's own word -> passed through");
        eng_cstr_dtor(&ln);
        T_nexec = 0; T_pass = 0;
        eng_cstr(&ln, "money *");
        hk_execline(0x77, (uint64_t)(uintptr_t)&ln, 5, 6);
        CHECK(T_nexec == 1 && T_pass == 1, "'money *' (not a country argument) -> passed through");
        eng_cstr_dtor(&ln);
        T_nexec = 0; T_pass = 0;
        g_main_tid = GetCurrentThreadId() + 1;
        eng_cstr(&ln, "add_liberty_desire * -100");
        hk_execline(0x77, (uint64_t)(uintptr_t)&ln, 5, 6);
        CHECK(T_nexec == 1 && T_pass == 1, "other thread -> passed through");
        eng_cstr_dtor(&ln);
        g_main_tid = GetCurrentThreadId();
        T_nexec = 0; T_conlen = 0; T_con[0] = 0;
        W.ply_ok = 0;
        eng_cstr(&ln, "add_liberty_desire M:*");
        hk_execline(0x77, (uint64_t)(uintptr_t)&ln, 5, 6);
        CHECK(T_nexec == 0 && strstr(T_con, "player country not known"), "'M:*' without the decoded player offset -> message, nothing run");
        eng_cstr_dtor(&ln);
        W.ply_ok = 1;
        T_cobj[1][3] = 9; T_cobj[2][3] = 8;                     /* ids no longer match their slots */
        T_nexec = 0; T_conlen = 0; T_con[0] = 0;
        eng_cstr(&ln, "add_liberty_desire *");
        hk_execline(0x77, (uint64_t)(uintptr_t)&ln, 5, 6);
        CHECK(T_nexec == 0 && strstr(T_con, "do not match"), "country id layout differs -> fail closed, nothing run");
        eng_cstr_dtor(&ln);
        T_cobj[1][3] = idBRZ; T_cobj[2][3] = idGBR;
        CHECK(T_bad_free == bad0 && T_neblk == 0, "provenance: engine lines freed by the engine only, console_plus lines never freed by it");
        g_printed_len = 0; g_printed[0] = 0;
        cmd_complete("", NULL);
        CHECK(strstr(g_printed, "wildcards: on") != NULL, "`complete` shows the wildcard status");
        g_ncmd -= 2;
        memset((void *)&W, 0, sizeof W); A.execline = NULL; o_execline = NULL;
    }
    g_console = NULL;
    names_full_reset();
    A.loc_get = NULL; A.con_print = NULL; A.dbget[CK_EVENT] = NULL;
}

/* ------------------------------------------------------------------ Part A4: country wildcards (logic) */
static int T_argok(const char *cmd, int argi, const char *tok, size_t tn, void *u)
{
    (void)u; (void)tok; (void)tn;
    if (!strcmp(cmd, "add_liberty_desire")) return argi == 0;
    if (!strcmp(cmd, "own")) return argi == 1;
    return 0;
}
static void part_a4(void)
{
    printf("== part A4: country wildcards (parse, scan, expand)\n");
    CpWild w;
    CHECK(cp_wild_parse("*", 1, &w) == CW_SUBJECTS, "'*' = subjects of anyone");
    CHECK(cp_wild_parse("M:*", 3, &w) == CW_SUBJECTS_OF_PLAYER && cp_wild_parse("m:*", 3, &w) == CW_SUBJECTS_OF_PLAYER, "'M:*' = subjects of the player");
    CHECK(cp_wild_parse("gbr:*", 5, &w) == CW_SUBJECTS_OF_TAG && !strcmp(w.tag, "GBR"), "'gbr:*' = subjects of GBR (%s)", w.tag);
    CHECK(cp_wild_parse("ALL", 3, &w) == CW_ALL && cp_wild_parse("all", 3, &w) == CW_ALL, "'ALL' = every country");
    CHECK(cp_wild_parse("GBR", 3, &w) == CW_NONE && cp_wild_parse("**", 2, &w) == CW_NONE && cp_wild_parse(":*", 2, &w) == CW_NONE &&
          cp_wild_parse("G-B:*", 5, &w) == CW_NONE && cp_wild_parse("-100", 4, &w) == CW_NONE && cp_wild_parse("ALLY", 4, &w) == CW_NONE,
          "not wildcards: GBR ** :* G-B:* -100 ALLY");
    size_t ts = 0, te = 0;
    CHECK(cp_wild_scan("add_liberty_desire * -100", T_argok, NULL, &ts, &te, &w) == 1 && ts == 19 && te == 20 && w.k == CW_SUBJECTS, "scan: '*' in the country argument");
    CHECK(cp_wild_scan("add_liberty_desire  M:*   -100", T_argok, NULL, &ts, &te, &w) == 1 && ts == 20 && te == 23 && w.k == CW_SUBJECTS_OF_PLAYER, "scan: extra blanks");
    CHECK(cp_wild_scan("add_liberty_desire GBR -100", T_argok, NULL, &ts, &te, &w) == 0, "scan: plain tag -> not ours");
    CHECK(cp_wild_scan("money *", T_argok, NULL, &ts, &te, &w) == 0, "scan: '*' in a non-country argument -> not ours");
    CHECK(cp_wild_scan("own * ALL", T_argok, NULL, &ts, &te, &w) == 1 && w.k == CW_ALL && ts == 6, "scan: only the country argument counts ('own * ALL')");
    CHECK(cp_wild_scan("x", T_argok, NULL, &ts, &te, &w) == 0 && cp_wild_scan("", T_argok, NULL, &ts, &te, &w) == 0, "scan: no arguments");
    char ln[64];
    CHECK(cp_wild_line("add_liberty_desire * -100", 19, 20, "BRZ", ln, sizeof ln) && !strcmp(ln, "add_liberty_desire BRZ -100"), "line: '%s'", ln);
    CHECK(!cp_wild_line("add_liberty_desire * -100", 19, 20, "BRZ", ln, 10), "line: too long -> refused");
    /* PRU (id 1) independent, GBR (2) subject of PRU, BRZ (3) subject of GBR, SAR (4) subject of an unknown id, SOK (5) "own overlord" */
    CpCountry c[5] = {{"PRU", 1, CP_NO_ID}, {"GBR", 2, 1}, {"BRZ", 3, 2}, {"SAR", 4, 99}, {"SOK", 5, 5}};
    int ix[8];
    cp_wild_parse("*", 1, &w);
    int m = cp_wild_expand(&w, c, 5, 1, ix, 8);
    CHECK(m == 2 && ix[0] == 1 && ix[1] == 2, "'*' -> GBR BRZ (unknown / self overlord skipped) (%d)", m);
    cp_wild_parse("M:*", 3, &w);
    m = cp_wild_expand(&w, c, 5, 1, ix, 8);
    CHECK(m == 1 && ix[0] == 1, "'M:*' (player PRU) -> GBR only (direct subjects)");
    CHECK(cp_wild_expand(&w, c, 5, CP_NO_ID, ix, 8) == -1 && cp_wild_expand(&w, c, 5, 77, ix, 8) == -1, "'M:*' without a known player -> -1");
    cp_wild_parse("gbr:*", 5, &w);
    m = cp_wild_expand(&w, c, 5, 1, ix, 8);
    CHECK(m == 1 && ix[0] == 2, "'gbr:*' -> BRZ");
    cp_wild_parse("XXX:*", 5, &w);
    CHECK(cp_wild_expand(&w, c, 5, 1, ix, 8) == -1, "'XXX:*' unknown tag -> -1");
    cp_wild_parse("BRZ:*", 5, &w);
    CHECK(cp_wild_expand(&w, c, 5, 1, ix, 8) == 0, "'BRZ:*' (no subjects) -> 0");
    cp_wild_parse("ALL", 3, &w);
    CHECK(cp_wild_expand(&w, c, 5, 1, ix, 8) == 5 && cp_wild_expand(&w, c, 5, 1, ix, 3) == 3, "'ALL' -> all 5, capped by max");
    /* code decoders on synthetic bytes */
    uint8_t code[160]; memset(code, 0x90, sizeof code);
    for (size_t i = 0; i < sizeof CP_OVL_PAT / sizeof CP_OVL_PAT[0]; i++) code[10 + i] = CP_OVL_PAT[i] < 0 ? 0 : (uint8_t)CP_OVL_PAT[i];
    uint32_t v;
    v = 0x100; memcpy(code + 10 + CP_OVL_ROOT, &v, 4);
    v = 0x1f64; memcpy(code + 10 + CP_OVL_CNT, &v, 4); v = 0x1f58; memcpy(code + 10 + CP_OVL_KEYS, &v, 4); v = 0x1f70; memcpy(code + 10 + CP_OVL_VALS, &v, 4);
    const uint8_t *root = NULL; uint32_t cn = 0, ky = 0, vl = 0;
    CHECK(cp_decode_overlord(code, sizeof code, &root, &cn, &ky, &vl) && root == code + 10 + 7 + 0x100 && cn == 0x1f64 && ky == 0x1f58 && vl == 0x1f70,
          "GetOverlord decoder: root rip-relative, count/keys/values offsets");
    code[10 + 23] = 0x4B;     /* mov edx,[rbx+18h] -> [rbx+4Bh] */
    CHECK(!cp_decode_overlord(code, sizeof code, &root, &cn, &ky, &vl), "GetOverlord decoder: other shape -> refused");
    static const uint8_t ply[] = {0x48,0x8B,0x05,0x10,0x00,0x00,0x00, 0x4C,0x8B,0xB8,0x08,0x06,0x00,0x00, 0xBA,0x3C,0x05,0x00,0x00,
        0x65,0x48,0x8B,0x04,0x25,0x58,0x00,0x00,0x00,0x48,0x8B,0x08,0x83,0x3C,0x0A,0x01,0xB8,0xFF,0xFF,0xFF,0xFF,0x74,0x07,
        0x41,0x8B,0x87,0xF8,0x00,0x00,0x00, 0x90,0x90,0x90,0x90};
    uint8_t pc[128]; memset(pc, 0xCC, sizeof pc); memcpy(pc + 30, ply, sizeof ply);
    uint32_t off = 0;
    CHECK(cp_decode_player(pc, sizeof pc, &root, &off) && off == 0xf8 && root == pc + 30 + 7 + 0x10, "player decoder: [[R]+0x608]+0x%x", off);
    pc[30 + 44] = 0x86;       /* [r14+..] instead of [r15+..] */
    CHECK(!cp_decode_player(pc, sizeof pc, &root, &off), "player decoder: different register -> refused");
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

    /* display names */
    CHECK(A.loc_get && RVA(A.loc_get) == 0x3db2390, "CLocalizationManager getter exe+0x%llx", A.loc_get ? RVA(A.loc_get) : 0ull);
    CHECK(A.loc_find && RVA(A.loc_find) == 0x3db2180, "localization map lookup exe+0x%llx (shape: buckets +8, mask +0x14, max probe +0x18, entry slot+0x10)", A.loc_find ? RVA(A.loc_find) : 0ull);
    CHECK(A.con_print && RVA(A.con_print) == 0x36bdd90, "CConsole output line exe+0x%llx", A.con_print ? RVA(A.con_print) : 0ull);
    CHECK(A.dbget[CK_EVENT] && RVA(A.dbget[CK_EVENT]) == 0x7cdd80 && A.dbvt[CK_EVENT], "CEventDatabase getter exe+0x%llx", A.dbget[CK_EVENT] ? RVA(A.dbget[CK_EVENT]) : 0ull);
    printf("       (localization use pattern: %d matches, the first one is used)\n", scan_count(PAT_LOCUSE));
    check_event_layout(g_base + 0x86e290);
    CHECK(A.dbget[CK_EVENT] != NULL, "the engine's `event` completer (exe+0x86e290) reads the event database layout console_plus uses");
    static const struct { unsigned long long exec; const char *what; unsigned arr; } X[] = {
        {0x9570d0, "invalidate_state", 0x1c30}, {0x956d40, "invalidate_character", 0x1300}, {0x957460, "invalidate_country", 0x30},
        {0x9577e0, "invalidate_ig", 0x900}, {0x8876b0, "building wage rate", 0x2d80},
    };
    int dg = 0;
    for (size_t i = 0; i < sizeof X / sizeof X[0]; i++) {
        uint8_t *s0, *s1; const uint8_t *root = NULL; uint32_t ao = 0, co = 0;
        int ok2 = fn_range(g_base + X[i].exec, &s0, &s1) && cp_decode_gs(s0, (size_t)(s1 - s0), &root, &ao, &co);
        ok2 = ok2 && RVA(root) == 0x5c3ef90 && ao == X[i].arr && co == X[i].arr + 0xc;
        printf("%s   %s: root exe+0x%llx, array +0x%x, count +0x%x\n", ok2 ? "ok " : "FAIL", X[i].what, root ? RVA(root) : 0ull, ao, co);
        dg += ok2;
    }
    CHECK(dg == (int)(sizeof X / sizeof X[0]), "objects by id decoded from %d/%d console commands (game state root exe+0x5c3ef90)", dg, (int)(sizeof X / sizeof X[0]));

    /* wildcards */
    CHECK(scan_count(PAT_EXECQ) == 1, "queued console lines loop pattern unique");
    CHECK(A.execline && RVA(A.execline) == 0x36bf740, "CConsole::ExecuteLine exe+0x%llx", A.execline ? RVA(A.execline) : 0ull);
    CHECK(W.ok && W.root && RVA(W.root) == 0x5c3ef90 && W.cnt == 0x1f64 && W.keys == 0x1f58 && W.vals == 0x1f70,
          "GetOverlord map (via make_independent): root exe+0x%llx, count +0x%x, keys +0x%x, values +0x%x", W.root ? RVA(W.root) : 0ull, W.cnt, W.keys, W.vals);
    {
        uint8_t *s0, *s1; const uint8_t *root = NULL; uint32_t off = 0;
        int okp = fn_range(g_base + 0x88c440, &s0, &s1) && cp_decode_player(s0, (size_t)(s1 - s0), &root, &off);
        CHECK(okp && RVA(root) == 0x5c3ef90 && off == 0xf8, "player country id decoded from add_liberty_desire (exe+0x88c440): [[exe+0x%llx]+0x608]+0x%x", root ? RVA(root) : 0ull, off);
        const uint8_t *r2 = NULL; uint32_t c2 = 0, k2 = 0, v2 = 0;
        CHECK(cp_decode_overlord(g_base + 0x93a030, 0x80, &r2, &c2, &k2, &v2) && r2 == W.root && c2 == W.cnt, "GetOverlord is exe+0x93a030");
    }
    /* plugin load against the real image: game part on, V3X console off (ini), no hooks in game code */
    char ini[MAX_PATH]; snprintf(ini, sizeof ini, "%sconsole_plus.ini", g_dir);
    FILE *f = fopen(ini, "w");
    if (f) { fputs("[console_plus]\nv3x_console=0\n", f); fclose(f); }
    g_nhooks = g_nframe_cbs = g_ncmds_reg = 0;
    int rc = v3x_plugin_load(api);
    CHECK(rc == 0 && A.ok && g_nframe_cbs == 1 && g_nhooks == 3 && g_hooks == 3 && A.execline && g_ncmds_reg == 2,
          "v3x_plugin_load: rc %d, on_frame %d, hooks %d (matcher + Autocomplete for display names, ExecuteLine for wildcards), `complete` + `find` registered (%d)", rc, g_nframe_cbs, g_nhooks, g_ncmds_reg);
    remove(ini);
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
    char rf[MAX_PATH]; snprintf(rf, sizeof rf, "%sconsole_child.txt", g_dir);
    remove(rf);
    char cmd[MAX_PATH * 2]; snprintf(cmd, sizeof cmd, "\"%s\" --console-child \"%s\"", self, rf);
    STARTUPINFOA si; PROCESS_INFORMATION pi; memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_MINIMIZE;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
        DWORD e = GetLastError();
        if (e == ERROR_NOT_SUPPORTED || e == ERROR_ACCESS_DENIED) { printf("SKIP part D: cannot create a process with its own console (error %lu)\n", e); return; }
        CHECK(0, "spawn child (%lu)", e); return;
    }
    DWORD wr = WaitForSingleObject(pi.hProcess, 20000);
    if (wr != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 9);
    DWORD ec = 99; GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (wr == WAIT_OBJECT_0 && ec == 4) {   /* child: CONIN$/CONOUT$ unavailable = no console could be created */
        printf("SKIP part D: the child got no console (CONIN$/CONOUT$ unavailable); Tab completion not tested\n");
        remove(rf); return;
    }
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
    test_tmp_dir(g_dir, sizeof g_dir, "namecomplete");
    static V3X_API a;
    a = make_api();
    api = &a;
    part_a();
    part_a2();
    part_a3();
    part_a4();
    part_b();
    part_b2();
    if (argc > 1) part_c(argv[1]);
    else printf("(part C skipped: no victoria3.exe given)\n");
    part_d(argv[0]);
    printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
    return fails ? 1 : 0;
}
