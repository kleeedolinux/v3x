/* my_plugin - V3X plugin template. Copy this folder, rename my_plugin everywhere, build:
 *
 *     python sdk/build_plugin.py templates/plugin_template --werror            -> build/plugins/my_plugin.dll
 *     python sdk/build_plugin.py templates/plugin_template --werror --test ../binaries/victoria3.exe
 *     python sdk/build_plugin.py templates/plugin_template --werror --install  (copies into the game)
 *
 * It uses only the safe APIs (docs/safe-coding.md); each step is one of its rules:
 *   1. a manifest (name, version, author, supported game builds, capabilities), shown by `plugins`;
 *   2. config from <game>/binaries/v3x/my_plugin.ini, every key optional, values clamped;
 *   3. a fail-closed anchor: find the per-tick GameState function by its log string, check its
 *      prologue bytes and that the result is a function start inside the exe, and do NOTHING if anything fails;
 *   4. an observe-only hook through api->hook_safe (no hand-written detour, so no argument count
 *      to get wrong), with pre/post callbacks that only count and time with atomics (no game
 *      state, no script, no locks, no allocation);
 *   5. a console command that formats into a bounded V3xStr and prints it (runs on the console
 *      thread: it only reads our own atomic counters);
 *   6. every API result checked (register_command, on_event, hook_safe);
 *   7. (API v5, optional) leveled logging (V3X_LOGD shows only with [log] my_plugin = debug in
 *      v3x/v3x.ini) and re-reading the ini on `reload my_plugin`, on the main thread (asserted).
 * Check the built DLL against the installed game and loader: python sdk/build_plugin.py check
 * build/plugins/my_plugin.dll
 * The engine function: "GameState tick" (docs/re/simulation.md, 1.15.1 exe+0x1465690), called
 * once per game tick; its only xref to the string "Processing Tick: " is inside it. */
#include "v3x_sdk.h"

#define TAG "my_plugin"

/* capabilities "hook" (API v6): the loader refuses patch / exec_command for this plugin, so a bug or a
 * hijacked code path cannot use them. Declare only what you use; older loaders ignore the field. */
V3X_PLUGIN_MANIFEST_CAPS("my_plugin", "0.2.0", "your name", "template: counts and times game ticks",
                         "release/1.15*", 0u, 0u, "hook");

static const V3X_API *api;   /* set once in v3x_plugin_load, read-only afterwards */

/* Shared between the hooked thread (writer), the console thread and the main thread (readers):
 * only touched through Interlocked* / v3x_atomic_*. */
static volatile LONG64 g_ticks, g_tick_us_total, g_tick_us_max;
static volatile int g_log_every;

static LONG64 load64(volatile LONG64 *p) { return InterlockedCompareExchange64(p, 0, 0); }

/* Extra anchor check: the result lies inside victoria3.exe and is the START of a .pdata function
 * (not somewhere in the middle of one). Works in the game and on the offline-mapped exe. */
static int check_fn_start(const V3X_API *a, void *result, void *user)
{
    (void)user;
    if (!v3x_mem_in_module(a->image_base(), a->image_size(), (uintptr_t)result, 16)) return 1;
    return a->function_start(result) == result ? 0 : 2;
}

/* exported for the offline test (test_my_plugin.c) */
V3xAnchor my_plugin_anchors[] = {
    {.name = "game_tick",
     .string = "Processing Tick: ",                               /* the function that logs this */
     .pattern = "48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24",      /* must match its first bytes */
     .xref = V3X_ANCHOR_ALL_SAME,                                 /* every xref in ONE function */
     .check = check_fn_start},
};
#define N_ANCHORS ((int)(sizeof my_plugin_anchors / sizeof my_plugin_anchors[0]))

/* pre/post run on the thread that runs the game tick. Keep them tiny and lock-free. */
static void tick_pre(V3X_HookCall *c)
{
    c->scratch[0] = (uint64_t)v3x_qpc();   /* carried to tick_post of this same call */
}

