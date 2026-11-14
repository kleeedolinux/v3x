/* Unit tests of the loader's file / text parsers that run BEFORE any plugin code does (loader 0.6.0):
 *   src/core_parse.h   cp_pattern_parse (strict IDA pattern), cp_caps_parse (manifest capabilities),
 *                      cp_official_has (signed plugins.json), cp_allow_has (trusted_plugins.txt)
 *   src/v3x_pe.h       SHA-256 and v3x_pe_read_plugin (bounded PE32+ reader: exports + manifest from the FILE)
 * No game, no loader: pure functions. Every input buffer is placed right before a PAGE_NOACCESS page, so a
 * read past the length handed to a parser is an access violation (and a UBSan trap under SANITIZE=1).
 *   core_parse_test.exe [real.dll ...]    extra arguments: real plugin DLL files that must parse
 * Exit code 0 = ALL OK. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/core_parse.h"
#include "../src/v3x_pe.h"
#include "core_pe_img.h"

static int fails, checks;
#define CHECK(c, ...) do { int ok_ = !!(c); checks++; if (!ok_) { printf("FAIL: "); printf(__VA_ARGS__); printf("  (%s:%d)\n", __FILE__, __LINE__); } fails += !ok_; } while (0)

/* copy of d[0..n) that ends exactly at a no-access page (n = 0 allowed) */
static void *guard_copy(const void *d, size_t n)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    size_t pg = si.dwPageSize, body = (n + pg - 1) / pg * pg;
    if (!body) body = pg;
    uint8_t *b = (uint8_t *)VirtualAlloc(NULL, body + pg, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old;
    if (!b || !VirtualProtect(b + body, pg, PAGE_NOACCESS, &old)) { printf("FAIL: VirtualAlloc\n"); exit(2); }
    if (n) memcpy(b + body - n, d, n);
    return b + body - n;
}
static void guard_free(void *p, size_t n)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    size_t pg = si.dwPageSize, body = (n + pg - 1) / pg * pg;
    if (!body) body = pg;
    VirtualFree((uint8_t *)p - (body - n), 0, MEM_RELEASE);
}

/* ---- pattern */
static int pat(const char *s, uint8_t *by, uint8_t *mk, int cap)
{
    size_t n = strlen(s);
    char *g = (char *)guard_copy(s, n);
    int r = cp_pattern_parse(g, n, by, mk, cap);
    guard_free(g, n);
    return r;
}
static void test_pattern(void)
{
    uint8_t by[300], mk[300];
    int r = pat("48 8B ?? 05 ? aB", by, mk, 256);
    CHECK(r == 6 && by[0] == 0x48 && by[1] == 0x8B && !mk[2] && by[3] == 5 && mk[0] && mk[1] && mk[3] && !mk[4] && by[5] == 0xAB && mk[5],
          "pattern: hex (any case), double and single question-mark wildcards (%d)", r);
    CHECK(pat("  \t90\t\t  CC  ", by, mk, 256) == 2 && by[0] == 0x90 && by[1] == 0xCC, "pattern: tabs and runs of blanks");
    CHECK(pat("", by, mk, 256) == 0 && pat("   ", by, mk, 256) == 0, "pattern: empty = 0 bytes");
    CHECK(pat("ZZ", by, mk, 256) == -1 && pat("4", by, mk, 256) == -1 && pat("123", by, mk, 256) == -1 && pat("0x90", by, mk, 256) == -1 &&
              pat("48 GG 05", by, mk, 256) == -1 && pat("???", by, mk, 256) == -1 && pat("?x", by, mk, 256) == -1 && pat("x?", by, mk, 256) == -1 &&
              pat("90,CC", by, mk, 256) == -1 && pat("9\xC3\xA9", by, mk, 256) == -1,
          "pattern: bad tokens (non-hex, 1 or 3 digits, 0x, three wildcards, wildcard+letter, comma, UTF-8) = -1 (a bad token used to be read as 00)");
    CHECK(pat("48 -1", by, mk, 256) == -1 && pat("+8 90", by, mk, 256) == -1, "pattern: sign characters are not hex");
    char big[3 * 300 + 1];
    for (int n = 0; n < 300; n++) memcpy(big + 3 * n, "90 ", 3);
    big[3 * 256] = 0;
    CHECK(pat(big, by, mk, 256) == 256, "pattern: exactly 256 bytes is accepted");
    big[3 * 257] = 0;
    for (int n = 256; n < 257; n++) memcpy(big + 3 * n, "90 ", 3);
    CHECK(pat(big, by, mk, 256) == -2, "pattern: 257 bytes = -2 (longer than the cap)");
    /* the length argument bounds the read, not the NUL */
    const char *s = "48 8B 05 FF";
    CHECK(cp_pattern_parse(s, 5, by, mk, 256) == 2 && cp_pattern_parse(s, 4, by, mk, 256) == -1 && cp_pattern_parse(s, 0, by, mk, 256) == 0,
          "pattern: only the first n bytes are read (a token cut by n is a bad token)");
    CHECK(cp_pattern_parse("90 90 90", 8, by, mk, 2) == -2 && cp_pattern_parse("90 90", 5, by, mk, 2) == 2, "pattern: cap = 2");
}

