/* Offline test of plugins/updater (no network: a fake up_http_fn serves GitHub-shaped answers).
 *
 *   updater_test.exe            unit tests: SHA-256, semver, JSON scanner (+ malformed / fuzz), GitHub list
 *                               and compat.json parsing, selection (1.15.1 vs 1.15.2, never downgrade, beta,
 *                               build_id vs branch, api), rate limit, cache file, download verification,
 *                               setup command line (round trip through CommandLineToArgvW), install.log result
 *   updater_test.exe <dir>      + end to end: <dir>/binaries/upd_child.exe (this file built with
 *                               -DUPDATER_CHILD, linked to version.dll) loads build/plugins/updater.dll with
 *                               V3X_BUILD_GATE=1 (unknown build = safe mode) and V3X_UPDATER_NO_NET=1 (no network);
 *                               a cached compatible release + a verified fake setup on disk -> at process exit the
 *                               plugin starts the "setup" (a copy of this exe) with --update --silent --game-dir
 *                               <dir> --wait-pid <child pid>; then a tampered setup -> nothing is started.
 * Build (test/run_all.sh "updater"):
 *   cc -o updt.exe test/updater_test.c -lshell32
 *   cc -DUPDATER_CHILD -o <dir>/binaries/upd_child.exe test/updater_test.c -Lbuild -l:version.dll
 * Exit code 0 = ALL OK. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef UPDATER_CHILD
/* ------------------------------------------------------------------ child: a "game" that loads the loader */
#include "../include/v3x.h"
__declspec(dllimport) const V3X_API *v3x_api(void);
static char g_out[16384];
static void sink(const char *t, void *u) { (void)u; strncat(g_out, t, sizeof g_out - strlen(g_out) - 1); }
int main(int argc, char **argv)
{
    const V3X_API *api = v3x_api();
    /* "" = do nothing (auto); "now" = type `update now` (the cached release is unsigned) and wait
     * until it is ready; "tamper" = `update now` with a modified setup on disk (never ready) */
    const char *mode = argc > 1 ? argv[1] : "";
    int want_ready = !strcmp(mode, "now");
    int armed = !(want_ready || !strcmp(mode, "tamper"));
    FILE *f = fopen("child_out.txt", "wb");
    int ready = 0;
    for (int i = 0; i < 100 && !ready; i++) {   /* <= 10 s */
        MSG m;
        PeekMessageW(&m, NULL, 0, 0, PM_REMOVE);
        g_out[0] = 0;
        if (!armed) {
            api->exec_capture("update now", sink, NULL);
            armed = strstr(g_out, "when you quit") != NULL;
            g_out[0] = 0;
        }
        api->exec_capture("update", sink, NULL);
        ready = strstr(g_out, "downloaded and verified") != NULL;
        if (!ready) Sleep(want_ready ? 100 : 30);
    }
    g_out[0] = 0;
    api->exec_capture("update", sink, NULL);
    api->exec_capture("plugins", sink, NULL);
    if (f) { fputs(g_out, f); fclose(f); }
    return 0;   /* -> ExitProcess -> process_exit */
}
#else

#include <shellapi.h>
/* the public TEST key of tools/sign_release.py (seed = SHA-256 of TEST_SEED_TEXT): trusted only by
 * this test binary, never by updater.dll */
#define V3X_RELEASE_KEY_DEVTEST "b397fa20bfc6bba89cd5e467041d6fce492345f3aa90d3fccda438bba02c738b"
#define TEST_SEED_TEXT "V3X PUBLIC TEST KEY - everyone knows this seed, never trust it in a release"
#include "../plugins/updater/up_logic.c"
#include "../plugins/updater/up_sig.c"

static int fails, checks;
#define CHECK(c, ...) do { int ok_ = !!(c); checks++; if (!ok_ || verbose) { printf("%s: ", ok_ ? "OK  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); } fails += !ok_; } while (0)
static int verbose = 1;

/* ------------------------------------------------------------------ fake HTTP */
#define MAXR 64
typedef struct { char url[400]; int status; char *body; size_t len; } route;
typedef struct { route r[MAXR]; int n; int calls, bad_urls; char log[8192]; } fake;

static void route_add(fake *f, const char *url, int status, const char *body, size_t len)
{
    route *r = &f->r[f->n++];
    snprintf(r->url, sizeof r->url, "%s", url);
    r->status = status;
    r->body = body ? (char *)malloc(len + 1) : NULL;
    if (body) { memcpy(r->body, body, len); r->body[len] = 0; }
    r->len = len;
}
static void fake_free(fake *f)
{
    for (int i = 0; i < f->n; i++) free(f->r[i].body);
    memset(f, 0, sizeof *f);
}
static int fake_get(void *ctx, const char *url, size_t max, char **body, size_t *len)
{
    fake *f = (fake *)ctx;
    f->calls++;
    strncat(f->log, url, sizeof f->log - strlen(f->log) - 2);
    strcat(f->log, "\n");
    if (!up_url_allowed(url)) f->bad_urls++;
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->r[i].url, url)) {
            if (f->r[i].status != 200) return f->r[i].status;
            if (f->r[i].len > max) return -15;
            *body = (char *)malloc(f->r[i].len + 1);
            memcpy(*body, f->r[i].body, f->r[i].len + 1);
            *len = f->r[i].len;
            return 200;
        }
    return 404;
}
static int offline_get(void *ctx, const char *url, size_t max, char **body, size_t *len)
{
    (void)ctx; (void)url; (void)max; (void)body; (void)len;
    return -12007;   /* ERROR_WINHTTP_NAME_NOT_RESOLVED */
}

/* ------------------------------------------------------------------ fixtures */
#define B1151 "6af3094f-60fb000"
#define B1152 "7b000000-6100000"
#define BR1151 "release/1.15.1"
#define BR1152 "release/1.15.2"

typedef struct {
    const char *tag;
    int draft, pre;
    int api;
    const char *builds;    /* JSON array body */
    int compat;            /* 0 = no compat.json asset, 1 = good, 2 = HTTP 500, 3 = no sha (SHA256SUMS), 4 = no sha at all */
    const char *compat_ver;   /* NULL = tag without v */
    int sign;              /* 0 = no compat.json.sig, 1 = signed with the test key, 2 = bad signature,
                              3 = signed with an unknown key, 4 = .sig listed but HTTP 404;
                              signed releases use make_dist.py's array "assets" layout */
} relspec;

