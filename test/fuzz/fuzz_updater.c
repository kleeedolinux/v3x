/* Fuzz harness: plugins/updater/up_logic.c - every parser the updater runs on downloaded or cached bytes:
 * the JSON scanner, the GitHub release list, compat.json (+ its signature, through up_sig.c /
 * installer/release_sig.h), the cache file (update_cache.json, user-writable), SHA256SUMS.txt, semver
 * and install.log's update-result line. The build trusts the public TEST key (never a release key) so
 * the signed-compat paths run. windows.h-free: also a libFuzzer/ASan target on Linux
 * (.github/workflows/ci.yml, job fuzz-linux).
 * Invariants: nothing reads past the input (it ends at a guard page); every returned string is
 * NUL-terminated inside its field; URLs kept are allowed ones; hashes kept are 64 hex digits; a cache
 * that parsed, written back and re-read gives the same releases; semver comparison is antisymmetric. */
#define V3X_RELEASE_KEY_DEVTEST "b397fa20bfc6bba89cd5e467041d6fce492345f3aa90d3fccda438bba02c738b"
#include "v3x_fuzz.h"
#include "../../plugins/updater/up_logic.c"
#include "../../plugins/updater/up_sig.c"

static void str_ok(const char *s, size_t cap)
{
    V3X_FUZZ_ASSERT(memchr(s, 0, cap) != NULL);
}

static void release_ok(const up_release *r)
{
    str_ok(r->tag, sizeof r->tag); str_ok(r->version, sizeof r->version); str_ok(r->compat_err, sizeof r->compat_err);
    str_ok(r->setup_name, sizeof r->setup_name); str_ok(r->setup_url, sizeof r->setup_url); str_ok(r->setup_sha, sizeof r->setup_sha);
    str_ok(r->compat_url, sizeof r->compat_url); str_ok(r->sig_url, sizeof r->sig_url); str_ok(r->sums_url, sizeof r->sums_url);
    V3X_FUZZ_ASSERT(r->nbuilds >= 0 && r->nbuilds <= UP_MAX_BUILDS && r->nassets >= 0 && r->nassets <= UP_MAX_ASSETS);
    for (int i = 0; i < r->nbuilds; i++) { str_ok(r->builds[i].branch, sizeof r->builds[i].branch); str_ok(r->builds[i].build_id, sizeof r->builds[i].build_id); }
    for (int i = 0; i < r->nassets; i++) { str_ok(r->assets[i].name, sizeof r->assets[i].name); V3X_FUZZ_ASSERT(up_url_allowed(r->assets[i].url)); }
    V3X_FUZZ_ASSERT(!r->setup_url[0] || up_url_allowed(r->setup_url));
    V3X_FUZZ_ASSERT(!r->sig_url[0] || up_url_allowed(r->sig_url));
    V3X_FUZZ_ASSERT(!r->compat_url[0] || up_url_allowed(r->compat_url));
    V3X_FUZZ_ASSERT(!r->setup_sha[0] || up_hex_valid(r->setup_sha));
    V3X_FUZZ_ASSERT(r->sig == 0 || r->sig == 1);
    if (r->sig == 1) V3X_FUZZ_ASSERT(r->compat == 1 && r->setup_sha[0] && r->compat_raw_n < sizeof r->compat_raw);
}

/* ---- JSON scanner: skip / iterate / strings / ints stay inside [p, e) */
static void walk(const char *p, const char *e, int depth)
{
    p = up_js_ws(p, e);
    if (!p || p >= e || depth > 40) return;
    const char *end = up_js_skip(p, e);
    V3X_FUZZ_ASSERT(!end || (end > p && end <= e));
    if (!end) return;
    char s[64];
    int k = up_js_string(p, e, s, sizeof s);
    V3X_FUZZ_ASSERT(k < 0 || (k < (int)sizeof s && (size_t)k == strlen(s)) || memchr(s, 0, (size_t)k + 1));
    long long iv;
    int bv;
    (void)up_js_int(p, e, &iv);
    (void)up_js_bool(p, e, &bv);
    if (*p != '[' && *p != '{') return;
    up_js_it it;
    if (up_js_begin(&it, p, e, *p)) return;
    char key[16];
    const char *v;
    int guard = 0;
    while (up_js_next(&it, *p == '{' ? key : NULL, sizeof key, &v) && guard++ < 256) {
        V3X_FUZZ_ASSERT(v >= p && v < end);
        if (*p == '{') str_ok(key, sizeof key);
        walk(v, end, depth + 1);
    }
    const char *g;
    (void)up_js_get(p, e, "assets", &g);
}

