# Getting started: install V3X and write your first plugin in 10 minutes

V3X loads your compiled C/C++ code into `victoria3.exe`, so you can observe and extend engine
functions from native code. This guide installs the loader, builds the template plugin, and runs it.
The template uses only the safe APIs; [safe-coding.md](safe-coding.md) explains each rule, and
[safety.md](safety.md) the engine-specific ones. Read both before you change game state.

**Single player only.** Native code is not part of the multiplayer checksum, so other players
would be out of sync. Back up your saves.

## 1. Requirements (2 minutes)

* Victoria 3 on Windows x64 (the plugins in this repo were reverse-engineered on **1.15.1**;
  other builds may make some plugins refuse to load, which is the intended fail-closed behaviour).
* Python 3.8+ and the zig compiler from pip: `pip install ziglang`. No Visual Studio needed.
  Any x64 Windows C compiler works (MSVC, clang, mingw); zig is the tested one.
* For the reverse-engineering tools: `pip install pefile capstone` (see [reverse-engineering.md](reverse-engineering.md)).
* A shell: Git Bash for `build.sh`, or cmd/PowerShell for the Python build script.

## 2. Install the loader (2 minutes)

Put (or clone) this repository as a folder **inside** the game folder, next to `binaries/`:

```
<Steam>/steamapps/common/Victoria 3/
    binaries/victoria3.exe
    v3x/                 <- this repository
```

Then build and install:

```sh
cd "<Steam>/steamapps/common/Victoria 3/v3x"
./build.sh install       # builds build/version.dll + build/plugins/*.dll, copies them into ../binaries
```

This puts `binaries/version.dll` (the loader) and `binaries/v3x/plugins/*.dll` in place. Start
the game normally (Steam or the launcher). A "V3X console" window opens next to it; the log is
`binaries/v3x/v3x.log`.

* Not all bundled plugins are for everyone. Disable one without deleting it in
  `binaries/v3x/v3x.ini`:
  ```ini
  [plugins]
  alloc_tracker = 0
  lazy_mesh = 0
  ```
  The key is the DLL file name without `.dll` (case-insensitive); `0`, `off`, `no` or `false`
  skip it, a missing key means enabled. The `plugins` console command lists skipped plugins as
  `disabled (v3x.ini [plugins])`. The `mod_menu` plugin edits this section from the main menu.
* No console window: `console=0` in `binaries/v3x/v3x.ini`, or the env var `V3X_NO_CONSOLE=1`.
* Disable V3X for one run: env var `V3X_DISABLE=1`.
* Uninstall: delete `binaries/version.dll` and `binaries/v3x/`. Steam "Verify files" does
  not remove version.dll (it is not a game file).

## 3. Build the template plugin (3 minutes)

```sh
python sdk/build_plugin.py templates/plugin_template --werror --test
```

This builds `build/plugins/my_plugin.dll` with the release hardening flags and `-Werror` (a
format mismatch or an ignored must-use result is a build error), then builds and runs the offline
test `templates/plugin_template/test_my_plugin.c`. The test maps `../binaries/victoria3.exe`
read-only (nothing in it runs) and checks that the plugin's anchor finds its target, that its
extra shape check fails closed on a wrong address, and that the status line stays inside its
buffer:

```
[my_plugin] anchor game_tick at exe+0x1465690 ok
OK  : anchors resolve on ...victoria3.exe
OK  : anchor check fails closed on a mid-function address and on a non-exe address
OK  : plugin loads and installs exactly 1 hook
9 checks, 0 failed
ALL OK
```

Add `--sanitize` to build the test with UBSan (undefined behaviour stops the test).

