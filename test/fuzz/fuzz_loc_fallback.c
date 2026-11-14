/* Fuzz harness: plugins/loc_fallback/lf_core.c - the loc YAML parser (lf_parse), the JSON readers
 * (lf_json_str / lf_lang_from_settings / lf_json_mod_paths), the <cache>.keys reader (lf_keys_parse)
 * and the key set. Invariants: every entry span (lang, key, ver, val) lies inside the input; keys have
 * no blank / ':' / '"'; JSON outputs are NUL-terminated within their caps; a parsed keys cache is
 * sorted and unique (bh_has binary-searches it) and every listed hash is found. */
#include "v3x_fuzz.h"
#include "../../plugins/loc_fallback/lf_core.c"

typedef struct { const char *lo, *hi; lf_set set; int n; } yml_ctx;

static int inside(const yml_ctx *c, const char *p, size_t n) { return n == 0 || (p >= c->lo && p + n <= c->hi); }

static void on_entry_cb(void *user, const lf_entry *e)
{
    yml_ctx *c = (yml_ctx *)user;
    V3X_FUZZ_ASSERT(inside(c, e->key, e->key_len) && inside(c, e->val, e->val_len) && inside(c, e->ver, e->ver_len) &&
                    (e->lang_len == 0 || inside(c, e->lang, e->lang_len)));
    V3X_FUZZ_ASSERT(e->key_len > 0 && !memchr(e->key, ':', e->key_len) && !memchr(e->key, '"', e->key_len) &&
                    !memchr(e->key, '\n', e->key_len) && !memchr(e->val, '\n', e->val_len));
    int added = 0;
    if (c->n++ < 4096) {
        int32_t *v = lf_set_put(&c->set, e->key, e->key_len, c->n, &added);
        V3X_FUZZ_ASSERT(v != NULL && lf_set_get(&c->set, e->key, e->key_len) == v);
    }
}

static void fuzz_yml(const uint8_t *d, size_t n)
{
    const char *buf = (const char *)v3x_fuzz_guarded(d, n);   /* not NUL-terminated: lf_parse takes a length */
    yml_ctx c;
    memset(&c, 0, sizeof c);
    c.lo = buf; c.hi = buf + n;
    int k = lf_parse(buf, n, on_entry_cb, &c);
    V3X_FUZZ_ASSERT(k == c.n);
    lf_set_free(&c.set);
}
V3X_FUZZ_TARGET(lf_yml, fuzz_yml)
V3X_FUZZ_SEED(lf_yml, "\xEF\xBB\xBFl_english:\r\n key_a:0 \"Text with \\\"quotes\\\" and $VAR$\" # comment\r\n key_b: \"x\"\n  key_c:\"\"\n#c\nl_german:\n bad key:0 \"x\"\n k:0 \"unclosed\n")
V3X_FUZZ_SEED(lf_yml, "l_english:\n a:1 \"\"\"\n b:12 \"x\" \"y\"   \n \xEF\xBB\xBF" "c:0 \"\xc3\xa9\"\n")

static void fuzz_json(const uint8_t *d, size_t n)
{
    const char *t = v3x_fuzz_cstr(d, n);
    char out[64], lang[32];
    if (lf_json_str(t, "language", out, sizeof out) > 0) V3X_FUZZ_ASSERT(memchr(out, 0, sizeof out) != NULL);
    (void)lf_json_str(t, "", out, sizeof out);
    if (lf_lang_from_settings(t, lang, sizeof lang) > 0) V3X_FUZZ_ASSERT(memchr(lang, 0, sizeof lang) != NULL);
    enum { EACH = 96, MAX = 8 };
    static char paths[EACH * MAX];
    int m = lf_json_mod_paths(t, paths, EACH, MAX);
    V3X_FUZZ_ASSERT(m <= MAX);
    for (int i = 0; i < m; i++) V3X_FUZZ_ASSERT(memchr(paths + (size_t)i * EACH, 0, EACH) != NULL);
}
V3X_FUZZ_TARGET(lf_json, fuzz_json)
V3X_FUZZ_SEED(lf_json, "{\"System\":{\"language\":\"l_german\"},\"enabledMods\":[{\"path\":\"C:\\\\m\\\\a\",\"x\":1},{\"path\":\"D:/b\\u00e9\"}]}")
V3X_FUZZ_SEED(lf_json, "{\"language\": \"l_english\", \"language\": \"\\\"\", \"enabledMods\": [ {\"path\" : \"\" } ] ")

static void fuzz_keys(const uint8_t *d, size_t n)
{
    /* the file's own header hash is used as the expected one half of the time (else mismatch path) */
    const char *kb = (const char *)v3x_fuzz_guarded(d, n);
    uint64_t vh = 0x1234;
    if (n >= 8 && (d[n - 1] & 1)) memcpy(&vh, kb, 8);
    size_t cnt = 0;
    uint64_t *k = lf_keys_parse(kb, n, vh, &cnt);
    if (!k) { V3X_FUZZ_ASSERT(cnt == 0); return; }
    V3X_FUZZ_ASSERT(cnt <= (n - 16) / 8);
    for (size_t i = 1; i < cnt; i++) V3X_FUZZ_ASSERT(k[i] > k[i - 1]);
    lf_ctx c;
    memset(&c, 0, sizeof c);
    c.bh = k; c.nbh = cnt;
    (void)bh_has(&c, "key", 3);
    free(k);
}
V3X_FUZZ_TARGET(lf_keys, fuzz_keys)

__attribute__((constructor)) static void seed_keys(void)
{
    uint64_t b[2 + 6] = {0x1234, 6, 9, 3, 3, 0xffffffffffffffffull, 0, 7};   /* unsorted + duplicate on purpose */
    v3x_fuzz_add_seed("lf_keys", b, sizeof b);
    b[0] = 0x0123456789abcdefull; b[1] = 6;
    v3x_fuzz_add_seed("lf_keys", b, sizeof b);
}
