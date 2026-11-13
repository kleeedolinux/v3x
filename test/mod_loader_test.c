/* Offline test of plugins/mod_loader (no game needed).
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -o D:/v3x_tmp/modload/mlt.exe test/mod_loader_test.c
 *   D:/v3x_tmp/modload/mlt.exe                 -> ALL OK (run from v3x/: parses plugins/mod_loader/mod_loader.ini.example)
 *   D:/v3x_tmp/modload/mlt.exe <victoria3.exe> also maps the real exe (no code runs) and resolves the PhysFS DIR
 *                                              archiver (stat / openRead / error slot) with the plugin's own code
 * Covers ml_core (config, path rules, set, playset / error.log parsers, root classes) and the runtime probe
 * logic against fake DIR stat/openRead functions over real temp folders (D:/v3x_tmp/modload/t): index build,
 * measure-mode predictions + mismatch detection, mode=on skips with the PhysFS error code set, verify
 * sampling that turns the cache off, hidden roots, 8-argument forwarding, report file. */
#include "../plugins/mod_loader/ml_core.c"
#include "../plugins/mod_loader/mod_loader.c"
#include <stdarg.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define TROOT "D:/v3x_tmp/modload/t"

static void x_log(const char *tag, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); printf("[%s] ", tag); vprintf(fmt, ap); putchar('\n'); va_end(ap); }
static int x_print(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int n = vprintf(fmt, ap); putchar('\n'); va_end(ap); return n; }
static const char *x_ver(void) { return "test"; }
static const char *x_v3x(void) { return TROOT "/v3x/"; }
static const char *x_game(void) { return TROOT "/game_install/binaries/"; }

/* ---- fake PhysFS DIR functions ------------------------------------------------------------------ */
static int f_err, f_calls;
static void *f_args[8];
static void t_set_err(void) { f_err = PHYSFS_ERR_NOT_FOUND; }
static int exists(const char *opaque, const char *name)
{
    char p[1200];
    snprintf(p, sizeof p, "%s%s", opaque, name);
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}
static uintptr_t fake_stat(PASS8_ARGS)
{
    f_calls++;
    void *a[8] = {PASS8};
    memcpy(f_args, a, sizeof a);
    if (exists(a0_, a1_)) return 0xABCD000000000001ull;   /* garbage upper half: only eax counts */
    f_err = 99;  /* the engine would set NOT_FOUND; a different value proves who set it */
    return 0xABCD000000000000ull;
}
static uintptr_t fake_open(PASS8_ARGS)
{
    f_calls++;
    void *a[8] = {PASS8};
    memcpy(f_args, a, sizeof a);
    if (exists(a0_, a1_)) return 0x1234;
    f_err = 99;
    return 0;
}

static void mkfile(const char *rel)
{
    char p[600];
    snprintf(p, sizeof p, TROOT "/%s", rel);
    for (char *c = p + strlen(TROOT) + 1; *c; c++)
        if (*c == '/') { *c = 0; CreateDirectoryA(p, NULL); *c = '/'; }
    FILE *f = fopen(p, "wb");
    if (f) { fputs(strstr(rel, ".yml") ? "l_english:\n" : "x = 1\n", f); fclose(f); }
}

static void wait_ready(root_t *r)
{
    for (int i = 0; i < 400 && r->state == IX_PENDING; i++) Sleep(5);
}

