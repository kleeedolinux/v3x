# Safety rules

Every rule here comes from a crash, a hang or a corrupted session while building the plugins in
this repo. A native plugin runs inside the game with no sandbox: one wrong pointer kills the
process, and a subtle mistake can corrupt a save without any crash. The rules are ordered by how
often they bit us.

## Argument forwarding

**Rule: a pass-through hook must forward every argument the engine function really takes.
Use `api->hook_safe`; if you write a raw detour, forward at least 8 integer slots (`V3X_PASS8`).**

What happened: `sim_opt`'s detour on the parallel-for helper `0xe02c90` (1.15.1) was written with
4 arguments. The function takes a 5th one on the stack (a source-location pointer at
`[rsp+0x28]`). The detour's own frame put garbage there, the original read it, and the game
crashed later at an unrelated-looking address (a NULL dereference at `0x7f2dbf`).

Why it is easy to get wrong: in the Win64 ABI arguments 1-4 travel in registers, so a 4-argument
detour on a 6-argument function "works" until the compiler reuses the stack slots. Nothing warns
you. There are no headers for the engine; you only know the argument count after reading the
callee.

How to avoid it:

* `api->hook_safe(target, pre, post, user)` (API v4) forwards all 16 integer slots and xmm0-3
  whatever the prototype is, and keeps the return value. Tested with 0-16 arguments, floats,
  doubles, mixed, struct returns, recursion and threads (`test/hook_safe_test.c`).
* For a raw detour (`api->hook`) on a function whose prototype you have not proven: the
  `V3X_PASS8_ARGS` / `V3X_PASS8` macros in `sdk/v3x_hook.h`. They do not preserve float
  arguments if your detour does floating-point work before calling the original.
* Measure: `tools/re_simopt_argcheck.py <exe> <rva>...` counts the stack arguments a function
  reads (callee side) and its callers pass (caller side).
* Two plugins cannot inline-hook the same function (MinHook returns 3,
  `MH_ERROR_ALREADY_CREATED`). If you need a function another plugin owns, subscribe to its event
  or ask for one.

## Script and RNG scope

**Rule: never run game script (effects, triggers, journal/event pulses, anything that may roll a
random number) outside an engine tick task. Check `v3x_script_random_allowed()` first.**

What happened (two separate crashes):

* `sim_opt stagger_journal` ran deferred journal pulses from the `save_begin` event. Saves run as
  a *session command on a job*, outside any tick task. The script RNG (`0x3787300`) requires an
  `ALLOW_RANDOM_IN_SCOPE` owner; there was none, and it dereferenced NULL at `0x3787360`, even
  though `GameStateAccess` read as Logic.
* `enhance fast_speed5` posted the next tick from the network/session thread. Ticks then ran
  back to back inside random scopes taken by other code; a scope exit restored NULL over the
  console's scope pointer, and the console crashed at the same address.

What the engine does (docs/re/simulation.md, docs/re/tick_par.md): the script RNG has ONE global
owner pointer (`exe+0x540ae80` on 1.15.1), not one per thread. The owner record holds the owning
thread id at `+0x10`. Tick execution sets it; parallel ranges and 38 other functions swap it with
`xchg`. So "am I on the right thread" is not enough: the scope must currently be owned by your
thread.

How to check:

```c
#include "v3x_game.h"
static V3xGame g;
/* load time */ v3x_game_resolve(api, &g);          /* finds the owner global by anchors, fail-closed */
/* in a hook */ if (v3x_script_random_allowed(&g) == 1) { /* script may roll random here */ }
```

`1` = a scope is active and owned by the calling thread; `0` = no (do not run script); `-1` =
not resolved (treat as no). This is necessary, not sufficient: you also need the right
`GameStateAccess` and no concurrent writers. The safe place to run script is inside a hook on a
tick task the engine itself runs (see "Ranked hook targets" in docs/re/simulation.md).

Related: the main thread (`on_frame`, `run_on_main`) renders and runs the UI. It is not a tick
task. Console commands run on the console input thread.

## DllMain

**Rule: do nothing in `DllMain`, and never do work in `DLL_PROCESS_DETACH`.**

* `DLL_PROCESS_ATTACH` runs under the loader lock. Creating a thread and waiting for it, loading
  libraries, or calling into the engine deadlocks or crashes (a Stellaris large-page loader
  crashed with `CreateThread` inside DllMain). Do your setup in `v3x_plugin_load`.
