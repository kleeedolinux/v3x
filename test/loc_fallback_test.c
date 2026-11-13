/* Offline test for plugins/loc_fallback (no game needed): #includes lf_core.c and checks the loc parser,
 * the JSON helpers, missing-key computation, file generation and cached / idempotent regeneration on a
 * synthetic game + mods tree under <empty temp dir>.
 *
 * Usage: loc_fallback_test.exe <empty temp dir> [<game root> <docs dir> [<stress language, default l_french> [<loc_fallback.dll>]]]
 *   with the dll it also loads the built plugin against a fake V3X_API (output under the temp dir).
 *   with the optional pair it also runs the real pipeline READ-ONLY against the real folders (game root =
 *   folder holding game/, docs = Documents/Paradox Interactive/Victoria 3); output and cache go to the temp
 *   dir. Prints the per-mod fill counts and cold / cached timings.      exit 0 = ALL OK */
#include "../plugins/loc_fallback/lf_core.c"

static int fails;
static void check(int ok, const char *what) { printf("%-66s %s\n", what, ok ? "OK" : "FAIL"); fails += !ok; }

/* ---- parser capture ---- */
static char got[64][160];
static int ngot;
static void cap(void *u, const lf_entry *e)
{
    (void)u;
    if (ngot < 64)
        snprintf(got[ngot++], sizeof got[0], "%.*s|%.*s|%.*s|%.*s", (int)e->lang_len, e->lang, (int)e->key_len, e->key,
                 (int)e->ver_len, e->ver, (int)e->val_len, e->val);
}
static int parse_str(const char *s)
{
    ngot = 0;
    return lf_parse(s, strlen(s), cap, NULL);
}