/* ---- capabilities */
static void test_caps(void)
{
    int unk = -1;
    char first[8];
    uint32_t m = cp_caps_parse("hook", 4, &unk, first, sizeof first);
    CHECK(m == CP_CAP_HOOK && unk == 0 && !first[0], "caps: \"hook\" = hook only");
    const char *all = "hook, patch;fs\tnet  script,exec";
    m = cp_caps_parse(all, strlen(all), &unk, first, sizeof first);
    CHECK(m == CP_CAP_ALL && unk == 0, "caps: every word, separated by ',' ';' space and tab = %u", m);
    m = cp_caps_parse("HoOk PATCH", 10, &unk, first, sizeof first);
    CHECK(m == (CP_CAP_HOOK | CP_CAP_PATCH), "caps: case-insensitive");
    m = cp_caps_parse("hook bogus patch hooks fs2", 26, &unk, first, sizeof first);
    CHECK(m == (CP_CAP_HOOK | CP_CAP_PATCH) && unk == 3 && !strcmp(first, "bogus"), "caps: unknown words counted (%d), first one reported (\"%s\"), ignored", unk, first);
    m = cp_caps_parse("averyverylongunknownword", 24, &unk, first, sizeof first);
    CHECK(m == 0 && unk == 1 && strlen(first) == 7, "caps: the reported unknown word is cut to the buffer (\"%s\")", first);
    m = cp_caps_parse("", 0, &unk, first, sizeof first);
    CHECK(m == 0 && unk == 0, "caps: \"\" declares nothing (mask 0: every enforced call is refused)");
    m = cp_caps_parse(NULL, 0, &unk, first, sizeof first);
    CHECK(m == 0 && unk == 0, "caps: NULL string");
    m = cp_caps_parse(" ,; ", 4, NULL, NULL, 0);
    CHECK(m == 0, "caps: only separators, NULL out-params");
    m = cp_caps_parse("hook exec", 4, &unk, first, sizeof first);
    CHECK(m == CP_CAP_HOOK, "caps: only the first n bytes are read (\"hook\" of \"hook exec\")");
    char *g = (char *)guard_copy("fs", 2);
    m = cp_caps_parse(g, 2, &unk, first, sizeof first);
    guard_free(g, 2);
    CHECK(m == CP_CAP_FS, "caps: no read past n");
    CHECK(CP_CAP_HOOK == 1u && CP_CAP_PATCH == 2u && CP_CAP_FS == 4u && CP_CAP_NET == 8u && CP_CAP_SCRIPT == 16u && CP_CAP_EXEC == 32u && CP_CAP_ALL == 63u,
          "caps: bit values");
}