static void test_core(void)
{
    char k[256];
    CHECK(ml_norm("common/Foo.TXT", k, sizeof k) == 14 && !strcmp(k, "common/foo.txt"));
    CHECK(ml_norm("a", k, sizeof k) == 1);
    CHECK(ml_norm("", k, sizeof k) < 0);
    CHECK(ml_norm("/common/x", k, sizeof k) < 0);
    CHECK(ml_norm("common//x", k, sizeof k) < 0);
    CHECK(ml_norm("common/./x", k, sizeof k) < 0);
    CHECK(ml_norm("common/../x", k, sizeof k) < 0);
    CHECK(ml_norm("common/x.", k, sizeof k) < 0);
    CHECK(ml_norm("common/x ", k, sizeof k) < 0);
    CHECK(ml_norm("COMMON~1/x.txt", k, sizeof k) < 0);
    CHECK(ml_norm("common\\x.txt", k, sizeof k) < 0);
    CHECK(ml_norm("common/x.txt:ads", k, sizeof k) < 0);
    CHECK(ml_norm("gfx/nul.dds", k, sizeof k) < 0);
    CHECK(ml_norm("gfx/com1", k, sizeof k) < 0);
    CHECK(ml_norm("gfx/console.dds", k, sizeof k) > 0);
    CHECK(ml_norm("gfx/\xc3\xa9.dds", k, sizeof k) < 0);
    CHECK(ml_norm("common/x/", k, sizeof k) < 0);
    CHECK(ml_norm("abcdefgh", k, 8) < 0);
    ml_norm_root("D:/Steam\\x//y\\", k, sizeof k);
    CHECK(!strcmp(k, "d:/steam/x/y"));

    CHECK(ml_classify("d:/steamlibrary/steamapps/workshop/content/529340/123", "d:/g", "c:/docs") == ML_ROOT_WORKSHOP);
    CHECK(ml_classify("d:/g/game/dlc/dlc001", "d:/g", "c:/docs") == ML_ROOT_DLC);
    CHECK(ml_classify("d:/g/game/dlc", "d:/g", "c:/docs") == ML_ROOT_OTHER);
    CHECK(ml_classify("d:/g/game", "d:/g", "c:/docs") == ML_ROOT_OTHER);
    CHECK(ml_classify("d:/g/game/dlcx/a", "d:/g", "c:/docs") == ML_ROOT_OTHER);
    CHECK(ml_classify("c:/docs/mod/my", "d:/g", "c:/docs") == ML_ROOT_LOCAL);
    CHECK(ml_classify("c:/docs", "d:/g", "c:/docs") == ML_ROOT_OTHER);
    CHECK(!strcmp(ml_root_leaf("d:/a/123"), "123"));

    CHECK(ml_in_list("111, 222 ,333", "222") && ml_in_list("111,222", "111") && !ml_in_list("1111", "111") &&
          !ml_in_list("", "1"));

    ml_set s = {0};
    CHECK(ml_set_add(&s, "common/a/b.txt", 14, 0) == 1);
    CHECK(ml_set_add(&s, "common/a/b.txt", 14, 0) == 0);
    CHECK(ml_set_has(&s, "common/a/b.txt", 14) && ml_set_has(&s, "common/a", 8) && ml_set_has(&s, "common", 6));
    CHECK(!ml_set_has(&s, "common/a/b.tx", 13) && !ml_set_has(&s, "commo", 5));
    CHECK(s.nfiles == 1 && s.ndirs == 2);
    for (int i = 0; i < 20000; i++) {
        int n = snprintf(k, sizeof k, "gfx/d%d/f%d.dds", i % 97, i);
        CHECK(ml_set_add(&s, k, (size_t)n, 0) == 1);
    }
    int found = 0;
    for (int i = 0; i < 20000; i++) { int n = snprintf(k, sizeof k, "gfx/d%d/f%d.dds", i % 97, i); found += ml_set_has(&s, k, (size_t)n); }
    CHECK(found == 20000);
    CHECK(s.nfiles == 20001 && s.ndirs == 2 + 1 + 97);
    int it = 0, cnt = 0, fl; const char *key;
    while ((it = ml_set_next(&s, it, &key, &fl)) >= 0) { cnt++; it++; }
    CHECK(cnt == (int)s.n);
    ml_set_free(&s);
    CHECK(!ml_set_has(&s, "x", 1));

    ml_config c;
    char err[160];
    ml_config_defaults(&c);
    CHECK(c.mode == ML_MEASURE && c.index_workshop && c.index_dlc && !c.index_local && c.verify_every == 64 && !c.skip_fatal);
    size_t n = 0;
    char *ex = read_file("plugins/mod_loader/mod_loader.ini.example", 65536, &n);
    CHECK(ex != NULL);
    if (ex) {
        ml_config d;
        ml_config_defaults(&d);
        CHECK(ml_config_parse(&d, ex, err, sizeof err) == 0);
        CHECK(!memcmp(&d, &c, sizeof c));       /* example == defaults */
        free(ex);
    }
    CHECK(ml_config_parse(&c, "\xEF\xBB\xBFmode = on\r\nskip_mods = 111,222 # x\n[s]\nverify_every=0\n", err, sizeof err) == 0);
    CHECK(c.mode == ML_ON && c.verify_every == 0 && !strcmp(c.skip_mods, "111,222"));
    CHECK(ml_config_parse(&c, "mode = fast\nbogus = 1\nfatal_min = 0\nnoeq\n", err, sizeof err) == 4);

}

