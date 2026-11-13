---
title: "Plugin template"
summary: "The minimal plugin that follows every safety rule, with its source and offline test."
group: modders
order: 11
source: "templates/plugin_template/README.md"
sourceUrl: "https://github.com/kleeedolinux/v3x/blob/main/templates/plugin_template/README.md"
updated: "2026-11-13"
---
A minimal plugin that follows every V3X safety rule: a manifest, an optional ini, a
fail-closed anchor, an observe-only `hook_safe` hook, a console command and an offline test.

| file | what |
|---|---|
| `my_plugin.c` | the plugin: counts and times the per-tick GameState function |
| `my_plugin.ini.example` | its config with the defaults (copy to `binaries/v3x/my_plugin.ini`) |
| `test_my_plugin.c` | offline test: callbacks, and the real anchor code against `victoria3.exe` mapped read-only |

## Build, test, install

From the repository root (needs `pip install ziglang`):

```sh
python sdk/build_plugin.py templates/plugin_template                 # build/plugins/my_plugin.dll
python sdk/build_plugin.py templates/plugin_template --test          # + offline test against the game exe
python sdk/build_plugin.py templates/plugin_template --test --install
python sdk/build_plugin.py check build/plugins/my_plugin.dll         # will the installed loader + game accept it?
```

`check` reads the DLL without loading it: exports, manifest, `min_api` against the installed
loader's API version, `game_builds` against the installed `victoria3.exe` (including the loader's
unknown-build safe mode after a game patch). Exit code 0 = compatible. With a v5 loader the
template also shows leveled logging (`[log] my_plugin = debug` in `v3x/v3x.ini`) and re-reads its
ini on `reload my_plugin` (docs/api-reference.md).

The plain compiler line, if you prefer it:

```sh
python -m ziglang cc -target x86_64-windows-gnu -O2 -Wno-unused-function -shared \
    -Iinclude -Isdk -o build/plugins/my_plugin.dll templates/plugin_template/my_plugin.c
```

## Make it yours

1. Copy the folder to `plugins/<name>/` and rename `my_plugin` in the file names, the `TAG`, the
   manifest and the ini section.
2. Replace the anchor: find a string your target function references (`tools/pestrings.py`,
   `tools/pexref.py`, see `docs/reverse-engineering.md`), check the prologue bytes, and keep
   `V3X_ANCHOR_ALL_SAME` or use `V3X_ANCHOR_SHAPE` if several functions use the string.
3. Run the offline test until it passes, then smoke-test in the game (load a save, play a few
   days, save, quit) with a backup of `binaries/v3x/*.log` from the previous run.

Read `docs/safety.md` before you change game state from a callback.


## Source: `my_plugin.c`

