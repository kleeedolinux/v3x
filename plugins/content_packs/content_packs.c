/* content_packs: mounts extra content folders into the game's virtual file system (PhysFS search path).
 *
 * Every sub-folder of <game>/binaries/v3x/content_packs/ is mounted at "/" with the highest priority,
 * so a file there (e.g. content_packs/my_pack/gfx/foo.dds) overrides game/gfx/foo.dds.
 * Packs are mounted in alphabetical order; each mount is prepended, so the LAST name wins on conflicts.
 * Optional content_packs/order.txt (one pack name per line, top = highest priority, '#' comments)
 * overrides that: listed packs go on top in file order, unlisted packs stay below them alphabetically.
 * Folders starting with '_' or '.' are skipped (disable a pack by renaming it to _my_pack).
 *
 * Scope: only adds search-path entries. It does not touch DLC/mod lists, ownership or checksums.
 *
 * How it hooks (RE notes: docs/re/vfs.md, offline check: tools/re_vfs_resolve.py):
 *   vfs_init   = function referencing "../platform_specific_game_data" (mounts platform data + game/).
 *                Hooked; packs are mounted right after it returns (so they sit above game/).
 *   mount      = int mount(void *unused, const char *dir, const char *mount_point) - PhysFS mount
 *                that prepends; found as the "xor r8d,r8d; call" inside MountData (the function
 *                referencing "No subdirs mounted for game dir from candidates: {}").
 *                Returns non-zero on success; mounting an already mounted dir is a no-op success.
 *   refresh    = void refresh(void) - rebuilds the cached search-path list; the target of
 *                "test r15d,r15d; jne <call refresh>" in MountData. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v3x.h"
#include "../../sdk/v3x_hook.h"   /* V3X_PASS8: forward 8 integer slots, whatever the real arg count */

V3X_EXPORT const char *v3x_plugin_name = "content_packs";

#define TAG "content_packs"
#define MAX_PACKS 64
#define SCAN_BYTES 0x600

typedef int (*mount_fn)(void *unused, const char *dir, const char *mount_point);
typedef void (*refresh_fn)(void);

static const V3X_API *api;
static mount_fn g_mount;
static refresh_fn g_refresh;
static v3x_pass8_fn g_init_orig;
static char g_root[MAX_PATH * 2];              /* .../binaries/v3x/content_packs (forward slashes) */
static char g_packs[MAX_PACKS][MAX_PATH * 2];  /* full paths, forward slashes, no trailing slash */
static int g_npacks, g_mounted, g_init_calls;
static int g_ordered = -1;                    /* -1 no order.txt, else number of listed packs used */

/* ---- pure helpers (unit-tested in test/content_packs_test.c) ---------------------------------- */

/* Address of the first occurrence of `pat` in [fn, fn+n), or NULL. */
static const uint8_t *cp_find(const uint8_t *fn, size_t n, const uint8_t *pat, size_t plen)
{
    for (size_t i = 0; i + plen <= n; i++)
        if (memcmp(fn + i, pat, plen) == 0) return fn + i;
    return NULL;
}

static const uint8_t *cp_rel32(const uint8_t *at, size_t insn_end_off)
{
    int32_t r;
    memcpy(&r, at, 4);
    return at + insn_end_off + r;
}

/* mount = target of `45 33 C0 E8 rel32` (xor r8d,r8d ; call mount) */
static const void *cp_resolve_mount(const uint8_t *fn, size_t n)
{
    static const uint8_t pat[] = {0x45, 0x33, 0xC0, 0xE8};
    const uint8_t *p = cp_find(fn, n, pat, sizeof pat);
    return p && (size_t)(p - fn) + 8 <= n ? cp_rel32(p + 4, 4) : NULL;
}

/* refresh = call target at the jump target of `45 85 FF 0F 85 rel32` (test r15d,r15d ; jne X ; X: call refresh) */
static const void *cp_resolve_refresh(const uint8_t *fn, size_t n)
{
    static const uint8_t pat[] = {0x45, 0x85, 0xFF, 0x0F, 0x85};
    const uint8_t *p = cp_find(fn, n, pat, sizeof pat);
    if (!p || (size_t)(p - fn) + 9 > n) return NULL;
    const uint8_t *x = cp_rel32(p + 5, 4);
    return x[0] == 0xE8 ? cp_rel32(x + 1, 4) : NULL;
}

static void cp_slashes(char *s)
{
    for (; *s; s++)
        if (*s == '\\') *s = '/';
}

static int cp_cmp(const void *a, const void *b) { return _stricmp((const char *)a, (const char *)b); }

/* Reorders names[0..n) by <root>/order.txt if present (one pack name per line, top = highest
 * priority; blank lines and '#' comments ignored, names case-insensitive). Result is MOUNT order
 * (lowest priority first): unlisted packs alphabetically, then listed packs bottom-to-top.
 * Listed names that are not existing packs are ignored. Returns number of listed packs used, or -1
 * when there is no order.txt. */