static int g_np; static char g_pp[4][64];
static void pcb(const char *p, void *u) { (void)u; if (g_np < 4) snprintf(g_pp[g_np++], 64, "%s", p); }

static void test_parsers(void)
{
    const char *js = "{ \"enabledMods\": [ {\"path\": \"D:\\\\Steam\\\\529340\\\\111\"}, {\"x\":1, \"path\":\"C:/m/\\u00e9\"} ],"
                     " \"disabledDLC\": [], \"other\": {\"path\": \"no\"} }";
    CHECK(ml_parse_playset(js, pcb, NULL) == 2);
    CHECK(g_np == 2 && !strcmp(g_pp[0], "D:\\Steam\\529340\\111") && !strcmp(g_pp[1], "C:/m/\xc3\xa9"));
    CHECK(ml_parse_playset("{\"x\":1}", pcb, NULL) == -1);

    const char *log =
        "[11:27:46][pdx_persistent_reader.cpp:268]: Error: \"Expected '{' after 'margin'\" in file: \"gui/advcm_main.gui\" near line: 116\n"
        "[13:34:58][jomini_script_system.cpp:247]: Script system error!\n"
        "  Error: pop_type_percent_country trigger [ Invalid pop type in scope ]\n"
        "  Script location: common/scripted_triggers/zw_x.txt:212\n"
        "\n"
        "[11:29:14][gamedatabase.h:394]: Duplicated key sakuya will not be created from file: common/scripted_guis/S.txt:1323\n"
        "[11:29:31][pdx_localize.cpp:199]: Duplicate localization key. Key 'A' is defined in both 'localization/english/a_l_english.yml' and 'x'.\n"
        "[11:29:32][dlc.cpp:753]: Mod Cheat Menu Pro (3194757702) version 1.12.* does not match game version 1.15.1.\n";
    size_t pos = 0;
    char p[256];
    int kind;
    CHECK(ml_errlog_next(log, &pos, p, sizeof p, &kind) && kind == ML_ERR_PARSE && !strcmp(p, "gui/advcm_main.gui"));
    CHECK(ml_errlog_next(log, &pos, p, sizeof p, &kind) && kind == ML_ERR_OTHER && !strcmp(p, "common/scripted_triggers/zw_x.txt"));
    CHECK(ml_errlog_next(log, &pos, p, sizeof p, &kind) && kind == ML_ERR_DUPKEY && !strcmp(p, "common/scripted_guis/s.txt"));
    CHECK(ml_errlog_next(log, &pos, p, sizeof p, &kind) && kind == ML_ERR_DUPKEY && !strcmp(p, "localization/english/a_l_english.yml"));
    CHECK(ml_errlog_next(log, &pos, p, sizeof p, &kind) && kind == ML_ERR_OTHER && p[0] == 0);
    CHECK(!ml_errlog_next(log, &pos, p, sizeof p, &kind));
}

