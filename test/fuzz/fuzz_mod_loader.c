/* Fuzz harness: plugins/mod_loader/ml_core.c - the on-disk index reader (ml_ixfile_read), the
 * mod_loader.ini parser, the playset JSON reader and the error.log scanner.
 * ml_ixfile: once as-is, once with the trailing FNV-1a checksum recomputed (so mutations get past
 * the checksum into the structure checks). Invariants: an accepted file re-serialises to exactly
 * the same bytes; roots are NUL-terminated < 600; hash arrays strictly increasing. */
#include "v3x_fuzz.h"
#include "../../plugins/mod_loader/ml_core.c"

static void check_ix(const uint8_t *buf, size_t len)
{
    ml_ixroot *r = NULL;
    int n = ml_ixfile_read(buf, len, &r);
    if (n < 0) { V3X_FUZZ_ASSERT(r == NULL); return; }
    const ml_ixroot **pp = (const ml_ixroot **)malloc(sizeof *pp * (size_t)(n ? n : 1));
    V3X_FUZZ_ASSERT(pp != NULL);
    for (int i = 0; i < n; i++) {
        V3X_FUZZ_ASSERT(memchr(r[i].root, 0, sizeof r[i].root) != NULL);
        for (uint32_t k = 1; k < r[i].hv.n; k++) V3X_FUZZ_ASSERT(r[i].hv.h[k] > r[i].hv.h[k - 1]);
        for (uint32_t k = 0; k < r[i].dirs.n; k++) (void)strlen(ml_dirs_name(&r[i].dirs, k));
        if (r[i].hv.n) (void)ml_hv_has(&r[i].hv, r[i].hv.h[r[i].hv.n / 2]);
        pp[i] = &r[i];
    }
    /* round trip, unless a name had an embedded NUL (the writer stores strlen) */
    size_t need = ml_ixfile_write(pp, n, NULL, 0);
    uint8_t *w = (uint8_t *)malloc(need);
    V3X_FUZZ_ASSERT(w && ml_ixfile_write(pp, n, w, need) == need);
    if (need == len) V3X_FUZZ_ASSERT(!memcmp(w, buf, len));
    free(w);
    free((void *)pp);
    ml_ixfile_free(r, n);
}

static void fuzz_ixfile(const uint8_t *d, size_t n)
{
    check_ix((const uint8_t *)v3x_fuzz_guarded(d, n), n);
    if (n >= 24) {                                  /* the same bytes with a valid checksum */
        uint8_t *g = (uint8_t *)v3x_fuzz_guarded(d, n);
        uint64_t sum = fnv((const char *)g, n - 8);
        memcpy(g + n - 8, &sum, 8);
        check_ix(g, n);
    }
}
V3X_FUZZ_TARGET(ml_ixfile, fuzz_ixfile)

__attribute__((constructor)) static void seed_ixfile(void)
{
    static ml_ixroot a, b;
    snprintf(a.root, sizeof a.root, "d:/steamlibrary/steamapps/workshop/content/529340/3385002128");
    ml_dirs_add(&a.dirs, "", 0x01d9aabbccddeeffull);
    ml_dirs_add(&a.dirs, "common", 2);
    ml_dirs_add(&a.dirs, "common/history", 3);
    for (int i = 0; i < 40; i++) ml_hv_push(&a.hv, ml_hash("common/history/x.txt", 18 + (size_t)(i % 3)) + (uint64_t)i * 977);
    ml_hv_finish(&a.hv);
    snprintf(b.root, sizeof b.root, "c:/users/x/documents/paradox interactive/victoria 3/mod/m");
    const ml_ixroot *rs[2] = {&a, &b};
    size_t n = ml_ixfile_write(rs, 2, NULL, 0);
    uint8_t *buf = (uint8_t *)malloc(n);
    if (buf && ml_ixfile_write(rs, 2, buf, n) == n) v3x_fuzz_add_seed("ml_ixfile", buf, n);
    free(buf);
}

static void fuzz_ml_text(const uint8_t *d, size_t n)
{
    const char *t = v3x_fuzz_cstr(d, n);
    ml_config c;
    char err[128];
    err[0] = 0;
    (void)ml_config_parse(&c, t, err, sizeof err);
    V3X_FUZZ_ASSERT(memchr(err, 0, sizeof err) != NULL);
    (void)ml_parse_playset(t, NULL, NULL);
    size_t pos = 0;
    char path[260];
    int kind, guard = 0;
    while (ml_errlog_next(t, &pos, path, sizeof path, &kind) && guard++ < 100000) {
        V3X_FUZZ_ASSERT(pos <= n && memchr(path, 0, sizeof path));
    }
    char norm[300];
    if (ml_norm(t, norm, sizeof norm) >= 0) V3X_FUZZ_ASSERT(memchr(norm, 0, sizeof norm) != NULL);
}
V3X_FUZZ_TARGET(ml_text, fuzz_ml_text)
V3X_FUZZ_SEED(ml_text, "mode = on\nverify_every = 64\nskip_fatal = 1\nskip_mods = a;b\n[x]\nindex_cache=1\n")
V3X_FUZZ_SEED(ml_text, "{\"enabledMods\":[{\"path\":\"D:\\\\x\\\\mod\\\\a\"},{\"path\":\"C:/m/b\"}],\"x\":[1,2,{\"y\":\"\\u00e9\"}]}")
V3X_FUZZ_SEED(ml_text, "[12:00:01][jomini_script_system.cpp:283]: Error: unexpected token in file: common/history/x.txt line: 3\n  continued\n[12:00:02][pdx_persistent_reader.cpp:216]: Error: \"Unexpected token: }\" in file: \"events/e.txt\" near line: 9\n")