/* signing with the public test key (or a random "unknown" key) */
static void b64(const uint8_t *in, size_t n, char *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[k++] = t[v >> 18 & 63]; out[k++] = t[v >> 12 & 63];
        out[k++] = i + 1 < n ? t[v >> 6 & 63] : '=';
        out[k++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[k] = 0;
}
static void sign_with(const uint8_t seed32[32], const char *msg, size_t n, char *line, size_t cap)
{
    uint8_t seed[32], sk[64], pk[32], sig[64];
    char kid[17], s64[100];
    memcpy(seed, seed32, 32);
    crypto_ed25519_key_pair(sk, pk, seed);
    crypto_ed25519_sign(sig, sk, (const uint8_t *)msg, n);
    v3x_sig_key_id_(pk, kid);
    b64(sig, 64, s64);
    snprintf(line, cap, "v3x-sig 1 %s %s\n", kid, s64);
}
static void test_seed(uint8_t out[32])
{
    up_sha256 s;
    up_sha256_init(&s);
    up_sha256_update(&s, TEST_SEED_TEXT, strlen(TEST_SEED_TEXT));
    up_sha256_final(&s, out);
}
static void sign_test(const char *msg, size_t n, char *line, size_t cap)
{
    uint8_t seed[32];
    test_seed(seed);
    sign_with(seed, msg, n, line, cap);
}

static char g_list[65536];
static void setup_body(const char *tag, char *out, size_t cap) { snprintf(out, cap, "FAKE V3X SETUP %s", tag); }

/* Builds the GitHub list + compat.json + setup routes for specs. */
static void make_world(fake *f, const relspec *s, int n)
{
    size_t k = 0;
    k += (size_t)snprintf(g_list + k, sizeof g_list - k, "[");
    for (int i = 0; i < n; i++) {
        char base[256], setup[64], sha[65], cj[2048], url[400];
        snprintf(base, sizeof base, UP_ASSET_PREFIX "%s/", s[i].tag);
        setup_body(s[i].tag, setup, sizeof setup);
        up_sha256_hex(setup, strlen(setup), sha);
        k += (size_t)snprintf(g_list + k, sizeof g_list - k,
                              "%s{\"url\":\"x\",\"tag_name\":\"%s\",\"name\":\"V3X %s\",\"draft\":%s,\"prerelease\":%s,"
                              "\"author\":{\"login\":\"k\",\"id\":1,\"x\":[1,2,{\"y\":null}]},\"body\":\"line\\n\\\"quoted\\\" \\u00e9\","
                              "\"assets\":[{\"name\":\"V3X-Setup.exe\",\"size\":%zu,\"browser_download_url\":\"%sV3X-Setup.exe\"},"
                              "{\"name\":\"evil.exe\",\"size\":1,\"browser_download_url\":\"https://evil.example/x.exe\"},"
                              "{\"name\":\"SHA256SUMS.txt\",\"size\":100,\"browser_download_url\":\"%sSHA256SUMS.txt\"}%s%s%s%s%s%s]}",
                              i ? "," : "", s[i].tag, s[i].tag, s[i].draft ? "true" : "false", s[i].pre ? "true" : "false",
                              strlen(setup), base, base, s[i].compat ? ",{\"name\":\"compat.json\",\"size\":10,\"browser_download_url\":\"" : "",
                              s[i].compat ? base : "", s[i].compat ? "compat.json\"}" : "",
                              s[i].sign ? ",{\"name\":\"compat.json.sig\",\"size\":120,\"browser_download_url\":\"" : "",
                              s[i].sign ? base : "", s[i].sign ? "compat.json.sig\"}" : "");
        const char *ver = s[i].compat_ver ? s[i].compat_ver : s[i].tag + 1;
        if (s[i].sign)   /* installer/make_dist.py layout */
            snprintf(cj, sizeof cj,
                     "{\n  \"schema\": 1,\n  \"v3x_version\": \"%s\",\n  \"api_version\": %d,\n  \"game_builds\": [%s],\n"
                     "  \"assets\": [{\"name\": \"V3X-Setup.exe\", \"size\": %zu, \"sha256\": \"%s\"},"
                     " {\"name\": \"install.ps1\", \"size\": 5, \"sha256\": \"%064d\"}]\n}\n",
                     ver, s[i].api, s[i].builds, strlen(setup), sha, 0);
        else
            snprintf(cj, sizeof cj,
                     "{\"v3x_version\":\"%s\",\"loader_semver\":\"%s\",\"api_version\":%d,\"game_builds\":[%s],"
                     "\"assets\":{\"setup\":\"V3X-Setup.exe\",\"zip\":\"v3x.zip\",\"install_ps1\":\"install.ps1\",\"sha256\":{%s%s%s}}}",
                     ver, ver, s[i].api, s[i].builds, s[i].compat == 3 || s[i].compat == 4 ? "" : "\"V3X-Setup.exe\":\"",
                     s[i].compat == 3 || s[i].compat == 4 ? "\"install.ps1\":\"" "0000000000000000000000000000000000000000000000000000000000000000\"" : sha,
                     s[i].compat == 3 || s[i].compat == 4 ? "" : "\"");
        snprintf(url, sizeof url, "%scompat.json", base);
        if (s[i].compat == 2) route_add(f, url, 500, NULL, 0);
        else if (s[i].compat) route_add(f, url, 200, cj, strlen(cj));
        if (s[i].sign) {
            char line[160];
            if (s[i].sign == 3) {
                uint8_t other[32];
                for (int b = 0; b < 32; b++) other[b] = (uint8_t)(b * 7 + 1);
                sign_with(other, cj, strlen(cj), line, sizeof line);
            } else {
                sign_test(cj, strlen(cj), line, sizeof line);
            }
            if (s[i].sign == 2) line[40] = line[40] == 'A' ? 'B' : 'A';   /* one base64 digit of the signature */
            snprintf(url, sizeof url, "%scompat.json.sig", base);
            if (s[i].sign == 4) route_add(f, url, 404, NULL, 0);
            else route_add(f, url, 200, line, strlen(line));
        }
        snprintf(url, sizeof url, "%sV3X-Setup.exe", base);
        route_add(f, url, 200, setup, strlen(setup));
        if (s[i].compat == 3) {
            char sums[256];
            snprintf(sums, sizeof sums, "%s  v3x.zip\r\n%s *V3X-Setup.exe\r\n", "1111111111111111111111111111111111111111111111111111111111111111", sha);
            snprintf(url, sizeof url, "%sSHA256SUMS.txt", base);
            route_add(f, url, 200, sums, strlen(sums));
        }
    }
    k += (size_t)snprintf(g_list + k, sizeof g_list - k, "]");
    route_add(f, UP_REPO_API, 200, g_list, k);
}

#define BLD(br, id) "{\"branch\":\"" br "\",\"build_id\":\"" id "\"}"
#define BRANCH(br) "{\"branch\":\"" br "\"}"

static const relspec WORLD[] = {
    {"v0.5.0", 0, 0, 5, BLD(BR1151, B1151), 1, NULL},
    {"v0.5.1", 0, 0, 5, BLD(BR1151, B1151), 1, NULL},
    {"v0.5.2", 0, 0, 5, BRANCH(BR1151), 1, NULL},                 /* branch-only entry */
    {"v0.6.0", 0, 0, 5, BLD(BR1152, B1152), 1, NULL},             /* 1.15.2 only */
    {"v0.6.1-rc1", 0, 1, 5, BLD(BR1152, B1152), 1, NULL},         /* pre-release */
    {"v0.7.0", 1, 0, 5, BLD(BR1151, B1151) "," BLD(BR1152, B1152), 1, NULL},   /* draft */
    {"nightly", 0, 0, 5, BLD(BR1151, B1151), 1, NULL},            /* not a version tag */
};

static up_catalog cat;

static up_choice run_sel(const relspec *w, int n, const char *ver, const char *build, const char *branch, int beta,
                         int supported, const char *want, const char *name)
{
    fake f;
    memset(&f, 0, sizeof f);
    make_world(&f, w, n);
    up_local l = {ver, 5, build, branch, beta, supported};
    char err[160];
    int rc = up_fetch_catalog(fake_get, &f, &l, 1000, &cat, err, sizeof err);
    up_choice c = up_select(&cat, &l);
    const char *got = c.best >= 0 ? cat.r[c.best].version : "-";
    CHECK(rc == 0 && !strcmp(got, want) && !f.bad_urls, "%s: picks %s (got %s, state %s, %d http calls)", name, want, got,
          up_state_name(c.state), f.calls);
    fake_free(&f);
    return c;
}

/* ------------------------------------------------------------------ tests */

static void t_sha(void)
{
    char h[65];
    up_sha256_hex("", 0, h);
    CHECK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "sha256(\"\")");
    up_sha256_hex("abc", 3, h);
    CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256(abc)");
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    up_sha256_hex(m, strlen(m), h);
    CHECK(!strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "sha256(448-bit vector)");
    char *a = (char *)malloc(1000000);
    memset(a, 'a', 1000000);
    up_sha256_hex(a, 1000000, h);
    CHECK(!strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "sha256(10^6 x 'a')");
    up_sha256 s;
    uint8_t d[32];
    char h2[65];
    up_sha256_init(&s);
    for (size_t off = 0, step = 1; off < 1000000; off += step, step = step * 3 % 131 + 1)
        up_sha256_update(&s, a + off, off + step > 1000000 ? 1000000 - off : step);
    up_sha256_final(&s, d);
    to_hex(d, h2);
    CHECK(!strcmp(h, h2), "sha256 streamed in odd chunks == one shot");
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof tmp, "%s\\upd_sha_%lu.bin", getenv("TEMP") ? getenv("TEMP") : ".", GetCurrentProcessId());
    FILE *f = fopen(tmp, "wb");
    fwrite(a, 1, 1000000, f);
    fclose(f);
    CHECK(up_sha256_file(tmp, h2) == 0 && !strcmp(h, h2), "sha256 of a file");
    DeleteFileA(tmp);
    CHECK(up_sha256_file(tmp, h2) == -1, "sha256 of a missing file fails");
    free(a);
    CHECK(up_hex_valid(h) && !up_hex_valid("abc") && !up_hex_valid(NULL), "hex_valid");
    char up[65];
    for (int i = 0; i < 65; i++) up[i] = (char)toupper((unsigned char)h[i]);
    CHECK(up_hex_eq(h, up) && !up_hex_eq(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") &&
              !up_hex_eq("zz", "zz"), "hex_eq case-insensitive, invalid never equal");
}