/* ---- runtime probe logic over real temp folders --------------------------------------------------- */
static void test_runtime(void)
{
    static V3X_API fake;
    fake.log = x_log; fake.print = x_print; fake.game_version = x_ver; fake.v3x_dir = x_v3x; fake.game_dir = x_game;
    api = &fake;
    QueryPerformanceFrequency(&g_freq);
    g_set_err = t_set_err;
    g_err_off = 0x5918;               /* only gates hiding; the test sink replaces the TLS write */
    ml_config_defaults(&cfg);
    o_stat = fake_stat; o_open = fake_open;
    CreateDirectoryA(TROOT, NULL);
    CreateDirectoryA(TROOT "/v3x", NULL);
    mkfile("ws/steamapps/workshop/content/529340/111/common/Foo.txt");
    mkfile("ws/steamapps/workshop/content/529340/111/gfx/a b/x.dds");
    mkfile("ws/steamapps/workshop/content/529340/111/localization/english/m_l_english.yml");   /* no BOM */
    mkfile("ws/steamapps/workshop/content/529340/222/common/Foo.txt");
    mkfile("ws/steamapps/workshop/content/529340/333/common/z.txt");
    mkfile("game_install/game/common/Foo.txt");
    snprintf(g_game_native, sizeof g_game_native, TROOT "/game_install");
    ml_norm_root(g_game_native, g_game_norm, sizeof g_game_norm);
    g_nplay = 0;
    add_play(TROOT "/ws/steamapps/workshop/content/529340/111", NULL);
    add_play(TROOT "/ws/steamapps/workshop/content/529340/222", NULL);
    add_play(TROOT "/ws/steamapps/workshop/content/529340/333", NULL);
    add_play(TROOT "/ws/steamapps/workshop/content/529340/444", NULL);   /* never mounted */
    g_play[2].hidden = 1;
    remove(TROOT "/ws/steamapps/workshop/content/529340/111/common/new.txt");
    remove(TROOT "/ws/steamapps/workshop/content/529340/222/common/late.txt");

    /* opaque strings like DIR_openArchive builds them: path + '\\' */
    static char op1[] = TROOT "/ws/steamapps/workshop/content/529340/111\\";
    static char op2[] = TROOT "/ws/steamapps/workshop/content/529340/222\\";
    static char op3[] = TROOT "/ws/steamapps/workshop/content/529340/333\\";
    static char opg[] = TROOT "/game_install/game\\";

    /* measure mode */
    cfg.mode = ML_MEASURE;
    f_calls = 0;
    CHECK((int)stat_detour(op1, "common/Foo.txt", (void *)3, (void *)4, (void *)5, (void *)6, (void *)7, (void *)8) == 1);
    CHECK(f_calls == 1 && f_args[0] == op1 && f_args[2] == (void *)3 && f_args[5] == (void *)6 && f_args[7] == (void *)8);
    root_t *r1 = root_find(op1);
    CHECK(r1 && r1->cls == ML_ROOT_WORKSHOP && r1->play == 0 && !r1->hidden);
    wait_ready(r1);
    CHECK(r1->state == IX_READY && r1->set.nfiles == 3);
    f_calls = 0; f_err = 0;
    CHECK((int)stat_detour(op1, "common/bar.txt", 0, 0, 0, 0, 0, 0) == 0 && f_calls == 1 && f_err == 99);
    CHECK((int)stat_detour(op1, "COMMON", 0, 0, 0, 0, 0, 0) == 1);               /* directory */
    CHECK((int)stat_detour(op1, "gfx/a b/X.dds", 0, 0, 0, 0, 0, 0) == 1);
    CHECK(open_detour(op1, "gfx/a b/x.dds", 0, 0, 0, 0, 0, 0) == 0x1234);
    CHECK(open_detour(op1, "gfx/missing.dds", 0, 0, 0, 0, 0, 0) == 0);
    /* 8.3 alias: Windows resolves COMMON~1 to "common" (if short names are on), the index cannot know
     * that, so such names are never predicted and always go to the engine */
    int alias = exists(op1, "COMMON~1/foo.txt");
    CHECK((int)stat_detour(op1, "COMMON~1/foo.txt", 0, 0, 0, 0, 0, 0) == alias);
    CHECK(f_calls == 6);
    CHECK(r1->would_n == 2 && r1->mismatch == 0 && r1->fp == 0);
    CHECK(r1->st_n == 5 && r1->st_miss == 2 - alias && r1->op_n == 2 && r1->op_miss == 1 && r1->skipped == 0);
    mkfile("ws/steamapps/workshop/content/529340/111/common/new.txt");          /* appears after indexing */
    CHECK((int)stat_detour(op1, "common/new.txt", 0, 0, 0, 0, 0, 0) == 1);       /* measure: real answer */
    CHECK(r1->mismatch == 1 && g_nmismatch == 1 && !g_disabled);
    /* a root that is not mod/DLC: passthrough, never indexed */
    CHECK((int)stat_detour(opg, "common/Foo.txt", 0, 0, 0, 0, 0, 0) == 1);
    root_t *rg = root_find(opg);
    CHECK(rg && rg->cls == ML_ROOT_OTHER && rg->state == IX_NONE);

    /* mode=on */
    cfg.mode = ML_ON; cfg.verify_every = 2; g_disabled = 0;
    CHECK((int)stat_detour(op2, "common/Foo.txt", 0, 0, 0, 0, 0, 0) == 1);
    root_t *r2 = root_find(op2);
    wait_ready(r2);
    CHECK(r2 && r2->state == IX_READY);
    f_calls = 0; f_err = 0;
    CHECK((int)stat_detour(op2, "common/nope.txt", 0, 0, 0, 0, 0, 0) == 0 && f_calls == 0 && f_err == PHYSFS_ERR_NOT_FOUND);
    f_err = 0;
    CHECK(open_detour(op2, "common/nope.txt", 0, 0, 0, 0, 0, 0) == 0 && f_calls == 1 && f_err == 99);  /* 2nd skip = verified */
    CHECK(r2->skipped == 2 && r2->verified == 1 && !g_disabled);
    CHECK((int)stat_detour(op2, "common/FOO.txt", 0, 0, 0, 0, 0, 0) == 1 && f_calls == 2);         /* hit: real call */
    mkfile("ws/steamapps/workshop/content/529340/222/common/late.txt");
    f_calls = 0;
    uintptr_t a = stat_detour(op2, "common/late.txt", 0, 0, 0, 0, 0, 0);   /* skip #3: answered "missing" (stale) */
    uintptr_t b = stat_detour(op2, "common/late.txt", 0, 0, 0, 0, 0, 0);   /* skip #4: verified -> mismatch */
    CHECK((int)a == 0 && (int)b == 1 && f_calls == 1 && g_disabled == 1 && r2->mismatch == 1);
    f_calls = 0;
    CHECK((int)stat_detour(op2, "common/nope.txt", 0, 0, 0, 0, 0, 0) == 0 && f_calls == 1);       /* cache off */

    /* hidden root (skip_mods) */
    f_calls = 0; f_err = 0;
    CHECK((int)stat_detour(op3, "common/z.txt", 0, 0, 0, 0, 0, 0) == 0 && f_calls == 0 && f_err == PHYSFS_ERR_NOT_FOUND);
    CHECK(open_detour(op3, "common/z.txt", 0, 0, 0, 0, 0, 0) == 0 && f_calls == 0);
    root_t *r3 = root_find(op3);
    CHECK(r3 && r3->hidden && r3->hidden_n == 2 && r3->state == IX_NONE);

    /* report */
    snprintf(g_last_event, sizeof g_last_event, "test");
    write_report();
    size_t n = 0;
    char *rep = read_file(TROOT "/v3x/mod_report.txt", 1 << 20, &n);
    CHECK(rep != NULL);
    if (rep) {
        CHECK(strstr(rep, "## playset") && strstr(rep, "NOT MOUNTED") && strstr(rep, "HIDDEN"));
        CHECK(strstr(rep, "mod>mod  common/foo.txt: 222 > 111"));
        CHECK(strstr(rep, "111            replaces 1 vanilla file(s)") || strstr(rep, "replaces 1 vanilla"));
        CHECK(strstr(rep, "m_l_english.yml has no UTF-8 BOM"));
        CHECK(strstr(rep, "index mismatches (must be 0; any = cache unsafe): 2"));
        free(rep);
    }
}

