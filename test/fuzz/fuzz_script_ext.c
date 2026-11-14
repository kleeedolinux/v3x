/* Fuzz harness: plugins/script_ext/sx_pack.h - sx_valid_rel / sx_valid_pack (the only gate between a
 * plugin-supplied relative path and a file write / delete under binaries/v3x/content_packs) and the
 * path composer sx_path. Invariants: an accepted rel has no "." / ".." component, no ':' / drive /
 * leading or doubled separator, no reserved device name, is printable ASCII <= 200 chars, is not the
 * manifest; the composed path starts with the pack root + pack + '\\' and contains no "..". */
#include "v3x_fuzz.h"
#include "../../plugins/script_ext/sx_pack.h"

static int has_comp(const char *p, const char *c)
{
    size_t cn = strlen(c);
    for (const char *s = p;;) {
        const char *e = s;
        while (*e && *e != '/' && *e != '\\') e++;
        if ((size_t)(e - s) == cn && !_strnicmp(s, c, cn)) return 1;
        if (!*e) return 0;
        s = e + 1;
    }
}

static void fuzz_rel(const uint8_t *d, size_t n)
{
    const char *rel = v3x_fuzz_cstr(d, n);
    if (sx_valid_rel(rel)) {
        size_t L = strlen(rel);
        V3X_FUZZ_ASSERT(L >= 1 && L <= 200 && rel[0] != '/' && rel[0] != '\\' && !strchr(rel, ':'));
        V3X_FUZZ_ASSERT(!has_comp(rel, "..") && !has_comp(rel, ".") && !strstr(rel, "//") && !strstr(rel, "\\\\"));
        V3X_FUZZ_ASSERT(!has_comp(rel, "con") && !has_comp(rel, "nul") && !has_comp(rel, "aux") && !has_comp(rel, "prn"));
        for (const char *p = rel; *p; p++) V3X_FUZZ_ASSERT(*p >= 0x20 && *p <= 0x7e);
        V3X_FUZZ_ASSERT(_stricmp(rel, SX_MANIFEST) != 0);
        /* compose (no reparse check hits: the root does not exist) */
        snprintf(sx_root, sizeof sx_root, "Z:\\v3x_fuzz_no_such_dir\\content_packs\\");
        char out[MAX_PATH];
        if (sx_path(out, sizeof out, "pack", rel)) {
            V3X_FUZZ_ASSERT(!strncmp(out, "Z:\\v3x_fuzz_no_such_dir\\content_packs\\pack\\", 43) && !strstr(out, "..\\") && !strchr(out, '/'));
        }
    }
    (void)sx_valid_pack(rel);
    (void)sx_needs_bom(rel, rel, strlen(rel));
}
V3X_FUZZ_TARGET(sx_rel, fuzz_rel)
V3X_FUZZ_SEED(sx_rel, "common/scripted_effects/v3x_test.txt")
V3X_FUZZ_SEED(sx_rel, "localization/english/x_l_english.yml")
V3X_FUZZ_SEED(sx_rel, "a/../b.txt")
V3X_FUZZ_SEED(sx_rel, "events\\COM1.txt")
V3X_FUZZ_SEED(sx_rel, "gfx/x.dds:stream")