static void t_semver(void)
{
    const char *order[] = {"0.4.9", "0.5.0", "v0.5.1", "0.6.0-alpha", "0.6.0-alpha.1", "0.6.0-alpha.beta", "0.6.0-beta",
                           "0.6.0-beta.2", "0.6.0-beta.11", "0.6.0-rc.1", "0.6.0-rc1", "0.6.0", "0.10.0", "1.0.0"};
    int n = (int)(sizeof order / sizeof *order), ok = 1;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            int c = up_version_cmp(order[i], order[j]);
            if ((i < j && c >= 0) || (i > j && c <= 0) || (i == j && c)) { ok = 0; printf("  order %s vs %s = %d\n", order[i], order[j], c); }
        }
    CHECK(ok, "semver precedence over %d versions (pre-releases < release, numeric ids, 0.10 > 0.6)", n);
    CHECK(up_version_cmp("1.2.3+build.5", "1.2.3") == 0 && up_version_cmp("V1.2.3", "1.2.3") == 0, "build metadata / V prefix ignored");
    const char *bad[] = {"", "1", "1.2", "1.2.3.4", "a.b.c", "1.2.3-", "1.2.3-a..b", "1.2.3-.a", "1.2.3+", "1.2.x", "-1.2.3",
                         "1.2.3 ", "1.2.3-a_b", "1234567890.0.0"};
    ok = 1;
    up_semver v;
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        if (up_semver_parse(bad[i], &v) == 0) { ok = 0; printf("  accepted %s\n", bad[i]); }
    CHECK(ok, "invalid versions rejected");
    CHECK(up_version_cmp("junk", "0.0.1") < 0 && up_version_cmp("0.0.1", "junk") > 0, "unparsable sorts lowest");
}

static int parse_ok(const char *s)
{
    const char *e = s + strlen(s), *x = up_js_skip(s, e);
    return x && up_js_ws(x, e) == e;
}

static void t_json(void)
{
    CHECK(parse_ok("{\"a\":[1,-2.5e+3,true,false,null,\"x\\\"y\",{}],\"b\":{}}") && parse_ok("  [ ]  ") && parse_ok("\"\""),
          "valid JSON skips");
    const char *bad[] = {"", "{", "[1,]", "{\"a\":}", "{\"a\" 1}", "{a:1}", "[1 2]", "\"abc", "\"a\\x\"", "\"\x01\"", "[01x]",
                         "tru", "{\"a\":1,}", "[}", "{]", "-", "1.", "1e", "\"\\u12G4\"", "[\"a\"]]"};
    int ok = 1;
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        if (parse_ok(bad[i])) { ok = 0; printf("  accepted %s\n", bad[i]); }
    CHECK(ok, "malformed JSON rejected (%zu cases)", sizeof bad / sizeof *bad);
    char deep[5000];
    memset(deep, '[', 2500);
    memset(deep + 2500, ']', 2500);
    CHECK(!up_js_skip(deep, deep + 5000), "nesting 2500 deep = malformed (no stack overflow)");
    char out[64];
    const char *s = "\"a\\n\\t\\/\\u00e9\\u20ac\\ud83d\\ude00\\u0000\\ud800x\"";
    int n = up_js_string(s, s + strlen(s), out, sizeof out);
    CHECK(n > 0 && !strcmp(out, "a\n\t/\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80??x"), "string escapes, UTF-8, surrogates, \\u0000 -> ?");
    const char *ls = "\"0123456789\"";
    CHECK(up_js_string(ls, ls + strlen(ls), out, 5) == -1 && out[0] == 0, "string that does not fit = -1 (no truncation)");
    const char *o = "{\"a\":1,\"b\":{\"a\":2},\"c\":\"z\",\"a\":3}";
    const char *v;
    long long x = 0;
    CHECK(up_js_get(o, o + strlen(o), "a", &v) == 1 && up_js_int(v, o + strlen(o), &x) == 0 && x == 1, "get: first key wins, nested ignored");
    CHECK(up_js_get(o, o + strlen(o), "zz", &v) == 0, "get: missing key");
    const char *ob = "{\"a\":1,\"b\":[}";
    CHECK(up_js_get(ob, ob + strlen(ob), "a", &v) == -1, "get on a malformed object = -1");
    const char *big = "123456789012345678901234567890";
    CHECK(up_js_int(big, big + strlen(big), &x) == -1, "integer overflow rejected");
}

static void t_fuzz(void)
{
    fake f;
    memset(&f, 0, sizeof f);
    make_world(&f, WORLD, (int)(sizeof WORLD / sizeof *WORLD));
    size_t n = strlen(g_list);
    char *buf = (char *)malloc(n + 1);
    int bad = 0, full = -2;
    for (size_t k = 0; k <= n; k++) {   /* every truncation */
        memcpy(buf, g_list, k);
        buf[k] = 0;
        int r = up_parse_releases(buf, k, &cat);
        if (k == n) full = r;
        else if (r >= 0 && k < n - 1) bad++;   /* only the complete text (or one missing ']') ... */
        if (cat.n > UP_MAX_REL) bad += 1000;
    }
    CHECK(full == 6 && bad == 0, "release list: all %zu truncations rejected without crash, full text = 6 releases (got %d, bad %d)", n,
          full, bad);
    /* random mutations: no crash, catalog stays sane */
    unsigned seed = 12345;
    int sane = 1, parsed = 0;
    for (int it = 0; it < 20000; it++) {
        memcpy(buf, g_list, n + 1);
        int muts = 1 + (int)(seed % 4);
        for (int m = 0; m < muts; m++) {
            seed = seed * 1103515245u + 12345u;
            size_t at = (seed >> 8) % n;
            static const char pool[] = "{}[]\",:\\ 0a-tfnu\x01\xff";
            seed = seed * 1103515245u + 12345u;
            buf[at] = pool[(seed >> 16) % (sizeof pool - 1)];
        }
        int r = up_parse_releases(buf, n, &cat);
        if (r >= 0) parsed++;
        for (int i = 0; i < cat.n; i++)
            if (memchr(cat.r[i].tag, 0, sizeof cat.r[i].tag) == NULL || memchr(cat.r[i].assets[0].url, 0, 400) == NULL ||
                cat.r[i].nassets > UP_MAX_ASSETS || (cat.r[i].setup_url[0] && !up_url_allowed(cat.r[i].setup_url)))
                sane = 0;
        if (cat.n > UP_MAX_REL) sane = 0;
        up_release r1;
        memset(&r1, 0, sizeof r1);
        snprintf(r1.version, sizeof r1.version, "0.5.0");
        up_parse_compat(buf, n, &r1);   /* not compat.json at all: must fail cleanly */
        if (r1.compat == 1) sane = 0;
        static up_catalog c2;
        up_cache_read(buf, n, &c2);
    }
    CHECK(sane, "20000 random mutations of the release list: no crash, bounded catalog (%d still parsed)", parsed);
    free(buf);
    /* truncated / mutated compat.json */
    const char *cj = "{\"v3x_version\":\"0.6.0\",\"api_version\":5,\"game_builds\":[{\"branch\":\"release/1.15.2\",\"build_id\":\"x\"}],"
                     "\"assets\":{\"setup\":\"V3X-Setup.exe\",\"sha256\":{\"V3X-Setup.exe\":\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\"}}}";
    size_t cn = strlen(cj);
    int good = 0, wrong = 0;
    for (size_t k = 0; k <= cn; k++) {
        up_release r1;
        memset(&r1, 0, sizeof r1);
        snprintf(r1.version, sizeof r1.version, "0.6.0");
        int rc = up_parse_compat(cj, k, &r1);
        if (k == cn) good = rc == 0 && r1.compat == 1;
        else if (rc == 0) wrong++;
    }
    CHECK(good && !wrong, "compat.json: every truncation rejected, full text accepted");
    fake_free(&f);
}

