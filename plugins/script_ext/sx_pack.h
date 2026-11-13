/* sx_pack.h - content-pack file writer of script_ext (pure Win32 file I/O, no engine access).
 * Unit-tested in test/script_ext_test.c. See sdk/v3x_script.h for the contract. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "v3x_script.h"

#define SX_MAX_PACKS 16
#define SX_MAX_FILES 512
#define SX_MANIFEST "v3x_script_ext.manifest"

static char sx_root[MAX_PATH];                 /* ...\binaries\v3x\content_packs\ ("" = unknown) */
static void (*sx_logf)(const char *fmt, ...);  /* optional logger (set once the plugin has its api) */

typedef struct { char name[64]; int active; int n; char *files[SX_MAX_FILES]; } sx_pack_t;
static sx_pack_t sx_packs[SX_MAX_PACKS];

static void sx_log(const char *fmt, const char *a, unsigned long b)
{
    if (sx_logf) sx_logf(fmt, a, b);
}

/* 1 if s is a valid pack name. */
static int sx_valid_pack(const char *s)
{
    if (!s || !s[0] || s[0] == '_' || s[0] == '.') return 0;
    size_t n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return 0;
        if (n >= 63) return 0;
    }
    return 1;
}

/* 1 if rel is a safe relative path: printable ASCII, no drive/colon, no leading separator, no empty,
 * "." or ".." component, max 200 chars, not the manifest itself. */
static int sx_valid_rel(const char *rel)
{
    if (!rel || !rel[0]) return 0;
    size_t n = strlen(rel);
    if (n > 200 || rel[0] == '/' || rel[0] == '\\') return 0;
    const char *c = rel;
    for (size_t i = 0; i <= n; i++) {
        char ch = rel[i];
        if (ch && (ch < 0x20 || ch > 0x7e || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' ||
                   ch == '>' || ch == '|'))
            return 0;
        if (ch == '/' || ch == '\\' || ch == 0) {
            size_t len = (size_t)(rel + i - c);
            if (len == 0) return 0;
            if (len == 1 && c[0] == '.') return 0;
            if (len == 2 && c[0] == '.' && c[1] == '.') return 0;
            if (c[len - 1] == '.' || c[len - 1] == ' ') return 0;   /* Windows strips them: aliasing */
            c = rel + i + 1;
        }
    }
    return _stricmp(rel, SX_MANIFEST) != 0;
}

/* 1 if the file is localization (*.yml) and data lacks the UTF-8 BOM. */
static int sx_needs_bom(const char *rel, const void *data, size_t n)
{
    size_t k = strlen(rel);
    if (k < 4 || _stricmp(rel + k - 4, ".yml")) return 0;
    const uint8_t *d = (const uint8_t *)data;
    return !(n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF);
}

static sx_pack_t *sx_find(const char *pack)
{
    for (int i = 0; i < SX_MAX_PACKS; i++)
        if (sx_packs[i].name[0] && !_stricmp(sx_packs[i].name, pack)) return &sx_packs[i];
    return NULL;
}

/* Creates every folder of path (absolute, '\\' separators) up to its last separator. */
static int sx_mkdirs(char *path)
{
    for (char *p = path + 3; *p; p++) {
        if (*p != '\\') continue;
        *p = 0;
        BOOL ok = CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
        *p = '\\';
        if (!ok) return 0;
    }
    return 1;
}

static int sx_path(char *out, size_t cap, const char *pack, const char *rel)
{
    int k = snprintf(out, cap, "%s%s\\%s", sx_root, pack, rel ? rel : "");
    if (k <= 0 || (size_t)k >= cap) return 0;
    for (char *p = out; *p; p++) if (*p == '/') *p = '\\';
    return 1;
}