/* ---- official list + allowlist */
#define H_A "1111111111111111111111111111111111111111111111111111111111111111"
#define H_B "abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd"
#define H_C "2222222222222222222222222222222222222222222222222222222222222222"
static int off(const char *json, const char *file, const char *sha)
{
    size_t n = strlen(json);
    char *g = (char *)guard_copy(json, n);
    int r = cp_official_has(g, n, file, sha);
    guard_free(g, n);
    return r;
}
static int allow(const char *txt, const char *sha)
{
    size_t n = strlen(txt);
    char *g = (char *)guard_copy(txt, n);
    int r = cp_allow_has(g, n, sha);
    guard_free(g, n);
    return r;
}
static void test_lists(void)
{
    const char *json =
        "{\"schema\":1,\"v3x_version\":\"0.6.0\",\"plugins\":[\n"
        "  {\"name\":\"hello\",\"file\":\"plugins/hello.dll\",\"sha256\":\"" H_A "\",\"version\":\"0.6.0\"},\n"
        "  {\"name\":\"zed\",\"sha256\":\"" H_B "\",\"file\":\"plugins\\\\zed.dll\"}\n"
        "]}";
    CHECK(off(json, "hello.dll", H_A), "official: (file, sha256) listed");
    CHECK(off(json, "HELLO.DLL", H_A), "official: file name compared case-insensitively");
    CHECK(!off(json, "hello.dll", H_B), "official: the sha256 of ANOTHER entry does not authorise the file");
    CHECK(!off(json, "hello.dll", H_C), "official: unknown sha256 refused");
    CHECK(!off(json, "evil.dll", H_A), "official: right hash under another file name refused");
    CHECK(!off(json, "plugins/hello.dll", H_A), "official: `file` is the plain name of the DLL on disk (base name only)");
    /* second entry: keys in the other order, and a backslash in the value is an escape = refused */
    CHECK(!off(json, "zed.dll", H_B), "official: an escape sequence inside a string value makes the entry unusable");
    const char *j2 = "{\"plugins\":[{\"sha256\":\"" H_B "\",\"file\":\"plugins/zed.dll\"}]}";
    CHECK(off(j2, "zed.dll", H_B), "official: sha256 before file works");
    CHECK(off("{\"plugins\":[{\"file\":\"a/b/c/zed.dll\",\"sha256\":\"ABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCD\"}]}",
              "zed.dll", H_B), "official: upper-case hex in the list matches");
    CHECK(!off("{\"plugins\":[{\"file\":\"zed.dll\"},{\"sha256\":\"" H_B "\"}]}", "zed.dll", H_B),
          "official: file of one entry and sha256 of the next are never combined");
    CHECK(!off("{\"plugins\":[{\"file\":\"zed.dll\",\"sha256\":\"" H_B "\"", "zed.dll", H_B), "official: truncated list (no closing brace) = not listed");
    CHECK(!off("{\"plugins\":[{\"file\":\"zed.dll\",\"sha256\":\"" H_B "\",\"x\":{\"y\":1}}]}", "zed.dll", H_B),
          "official: a nested value in an entry = refused (not part of the format)");
    CHECK(!off("{\"schema\":1}", "zed.dll", H_B) && !off("", "zed.dll", H_B) && !off("[]", "zed.dll", H_B) && !off("\"plugins\"", "zed.dll", H_B),
          "official: no plugins array = nothing listed");
    CHECK(!off("{\"plugins\":[{\"file\":\"zed.dll\",\"sha256\":\"" H_B "\"}]}", "zed.dll", "abc") &&
              !off("{\"plugins\":[{\"file\":\"zed.dll\",\"sha256\":\"" H_B "0\"}]}", "zed.dll", H_B) &&
              !off("{\"plugins\":[{\"file\":\"zed.dll\",\"sha256\":\"" H_B "\"}]}", "zed.dll", ""),
          "official: a sha256 that is not exactly 64 digits never matches");
    CHECK(!off("{\"plugins\":[{\"file\":\"\",\"sha256\":\"" H_B "\"}]}", "zed.dll", H_B) && !off("{\"plugins\":[{\"file\":\"x/\",\"sha256\":\"" H_B "\"}]}", "zed.dll", H_B),
          "official: empty base name");
    CHECK(!off("{\"plugins\":[{\"file\":\"zed.dll\",\"sha256\":\"zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz\"}]}", "zed.dll",
              "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"),
          "official: non-hex 'hash' never matches, even against itself");
    CHECK(cp_official_has(NULL, 0, "a.dll", H_A) == 0, "official: NULL text");
    /* a long list: the 2000th entry is found, the guard limit does not cut a normal list */
    {
        size_t cap = 2000 * 140 + 64;
        char *big = (char *)malloc(cap);
        size_t k = (size_t)snprintf(big, cap, "{\"plugins\":[");
        for (int i = 0; i < 2000; i++)
            k += (size_t)snprintf(big + k, cap - k, "%s{\"file\":\"plugins/p%04d.dll\",\"sha256\":\"%s\"}", i ? "," : "", i, i == 1999 ? H_B : H_A);
        snprintf(big + k, cap - k, "]}");
        CHECK(off(big, "p1999.dll", H_B) && !off(big, "p1999.dll", H_A) && off(big, "p0000.dll", H_A), "official: a 2000-entry list");
        free(big);
    }
    /* allowlist */
    const char *al = "# my plugins\r\n" H_A " hello.dll\r\n\r\n   " H_B "\tzed.dll  \r\n  # " H_C " commented.dll\r\n" H_C;
    CHECK(allow(al, H_A) && allow(al, H_B) && allow(al, H_C), "allow: lines with CRLF, leading blanks, a tab, and a last line without newline");
    CHECK(!allow("# " H_A "\n", H_A), "allow: a comment line trusts nothing");
    CHECK(!allow(H_A "0 x.dll\n", H_A) && !allow("0" H_A " x.dll\n", H_A), "allow: 65 hex digits is not the 64-digit hash");
    CHECK(!allow("x " H_A "\n", H_A), "allow: the hash must be the first word of the line (the name column is only a note)");
    CHECK(allow("ABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCD x\n", H_B), "allow: upper-case hex matches");
    CHECK(!allow("", H_A) && !allow("\n\n", H_A) && cp_allow_has(NULL, 0, H_A) == 0, "allow: empty / NULL");
    CHECK(!allow(H_A, "short") && !allow(H_A, H_C), "allow: other hashes");
}

