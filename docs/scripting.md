# Extending game script from a V3X plugin

This guide is for modders who want a native V3X plugin to add game script (effects, triggers,
script values, events, on_actions), localization or GUI. Engine background:
[docs/re/script.md](re/script.md).

## What works today

| Layer | Status |
|---|---|
| **Ship script, loc and GUI files from a plugin** (`script_ext`, `sdk/v3x_script.h`) | implemented, tested offline (`test/script_ext_test.c`), **not run in game yet** |
| **Run effect or trigger text from C at runtime** (`run_effect_text`, `eval_trigger_text`, console `sxrun` / `sxtrigger`) | implemented, tested offline against the real exe (anchors) and fake engine functions (flow), **not run in game yet** |
| **New script keywords implemented in C** (`register_effect`, `register_trigger`: `v3x_<name> = ...`) | implemented, tested offline the same way, **not run in game yet** |

Everything marked "not run in game yet" is **unverified in a running Victoria 3**. The two runtime layers
call engine functions found through anchors (strings and byte shapes, docs/re/script.md section 6); if
any anchor does not match the game build the feature switches itself off and `v3x.log` says why. Check
`features()` or the console command `scriptext` before relying on them.

Shipping files covers most "add something to script" cases. Anything a mod can define, a plugin can
now define too: scripted effects and triggers, script values, events, decisions, journal entries,
on_actions, defines, localization and GUI. The plugin can also **generate** those files at load,
for example from its ini, the player's settings or data computed in C. The two runtime layers add what
files cannot do: run a piece of script **now** on request, and give script a **keyword whose body is C**.

## How it works

1. Your plugin's `v3x_plugin_load` runs before the game starts. It calls `script_ext` to write files
   into `binaries/v3x/content_packs/<pack>/`.
2. The `content_packs` plugin mounts every pack into the game's virtual file system when the VFS
   initializes. That happens after all plugins are loaded and before any script is read
   ([docs/re/vfs.md](re/vfs.md)).
3. The engine reads `common/...`, `events/...`, `localization/...` and `gui/...` from the pack
   exactly as it reads a mod's files.

Priority: packs sit above `game/` and **below Paradox mods** (mods are mounted later and go on top).
A new file name (for example `common/scripted_effects/my_plugin_effects.txt`) adds to the game; it
does not replace anything. A file with the same path as a vanilla file replaces the vanilla file.

Both `script_ext` and `content_packs` must be installed and enabled.

## API (`sdk/v3x_script.h`)

`v3x_script_get(api)` returns script_ext's table only once the loader has loaded `script_ext.dll`
(it never loads it itself, so `[plugins] script_ext = 0`, the crash guard and the build gate always
apply). Plugins load in file-name order, so a plugin whose name sorts before `script_ext` gets `NULL`
in its own `v3x_plugin_load`. Do the script work in the `plugins_loaded` event (API v6: main thread,
once, after the last `v3x_plugin_load`, still before the engine starts) and fall back to
`v3x_plugin_load` on older loaders. Check every result:

<!-- sample: file -->
```c
#include "v3x_script.h"

static const V3X_API *api;
static const char MY_EFFECTS[] = "my_pack_effect = { debug_log = \"my_pack_effect ran\" }\n";

static void write_my_pack(void)
{
    const V3X_ScriptExt *sx = v3x_script_get(api);   /* NULL: script_ext not installed / disabled */
    if (!sx) { api->log(TAG, "script_ext not loaded: no pack written"); return; }
    int rc = sx->begin_pack("my_pack");   /* creates the pack, deletes the files YOU wrote there last session */
    if (rc != V3X_SCRIPT_OK) { api->log(TAG, "begin_pack failed (%d)", rc); return; }
    rc = sx->write_file("my_pack", "common/scripted_effects/my_pack_effects.txt", MY_EFFECTS, sizeof MY_EFFECTS - 1);
    if (rc != V3X_SCRIPT_OK) api->log(TAG, "write_file failed (%d)", rc);
    int n = sx->end_pack("my_pack");     /* writes the manifest: the file count, or < 0 */
    if (n < 0) api->log(TAG, "end_pack failed (%d)", n);
}

static void on_plugins_loaded(const char *name, void *data, void *user)
{
    (void)name; (void)data; (void)user;
    write_my_pack();
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    if (api->api_version >= 6) {   /* every plugin is loaded when the event comes */
        int rc = api->on_event(V3X_EVENT_PLUGINS_LOADED, on_plugins_loaded, NULL);
        if (rc != V3X_OK) { api->log(TAG, "on_event failed (%d)", rc); write_my_pack(); }
    } else {
        write_my_pack();           /* older loader: works if script_ext loaded before this plugin */
    }
    return 0;
}
```