static int sx_write_raw(const char *path, const void *pre, size_t npre, const void *data, size_t n)
{
    char tmp[MAX_PATH + 16];
    snprintf(tmp, sizeof tmp, "%s.v3xtmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    int ok = (!npre || fwrite(pre, 1, npre, f) == npre) && (!n || fwrite(data, 1, n, f) == n);
    ok = (fclose(f) == 0) && ok;
    if (ok) ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileA(tmp);
    return ok;
}

static int sx_begin_pack(const char *pack)
{
    if (!sx_valid_pack(pack) || !sx_root[0]) return V3X_SCRIPT_EARG;
    sx_pack_t *p = sx_find(pack);
    if (!p) {
        for (int i = 0; i < SX_MAX_PACKS && !p; i++) if (!sx_packs[i].name[0]) p = &sx_packs[i];
        if (!p) return V3X_SCRIPT_EARG;
        snprintf(p->name, sizeof p->name, "%s", pack);
    }
    for (int i = 0; i < p->n; i++) { free(p->files[i]); p->files[i] = NULL; }
    p->n = 0;
    char dir[MAX_PATH];
    if (!sx_path(dir, sizeof dir, pack, "") || !sx_mkdirs(dir)) {
        sx_log("begin_pack %s: cannot create folder (error %lu)", pack, GetLastError());
        return V3X_SCRIPT_EIO;
    }
    /* delete the files of the previous session's manifest (validated again: never outside the pack) */
    char man[MAX_PATH];
    sx_path(man, sizeof man, pack, SX_MANIFEST);
    FILE *f = fopen(man, "rb");
    if (f) {
        char line[260];
        while (fgets(line, sizeof line, f)) {
            size_t k = strlen(line);
            while (k && (line[k - 1] == '\n' || line[k - 1] == '\r')) line[--k] = 0;
            char fp[MAX_PATH];
            if (sx_valid_rel(line) && sx_path(fp, sizeof fp, pack, line)) DeleteFileA(fp);
        }
        fclose(f);
        DeleteFileA(man);
    }
    p->active = 1;
    return V3X_SCRIPT_OK;
}

static int sx_write_file(const char *pack, const char *rel, const void *data, size_t n)
{
    if (!sx_valid_pack(pack) || !sx_valid_rel(rel) || (n && !data)) return V3X_SCRIPT_EARG;
    sx_pack_t *p = sx_find(pack);
    if (!p || !p->active) return V3X_SCRIPT_ESTATE;
    char path[MAX_PATH];
    if (!sx_path(path, sizeof path, pack, rel)) return V3X_SCRIPT_EARG;
    if (!sx_mkdirs(path)) { sx_log("write %s: mkdir failed (error %lu)", rel, GetLastError()); return V3X_SCRIPT_EIO; }
    static const uint8_t bom[3] = {0xEF, 0xBB, 0xBF};
    int add_bom = sx_needs_bom(rel, data, n);
    if (!sx_write_raw(path, bom, add_bom ? 3 : 0, data, n)) {
        sx_log("write %s failed (error %lu)", rel, GetLastError());
        return V3X_SCRIPT_EIO;
    }
    for (int i = 0; i < p->n; i++) if (!_stricmp(p->files[i], rel)) return V3X_SCRIPT_OK;
    if (p->n < SX_MAX_FILES) {
        size_t k = strlen(rel) + 1;
        char *c = (char *)malloc(k);
        if (c) { memcpy(c, rel, k); p->files[p->n++] = c; }
    }
    return V3X_SCRIPT_OK;
}

static int sx_end_pack(const char *pack)
{
    if (!sx_valid_pack(pack)) return V3X_SCRIPT_EARG;
    sx_pack_t *p = sx_find(pack);
    if (!p || !p->active) return V3X_SCRIPT_ESTATE;
    char buf[SX_MAX_FILES * 4 + 64], man[MAX_PATH];
    size_t k = 0;
    char *big = NULL;
    size_t need = 1;
    for (int i = 0; i < p->n; i++) need += strlen(p->files[i]) + 1;
    char *out = need <= sizeof buf ? buf : (big = (char *)malloc(need));
    if (!out) return V3X_SCRIPT_EIO;
    for (int i = 0; i < p->n; i++) { size_t l = strlen(p->files[i]); memcpy(out + k, p->files[i], l); k += l; out[k++] = '\n'; }
    sx_path(man, sizeof man, pack, SX_MANIFEST);
    int ok = sx_write_raw(man, NULL, 0, out, k);
    free(big);
    p->active = 0;
    return ok ? p->n : V3X_SCRIPT_EIO;
}

static const char *sx_packs_dir(void) { return sx_root; }

/* Root from the DLL location: <x>\binaries\v3x\plugins\script_ext.dll -> <x>\binaries\v3x\content_packs\ */
static void sx_root_from_module(HMODULE self)
{
    char p[MAX_PATH];
    DWORD n = GetModuleFileNameA(self, p, sizeof p);
    if (!n || n >= sizeof p) return;
    char *s = strrchr(p, '\\');
    if (!s) return;
    *s = 0;                                    /* ...\plugins */
    s = strrchr(p, '\\');
    if (!s || _stricmp(s + 1, "plugins")) return;
    *s = 0;                                    /* ...\v3x */
    snprintf(sx_root, sizeof sx_root, "%s\\content_packs\\", p);
    CreateDirectoryA(sx_root, NULL);
}