/* ---- SHA-256 */
static void test_sha(void)
{
    char hex[65];
    v3x_sha256_hex("", 0, hex);
    CHECK(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "sha256(\"\")");
    v3x_sha256_hex("abc", 3, hex);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256(\"abc\")");
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    v3x_sha256_hex(m, strlen(m), hex);
    CHECK(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "sha256(56-byte vector: padding crosses a block)");
    char *mil = (char *)malloc(1000000);
    memset(mil, 'a', 1000000);
    v3x_sha256_hex(mil, 1000000, hex);
    CHECK(!strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "sha256(1,000,000 x 'a')");
    /* incremental updates in odd pieces equal one call */
    char want[65];
    v3x_sha256_hex(mil, 4099, want);
    V3xSha256 c;
    uint8_t d[32];
    v3x_sha256_init(&c);
    size_t pos = 0;
    for (size_t step = 1; pos < 4099; step = step * 3 + 1) {
        size_t k = step < 4099 - pos ? step : 4099 - pos;
        v3x_sha256_update(&c, mil + pos, k);
        pos += k;
    }
    v3x_sha256_final(&c, d);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { hex[2 * i] = hx[d[i] >> 4]; hex[2 * i + 1] = hx[d[i] & 15]; }
    hex[64] = 0;
    CHECK(!strcmp(hex, want), "sha256: streaming in uneven pieces = one call");
    free(mil);
}

