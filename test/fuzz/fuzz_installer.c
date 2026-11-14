/* Fuzz harness: installer/setup_parse.h - what V3X-Setup.exe parses from bytes it did not write itself:
 * the payload footer + index + per-file hashes appended to the exe (a damaged / hostile download),
 * meta.txt, binaries/v3x/install_manifest.txt (user-writable; H2: never follow a bad line) and the
 * compat.json scanner of --verify-release. SHA-256 comes from plugins/updater/up_logic.c (portable C).
 * windows.h-free: also a libFuzzer/ASan target on Linux (.github/workflows/ci.yml, job fuzz-linux).
 * Invariants: a payload that parses has only known kinds, plain relative paths, entries inside the
 * file area whose bytes hash to the index value; a payload built from the input (valid by
 * construction) always parses and round-trips; any single changed byte in it is rejected; manifest
 * entries kept are bin/config + 64 hex + a plain path, the backup only v3x/backup/version(.N).dll;
 * the compat scanner returns objects inside the buffer. */
#include "v3x_fuzz.h"
#include "../../installer/setup_parse.h"
#include "../../plugins/updater/up_logic.c"

/* up_logic.c references the signature check (up_sig.c); not needed here */
int up_sig_check(const char *json, size_t n, const char *sig, size_t sn, char key_id[17], const char **why)
{
    (void)json; (void)n; (void)sig; (void)sn;
    if (key_id) key_id[0] = 0;
    if (why) *why = "not in this harness";
    return -1;
}
int up_sig_keys(void) { return 0; }

static int fz_sha256(const void *d, size_t n, uint8_t out[32])
{
    up_sha256 s;
    up_sha256_init(&s);
    up_sha256_update(&s, d, n);
    up_sha256_final(&s, out);
    return 1;
}

static void check_entries(const uint8_t *blob, size_t n, const Entry *e, int ne)
{
    V3X_FUZZ_ASSERT(ne >= 0 && ne <= 4096);
    uint64_t ioff;
    memcpy(&ioff, blob + n - 56, 8);
    for (int i = 0; i < ne; i++) {
        V3X_FUZZ_ASSERT(!strcmp(e[i].kind, "bin") || !strcmp(e[i].kind, "ini") || !strcmp(e[i].kind, "v3xini") || !strcmp(e[i].kind, "meta"));
        V3X_FUZZ_ASSERT(rel_ok(e[i].rel) && e[i].off <= ioff && e[i].size <= ioff - e[i].off && e[i].data == blob + e[i].off);
        uint8_t h[32];
        char hx[65];
        fz_sha256(e[i].data, (size_t)e[i].size, h);
        hex32(h, hx);
        V3X_FUZZ_ASSERT(!strcmp(hx, e[i].sha));
        if (!strcmp(e[i].kind, "meta")) {
            char ver[64] = "?";
            Build b[16];
            Prof p[64];
            int nb = 0, np = 0;
            V3X_FUZZ_ASSERT(sp_meta_parse(e[i].data, (size_t)e[i].size, ver, sizeof ver, b, &nb, p, &np));
            V3X_FUZZ_ASSERT(nb <= 16 && np <= 64 && strlen(ver) < sizeof ver);
        }
    }
}

/* Builds a valid payload in buf from the input: pieces separated by 0xFF are files, names from a fixed
 * list (plus one taken from the input). Returns the length. */
