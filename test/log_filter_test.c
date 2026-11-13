/* Offline test of the log_filter rule engine (no game needed).
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -o D:/v3x_tmp/log-filter/lft.exe test/log_filter_test.c
 *   D:/v3x_tmp/log-filter/lft.exe plugins/log_filter/log_filter.ini.example     -> prints ALL OK */
#include "../plugins/log_filter/lf_rules.c"
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int dec(lf_config *c, const char *loc, const char *msg)
{
    return lf_decide(c, 4, loc, msg, strlen(msg), NULL);
}

int main(int argc, char **argv)
{
    /* glob */
    CHECK(lf_glob("*", "", 0));
    CHECK(lf_glob("abc", "abc", 3));
    CHECK(!lf_glob("abc", "abcd", 4));
    CHECK(lf_glob("a*d", "abcd", 4));
    CHECK(lf_glob("*is orphaned", "Event x.1 is orphaned", 21));
    CHECK(!lf_glob("*is orphaned", "Event x.1 is orphaned!", 22));
    CHECK(lf_glob("a?c", "abc", 3));
    CHECK(!lf_glob("a?c", "ac", 2));
    CHECK(lf_glob("*a*b*c*", "xxaxxbxxcxx", 11));
    CHECK(lf_glob("Compile*error X*", "Compile error:\n foo error X3000: bar", 36));
    CHECK(lf_glob("abc", "abcdef", 3));          /* explicit length: not NUL-terminated input */
    CHECK(!lf_glob("ABC", "abc", 3));            /* case-sensitive */

    /* parse errors */
    lf_config c;
    char err[160];
    CHECK(lf_config_init(&c) == 0);
    CHECK(lf_config_parse(&c, "foo = 1\ndrop = nosep\ndrop_msg =\nnoeq\ndrop = a :: \n", err, sizeof err) == 5);
    CHECK(!strncmp(err, "line 1:", 7));
    CHECK(c.nrules == 0);
    lf_config_free(&c);

    /* the built-in defaults == the shipped example file */
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        CHECK(f != NULL);
        if (f) {
            static char buf[65536];
            size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
            CHECK(n == strlen(LF_DEFAULT_INI) && !memcmp(buf, LF_DEFAULT_INI, n));
        }
    } else printf("note: example file not given, skipping identity check\n");

    /* default rules against real lines from error.log */
    CHECK(lf_config_init(&c) == 0);
    CHECK(lf_config_parse(&c, LF_DEFAULT_INI, err, sizeof err) == 0);
    CHECK(c.nrules == 5 && c.dedupe_first == 3 && c.summary_interval == 60 && c.enabled == 1);
    CHECK(dec(&c, "jomini_eventmanager.cpp:376", "Event advcm_modifier.16 is orphaned") == LF_DROP_RULE);
    CHECK(dec(&c, "modifier_type_definition_database.cpp:43",
              "Modifier type definition cheatdecrees_technological_ascendancy is defined in script but not in code, "
              "it should either be added to code or flagged script_only.") == LF_DROP_RULE);
    CHECK(dec(&c, "jomini_effect.cpp:1140", "Variable 'x' is set but is never used. Note that use in ...") == LF_DROP_RULE);
    CHECK(dec(&c, "lexer.cpp:285", "File 'common/script_values/a.txt' should be in utf8-bom encoding (will try to use it anyways)") == LF_DROP_RULE);
    CHECK(dec(&c, "dlc.cpp:822", "Could not find item in store backend.") == LF_DROP_RULE);
    /* real problems must pass */
    CHECK(dec(&c, "jomini_effect.cpp:1144", "Variable 'y' is used but is never set.") == LF_PASS);
    CHECK(dec(&c, "pdx_persistent_reader.cpp:268", "Error: \"Expected '{' after 'margin', got '5'.\" in file: \"gui/a.gui\" near line: 116") == LF_PASS);
    CHECK(dec(&c, "jomini_script_system.cpp:247", "Script system error!\n  Error: create_building effect [ x ]") == LF_PASS);
    CHECK(dec(&c, "gfx_dx11_master_context.cpp:605", "Compile error:\n a.scache(1,2): error X3004: undeclared identifier") == LF_PASS);
    CHECK(dec(&c, "virtualfilesystem.cpp:569", "Could not find texture due to 'x'") == LF_PASS);
    CHECK(dec(&c, "lexer.cpp:285", "File 'a.txt' should be in utf8-bom encoding") == LF_PASS);   /* not the 'will try' text */
    CHECK(dec(&c, "jomini_eventmanager.cpp:376", "Event x is orphaned\nbut more") == LF_PASS);

    /* dedupe: first 3 pass, rest dropped, different text or location is a new key */
    for (int i = 0; i < 3; i++) CHECK(dec(&c, "a.cpp:1", "same") == LF_PASS);
    lf_dup *d = NULL;
    CHECK(lf_decide(&c, 4, "a.cpp:1", "same", 4, &d) == LF_DROP_DUP && d && d->count == 4);
    CHECK(d && d->sample && !strcmp(d->sample, "a.cpp:1: same"));
    CHECK(dec(&c, "a.cpp:1", "same") == LF_DROP_DUP);
    CHECK(dec(&c, "a.cpp:2", "same") == LF_PASS);
    CHECK(dec(&c, "a.cpp:1", "other") == LF_PASS);
    CHECK(c.dropped_dup == 2 && c.dropped_rule == 5);

    /* min_level: below it nothing is filtered */
    c.min_level = 4;
    CHECK(lf_decide(&c, 1, "dlc.cpp:822", "Could not find item in store backend.", 37, NULL) == LF_PASS);
    CHECK(lf_decide(&c, 4, "dlc.cpp:822", "Could not find item in store backend.", 37, NULL) == LF_DROP_RULE);
    c.min_level = 0;
    /* disabled: everything passes */
    c.enabled = 0;
    CHECK(dec(&c, "dlc.cpp:822", "Could not find item in store backend.") == LF_PASS);
    c.enabled = 1;

    /* stats report */
    static char rep[65536];
    size_t rn = lf_stats_report(&c, rep, sizeof rep);
    CHECK(rn > 0 && strstr(rep, "dropped by rule 6") && strstr(rep, "a.cpp:1: same") && strstr(rep, "dlc.cpp:822"));
    CHECK(lf_stats_report(&c, rep, 16) == 15 && strlen(rep) == 15);    /* truncation stays NUL-terminated */
    lf_config_free(&c);

    /* keep beats drop and is exempt from dedupe; precise loc rules */
    CHECK(lf_config_init(&c) == 0);
    CHECK(lf_config_parse(&c, "dedupe_first = 1\r\ndrop_loc = gfx_*.cpp:*\r\nkeep = gfx_dx11_master_context.cpp:605 :: *error X*\r\n"
                              "drop_msg = *noise*\n", err, sizeof err) == 0);
    CHECK(c.nrules == 3);
    CHECK(dec(&c, "gfx_effect.cpp:697", "Failed reloading") == LF_DROP_RULE);
    CHECK(dec(&c, "gfx_dx11_master_context.cpp:605", "Compile error:\n warning X3206") == LF_DROP_RULE);
    CHECK(dec(&c, "gfx_dx11_master_context.cpp:605", "Compile error:\n error X3004") == LF_KEEP);
    CHECK(dec(&c, "gfx_dx11_master_context.cpp:605", "Compile error:\n error X3004") == LF_KEEP);
    CHECK(dec(&c, "x.cpp:1", "pure noise here") == LF_DROP_RULE);
    CHECK(dec(&c, "x.cpp:1", "real") == LF_PASS);
    CHECK(dec(&c, "x.cpp:1", "real") == LF_DROP_DUP);
    lf_config_free(&c);

    /* message templates */
    {
        char t[256];
        const char *a = "Event advcm_modifier.16 is orphaned", *b = "Event other.3 is orphaned";
        uint64_t ha = lf_template("x.cpp:1", a, strlen(a), t, sizeof t);
        CHECK(!strcmp(t, "Event * is orphaned"));
        CHECK(ha == lf_template("x.cpp:1", b, strlen(b), NULL, 0));
        CHECK(ha != lf_template("x.cpp:2", b, strlen(b), NULL, 0));           /* location is part of the key */
        const char *e = "Error: \"Expected '{' after 'margin', got '5'.: 5, near line: 116\" in file: \"gui/advcm_main.gui\" near line: 116";
        lf_template("p.cpp:268", e, strlen(e), t, sizeof t);
        CHECK(!strcmp(t, "* Q in * Q near * *"));
        const char *s1 = "Script system error!\n  Error: create_building effect [ Invalid building ]\n  Script location: a.txt:12";
        const char *s2 = "Script system error!\n  Error: create_building effect [ Attempting to manually create auto-placed building ]\n  Script location: a.txt:12";
        CHECK(lf_template("s.cpp:247", s1, strlen(s1), NULL, 0) != lf_template("s.cpp:247", s2, strlen(s2), NULL, 0));
        lf_template("s.cpp:247", s1, strlen(s1), t, sizeof t);
        CHECK(!strcmp(t, "Script system error! * * effect [ Invalid building ] Script * *"));
        CHECK(lf_template("a", "abc def", 7, t, 4) && !strcmp(t, "abc"));     /* output truncation */
        CHECK(lf_template("a", "x 'unterminated", 15, t, sizeof t) && !strcmp(t, "x Q"));
    }

    /* presets */
    CHECK(lf_config_init(&c) == 0);
    CHECK(lf_config_parse(&c, "preset = aggressive\n", err, sizeof err) == 0);
    CHECK(c.preset == 1 && c.dedupe_first == 1 && c.dedupe_template == 1 && c.summary_interval == 0);
    CHECK(lf_config_parse(&c, "preset = aggressive\ndedupe_first = 2\n", err, sizeof err) == 0 && c.dedupe_first == 2);
    CHECK(lf_config_parse(&c, "preset = default\n", err, sizeof err) == 0);
    CHECK(c.preset == 0 && c.dedupe_first == 3 && c.dedupe_template == 0 && c.summary_interval == 60);
    CHECK(lf_config_parse(&c, "preset = loud\n", err, sizeof err) == 1);
    lf_config_free(&c);
    CHECK(lf_config_init(&c) == 0);
    CHECK(lf_config_parse(&c, LF_DEFAULT_INI, err, sizeof err) == 0);
    CHECK(lf_config_parse(&c, "preset = aggressive\n", err, sizeof err) == 0);
    /* aggressive: the first of every (loc, template) is logged, repeats with other names are not */
    CHECK(dec(&c, "pdx_gui_factory.cpp:275", "failed reading property, at gui/a.gui:1") == LF_PASS);
    CHECK(dec(&c, "pdx_gui_factory.cpp:275", "failed reading property, at gui/b.gui:9") == LF_DROP_DUP);
    CHECK(dec(&c, "pdx_gui_factory.cpp:268", "failed reading callback, at gui/b.gui:9") == LF_PASS);
    CHECK(dec(&c, "jomini_eventmanager.cpp:376", "Event a.1 is orphaned") == LF_DROP_RULE);   /* rules still apply */
    lf_stats_report(&c, rep, sizeof rep);
    CHECK(strstr(rep, "preset=aggressive") && strstr(rep, "most repeated templates"));
    lf_config_free(&c);

    /* regression: editing only the preset line of the shipped file must switch the preset */
    {
        static char ini[sizeof LF_DEFAULT_INI + 16];
        const char *p = strstr(LF_DEFAULT_INI, "\npreset = default\n");
        CHECK(p != NULL);
        if (p) {
            size_t pre = (size_t)(p - LF_DEFAULT_INI) + 1;
            memcpy(ini, LF_DEFAULT_INI, pre);
            strcpy(ini + pre, "preset = aggressive\n");
            strcat(ini, p + strlen("\npreset = default\n"));
            CHECK(lf_config_init(&c) == 0);
            CHECK(lf_config_parse(&c, ini, err, sizeof err) == 0);
            CHECK(c.preset == 1 && c.dedupe_first == 1 && c.dedupe_template == 1 && c.summary_interval == 0);
            CHECK(c.nrules == 5);
            lf_config_free(&c);
        }
    }

    /* dedupe table full -> pass through, never drop unknown keys */
    CHECK(lf_config_init(&c) == 0);
    c.dedupe_first = 1;
    char m[32]; int drops = 0;
    for (int i = 0; i < (int)LF_DEDUPE_CAP; i++) {
        int k = snprintf(m, sizeof m, "msg %d", i);
        if (lf_decide(&c, 4, "f.cpp:1", m, (size_t)k, NULL) != LF_PASS) drops++;
    }
    CHECK(drops == 0 && c.ndups <= LF_DEDUPE_CAP / 4 * 3);
    lf_config_free(&c);

    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails != 0;
}