static void t_parse(void)
{
    fake f;
    memset(&f, 0, sizeof f);
    make_world(&f, WORLD, (int)(sizeof WORLD / sizeof *WORLD));
    int n = up_parse_releases(g_list, strlen(g_list), &cat);
    CHECK(n == 6, "release list: 6 version tags ('nightly' skipped), got %d", n);
    int pre = -1, draft = -1, evil = 0;
    for (int i = 0; i < cat.n; i++) {
        if (!strcmp(cat.r[i].tag, "v0.6.1-rc1")) pre = cat.r[i].prerelease;
        if (!strcmp(cat.r[i].tag, "v0.7.0")) draft = cat.r[i].draft;
        for (int j = 0; j < cat.r[i].nassets; j++) evil += strstr(cat.r[i].assets[j].url, "evil") != NULL;
    }
    CHECK(pre == 1 && draft == 1 && !evil, "prerelease / draft flags; foreign asset URL dropped");
    CHECK(cat.r[0].compat_url[0] && cat.r[0].sums_url[0] && !strcmp(cat.r[0].version, "0.5.0"), "compat.json + SHA256SUMS urls, version from tag");

    up_release r = cat.r[3];   /* v0.6.0 */
    const char *cj = "\xEF\xBB\xBF{\"v3x_version\":\"0.6.0\",\"api_version\":5,\"game_builds\":[{\"branch\":\"release/1.15.2\",\"build_id\":\"" B1152
                     "\"},{\"other\":1},7],\"assets\":{\"setup\":\"V3X-Setup.exe\",\"sha256\":{\"V3X-Setup.exe\":\""
                     "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855\"}}}";
    CHECK(up_parse_compat(cj, strlen(cj), &r) == 0 && r.nbuilds == 1 && r.api_version == 5 && r.setup_url[0] &&
              up_hex_valid(r.setup_sha), "compat.json with BOM, junk build entries ignored, setup resolved");
    struct { const char *json, *why; } badc[] = {
        {"{\"v3x_version\":\"0.6.1\",\"api_version\":5,\"game_builds\":[]}", "version does not match the tag"},
        {"{\"v3x_version\":\"0.6.0\",\"game_builds\":[]}", "no api_version"},
        {"{\"v3x_version\":\"0.6.0\",\"api_version\":5}", "no game_builds"},
        {"{\"v3x_version\":\"0.6.0\",\"api_version\":5,\"game_builds\":[],\"assets\":{\"sha256\":{\"V3X-Setup.exe\":\"abc\"}}}", "bad sha256"},
        {"{\"v3x_version\":\"0.6.0\",\"api_version\":5,\"game_builds\":[],\"assets\":{\"setup\":\"..\\\\x.exe\"}}", "bad setup name"},
        {"[1]", "malformed"},
        {"{\"v3x_version\":\"0.6.0\",\"api_version\":5,\"game_builds\":[1,}", "malformed"},
    };
    int ok = 1;
    for (size_t i = 0; i < sizeof badc / sizeof *badc; i++) {
        up_release x = cat.r[3];
        if (up_parse_compat(badc[i].json, strlen(badc[i].json), &x) != -1 || x.compat != -1) { ok = 0; printf("  accepted: %s\n", badc[i].why); }
    }
    CHECK(ok, "invalid compat.json rejected (%zu cases, fail closed)", sizeof badc / sizeof *badc);
    char hex[65];
    const char *sums = "aaaa  short\n1111111111111111111111111111111111111111111111111111111111111111  v3x.zip\r\n"
                       "ABCDEF0000000000000000000000000000000000000000000000000000000000 *V3X-Setup.exe\r\n";
    CHECK(up_sums_find(sums, strlen(sums), "V3X-Setup.exe", hex) == 0 &&
              !strcmp(hex, "abcdef0000000000000000000000000000000000000000000000000000000000") &&
              up_sums_find(sums, strlen(sums), "V3X-Setup", hex) == -1, "SHA256SUMS lookup (binary marker, CRLF, exact name)");
    fake_free(&f);
}

static void t_select(void)
{
    const int N = (int)(sizeof WORLD / sizeof *WORLD);
    up_choice c;
    /* the lead's case: game still 1.15.1 -> never the 1.15.2-only release */
    c = run_sel(WORLD, N, "0.5.0", B1151, BR1151, 0, 1, "0.5.2", "local 0.5.0 on 1.15.1");
    CHECK(c.state == UP_AVAILABLE && c.newest >= 0 && !strcmp(cat.r[c.newest].version, "0.6.0"),
          "  state available, newest known = 0.6.0 (not offered)");
    char text[600];
    up_local l1 = {"0.5.0", 5, B1151, BR1151, 0, 1};
    up_status_text(&cat, &l1, c, text, sizeof text);
    CHECK(strstr(text, "0.5.2 is available") && strstr(text, "0.6.0 exists too") && strstr(text, BR1152), "  text: %s", text);
    c = run_sel(WORLD, N, "0.5.2", B1151, BR1151, 0, 1, "-", "local 0.5.2 on 1.15.1: only 1.15.2 releases are newer");
    CHECK(c.state == UP_NEWER_INCOMPATIBLE && c.newest >= 0, "  state newer-incompatible");
    up_local l2 = {"0.5.2", 5, B1151, BR1151, 0, 1};
    up_status_text(&cat, &l2, c, text, sizeof text);
    CHECK(strstr(text, "does NOT support your game") && strstr(text, "0.6.0"), "  text: %s", text);
    /* the game became 1.15.2 -> the 1.15.2 release, and it is urgent (plugins gated off) */
    c = run_sel(WORLD, N, "0.5.2", B1152, BR1152, 0, 0, "0.6.0", "local 0.5.2, game now 1.15.2 (unknown to the loader)");
    CHECK(c.state == UP_URGENT, "  state urgent");
    up_local l3 = {"0.5.2", 5, B1152, BR1152, 0, 0};
    up_status_text(&cat, &l3, c, text, sizeof text);
    CHECK(strstr(text, "switched off") && strstr(text, "0.6.0 supports it"), "  text: %s", text);
    c = run_sel(WORLD, N, "0.5.0", B1152, BR1152, 0, 1, "0.6.0", "local 0.5.0 on 1.15.2 (loader knows it)");
    CHECK(c.state == UP_AVAILABLE, "  state available");
    /* never downgrade / equal */
    c = run_sel(WORLD, N, "0.6.0", B1151, BR1151, 0, 1, "-", "local 0.6.0 on 1.15.1: 0.5.x are older");
    CHECK(c.state == UP_CURRENT, "  state up-to-date (drafts ignored, pre-release ignored on stable)");
    c = run_sel(WORLD, N, "9.0.0", B1151, BR1151, 1, 1, "-", "local 9.0.0: nothing newer even on beta");
    CHECK(c.state == UP_CURRENT, "  state up-to-date");
    c = run_sel(WORLD, N, "0.6.0", B1152, BR1152, 0, 1, "-", "local 0.6.0 on 1.15.2, stable");
    c = run_sel(WORLD, N, "0.6.0", B1152, BR1152, 1, 1, "0.6.1-rc1", "local 0.6.0 on 1.15.2, channel beta");
    CHECK(c.state == UP_AVAILABLE, "  beta: pre-release offered");
    c = run_sel(WORLD, N, "0.6.1-rc1", B1152, BR1152, 1, 1, "-", "local 0.6.1-rc1, beta: no re-install of itself");
    /* nothing compatible at all */
    c = run_sel(WORLD, N, "0.5.0", "deadbeef-1", "release/1.16.0", 0, 0, "-", "game 1.16.0, no release supports it");
    CHECK(c.state == UP_UNSUPPORTED_NOFIX, "  state unsupported (no fix yet)");
    up_local l4 = {"0.5.0", 5, "deadbeef-1", "release/1.16.0", 0, 0};
    up_status_text(&cat, &l4, c, text, sizeof text);
    CHECK(strstr(text, "no V3X release supports it yet") != NULL, "  text: %s", text);
    /* build_id vs branch: a hotfix keeps the branch name but changes the build id */
    c = run_sel(WORLD, N, "0.5.0", "6af30950-60fb000", BR1151, 0, 0, "0.5.2",
                "hotfix build on branch 1.15.1: only the branch-only entry (0.5.2) matches, not build_id entries");
    /* api / verification */
    relspec w2[] = {
        {"v0.8.0", 0, 0, 4, BLD(BR1151, B1151), 1, NULL},    /* lower API than the running loader */
        {"v0.7.3", 0, 0, 5, BLD(BR1151, B1151), 2, NULL},    /* compat.json HTTP 500 */
        {"v0.7.2", 0, 0, 5, BLD(BR1151, B1151), 0, NULL},    /* no compat.json asset */
        {"v0.7.1", 0, 0, 5, BLD(BR1151, B1151), 4, NULL},    /* no sha for the setup anywhere */
        {"v0.7.0", 0, 0, 5, BLD(BR1151, B1151), 3, NULL},    /* sha only in SHA256SUMS.txt */
        {"v0.6.9", 0, 0, 5, BLD(BR1151, B1151), 1, "0.6.8"}, /* compat.json of another version */
    };
    c = run_sel(w2, 6, "0.5.0", B1151, BR1151, 0, 1, "0.7.0",
                "api v4 release, compat HTTP 500, no compat, no sha: skipped; sha from SHA256SUMS accepted");
    int k = -1;
    for (int i = 0; i < cat.n; i++) if (!strcmp(cat.r[i].version, "0.6.9")) k = i;
    CHECK(k >= 0 && cat.r[k].compat == -1 && strstr(cat.r[k].compat_err, "does not match"), "  mismatched compat.json rejected");
    /* only candidates' compat.json is fetched */
    fake f;
    memset(&f, 0, sizeof f);
    make_world(&f, WORLD, N);
    up_local l5 = {"0.5.2", 5, B1151, BR1151, 0, 1};
    char err[160];
    up_fetch_catalog(fake_get, &f, &l5, 1, &cat, err, sizeof err);
    CHECK(f.calls == 2 && strstr(f.log, "v0.6.0/compat.json") && !strstr(f.log, "v0.5.1/compat.json") && !strstr(f.log, "v0.7.0"),
          "only newer non-draft releases' compat.json fetched (%d calls)", f.calls);
    fake_free(&f);
    /* offline */
    static up_catalog keep;
    keep = cat;
    int rc = up_fetch_catalog(offline_get, NULL, &l5, 2, &cat, err, sizeof err);
    CHECK(rc == -1 && strstr(err, "network error") && !memcmp(&keep, &cat, sizeof cat), "offline: -1, '%s', catalog untouched", err);
    memset(&f, 0, sizeof f);
    route_add(&f, UP_REPO_API, 403, NULL, 0);
    rc = up_fetch_catalog(fake_get, &f, &l5, 2, &cat, err, sizeof err);
    CHECK(rc == -1 && strstr(err, "HTTP 403"), "rate-limited by GitHub: '%s'", err);
    fake_free(&f);
    route_add(&f, UP_REPO_API, 200, "{\"message\":\"x\"}", 15);
    rc = up_fetch_catalog(fake_get, &f, &l5, 2, &cat, err, sizeof err);
    CHECK(rc == -1 && strstr(err, "malformed"), "non-array answer: '%s'", err);
    fake_free(&f);
    up_local lo = {"0.5.0", 5, B1151, BR1151, 0, 1};
    static up_catalog empty;
    memset(&empty, 0, sizeof empty);
    up_choice off = {UP_OFFLINE, -1, -1};
    up_status_text(&empty, &lo, off, text, sizeof text);
    CHECK(strstr(text, "could not check") != NULL, "offline text: %s", text);
    CHECK(!up_url_allowed("https://github.com/someoneelse/v3x/releases/download/v1/x.exe") &&
              !up_url_allowed("http://github.com/kleeedolinux/v3x/releases/download/v1/x.exe") &&
              !up_url_allowed(UP_ASSET_PREFIX "v1/../../x.exe") && !up_url_allowed(UP_ASSET_PREFIX "v1/a b.exe") &&
              up_url_allowed(UP_ASSET_PREFIX "v0.6.0/V3X-Setup.exe") && up_url_allowed(UP_REPO_API),
          "URL allow-list: only this repository, https, no '..'/spaces");
}