static size_t build_payload(const uint8_t *d, size_t n, uint8_t *buf, size_t cap, int *count)
{
    static const char *const names[] = {"version.dll", "v3x/plugins/a.dll", "v3x/a.ini", "v3x/v3x.ini", "meta.txt", "v3x/plugins.json"};
    static const char *const kinds[] = {"bin", "bin", "ini", "v3xini", "meta", "bin"};
    size_t k = 0;
    memcpy(buf, "MZstub", 6); k = 6;
    char index[4096];
    size_t il = 0;
    int c = 0;
    const uint8_t *p = d, *e = d + n;
    while (p <= e && c < 6) {
        const uint8_t *q = (const uint8_t *)memchr(p, 0xFF, (size_t)(e - p));
        if (!q) q = e;
        size_t len = (size_t)(q - p);
        if (k + len + 8192 > cap) break;
        memcpy(buf + k, p, len);
        uint8_t h[32];
        char hx[65];
        fz_sha256(p, len, h);
        hex32(h, hx);
        int w = snprintf(index + il, sizeof index - il, "%s %zu %zu %s %s\n", kinds[c], k, len, hx, names[c]);
        if (w <= 0 || (size_t)w >= sizeof index - il) break;
        il += (size_t)w;
        k += len;
        c++;
        p = q + 1;
    }
    uint64_t ioff = k;
    memcpy(buf + k, index, il);
    k += il;
    uint8_t ih[32];
    fz_sha256(index, il, ih);
    uint32_t isz = (uint32_t)il, cnt = (uint32_t)c;
    memcpy(buf + k, "V3XPAYL1", 8);
    memcpy(buf + k + 8, &ioff, 8);
    memcpy(buf + k + 16, &isz, 4);
    memcpy(buf + k + 20, &cnt, 4);
    memcpy(buf + k + 24, ih, 32);
    memcpy(buf + k + 56, "V3XEND\0\0", 8);
    k += 64;
    *count = c;
    return k;
}

static uint8_t g_blob[3 * 16384 + 65536];

static void fuzz_payload(const uint8_t *d, size_t n)
{
    Entry *ent = NULL;
    int ne = 0;
    if (n && (d[0] & 1)) {   /* raw bytes as the whole setup file */
        const uint8_t *b = (const uint8_t *)v3x_fuzz_guarded(d + 1, n - 1);
        if (sp_payload_parse(b, n - 1, fz_sha256, &ent, &ne)) check_entries(b, n - 1, ent, ne);
        else V3X_FUZZ_ASSERT(!ent && ne == 0);
        free(ent);
        return;
    }
    int cnt = 0;
    size_t bn = build_payload(n ? d + 1 : d, n ? n - 1 : 0, g_blob, sizeof g_blob, &cnt);
    const uint8_t *b = (const uint8_t *)v3x_fuzz_guarded(g_blob, bn);
    V3X_FUZZ_ASSERT(sp_payload_parse(b, bn, fz_sha256, &ent, &ne) && ne == cnt);
    check_entries(b, bn, ent, ne);
    free(ent);
    ent = NULL;
    /* one flipped bit anywhere after the stub must be rejected: file bytes and the index are hashed,
     * the footer's magic / count / end tag are compared, its offset / size / hash select or check the
     * hashed index */
    if (n >= 3 && bn > 6) {
        size_t pos = 6 + ((size_t)d[1] << 8 | d[2]) % (bn - 6);
        uint8_t *m = (uint8_t *)v3x_fuzz_guarded(g_blob, bn);
        m[pos] ^= (uint8_t)(1u << (d[1] & 7));
        int ok = sp_payload_parse(m, bn, fz_sha256, &ent, &ne);
        V3X_FUZZ_ASSERT(!ok && !ent);
        free(ent);
    }
}
V3X_FUZZ_TARGET(setup_payload, fuzz_payload)
V3X_FUZZ_SEED(setup_payload, "\x00MZ dll bytes\xFF[x]\r\na=1\xFF; ini\xFF[plugins]\r\nhello=0\xFFversion 0.5.1\nbuild 6af3094f-60fb000 release/1.15.1\nplugin hello 0\n\xFF{}")
V3X_FUZZ_SEED(setup_payload, "\x01MZ\x00\x00V3XPAYL1\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00")
V3X_FUZZ_SEED(setup_payload, "\x00\xFF\xFF\xFFversion\n")