static int cp_apply_order(const char *root, char (*names)[MAX_PATH], int n)
{
    char path[MAX_PATH * 2 + 16], line[MAX_PATH + 8];
    snprintf(path, sizeof path, "%s/order.txt", root);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int rank[MAX_PACKS], nlisted = 0;  /* rank[i] = line index (0 = top) or -1 if unlisted */
    for (int i = 0; i < n; i++) rank[i] = -1;
    while (fgets(line, sizeof line, f)) {
        char *b = line, *e;
        while (*b == ' ' || *b == '\t') b++;
        if ((unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF) b += 3;
        e = b + strlen(b);
        while (e > b && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t' || e[-1] == '/' || e[-1] == '\\')) *--e = 0;
        if (!*b || *b == '#') continue;
        for (int i = 0; i < n; i++)
            if (rank[i] < 0 && _stricmp(names[i], b) == 0) { rank[i] = nlisted++; break; }
    }
    fclose(f);
    char out[MAX_PACKS][MAX_PATH];
    int k = 0;
    for (int i = 0; i < n; i++)
        if (rank[i] < 0) memcpy(out[k++], names[i], MAX_PATH);
    for (int r = nlisted - 1; r >= 0; r--)
        for (int i = 0; i < n; i++)
            if (rank[i] == r) memcpy(out[k++], names[i], MAX_PATH);
    memcpy(names, out, sizeof(out[0]) * n);
    return nlisted;
}

/* Fills g_packs (full paths, mount order) from sub-folders of `root` (forward-slash path, no
 * trailing slash), sorted alphabetically or by order.txt. Returns count. */
static int cp_scan(const char *root)
{
    static char names[MAX_PACKS][MAX_PATH];
    char pat[MAX_PATH * 2 + 4];
    WIN32_FIND_DATAA fd;
    int n = 0;
    g_npacks = 0;
    snprintf(pat, sizeof pat, "%s/*", root);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.' || fd.cFileName[0] == '_') continue;
        if (n == MAX_PACKS) break;
        snprintf(names[n++], sizeof names[0], "%s", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(names, n, sizeof names[0], cp_cmp);
    g_ordered = cp_apply_order(root, names, n);
    for (int i = 0; i < n; i++) snprintf(g_packs[i], sizeof g_packs[0], "%s/%s", root, names[i]);
    return g_npacks = n;
}

/* Mounts all packs in order (each prepends, so the last one ends up on top) and refreshes once. */
static int cp_mount_all(void)
{
    int ok = 0;
    cp_scan(g_root);
    for (int i = 0; i < g_npacks; i++) {
        int r = g_mount(NULL, g_packs[i], NULL);
        if (r) ok++;
        if (api) api->log(TAG, "mount %s -> %s", g_packs[i], r ? "ok" : "FAILED");
    }
    if (ok && g_refresh) g_refresh();
    g_mounted = ok;
    return ok;
}

/* ---- runtime --------------------------------------------------------------------------------- */

static uint64_t init_detour(V3X_PASS8_ARGS)
{
    uint64_t r = g_init_orig(V3X_PASS8);
    g_init_calls++;
    int n = cp_mount_all();
    api->log(TAG, "vfs init #%d done: %d/%d pack(s) mounted from %s", g_init_calls, n, g_npacks, g_root);
    return r;
}

static void cmd_packs(const char *args, void *user)
{
    (void)args, (void)user;
    api->print("content_packs: root %s, vfs init calls %d, mounted %d, order.txt %s", g_root, g_init_calls,
               g_mounted, g_ordered < 0 ? "absent" : "used");
    for (int i = g_npacks - 1; i >= 0; i--) /* top priority first */
        api->print("  [%d] %s", i, g_packs[i]);
}

static void *fn_by_string(const char *s)
{
    void *str = api->find_string(s);
    void *x = str ? api->find_xref(str, 0) : NULL;
    return x ? api->function_start(x) : NULL;
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    snprintf(g_root, sizeof g_root, "%scontent_packs", api->v3x_dir());
    cp_slashes(g_root);
    CreateDirectoryA(g_root, NULL);

    void *init = fn_by_string("../platform_specific_game_data");
    const uint8_t *md = fn_by_string("No subdirs mounted for game dir from candidates: {}");
    g_mount = md ? (mount_fn)cp_resolve_mount(md, SCAN_BYTES) : NULL;
    g_refresh = md ? (refresh_fn)cp_resolve_refresh(md, SCAN_BYTES) : NULL;
    uintptr_t base = api->image_base();
    api->log(TAG, "vfs_init=%p mount_data=%p mount=%p refresh=%p (rva %llx/%llx/%llx/%llx)", init, (void *)md,
             (void *)g_mount, (void *)g_refresh, init ? (unsigned long long)((uintptr_t)init - base) : 0ull,
             md ? (unsigned long long)((uintptr_t)md - base) : 0ull,
             g_mount ? (unsigned long long)((uintptr_t)g_mount - base) : 0ull,
             g_refresh ? (unsigned long long)((uintptr_t)g_refresh - base) : 0ull);
    if (!init || !g_mount) {
        api->log(TAG, "signature not found (game patched?) - content packs disabled");
        return 0; /* do not fail the loader */
    }
    int st = api->hook(init, (void *)init_detour, (void **)&g_init_orig);
    if (st) {
        api->log(TAG, "hook vfs_init failed: %d", st);
        return 0;
    }
    if (api->api_version >= 2 && api->struct_size >= sizeof(V3X_API))
        api->register_command("packs", "list content packs and mount state", cmd_packs, NULL);
    api->log(TAG, "ready, %d pack(s) in %s", cp_scan(g_root), g_root);
    return 0;
}
