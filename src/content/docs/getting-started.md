---
title: "Getting started: install V3X and write your first plugin in 10 minutes"
summary: "Install the loader, build the template plugin and run it, in about 10 minutes."
group: modders
order: 10
source: "docs/getting-started.md"
sourceUrl: "https://github.com/kleeedolinux/v3x/blob/main/docs/getting-started.md"
updated: "2026-11-13"
---
V3X loads your compiled C/C++ code into `victoria3.exe`, so you can hook and patch engine
functions directly. This guide installs the loader, builds the template plugin, and runs it.
Read [safety.md](/docs/safety/) before you change game state.

**Single player only.** Native code is not part of the multiplayer checksum, so other players
would be out of sync. Back up your saves.

## 1. Requirements (2 minutes)

* Victoria 3 on Windows x64 (the plugins in this repo were reverse-engineered on **1.15.1**;
  other builds may make some plugins refuse to load, which is the intended fail-closed behaviour).
* Python 3.8+ and the zig compiler from pip: `pip install ziglang`. No Visual Studio needed.
  Any x64 Windows C compiler works (MSVC, clang, mingw); zig is the tested one.
* For the reverse-engineering tools: `pip install pefile capstone` (see [reverse-engineering.md](/docs/reverse-engineering/)).
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
python sdk/build_plugin.py templates/plugin_template --test
```

This builds `build/plugins/my_plugin.dll`, then builds and runs the offline test
`templates/plugin_template/test_my_plugin.c`. The test maps `../binaries/victoria3.exe`
read-only (nothing in it runs) and checks that the plugin's anchor finds its target:

```
[my_plugin] anchor game_tick at exe+0x1465690 ok
OK  : anchors resolve on ...victoria3.exe
OK  : plugin loads and installs exactly 1 hook
4 checks, 0 failed
ALL OK
```

If the anchor fails on your game build, the plugin would refuse to install anything. That is
the point: see [safety.md](/docs/safety/#fail-closed-anchors).

## 4. Run it (2 minutes)

```sh
python sdk/build_plugin.py templates/plugin_template --install
```

Start the game, load a save, unpause for a few days, and type into the V3X console (the
numbers below are illustrative; the template has not been run in the game as part of this repo's tests):

```
> plugins
  my_plugin            0.1.0      my_plugin.dll                loaded at 00007FF...
                       template: counts and times game ticks  -- your name
> mytick
my_plugin: 412 ticks, avg 9312 us, max 61044 us
```

## 5. Read the template (1 minute)

`templates/plugin_template/my_plugin.c` is about 100 lines:

```c
#include "v3x_sdk.h"

V3X_PLUGIN_MANIFEST("my_plugin", "0.1.0", "your name", "template: counts and times game ticks",
                    "release/1.15*");

V3xAnchor my_plugin_anchors[] = {
    {"game_tick", "Processing Tick: ", "48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24", V3X_ANCHOR_ALL_SAME},
};

static void tick_pre(V3X_HookCall *c)  { /* store a start time in c->scratch[0] */ }
static void tick_post(V3X_HookCall *c) { /* count, time; c->ret is the return value */ }

V3X_EXPORT int v3x_plugin_load(const V3X_API *a)
{
    api = a;
    if (!V3X_HAS(api, hook_safe)) return 1;                 /* loader too old */
    if (my_plugin_resolve(api) != 0) return 1;              /* anchor failed: install nothing */
    return api->hook_safe(my_plugin_anchors[0].result, tick_pre, tick_post, NULL) ? 1 : 0;
}
```

* `V3X_PLUGIN_MANIFEST` exports name, version, author, description and the supported game
  builds. The `plugins` command shows them; a build mismatch is logged as a WARNING.
* The anchor finds the function that references the log string `"Processing Tick: "` and checks
  its first bytes. Nothing is hardcoded.
* `api->hook_safe` installs a generic pass-through thunk: you never write a detour with a guessed
  prototype. `pre` sees every argument (`c->args[]`, `c->xmm[]`), `post` sees the return value.

## Next steps

| you want to | read |
|---|---|
| know every API function and on which thread it may be called | [api-reference.md](/docs/api-reference/) |
| not crash the game | [safety.md](/docs/safety/) |
| find engine functions | [reverse-engineering.md](/docs/reverse-engineering/), [engine-map.md](/docs/engine-map/) |
| see what the bundled plugins do | [plugins.md](/docs/plugins/) |
| understand a crash | [troubleshooting.md](/docs/troubleshooting/) |
| common questions | [faq.md](/docs/faq/) |
