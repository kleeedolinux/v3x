/* Unit test for plugins/content_packs (no game needed): #includes the plugin, checks the signature
 * decoders on synthetic buffers using the real opcode sequences of MountData (rva 0x3e17820) and the
 * scan/order/mount logic against a fake mount function on a temp folder tree.
 * Usage: content_packs_test.exe <empty temp dir>     exit 0 = ALL OK */
#include "../plugins/content_packs/content_packs.c"

static int fails;
static void check(int ok, const char *what) { printf("%-60s %s\n", what, ok ? "OK" : "FAIL"); fails += !ok; }

static char calls[16][MAX_PATH * 2];
static int ncalls, nrefresh, fail_name_b;
static int fake_mount(void *u, const char *dir, const char *mp)
{
    (void)u;
    if (mp) return 0;
    snprintf(calls[ncalls++], sizeof calls[0], "%s", dir);
    return !(fail_name_b && strstr(dir, "/b_pack"));
}
static void fake_refresh(void) { nrefresh++; }

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: %s <empty temp dir>\n", argv[0]); return 2; }
    /* --- decoders: synthetic function with real instruction bytes ---------------------------- */
    uint8_t fn[0x200] = {0};
    /* at 0x10: xor r8d,r8d ; call +0x100  -> target = 0x10+3+5+0x100 = 0x118 */
    memcpy(fn + 0x10, "\x45\x33\xC0\xE8\x00\x01\x00\x00", 8);
    /* at 0x40: test r15d,r15d ; jne +0x20 -> X = 0x40+9+0x20 = 0x69 ; X: call +0x50 -> 0x69+5+0x50 = 0xBE */
    memcpy(fn + 0x40, "\x45\x85\xFF\x0F\x85\x20\x00\x00\x00", 9);
    memcpy(fn + 0x69, "\xE8\x50\x00\x00\x00", 5);
    const uint8_t *lo = fn, *hi = fn + sizeof fn;          /* the "code range" of this synthetic image */
    check(cp_resolve_mount(fn, sizeof fn, lo, hi) == fn + 0x118, "mount = xor r8d,r8d; call target");
    check(cp_resolve_refresh(fn, sizeof fn, lo, hi) == fn + 0xBE, "refresh = call at jne target");
    check(cp_resolve_mount(fn, 0x12, lo, hi) == NULL, "mount: pattern cut by scan length -> NULL");
    check(cp_resolve_mount(fn, sizeof fn, lo, fn + 0x110) == NULL && cp_resolve_refresh(fn, sizeof fn, lo, fn + 0xB0) == NULL,
          "L8: call targets outside the code range -> NULL");
    memcpy(fn + 0x45, "\x00\x00\x00\x40", 4);              /* jne +0x40000000: X far outside, never read */
    check(cp_resolve_refresh(fn, sizeof fn, lo, hi) == NULL, "L8: computed jne target outside the code range -> NULL, not dereferenced");
    memcpy(fn + 0x45, "\x20\x00\x00\x00", 4);
    check(cp_resolve_mount(fn, sizeof fn, NULL, NULL) == NULL && cp_resolve_refresh(fn, sizeof fn, NULL, NULL) == NULL,
          "L8: no code range known -> NULL");
    fn[0x69] = 0x90;
    check(cp_resolve_refresh(fn, sizeof fn, lo, hi) == NULL, "refresh: jne target not a call -> NULL");
    uint8_t none[64] = {0};
    check(cp_resolve_mount(none, sizeof none, none, none + 64) == NULL && cp_resolve_refresh(none, sizeof none, none, none + 64) == NULL,
          "no patterns -> NULL");

    /* --- scan + mount order on a temp tree ---------------------------------------------------- */
    snprintf(g_root, sizeof g_root, "%s\\content_packs", argv[1]);
    cp_slashes(g_root);
    const char *dirs[] = {"", "/b_pack", "/A_pack", "/c_pack", "/_disabled", "/.hidden"};
    char p[MAX_PATH * 2];
    for (int i = 0; i < 6; i++) { snprintf(p, sizeof p, "%s%s", g_root, dirs[i]); CreateDirectoryA(p, NULL); }
    snprintf(p, sizeof p, "%s/file.txt", g_root);
    FILE *f = fopen(p, "w"); if (f) { fputs("x", f); fclose(f); }

    g_mount = fake_mount; g_refresh = fake_refresh;
    int n = cp_mount_all();
    check(n == 3 && ncalls == 3, "3 packs mounted (skips _x, .x, files)");
    char exp[MAX_PATH * 2];
    snprintf(exp, sizeof exp, "%s/A_pack", g_root);
    check(ncalls == 3 && strcmp(calls[0], exp) == 0, "order: A_pack first (lowest priority)");
    check(ncalls == 3 && strstr(calls[1], "/b_pack") && strstr(calls[2], "/c_pack"), "order: b_pack, c_pack last (top)");
    check(!strchr(calls[0], '\\') && calls[0][strlen(calls[0]) - 1] != '/', "paths: forward slashes, no trailing slash");
    check(nrefresh == 1, "refresh called once after mounting");

    ncalls = nrefresh = 0; fail_name_b = 1;
    n = cp_mount_all();
    check(n == 2 && ncalls == 3 && nrefresh == 1, "one failing mount: others still mounted, refresh once");

    /* --- order.txt: top = highest priority; unlisted below alphabetically; junk ignored ------- */
    ncalls = nrefresh = 0; fail_name_b = 0;
    snprintf(p, sizeof p, "%s/order.txt", g_root);
    f = fopen(p, "wb");
    if (f) { fputs("\xEF\xBB\xBF# my order\r\n  c_pack/ \r\n\r\nmissing_pack\nA_PACK\n_disabled\n", f); fclose(f); }
    n = cp_mount_all();
    check(n == 3 && ncalls == 3 && g_ordered == 2, "order.txt: 3 mounted, 2 listed names matched");
    check(ncalls == 3 && strstr(calls[0], "/b_pack") && strstr(calls[1], "/A_pack") && strstr(calls[2], "/c_pack"),
          "order.txt: unlisted b first, then A, c_pack (top line) last");
    f = fopen(p, "wb");
    if (f) { fputs("# empty\n", f); fclose(f); }
    ncalls = 0;
    cp_mount_all();
    check(g_ordered == 0 && ncalls == 3 && strstr(calls[0], "/A_pack") && strstr(calls[2], "/c_pack"),
          "order.txt with no names: alphabetical");
    DeleteFileA(p);
    ncalls = 0;
    cp_mount_all();
    check(g_ordered == -1 && ncalls == 3 && strstr(calls[0], "/A_pack"), "order.txt removed: alphabetical");

    ncalls = nrefresh = 0; fail_name_b = 0;
    snprintf(g_root, sizeof g_root, "%s/does_not_exist", argv[1]);
    cp_slashes(g_root);
    n = cp_mount_all();
    check(n == 0 && ncalls == 0 && nrefresh == 0, "missing root: nothing mounted, no refresh");

    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails ? 1 : 0;
}