static void fuzz_json(const uint8_t *d, size_t n)
{
    const char *t = (const char *)v3x_fuzz_guarded(d, n);   /* no NUL: bounded by e */
    walk(t, t + n, 0);
}
#if !defined(V3X_FUZZ_LIBFUZZER) || defined(FUZZ_ONE_up_json)
V3X_FUZZ_TARGET(up_json, fuzz_json)
#endif
V3X_FUZZ_SEED(up_json, "{\"a\":[1,-2,3.5e+2,true,false,null,\"x\\u00e9\\ud83d\\ude00\\n\"],\"b\":{\"c\":{}}}")
V3X_FUZZ_SEED(up_json, "[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]")
V3X_FUZZ_SEED(up_json, "\"\\ud800\\udc00\\uDFFF\\u0000\"")

/* ---- GitHub release list */
static up_catalog g_cat, g_cat2;
static char g_buf[1 << 20];

static void fuzz_releases(const uint8_t *d, size_t n)
{
    const char *t = (const char *)v3x_fuzz_guarded(d, n);
    int rc = up_parse_releases(t, n, &g_cat);
    V3X_FUZZ_ASSERT(rc == -1 || (rc >= 0 && rc <= UP_MAX_REL && rc == g_cat.n));
    for (int i = 0; i < g_cat.n; i++) {
        release_ok(&g_cat.r[i]);
        up_semver sv;
        V3X_FUZZ_ASSERT(up_semver_parse(g_cat.r[i].version, &sv) == 0);
    }
}
#if !defined(V3X_FUZZ_LIBFUZZER) || defined(FUZZ_ONE_up_releases)
V3X_FUZZ_TARGET(up_releases, fuzz_releases)
#endif
V3X_FUZZ_SEED(up_releases, "[{\"tag_name\":\"v0.5.1\",\"draft\":false,\"prerelease\":false,\"assets\":[{\"name\":\"V3X-Setup.exe\",\"size\":1000,"
                           "\"browser_download_url\":\"https://github.com/kleeedolinux/v3x/releases/download/v0.5.1/V3X-Setup.exe\"},"
                           "{\"name\":\"compat.json\",\"browser_download_url\":\"https://github.com/kleeedolinux/v3x/releases/download/v0.5.1/compat.json\"},"
                           "{\"name\":\"compat.json.sig\",\"browser_download_url\":\"https://github.com/kleeedolinux/v3x/releases/download/v0.5.1/compat.json.sig\"},"
                           "{\"name\":\"SHA256SUMS.txt\",\"browser_download_url\":\"https://evil.example/../x\"}]},"
                           "{\"tag_name\":\"v0.6.0-rc.1\",\"draft\":true},{\"tag_name\":\"latest\"}]")

/* ---- compat.json (+ signature after a 0xFF byte) for a release whose tag says 0.5.1 */
static void fuzz_compat(const uint8_t *d, size_t n)
{
    const uint8_t *sep = (const uint8_t *)memchr(d, 0xFF, n);
    size_t jn = sep ? (size_t)(sep - d) : n;
    const char *json = (const char *)v3x_fuzz_guarded(d, jn);
    const char *sig = sep ? (const char *)v3x_fuzz_guarded(sep + 1, n - jn - 1) : NULL;
    up_release *r = &g_cat.r[0];
    memset(r, 0, sizeof *r);
    snprintf(r->tag, sizeof r->tag, "v0.5.1");
    snprintf(r->version, sizeof r->version, "0.5.1");
    r->nassets = 1;
    snprintf(r->assets[0].name, sizeof r->assets[0].name, "V3X-Setup.exe");
    snprintf(r->assets[0].url, sizeof r->assets[0].url, "%sv0.5.1/V3X-Setup.exe", UP_ASSET_PREFIX);
    r->assets[0].size = 1000;
    int rc = up_apply_compat(r, json, jn, sig, sig ? n - jn - 1 : 0);
    V3X_FUZZ_ASSERT((rc == 0) == (r->compat == 1));
    release_ok(r);
    if (rc == 0) V3X_FUZZ_ASSERT(r->api_version > 0 && r->api_version <= 1000 && up_name_ok(r->setup_name));
    if (sig && rc == 0) V3X_FUZZ_ASSERT(r->sig == 1 && r->compat_raw_n == jn && !memcmp(r->compat_raw, json, jn));
}
#if !defined(V3X_FUZZ_LIBFUZZER) || defined(FUZZ_ONE_up_compat)
V3X_FUZZ_TARGET(up_compat, fuzz_compat)
#endif
V3X_FUZZ_SEED(up_compat, "{\"schema\":1,\"v3x_version\":\"0.5.1\",\"api_version\":5,\"game_builds\":[{\"branch\":\"release/1.15.1\",\"build_id\":\"6af3094f-60fb000\"}],\"assets\":[{\"name\":\"V3X-Setup.exe\",\"size\":1000,\"sha256\":\"abababababababababababababababababababababababababababababababab\"}]}\n")
V3X_FUZZ_SEED(up_compat, "{\"schema\":1,\"v3x_version\":\"0.5.1\",\"api_version\":5,\"game_builds\":[{\"branch\":\"release/1.15.1\",\"build_id\":\"6af3094f-60fb000\"}],\"assets\":[{\"name\":\"V3X-Setup.exe\",\"size\":1000,\"sha256\":\"abababababababababababababababababababababababababababababababab\"}]}\n" "\xFF" "v3x-sig 1 c91e9e3c88025a36 mZGTMIik4bxtSMEiIJxVV2rg7CGfYqMvhkfLPpljaBV4fKvbP161SlMQd6iktl91K1H7cgJnZASOh3PC98qjAg==\n")
V3X_FUZZ_SEED(up_compat, "\xEF\xBB\xBF{\"v3x_version\":\"v0.5.1\",\"api_version\":5,\"game_builds\":[{\"branch\":\"release/1.15.1\"}],"
                         "\"assets\":{\"setup\":\"V3X-Setup.exe\",\"sha256\":{\"V3X-Setup.exe\":\"ABABABABABABABABABABABABABABABABABABABABABABABABABABABABABABABAB\"}}}")

