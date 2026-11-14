# FAQ

**Is this a mod? Does it work with Workshop mods?**
It is a native code loader, a layer below mods (like SKSE for Skyrim). Normal mods keep working;
V3X plugins run alongside them. `content_packs` can override game files without a mod.

**Does it work in multiplayer?**
No. Native code is not part of the game's checksum, so other players would run different code.
Plugins that change the simulation would desync. Use V3X in single player only.

**Achievements / ironman?**
V3X does not touch achievement code, but whether achievements remain valid depends on what your
plugins do (the `ironman_console` plugin enables the console in ironman). That is your call.

**Will it break saves?**
Read-only plugins (profilers, loggers) cannot. Plugins that change the simulation (`sim_opt
throttle`, `enhance speed_table`) change what is computed and therefore what is saved. They are off
by default. Back up saves before testing anything new.

**What happens when the game updates?**
Every plugin finds its targets by anchors (strings, RTTI, byte shapes) and refuses to install
anything when an anchor does not match. After a patch some plugins may log `anchor ... FAILED`
and do nothing until their anchors are updated. Nothing in this repo uses hardcoded addresses
at runtime; the RVAs in docs are for orientation only (1.15.1).

**Which compilers can build plugins?**
Any C or C++ compiler for x64 Windows. The repo uses zig (`pip install ziglang`, `zig cc
-target x86_64-windows-gnu`), which needs no Visual Studio. MSVC: `cl /LD /O2 /Iinclude /Isdk
my_plugin.c`. The API is plain C; in C++ include the headers as usual (`V3X_EXPORT` adds
`extern "C"`).

**Can I write plugins in Rust / Zig / C#?**
Anything that produces an x64 DLL exporting `v3x_plugin_load` with the C ABI and reads the
`V3X_API` struct from `include/v3x.h`. Rust with `#[no_mangle] extern "C"` and a `#[repr(C)]`
copy of the struct works in principle; nothing but C is tested here.

**How do I write a plugin that does not crash the game?**
Start from `templates/plugin_template/` (it uses only the safe APIs and builds with `-Werror`) and
follow [safe-coding.md](safe-coding.md): `V3xStr` for strings, `sdk/v3x_mem.h` for engine memory,
`V3xSpan` for file bytes, checked arithmetic, atomics for shared state, `hook_safe` on fail-closed
anchors, `v3x_path_join_checked` for paths from data, every result checked. The engine-specific
rules (script scope, DllMain, patching) are in [safety.md](safety.md).

**Why `hook_safe` instead of `hook`?**
A raw detour must match the function's real prototype. The engine has no headers, and a detour
with too few arguments corrupts the call (it crashed the game once). `hook_safe` forwards every
argument and the return value whatever the prototype, gives your callback all of them, restores
every register the caller may rely on, and can be shared with other plugins. A raw `hook` is an
unsafe escape hatch for replacing a function whose exact prototype you have proven
([safe-coding.md](safe-coding.md#hooks) says what can go wrong and how to contain it).

**Can two plugins hook the same function?**
Yes, with `hook_safe`: subscribers share one thunk and run nested in load order (pre A, pre B,
original, post B, post A), up to 8 per function. A raw `hook` is exclusive: on a function that
already has a raw hook or `hook_safe` subscribers it returns `V3X_EHOOKBUSY` (100) and v3x.log
names the holder. Check the result and fail closed; if you need a function another plugin holds
with a raw hook, ask its owner to move to `hook_safe` or to emit an event.

**On which thread does my code run?**
`v3x_plugin_load`, `on_frame`, `run_on_main`: main thread. Console commands: the thread that ran
the command (the console input thread, or the devbridge pipe thread). Hook callbacks: whichever
engine thread called the function. Event subscribers: the emitting thread. Share data between them
only through atomics (`v3x_atomic_*`), and assert the main thread in code that touches game state
(`V3X_ASSERT_MAIN(api)`). See the table in
[api-reference.md](api-reference.md#thread-rules-at-a-glance) and
[safe-coding.md](safe-coding.md#threads).

**Can I run game script / effects from my plugin?**
Only from inside a tick task, and only after `v3x_script_random_allowed()` returns 1 (the
script RNG needs a scope owned by the current thread). Not from `on_frame`, console commands,
`save_begin` or your own threads. See [safety.md](safety.md#script-and-rng-scope).

**How do I call an engine function?**
Prefer not to: observe it with `hook_safe`, or use what the bundled plugins expose (events,
`sdk/v3x_script.h`). Most engine functions assume locks and state (`GameStateAccess`, scopes)
that only exist in specific contexts.

> **Unsafe — when you really need it:** calling an engine function through a function pointer.
> * What can go wrong: a wrong prototype corrupts the stack; the right prototype on the wrong
>   thread or at the wrong time breaks engine invariants, often only minutes later or in the save.
> * How to contain it: resolve the address with a fail-closed anchor plus a `check` callback,
>   prove the prototype (`tools/re_simopt_argcheck.py`, the callers' argument setup), and call it
>   only from a hook on a function the engine calls in the same context (same thread, same
>   locks), never from a console command or your own thread.

**How do I find a function?**
[reverse-engineering.md](reverse-engineering.md): strings -> xrefs -> function -> profile ->
anchor -> offline check. [engine-map.md](engine-map.md) lists what is already known.

**How do I disable a plugin?**
`binaries/v3x/v3x.ini`:
```ini
[plugins]
lazy_mesh = 0
```
or the main-menu V3X button (mod_menu), or delete the DLL. `V3X_DISABLE=1` disables V3X entirely.

**Where are the logs?**
`binaries/v3x/v3x.log` (all of V3X and the plugins), `binaries/v3x/crash.log`, plugin outputs in
`binaries/v3x/`. The game's own logs: `Documents/Paradox Interactive/Victoria 3/logs/`.

**The V3X console window: can I close it?**
Its close button is removed because closing a console window kills the process it belongs to
(the game). Hide it with `console=0` in `binaries/v3x/v3x.ini`.

**Is it a virus? Why does it use version.dll?**
Windows loads `version.dll` from the exe's folder before the system one (it is not a KnownDLL).
V3X uses this to get loaded and forwards all 17 real functions to `System32\version.dll`. The
source is all here; build it yourself. Some antivirus tools flag any DLL proxy heuristically.

**Linux / Proton?**
Untested. The loader uses only Win32 APIs that Wine implements, but the version.dll override may
need `WINEDLLOVERRIDES="version=n,b"`.