/* ---- real exe: resolve the DIR archiver with the plugin's code ------------------------------------ */
static uint8_t *X_base; static size_t X_size;
static uintptr_t xb(void) { return (uintptr_t)X_base; }
static size_t xs(void) { return X_size; }
static void *x_find_string(const char *s)
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(X_base + ((IMAGE_DOS_HEADER *)X_base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    size_t n = strlen(s) + 1;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (memcmp(sec->Name, ".rdata", 6)) continue;
        uint8_t *p = X_base + sec->VirtualAddress;
        for (size_t o = 0; o + n <= sec->Misc.VirtualSize; o++)
            if (p[o] == (uint8_t)s[0] && !memcmp(p + o, s, n) && (o == 0 || p[o - 1] == 0)) return p + o;
    }
    return NULL;
}
static int real_exe(const char *path)
{
    X_base = (uint8_t *)LoadLibraryExA(path, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!X_base) { printf("cannot map %s (%lu)\n", path, GetLastError()); return 1; }
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(X_base + ((IMAGE_DOS_HEADER *)X_base)->e_lfanew);
    X_size = nt->OptionalHeader.SizeOfImage;
    static V3X_API fake;
    fake.log = x_log; fake.image_base = xb; fake.image_size = xs; fake.find_string = x_find_string;
    api = &fake;
    const char *why = resolve();
    printf("real exe: %s | stat rva %llx openRead rva %llx openArchive rva %llx err_off 0x%x\n", why ? why : "resolved",
           g_stat_fn ? (unsigned long long)((uint8_t *)g_stat_fn - X_base) : 0ull,
           g_open_fn ? (unsigned long long)((uint8_t *)g_open_fn - X_base) : 0ull,
           g_archive_fn ? (unsigned long long)((uint8_t *)g_archive_fn - X_base) : 0ull, (unsigned)g_err_off);
    return why != NULL || g_err_off <= 0;
}

int main(int argc, char **argv)
{
    test_core();
    test_parsers();
    test_runtime();
    if (argc > 1) fails += real_exe(argv[1]);
    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails != 0;
}