/* ---- cache file: parse, invariants, write back, parse again = same releases */
static const char k_cache[] = "{\"schema\":1,\"checked_at\":5,\"attempt_at\":6,\"key\":\"k\",\"releases\":[{\"tag\":\"v0.5.1\",\"version\":\"0.5.1\",\"draft\":false,\"prerelease\":false,\"compat\":1,\"compat_err\":\"\",\"api_version\":5,\"setup_name\":\"V3X-Setup.exe\",\"setup_url\":\"https://github.com/kleeedolinux/v3x/releases/download/v0.5.1/V3X-Setup.exe\",\"setup_sha\":\"\",\"setup_size\":1000,\"sig_url\":\"\",\"compat_raw\":\"{\\\"schema\\\":1,\\\"v3x_version\\\":\\\"0.5.1\\\",\\\"api_version\\\":5,\\\"game_builds\\\":[{\\\"branch\\\":\\\"release/1.15.1\\\",\\\"build_id\\\":\\\"6af3094f-60fb000\\\"}],\\\"assets\\\":[{\\\"name\\\":\\\"V3X-Setup.exe\\\",\\\"size\\\":1000,\\\"sha256\\\":\\\"abababababababababababababababababababababababababababababababab\\\"}]}\\n\",\"compat_sig\":\"v3x-sig 1 c91e9e3c88025a36 mZGTMIik4bxtSMEiIJxVV2rg7CGfYqMvhkfLPpljaBV4fKvbP161SlMQd6iktl91K1H7cgJnZASOh3PC98qjAg==\\n\",\"game_builds\":[]}]}\n";
static void fuzz_cache(const uint8_t *d, size_t n)
{
    static int self_checked;
    if (!self_checked) {   /* the signed seed really takes the re-verify path (sig = 1, hash from the signed compat) */
        V3X_FUZZ_ASSERT(up_cache_read(k_cache, sizeof k_cache - 1, &g_cat) == 0 && g_cat.n == 1 && g_cat.r[0].sig == 1 &&
                        g_cat.r[0].setup_size == 1000 && g_cat.r[0].nbuilds == 1);
        self_checked = 1;
    }
    const char *t = (const char *)v3x_fuzz_guarded(d, n);
    if (up_cache_read(t, n, &g_cat) != 0) {
        V3X_FUZZ_ASSERT(g_cat.n == 0);
        return;
    }
    V3X_FUZZ_ASSERT(g_cat.n >= 0 && g_cat.n <= UP_MAX_REL);
    str_ok(g_cat.key, sizeof g_cat.key);
    for (int i = 0; i < g_cat.n; i++) release_ok(&g_cat.r[i]);
    size_t w = up_cache_write(&g_cat, g_buf, sizeof g_buf);
    if (!w) return;
    V3X_FUZZ_ASSERT(w < sizeof g_buf && g_buf[w] == 0);
    V3X_FUZZ_ASSERT(up_cache_read(g_buf, w, &g_cat2) == 0 && g_cat2.n == g_cat.n);
    for (int i = 0; i < g_cat.n; i++) {
        const up_release *a = &g_cat.r[i], *b = &g_cat2.r[i];
        V3X_FUZZ_ASSERT(!strcmp(a->tag, b->tag) && !strcmp(a->version, b->version) && a->sig == b->sig &&
                        !strcmp(a->setup_sha, b->setup_sha) && a->nbuilds == b->nbuilds && a->compat == b->compat);
    }
}
#if !defined(V3X_FUZZ_LIBFUZZER) || defined(FUZZ_ONE_up_cache)
V3X_FUZZ_TARGET(up_cache, fuzz_cache)
#endif
V3X_FUZZ_SEED(up_cache, k_cache)
V3X_FUZZ_SEED(up_cache, "{\"schema\":1,\"key\":\"a\\\"b\\\\c\",\"releases\":[{\"tag\":\"v1.0.0\",\"version\":\"1.0.0\",\"setup_url\":\"https://evil/\","
                        "\"setup_sha\":\"zz\",\"setup_name\":\"..\\\\x.exe\",\"compat\":7,\"game_builds\":[{\"branch\":\"b\",\"build_id\":\"\"}]}]}")

