/* gfx_driver: D3D11 device-creation tweaks for weak (integrated) GPUs.
 *
 * prevent_driver_threading = 1 adds D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS
 * to the game's D3D11CreateDevice calls (1.15.1 call sites 0x3c68932 / 0x3c68e45, both through the
 * d3d11!D3D11CreateDevice IAT slot). Measured on Iris Xe: ~10 igd10um64xe.dll worker threads used
 * ~400% of a core in total; this flag asks the driver not to spread its command processing over
 * worker threads. On a 15 W chip whose CPU and iGPU share one power budget, that CPU time costs
 * GPU clock. Whether FPS goes up has to be measured (A/B with the flag 0/1).
 *
 * Mechanism: the exe's IAT slot for D3D11CreateDevice is redirected to a wrapper (only the game's
 * own calls are affected, not other modules). If device creation fails with the extra flag, the
 * wrapper retries with the engine's original flags, so the worst case is "no change".
 * Config: <game>/binaries/v3x/gfx_driver.ini (defaults = gfx_driver.ini.example). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v3x.h"
#include "gd_iat.h"

V3X_EXPORT const char *v3x_plugin_name = "gfx_driver";
V3X_PLUGIN_MANIFEST("gfx_driver", "0.5.0", "v3x", "D3D11 device-creation tweaks for weak GPUs", "release/1.15.1");
#define TAG "gfx_driver"

typedef HRESULT(WINAPI *create_fn)(void *adapter, int driver_type, HMODULE sw, UINT flags,
                                   const int *levels, UINT nlevels, UINT sdk, void **dev,
                                   int *level, void **ctx);
static const V3X_API *api;
static create_fn orig_create;
static int g_prevent = 1;

static HRESULT WINAPI hk_create(void *adapter, int driver_type, HMODULE sw, UINT flags, const int *levels,
                                UINT nlevels, UINT sdk, void **dev, int *level, void **ctx)
{
    UINT f = gd_device_flags(flags, g_prevent);
    HRESULT hr = orig_create(adapter, driver_type, sw, f, levels, nlevels, sdk, dev, level, ctx);
    if (FAILED(hr) && f != flags) {
        api->log(TAG, "D3D11CreateDevice flags %#x failed (hr %#lx), retrying with engine flags %#x",
                 f, (unsigned long)hr, flags);
        hr = orig_create(adapter, driver_type, sw, flags, levels, nlevels, sdk, dev, level, ctx);
    } else {
        api->log(TAG, "D3D11CreateDevice(driver type %d) engine flags %#x -> %#x: hr %#lx", driver_type,
                 flags, f, (unsigned long)hr);
    }
    return hr;
}

static void load_config(void)
{
    char path[MAX_PATH], line[256];
    snprintf(path, sizeof path, "%sgfx_driver.ini", api->v3x_dir());
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *h = strpbrk(line, "#;");
        if (h) *h = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ' || *k == '\t') k++;
        for (char *e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        if (!strcmp(k, "prevent_driver_threading")) g_prevent = atoi(v) != 0;
    }
    fclose(f);
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    if (a->api_version < 1 || a->struct_size < offsetof(V3X_API, v3x_dir) + sizeof(a->v3x_dir)) return 1;
    load_config();
    if (!g_prevent) { a->log(TAG, "prevent_driver_threading = 0: nothing to do"); return 0; }
    void **slot = gd_find_iat_slot((uint8_t *)a->image_base(), "d3d11.dll", "D3D11CreateDevice");
    HMODULE d3d = GetModuleHandleA("d3d11.dll");
    void *real = d3d ? (void *)GetProcAddress(d3d, "D3D11CreateDevice") : NULL;
    if (!slot || !real) { a->log(TAG, "D3D11CreateDevice import not found: off"); return 0; }
    if (*slot != real) { a->log(TAG, "IAT slot already redirected (%p != %p): off", *slot, real); return 0; }
    orig_create = (create_fn)real;
    void *mine = (void *)hk_create;
    if (a->patch(slot, &mine, sizeof mine)) { a->log(TAG, "IAT patch failed: off"); orig_create = NULL; return 0; }
    a->log(TAG, "D3D11CreateDevice IAT slot %#llx redirected: + PREVENT_INTERNAL_THREADING_OPTIMIZATIONS",
           (unsigned long long)((uintptr_t)slot - a->image_base()));
    return 0;
}
