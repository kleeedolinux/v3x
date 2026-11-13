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
    wchar_t w[1200];
    snprintf(p, sizeof p, "%s%s", opaque, name);
    if (MultiByteToWideChar(CP_UTF8, 0, p, -1, w, 1200) <= 0) return 0;   /* PhysFS converts UTF-8 too */
    return GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
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

/* The cache signature is "every directory keeps its mtime". NTFS stamps a directory change with the
 * coarse system clock (~1-16 ms), so a file created within the same tick as the previous change of
 * that directory leaves its mtime unchanged and the change is invisible to the signature. Tests that
 * need "this directory changed" set the mtime explicitly (+10 s) instead of relying on timing. */
static void bump_dir_mtime(const char *rel)
{
    char p[600];
    snprintf(p, sizeof p, TROOT "/%s", rel);
    HANDLE h = CreateFileA(p, FILE_WRITE_ATTRIBUTES | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    FILETIME ft; ULARGE_INTEGER u;
    int ok = h != INVALID_HANDLE_VALUE && GetFileTime(h, NULL, NULL, &ft);
    if (ok) {
        u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime; u.QuadPart += 10ull * 10000000ull;
        ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
        ok = SetFileTime(h, NULL, NULL, &ft) != 0;
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok) { printf("FAIL bump_dir_mtime(%s): error %lu\n", rel, GetLastError()); fails++; }
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
    CHECK(ml_classify("d:/g/game", "d:/g", "c:/docs") == ML_ROOT_GAME && ml_classify("d:/g/jomini", "d:/g", "") == ML_ROOT_GAME);
    CHECK(ml_classify("d:/g/game/common", "d:/g", "") == ML_ROOT_OTHER && ml_classify("d:/g", "d:/g", "") == ML_ROOT_OTHER);
    CHECK(ml_classify("d:/g/binaries/v3x/content_packs/p", "d:/g", "") == ML_ROOT_PACK);
    CHECK(ml_classify("d:/g/binaries/v3x/content_packs", "d:/g", "") == ML_ROOT_OTHER);
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
    CHECK(r1->state == IX_READY && r1->hv.n == 8 && r1->dirs.n == 6);
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
    bump_dir_mtime("ws/steamapps/workshop/content/529340/111/common");           /* test_cache relies on this change */
    CHECK((int)stat_detour(op1, "common/new.txt", 0, 0, 0, 0, 0, 0) == 1);       /* measure: real answer */
    CHECK(r1->mismatch == 1 && g_nmismatch == 1 && !g_disabled);
    /* install root <game>/game: indexed too (index_game=1) */
    CHECK((int)stat_detour(opg, "common/Foo.txt", 0, 0, 0, 0, 0, 0) == 1);
    root_t *rg = root_find(opg);
    CHECK(rg && rg->cls == ML_ROOT_GAME);
    if (rg) wait_ready(rg);
    CHECK(rg && rg->state == IX_READY);
    LONG64 w0 = rg ? rg->would_n : 0;
    CHECK((int)stat_detour(opg, "common/bar.txt", 0, 0, 0, 0, 0, 0) == 0 && rg && rg->would_n == w0 + 1);
    /* a root that is not install/mod/DLC/pack (user dir): passthrough, never indexed, misses sampled */
    static char opu[] = TROOT "/userdir\\";
    CreateDirectoryA(TROOT "/userdir", NULL);
    LONG u0 = g_nunpred;
    CHECK((int)stat_detour(opu, "save games/x.v3", 0, 0, 0, 0, 0, 0) == 0);
    root_t *ru = root_find(opu);
    CHECK(ru && ru->cls == ML_ROOT_OTHER && ru->state == IX_NONE && ru->unpred_n == 1 && g_nunpred == u0 + 1);
    CHECK(strstr(g_unpred[u0], "root-not-indexed stat userdir: save games/x.v3") != NULL);

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

/* ---- replay: every real file path of every real root, probed against every root ------------------
 * Roots = <game>/{game,jomini,clausewitz,platform_specific_game_data}, <game>/game/dlc/<x>,
 * <game>/binaries/v3x/content_packs/<x> and the playset mods in content_load.json - the indexable part of
 * the in-game search path. Lookup paths = every file and directory path found in any of them (original
 * case from disk, as the engine asks for it). Truth = GetFileAttributesW, i.e. what DIR stat returns. */
static char g_rp[64][600]; static int g_nrp;
static void rp_add(const char *p, void *u) { (void)u; if (g_nrp < 64) snprintf(g_rp[g_nrp++], 600, "%s\\", p); }
static char **g_paths; static int g_npaths, g_cpaths;
static void collect(wchar_t *path, size_t plen, char *rel, size_t rlen)
{
    wcscpy(path + plen, L"\\*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(path, &fd);
    path[plen] = 0;
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.' && (!fd.cFileName[1] || (fd.cFileName[1] == '.' && !fd.cFileName[2]))) continue;
        char u[800];
        int ul = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, u, sizeof u, NULL, NULL);
        if (ul <= 1 || rlen + ul + 2 > 1000) continue;
        size_t rl = rlen;
        if (rl) rel[rl++] = '/';
        memcpy(rel + rl, u, ul);
        if (g_npaths == g_cpaths) { g_cpaths = g_cpaths ? g_cpaths * 2 : 65536; g_paths = realloc(g_paths, g_cpaths * sizeof *g_paths); }
        g_paths[g_npaths++] = _strdup(rel);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            size_t nl = wcslen(fd.cFileName);
            path[plen] = L'\\';
            wcscpy(path + plen + 1, fd.cFileName);
            collect(path, plen + 1 + nl, rel, rl + ul - 1);
            path[plen] = 0;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    rel[rlen] = 0;
}
static int cmpstr(const void *a, const void *b) { return _stricmp(*(char *const *)a, *(char *const *)b); }

static int replay(const char *game, const char *docs, int step)
{
    static V3X_API fake;
    fake.log = x_log; fake.print = x_print; fake.game_version = x_ver; fake.v3x_dir = x_v3x; fake.game_dir = x_game;
    api = &fake;
    QueryPerformanceFrequency(&g_freq);
    g_set_err = t_set_err;
    ml_config_defaults(&cfg);
    cfg.log_misses = 20;
    o_stat = fake_stat; o_open = fake_open;
    snprintf(g_game_native, sizeof g_game_native, "%s", game);
    ml_norm_root(game, g_game_norm, sizeof g_game_norm);
    snprintf(g_docs_native, sizeof g_docs_native, "%s", docs);
    ml_norm_root(docs, g_docs_norm, sizeof g_docs_norm);
    static const char *inst[] = {"game", "jomini", "clausewitz", "platform_specific_game_data"};
    char b[700];
    for (int i = 0; i < 4; i++) { snprintf(b, sizeof b, "%s/%s", game, inst[i]); rp_add(b, NULL); }
    const char *subs[] = {"game/dlc", "binaries/v3x/content_packs"};
    for (int k = 0; k < 2; k++) {
        WIN32_FIND_DATAA fd;
        snprintf(b, sizeof b, "%s/%s/*", game, subs[k]);
        HANDLE h = FindFirstFileA(b, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
                char r[700]; snprintf(r, sizeof r, "%s/%s/%s", game, subs[k], fd.cFileName); rp_add(r, NULL);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    snprintf(b, sizeof b, "%s/content_load.json", docs);
    char *js = read_file(b, 1 << 20, NULL);
    if (js) { ml_parse_playset(js, add_play, NULL); ml_parse_playset(js, rp_add, NULL); free(js); }
    for (int i = 0; i < g_nrp; i++)                       /* engine form: '/' separators, trailing '\' */
        for (char *c = g_rp[i]; c[1]; c++) if (*c == '\\') *c = '/';
    char rel[1100];
    for (int i = 0; i < g_nrp; i++) {
        wchar_t w[1100];
        MultiByteToWideChar(CP_UTF8, 0, g_rp[i], -1, w, 1100);
        size_t n = wcslen(w);
        while (n && (w[n - 1] == L'\\' || w[n - 1] == L'/')) w[--n] = 0;
        rel[0] = 0;
        collect(w, n, rel, 0);
    }
    qsort(g_paths, g_npaths, sizeof *g_paths, cmpstr);
    int u = 0;
    for (int i = 0; i < g_npaths; i++) if (!u || _stricmp(g_paths[u - 1], g_paths[i])) g_paths[u++] = g_paths[i];
    g_npaths = u;
    CreateDirectoryA(TROOT, NULL);
    CreateDirectoryA(TROOT "/v3x", NULL);
    remove(TROOT "/v3x/mod_loader.index");
    cfg.index_cache = 1;
    g_ncache = 0;
    /* pass A: fresh build */
    LARGE_INTEGER t0, t1, t2;
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < g_nrp; i++) { root_t *r = root_get(g_rp[i]); if (r) wait_ready(r); }
    QueryPerformanceCounter(&t1);
    double build_sum = 0, build_max = 0; size_t mem = 0, keys = 0, dirs = 0;
    for (LONG k = 0; k < g_nroots; k++) {
        root_t *r = &g_roots[k];
        build_sum += r->build_ms; if (r->build_ms > build_max) build_max = r->build_ms;
        mem += (size_t)r->hv.cap * 8 + r->dirs.ncap + (size_t)r->dirs.cap * 12; keys += r->hv.n; dirs += r->dirs.n;
    }
    printf("build: %d roots, %zu keys, %zu dirs, wall %.1f ms (sum %.1f ms, max root %.1f ms), memory %zu KB (%.1f B/key)\n",
           g_nrp, keys, dirs, (double)(t1.QuadPart - t0.QuadPart) * 1000 / g_freq.QuadPart, build_sum, build_max, mem / 1024,
           (double)mem / (keys ? keys : 1));
    QueryPerformanceCounter(&t0);
    size_t fsz = cache_save();
    QueryPerformanceCounter(&t1);
    printf("save: mod_loader.index %zu KB in %.1f ms\n", fsz / 1024, (double)(t1.QuadPart - t0.QuadPart) * 1000 / g_freq.QuadPart);
    /* lookup cost, no syscalls: hash + interpolation search */
    uint64_t *hs = malloc((size_t)g_npaths * 8);
    char kb[1100];
    int nh = 0;
    for (int p = 0; p < g_npaths; p++) { int kl = ml_norm(g_paths[p], kb, sizeof kb); if (kl >= 0) hs[nh++] = ml_hash(kb, kl); }
    uint8_t *ansA = calloc((size_t)nh * g_nrp, 1), *ansB = calloc((size_t)nh * g_nrp, 1);
    QueryPerformanceCounter(&t0);
    LONG64 hits = 0;
    for (int i = 0; i < g_nrp; i++) { root_t *r = root_find(g_rp[i]); for (int p = 0; p < nh; p++) hits += ansA[(size_t)i * nh + p] = (uint8_t)ml_hv_has(&r->hv, hs[p]); }
    QueryPerformanceCounter(&t1);
    double ns = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / g_freq.QuadPart / ((double)nh * g_nrp);
    QueryPerformanceCounter(&t0);
    for (int p = 0; p < g_npaths; p++) { uint64_t hh; if (ml_key(g_paths[p], &hh) >= 0) hits += (LONG64)(hh & 2); }
    QueryPerformanceCounter(&t1);
    printf("lookup: %.1f ns per index lookup (%d keys x %d roots), ml_key (normalize+hash, hot path) %.1f ns per path\n", ns, nh, g_nrp,
           (double)(t1.QuadPart - t0.QuadPart) * 1e9 / g_freq.QuadPart / g_npaths);
    /* syscall replay: every prediction against the real answer */
    LONG64 probes = 0, misses = 0;
    QueryPerformanceCounter(&t0);
    for (int p = 0; p < g_npaths; p += step)
        for (int i = 0; i < g_nrp; i++) {
            misses += !(int)stat_detour(g_rp[i], g_paths[p], 0, 0, 0, 0, 0, 0);
            probes++;
        }
    QueryPerformanceCounter(&t1);
    LONG64 t[11], un = 0;
    totals(t);
    int ix = 0;
    for (LONG k = 0; k < g_nroots; k++) { ix += g_roots[k].state == IX_READY; un += g_roots[k].unpred_n; }
    printf("replay: %d roots (%d indexed), %d unique paths (every %d probed), %lld probes, %lld misses, "
           "predicted %lld (%.2f%%), unpredicted %lld, mismatches %lld, false hits %lld, %.1f s\n",
           g_nrp, ix, g_npaths, step, probes, misses, t[5], 100.0 * t[5] / (misses ? misses : 1), un, t[8], t[9],
           (double)(t1.QuadPart - t0.QuadPart) / g_freq.QuadPart);
    for (LONG i = 0; i < g_nunpred && i < 3; i++) printf("  unpredicted: %s\n", g_unpred[i]);
    for (LONG i = 0; i < g_nmismatch && i < 16; i++) printf("  MISMATCH: %s\n", g_mismatch[i]);
    int bad = t[8] != 0 || t[9] != 0 || ix != g_nrp || t[5] + un != misses;
    /* pass B: next launch - load the file, verify signatures, reuse */
    LONG nA = g_nroots;
    g_nroots = 0;
    QueryPerformanceCounter(&t0);
    cache_load();
    QueryPerformanceCounter(&t1);
    for (int i = 0; i < g_nrp; i++) { root_t *r = root_get(g_rp[i]); if (r) wait_ready(r); }
    QueryPerformanceCounter(&t2);
    int reused = 0; double vsum = 0;
    for (LONG k = 0; k < g_nroots; k++) { reused += g_roots[k].from_cache; vsum += g_roots[k].build_ms; }
    for (int i = 0; i < g_nrp; i++) { root_t *r = root_find(g_rp[i]); for (int p = 0; p < nh; p++) ansB[(size_t)i * nh + p] = (uint8_t)ml_hv_has(&r->hv, hs[p]); }
    int same = !memcmp(ansA, ansB, (size_t)nh * g_nrp);
    printf("reuse: %s; load %.1f ms, %d/%ld roots reused, signature check wall %.1f ms (sum %.1f ms) vs build %.1f ms; "
           "answers identical to the fresh build: %s\n", g_cache_note, (double)(t1.QuadPart - t0.QuadPart) * 1000 / g_freq.QuadPart,
           reused, nA, (double)(t2.QuadPart - t1.QuadPart) * 1000 / g_freq.QuadPart, vsum, build_sum, same ? "yes" : "NO");
    (void)hits;
    return bad || !same || reused != g_nrp;
}

/* ---- compact index + persisted file ------------------------------------------------------------- */
static void test_index(void)
{
    ml_hv v = {0};
    char k[64];
    for (int i = 0; i < 50000; i++) { int n = snprintf(k, sizeof k, "gfx/f%d.dds", i); CHECK(!ml_hv_push(&v, ml_hash(k, n))); }
    for (int i = 0; i < 1000; i++) { int n = snprintf(k, sizeof k, "gfx/f%d.dds", i); ml_hv_push(&v, ml_hash(k, n)); }  /* dups */
    ml_hv_finish(&v);
    CHECK(v.n == 50000 && v.cap == 50000);
    int sorted = 1;
    for (uint32_t i = 1; i < v.n; i++) sorted &= v.h[i] > v.h[i - 1];
    CHECK(sorted);
    int has = 0, not = 0;
    for (int i = 0; i < 50000; i++) { int n = snprintf(k, sizeof k, "gfx/f%d.dds", i); has += ml_hv_has(&v, ml_hash(k, n)); }
    for (int i = 50000; i < 100000; i++) { int n = snprintf(k, sizeof k, "gfx/f%d.dds", i); not += ml_hv_has(&v, ml_hash(k, n)); }
    CHECK(has == 50000 && not == 0);
    CHECK(ml_hv_has(&v, v.h[0]) && ml_hv_has(&v, v.h[v.n - 1]) && !ml_hv_has(&v, 0) && !ml_hv_has(&v, ~0ull));
    ml_hv small = {0};
    CHECK(!ml_hv_has(&small, 5));
    ml_hv_push(&small, 7); ml_hv_finish(&small);
    CHECK(ml_hv_has(&small, 7) && !ml_hv_has(&small, 6) && !ml_hv_has(&small, 8));

    /* ml_key == ml_hash(ml_norm(x)), same rejects */
    static const char *kn[] = {"common/Foo.TXT", "gfx/a b/X.dds", "a", "", "/x", "a//b", "a/./b", "a/../b", "a.", "a ",
                               "COMMON~1/x", "a\\b", "x:y", "gfx/nul.dds", "gfx/com1", "gfx/console.dds", "gfx/\xc3\xa9", "a/",
                               "LPT9.txt/x", "aux", "auxx"};
    int same_k = 1;
    for (int i = 0; i < (int)(sizeof kn / sizeof *kn); i++) {
        char nb[256]; uint64_t h = 0;
        int a1 = ml_norm(kn[i], nb, sizeof nb), a2 = ml_key(kn[i], &h);
        int ok = a1 == a2 && (a1 < 0 || h == ml_hash(nb, (size_t)a1));
        same_k &= ok;
        if (!ok) printf("  ml_key differs: '%s' %d %d\n", kn[i], a1, a2);
    }
    CHECK(same_k);

    /* file round trip */
    ml_ixroot a = {0}, b = {0};
    snprintf(a.root, sizeof a.root, "d:/x/111");
    ml_dirs_add(&a.dirs, "", 11); ml_dirs_add(&a.dirs, "common", 22); ml_dirs_add(&a.dirs, "gfx/\xc3\xa9", 33);
    a.hv = v;
    snprintf(b.root, sizeof b.root, "d:/x/empty");
    ml_dirs_add(&b.dirs, "", 44);
    const ml_ixroot *rs[2] = {&a, &b};
    size_t sz = ml_ixfile_write(rs, 2, NULL, 0);
    uint8_t *buf = malloc(sz);
    CHECK(ml_ixfile_write(rs, 2, buf, sz) == sz && ml_ixfile_write(rs, 2, buf, sz - 1) == 0);
    ml_ixroot *out;
    int n = ml_ixfile_read(buf, sz, &out);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(!strcmp(out[0].root, "d:/x/111") && out[0].dirs.n == 3 && !strcmp(ml_dirs_name(&out[0].dirs, 2), "gfx/\xc3\xa9") &&
              out[0].dirs.mt[2] == 33 && out[0].hv.n == v.n && !memcmp(out[0].hv.h, v.h, (size_t)v.n * 8));
        CHECK(!strcmp(out[1].root, "d:/x/empty") && out[1].dirs.n == 1 && out[1].hv.n == 0);
        ml_ixfile_free(out, n);
    }
    /* corruption: every single-byte flip at sampled offsets, truncation, wrong version -> rejected */
    int rejected = 0, tries = 0;
    for (size_t off = 0; off < sz; off += 997) {
        buf[off] ^= 0x5A; tries++;
        if (ml_ixfile_read(buf, sz, &out) < 0) rejected++; else ml_ixfile_free(out, 2);
        buf[off] ^= 0x5A;
    }
    CHECK(rejected == tries);
    CHECK(ml_ixfile_read(buf, sz - 8, &out) < 0 && ml_ixfile_read(buf, 10, &out) < 0);
    uint32_t ver = 99; memcpy(buf + 8, &ver, 4);
    CHECK(ml_ixfile_read(buf, sz, &out) < 0);
    free(buf);
    ml_dirs_free(&a.dirs); ml_dirs_free(&b.dirs); ml_hv_free(&v); ml_hv_free(&small);
}

/* after test_runtime: save, "relaunch", reuse unchanged roots, rebuild a changed one, ignore a corrupt file */
static void test_cache(void)
{
    static char op1[] = TROOT "/ws/steamapps/workshop/content/529340/111\\";
    static char op5[] = TROOT "/ws/steamapps/workshop/content/529340/555\\";
    static const char *names[] = {"common/Foo.txt", "common/new.txt", "common/bar.txt", "gfx/a b", "gfx/a b/x.dds",
                                  "gfx/zz.dds", "localization/english/m_l_english.yml", "gfx/sub/deep/q.txt"};
    mkfile("ws/steamapps/workshop/content/529340/555/gfx/sub/deep/q.txt");
    remove(TROOT "/ws/steamapps/workshop/content/529340/555/gfx/sub/deep/added.txt");
    cfg.mode = ML_MEASURE; cfg.index_cache = 1; g_disabled = 0;
    g_ncache = 0;
    stat_detour(op5, "gfx", 0, 0, 0, 0, 0, 0);
    root_t *r5 = root_find(op5);
    root_t *r1 = NULL;                       /* the root test_runtime built (other opaque pointer, same folder) */
    for (LONG i = 0; i < g_nroots; i++) if (strstr(g_roots[i].norm, "529340/111")) { r1 = &g_roots[i]; break; }
    if (r5) wait_ready(r5);
    CHECK(r1 && r5 && r1->state == IX_READY && r5->state == IX_READY && !r5->from_cache);
    if (!r1 || !r5) return;
    int a1[8], a5[8];
    char k[256];
    for (int i = 0; i < 8; i++) {
        int kl = ml_norm(names[i], k, sizeof k);
        a1[i] = ml_hv_has(&r1->hv, ml_hash(k, kl));
        a5[i] = ml_hv_has(&r5->hv, ml_hash(k, kl));
    }
    /* 111 was built before common/new.txt existed (dir mtime recorded before listing) -> stale -> must rebuild */
    CHECK(a1[1] == 0);
    CHECK(cache_save() > 0);
    /* relaunch */
    g_nroots = 0;
    cache_load();
    CHECK(g_ncache >= 2 && strstr(g_cache_note, "root(s)"));
    stat_detour(op5, "gfx", 0, 0, 0, 0, 0, 0);
    stat_detour(op1, "gfx", 0, 0, 0, 0, 0, 0);
    r5 = root_find(op5); r1 = root_find(op1);
    if (r5) wait_ready(r5);
    if (r1) wait_ready(r1);
    CHECK(r5 && r5->from_cache && r5->state == IX_READY);
    CHECK(r1 && !r1->from_cache && r1->state == IX_READY);           /* changed since its build -> rebuilt */
    if (!r1 || !r5) return;
    int same = 1;
    for (int i = 0; i < 8; i++) {
        int kl = ml_norm(names[i], k, sizeof k);
        same &= ml_hv_has(&r5->hv, ml_hash(k, kl)) == a5[i];
    }
    CHECK(same);                                                       /* load == build */
    {
        int kl = ml_norm("common/new.txt", k, sizeof k);
        CHECK(ml_hv_has(&r1->hv, ml_hash(k, kl)));                     /* the rebuilt root sees the new file */
    }
    /* change a deep directory of 555 -> next launch rebuilds 555 */
    CHECK(cache_save() > 0);
    mkfile("ws/steamapps/workshop/content/529340/555/gfx/sub/deep/added.txt");
    bump_dir_mtime("ws/steamapps/workshop/content/529340/555/gfx/sub/deep");
    g_nroots = 0;
    cache_load();
    stat_detour(op5, "gfx", 0, 0, 0, 0, 0, 0);
    r5 = root_find(op5);
    if (r5) wait_ready(r5);
    CHECK(r5 && !r5->from_cache && r5->state == IX_READY);
    if (r5) { int kl = ml_norm("gfx/sub/deep/added.txt", k, sizeof k); CHECK(ml_hv_has(&r5->hv, ml_hash(k, kl))); }
    /* corrupt file -> ignored, full rebuild */
    CHECK(cache_save() > 0);
    FILE *f = fopen(TROOT "/v3x/mod_loader.index", "r+b");
    if (f) { fseek(f, 40, SEEK_SET); fputc('#', f); fclose(f); }
    g_nroots = 0;
    cache_load();
    CHECK(g_ncache == 0 && strstr(g_cache_note, "corrupt"));
    stat_detour(op5, "gfx", 0, 0, 0, 0, 0, 0);
    r5 = root_find(op5);
    if (r5) wait_ready(r5);
    CHECK(r5 && !r5->from_cache && r5->state == IX_READY);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 3 && !strcmp(argv[1], "--replay")) {
        int bad = replay(argv[2], argv[3], argc > 4 ? atoi(argv[4]) : 1);
        printf(bad ? "REPLAY FAILED\n" : "REPLAY OK\n");
        return bad;
    }
    test_core();
    test_parsers();
    test_index();
    test_runtime();
    test_cache();
    if (argc > 1) fails += real_exe(argv[1]);
    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails != 0;
}
