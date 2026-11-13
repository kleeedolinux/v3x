/* asset_stream: streaming of engine assets instead of loading everything up front.
 * RE notes and the full design (all categories, ranking, never-needed audit): docs/re/streaming.md.
 *
 * Piece 1 (this file): force the engine's own texture streamer on.
 *   The engine has a working CGfxTextureStreamer (pdx_gfx2/gfx_texture_streamer.cpp) that adds and
 *   drops mips of "streamable" textures at runtime within a byte budget. It only starts when the
 *   Graphics setting texture_streaming is on, and every quality preset in game/compound_settings.txt
 *   forces it off. The settings reader (1.15.1 fn 0x3783730) fills a descriptor on the stack and
 *   passes it to the gfx master-context init (fn 0x3c3fdc0, rdx). We hook that init and set
 *   descriptor+0x14 (streaming) = 1 and +0x18 (budget bytes) = cache_mb << 20 before the original
 *   runs; the engine then calls its own Start(budget). Nothing else is patched.
 * Default: enabled = 0 (installs nothing). Fails closed: any anchor check that does not match
 * byte-for-byte, or a descriptor that does not look like the 1.15.1 layout, = no change.
 * Config: <game>/binaries/v3x/asset_stream.ini (defaults = asset_stream.ini.example).
 * Console (API v2): `assetstream` prints the state. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v3x.h"
#include "as_logic.h"
#include "as_resolve.h"

V3X_EXPORT const char *v3x_plugin_name = "asset_stream";
#define TAG "asset_stream"

typedef uintptr_t (*master_init_fn)(void *ctx, void *desc);

static const V3X_API *api;
static as_config g_cfg;
static as_anchors g_a;
static master_init_fn orig_master_init;
static int g_hooked;
/* written once on the render-init thread, read by the console thread (torn reads harmless) */
static volatile LONG g_calls;
static as_apply_result g_res;
static volatile int g_after_enabled = -1;
static volatile unsigned long long g_after_budget;
static volatile int g_init_ok = -1;

static int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
    if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (const uint8_t *)p + n <= (const uint8_t *)mi.BaseAddress + mi.RegionSize;
}

static uintptr_t hk_master_init(void *ctx, void *desc)
{
    LONG n = InterlockedIncrement(&g_calls);
    as_apply_result r; memset(&r, 0, sizeof r); r.rejected = 1;
    if (desc && readable(desc, AS_DESC_CACHE_BYTES + 8)) r = as_apply(&g_cfg, (uint8_t *)desc);
    if (n == 1) g_res = r;
    uintptr_t ret = orig_master_init(ctx, desc);
    if (n == 1) {
        g_init_ok = (int)(ret & 0xff);
        uint8_t *sr = ctx ? (uint8_t *)ctx + AS_CTX_STREAMER : NULL;
        if (sr && readable(sr, AS_STREAMER_ENABLED + 1)) {
            g_after_enabled = sr[AS_STREAMER_ENABLED];
            unsigned long long b; memcpy(&b, sr + AS_STREAMER_BUDGET, 8); g_after_budget = b;
        }
    }
    api->log(TAG, "gfx init #%ld: descriptor %s; texture_streaming %d -> %d, cache %llu -> %llu MB; "
             "init returned %d; streamer enabled=%d budget=%llu MB",
             (long)n, r.rejected ? "REJECTED (layout mismatch, left untouched)" : (r.changed ? "changed" : "unchanged"),
             r.old_streaming, r.new_streaming, (unsigned long long)(r.old_cache >> 20),
             (unsigned long long)(r.new_cache >> 20), (int)(ret & 0xff), g_after_enabled,
             (unsigned long long)(g_after_budget >> 20));
    return ret;
}

static void cmd_status(const char *args, void *user)
{
    (void)args; (void)user;
    uintptr_t base = api->image_base();
    api->print("asset_stream: config enabled=%d texture_streaming=%d cache_mb=%d", g_cfg.enabled,
               g_cfg.texture_streaming, g_cfg.cache_mb);
    if (!g_hooked) { api->print("  not hooked%s%s", g_a.err[0] ? ": " : "", g_a.err); return; }
    api->print("  hook on gfx master init +%#llx (gate +%#llx, Start +%#llx); init calls: %ld",
               (unsigned long long)((uintptr_t)g_a.master_init - base), (unsigned long long)((uintptr_t)g_a.gate - base),
               (unsigned long long)((uintptr_t)g_a.streamer_start - base), (long)g_calls);
    if (g_calls)
        api->print("  descriptor %s: streaming %d -> %d, cache %llu -> %llu MB; streamer enabled=%d budget=%llu MB",
                   g_res.rejected ? "REJECTED" : (g_res.changed ? "changed" : "unchanged"), g_res.old_streaming,
                   g_res.new_streaming, (unsigned long long)(g_res.old_cache >> 20),
                   (unsigned long long)(g_res.new_cache >> 20), g_after_enabled, (unsigned long long)(g_after_budget >> 20));
    api->print("  engine console: Texture.Streamer.Info / Pause / Unpause / FullyLoadAndPause");
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (1 << 20)) { fclose(f); return NULL; }
    char *b = (char *)malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    if (b) b[n] = 0;
    fclose(f);
    return b;
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    if (a->api_version < 1 || a->struct_size < offsetof(V3X_API, v3x_dir) + sizeof(a->v3x_dir)) return 1;
    as_config_defaults(&g_cfg);
    char path[MAX_PATH], err[128];
    snprintf(path, sizeof path, "%sasset_stream.ini", a->v3x_dir());
    char *txt = read_file(path);
    int bad = as_config_parse(&g_cfg, txt ? txt : AS_DEFAULT_INI, err, sizeof err);
    if (bad) {
        a->log(TAG, "%s: %d bad line(s), first: %s -> plugin disabled", txt ? path : "built-in config", bad, err);
        free(txt);
        return 0;
    }
    free(txt);
    as_config_clamp(&g_cfg);
    if (a->api_version >= 2 && a->struct_size >= offsetof(V3X_API, print) + sizeof(a->print) && a->register_command)
        a->register_command("assetstream", "asset_stream status (texture streamer forcing)", cmd_status, NULL);
    if (!g_cfg.enabled) { a->log(TAG, "disabled in config (enabled = 0)"); return 0; }
    if (!g_cfg.texture_streaming) { a->log(TAG, "texture_streaming = 0: nothing to do"); return 0; }

    if (!as_resolve(a, &g_a)) { a->log(TAG, "anchors not found, texture streaming NOT forced: %s", g_a.err); return 0; }
    uintptr_t base = a->image_base();
    int r = a->hook(g_a.master_init, (void *)hk_master_init, (void **)&orig_master_init);
    if (r) { a->log(TAG, "hook on gfx master init failed (%d)", r); return 0; }
    g_hooked = 1;
    a->log(TAG, "will force texture streaming on (cache %s%d MB) at gfx init +%#llx",
           g_cfg.cache_mb ? "" : "from settings, ", g_cfg.cache_mb,
           (unsigned long long)((uintptr_t)g_a.master_init - base));
    return 0;
}