static void put(const char *root, const char *rel, const char *text)
{
    char p[LF_PATH];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    char d[LF_PATH];
    snprintf(d, sizeof d, "%s", p);
    *strrchr(d, '/') = 0;
    make_dirs(d);
    FILE *f = fopen(p, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

static char *slurp(const char *p) { return read_all(p, NULL); }

/* ---- fake V3X_API for the DLL check ---- */
#include <stdarg.h>
#include "../include/v3x.h"
static char g_logbuf[16384], dll_game[LF_PATH], dll_v3x[LF_PATH], g_cmd_name[32];
static v3x_command_fn g_cmd;
static void fake_vput(const char *tag, const char *fmt, va_list ap)
{
    size_t n = strlen(g_logbuf);
    if (tag) n += (size_t)snprintf(g_logbuf + n, sizeof g_logbuf - n, "[%s] ", tag);
    if (n < sizeof g_logbuf) n += (size_t)vsnprintf(g_logbuf + n, sizeof g_logbuf - n, fmt, ap);
    if (n + 1 < sizeof g_logbuf) g_logbuf[n] = '\n', g_logbuf[n + 1] = 0;
}
static void fake_log(const char *tag, const char *fmt, ...) { va_list ap; va_start(ap, fmt); fake_vput(tag, fmt, ap); va_end(ap); }
static int fake_print(const char *fmt, ...) { va_list ap; va_start(ap, fmt); fake_vput(NULL, fmt, ap); va_end(ap); return 0; }
static const char *fake_game_dir(void) { return dll_game; }
static const char *fake_v3x_dir(void) { return dll_v3x; }
static int fake_reg(const char *name, const char *help, v3x_command_fn fn, void *user)
{
    (void)help, (void)user;
    snprintf(g_cmd_name, sizeof g_cmd_name, "%s", name);
    g_cmd = fn;
    return 0;
}

static int count_lines_with(const char *t, const char *needle)
{
    int n = 0;
    for (const char *s = t ? strstr(t, needle) : NULL; s; s = strstr(s + 1, needle)) n++;
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: %s <empty temp dir> [<game root> <docs dir>]\n", argv[0]); return 2; }
    char T[LF_PATH];
    snprintf(T, sizeof T, "%s", argv[1]);
    for (char *s = T; *s; s++) if (*s == '\\') *s = '/';

    /* --- parser ----------------------------------------------------------------------------------- */
    int n = parse_str("\xEF\xBB\xBFl_english:\r\n # comment\r\n key_a:0 \"Hello\"\r\n key_b: \"World\" # tail\r\n"
                      "\tkey_c:\"no space\"\n key_d:12 \"say \\\"hi\\\" now\"\n key_e: \"a \"quoted\" b\"\n");
    check(n == 5, "parser: 5 entries (BOM, CRLF, comments, tab indent)");
    check(ngot == 5 && !strcmp(got[0], "l_english|key_a|0|Hello"), "parser: key:0 \"text\"");
    check(!strcmp(got[1], "l_english|key_b||World"), "parser: key: \"text\" # trailing comment");
    check(!strcmp(got[2], "l_english|key_c||no space"), "parser: key:\"text\" without blank");
    check(!strcmp(got[3], "l_english|key_d|12|say \\\"hi\\\" now"), "parser: escaped quotes kept verbatim");
    check(!strcmp(got[4], "l_english|key_e||a \"quoted\" b"), "parser: unescaped inner quotes -> last quote");
    n = parse_str(" bad key: \"x\"\n unclosed: \"abc\n novalue:0\n #x: \"y\"\n k1: \"v\"");
    check(n == 1 && !strcmp(got[0], "|k1||v"), "parser: skips space-in-key / unclosed / no value; no header = ''");
    n = parse_str("l_english:\n a: \"1\"\nl_braz_por:\n a: \"2\"\n  l_french:   # indented header + comment\n a: \"3\"\n");
    check(n == 3 && !strcmp(got[0], "l_english|a||1") && !strcmp(got[1], "l_braz_por|a||2") && !strcmp(got[2], "l_french|a||3"),
          "parser: several headers in one file (languages.yml style)");
    n = parse_str(" l_english: \"English\"\n x.y-z_1:0 \"\"\n");
    check(n == 2 && !strcmp(got[0], "|l_english||English") && !strcmp(got[1], "|x.y-z_1|0|"),
          "parser: 'l_english: \"text\"' is an entry, empty value ok");
    n = parse_str("l_english:\n a: \"x\" #c \"q\"\n");
    check(n == 1, "parser: quote inside trailing comment (tolerated)");
    check(lf_parse("", 0, cap, NULL) == 0 && lf_parse("\xEF\xBB\xBF", 3, cap, NULL) == 0, "parser: empty / BOM only");

    /* --- json --------------------------------------------------------------------------------------- */
    char lang[32];
    check(lf_lang_from_settings("{\"System\":\t{\n\t\t\"language\":\t\"l_braz_por\",\n\"x\":1}}", lang, sizeof lang) &&
              !strcmp(lang, "l_braz_por"), "settings: language = l_braz_por");
    check(!lf_lang_from_settings("{\"System\": {}}", lang, sizeof lang), "settings: no language -> 0");
    check(lf_lang_index("l_braz_por") == 1 && lf_lang_index("l_english") == 0 && lf_lang_index("braz_por") < 0,
          "lang ids");
    static char mods[8][LF_PATH];
    n = lf_json_mod_paths("{\"enabledMods\": [ {\"path\": \"C:\\\\a\\\\b\"}, {\"path\":\"D:/x\\u00e9\"}, {\"other\":\"p\"} ],"
                          " \"disabledDLC\": [{\"path\":\"no\"}]}", mods[0], sizeof mods[0], 8);
    check(n == 2 && !strcmp(mods[0], "C:\\a\\b") && !strcmp(mods[1], "D:/x\xC3\xA9"), "content_load: paths, escapes, \\u00e9");
    char nm[64];
    check(lf_json_str("{\"name\": \"My \\\"Mod\\\"\", \"id\": 1}", "name", nm, sizeof nm) && !strcmp(nm, "My \"Mod\""),
          "metadata name");

    /* --- set ---------------------------------------------------------------------------------------- */
    lf_set s = {0};
    static char keys[20000][12];
    int ok = 1, added;
    for (int i = 0; i < 20000; i++) {
        snprintf(keys[i], 12, "k%d", i);
        int32_t *v = lf_set_put(&s, keys[i], strlen(keys[i]), i, &added);
        ok &= v && added && *v == i;
    }
    for (int i = 0; i < 20000; i++) { int32_t *v = lf_set_get(&s, keys[i], strlen(keys[i])); ok &= v && *v == i; }
    ok &= lf_set_put(&s, "k5", 2, 99, &added) && !added && !lf_set_get(&s, "k20000", 6) && s.count == 20000;
    lf_set_free(&s);
    check(ok, "key set: 20000 inserts, lookups, duplicate, miss");

    /* --- pipeline on a synthetic tree ------------------------------------------------------------- */
    char G[LF_PATH], D[LF_PATH], P[LF_PATH], M1[LF_PATH], M2[LF_PATH], M3[LF_PATH], buf[LF_PATH * 3];
    snprintf(G, sizeof G, "%s/g", T);
    snprintf(D, sizeof D, "%s/docs", T);
    snprintf(P, sizeof P, "%s/packs", T);
    snprintf(M1, sizeof M1, "%s/ws/111", T);
    snprintf(M2, sizeof M2, "%s/ws/222", T);
    snprintf(M3, sizeof M3, "%s/ws/333", T);
    const char *BOM = "\xEF\xBB\xBF";
    snprintf(buf, sizeof buf, "%sl_english:\n van_a:0 \"Vanilla A\"\n van_only_en: \"VO\"\n", BOM);
    put(G, "game/localization/english/v_l_english.yml", buf);
    snprintf(buf, sizeof buf, "%sl_braz_por:\n van_a:0 \"Vanilla A pt\"\n", BOM);
    put(G, "game/localization/braz_por/v_l_braz_por.yml", buf);
    put(G, "game/localization/french/v_l_french.yml", "l_french:\n van_a: \"fr\"\n m1_a: \"fr\"\n");
    snprintf(buf, sizeof buf, "%sl_braz_por:\n cw_date: \"d\"\n", BOM);
    put(G, "clausewitz/localization/cw_l_braz_por.yml", buf);
    put(G, "jomini/localization/j_l_english.yml", "l_english:\n jom_en: \"J\"\n");
    put(G, "game/localization/languages.yml", "l_english:\n l_english: \"English\"\nl_braz_por:\n l_english: \"English\"\n");
    /* mod 1: English only, one vanilla key overridden in English, a replace/ file, a subfolder */
    snprintf(buf, sizeof buf, "%sl_english:\n m1_a:0 \"Mod one A\"\n m1_b: \"Mod \\\"one\\\" B\" # c\n van_a: \"changed\"\n", BOM);
    put(M1, "localization/english/m1_l_english.yml", buf);
    put(M1, "localization/english/sub/deep_l_english.yml", "l_english:\n m1_deep: \"Deep\"\n");
    put(M1, "localization/replace/english/r_l_english.yml", "l_english:\n shared: \"from m1 replace\"\n");
    put(M1, "localization/english/notes.txt", "l_english:\n not_yml: \"x\"\n");
    /* mod 2: English + partial braz_por; also defines 'shared' (normal, higher priority) */
    put(M2, "localization/english/m2_l_english.yml", "l_english:\n m2_a: \"Two A\"\n m2_b: \"Two B\"\n shared: \"from m2\"\n");
    put(M2, "localization/braz_por/m2_l_braz_por.yml", "\xEF\xBB\xBFl_braz_por:\n m2_a: \"Dois A\"\n");
    /* mod 3: translation mod for mod 1, plus an override of m2's English file (same path, higher prio) */
    put(M3, "localization/braz_por/m1_tr_l_braz_por.yml", "l_braz_por:\n m1_deep: \"Fundo\"\n");
    put(M3, "localization/english/m2_l_english.yml", "l_english:\n m2_a: \"Two A v2\"\n m2_b: \"Two B v2\"\n m2_c: \"Two C\"\n");
    put(D, "pdx_settings.json", "{\n\t\"System\":\t{\n\t\t\"language\":\t\"l_braz_por\"\n\t}\n}\n");
    snprintf(buf, sizeof buf, "{\"enabledMods\": [{\"path\": \"%s\"}, {\"path\": \"%s\"}, {\"path\": \"%s\"}]}", M1, M2, M3);
    for (char *q = buf; *q; q++) if (*q == '\\') *q = '/';
    put(D, "content_load.json", buf);
    put(M1, ".metadata/metadata.json", "{\"name\": \"Mod One\"}");
    /* another content pack and a stale fallback for another language */
    put(P, "other_pack/localization/braz_por/p_l_braz_por.yml", "l_braz_por:\n m2_b: \"pack pt\"\n");
    put(P, LF_PACK "/localization/french/" LF_OUT_PREFIX "l_french.yml", "l_french:\n stale: \"x\"\n");

    lf_paths p = {0};
    snprintf(p.game_root, sizeof p.game_root, "%s", G);
    snprintf(p.docs, sizeof p.docs, "%s", D);
    snprintf(p.packs, sizeof p.packs, "%s", P);
    snprintf(p.cache, sizeof p.cache, "%s/lf.cache", T);
    lf_cfg cfg = {1, 0, 0};
    static lf_result r;
    int st = lf_run(&p, &cfg, &r);
    printf("  run1: %s, filled %d, files %d parsed %d, %.1f ms, err '%s'\n", lf_status_name(st), r.total_filled, r.nfiles,
           r.nparsed, r.ms, r.err);
    check(st == LF_WRITTEN && !strcmp(r.lang, "l_braz_por"), "run1: regenerated for l_braz_por");
    char *out = slurp(r.out_path);
    char expect_path[LF_PATH];
    snprintf(expect_path, sizeof expect_path, "%s/" LF_PACK "/localization/braz_por/" LF_OUT_PREFIX "l_braz_por.yml", P);
    check(out && !strcmp(r.out_path, expect_path), "output at packs/v3x_loc_fallback/localization/braz_por/zz_..._l_braz_por.yml");
    check(out && !memcmp(out, "\xEF\xBB\xBFl_braz_por:\n", 15), "output: UTF-8 BOM + l_braz_por: header");
    check(count_lines_with(out, " m1_a:0 \"Mod one A\"\n") == 1, "filled: m1_a with version and text verbatim");
    check(count_lines_with(out, " m1_b: \"Mod \\\"one\\\" B\"\n") == 1, "filled: m1_b escaped quotes verbatim, comment dropped");
    check(count_lines_with(out, "m1_deep") == 0, "not filled: m1_deep (translated by mod 3)");
    check(count_lines_with(out, " van_a") == 0, "not filled: van_a (vanilla has braz_por)");
    check(count_lines_with(out, " m2_a") == 0, "not filled: m2_a (mod 2 braz_por)");
    check(count_lines_with(out, " m2_b") == 0, "not filled: m2_b (another content pack has it)");
    check(count_lines_with(out, " m2_c: \"Two C\"") == 1, "filled: m2_c from mod 3's override of m2's English file");
    check(count_lines_with(out, "Two A v2") + count_lines_with(out, "\"Two B\"") == 0, "overridden m2 English file not read");
    check(count_lines_with(out, " shared: \"from m1 replace\"") == 1, "duplicate English key: replace/ wins over priority");
    check(count_lines_with(out, "van_only_en") + count_lines_with(out, "jom_en") == 0, "also_fill_vanilla=0: vanilla not filled");
    check(count_lines_with(out, "not_yml") == 0, "non-.yml files ignored");
    check(r.total_filled == 4 && r.mods[0].filled == 3 && r.mods[1].filled == 0 && r.mods[2].filled == 1,
          "counts: total 4 = mod1 3 + mod3 1");
    check(!strcmp(r.mods[0].name, "Mod One") && !strcmp(r.mods[1].name, "222") && !strcmp(r.mods[0].id, "111"),
          "mod names from metadata.json, id fallback");
    snprintf(buf, sizeof buf, "%s/" LF_PACK "/localization/french/" LF_OUT_PREFIX "l_french.yml", P);
    check(GetFileAttributesA(buf) == INVALID_FILE_ATTRIBUTES, "stale fallback of another language removed");
    /* the output re-parses as valid loc with the right entries */
    ngot = 0;
    n = out ? lf_parse(out, strlen(out), cap, NULL) : 0;
    check(n == 4 && !strncmp(got[0], "l_braz_por|", 11), "output re-parses: 4 entries under l_braz_por");

    /* idempotent: second run is a cache hit with identical output */
    lf_result r2;
    st = lf_run(&p, &cfg, &r2);
    char *out2 = slurp(r2.out_path);
    check(st == LF_CACHED && r2.total_filled == 4 && r2.mods[0].filled == 3 && out2 && !strcmp(out, out2),
          "run2: cached, same counts, file unchanged");
    printf("  run2: %s %.2f ms\n", lf_status_name(st), r2.ms);
    free(out2);
    /* forced rebuild gives byte-identical output */
    cfg.force = 1;
    st = lf_run(&p, &cfg, &r2);
    out2 = slurp(r2.out_path);
    check(st == LF_WRITTEN && out2 && !strcmp(out, out2), "forced rebuild: byte-identical output");
    free(out2);
    cfg.force = 0;
    /* output deleted by hand -> regenerated despite the cache */
    DeleteFileA(r.out_path);
    st = lf_run(&p, &cfg, &r2);
    check(st == LF_WRITTEN && GetFileAttributesA(r.out_path) != INVALID_FILE_ATTRIBUTES, "missing output -> regenerated");
    /* an input changes -> regenerated */
    Sleep(20);
    put(M2, "localization/braz_por/m2_l_braz_por.yml", "\xEF\xBB\xBFl_braz_por:\n m2_a: \"Dois A\"\n m2_c: \"Dois C\"\n");
    st = lf_run(&p, &cfg, &r2);
    out2 = slurp(r2.out_path);
    check(st == LF_WRITTEN && r2.total_filled == 3 && count_lines_with(out2, "m2_c") == 0, "input changed -> regenerated, m2_c now translated");
    check(r.vanilla_keys_cached == 0 && r2.vanilla_keys_cached == 1 && count_lines_with(out2, " van_a") == 0,
          "mod change: vanilla keys from the .keys cache (van_a still known)");
    free(out2);
    /* new file added -> regenerated */
    put(M2, "localization/english/new_l_english.yml", "l_english:\n m2_new: \"N\"\n");
    st = lf_run(&p, &cfg, &r2);
    check(st == LF_WRITTEN && r2.total_filled == 4, "new English file -> regenerated, +1 key");
    /* also_fill_vanilla */
    cfg.also_fill_vanilla = 1;
    st = lf_run(&p, &cfg, &r2);
    out2 = slurp(r2.out_path);
    check(st == LF_WRITTEN && r2.vanilla_filled == 2 && count_lines_with(out2, " van_only_en: \"VO\"") == 1 &&
              count_lines_with(out2, " jom_en") == 1 && count_lines_with(out2, " l_english") == 0,
          "also_fill_vanilla=1: vanilla + jomini English keys filled (config change busts cache)");
    free(out2);
    cfg.also_fill_vanilla = 0;
    /* everything translated -> no file */
    put(M3, "localization/braz_por/all_l_braz_por.yml",
        "l_braz_por:\n m1_a: \"a\"\n m1_b: \"b\"\n shared: \"s\"\n m2_new: \"n\"\n");
    st = lf_run(&p, &cfg, &r2);
    check(st == LF_NOTHING && r2.total_filled == 0 && GetFileAttributesA(r.out_path) == INVALID_FILE_ATTRIBUTES,
          "nothing missing -> no file");
    check(r2.nparsed == 9, "mods translate everything -> vanilla files not read (9 mod/pack files parsed)");
    st = lf_run(&p, &cfg, &r2);
    check(st == LF_CACHED && r2.total_filled == 0, "nothing missing, again -> cached");
    /* English player: files removed */
    DeleteFileA(r.out_path);
    snprintf(buf, sizeof buf, "%s/ws/333/localization/braz_por/all_l_braz_por.yml", T);
    DeleteFileA(buf);
    st = lf_run(&p, &cfg, &r2);
    check(st == LF_WRITTEN, "back to missing keys -> written");
    snprintf(p.lang, sizeof p.lang, "l_english");
    st = lf_run(&p, &cfg, &r2);
    check(st == LF_ENGLISH && GetFileAttributesA(r.out_path) == INVALID_FILE_ATTRIBUTES, "English: nothing to do, file removed");
    p.lang[0] = 0;
    st = lf_run(&p, &cfg, &r2);
    cfg.enabled = 0;
    int st2 = lf_run(&p, &cfg, &r2);
    check(st == LF_WRITTEN && st2 == LF_DISABLED && GetFileAttributesA(r.out_path) == INVALID_FILE_ATTRIBUTES,
          "enabled=0: file removed");
    cfg.enabled = 1;
    snprintf(p.lang, sizeof p.lang, "l_klingon");
    st = lf_run(&p, &cfg, &r2);
    check(st == LF_ERROR && strstr(r2.err, "unknown language"), "unknown language -> error, nothing written");
    p.lang[0] = 0;
    free(out);

    /* --- real folders (read-only) ------------------------------------------------------------- */
    if (argc >= 4) {
        lf_paths rp = {0};
        snprintf(rp.game_root, sizeof rp.game_root, "%s", argv[2]);
        snprintf(rp.docs, sizeof rp.docs, "%s", argv[3]);
        snprintf(rp.packs, sizeof rp.packs, "%s/real_packs", T);
        snprintf(rp.cache, sizeof rp.cache, "%s/real.cache", T);
        make_dirs(rp.packs);
        lf_cfg rc = {1, 0, 1};
        static lf_result rr;
        st = lf_run(&rp, &rc, &rr);
        printf("  REAL cold: %s lang %s in %.0f ms: %d keys filled, %d mods, %d roots, %d files, %d parsed, %d present, "
               "%d English candidates, %d dup; err '%s'\n", lf_status_name(st), rr.lang, rr.ms, rr.total_filled, rr.nmods,
               rr.nroots, rr.nfiles, rr.nparsed, rr.present_keys, rr.english_keys, rr.dup_english, rr.err);
        for (int i = 0; i < rr.nmods; i++)
            printf("    %5d filled  (%5d en, %5d %s)  %s  %s\n", rr.mods[i].filled, rr.mods[i].en_keys, rr.mods[i].lang_keys,
                   rr.lang, rr.mods[i].id, rr.mods[i].name);
        check(st == LF_WRITTEN || st == LF_NOTHING || st == LF_ENGLISH, "real: pipeline ran");
        check(rr.ms < 2000, "real: cold run < 2 s");
        rc.force = 0;
        static lf_result rr2;
        st = lf_run(&rp, &rc, &rr2);
        printf("  REAL vanilla keys %s\n", rr.vanilla_keys_cached ? "from cache" : "parsed (or not needed)");
        printf("  REAL cached: %s in %.1f ms, %d keys\n", lf_status_name(st), rr2.ms, rr2.total_filled);
        check(rr.status == LF_ENGLISH || (st == LF_CACHED && rr2.total_filled == rr.total_filled), "real: second run cached");
        rc.also_fill_vanilla = 1;
        rc.force = 1;
        st = lf_run(&rp, &rc, &rr2);
        printf("  REAL also_fill_vanilla=1: %s in %.0f ms, %d keys (%d vanilla)\n", lf_status_name(st), rr2.ms,
               rr2.total_filled, rr2.vanilla_filled);
        rc.also_fill_vanilla = 0;
        /* stress: a language several of these mods do not translate (vanilla must be read) */
        snprintf(rp.lang, sizeof rp.lang, "%s", argc >= 5 ? argv[4] : "l_french");
        st = lf_run(&rp, &rc, &rr2);
        printf("  REAL %s (stress, forced): %s in %.0f ms, %d keys filled, %d files parsed\n", rp.lang, lf_status_name(st),
               rr2.ms, rr2.total_filled, rr2.nparsed);
        DeleteFileA(rp.cache);
        rc.force = 0;
        st = lf_run(&rp, &rc, &rr2);
        printf("  REAL stress language, main cache gone, vanilla .keys cache: %s in %.0f ms, %d keys, vanilla keys %s\n",
               lf_status_name(st), rr2.ms, rr2.total_filled, rr2.vanilla_keys_cached ? "from cache" : "parsed");
        check(rr2.vanilla_keys_cached && rr2.total_filled > 0, "real stress language: vanilla .keys cache used, keys filled");
        rp.lang[0] = 0;
        rc.force = 1;
        st = lf_run(&rp, &rc, &rr2);   /* leave the default-config output for inspection */
        printf("  REAL output: %s\n", rr2.out_path);
    }

    /* --- built DLL against a fake V3X_API: game_dir = <game root>/binaries/ (real, read-only),
     *     v3x_dir = <temp>/v3x/ (ini, cache, content_packs). Docs = the real Documents folder. --- */
    if (argc >= 6) {
        snprintf(dll_game, sizeof dll_game, "%s/binaries/", argv[2]);
        snprintf(dll_v3x, sizeof dll_v3x, "%s/v3x/", T);
        snprintf(buf, sizeof buf, "%s/v3x/content_packs", T);
        make_dirs(buf);
        snprintf(buf, sizeof buf, "%s/v3x/loc_fallback.ini", T);
        FILE *f = fopen(buf, "wb");
        if (f) { fputs("[loc_fallback]\nenabled = 1\nlanguage = l_german   ; forced for the test\n", f); fclose(f); }
        HMODULE h = LoadLibraryA(argv[5]);
        int (*load)(const V3X_API *) = h ? (int (*)(const V3X_API *))(void *)GetProcAddress(h, "v3x_plugin_load") : NULL;
        const V3X_PluginManifest *man = h ? (const V3X_PluginManifest *)(void *)GetProcAddress(h, "v3x_plugin_manifest") : NULL;
        check(load && man && !strcmp(man->name, "loc_fallback"), "dll: exports v3x_plugin_load + manifest");
        V3X_API api = {0};
        api.api_version = V3X_API_VERSION;
        api.struct_size = sizeof api;
        api.log = fake_log;
        api.print = fake_print;
        api.game_dir = fake_game_dir;
        api.v3x_dir = fake_v3x_dir;
        api.register_command = fake_reg;
        int rc = load ? load(&api) : -1;
        check(rc == 0 && g_cmd && !strcmp(g_cmd_name, "locfallback"), "dll: load ok, 'locfallback' registered");
        check(strstr(g_logbuf, "language l_german: regenerated") != NULL && strstr(g_logbuf, "key(s) filled"),
              "dll: summary logged (forced l_german via ini)");
        snprintf(buf, sizeof buf, "%s/v3x/content_packs/" LF_PACK "/localization/german/" LF_OUT_PREFIX "l_german.yml", T);
        check(GetFileAttributesA(buf) != INVALID_FILE_ATTRIBUTES, "dll: fallback file written under v3x_dir/content_packs");
        g_logbuf[0] = 0;
        if (g_cmd) g_cmd("", NULL), g_cmd("rebuild", NULL);
        check(strstr(g_logbuf, "rebuilt") && strstr(g_logbuf, "language l_german"), "dll: 'locfallback' + 'rebuild' print");
        printf("%s", g_logbuf);
    }
    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails ? 1 : 0;
}