static void fuzz_manifest(const uint8_t *d, size_t n)
{
    char *t = (char *)v3x_fuzz_cstr(d, n);
    Manifest m;
    memset(&m, 0, sizeof m);
    g_oom = 0;
    mf_parse(t, &m);
    V3X_FUZZ_ASSERT(m.n >= 0 && m.n <= m.cap && strlen(m.version) < sizeof m.version);
    V3X_FUZZ_ASSERT(!m.backup_rel[0] || (backup_rel_ok(m.backup_rel) && hex64_ok(m.backup_sha)));
    for (int i = 0; i < m.n; i++) {
        V3X_FUZZ_ASSERT((!strcmp(m.f[i].kind, "bin") || !strcmp(m.f[i].kind, "config")) && hex64_ok(m.f[i].sha) && rel_ok(m.f[i].rel));
        for (int j = 0; j < i; j++) V3X_FUZZ_ASSERT(_stricmp(m.f[i].rel, m.f[j].rel) != 0);
        V3X_FUZZ_ASSERT(mf_find(&m, m.f[i].rel) == &m.f[i]);
    }
    free(m.f);
}
V3X_FUZZ_TARGET(setup_manifest, fuzz_manifest)
V3X_FUZZ_SEED(setup_manifest, "# V3X install manifest\nv3x_manifest 1\nversion 0.5.1\ninstalled 2026-11-13 10:00\ngame_build 6af3094f-60fb000 release/1.15.1\n"
                              "backup 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef v3x/backup/version.dll\n"
                              "file bin 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef version.dll\n"
                              "file config 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef v3x/v3x.ini\n"
                              "file bin 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef V3X/V3X.INI\n")
V3X_FUZZ_SEED(setup_manifest, "\xEF\xBB\xBFv3x_manifest 1\r\nbackup 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef ../../secret.dll\r\n"
                              "backup 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef v3x/backup/version.100.dll\r\n"
                              "file bin 0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef C:/Windows/x.dll\r\n"
                              "file exe 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef a\\b.dll\r\nfile bin x NUL\r\n")

static void fuzz_compat(const uint8_t *d, size_t n)
{
    const char *p = (const char *)v3x_fuzz_guarded(d, n), *e = p + n;
    char ver[64];
    if (cj_str(p, e, "v3x_version", ver, sizeof ver)) V3X_FUZZ_ASSERT(strlen(ver) < sizeof ver);
    const char *as = cj_assets(p, e);
    if (!as) return;
    V3X_FUZZ_ASSERT(as >= p && as < e && *as == '[');
    const char *it = as + 1, *ob, *oe;
    for (int guard = 0; guard < 4096; guard++) {
        int r = cj_next_asset(&it, e, &ob, &oe);
        if (r <= 0) break;
        V3X_FUZZ_ASSERT(ob >= as && ob < oe && oe < e && *ob == '{' && *oe == '}' && it == oe + 1);
        char name[96], hex[80];
        unsigned long long size;
        if (cj_str(ob, oe, "name", name, sizeof name)) V3X_FUZZ_ASSERT(strlen(name) < sizeof name);
        if (cj_str(ob, oe, "sha256", hex, sizeof hex) && hex64_ok(hex)) V3X_FUZZ_ASSERT(strlen(hex) == 64);
        if (cj_int(ob, oe, "size", &size)) V3X_FUZZ_ASSERT(size < 1000000000000000ull);
    }
}
V3X_FUZZ_TARGET(setup_compat, fuzz_compat)
V3X_FUZZ_SEED(setup_compat, "{\n  \"schema\": 1,\n  \"v3x_version\": \"0.5.1\",\n  \"assets\": [\n    {\n      \"name\": \"V3X-Setup.exe\",\n      \"size\": 7408521,\n"
                            "      \"sha256\": \"10616d2c00f5c19fd6e7ba649168e75d9db47d5ba2b4eb0980a527c337571943\"\n    },\n    {\"name\": \"install.ps1\", \"size\": 1, \"sha256\": \"x\"}\n  ]\n}\n")
V3X_FUZZ_SEED(setup_compat, "{\"assets\" : [{\"name\":\"a\\\"b\"}, {\"size\": 9999999999999999}, {\"name\": \"x\", {}]")