static void t_rate(void)
{
    up_catalog c;
    memset(&c, 0, sizeof c);
    const char *key = "0.5.0|5|" B1151 "|" BR1151 "|stable";
    CHECK(up_should_fetch(&c, key, 1000000, UP_INTERVAL_S), "never checked -> fetch");
    c.attempt_at = 1000000;
    snprintf(c.key, sizeof c.key, "%s", key);
    CHECK(!up_should_fetch(&c, key, 1000000 + 60, UP_INTERVAL_S), "1 min later -> no");
    CHECK(!up_should_fetch(&c, key, 1000000 + UP_INTERVAL_S - 1, UP_INTERVAL_S), "6 h - 1 s -> no");
    CHECK(up_should_fetch(&c, key, 1000000 + UP_INTERVAL_S, UP_INTERVAL_S), "6 h later -> fetch");
    CHECK(up_should_fetch(&c, key, 999000, UP_INTERVAL_S), "clock went back -> fetch");
    CHECK(up_should_fetch(&c, "0.5.0|5|" B1152 "|" BR1152 "|stable", 1000000 + 60, UP_INTERVAL_S),
          "game build changed -> fetch at once");
    up_local l = {"0.5.0", 5, B1151, BR1151, 1, 1};
    char k2[200];
    up_local_key(&l, k2, sizeof k2);
    CHECK(strcmp(k2, key) && up_should_fetch(&c, k2, 1000000 + 60, UP_INTERVAL_S), "channel changed -> fetch (%s)", k2);
    CHECK(up_force_allowed(0, 5) && !up_force_allowed(100, 159) && up_force_allowed(100, 160) && up_force_allowed(100, 50),
          "`update check`: once per minute");
}

static void t_cache(void)
{
    fake f;
    memset(&f, 0, sizeof f);
    make_world(&f, WORLD, (int)(sizeof WORLD / sizeof *WORLD));
    up_local l = {"0.5.0", 5, B1151, BR1151, 1, 1};
    char err[160];
    up_fetch_catalog(fake_get, &f, &l, 1234567, &cat, err, sizeof err);
    snprintf(cat.r[0].compat_err, sizeof cat.r[0].compat_err, "with \"quotes\" \\ and\nnewline");
    static char buf[1 << 18];
    size_t n = up_cache_write(&cat, buf, sizeof buf);
    static up_catalog back;
    int rc = up_cache_read(buf, n, &back);
    int same = rc == 0 && back.n == cat.n && back.checked_at == 1234567 && !strcmp(back.key, cat.key);
    for (int i = 0; same && i < cat.n; i++) {
        const up_release *a = &cat.r[i], *b = &back.r[i];
        same = !strcmp(a->tag, b->tag) && !strcmp(a->version, b->version) && a->draft == b->draft &&
               a->prerelease == b->prerelease && a->compat == b->compat && a->api_version == b->api_version &&
               a->nbuilds == b->nbuilds && !strcmp(a->setup_url, b->setup_url) && !strcmp(a->setup_sha, b->setup_sha) &&
               a->setup_size == b->setup_size && !strcmp(a->compat_err, b->compat_err);
        for (int j = 0; same && j < a->nbuilds; j++)
            same = !strcmp(a->builds[j].branch, b->builds[j].branch) && !strcmp(a->builds[j].build_id, b->builds[j].build_id);
    }
    CHECK(same, "cache round trip (%zu bytes, %d releases, escapes)", n, back.n);
    up_choice c1 = up_select(&cat, &l), c2 = up_select(&back, &l);
    CHECK(c1.state == c2.state && c1.best == c2.best, "selection from the cache == selection from the network");
    CHECK(up_cache_write(&cat, buf, 100) == 0, "cache write into a too small buffer = 0");
    int bad = 0;
    for (size_t k = 0; k < n - 2; k++)
        if (up_cache_read(buf, k, &back) == 0) bad++;
    CHECK(bad == 0, "every truncated cache file rejected");
    const char *wrong = "{\"schema\":2,\"releases\":[]}";
    CHECK(up_cache_read(wrong, strlen(wrong), &back) == -1 && back.n == 0, "unknown cache schema rejected");
    const char *evil = "{\"schema\":1,\"releases\":[{\"tag\":\"v9.0.0\",\"version\":\"9.0.0\",\"draft\":false,\"prerelease\":false,"
                       "\"compat\":1,\"api_version\":5,\"setup_url\":\"https://evil.example/x.exe\",\"setup_sha\":\"zz\","
                       "\"game_builds\":[{\"branch\":\"\",\"build_id\":\"" B1151 "\"}]}]}";
    CHECK(up_cache_read(evil, strlen(evil), &back) == 0 && !back.r[0].setup_url[0] && !back.r[0].setup_sha[0] &&
              up_select(&back, &l).best == -1, "an edited cache cannot point the download elsewhere");
    fake_free(&f);
}

