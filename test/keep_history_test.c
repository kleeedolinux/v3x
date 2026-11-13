/* Offline test of plugins/keep_history against the real victoria3.exe, without running the game.
 * Maps the exe as an image (LoadLibraryEx, no imports resolved, no code run), gives the plugin a
 * fake V3X_API whose scan functions work on that image, and records patch() calls instead of
 * writing. Checks: plugin loads (returns 0), patches exactly 11 sites, each site is a
 * `call rel32` to the same function, and (if given) the RVAs match expected ones.
 * Also checks record_all: with no ini (default on) 2 hooks, SetCountryTrendRate and
 * ShouldUpdateTrends; env KH_TEST_DIR=<dir with keep_history.ini> to test record_all=0 (0 hooks).
 * Usage: keep_history_test.exe <victoria3.exe> <keep_history.dll> [expected first site RVA hex]
 *        [expected hook count] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <windows.h>
#include "v3x.h"

static uint8_t *g_base; static size_t g_size;
static uint8_t *g_text; static size_t g_text_size;
static struct { uint8_t *p; size_t n; } g_data[8]; static int g_ndata;
static uintptr_t g_sites[64]; static int g_nsites; static int g_patch_bad;

static void t_log(const char *tag, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); printf("[%s] ", tag); vprintf(fmt, ap); printf("\n"); va_end(ap);
}
static uintptr_t t_base(void) { return (uintptr_t)g_base; }
static size_t t_size(void) { return g_size; }
static const char *t_ver(void) { return "test"; }
static void *t_scan(const char *p) { (void)p; return NULL; }
static void *t_find_string(const char *s)
{
    size_t n = strlen(s) + 1;
    for (int r = 0; r < g_ndata; r++)
        for (size_t i = 0; i + n <= g_data[r].n; i++)
            if (g_data[r].p[i] == (uint8_t)s[0] && !memcmp(g_data[r].p + i, s, n) && (i == 0 || g_data[r].p[i - 1] == 0))
                return g_data[r].p + i;
    return NULL;
}
static void *t_find_xref(const void *target, int index)
{
    for (size_t i = 0; i + 7 <= g_text_size; i++) {
        uint8_t rex = g_text[i], op = g_text[i + 1];
        if ((rex != 0x48 && rex != 0x4C) || (op != 0x8D && op != 0x8B) || (g_text[i + 2] & 0xC7) != 0x05) continue;
        int32_t d; memcpy(&d, g_text + i + 3, 4);
        if (g_text + i + 7 + d == (const uint8_t *)target && index-- == 0) return g_text + i;
    }
    return NULL;
}
static void *t_function_start(const void *addr)
{
    DWORD64 image = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)addr, &image, NULL);
    if (!rf) return NULL;
    for (int guard = 0; guard < 32; guard++) {
        uint8_t *ui = (uint8_t *)image + rf->UnwindData;
        if (!((ui[0] >> 3) & UNW_FLAG_CHAININFO)) break;
        rf = (PRUNTIME_FUNCTION)(ui + 4 + ((ui[2] + 1) & ~1u) * 2);
    }
    return (uint8_t *)image + rf->BeginAddress;
}
static uintptr_t g_hooks[8]; static int g_nhooks;
static int t_hook(void *t, void *d, void **o) { (void)d; *o = t; if (g_nhooks < 8) g_hooks[g_nhooks++] = (uintptr_t)t; return 0; }
static int t_unhook(void *t) { (void)t; return 0; }
static int t_patch(void *addr, const void *bytes, size_t n)
{
    static const uint8_t nop5[5] = {0x0F, 0x1F, 0x44, 0x00, 0x00};
    if (n != 5 || memcmp(bytes, nop5, 5) || ((uint8_t *)addr)[0] != 0xE8) g_patch_bad++;
    if (g_nsites < 64) g_sites[g_nsites++] = (uintptr_t)addr;
    return 0;
}
static int t_on_frame(v3x_frame_fn f, void *u) { (void)f; (void)u; return 0; }
static char g_dir[MAX_PATH] = ".\\";
static const char *t_dir(void) { return g_dir; }

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: %s victoria3.exe keep_history.dll [first_site_rva]\n", argv[0]); return 2; }
    HMODULE exe = LoadLibraryExA(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!exe) { printf("FAIL: cannot map %s (%lu)\n", argv[1], GetLastError()); return 1; }
    g_base = (uint8_t *)exe;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    g_size = nt->OptionalHeader.SizeOfImage;
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (!memcmp(s->Name, ".text", 5)) { g_text = g_base + s->VirtualAddress; g_text_size = s->Misc.VirtualSize; }
        else if ((!memcmp(s->Name, ".rdata", 6) || !memcmp(s->Name, ".data", 5)) && g_ndata < 8)
            { g_data[g_ndata].p = g_base + s->VirtualAddress; g_data[g_ndata++].n = s->Misc.VirtualSize; }
    }
    HMODULE pl = LoadLibraryA(argv[2]);
    if (!pl) { printf("FAIL: cannot load plugin %s (%lu)\n", argv[2], GetLastError()); return 1; }
    v3x_plugin_load_fn load = (v3x_plugin_load_fn)(void *)GetProcAddress(pl, "v3x_plugin_load");
    if (!load) { printf("FAIL: no v3x_plugin_load\n"); return 1; }

    V3X_API api = {0};
    api.api_version = 1; api.struct_size = offsetof(V3X_API, register_command);
    api.log = t_log; api.image_base = t_base; api.image_size = t_size; api.game_version = t_ver;
    api.pattern_scan = t_scan; api.find_string = t_find_string; api.find_xref = t_find_xref;
    api.function_start = t_function_start; api.hook = t_hook; api.unhook = t_unhook;
    api.patch = t_patch; api.on_frame = t_on_frame; api.game_dir = t_dir; api.v3x_dir = t_dir;

    const char *d = getenv("KH_TEST_DIR");
    if (d) snprintf(g_dir, sizeof g_dir, "%s\\", d);
    int fails = 0;
    int r = load(&api);
    if (r != 0) { printf("FAIL: v3x_plugin_load returned %d\n", r); fails++; }
    if (g_nsites != 11) { printf("FAIL: %d patch sites (want 11)\n", g_nsites); fails++; }
    if (g_patch_bad) { printf("FAIL: %d patches not 5-byte nop over a call\n", g_patch_bad); fails++; }
    const uint8_t *tgt0 = NULL;
    for (int i = 0; i < g_nsites; i++) {
        uint8_t *p = (uint8_t *)g_sites[i]; int32_t d; memcpy(&d, p + 1, 4);
        const uint8_t *t = p + 5 + d;
        if (!i) tgt0 = t;
        if (t != tgt0) { printf("FAIL: site %d calls a different function\n", i); fails++; }
        printf("site %2d rva %08llx -> %08llx\n", i, (unsigned long long)(g_sites[i] - (uintptr_t)g_base),
               (unsigned long long)((uintptr_t)t - (uintptr_t)g_base));
    }
    if (argc > 3 && g_nsites && g_sites[0] - (uintptr_t)g_base != strtoull(argv[3], NULL, 16)) {
        printf("FAIL: first site rva differs from expected %s\n", argv[3]); fails++;
    }
    int want_hooks = argc > 4 ? atoi(argv[4]) : 2;
    for (int i = 0; i < g_nhooks; i++)
        printf("hook %d rva %08llx\n", i, (unsigned long long)(g_hooks[i] - (uintptr_t)g_base));
    if (g_nhooks != want_hooks) { printf("FAIL: %d hooks (want %d)\n", g_nhooks, want_hooks); fails++; }
    printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
    return fails ? 1 : 0;
}