Rules:

* Call it from `plugins_loaded` (or `v3x_plugin_load`). Files written later in the session are not
  read: the game loads its script databases once, at startup.
* A pack belongs to the plugin that first called `begin_pack` for it; its manifest records the
  owner's DLL file name (`#owner my_plugin.dll`), so later sessions keep that owner.
  `begin_pack` / `write_file` / `end_pack` on another plugin's pack, from a plugin the loader
  disabled, or from code outside any DLL return `V3X_SCRIPT_EOWNER`.
* Pack names may contain letters, digits, `_`, `-` and `.`, up to 63 characters, and must not start
  with `_` or `.` (content_packs skips such folders). Paths are relative (`/` or `\`) with no `..`
  and no drive letter, up to 200 characters. Anything else returns `V3X_SCRIPT_EARG`.
* `*.yml` files get a UTF-8 BOM if they lack one, because the engine needs it for localization.
* Files are replaced atomically. A file you stop writing is deleted at the next `begin_pack`. Files
  in the pack that were not written by `script_ext` are never touched.
* Prefix your names (`my_plugin_...`). Vanilla uses no `v3x_` names (checked by the test against
  1.15.1), so `v3x_` is free for V3X's own content.
* In the V3X console, `scriptext` lists the packs written this session (with their owner) and every native
  keyword with its owner DLL and whether it was switched off.

## Complete example: `v3x_hello` logs the country every month

The plugin adds:

* a scripted effect `v3x_hello`
* an on_action that runs it for every country each month
* one localization key

The effect logs the scopes with the engine's own `debug_log` / `debug_log_scopes` (output goes to
`Documents/Paradox Interactive/Victoria 3/logs/debug.log`).

<!-- sample: complete -->
```c
/* plugins/hello_script/hello_script.c - build: python sdk/build_plugin.py plugins/hello_script --werror */
#include "v3x_sdk.h"
#include "v3x_script.h"

#define TAG "hello_script"
V3X_EXPORT const char *v3x_plugin_name = TAG;
V3X_PLUGIN_MANIFEST(TAG, "1.0.0", "you", "ships a v3x_hello scripted effect", "*");
static const V3X_API *api;

static const char EFFECTS[] =
    "# generated by hello_script\n"
    "v3x_hello = {\n"
    "    debug_log = \"v3x_hello ran\"\n"
    "    debug_log_scopes = yes\n"
    "}\n";

static const char ON_ACTIONS[] =
    "# appends to the vanilla on_action (root = country)\n"
    "on_monthly_pulse_country = {\n"
    "    on_actions = { v3x_hello_monthly }\n"
    "}\n"
    "v3x_hello_monthly = {\n"
    "    effect = { v3x_hello = yes }\n"
    "}\n";

static const char LOC[] =
    "l_english:\n"
    " v3x_hello_title: \"Hello from V3X\"\n";

static const struct { const char *path, *text; size_t n; } FILES[] = {
    {"common/scripted_effects/v3x_hello_effects.txt", EFFECTS, sizeof EFFECTS - 1},
    {"common/on_actions/v3x_hello_on_actions.txt", ON_ACTIONS, sizeof ON_ACTIONS - 1},
    {"localization/english/v3x_hello_l_english.yml", LOC, sizeof LOC - 1},
};

static void write_pack(void)
{
    const V3X_ScriptExt *sx = v3x_script_get(api);
    if (!sx) { api->log(TAG, "script_ext not loaded - nothing written"); return; }
    int rc = sx->begin_pack("hello_script");
    if (rc != V3X_SCRIPT_OK) { api->log(TAG, "begin_pack failed (%d)", rc); return; }
    for (size_t i = 0; i < sizeof FILES / sizeof FILES[0]; i++) {
        rc = sx->write_file("hello_script", FILES[i].path, FILES[i].text, FILES[i].n);
        if (rc != V3X_SCRIPT_OK) api->log(TAG, "write %s failed (%d)", FILES[i].path, rc);
    }
    int n = sx->end_pack("hello_script");
    api->log(TAG, n >= 0 ? "wrote %d file(s)" : "end_pack failed (%d)", n);
}

static void on_plugins_loaded(const char *name, void *data, void *user)
{
    (void)name; (void)data; (void)user;
    write_pack();
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    if (api->api_version >= 6 && api->on_event(V3X_EVENT_PLUGINS_LOADED, on_plugins_loaded, NULL) == V3X_OK) return 0;
    write_pack();   /* older loader (or on_event failed): works when script_ext loaded first */
    return 0;
}
```

Check it in game (not yet done by us):

1. `debug.log` has `v3x_hello ran` once per country per month.
2. `error.log` has no script errors for these files.
3. The console command `script_docs` lists `v3x_hello` among the scripted effects.

A scripted effect runs in the game's own script context (tick tasks, events, on_actions,
decisions). That is the safe place for script (docs/safety.md "Script and RNG scope"). Never run
script yourself from a hook or a frame callback with your own code: when you need to run script from
C, use `run_effect_text` below, which takes the script random scope the way the engine does.

Open point to check in game: the example assumes that a second `on_monthly_pulse_country`
definition **merges** its `on_actions` list into the vanilla one. That is the usual Jomini
behaviour, but it is not verified for 1.15.1. If it does not merge, trigger your on_action from an
event or a decision instead, or ship a copy of the vanilla on_action file with your line added. A
copy of a vanilla file breaks when the game patches that file.

## Run script text at runtime (`run_effect_text`, `eval_trigger_text`)

<!-- sample: file | static const V3X_API *api; -->
```c
#include "v3x_script.h"

/* main thread only: call it from a run_on_main job or a frame callback */
static void run_once(void)
{
    const V3X_ScriptExt *sx = v3x_script_get(api);
    if (!sx || !V3X_SCRIPT_HAS(sx, run_effect_text) || !(sx->features() & V3X_SCRIPT_F_RUN)) return;
    char err[256] = "", safe[256];
    int rc = sx->run_effect_text("add_treasury = 1000", err, sizeof err);
    if (rc != V3X_SCRIPT_OK) {
        v3x_sanitize_line(safe, sizeof safe, err);   /* err can carry script / mod text */
        api->log(TAG, "run_effect_text failed (%d): %s", rc, safe);
        return;
    }
    int res = 0;
    rc = sx->eval_trigger_text("is_ai = no", &res, err, sizeof err);   /* res = 0 / 1 when rc == OK */
    if (rc == V3X_SCRIPT_OK) api->log(TAG, "is_ai = no -> %s", res ? "true" : "false");
}
```

What it does: parses the text with the engine's own parser, with **the player's country as root scope**,
and executes it (or evaluates it) once. It calls the same engine functions as the object explorer's
Script Runner (docs/re/script.md section 5) and takes the script random scope the way that window does,
so `random_list` and friends work.

Rules:

* **Main thread only.** From any other thread you get `V3X_SCRIPT_ETHREAD`. From a console command or a
  worker thread use `api->run_on_main(fn, user)` (runs in the next frame tick on the main thread).
  `run_on_main` / `on_frame` are not tick tasks, and that is fine here only because script_ext itself
  takes the engine's script random scope for the duration of the call (docs/safety.md "Script and RNG
  scope"): it does not defer into a tick task. Do not run script any other way from a frame callback
  or a hook.
* A game must be loaded: otherwise `V3X_SCRIPT_ENOGAME`. Not inside another run (`V3X_SCRIPT_EBUSY`).
* If another thread holds the script random scope (a tick task is running), you get `V3X_SCRIPT_EBUSY`
  and nothing runs; try again on the next frame. Just like the engine's own console commands, a run can
  still collide with a simulation tick at high game speed (this is the engine's own order: random scope
  first, then game-state access): pause the game for anything important.
* `V3X_SCRIPT_ESCRIPT`: the engine's parser rejected the text; `err` holds its message (for example the
  unknown keyword or the line). `V3X_SCRIPT_OK` means the engine **ran** it. Errors raised while it
  executes (wrong scope, missing field) are written to the game's logs
  (`Documents/Paradox Interactive/Victoria 3/logs/`), not returned.
* `V3X_SCRIPT_EOFF`: an anchor failed (game updated?), `script_ext` is not loaded yet, or an earlier run
  faulted. The reason is in `v3x.log` (`script runner OFF: ...`) and in the console command `scriptext`.
* Text up to 32 KiB. No effect from this API is ever synchronized with multiplayer or checked against
  ironman: **do not use it in multiplayer** (it changes game state on one machine only).
* A hardware fault during a run (it would be an engine bug or a wrong anchor) restores the random scope
  and disables running until restart.

The effect and trigger names used in the examples (`add_treasury`, `is_ai`) are ordinary vanilla names
for a country scope; check them with the engine console command `script_docs` if the engine reports
an unknown keyword.

Console, in the V3X console window (they are V3X commands, not engine console commands):

```
sxrun add_treasury = 1000        -> effect on the player's country (queued to the main thread)
sxtrigger is_ai = no             -> prints "sxtrigger: true" or "false"
scriptext                        -> content packs, script runner status, native keywords
```

### Complete example: a plugin command `give_gold <n>`

The command handler runs on the console input thread, so it parses and range-checks the argument,
claims a static job slot with a compare-and-swap (nothing to leak if the job never runs, two commands
cannot overwrite each other) and hands the work to the main thread:

<!-- sample: complete -->
```c
/* plugins/give_gold/give_gold.c - build: python sdk/build_plugin.py plugins/give_gold --werror */
#include <errno.h>
#include <stdlib.h>
#include "v3x_sdk.h"
#include "v3x_script.h"

#define TAG "give_gold"
V3X_EXPORT const char *v3x_plugin_name = TAG;
V3X_PLUGIN_MANIFEST(TAG, "1.0.0", "you", "give_gold <n>: add n to the treasury through script", "*");
static const V3X_API *api;

static volatile int g_busy;   /* 0 = free, 1 = a job is queued (claimed with cas) */
static long g_amount;         /* written only by the thread that won the cas */

static void on_main(void *user)                       /* main thread, next frame tick */
{
    (void)user;
    const V3X_ScriptExt *sx = v3x_script_get(api);
    if (!sx || !V3X_SCRIPT_HAS(sx, run_effect_text) || !(sx->features() & V3X_SCRIPT_F_RUN)) {
        api->print("give_gold: script runner not available (see v3x.log)");
    } else {
        char text[64], err[256] = "", safe[256];
        V3xStr s;
        v3x_str_init(&s, text, sizeof text);
        if (v3x_str_appendf(&s, "add_treasury = %ld", g_amount) != V3X_R_OK) {   /* refuse a cut command */
            api->print("give_gold: internal error (text too long)");
        } else {
            int rc = sx->run_effect_text(v3x_str_cstr(&s), err, sizeof err);
            v3x_sanitize_line(safe, sizeof safe, err);
            api->print("give_gold: %s (%d) %s", rc == V3X_SCRIPT_OK ? "ran" : "failed", rc, safe);
        }
    }
    v3x_atomic_store_int(&g_busy, 0);                  /* release the slot */
}

static void cmd(const char *args, void *user)         /* console input thread: hand over to the main thread */
{
    (void)user;
    char *end = NULL;
    errno = 0;
    long n = strtol(args, &end, 10);
    while (end && *end == ' ') end++;
    if (end == args || !end || *end != 0 || errno == ERANGE || n < -1000000 || n > 1000000) {
        api->print("usage: give_gold <n>   (n = -1000000 .. 1000000)");
        return;
    }
    if (!v3x_atomic_cas_int(&g_busy, 0, 1)) { api->print("give_gold: busy, try again"); return; }
    g_amount = n;
    if (api->run_on_main(on_main, NULL) != V3X_OK) {
        api->print("give_gold: main-thread queue full, try again");
        v3x_atomic_store_int(&g_busy, 0);
    }
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    int rc = api->register_command("give_gold", "give_gold <n>: add n to the player's treasury through script", cmd, NULL);
    if (rc != V3X_OK) api->log(TAG, "register_command failed (%d)", rc);
    return 0;
}
```

Check it in game (not yet done by us): `give_gold 1000` in the V3X console prints `ran (0)`, the treasury
changes by 1000, `v3x.log` has the `script runner ready: ...` line from the first use (the anchors are
resolved lazily, about half a second the first time) and `error.log` has no script error.

## New script keywords in C (`register_effect`, `register_trigger`)

<!-- sample: skip | excerpt of the V3X_ScriptExt table in sdk/v3x_script.h (function pointer fields) -->
```c
int register_effect (const char *name, void (*cb)(const V3X_ScriptScope *scope, const char *value, void *user), void *user);
int register_trigger(const char *name, int  (*cb)(const V3X_ScriptScope *scope, const char *value, void *user), void *user);
```

After this, game script can write `v3x_my_effect = "some value"` as an effect, or `v3x_my_check = yes` as a
trigger, and the engine calls your C function with the scope it runs in and the text on the right of `=`.

How: the engine registers its ~700 effect and ~1000 trigger keywords from static initializers at
process start (docs/re/script.md section 1). script_ext hooks one of them (the `debug_log` effect's)
and, right after it, registers your keywords through the same engine functions with the same data
structure as a vanilla keyword. The new effect / trigger object is a copy of `debug_log`'s with
only Execute / Evaluate replaced by a function that calls your callback.

Rules:

* Call it **only from `v3x_plugin_load` or the `plugins_loaded` event** (the engine's static
  initialization must not have run yet), on the main thread. Later calls return `V3X_SCRIPT_ELATE`. A call that happens before script_ext's own
  load only queues the name; it is registered when the engine starts. After script_ext loaded,
  `V3X_SCRIPT_EOFF` means the anchors failed and nothing is hooked.
* Names: `v3x_` + `[a-z0-9_]`, at most 63 characters, unique per kind (`V3X_SCRIPT_EEXIST`), at most 64
  per kind (`V3X_SCRIPT_EFULL`). The prefix keeps you out of vanilla's namespace (the test checks
  that 1.15.1 has no `v3x_` string); do not also define a scripted effect with the same name, the
  engine would not tell the two apart.
* `value` is the text right of `=`: an identifier, a number or a quoted string without the quotes
  (`yes`, `1000`, `hello world`). Block syntax (`v3x_x = { ... }`) is not supported: the engine reports
  a script error for it. The text is valid only during the call.
* `scope->type` / `scope->id` are the engine's scope reference (`V3X_SCOPE_COUNTRY` = 5, `id` is the
  country id). Other scope type numbers are not named; docs/re/script.md section 4. The keyword accepts
  whatever scope `debug_log` accepts (any).
* **Your callback runs on whatever thread the engine executes the script on**, including tick worker
  threads and the UI thread (tooltips evaluate triggers). Keep it short, thread-safe and free of waits;
  call only `api->log` from the V3X API. A trigger may be evaluated thousands of times a day: make it
  pure and fast.
* A keyword belongs to the DLL that contains its callback. script_ext calls it under its own fault
  guard: a hardware fault (access violation, illegal instruction, divide by zero) in the callback is
  caught, that keyword is switched off until restart (an effect then does nothing, a trigger is
  false) and `v3x.log` names the fault, `<module>+0x<offset>` and the keyword. When the loader
  disables your plugin (a fault in another of its callbacks, a failed `v3x_plugin_load`), all its
  keywords are switched off the same way (`plugin_disabled` event).

> **Unsafe — when you really need it:** the guard is not a sandbox. It does not unwind your frames
> (a lock your callback held stays held, memory it was writing stays half-written), it does not
> catch C++ exceptions or a stack overflow, and memory the callback corrupted before the fault is
> not repaired. Keep callbacks free of locks and allocation, validate `value` (any text a mod
> writes reaches you) and read engine memory only through `v3x_mem_read` (sdk/v3x_mem.h).
* Script that uses a keyword must be loaded **after** it is registered: ship that script through
  `begin_pack` / `write_file` (the VFS reads it later), as in the example.

`features()` bits: `V3X_SCRIPT_F_REGISTER` (armed, waiting for the engine's static init),
`V3X_SCRIPT_F_REGISTERED` (done). Check them from a frame callback or console command, not from your
`v3x_plugin_load` (script_ext may load after you).

### Complete example: `v3x_note` effect and `v3x_scope_is_country` trigger

<!-- sample: complete -->
```c
/* plugins/native_kw/native_kw.c - build: python sdk/build_plugin.py plugins/native_kw --werror */
#include <string.h>
#include "v3x_sdk.h"
#include "v3x_script.h"

#define TAG "native_kw"
V3X_EXPORT const char *v3x_plugin_name = TAG;
V3X_PLUGIN_MANIFEST(TAG, "1.0.0", "you", "v3x_note effect and v3x_scope_is_country trigger", "*");
static const V3X_API *api;

/* v3x_note = "text": any thread (tick workers too); only logs. value is mod text: sanitize it. */
static void fx_note(const V3X_ScriptScope *scope, const char *value, void *user)
{
    (void)user;
    char safe[128];
    v3x_sanitize_line(safe, sizeof safe, value ? value : "");
    api->log(TAG, "v3x_note '%s' in scope type %u id %llu", safe, (unsigned)scope->type, (unsigned long long)scope->id);
}

/* v3x_scope_is_country = yes|no: true if the current scope is (or is not) a country; pure and fast */
static int tr_is_country(const V3X_ScriptScope *scope, const char *value, void *user)
{
    (void)user;
    int is = scope->type == V3X_SCOPE_COUNTRY;
    return value && strcmp(value, "no") == 0 ? !is : is;
}

static const char EFFECTS[] =
    "v3x_native_demo = {\n"
    "    if = {\n"
    "        limit = { v3x_scope_is_country = yes }\n"
    "        v3x_note = \"demo ran in a country\"\n"
    "    }\n"
    "}\n";

static const char ON_ACTIONS[] =
    "on_monthly_pulse_country = { on_actions = { v3x_native_demo_monthly } }\n"
    "v3x_native_demo_monthly = { effect = { v3x_native_demo = yes } }\n";

static void setup(void)
{
    const V3X_ScriptExt *sx = v3x_script_get(api);
    if (!sx || !V3X_SCRIPT_HAS(sx, register_effect)) { api->log(TAG, "script_ext (version 2) not loaded"); return; }
    int e = sx->register_effect("v3x_note", fx_note, NULL);
    int t = sx->register_trigger("v3x_scope_is_country", tr_is_country, NULL);
    api->log(TAG, "register_effect %d, register_trigger %d", e, t);     /* 0 = queued */
    if (e || t) return;                  /* do not ship script that uses keywords that do not exist */
    int rc = sx->begin_pack(TAG);
    if (rc != V3X_SCRIPT_OK) { api->log(TAG, "begin_pack failed (%d)", rc); return; }
    if (sx->write_file(TAG, "common/scripted_effects/v3x_native_demo_effects.txt", EFFECTS, sizeof EFFECTS - 1) != V3X_SCRIPT_OK ||
        sx->write_file(TAG, "common/on_actions/v3x_native_demo_on_actions.txt", ON_ACTIONS, sizeof ON_ACTIONS - 1) != V3X_SCRIPT_OK)
        api->log(TAG, "write_file failed: the demo script is incomplete");
    rc = sx->end_pack(TAG);
    if (rc < 0) api->log(TAG, "end_pack failed (%d)", rc);
}

static void on_plugins_loaded(const char *name, void *data, void *user)
{
    (void)name; (void)data; (void)user;
    setup();
}

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    if (api->api_version >= 6 && api->on_event(V3X_EVENT_PLUGINS_LOADED, on_plugins_loaded, NULL) == V3X_OK) return 0;
    setup();   /* older loader (or on_event failed): works when script_ext loaded first */
    return 0;
}
```

Check it in game (not yet done by us), in this order, and stop at the first failure:

1. `v3x.log` has `keyword registration armed: registrar exe+0x5d9110 hooked ...` at start, then
   `registered effect v3x_note (token N)`, `registered trigger v3x_scope_is_country (token M)` and
   `static init: 2 script keyword(s) registered`. The console command `scriptext` lists both.
2. The console command `script_docs` (engine) lists `v3x_note` and `v3x_scope_is_country` (their description is
   `V3X native effect (script_ext): ...`; whether script_docs prints descriptions is not known).
3. After a month, `v3x.log` has `v3x_note 'demo ran in a country' in scope type 5 id ...` once per
   country, and `error.log` has no script errors for these files.
4. Quick test without waiting a month: `sxrun v3x_note = "hello"` prints `done` and logs the line once
   with the player's country id.
5. `sxtrigger v3x_scope_is_country = yes` prints `true`; `sxtrigger v3x_scope_is_country = no` prints `false`.

If step 1 shows `keyword registration OFF: ...` the reason names the anchor; nothing was hooked.

## Not available / limits

* No block syntax for native keywords, no per-keyword supported-scope mask (all scopes), no tooltip
  text, no localization of the keyword. A native keyword is a thin hook, not a full script type.
* `run_effect_text` always uses the player's country as root; other roots (a state, a character) are not
  supported.
* No multiplayer, ironman or achievement check on either runtime layer.
* Until an in-game check says otherwise, treat both runtime layers as experimental. Use script itself
  (shipped files) where that is enough, and communicate from C to script by generating files at load
  (values, lists, effects built from your data). Communicate from script to C with a native keyword
  or by reading what script writes to the logs (`debug_log`).

## Files

* `sdk/v3x_script.h`: API (version 2) and the `v3x_script_get` helper, `V3X_SCRIPT_HAS`
* `plugins/script_ext/script_ext.c`: plugin (exports `v3x_script_ext`; console `scriptext`, `sxrun`, `sxtrigger`)
* `plugins/script_ext/sx_pack.h`: file writer
* `plugins/script_ext/sx_engine.h`: Script Runner anchors and the run / eval flow
* `plugins/script_ext/sx_reg.h`: keyword registration anchors, vtable copies, callback wrappers
* `plugins/script_ext/sx_guard.h`: owners (packs, keywords), the fault guard around keyword callbacks, disabled plugins
* `test/script_ext_test.c`: offline test (`ONLY=script_ext test/run_all.sh`). It checks the RE anchors in
  docs/re/script.md against the exe, the engine-call flow against fake functions and the name rules.
* `tools/re_script_rtti.py`: RTTI vtable list used for the RE