static void t_download(void)
{
    fake f;
    memset(&f, 0, sizeof f);
    make_world(&f, WORLD, (int)(sizeof WORLD / sizeof *WORLD));
    up_local l = {"0.5.0", 5, B1151, BR1151, 0, 1};
    char err[160], *data;
    size_t n;
    up_fetch_catalog(fake_get, &f, &l, 1, &cat, err, sizeof err);
    up_choice c = up_select(&cat, &l);
    up_release r = cat.r[c.best];
    CHECK(up_download_setup(fake_get, &f, &r, &data, &n, err, sizeof err) == 0 && n == strlen("FAKE V3X SETUP v0.5.2") &&
              !memcmp(data, "FAKE V3X SETUP v0.5.2", n), "download + SHA-256 verified");
    free(data);
    up_release t = r;
    t.setup_sha[0] = t.setup_sha[0] == 'a' ? 'b' : 'a';
    CHECK(up_download_setup(fake_get, &f, &t, &data, &n, err, sizeof err) == -1 && !data && strstr(err, "SHA-256 mismatch"),
          "wrong hash -> refused (%s)", err);
    t = r;
    t.setup_size += 1;
    CHECK(up_download_setup(fake_get, &f, &t, &data, &n, err, sizeof err) == -1 && strstr(err, "size mismatch"), "wrong size -> refused");
    t = r;
    t.setup_sha[0] = 0;
    CHECK(up_download_setup(fake_get, &f, &t, &data, &n, err, sizeof err) == -1, "no hash -> refused (never unverified)");
    t = r;
    snprintf(t.setup_url, sizeof t.setup_url, UP_ASSET_PREFIX "v0.5.2/missing.exe");
    CHECK(up_download_setup(fake_get, &f, &t, &data, &n, err, sizeof err) == -1 && strstr(err, "404"), "HTTP 404 -> refused");
    t = r;
    snprintf(t.setup_url, sizeof t.setup_url, "https://evil.example/V3X-Setup.exe");
    int calls = f.calls;
    CHECK(up_download_setup(fake_get, &f, &t, &data, &n, err, sizeof err) == -1 && f.calls == calls, "foreign URL never requested");
    fake_free(&f);
}

/* release signatures (H1), rollback floor and cache trust (M4) */
static void t_sig(void)
{
    const char *msg = "{\"v3x_version\":\"1.0.0\"}\n";
    char line[160], kid[17];
    const char *why = "";
    sign_test(msg, strlen(msg), line, sizeof line);
    CHECK(up_sig_check(msg, strlen(msg), line, strlen(line), kid, &why) == V3X_SIG_SLOT_DEVTEST && !strcmp(kid, "c91e9e3c88025a36"),
          "sig: test-key signature verifies (key id %s = tools/sign_release.py TEST key)", kid);
    char m2[64];
    snprintf(m2, sizeof m2, "%s", msg);
    m2[3] ^= 1;
    CHECK(up_sig_check(m2, strlen(m2), line, strlen(line), kid, &why) < 0 && !strcmp(why, "BAD signature"), "sig: one changed byte -> %s", why);
    char l2[160];
    snprintf(l2, sizeof l2, "%s", line);
    l2[40] = l2[40] == 'A' ? 'B' : 'A';
    CHECK(up_sig_check(msg, strlen(msg), l2, strlen(l2), kid, &why) < 0, "sig: changed signature digit -> %s", why);
    const char *bad[] = {"", "v3x-sig 1 c91e9e3c88025a36", "v3x-sig 2 c91e9e3c88025a36 AAAA==", "garbage\n",
                         "v3x-sig 1 C91E9E3C88025A36 AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=="};
    int ok = 1;
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        if (up_sig_check(msg, strlen(msg), bad[i], strlen(bad[i]), kid, &why) >= 0) ok = 0;
    CHECK(ok, "sig: malformed signature files rejected (%zu cases)", sizeof bad / sizeof *bad);
    uint8_t other[32];
    for (int b = 0; b < 32; b++) other[b] = (uint8_t)(b * 7 + 1);
    sign_with(other, msg, strlen(msg), l2, sizeof l2);
    CHECK(up_sig_check(msg, strlen(msg), l2, strlen(l2), kid, &why) < 0 && strstr(why, "unknown key"), "sig: another key -> %s", why);
    CHECK(up_sig_keys() >= 1, "sig: %d trusted key(s) in this test build (release_keys.h + test key)", up_sig_keys());
    for (int i = 0, mut = 0; i < 2000; i++) {   /* random signature lines never crash, never verify */
        char fz[160];
        snprintf(fz, sizeof fz, "%s", line);
        size_t n = strlen(fz);
        unsigned sd = (unsigned)i * 2654435761u;
        fz[sd % n] = (char)(sd >> 8);
        if (fz[sd % n] == line[sd % n]) continue;
        if (up_sig_check(msg, strlen(msg), fz, n, kid, &why) >= 0) mut++;
        if (i == 1999) CHECK(mut == 0, "sig: 2000 one-byte mutations of the .sig line: none verifies");
    }

    /* signed world: hashes from the signed compat.json (make_dist layout), auto only when signed */
    relspec w[] = {
        {"v0.9.0", 0, 0, 5, BLD(BR1151, B1151), 1, NULL, 1},   /* signed */
        {"v0.8.0", 0, 0, 5, BLD(BR1151, B1151), 1, NULL, 0},   /* unsigned (older draft layout) */
        {"v0.9.5", 0, 0, 5, BLD(BR1151, B1151), 1, NULL, 2},   /* bad signature */
        {"v0.9.6", 0, 0, 5, BLD(BR1151, B1151), 1, NULL, 3},   /* unknown key */
        {"v0.9.7", 0, 0, 5, BLD(BR1151, B1151), 1, NULL, 4},   /* .sig listed, HTTP 404 */
    };
    fake f;
    memset(&f, 0, sizeof f);
    make_world(&f, w, 5);
    up_local l = {"0.5.0", 5, B1151, BR1151, 0, 1};
    char err[160];
    int rc = up_fetch_catalog(fake_get, &f, &l, 1000, &cat, err, sizeof err);
    up_choice c = up_select(&cat, &l);
    int i90 = -1, i80 = -1, i95 = -1, i96 = -1, i97 = -1;
    for (int i = 0; i < cat.n; i++) {
        if (!strcmp(cat.r[i].version, "0.9.0")) i90 = i;
        if (!strcmp(cat.r[i].version, "0.8.0")) i80 = i;
        if (!strcmp(cat.r[i].version, "0.9.5")) i95 = i;
        if (!strcmp(cat.r[i].version, "0.9.6")) i96 = i;
        if (!strcmp(cat.r[i].version, "0.9.7")) i97 = i;
    }
    CHECK(rc == 0 && i90 >= 0 && cat.r[i90].sig == 1 && up_release_auto_ok(&cat.r[i90]) && up_hex_valid(cat.r[i90].setup_sha) &&
              cat.r[i90].setup_size > 0, "signed 0.9.0: sig=1, hash + size from the signed compat.json (array assets)");
    CHECK(i80 >= 0 && cat.r[i80].compat == 1 && cat.r[i80].sig == 0 && !up_release_auto_ok(&cat.r[i80]),
          "unsigned 0.8.0: parsed, sig=0, never automatic");
    CHECK(i95 >= 0 && cat.r[i95].compat == -1 && strstr(cat.r[i95].compat_err, "BAD signature"), "bad signature 0.9.5: rejected (%s)",
          i95 >= 0 ? cat.r[i95].compat_err : "?");
    CHECK(i96 >= 0 && cat.r[i96].compat == -1 && strstr(cat.r[i96].compat_err, "unknown key"), "unknown key 0.9.6: rejected (%s)",
          i96 >= 0 ? cat.r[i96].compat_err : "?");
    CHECK(i97 >= 0 && cat.r[i97].compat == -1 && strstr(cat.r[i97].compat_err, "sig"), "listed .sig not downloadable 0.9.7: rejected, not 'unsigned' (%s)",
          i97 >= 0 ? cat.r[i97].compat_err : "?");
    CHECK(c.best == i90 && c.state == UP_AVAILABLE, "selection: the signed 0.9.0 (bad/unknown/missing-sig newer ones skipped)");
    /* cache: signed state survives only by re-verification */
    static char buf[1 << 20];
    size_t n = up_cache_write(&cat, buf, sizeof buf);
    static up_catalog back;
    CHECK(n && up_cache_read(buf, n, &back) == 0 && back.r[i90].sig == 1 && up_hex_eq(back.r[i90].setup_sha, cat.r[i90].setup_sha) &&
              back.r[i80].sig == 0, "cache round trip: signed release re-verified (sig=1), unsigned stays sig=0");
    /* an edited cache: other hash for the signed release -> the signed compat.json's hash wins */
    char *hp = strstr(buf, cat.r[i90].setup_sha);
    if (hp) hp[0] = hp[0] == 'a' ? 'b' : 'a';
    CHECK(hp && up_cache_read(buf, n, &back) == 0 && up_hex_eq(back.r[i90].setup_sha, cat.r[i90].setup_sha),
          "cache edit of setup_sha of a signed release ignored (taken from the signed compat.json)");
    /* an edited cache: changed signed text -> no longer signed, not eligible */
    char *rp = strstr(buf, "\"compat_raw\":");
    if (rp) { rp = strstr(rp, "api_version"); if (rp) rp[0] = 'A'; }
    CHECK(rp && up_cache_read(buf, n, &back) == 0 && back.r[i90].sig == 0 && back.r[i90].compat == -1,
          "cache edit inside the signed compat.json -> signature fails, release rejected");
    const char *ev = "{\"schema\":1,\"releases\":[{\"tag\":\"v9.0.0\",\"version\":\"9.0.0\",\"draft\":false,\"prerelease\":false,"
                     "\"compat\":1,\"sig\":1,\"api_version\":5,\"setup_name\":\"..\\\\..\\\\evil.exe\",\"setup_url\":\"" UP_ASSET_PREFIX
                     "v9.0.0/V3X-Setup.exe\",\"setup_sha\":\"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\","
                     "\"game_builds\":[{\"branch\":\"\",\"build_id\":\"" B1151 "\"}]}]}";
    CHECK(up_cache_read(ev, strlen(ev), &back) == 0 && back.r[0].sig == 0 && !up_release_auto_ok(&back.r[0]) &&
              !strcmp(back.r[0].setup_name, UP_SETUP_NAME), "cache saying sig=1 without a signed text: unsigned; setup_name '..\\..\\evil.exe' -> %s",
          back.r[0].setup_name);
    /* rollback floor */
    up_local lf = {"0.5.0", 5, B1151, BR1151, 0, 1, "0.9.1"};
    up_choice cf = up_select(&cat, &lf);
    CHECK(cf.best < 0, "floor 0.9.1: the signed 0.9.0 is below the floor -> not offered (state %s)", up_state_name(cf.state));
    up_local lg = {"0.5.0", 5, B1151, BR1151, 0, 1, "0.9.0"};
    CHECK(up_select(&cat, &lg).best == i90, "floor 0.9.0: 0.9.0 itself is still allowed");
    CHECK(up_name_ok("V3X-Setup.exe") && !up_name_ok("..") && !up_name_ok("a/b.exe") && !up_name_ok("a\\b.exe") && !up_name_ok("C:x.exe") &&
              !up_name_ok("") && !up_name_ok("x.exe."), "setup names: only plain file names");
    fake_free(&f);
}

