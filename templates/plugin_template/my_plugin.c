/* my_plugin - V3X plugin template. Copy this folder, rename my_plugin everywhere, build:
 *
 *     python sdk/build_plugin.py templates/plugin_template            -> build/plugins/my_plugin.dll
 *     python sdk/build_plugin.py templates/plugin_template --test ../binaries/victoria3.exe
 *     python sdk/build_plugin.py templates/plugin_template --install  (copies into the game)
 *
 * What it shows (each step is one of the rules in docs/safety.md):
 *   1. a manifest (name, version, author, supported game builds), shown by the `plugins` command;
 *   2. config from <game>/binaries/v3x/my_plugin.ini, every key optional;
 *   3. a fail-closed anchor: find the per-tick GameState function by its log string, check its
 *      prologue bytes, and do NOTHING if anything does not match;
 *   4. an observe-only hook through api->hook_safe (no hand-written detour, so no argument count
 *      to get wrong), with pre/post callbacks that only count and time (no game state, no script);
 *   5. a console command that prints the numbers (runs on the console thread: it only reads
 *      our own counters).
 * The engine function: "GameState tick" (docs/re/simulation.md, 1.15.1 exe+0x1465690), called
 * once per game tick; its only xref to the string "Processing Tick: " is inside it. */
#include "v3x_sdk.h"

#define TAG "my_plugin"

V3X_PLUGIN_MANIFEST("my_plugin", "0.1.0", "your name", "template: counts and times game ticks",
                    "release/1.15*");

static const V3X_API *api;
static volatile LONG64 g_ticks, g_tick_us_total, g_tick_us_max;
static LARGE_INTEGER g_freq;
static int g_log_every;

/* exported for the offline test (test_my_plugin.c) */
V3xAnchor my_plugin_anchors[] = {
    /* name, string, prologue pattern at the function start, mode */
    {"game_tick", "Processing Tick: ", "48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24", V3X_ANCHOR_ALL_SAME},
};
#define N_ANCHORS ((int)(sizeof my_plugin_anchors / sizeof my_plugin_anchors[0]))

/* pre/post run on the thread that runs the game tick. Keep them tiny and lock-free. */
static void tick_pre(V3X_HookCall *c)
{
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    c->scratch[0] = (uint64_t)t.QuadPart;   /* carried to tick_post of this same call */
}

static void tick_post(V3X_HookCall *c)
{
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    LONG64 us = (LONG64)((t.QuadPart - (LONG64)c->scratch[0]) * 1000000 / g_freq.QuadPart);
    LONG64 n = InterlockedIncrement64(&g_ticks);
    InterlockedAdd64(&g_tick_us_total, us);
    LONG64 m = g_tick_us_max;
    while (us > m && InterlockedCompareExchange64(&g_tick_us_max, us, m) != m) m = g_tick_us_max;
    if (g_log_every > 0 && n % g_log_every == 0)
        api->log(TAG, "%lld ticks, last %lld us", (long long)n, (long long)us);
}

static void cmd_status(const char *args, void *user)
{
    (void)args; (void)user;
    LONG64 n = g_ticks;
    api->print("my_plugin: %lld ticks, avg %lld us, max %lld us", (long long)n,
               (long long)(n ? g_tick_us_total / n : 0), (long long)g_tick_us_max);
}

/* Resolves everything without side effects; 0 = all anchors ok. Shared with the offline test. */
int my_plugin_resolve(const V3X_API *a)
{
    return v3x_anchor_resolve_all(a, TAG, my_plugin_anchors, N_ANCHORS);
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    QueryPerformanceFrequency(&g_freq);

    V3xIni ini;
    v3x_ini_load_plugin(&ini, api, "my_plugin");
    int enabled = (int)v3x_ini_bool(&ini, "my_plugin", "enabled", 1);
    g_log_every = (int)v3x_ini_int(&ini, "my_plugin", "log_every", 0);
    v3x_ini_free(&ini);
    if (!enabled) { api->log(TAG, "disabled in my_plugin.ini"); return 0; }

    if (!V3X_HAS(api, hook_safe)) { api->log(TAG, "loader too old (needs API v4), nothing installed"); return 1; }
    if (my_plugin_resolve(api) != 0) { api->log(TAG, "anchor FAILED: nothing installed (game updated?)"); return 1; }

    int rc = api->hook_safe(my_plugin_anchors[0].result, tick_pre, tick_post, NULL);
    if (rc != 0) { api->log(TAG, "hook_safe FAILED (%d): nothing installed", rc); return 1; }
    api->register_command("mytick", "my_plugin: game tick count and timing", cmd_status, NULL);
    api->log(TAG, "observing the game tick (log_every=%d)", g_log_every);
    return 0;
}
