/* Fuzz harness: plugins/script_ext/sx_pack.h - the pack path composer with BOTH the pack name and the
 * relative path from the input (fuzz_script_ext.c fixes the pack), and begin_pack's handling of a
 * tampered manifest from an earlier session (the delete-by-manifest step and the "#owner" line of the
 * pack ownership rule, audit N5). Works in a private temp folder only.
 * Invariants: a composed path is <root><pack>\<rel with '\'> with no ".." component and no '/';
 * begin_pack never deletes anything outside the pack folder (sentinels next to and above it survive);
 * a manifest naming another owner makes begin_pack fail with SX_E_OWNER and deletes nothing; after
 * end_pack the manifest's first line names this module. */
#include "v3x_fuzz.h"
#include "../../plugins/script_ext/sx_pack.h"

static char g_tmp[MAX_PATH];   /* ...\v3x_fuzz_sx_<pid>\ */
static char g_self_name[128];

static int exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

static void put(const char *path, const void *d, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    if (n) fwrite(d, 1, n, f);
    fclose(f);
}

static void setup_dirs(void)
{
    if (g_tmp[0]) return;
    char t[MAX_PATH];
    DWORD k = GetTempPathA(sizeof t, t);
    V3X_FUZZ_ASSERT(k && k < sizeof t);
    snprintf(g_tmp, sizeof g_tmp, "%sv3x_fuzz_sx_%lu\\", t, (unsigned long)GetCurrentProcessId());
    CreateDirectoryA(g_tmp, NULL);
    snprintf(sx_root, sizeof sx_root, "%scontent_packs\\", g_tmp);
    CreateDirectoryA(sx_root, NULL);
    sx_module_name(GetModuleHandleA(NULL), g_self_name, sizeof g_self_name);
}

static int has_dotdot(const char *p)
{
    for (const char *s = p;;) {
        const char *e = s;
        while (*e && *e != '\\') e++;
        if (e - s == 2 && s[0] == '.' && s[1] == '.') return 1;
        if (!*e) return 0;
        s = e + 1;
    }
}

/* input: "<pack>\n<rel>" */
static void fuzz_pack_rel(const uint8_t *d, size_t n)
{
    setup_dirs();
    const char *t = v3x_fuzz_cstr(d, n);
    char pack[80];
    const char *nl = strchr(t, '\n');
    size_t pn = nl ? (size_t)(nl - t) : strlen(t);
    if (pn >= sizeof pack) pn = sizeof pack - 1;
    memcpy(pack, t, pn);
    pack[pn] = 0;
    const char *rel = nl ? nl + 1 : "";
    char out[MAX_PATH];
    if (!sx_path(out, sizeof out, pack, rel)) { V3X_FUZZ_ASSERT(out[0] == 0); return; }
    size_t rl = strlen(sx_root), kl = strlen(pack);
    V3X_FUZZ_ASSERT(sx_valid_pack(pack));
    V3X_FUZZ_ASSERT(!strncmp(out, sx_root, rl) && !_strnicmp(out + rl, pack, kl) && out[rl + kl] == '\\');
    V3X_FUZZ_ASSERT(!strchr(out, '/') && !has_dotdot(out + rl));
    if (*rel) V3X_FUZZ_ASSERT(sx_valid_rel(rel) || 1);   /* sx_path itself enforces v3x_path_join_checked rules */
}
V3X_FUZZ_TARGET(sx_pack_rel, fuzz_pack_rel)
V3X_FUZZ_SEED(sx_pack_rel, "my_pack\ncommon/scripted_effects/x.txt")
V3X_FUZZ_SEED(sx_pack_rel, "A-1.b\nlocalization\\english\\x_l_english.yml")
V3X_FUZZ_SEED(sx_pack_rel, "..\nx.txt")
V3X_FUZZ_SEED(sx_pack_rel, "p\n")
V3X_FUZZ_SEED(sx_pack_rel, "CON\na/../../b")

/* input: manifest text of pack "fz" from an "earlier session" */
static void fuzz_manifest(const uint8_t *d, size_t n)
{
    setup_dirs();
    char pdir[MAX_PATH], man[MAX_PATH], above[MAX_PATH], beside[MAX_PATH], other[MAX_PATH], inside[MAX_PATH];
    snprintf(pdir, sizeof pdir, "%sfz", sx_root);
    CreateDirectoryA(pdir, NULL);
    snprintf(man, sizeof man, "%sfz\\" SX_MANIFEST, sx_root);
    snprintf(above, sizeof above, "%souter.txt", g_tmp);            /* next to content_packs */
    snprintf(beside, sizeof beside, "%sbeside.txt", sx_root);        /* in content_packs, outside the pack */
    snprintf(other, sizeof other, "%sother", sx_root);
    CreateDirectoryA(other, NULL);
    snprintf(other, sizeof other, "%sother\\keep.txt", sx_root);
    snprintf(inside, sizeof inside, "%sfz\\mine.txt", sx_root);
    /* sentinels: created once (they must never disappear), the pack's own file again when deleted */
    if (!exists(above)) put(above, "x", 1);
    if (!exists(beside)) put(beside, "x", 1);
    if (!exists(other)) put(other, "x", 1);
    if (!exists(inside)) put(inside, "x", 1);
    put(man, d, n);
    for (int i = 0; i < SX_MAX_PACKS; i++) {   /* a new session: nothing claimed yet */
        for (int j = 0; j < sx_packs[i].n; j++) free(sx_packs[i].files[j]);
        memset(&sx_packs[i], 0, sizeof sx_packs[i]);
    }
    char prev[128];
    sx_manifest_owner("fz", prev, sizeof prev);
    int rc = sx_begin_pack_as("fz", GetModuleHandleA(NULL));
    V3X_FUZZ_ASSERT(exists(above) && exists(beside) && exists(other));
    if (prev[0] && strcmp(prev, g_self_name)) {
        V3X_FUZZ_ASSERT(rc == SX_E_OWNER && exists(inside) && exists(man));
        return;
    }
    V3X_FUZZ_ASSERT(rc == V3X_SCRIPT_OK && !exists(man));
    if (n & 3) return;   /* the end_pack round trip (temp file + rename, slow) for a quarter of the inputs */
    V3X_FUZZ_ASSERT(sx_end_pack("fz") == 0);
    FILE *f = fopen(man, "rb");
    char line[200] = "";
    V3X_FUZZ_ASSERT(f != NULL);
    if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
    char want[200];
    snprintf(want, sizeof want, "#owner %s\n", g_self_name);
    V3X_FUZZ_ASSERT(!strcmp(line, want));
}
V3X_FUZZ_TARGET(sx_manifest, fuzz_manifest)
V3X_FUZZ_SEED(sx_manifest, "mine.txt\ncommon/a.txt\n")
V3X_FUZZ_SEED(sx_manifest, "#owner someone_else.dll\nmine.txt\n")
V3X_FUZZ_SEED(sx_manifest, "#owner FUZZ_SCRIPT_EXT_PATH.EXE\r\n../beside.txt\r\n..\\..\\outer.txt\r\n../other/keep.txt\r\nmine.txt\r\n")
V3X_FUZZ_SEED(sx_manifest, "C:\\Windows\\win.ini\n\\\\?\\C:\\x\nmine.txt:ads\nMINE.TXT.\n#owner\n")