static void t_cmdline(void)
{
    char root[MAX_PATH];
    CHECK(up_game_root("D:\\Steam\\steamapps\\common\\Victoria 3\\binaries\\", root, sizeof root) == 0 &&
              !strcmp(root, "D:\\Steam\\steamapps\\common\\Victoria 3"), "game root from game_dir(): %s", root);
    CHECK(up_game_root("D:/x/Binaries", root, sizeof root) == 0 && !strcmp(root, "D:/x"), "game root, / and case");
    CHECK(up_game_root("D:\\x\\mybinaries\\", root, sizeof root) == 0 && !strcmp(root, "D:\\x\\mybinaries"), "only a whole 'binaries' component");
    CHECK(up_game_root("D:\\binaries\\", root, sizeof root) == -1 && up_game_root("", root, sizeof root) == -1, "never a bare drive / empty");
    const char *exe = "D:\\Steam Library\\Victoria 3\\binaries\\v3x\\update\\V3X-Setup.exe";
    const char *gr[] = {"D:\\Steam Library\\steamapps\\common\\Victoria 3", "C:\\odd\\\"quote\" dir\\\\", "C:\\nospace", "C:\\trail\\"};
    int ok = 1;
    for (size_t i = 0; i < sizeof gr / sizeof *gr; i++) {
        char cmd[2048];
        if (up_build_cmdline(cmd, sizeof cmd, exe, gr[i], 4242)) { ok = 0; continue; }
        wchar_t w[2048];
        MultiByteToWideChar(CP_ACP, 0, cmd, -1, w, 2048);
        int argc = 0;
        wchar_t **argv = CommandLineToArgvW(w, &argc);
        char a[8][600];
        for (int j = 0; j < argc && j < 8; j++) WideCharToMultiByte(CP_ACP, 0, argv[j], -1, a[j], 600, NULL, NULL);
        int good = argc == 7 && !strcmp(a[0], exe) && !strcmp(a[1], "--update") && !strcmp(a[2], "--silent") &&
                   !strcmp(a[3], "--game-dir") && !strcmp(a[4], gr[i]) && !strcmp(a[5], "--wait-pid") && !strcmp(a[6], "4242");
        if (!good) { ok = 0; printf("  %s -> argc %d\n", cmd, argc); }
        if (i == 0 && verbose) printf("  cmdline: %s\n", cmd);
        LocalFree(argv);
    }
    CHECK(ok, "setup command line round-trips through CommandLineToArgvW (spaces, quotes, trailing backslashes)");
    char small[20];
    CHECK(up_build_cmdline(small, sizeof small, exe, gr[0], 1) == -1 && small[0] == 0, "too small buffer -> -1");
}

static void t_install_result(void)
{
    char d[160];
    const char *a = "---- 2026 V3X-Setup 0.5.0\r\ninstalled\r\nupdate-result: failed code=3 running\r\n---- again\r\n"
                    "update-result: ok version=0.6.0\r\n";
    CHECK(up_install_result(a, strlen(a), d, sizeof d) == 1 && !strcmp(d, "0.6.0"), "install.log: last line ok -> %s", d);
    const char *b = "update-result: ok version=0.6.0\nupdate-result: failed code=7 timeout\n";
    CHECK(up_install_result(b, strlen(b), d, sizeof d) == 0 && !strcmp(d, "failed code=7 timeout"), "install.log: last failed -> %s", d);
    CHECK(up_install_result("nothing\n", 8, d, sizeof d) == -1, "install.log without result");
}

/* ------------------------------------------------------------------ end to end */

static int file_text(const char *p, char *out, size_t cap)
{
    FILE *f = fopen(p, "rb");
    if (!f) { out[0] = 0; return -1; }
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = 0;
    return 0;
}

static int pe_build_id(const char *exe, char *out, size_t cap)
{
    FILE *f = fopen(exe, "rb");
    unsigned char h[4096];
    size_t n = f ? fread(h, 1, sizeof h, f) : 0;
    if (f) fclose(f);
    if (n < 0x200) return -1;
    uint32_t pe = *(uint32_t *)(h + 0x3c);
    if (pe + 0x60 > n) return -1;
    snprintf(out, cap, "%08lx-%lx", (unsigned long)*(uint32_t *)(h + pe + 8), (unsigned long)*(uint32_t *)(h + pe + 24 + 56));
    return 0;
}

static int run_child(const char *bin, const char *arg, DWORD *pid)
{
    char cmd[MAX_PATH * 2];
    snprintf(cmd, sizeof cmd, "\"%s\\upd_child.exe\" %s", bin, arg);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    SetEnvironmentVariableA("V3X_NO_CONSOLE", "1");
    SetEnvironmentVariableA("V3X_BUILD_GATE", "1");     /* unknown build -> safe mode */
    SetEnvironmentVariableA("V3X_UPDATER_NO_NET", "1"); /* never the network */
    SetEnvironmentVariableA("V3X_UPDATER_FORCE", "1");  /* behave as inside victoria3.exe */
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, bin, &si, &pi)) return -1;
    *pid = pi.dwProcessId;
    DWORD w = WaitForSingleObject(pi.hProcess, 30000);
    if (w != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 9);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return w == WAIT_OBJECT_0 ? 0 : -1;
}

