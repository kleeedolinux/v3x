/* Fuzz harness: plugins/par_load/pl_core.c - the blend-shape prefetch list reader (pl_pf_load, through
 * a temp file; the list is a cache under binaries/v3x and not trusted) and the L5 path gate
 * (pl_pf_path_ok). Invariants: every loaded native path is NULL or passes pl_pf_path_ok for the
 * configured roots (so UNC / device / ".." / alternate-stream paths are never opened); vfs strings
 * are NUL-terminated; at most PL_PF_MAX entries. Nothing is ever read from the listed paths here. */
#include "v3x_fuzz.h"
#include "../../plugins/par_load/pl_core.c"

static char g_tmp[MAX_PATH];

static int has_dotdot(const char *p)          /* a ".." component */
{
    for (const char *s = p;;) {
        const char *e = s;
        while (*e && *e != '/' && *e != '\\') e++;
        if (e - s == 2 && s[0] == '.' && s[1] == '.') return 1;
        if (!*e) return 0;
        s = e + 1;
    }
}

static void fuzz_pf(const uint8_t *d, size_t n)
{
    if (!g_tmp[0]) {
        char t[MAX_PATH];
        DWORD k = GetTempPathA(sizeof t, t);
        snprintf(g_tmp, sizeof g_tmp, "%sv3x_fuzz_pl_%lu.txt", k && k < sizeof t ? t : ".\\", GetCurrentProcessId());
    }
    FILE *f = fopen(g_tmp, "wb");
    if (!f) return;
    if (n) fwrite(d, 1, n, f);
    fclose(f);
    static pl_pf p;
    pl_pf_reset(&p);
    pl_pf_add_root(&p, "C:\\Games\\Victoria 3");
    pl_pf_add_root(&p, "D:/SteamLibrary/steamapps/workshop/content/529340");
    int k = pl_pf_load(&p, g_tmp);
    V3X_FUZZ_ASSERT(k >= 0 && k <= PL_PF_MAX && k == p.n);
    for (int i = 0; i < p.n; i++) {
        V3X_FUZZ_ASSERT(p.e[i].vfs != NULL);
        if (p.e[i].native) {
            const char *s = p.e[i].native;
            V3X_FUZZ_ASSERT(pl_pf_path_ok(&p, s));
            V3X_FUZZ_ASSERT(!(s[0] == '\\' && s[1] == '\\') && !has_dotdot(s) && !strchr(s + 2, ':') && !strpbrk(s, "<>\"|?*"));
        }
    }
    pl_pf_reset(&p);
    DeleteFileA(g_tmp);
}
V3X_FUZZ_TARGET(pl_prefetch, fuzz_pf)
V3X_FUZZ_SEED(pl_prefetch, "# par_load blend-shape prefetch list: size<TAB>vfs path<TAB>native path (engine read order)\n"
                           "1234\tgfx/models/a.mesh\tC:\\Games\\Victoria 3\\game\\gfx\\models\\a.mesh\n"
                           "-1\tgfx/b.mesh\t\n"
                           "77\tgfx/c.mesh\t\\\\server\\share\\c.mesh\n"
                           "5\tgfx/d.mesh\tC:\\Games\\Victoria 3\\..\\..\\Windows\\win.ini\n"
                           "9\tgfx/e.mesh\tD:/SteamLibrary/steamapps/workshop/content/529340/3385/gfx/e.mesh\r\n")

static void fuzz_path(const uint8_t *d, size_t n)
{
    const char *s = v3x_fuzz_cstr(d, n);
    static pl_pf p;
    p.nroots = 0;
    pl_pf_add_root(&p, "C:\\Games\\Victoria 3\\");
    if (n > 3 && (d[0] & 1)) pl_pf_add_root(&p, s);          /* a fuzzed root, too */
    if (pl_pf_path_ok(&p, s)) {
        V3X_FUZZ_ASSERT(((s[0] | 32) >= 'a' && (s[0] | 32) <= 'z') && s[1] == ':' && (s[2] == '\\' || s[2] == '/'));
        V3X_FUZZ_ASSERT(!strchr(s + 2, ':') && !strstr(s, "\\\\") && !strstr(s, "//"));
        for (const char *c = s; *c; c++) V3X_FUZZ_ASSERT((unsigned char)*c >= 0x20);
    }
}
V3X_FUZZ_TARGET(pl_path, fuzz_path)
V3X_FUZZ_SEED(pl_path, "C:\\Games\\Victoria 3\\game\\gfx\\a.mesh")
V3X_FUZZ_SEED(pl_path, "\\\\?\\C:\\Games\\Victoria 3\\x")
V3X_FUZZ_SEED(pl_path, "c:/games/victoria 3/../x")
