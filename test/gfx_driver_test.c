/* Offline test of gfx_driver (no game, no GPU).
 *   python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -o D:/v3x_tmp/gfx/gdt.exe test/gfx_driver_test.c
 *   D:/v3x_tmp/gfx/gdt.exe plugins/gfx_driver/gfx_driver.ini.example [path/to/victoria3.exe]
 * Checks: the ini example parses (inline comments), flag policy, the retry-without-flag path, and
 * (with the exe) that the D3D11CreateDevice IAT slot is found in the real victoria3.exe. */
#include "../plugins/gfx_driver/gd_iat.c"
#include "../plugins/gfx_driver/gfx_driver.c"
#include <stdarg.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char g_dir[MAX_PATH];
static const char *f_dir(void) { return g_dir; }
static void f_log(const char *tag, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); printf("  [%s] ", tag); vprintf(fmt, ap); printf("\n"); va_end(ap); }

static UINT g_seen[4]; static int g_calls;
static HRESULT WINAPI fake_create(void *a, int t, HMODULE s, UINT flags, const int *l, UINT n, UINT sdk,
                                  void **d, int *lv, void **c)
{
    (void)a; (void)t; (void)s; (void)l; (void)n; (void)sdk; (void)d; (void)lv; (void)c;
    if (g_calls < 4) g_seen[g_calls] = flags;
    g_calls++;
    return (flags & GD_PREVENT_INTERNAL_THREADING) ? E_INVALIDARG : S_OK;   /* driver rejects the flag */
}

int main(int argc, char **argv)
{
    V3X_API fake; memset(&fake, 0, sizeof fake);
    fake.api_version = 1; fake.struct_size = sizeof fake; fake.log = f_log; fake.v3x_dir = f_dir;
    api = &fake;

    CHECK(gd_device_flags(0x20, 1) == 0x28);
    CHECK(gd_device_flags(0x20, 0) == 0x20);
    CHECK(!(gd_device_flags(0x20, 1) & GD_SINGLETHREADED));

    /* config: the shipped example (inline comment on the value line), then a 0 override */
    if (argc > 1) {
        char full[MAX_PATH]; GetFullPathNameA(argv[1], MAX_PATH, full, NULL);
        char *slash = strrchr(full, '\\');
        GetTempPathA(MAX_PATH, g_dir);
        char ini[MAX_PATH]; snprintf(ini, sizeof ini, "%sgfx_driver.ini", g_dir);
        CopyFileA(full, ini, FALSE);
        g_prevent = 0; load_config(); CHECK(g_prevent == 1);
        FILE *f = fopen(ini, "w"); fputs("[gfx_driver]\r\nprevent_driver_threading=0 ; off\r\n", f); fclose(f);
        g_prevent = 1; load_config(); CHECK(g_prevent == 0);
        DeleteFileA(ini);
        (void)slash;
    }

    /* wrapper: adds the flag, and on failure retries with the engine's flags */
    orig_create = fake_create; g_prevent = 1; g_calls = 0;
    HRESULT hr = hk_create(NULL, 1, NULL, 0x20, NULL, 0, 7, NULL, NULL, NULL);
    CHECK(hr == S_OK && g_calls == 2 && g_seen[0] == 0x28 && g_seen[1] == 0x20);
    g_prevent = 0; g_calls = 0;
    hr = hk_create(NULL, 1, NULL, 0x20, NULL, 0, 7, NULL, NULL, NULL);
    CHECK(hr == S_OK && g_calls == 1 && g_seen[0] == 0x20);

    /* the real exe's import table */
    if (argc > 2) {
        HMODULE h = LoadLibraryExA(argv[2], NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        CHECK(h != NULL);
        if (h) {
            uint8_t *base = (uint8_t *)((uintptr_t)h & ~(uintptr_t)3);
            void **slot = gd_find_iat_slot(base, "D3D11.DLL", "D3D11CreateDevice");
            printf("  D3D11CreateDevice IAT slot RVA %#llx\n", slot ? (unsigned long long)((uint8_t *)slot - base) : 0ull);
            CHECK(slot != NULL);
            CHECK(gd_find_iat_slot(base, "d3d11.dll", "NoSuchFunction") == NULL);
            CHECK(gd_find_iat_slot(base, "nosuch.dll", "D3D11CreateDevice") == NULL);
        }
    }
    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails != 0;
}