* `DLL_PROCESS_DETACH` at exit runs after `ExitProcess` killed every other thread. Any of them may
  have died holding a lock (the V3X log lock, a CRT `FILE` lock, the heap lock, MinHook's lock).
  Taking any of those hangs the process forever. The hang we hunted: the devtools sampler had
  suspended the main thread right as it entered `NtTerminateProcess`; the sampler was killed
  before resuming it.
* Since API v4 the core emits `process_exit` (from a hook on `RtlExitUserProcess`, all threads
  still alive). Write final reports there or on the `load_transition` Game->Empty event, keep it
  short, and never wait for other threads without a timeout. All V3X API calls refuse to do
  anything once the process is exiting.
* Never `FreeLibrary` your own plugin: a thread may still be inside your hook.

## Fail-closed anchors

**Rule: never hook or patch an address you have not proven on the running build. If any check
fails, install nothing and log why.**

* No hardcoded addresses. Every game patch moves everything. Find code by things that survive:
  strings the function references, RTTI class names, unique byte shapes.
* Prove uniqueness. `pattern_scan` returns the first match; a pattern with two matches is a coin
  flip. A string used by several functions is ambiguous. `sdk/v3x_anchor.h` refuses both:

  ```c
  static V3xAnchor A[] = {
      {"game_tick", "Processing Tick: ", "48 89 5C 24 ?? 48 89 74 24", V3X_ANCHOR_ALL_SAME},
  };
  if (v3x_anchor_resolve_all(api, TAG, A, 1) != 0) return 1;   /* logged, nothing installed */
  ```

* Check the shape, not only the location: prologue bytes, the call you expect at an offset, the
  field offset an instruction uses. Add a `check` callback for these.
* Per feature: if one anchor of an optional feature fails, disable that feature, not the whole
  plugin, and say so in the log (`feature X FAILED: <why>`; the console shows it red).
* Write an offline check (`sdk/v3x_testkit.h`) that runs the plugin's real anchor code against
  `victoria3.exe` mapped as an image, so a game update is caught before anyone starts the game.
* A manifest's `game_builds` documents what you tested; it does not replace anchors.

## Threads and locks in hooks

* A hook runs on whatever thread calls the function. Engine workers run with the scheduler's
  state; if you block them, the whole tick waits.
* No unbounded locks inside hooks. Never take a lock in a hook that your other code holds while
  calling into the engine (deadlock). Prefer `Interlocked*` counters.
* Do not `log` per call in hot functions (each log takes a global lock and writes the file).
  Count, and report from a console command or every N seconds.
* Do not allocate in code that runs while another thread is suspended (samplers): the
  suspended thread may hold the heap lock.
* Patch code only while it cannot run: at load time (before the game starts) or between ticks on
  the main thread, with workers idle.

## Reading game memory

* Prefer reading through `sdk/v3x_mem.h` (`v3x_mem_read_u64`, `v3x_mem_chain`,
  `v3x_mem_read_cstr`) in console commands, diagnostics and anything that follows reverse-
  engineered pointer chains. They never fault: VirtualQuery + ReadProcessMemory.
* A fault on the console thread or a worker kills the game. The devconsole plugin reads every
  byte this way, so a wrong address prints an error.
* Raw dereferences are fine for pointers the engine just handed you as arguments in a hook,
  with a layout you verified.

## Test offline first, then smoke test, with logs backed up

**Rule: an offline test must pass before the game starts with your plugin; then a short smoke
test; then a long session.**

1. **Offline** (`python sdk/build_plugin.py <dir> --test`): anchors against the real exe, config
   parsing, the pure logic of your plugin compiled into a test exe. No game process.
2. **Back up the previous run's logs** before every in-game test: `binaries/v3x/*.log`,
   `crash.log`, `crash.prev.log`, and the game's `Documents/Paradox Interactive/Victoria 3/logs/`.
   The next start overwrites them, and the crash you want to understand is usually in the run you
   just lost. (`crash_logger` keeps one previous `crash.prev.log`; that is not enough when you
   restart twice.)
3. **Smoke test**: start, load a save, play a few in-game days, save, load, quit to the menu,
   quit. Read `v3x.log` for `FAILED`/`WARNING`. A plugin that "loads only" is not a tested plugin;
   the README tables separate "loads (smoke test)" from "played (4h session)".
4. **Long session** for anything that changes behaviour: saves, autosaves, speed 5, a load of a
   second save, alt-tab, minimise.
5. Never test on your only copy of an ironman save.

## Changing simulation behaviour

* Anything that changes what the simulation computes changes saves and makes multiplayer
  desync. Single player only; make it opt-in (default off) and say so in the ini.
* "Declared independent" tick tasks are not safe to run in parallel: they share unsynchronized
  globals (profiler scope, render guards, the RNG owner). tick_par's analysis found no task pair
  that is provably safe (docs/re/tick_par.md).
* Skipping or deferring engine work is only safe if nothing reads its result before the next
  run, including a save that may happen at any pause.

## Disk and memory

* The game's commit can exceed 17 GB. Writing big files to a full C: fails saves
  (`Failed writing file`) and can crash the game. Write plugin outputs to `api->v3x_dir()`,
  rotate or cap them, and do not write per tick.
* Test scratch data (`TEMP`, `ZIG_GLOBAL_CACHE_DIR`) can be moved to another drive; see
  CONTRIBUTING.md.