[templates/plugin_template/my_plugin.c on GitHub](https://github.com/kleeedolinux/v3x/blob/main/templates/plugin_template/my_plugin.c)

```c
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
 *      our own counters);
 *   6. (API v5, optional) leveled logging (V3X_LOGD shows only with [log] my_plugin = debug in
 *      v3x/v3x.ini) and re-reading the ini on the console command `reload my_plugin`.
 * Check the built DLL against the installed game and loader: python sdk/build_plugin.py check
 * build/plugins/my_plugin.dll
 * The engine function: "GameState tick" (docs/re/simulation.md, 1.15.1 exe+0x1465690), called
 * once per game tick; its only xref to the string "Processing Tick: " is inside it. */
#include "v3x_sdk.h"

#define TAG "my_plugin"

V3X_PLUGIN_MANIFEST("my_plugin", "0.1.0", "your name", "template: counts and times game ticks",
                    "release/1.15*");

static const V3X_API *api;
static volatile LONG64 g_ticks, g_tick_us_total, g_tick_us_max;
static volatile int g_log_every;

/* exported for the offline test (test_my_plugin.c) */
V3xAnchor my_plugin_anchors[] = {
    /* name, string, prologue pattern at the function start, mode */
    {"game_tick", "Processing Tick: ", "48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24", V3X_ANCHOR_ALL_SAME},
};
#define N_ANCHORS ((int)(sizeof my_plugin_anchors / sizeof my_plugin_anchors[0]))

/* pre/post run on the thread that runs the game tick. Keep them tiny and lock-free. */
static void tick_pre(V3X_HookCall *c)
{
    c->scratch[0] = (uint64_t)v3x_qpc();   /* carried to tick_post of this same call */
}

static void tick_post(V3X_HookCall *c)
{
    LONG64 us = (LONG64)(v3x_qpc_ms(v3x_qpc() - (int64_t)c->scratch[0]) * 1000.0);
    LONG64 n = InterlockedIncrement64(&g_ticks);
    InterlockedAdd64(&g_tick_us_total, us);
    LONG64 m = g_tick_us_max;
    while (us > m && InterlockedCompareExchange64(&g_tick_us_max, us, m) != m) m = g_tick_us_max;
    if (g_log_every > 0 && n % g_log_every == 0)
        V3X_LOGD(api, TAG, "%lld ticks, last %lld us", (long long)n, (long long)us);   /* debug level */
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

/* Reads my_plugin.ini (every key optional); returns `enabled`. */
static int read_config(void)
{
    V3xIni ini;
    v3x_ini_load_plugin(&ini, api, "my_plugin");
    int enabled = (int)v3x_ini_bool(&ini, "my_plugin", "enabled", 1);
    g_log_every = (int)v3x_ini_int(&ini, "my_plugin", "log_every", 0);
    v3x_ini_free(&ini);
    return enabled;
}

/* `reload my_plugin` / `reload` (API v5): main thread. Only settings that are safe to change live. */
static void on_reload(const char *event, void *data, void *user)
{
    (void)event; (void)user;
    if (!v3x_reload_is_for(data, TAG)) return;
    read_config();
    V3X_LOGI(api, TAG, "config reloaded (log_every=%d)", g_log_every);
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    if (!read_config()) { api->log(TAG, "disabled in my_plugin.ini"); return 0; }

    if (!V3X_HAS(api, hook_safe)) { api->log(TAG, "loader too old (needs API v4), nothing installed"); return 1; }
    if (my_plugin_resolve(api) != 0) { api->log(TAG, "anchor FAILED: nothing installed (game updated?)"); return 1; }

    int rc = api->hook_safe(my_plugin_anchors[0].result, tick_pre, tick_post, NULL);
    if (rc != 0) { api->log(TAG, "hook_safe FAILED (%d): nothing installed", rc); return 1; }
    api->register_command("mytick", "my_plugin: game tick count and timing", cmd_status, NULL);
    if (V3X_HAS(api, log_at)) api->on_event(V3X_EVENT_CONFIG_RELOAD, on_reload, NULL);   /* v5 loaders only */
    api->log(TAG, "observing the game tick (log_every=%d)", g_log_every);
    return 0;
}
```


## Source: `my_plugin.ini.example`

[templates/plugin_template/my_plugin.ini.example on GitHub](https://github.com/kleeedolinux/v3x/blob/main/templates/plugin_template/my_plugin.ini.example)

```ini
; my_plugin configuration. Copy to <game>/binaries/v3x/my_plugin.ini. Every key is optional;
; a missing file means these defaults.
[my_plugin]
; 0 = load but install nothing
enabled = 1
; write a log line every N game ticks (0 = never)
log_every = 0
```


## Source: `test_my_plugin.c`

[templates/plugin_template/test_my_plugin.c on GitHub](https://github.com/kleeedolinux/v3x/blob/main/templates/plugin_template/test_my_plugin.c)

```c
/* Offline test for my_plugin: no game process, nothing executes in the exe.
 *     python sdk/build_plugin.py templates/plugin_template --test [path/to/victoria3.exe]
 * Without an exe it checks the callbacks; with one it maps the exe read-only (sdk/v3x_testkit.h)
 * and runs the plugin's real anchor code against it, then loads the plugin against the fake API
 * (hooks are recorded, not installed). Exit code 0 = ALL OK. */
#include "v3x_testkit.h"
#include "my_plugin.c"

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    /* 1. callbacks: pre stores a timestamp, post counts */
    V3X_API fake;
    v3x_testkit_api(&fake);
    api = &fake;
    V3X_HookCall c; memset(&c, 0, sizeof c);
    tick_pre(&c);
    V3X_TEST_CHECK(c.scratch[0] != 0, "pre stores the start time in scratch[0]");
    tick_post(&c);
    V3X_TEST_CHECK(g_ticks == 1, "post counts one tick");
    g_log_every = 7;
    on_reload("config_reload", (void *)"other_plugin", NULL);
    V3X_TEST_CHECK(g_log_every == 7, "reload for another plugin is ignored");
    on_reload("config_reload", NULL, NULL);   /* no my_plugin.ini in the testkit dir: defaults */
    V3X_TEST_CHECK(g_log_every == 0, "reload (all) re-reads the config");

    /* 2. anchors against the real exe */
    if (argc > 1) {
        V3X_API real;
        if (v3x_testkit_map_exe(argv[1], &real) != 0) V3X_TEST_CHECK(0, "map %s", argv[1]);
        else {
            V3X_TEST_CHECK(my_plugin_resolve(&real) == 0, "anchors resolve on %s", argv[1]);
            printf("  game_tick at exe+0x%llx (1.15.1: 0x1465690)\n", v3x_testkit_rva(my_plugin_anchors[0].result));
            V3X_TEST_CHECK(v3x_plugin_load(&real) == 0 && v3x_tk.hooks == 1, "plugin loads and installs exactly 1 hook");
        }
    } else
        printf("(no exe given: anchor check skipped)\n");
    return v3x_testkit_done();
}
```
