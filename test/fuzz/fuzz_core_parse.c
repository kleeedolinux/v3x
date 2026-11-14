/* Fuzz harnesses of the loader's pre-LoadLibrary parsers (loader 0.6.0): src/core_parse.h (strict IDA
 * pattern, manifest capabilities, signed plugins.json, per-user trusted_plugins.txt) and src/v3x_pe.h
 * (bounded PE reader over a plugin DLL file). Every input goes to the parser as (pointer, length) in a
 * buffer that ends at a PAGE_NOACCESS page, so a read past the length is a crash, not a silent success. */
#include "v3x_fuzz.h"
#include "../../src/core_parse.h"
#include "../../src/v3x_pe.h"
#include "../core_pe_img.h"

#define HEX_A "1111111111111111111111111111111111111111111111111111111111111111"

/* ---- cp_pattern_parse: invariants of the result */
static void fuzz_core_pattern(const uint8_t *d, size_t n)
{
    const char *s = (const char *)v3x_fuzz_guarded(d, n);
    uint8_t by[32], mk[32];
    memset(by, 0xAA, sizeof by);
    memset(mk, 0xAA, sizeof mk);
    int r = cp_pattern_parse(s, n, by, mk, 16);
    V3X_FUZZ_ASSERT(r >= -2 && r <= 16);
    for (int i = 16; i < 32; i++) V3X_FUZZ_ASSERT(by[i] == 0xAA && mk[i] == 0xAA);   /* never writes past cap */
    for (int i = 0; i < r; i++) V3X_FUZZ_ASSERT(mk[i] <= 1 && (mk[i] || by[i] == 0));
    if (r > 0) {
        /* the same tokens re-rendered must parse to the same bytes */
        char txt[16 * 3 + 1];
        size_t k = 0;
        for (int i = 0; i < r; i++) k += (size_t)snprintf(txt + k, sizeof txt - k, mk[i] ? "%02X " : "?? ", by[i]);
        uint8_t b2[16], m2[16];
        V3X_FUZZ_ASSERT(cp_pattern_parse(txt, k, b2, m2, 16) == r && !memcmp(by, b2, (size_t)r) && !memcmp(mk, m2, (size_t)r));
    }
    uint8_t b3[300], m3[300];
    (void)cp_pattern_parse(s, n, b3, m3, 256);
}
V3X_FUZZ_TARGET(core_pattern, fuzz_core_pattern)
V3X_FUZZ_SEED(core_pattern, "48 8B ?? 05 ? FF 15 ?? ?? ?? ??")
V3X_FUZZ_SEED(core_pattern, "  \t90\t\tcc 0x90 ZZ ??? 1 ")
V3X_FUZZ_SEED(core_pattern, "")

/* ---- cp_caps_parse */
static void fuzz_core_caps(const uint8_t *d, size_t n)
{
    const char *s = (const char *)v3x_fuzz_guarded(d, n);
    int unk = -1;
    char first[12];
    uint32_t m = cp_caps_parse(s, n, &unk, first, sizeof first);
    V3X_FUZZ_ASSERT((m & ~CP_CAP_ALL) == 0 && unk >= 0 && strlen(first) < sizeof first);
    if (!unk) V3X_FUZZ_ASSERT(!first[0]);
    uint32_t m2 = cp_caps_parse(s, n, NULL, NULL, 0);   /* out-params are optional */
    V3X_FUZZ_ASSERT(m2 == m);
}
V3X_FUZZ_TARGET(core_caps, fuzz_core_caps)
V3X_FUZZ_SEED(core_caps, "hook, patch;fs\tnet script exec")
V3X_FUZZ_SEED(core_caps, "HOOK bogus,;;  Patch aVeryLongUnknownWordThatIsCut")
V3X_FUZZ_SEED(core_caps, "")