/* ---- SHA256SUMS.txt: first line = the file name looked up, the rest = the sums file */
static void fuzz_sums(const uint8_t *d, size_t n)
{
    const uint8_t *nl = (const uint8_t *)memchr(d, '\n', n);
    char name[64] = "V3X-Setup.exe";
    size_t off = 0;
    if (nl && (size_t)(nl - d) < sizeof name) { memcpy(name, d, (size_t)(nl - d)); name[nl - d] = 0; off = (size_t)(nl - d) + 1; }
    const char *t = (const char *)v3x_fuzz_guarded(d + off, n - off);
    char hex[65];
    if (up_sums_find(t, n - off, name, hex) == 0) {
        V3X_FUZZ_ASSERT(up_hex_valid(hex) && strlen(hex) == 64);
        for (int i = 0; i < 64; i++) V3X_FUZZ_ASSERT(!(hex[i] >= 'A' && hex[i] <= 'F'));
    } else {
        V3X_FUZZ_ASSERT(hex[0] == 0);
    }
}
#if !defined(V3X_FUZZ_LIBFUZZER) || defined(FUZZ_ONE_up_sums)
V3X_FUZZ_TARGET(up_sums, fuzz_sums)
#endif
V3X_FUZZ_SEED(up_sums, "V3X-Setup.exe\n1111111111111111111111111111111111111111111111111111111111111111  v3x.zip\r\n"
                       "ABABABABABABABABABABABABABABABABABABABABABABABABABABABABABABABAB *V3X-Setup.exe\r\n")
V3X_FUZZ_SEED(up_sums, "x\n  abababababababababababababababababababababababababababababababab\tx  \n")

/* ---- semver: "a\nb"; cmp antisymmetric and reflexive; install.log result line */
static void fuzz_semver(const uint8_t *d, size_t n)
{
    const char *t = v3x_fuzz_cstr(d, n);
    char a[96], b[96];
    const char *nl = strchr(t, '\n');
    size_t an = nl ? (size_t)(nl - t) : strlen(t);
    if (an >= sizeof a) an = sizeof a - 1;
    memcpy(a, t, an); a[an] = 0;
    snprintf(b, sizeof b, "%s", nl ? nl + 1 : "");
    up_semver x, y;
    int pa = up_semver_parse(a, &x), pb = up_semver_parse(b, &y);
    if (pa == 0) { str_ok(x.pre, sizeof x.pre); V3X_FUZZ_ASSERT(up_semver_cmp(&x, &x) == 0); }
    if (pa == 0 && pb == 0) {
        int c1 = up_semver_cmp(&x, &y), c2 = up_semver_cmp(&y, &x);
        V3X_FUZZ_ASSERT((c1 > 0) == (c2 < 0) && (c1 == 0) == (c2 == 0));
    }
    int v1 = up_version_cmp(a, b), v2 = up_version_cmp(b, a);
    V3X_FUZZ_ASSERT((v1 > 0) == (v2 < 0));
    char det[32];
    int ir = up_install_result(t, n, det, sizeof det);
    V3X_FUZZ_ASSERT(ir >= -1 && ir <= 1 && strlen(det) < sizeof det);
    char root[64];
    if (up_game_root(t, root, sizeof root) == 0) str_ok(root, sizeof root);
}
#if !defined(V3X_FUZZ_LIBFUZZER) || defined(FUZZ_ONE_up_semver)
V3X_FUZZ_TARGET(up_semver, fuzz_semver)
#endif
V3X_FUZZ_SEED(up_semver, "1.2.3-rc.1+b5\nv1.2.3-rc.1.x")
V3X_FUZZ_SEED(up_semver, "0.5.0-alpha.10\n0.5.0-alpha.9")
V3X_FUZZ_SEED(up_semver, "99999999999999999999.0.0\n1.0.0")
V3X_FUZZ_SEED(up_semver, "log line\nupdate-result: ok version=0.5.1\r\nupdate-result: failed rc=3 disk full\n")