/* ---- PE reader */
static int pe(const PeOpt *o, V3xPePlugin *out)
{
    uint8_t img[PEI_SIZE];
    size_t n = pei_build(img, o);
    uint8_t *g = (uint8_t *)guard_copy(img, n);
    int r = v3x_pe_read_plugin(g, n, out);
    guard_free(g, n);
    return r;
}
static int pe_mut(const PeOpt *o, size_t at, uint32_t v, int bytes, V3xPePlugin *out)   /* build, then overwrite 1/2/4 bytes at `at` */
{
    uint8_t img[PEI_SIZE];
    size_t n = pei_build(img, o);
    for (int i = 0; i < bytes; i++) img[at + (size_t)i] = (uint8_t)(v >> (8 * i));
    uint8_t *g = (uint8_t *)guard_copy(img, n);
    int r = v3x_pe_read_plugin(g, n, out);
    guard_free(g, n);
    return r;
}
static void test_pe(int argc, char **argv)
{
    V3xPePlugin p;
    PeOpt o;
    memset(&o, 0, sizeof o);
    o.mf_flags = 1; o.mf_min_api = 6;
    CHECK(pe(&o, &p) == 0 && p.has_load && p.has_manifest && p.has_name, "pe: valid image: has_load, manifest, v3x_plugin_name (%s)", p.why);
    CHECK(!strcmp(p.name, "pei_plugin") && !strcmp(p.version, "1.2.3") && !strcmp(p.author, "tester") && !strcmp(p.desc, "a test plugin") &&
              !strcmp(p.builds, "release/1.15.1;unkn*") && !strcmp(p.caps, "hook, patch") && !strcmp(p.plugin_name, "pei_stem"),
          "pe: manifest strings and v3x_plugin_name read from the file (\"%s\" \"%s\" \"%s\")", p.name, p.caps, p.plugin_name);
    CHECK(p.mf_size == 64 && p.mf_flags == 1 && p.mf_min_api == 6 && p.mf_has_caps && !p.mf_builds_null, "pe: manifest numbers (size %u flags %u min_api %u)",
          p.mf_size, p.mf_flags, p.mf_min_api);
    /* an old manifest (v4/v5 header): no capabilities field */
    o.mf_size = 56;
    CHECK(pe(&o, &p) == 0 && !p.mf_has_caps && !p.caps[0] && !strcmp(p.builds, "release/1.15.1;unkn*"), "pe: struct_size 56 (older header) = no capabilities field read");
    o.mf_size = 48;
    CHECK(pe(&o, &p) == 0 && p.mf_builds_null && !p.mf_has_caps, "pe: struct_size 48: game_builds not covered = NULL");
    o.mf_size = 8;
    CHECK(pe(&o, &p) == -1, "pe: manifest struct_size below `name` refused");
    o.mf_size = 0;
    o.caps_null = 1; o.builds_null = 1;
    CHECK(pe(&o, &p) == 0 && p.mf_builds_null && !p.mf_has_caps && !p.caps[0], "pe: NULL game_builds / capabilities pointers = undeclared");
    o.caps_null = o.builds_null = 0;
    o.bad_str = 1;
    CHECK(pe(&o, &p) == -1, "pe: a manifest string pointing outside the image refuses the plugin");
    o.bad_str = 0; o.gap_str = 1;
    CHECK(pe(&o, &p) == -1, "pe: a string pointing into an RVA with no file data refuses the plugin");
    o.gap_str = 0; o.unterminated = 1;
    CHECK(pe(&o, &p) == -1, "pe: a string without a NUL inside the file data refuses the plugin");
    o.unterminated = 0;
    o.no_load = 1;
    CHECK(pe(&o, &p) == 0 && !p.has_load && p.has_manifest, "pe: no v3x_plugin_load export: has_load = 0 (the loader skips it before LoadLibrary)");
    o.no_load = 0; o.load_forwarder = 1;
    CHECK(pe(&o, &p) == 0 && !p.has_load, "pe: v3x_plugin_load as a forwarder string is not a function");
    o.load_forwarder = 0; o.no_manifest = 1; o.no_name = 1;
    CHECK(pe(&o, &p) == 0 && p.has_load && !p.has_manifest && !p.has_name && !p.name[0], "pe: no manifest / v3x_plugin_name exports");
    o.no_manifest = o.no_name = 0; o.no_exports = 1;
    CHECK(pe(&o, &p) == 0 && !p.has_load, "pe: no export directory: has_load = 0");
    o.no_exports = 0;
    /* long strings are cut, never in the middle of a UTF-8 character */
    char longname[200];
    memset(longname, 'n', 62);
    memcpy(longname + 62, "\xC3\xA9\xC3\xA9", 5);   /* 62 x 'n' + 2 two-byte characters */
    o.name = longname;
    CHECK(pe(&o, &p) == 0 && strlen(p.name) == 62 && !memcmp(p.name, longname, 62), "pe: a 66-byte name is cut to 62 bytes (no half character), got %zu", strlen(p.name));
    memset(longname, 'm', 150); longname[150] = 0;
    CHECK(pe(&o, &p) == 0 && strlen(p.name) == 63, "pe: a 150-byte name is cut to the field size (%zu)", strlen(p.name));
    o.name = NULL;

    /* broken headers: always -1, never a crash */
    CHECK(pe_mut(&o, 0, 0x4d4d, 2, &p) == -1, "pe: not MZ");
    CHECK(pe_mut(&o, 0x3c, 0x7fffff00u, 4, &p) == -1, "pe: e_lfanew far beyond the file");
    CHECK(pe_mut(&o, 0x3c, 0xffffffffu, 4, &p) == -1, "pe: e_lfanew = 0xffffffff");
    CHECK(pe_mut(&o, 0x40, 0x4551, 2, &p) == -1, "pe: wrong PE signature");
    CHECK(pe_mut(&o, 0x44, 0x014c, 2, &p) == -1 && !strstr(p.why, "ok"), "pe: x86 image refused (%s)", p.why);
    CHECK(pe_mut(&o, 0x56, 0x0022, 2, &p) == -1, "pe: not a DLL (no IMAGE_FILE_DLL)");
    CHECK(pe_mut(&o, 0x58, 0x010b, 2, &p) == -1, "pe: PE32 (not PE32+) optional header");
    CHECK(pe_mut(&o, 0x54, 0x20, 2, &p) == -1, "pe: SizeOfOptionalHeader too small");
    CHECK(pe_mut(&o, 0x54, 0xffff, 2, &p) == -1, "pe: SizeOfOptionalHeader beyond the file");
    CHECK(pe_mut(&o, 0x46, 0, 2, &p) == -1 && pe_mut(&o, 0x46, 97, 2, &p) == -1 && pe_mut(&o, 0x46, 0xffff, 2, &p) == -1,
          "pe: 0 or > 96 sections refused");
    CHECK(pe_mut(&o, 0x58 + 112, 0xf0000000u, 4, &p) == -1, "pe: export directory RVA outside every section");
    CHECK(pe_mut(&o, 0x200 + 24, 70000, 4, &p) == -1, "pe: 70000 export names refused");
    CHECK(pe_mut(&o, 0x200 + 28, 0x1ff0, 4, &p) == -1, "pe: export function table (RVA 0x1ff0, 12 bytes) crossing the section end");
    CHECK(pe_mut(&o, 0x200 + 36, 0xfffffff0u, 4, &p) == -1, "pe: export ordinal table RVA outside the sections");
    CHECK(pe_mut(&o, 0x200 + 0x60, 9, 2, &p) == 0 && !p.has_load, "pe: an ordinal >= NumberOfFunctions skips that name (v3x_plugin_load not found)");
    CHECK(pe_mut(&o, 0x200 + 0x40, 0x5000, 4, &p) == 0 && !p.has_load, "pe: v3x_plugin_load RVA beyond SizeOfImage is not a function");
    pe_mut(&o, 0x148 + 16, 0xffffffffu, 4, &p);   /* raw size 0xffffffff: any answer, no fault */
    CHECK(p.why != NULL, "pe: section raw size 0xffffffff parses without a fault (%s)", p.why);
    CHECK(pe_mut(&o, 0x148 + 20, 0xfffffff0u, 4, &p) == -1, "pe: section raw pointer beyond the file");
    pe_mut(&o, 0x200 + 0x100, 0x7fffffffu, 4, &p);   /* huge manifest struct_size: clamped, no crash */
    CHECK(p.mf_size == 0x7fffffffu, "pe: huge manifest struct_size is clamped internally (%s)", p.why);

    /* truncation: every length of the image, parsed from a buffer that ends at a guard page */
    uint8_t img[PEI_SIZE];
    size_t full = pei_build(img, &o);
    int trunc_bad = 0, trunc_load = 0;
    for (size_t n = 0; n < full; n++) {
        uint8_t *g = (uint8_t *)guard_copy(img, n);
        int r = v3x_pe_read_plugin(g, n, &p);
        guard_free(g, n);
        if (n < 0x200 + 0x7f && r == 0 && p.has_load) trunc_load++;
        if (r != 0 && r != -1) trunc_bad++;
    }
    CHECK(!trunc_bad && !trunc_load, "pe: all %zu truncations parse without a fault; a cut file never reports v3x_plugin_load", full);
    /* single-byte corruption sweep (every byte x 3 values): no fault */
    size_t sweep = 0;
    for (size_t at = 0; at < full; at++)
        for (int v = 0; v < 3; v++) {
            uint8_t cp[PEI_SIZE];
            memcpy(cp, img, full);
            cp[at] = v == 0 ? 0x00 : v == 1 ? 0xff : (uint8_t)(cp[at] ^ 0x80);
            uint8_t *g = (uint8_t *)guard_copy(cp, full);
            (void)v3x_pe_read_plugin(g, full, &p);
            guard_free(g, full);
            sweep++;
        }
    CHECK(sweep == full * 3, "pe: %zu single-byte corruptions parse without a fault", sweep);
    CHECK(v3x_pe_read_plugin(NULL, 0, &p) == -1 && v3x_pe_read_plugin((const uint8_t *)"MZ", 2, &p) == -1, "pe: NULL / 2 bytes");

    /* real plugin DLLs given on the command line must parse the same way the OS maps them */
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { CHECK(0, "pe: cannot open %s", argv[i]); continue; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *d = (uint8_t *)malloc((size_t)sz);
        size_t got = fread(d, 1, (size_t)sz, f);
        fclose(f);
        uint8_t *g = (uint8_t *)guard_copy(d, got);
        int r = v3x_pe_read_plugin(g, got, &p);
        CHECK(r == 0 && p.has_load, "pe: real plugin %s: parsed (%s), v3x_plugin_load found, name \"%s\" version \"%s\"", argv[i], p.why, p.name, p.version);
        /* every truncation of the real file in 4 KiB steps plus the boundaries */
        int bad = 0;
        for (size_t n = 0; n < got; n += (n < 2048 ? 61 : 4093)) {
            uint8_t *t = (uint8_t *)guard_copy(d, n);
            int rr = v3x_pe_read_plugin(t, n, &p);
            guard_free(t, n);
            bad += rr != 0 && rr != -1;
        }
        CHECK(!bad, "pe: truncated copies of %s parse without a fault", argv[i]);
        guard_free(g, got);
        free(d);
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_pattern();
    test_caps();
    test_lists();
    test_sha();
    test_pe(argc, argv);
    printf("%d checks, %d failed\n%s\n", checks, fails, fails ? "FAILED" : "ALL OK");
    return fails ? 1 : 0;
}
