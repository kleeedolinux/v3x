/* Scratch directories for the offline tests (header-only, Windows).
 *
 * Root = $V3X_TMP if set, else the process temp dir (GetTempPathA: %TMP% / %TEMP%). test/run_all.sh
 * exports TEMP=TMP=<its scratch root> (V3X_TMP or $TEMP/v3x), so tests run by it write only under that
 * root; CI sets V3X_TMP=$RUNNER_TEMP/v3x. No test may use a fixed drive or folder.
 *
 *   static char g_tdir[MAX_PATH];
 *   test_tmp_dir(g_tdir, sizeof g_tdir, "lazymesh/t");   -> "<root>/lazymesh/t/" (created, '/' separators)
 */
#ifndef V3X_TEST_TMP_H
#define V3X_TEST_TMP_H
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Writes "<root>/<sub>/" to out (forward slashes, one trailing '/') and creates every missing level.
 * Exits the test (code 2, with a message) when the path does not fit or cannot be created. */
static char *test_tmp_dir(char *out, size_t cap, const char *sub)
{
    char root[MAX_PATH];
    const char *e = getenv("V3X_TMP");
    if (e && *e) snprintf(root, sizeof root, "%s", e);
    else {
        DWORD n = GetTempPathA(sizeof root, root);
        if (!n || n >= sizeof root) snprintf(root, sizeof root, ".");
    }
    size_t rl = strlen(root);
    while (rl > 1 && (root[rl - 1] == '/' || root[rl - 1] == '\\')) root[--rl] = 0;
    int w = snprintf(out, cap, "%s/%s/", root, sub && *sub ? sub : ".");
    if (w < 0 || (size_t)w >= cap) { printf("FAIL test_tmp_dir: path too long (%s/%s)\n", root, sub); exit(2); }
    for (char *c = out; *c; c++) if (*c == '\\') *c = '/';
    /* create each level; skip the drive ("D:/") or UNC prefix, existing levels are fine */
    char *start = out;
    if (start[0] && start[1] == ':') start += 2;
    else if (start[0] == '/' && start[1] == '/') { start += 2; for (int k = 0; k < 2 && (start = strchr(start, '/')) != NULL; k++) start++; if (!start) start = out + strlen(out); }
    for (char *c = start + (*start == '/'); *c; c++) {
        if (*c != '/') continue;
        *c = 0;
        if (!CreateDirectoryA(out, NULL) && GetLastError() != ERROR_ALREADY_EXISTS &&
            GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES) {
            printf("FAIL test_tmp_dir: cannot create %s (%lu)\n", out, GetLastError());
            exit(2);
        }
        *c = '/';
    }
    return out;
}

#endif