If the anchor fails on your game build, the plugin would refuse to install anything. That is
the point: see [safety.md](safety.md#fail-closed-anchors).

## 4. Run it (2 minutes)

```sh
python sdk/build_plugin.py templates/plugin_template --werror --install
```

**Allow your plugin once.** The loader only runs plugin DLLs it can trust
([ADR 0002](adr/0002-trust-model.md)): the ones V3X ships are listed in a signed `plugins.json`, yours
is not. On the first start after installing it, the V3X console says
`1 plugin(s) were NOT loaded because they are not signed by V3X and not in your trusted list`, and
nothing of `my_plugin.dll` has run. Type:

```
> trust my_plugin
trusted my_plugin.dll (sha256 ...). Restart the game to load it.
```

and restart the game. `trust` stores the SHA-256 of exactly that file in your per-user list
`%LOCALAPPDATA%\V3X\trusted_plugins.txt` (delete its line to undo). A rebuilt DLL has a new hash, so
after every rebuild you trust it again, or use the developer switch below.

> **Unsafe — when you really need it:** `V3X_TRUST=dev` (no trust check at all).
> * When: you rebuild and restart many times while you develop.
> * What can go wrong: the check is off for **every** DLL in `binaries/v3x/plugins/`, including one
>   another program dropped there; `v3x.log` logs a WARNING at every start while it is on.
> * How to contain it: set it only for the game process you start for testing, e.g. from a terminal
>   (`set V3X_TRUST=dev` in cmd, then start `binaries\victoria3.exe` from that terminal), not as a
>   permanent user variable on a machine you also play on. Keep only DLLs you built in the plugins
>   folder. (Not verified here whether every launch path passes the variable on; if `plugins` does
>   not show `trust: dev (unchecked)`, use `trust my_plugin`.)

Start the game, load a save, unpause for a few days, and type into the V3X console (the
numbers below are illustrative; the template has not been run in the game as part of this repo's tests):

```
> plugins
  my_plugin            0.2.0      my_plugin.dll                loaded at 00007FF...
                       template: counts and times game ticks  -- your name
                       trust: trusted (allowlist); capabilities: hook
> mytick
my_plugin: 412 ticks, avg 9312 us, max 61044 us
```

## 5. Read the template (1 minute)

`templates/plugin_template/my_plugin.c` is about 150 lines. Its core, shortened (this excerpt
compiles on its own; the full file adds the timing, the ini, the console command and reload):

<!-- sample: complete -->
```c
#include "v3x_sdk.h"

/* "hook": the only capability it uses; the loader (API v6) refuses patch / exec for it */
V3X_PLUGIN_MANIFEST_CAPS("my_plugin", "0.2.0", "your name", "template: counts and times game ticks",
                         "release/1.15*", 0u, 0u, "hook");

static const V3X_API *api;
static volatile LONG64 g_ticks;   /* written by the game's tick thread, read by the console */

static V3xAnchor my_plugin_anchors[] = {
    {.name = "game_tick", .string = "Processing Tick: ",
     .pattern = "48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24", .xref = V3X_ANCHOR_ALL_SAME},
};

static void tick_pre(V3X_HookCall *c) { c->scratch[0] = (uint64_t)v3x_qpc(); }   /* per call */
static void tick_post(V3X_HookCall *c) { (void)c; InterlockedIncrement64(&g_ticks); }

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    if (!V3X_HAS(api, hook_safe)) return 1;                                   /* loader too old */
    if (v3x_anchor_resolve_all(api, "my_plugin", my_plugin_anchors, 1) != 0)
        return 1;                                                             /* fail closed */
    int rc = api->hook_safe(my_plugin_anchors[0].result, tick_pre, tick_post, NULL);
    if (rc != V3X_OK) { api->log("my_plugin", "hook_safe FAILED (%d)", rc); return 1; }
    return 0;
}
```

* `V3X_PLUGIN_MANIFEST_CAPS` exports name, version, author, description, the supported game
  builds and the capabilities. The `plugins` command shows them; a build mismatch is logged as a
  WARNING. Declare only what you use.
* The anchor finds the function that references the log string `"Processing Tick: "`, requires
  every reference to be in that one function, and checks its first bytes. Nothing is hardcoded;
  if anything does not match, the plugin installs nothing and says why in `v3x.log`.
* `api->hook_safe` installs a generic pass-through thunk: you never write a detour with a guessed
  prototype. `pre` sees every argument (`c->args[]`, `c->xmm[]`), `post` sees the return value.
  State from `pre` to `post` goes in `c->scratch[]`, counters shared across threads are atomic.
* Every result is checked; returning non-zero makes the loader roll the plugin back.

## Next steps

| you want to | read |
|---|---|
| know every API function and on which thread it may be called | [api-reference.md](api-reference.md) |
| write code that cannot overrun, race or read freed memory | [safe-coding.md](safe-coding.md) |
| not crash the game | [safety.md](safety.md) |
| find engine functions | [reverse-engineering.md](reverse-engineering.md), [engine-map.md](engine-map.md) |
| see what the bundled plugins do | [plugins.md](plugins.md) |
| understand a crash | [troubleshooting.md](troubleshooting.md) |
| common questions | [faq.md](faq.md) |