static void tick_post(V3X_HookCall *c)
{
    int64_t dt = v3x_qpc() - (int64_t)c->scratch[0];
    LONG64 us = dt > 0 ? (LONG64)(v3x_qpc_ms(dt) * 1000.0) : 0;
    LONG64 n = InterlockedIncrement64(&g_ticks);
    InterlockedAdd64(&g_tick_us_total, us);
    LONG64 m = load64(&g_tick_us_max);
    while (us > m) {
        LONG64 seen = InterlockedCompareExchange64(&g_tick_us_max, us, m);
        if (seen == m) break;
        m = seen;
    }
    int every = v3x_atomic_load_int(&g_log_every);
    if (every > 0 && n % every == 0)
        V3X_LOGD(api, TAG, "%lld ticks, last %lld us", (long long)n, (long long)us);   /* debug level */
}

/* Formats the status line into a caller buffer; never writes past cap. Shared with the test. */
static V3xResult format_status(char *buf, size_t cap)
{
    V3xStr s;
    v3x_str_init(&s, buf, cap);
    LONG64 n = load64(&g_ticks);
    return v3x_str_appendf(&s, "my_plugin: %lld ticks, avg %lld us, max %lld us", (long long)n,
                           (long long)(n ? load64(&g_tick_us_total) / n : 0), (long long)load64(&g_tick_us_max));
}

/* console thread (or whoever calls exec_command): reads only our own atomics */
static void cmd_status(const char *args, void *user)
{
    (void)args; (void)user;
    char line[160];
    if (format_status(line, sizeof line) != V3X_R_OK) V3X_LOGW(api, TAG, "status line truncated");
    api->print("%s", line);
}

/* Resolves everything without side effects; 0 = all anchors ok. Shared with the offline test. */
int my_plugin_resolve(const V3X_API *a)
{
    return v3x_anchor_resolve_all(a, TAG, my_plugin_anchors, N_ANCHORS);
}

/* Reads my_plugin.ini (every key optional, out-of-range values clamped); returns `enabled`. */
static int read_config(void)
{
    V3xIni ini;
    if (v3x_ini_load_plugin(&ini, api, "my_plugin") != 0)
        V3X_LOGW(api, TAG, "my_plugin.ini unreadable: using defaults");
    int enabled = v3x_ini_bool(&ini, "my_plugin", "enabled", 1);
    long long every = v3x_ini_int(&ini, "my_plugin", "log_every", 0);
    v3x_ini_free(&ini);
    if (every < 0) every = 0;
    if (every > 1000000) every = 1000000;
    v3x_atomic_store_int(&g_log_every, (int)every);
    return enabled;
}

/* `reload my_plugin` / `reload` (API v5): emitted on the main thread. Only settings that are safe to
 * change live. */
static void on_reload(const char *event, void *data, void *user)
{
    (void)event; (void)user;
    if (!v3x_reload_is_for(data, TAG)) return;
    if (!V3X_ASSERT_MAIN(api)) return;   /* logged; never expected */
    read_config();
    V3X_LOGI(api, TAG, "config reloaded (log_every=%d)", v3x_atomic_load_int(&g_log_every));
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    if (!read_config()) { api->log(TAG, "disabled in my_plugin.ini"); return 0; }

    if (!V3X_HAS(api, hook_safe)) { api->log(TAG, "loader too old (needs API v4), nothing installed"); return 1; }
    if (my_plugin_resolve(api) != 0) { api->log(TAG, "anchor FAILED: nothing installed (game updated?)"); return 1; }

    int rc = api->hook_safe(my_plugin_anchors[0].result, tick_pre, tick_post, NULL);
    if (rc != V3X_OK) { api->log(TAG, "hook_safe FAILED (%d): nothing installed", rc); return 1; }
    /* Optional parts: a failure costs the feature, not the plugin. */
    rc = api->register_command("mytick", "my_plugin: game tick count and timing", cmd_status, NULL);
    if (rc != V3X_OK) V3X_LOGW(api, TAG, "command mytick not registered (%d)", rc);
    if (V3X_HAS(api, log_at)) {   /* v5 loaders only */
        rc = api->on_event(V3X_EVENT_CONFIG_RELOAD, on_reload, NULL);
        if (rc != V3X_OK) V3X_LOGW(api, TAG, "config_reload not subscribed (%d)", rc);
    }
    api->log(TAG, "observing the game tick (log_every=%d)", v3x_atomic_load_int(&g_log_every));
    return 0;
}