/* ---- cp_official_has / cp_allow_has: the file name and hash come from the input as well */
static void fuzz_core_lists(const uint8_t *d, size_t n)
{
    const char *t = (const char *)v3x_fuzz_guarded(d, n);
    static const char *const files[] = {"hello.dll", "HELLO.DLL", "a.dll", "", "p0.dll"};
    static const char *const shas[] = {HEX_A, "abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd",
                                       "ABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCD"};
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++)
        for (size_t k = 0; k < sizeof shas / sizeof shas[0]; k++) {
            int r = cp_official_has(t, n, files[i], shas[k]);
            V3X_FUZZ_ASSERT(r == 0 || r == 1);
            if (r) V3X_FUZZ_ASSERT(n >= 64);                  /* a match needs the 64 digits in the text */
        }
    for (size_t k = 0; k < sizeof shas / sizeof shas[0]; k++) {
        int r = cp_allow_has(t, n, shas[k]);
        V3X_FUZZ_ASSERT(r == 0 || r == 1);
        if (r) V3X_FUZZ_ASSERT(n >= 64);
    }
    /* a hash taken from the input itself (64 bytes at offset 0): may match, must not fault */
    if (n >= 64) {
        char h[65];
        for (int i = 0; i < 64; i++) h[i] = "0123456789abcdef"[(unsigned char)d[i] & 15];
        h[64] = 0;
        (void)cp_official_has(t, n, "hello.dll", h);
        (void)cp_allow_has(t, n, h);
    }
}
V3X_FUZZ_TARGET(core_lists, fuzz_core_lists)
V3X_FUZZ_SEED(core_lists, "{\"schema\":1,\"v3x_version\":\"0.6.0\",\"plugins\":[{\"name\":\"hello\",\"file\":\"plugins/hello.dll\",\"sha256\":\"" HEX_A "\",\"version\":\"0.6.0\"},{\"file\":\"a.dll\",\"sha256\":\"abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd\"}]}")
V3X_FUZZ_SEED(core_lists, "# my plugins\r\n" HEX_A " hello.dll\r\n\r\n  ABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCD\tx.dll\r\n")
V3X_FUZZ_SEED(core_lists, "{\"plugins\":[{\"file\":\"a\\\\b.dll\",\"x\":{\"y\":[1]}},{\"file\"")

/* ---- v3x_pe_read_plugin over a random buffer; the seeds are valid images */
static void fuzz_core_pe(const uint8_t *d, size_t n)
{
    const uint8_t *f = (const uint8_t *)v3x_fuzz_guarded(d, n);
    V3xPePlugin p;
    int r = v3x_pe_read_plugin(f, n, &p);
    V3X_FUZZ_ASSERT(r == 0 || r == -1);
    V3X_FUZZ_ASSERT(p.why != NULL);
    V3X_FUZZ_ASSERT(strlen(p.name) < sizeof p.name && strlen(p.version) < sizeof p.version && strlen(p.author) < sizeof p.author &&
                    strlen(p.desc) < sizeof p.desc && strlen(p.builds) < sizeof p.builds && strlen(p.caps) < sizeof p.caps &&
                    strlen(p.plugin_name) < sizeof p.plugin_name);
    char hex[65];
    v3x_sha256_hex(f, n, hex);
    V3X_FUZZ_ASSERT(strlen(hex) == 64);
}
V3X_FUZZ_TARGET(core_pe, fuzz_core_pe)

/* The PE seeds are binary: registered at startup from the image builder (V3X_FUZZ_SEED takes string
 * literals, which cannot hold the NUL bytes). */
#ifndef V3X_FUZZ_LIBFUZZER
static void add_pe_seeds(void) __attribute__((constructor));
static void add_pe_seeds(void)
{
    static uint8_t img[PEI_SIZE];
    PeOpt o;
    memset(&o, 0, sizeof o);
    size_t n = pei_build(img, &o);
    v3x_fuzz_add_seed("core_pe", img, n);
    o.mf_size = 56; o.builds = "*";
    n = pei_build(img, &o);
    v3x_fuzz_add_seed("core_pe", img, n);
    memset(&o, 0, sizeof o);
    o.no_load = 1; o.builds_null = 1;
    n = pei_build(img, &o);
    v3x_fuzz_add_seed("core_pe", img, n);
}
#endif