static void write_cache(const char *v3x, const char *build, const char *sha, long long size)
{
    char p[MAX_PATH], buf[4096];
    snprintf(p, sizeof p, "%s\\update_cache.json", v3x);
    long long now = (long long)time(NULL);
    snprintf(buf, sizeof buf,
             "{\"schema\":1,\"checked_at\":%lld,\"attempt_at\":%lld,\"key\":\"stale-key-forces-a-(blocked)-fetch\",\"releases\":["
             "{\"tag\":\"v99.0.0\",\"version\":\"99.0.0\",\"draft\":false,\"prerelease\":false,\"compat\":1,\"compat_err\":\"\","
             "\"api_version\":5,\"setup_name\":\"V3X-Setup.exe\",\"setup_url\":\"" UP_ASSET_PREFIX "v99.0.0/V3X-Setup.exe\","
             "\"setup_sha\":\"%s\",\"setup_size\":%lld,\"game_builds\":[{\"branch\":\"\",\"build_id\":\"%s\"}]},"
             "{\"tag\":\"v0.0.1\",\"version\":\"0.0.1\",\"draft\":false,\"prerelease\":false,\"compat\":1,\"compat_err\":\"\","
             "\"api_version\":5,\"setup_name\":\"V3X-Setup.exe\",\"setup_url\":\"" UP_ASSET_PREFIX "v0.0.1/V3X-Setup.exe\","
             "\"setup_sha\":\"%s\",\"setup_size\":0,\"game_builds\":[{\"branch\":\"\",\"build_id\":\"%s\"}]}]}",
             now, now, sha, size, build, sha, build);
    FILE *f = fopen(p, "wb");
    fputs(buf, f);
    fclose(f);
}

static int fake_setup_main(int argc, wchar_t **argv)
{
    /* "V3X-Setup.exe --update ...": record argv next to ourselves */
    wchar_t self[MAX_PATH], out[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    wcscpy(out, self);
    wchar_t *s = wcsrchr(out, L'\\');
    if (s) wcscpy(s + 1, L"setup_args.txt");
    FILE *f = _wfopen(out, L"wb");
    if (!f) return 1;
    for (int i = 0; i < argc; i++) fprintf(f, "%ls\n", argv[i]);
    fclose(f);
    return 0;
}

static void t_e2e(const char *dir)
{
    char bin[MAX_PATH], v3x[MAX_PATH], upd[MAX_PATH], child[MAX_PATH], setup[MAX_PATH], args[MAX_PATH], build[64];
    snprintf(bin, sizeof bin, "%s\\binaries", dir);
    snprintf(v3x, sizeof v3x, "%s\\v3x", bin);
    snprintf(upd, sizeof upd, "%s\\update", v3x);
    snprintf(child, sizeof child, "%s\\upd_child.exe", bin);
    snprintf(setup, sizeof setup, "%s\\V3X-Setup.exe", upd);
    snprintf(args, sizeof args, "%s\\setup_args.txt", upd);
    CreateDirectoryA(upd, NULL);
    char ini[MAX_PATH];
    snprintf(ini, sizeof ini, "%s\\v3x.ini", v3x);
    FILE *fi = fopen(ini, "wb");
    if (fi) { fputs("[update]\r\ndelay_sec = 0\r\n", fi); fclose(fi); }
    CHECK(pe_build_id(child, build, sizeof build) == 0, "e2e: child build id %s", build);
    char self[MAX_PATH], hex[65];
    GetModuleFileNameA(NULL, self, MAX_PATH);
    CHECK(CopyFileA(self, setup, FALSE) && up_sha256_file(setup, hex) == 0, "e2e: fake setup placed (%.16s...)", hex);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    GetFileAttributesExA(setup, GetFileExInfoStandard, &fa);
    write_cache(v3x, build, hex, (long long)fa.nFileSizeLow);
    DeleteFileA(args);
    DWORD pid = 0;
    char out[16384], a[4096], log[65536];
    char co[MAX_PATH], lp[MAX_PATH];
    snprintf(co, sizeof co, "%s\\child_out.txt", bin);
    snprintf(lp, sizeof lp, "%s\\v3x.log", v3x);
    /* 1. auto = 1 (default) but the cached release is unsigned: announced, never started (H1) */
    CHECK(run_child(bin, "", &pid) == 0, "e2e: child ran and exited (pid %lu)", pid);
    Sleep(1000);
    file_text(co, out, sizeof out);
    file_text(lp, log, sizeof log);
    CHECK(strstr(log, "Safe mode") && strstr(log, "loaded plugin 'updater'"), "e2e: updater loads in safe mode (manifest \"*\")");
    CHECK(strstr(out, "99.0.0") && strstr(out, "NOT signed") && strstr(out, "supports it") && !strstr(out, "downloaded and verified"),
          "e2e: `update` shows the urgent 99.0.0, NOT signed, not downloaded on its own");
    CHECK(GetFileAttributesA(args) == INVALID_FILE_ATTRIBUTES && strstr(log, "99.0.0 is NOT signed") && !strstr(log, "started the V3X"),
          "e2e: unsigned release + auto=1 -> nothing started at exit");
    CHECK(strstr(log, "update check failed") != NULL, "e2e: blocked network = one log line");
    /* 2. `update now`: the player asked, so the (hash-verified) unsigned setup is started at exit */
    write_cache(v3x, build, hex, (long long)fa.nFileSizeLow);
    CHECK(run_child(bin, "now", &pid) == 0, "e2e: 'update now' child ran and exited (pid %lu)", pid);
    file_text(co, out, sizeof out);
    file_text(lp, log, sizeof log);
    CHECK(strstr(out, "99.0.0") && strstr(out, "downloaded and verified"), "e2e: after 'update now' the cached setup is verified on disk");
    for (int i = 0; i < 100 && GetFileAttributesA(args) == INVALID_FILE_ATTRIBUTES; i++) Sleep(100);
    file_text(args, a, sizeof a);
    char want[1024];
    snprintf(want, sizeof want, "%s\n--update\n--silent\n--game-dir\n%s\n--wait-pid\n%lu\n", setup, dir, pid);
    for (char *p = a; *p; p++) if (*p == '\r') memmove(p, p + 1, strlen(p));
    CHECK(!_stricmp(a, want), "e2e: at process exit the setup was started with the exact arguments\n%s", a);
    file_text(lp, log, sizeof log);
    CHECK(strstr(log, "started the V3X 99.0.0 setup") && strstr(log, "UNSIGNED release, started because you typed"),
          "e2e: start logged (and that it was unsigned + asked for)");
    char att[64], fl[64], ap[MAX_PATH];
    snprintf(ap, sizeof ap, "%s\\attempts.txt", upd);
    file_text(ap, att, sizeof att);
    CHECK(!strncmp(att, "99.0.0 1", 8), "e2e: attempt counted (%s)", att);
    snprintf(ap, sizeof ap, "%s\\floor.txt", upd);
    file_text(ap, fl, sizeof fl);
    CHECK(!strncmp(fl, "99.0.0", 6), "e2e: rollback floor raised to the started version (%s)", fl);

    /* tampered setup on disk: verification fails, the (blocked) re-download fails, nothing is started */
    FILE *f = fopen(setup, "ab");
    fputs("tampered", f);
    fclose(f);
    write_cache(v3x, build, hex, (long long)fa.nFileSizeLow);
    DeleteFileA(args);
    CHECK(run_child(bin, "tamper", &pid) == 0, "e2e: tamper run exited");
    Sleep(1500);
    file_text(lp, log, sizeof log);
    file_text(co, out, sizeof out);
    CHECK(GetFileAttributesA(args) == INVALID_FILE_ATTRIBUTES && !strstr(out, "downloaded and verified") &&
              strstr(log, "download of V3X 99.0.0 not used"), "e2e: tampered setup -> nothing started");
}

int main(int argc, char **argv)
{
    int wargc;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargc > 1 && !wcscmp(wargv[1], L"--update")) return fake_setup_main(wargc, wargv);
    setvbuf(stdout, NULL, _IONBF, 0);
    t_sha();
    t_semver();
    t_json();
    t_fuzz();
    t_parse();
    t_select();
    t_rate();
    t_cache();
    t_download();
    t_sig();
    t_cmdline();
    t_install_result();
    if (argc > 1) {
        char full[MAX_PATH];
        GetFullPathNameA(argv[1], MAX_PATH, full, NULL);
        for (char *p = full; *p; p++) if (*p == '/') *p = '\\';
        size_t n = strlen(full);
        while (n && full[n - 1] == '\\') full[--n] = 0;
        t_e2e(full);
    }
    printf("%d checks, %d failed\n%s\n", checks, fails, fails ? "FAILED" : "ALL OK");
    return fails ? 1 : 0;
}
#endif
