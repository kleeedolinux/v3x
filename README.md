# V3X — native code loader for Victoria 3

Regular Victoria 3 mods are script and data (`common/`, `events/`, GUI) that the engine interprets.
V3X is a different layer: it runs **your compiled code inside `victoria3.exe`**, so plugins can
hook, replace and patch engine functions directly, like a script extender.

## How it works

1. **Proxy DLL.** `victoria3.exe` imports `VERSION.dll`, and version.dll is not a Windows
   KnownDLL. Windows therefore loads `binaries/version.dll` (ours) before the system one. Our
   DLL forwards all 17 exports to `System32\version.dll` (`src/proxy.c`), so the game behaves
   normally.
2. **Entry hook.** In `DllMain`, V3X installs a MinHook hook on the exe's entry point and does
   nothing else under the loader lock (`src/core.c`).
3. **Plugin load.** On the main thread, before the CRT and static constructors run, V3X loads
   every `binaries/v3x/plugins/*.dll` and calls `v3x_plugin_load(&api)`. Then the game starts.
4. **Frame tick.** A `PeekMessageW` hook calls the plugins' `on_frame` callbacks on the main
   thread, about once per frame.

### Finding engine code that survives patches

Hardcoded addresses break on every game update. The API instead lets you anchor on things that
rarely change:

```c
void *s  = api->find_string("No Console commands in ironman!"); // .rdata
void *x  = api->find_xref(s, 0);        // the instruction that loads that string
void *fn = api->function_start(x);      // enclosing function, from the .pdata unwind table
api->hook(fn, my_detour, (void **)&original);
```

`pattern_scan("48 8B 05 ?? ?? ?? ??")` (IDA-style signatures) is also available, along with
`patch()` for raw byte writes.

## Install / uninstall

```sh
./build.sh install      # builds, copies version.dll + plugins into ../binaries
```

* Uninstall: delete `binaries/version.dll` (and `binaries/v3x/`). Steam "Verify files" does not
  remove it, because version.dll is not a game file.
* Disable for one run: set the environment variable `V3X_DISABLE=1`.
* Log: `binaries/v3x/v3x.log`.
* Live console: a "V3X console" window opens next to the game and shows the loader and plugin
  logs in color (red = failures and exceptions). Its close button is removed, because closing a
  console window kills the game. Turn it off with `console=0` in `binaries/v3x/v3x.ini` or
  `V3X_NO_CONSOLE=1`. You can type commands into it (see [Console commands](#console-commands)).
* Enable / disable plugins: main menu > **V3X** button (plugin `mod_menu`, needs `content_packs`).
  Each tick is saved to the `[plugins]` section of `binaries/v3x/v3x.ini` (`name=0` = the DLL is
  not loaded) and takes effect at the next start. You can also edit that section by hand, or use the
  console: `modmenu toggle <plugin>`. mod_menu writes the menu to
  `binaries/v3x/content_packs/v3x_menu/` at every start. **When you remove or disable mod_menu,
  delete that folder too.** A Paradox mod that changes `gui/frontend/frontend_main.gui` is mounted
  above content packs, so it wins and the V3X button does not show (mod_menu logs a WARNING naming
  the mod; the console still works). Details: `docs/re/gui.md`.

## Writing a plugin

See `include/v3x.h` (the whole API) and `plugins/hello/hello.c` (template). Any C or C++
compiler targeting x64 Windows works. With this repo's toolchain:

```sh
python -m ziglang cc -target x86_64-windows-gnu -O2 -shared -Iinclude -o my.dll my.c
```

Plugins run at the very start of the process: code is mapped and hookable, but game globals
do not exist yet. Hook functions now and do the work when they are called, or use `on_frame`.

The API is versioned (`api_version`, `struct_size`); check them before using newer fields:

* v1: logging, scanning (`pattern_scan`, `find_string`, `find_xref`, `function_start`), hooks,
  `patch`, `on_frame`, paths.
* v2: console commands: `register_command`, `print`, `run_on_main`, `exec_command`.
* v3: `exec_capture` (run a command and also receive its `print`/`log` output in a callback),
  and named events between plugins: `on_event` / `emit_event` (e.g. `load_transition`).

## Plugins

Every plugin finds its targets by string / RTTI anchors plus byte checks (reverse-engineered on
1.15.1) and **fails closed**: if an anchor does not match, that plugin (or that feature) changes
nothing and says why in `v3x.log`. "built+tests" = builds and its offline test passes;
"in game" = how far it has been run in the real game (1.15.1).

| plugin | built+tests | in game | what it does | default | console |
|---|---|---|---|---|---|
| `hello` | yes | played (4h session) | template; logs on load and every 3000 frames | always on | - |
| `ironman_console` | yes | played (4h session) | the developer console works in ironman: hooks the "is the console blocked?" predicate and clears only the ironman reason, so multiplayer stays blocked | always on | - |
| `crash_logger` | yes | played (4h session) | own crash log `v3x/crash.log` (previous run: `crash.prev.log`): C++ throws with type names, fatal SEH exceptions with registers and stack, abort, big memory changes | always on | - |
| `devtools` | yes | played (4h session) | CPU per thread, optional ~1 kHz sampling profiler, memory and GPU memory reports in `v3x/devtools.log`, profile CSVs (per load phase with API v3) | profiler off | `profile` |
| `devconsole` | yes | loads only (smoke test); commands not typed in game yet | reverse-engineering commands with Zydis disassembly; reads memory only | always on | `dis` `fn` `xref` `scan` `mem` `mods` |
| `devbridge` | yes | played (4h session); MCP answered live | runs console commands for a local tool (`mcp/v3x_mcp.py`) over the named pipe `\\.\pipe\v3x_devbridge` and returns their output; adds no commands or hooks | on | - |
| `log_filter` | yes | played (4h session) | drops / dedupes noisy engine log messages in `spdlog::logger::sink_it_` before any log file sees them; stats in `v3x/log_filter_stats.txt` | on, 5 noise rules | - |
| `content_packs` | yes | played (4h session); loads, no packs tested | mounts every folder in `v3x/content_packs/` on top of the game's virtual file system (file overrides without a mod; optional `order.txt`; `_name` = disabled) | always on | `packs` |
| `mod_menu` | yes | not run in game | **V3X** button + plugin window on the main menu: hooks one GUI static initializer (OpenWikiUrl registrar) to register the GUI functions `V3XToggle<i>`, generates `content_packs/v3x_menu/gui/frontend/frontend_main.gui` from the vanilla file, ticks write `[plugins]` in `v3x.ini` (restart needed); fails closed to the vanilla menu | on | `modmenu` |
| `keep_history` | yes | played (4h session) | graphs of a country you stop playing are no longer wiped (NOPs the 11 `Trend::Clear` calls); `record_all` also samples AI countries like players (28 days, daily) | on | - |
| `tick_profiler` | yes | played (4h session) | per-tick-task wall-time profile (hook on `RunTask`); top-25 table every 60 s and `v3x/tick_profile.csv` | on (`V3X_NO_TICKPROF=1` = off) | `ticks` |
| `sim_opt` | yes | loads only (smoke test) | tick diagnostics via a slot-6 wrapper on every `CTickTask_` vtable; opt-in `throttle` (skip 1 of N runs) and `stagger_journal` (spread the monthly journal pulse) change the simulation, single player only | only `diag_trades` (read-only) | `simopt` |
| `fast_load` | yes | played (4h session); `pump_yield`/`worker_spin`: loads only (smoke test) | load-time measurement hooks, `v3x/load_timeline.txt`; emits `load_transition(_begin)`; optional `mesh_threads`, `pump_yield`, `worker_spin` | timeline on, no behaviour change | - |
| `mem_tuner` | yes | played (4h session); periodic collect / census / gui pool: loads only (smoke test) | tunes the engine's mimalloc options before CRT init (lower commit), `mi_collect` after loads and on commit growth, one OOM rescue, mimalloc error log, commit census | on | `memcollect` `memstat` |
| `alloc_tracker` | yes | played (4h session) | diagnostic: hooks mimalloc `mi_new`/`mi_malloc`/`mi_free`, live and total bytes per call site in `v3x/alloc_report.txt` | off | `allocs` |
| `cpu_sched` | yes | played (4h session) | on hybrid (P/E) CPUs: main thread and ABOVE_NORMAL workers on P-cores, BELOW_NORMAL workers on E-cores (CPU sets); optional `task_threads`; does nothing on non-hybrid CPUs | on | `cpusched` |
| `frame_pacer` | yes | played (4h session) | replaces the frame-cap busy-wait with a timer sleep; lower caps when idle, paused, in background or minimised; optional `tick_render_hz` | on | `frame_pacer` |
| `gfx_driver` | yes | played (4h session) | creates the D3D11 device with `PREVENT_INTERNAL_THREADING_OPTIMIZATIONS` (fewer GPU-driver threads on iGPUs); retries without it on failure | on | - |
| `asset_stream` | yes | played (4h session) | forces the engine's own texture streamer on (every quality preset turns it off), budget `cache_mb` | off | `assetstream` |
| `lazy_mesh` | yes | played (4h session) (incl. `lazy`); prefetch / merge: loads only (smoke test) | `measure` records which mesh types are really used (`v3x/lazy_mesh_used.txt`); `lazy` loads deferred mesh types on first use | off | `lazymesh` |
| `map_cache` | yes | loads only (smoke test) | measures repeated texture disk reads (weak texture cache) behind map-mode / GUI lag, `v3x/map_cache_report.txt`; `mode=on` keeps re-read textures alive (LRU) | `measure` | `mapcache` |
| `enhance` | yes | loads only (smoke test) | single-player engine fixes: `binary_saves_in_debug` (debug_mode no longer forces 156 MB text saves), `deflate_level`, `speed_table`, `save_timing`, `lean_debug`, `fast_speed5` (*experimental*); always hooks `SaveGameState` to emit `save_begin`/`save_end` | binary saves + save log on | `enhance` `speed` `lean` `fast5` |

### Configuration

Each plugin reads `binaries/v3x/<plugin>.ini`; every key is optional and a missing file means the
defaults below. Where a `plugins/<p>/<p>.ini.example` exists it holds the defaults with comments.

| plugin | keys (default) |
|---|---|
| `devtools` | `profile=0` (or env `V3X_PROFILE=1`; Ctrl+hotkey toggles), `interval_sec=10`, `hotkey_report=F10`, `sample_hz=1000`, `sample_threads=4`, `unwind_depth=8` |
| `devbridge` | `enabled=1`, `pipe=v3x_devbridge` (env `V3X_DEVBRIDGE_PIPE` wins) |
| `log_filter` | `preset=default` (`aggressive`), `enabled=1`, `min_level=0`, `dedupe_first=3`, `dedupe_template=0`, `summary_interval=60`; rules `drop`/`drop_loc`/`drop_msg`, `keep`/`keep_loc`/`keep_msg` (max 128) |
| `keep_history` | `[keep_history]` `record_all=1`, `rate_days=28` |
| `sim_opt` | `[sim_opt]` `enabled=1`, `diag=0`, `diag_trades=1`, `throttle=0`, `throttle_tasks=HandleAutonomousInvestment:3,...`, `stagger_journal=0`, `stagger_days=4` (env `V3X_NO_SIMOPT=1` = off) |
| `fast_load` | `timeline=1`, `db_nodes=1`, `mesh_threads=0`, `counters=1`, `pump_yield=0`, `worker_spin=0`, `checksum_cache=0` (fails closed in this build), `cache_verify=1` |
| `mem_tuner` | `[mem_tuner] enabled=1`; `[options] enabled=1 eager_commit=0 abandoned_page_decommit=1 decommit_delay=10 segment_decommit_delay=100 decommit_extend_delay=0`; `[collect] after_transition=1 own_hook=0 delay_frames=30`; `[oom] enabled=1 reserve_mb=128 log_mimalloc_errors=1 max_error_logs=32`; `[periodic] enabled=1 force=0 check_sec=30 grow_mb=256 min_interval_min=5 max_ms=100`; `[census] enabled=1 every_min=10`; `[gui_anim_pool] diag=1` |
| `alloc_tracker` | `[alloc_tracker]` `enabled=0`, `big_kb=64`, `sample_kb=256`, `stack_mb=1`, `report_interval_s=30`, `report_top=40`, `wrapper_max_bytes=256`, `skip=` |
| `cpu_sched` | `[cpu_sched]` `enabled=1`, `main=pcores`, `below_normal=ecores`, `normal=all`, `above_normal=pcores`, `task_threads=0` |
| `frame_pacer` | `[frame_pacer]` `enabled=1`, `replace_busy_wait=1`, `idle_fps=20`, `idle_after_s=30`, `paused_idle_after_s=3`, `background_fps=20`, `minimized_fps=5`, `sim_idle_ms=3000`, `tick_render_hz=0`, `log_interval_s=10` |
| `gfx_driver` | `[gfx_driver]` `prevent_driver_threading=1` |
| `asset_stream` | `[asset_stream]` `enabled=0`, `texture_streaming=1`, `cache_mb=512` |
| `lazy_mesh` | `mode=off` (`measure`, `lazy`), `skip_debug=1`, `defer_dirs=gfx/models/portraits/, gfx/models/military/`, `defer_all=0`, `eager_contains=bridge`, `eager_file=lazy_mesh_eager.txt`, `startup_from_report=1`, `prefetch_on_game=1`, `prefetch_all=0`, `prefetch_gap_ms=20`, `merge_report=1`, `prefetch_after_s=0`, `wait_timeout_ms=60000`, `log_loads=1` |
| `map_cache` | `mode=measure` (`off`, `on`), `spike_ms=250`, `load_log_ms=50`, `load_log_mb=8`, `log_budget=2000`, `big_reserve_mb=16`, `summary_sec=300`; `mode=on` only: `budget_mb=256`, `max_texture_mb=32`, `repeat_min=2`, `max_pins=1024`, `unknown_size_kb=1024` |
| `enhance` | `[enhance]` `binary_saves_in_debug=1`, `deflate_level=0`, `speed_table=0`, `speed1=2.0`, `speed2=1.0`, `speed3=0.5`, `speed4=0.25`, `save_timing=1`, `lean_debug=0`, `lean_gui=1`, `lean_tooltips=1`, `lean_theater_cache=1`, `fast_speed5=0` |

`hello`, `ironman_console`, `crash_logger`, `devconsole`, `content_packs` and `tick_profiler`
have no ini.

## Console commands

Type them into the V3X console window (`help` lists everything that is registered). Commands run
on the console input thread, not the game's main thread.

| command | from | what it does |
|---|---|---|
| `help [command]` | core | list commands |
| `plugins` | core | plugin DLLs and their load status |
| `log <text>` | core | write a line to `v3x.log` |
| `version` | core | loader API version, game version, exe base |
| `quit`, `exit` | core | does not quit; says how to close the game |
| `dis <addr> [count]` | devconsole | disassemble (default 20, max 500 instructions) |
| `fn <addr> [max]` | devconsole | the whole function containing addr (bounds from `.pdata`) |
| `xref <addr\|"string"> [max]` | devconsole | rip-relative `lea`/`mov` in the game's `.text` that reference addr or the exact string |
| `scan [-n max] <ida pattern>` | devconsole | all matches in the game's `.text` (default max 20), e.g. `scan 48 8B 05 ?? ?? ?? ?? 48 85 C0` |
| `mem <addr> [len]` | devconsole | hex + ASCII dump (default 128, max 4096 bytes) |
| `mods [filter]` | devconsole | loaded modules: base, size, name |
| `profile on\|off\|report\|reset` | devtools | sampling profiler |
| `packs` | content_packs | content packs and their mount state |
| `modmenu` | mod_menu | plugin list (now / next start); `modmenu toggle <plugin>` flips one for the next start |
| `ticks [reset]` | tick_profiler | tick task profile |
| `simopt [reset \| throttle on\|off]` | sim_opt | diag / trades report, reset, switch throttling |
| `memcollect` | mem_tuner | return free mimalloc memory to Windows (`mi_collect(true)`) |
| `memstat` | mem_tuner | commit census (mimalloc segments vs other) + periodic collect stats |
| `allocs [top N]` | alloc_tracker | top allocation call sites by live and total bytes |
| `cpusched` | cpu_sched | P/E core map and each worker's CPU set |
| `frame_pacer` | frame_pacer | frame pacer status |
| `assetstream` | asset_stream | texture streamer forcing status |
| `lazymesh [status \| dump \| load <type>]` | lazy_mesh | status, write `v3x/lazy_mesh_used.txt`, load one type |
| `mapcache [status \| report \| flush]` | map_cache | status, write `v3x/map_cache_report.txt`, release pins |
| `enhance` | enhance | which enhancements are active |
| `speed [<1-4> <seconds per tick>]` | enhance | show / set the game-speed table (not saved to the ini) |
| `lean [on\|off]` | enhance | debug_mode without its debug-only GUI / tooltips |
| `fast5 [on\|off]` | enhance | post the next speed-5 tick when the previous one ends |

devconsole addresses can be written as hex (`0x7ff6...`, WinDbg backticks allowed), `+rva` in
the game exe (`+0x3e17820`), or `module+offset` exactly as the commands print them
(`victoria3.exe+0x3e17820`). Game memory is only read through `ReadProcessMemory` after a
`VirtualQuery` check, so a bad address prints an error instead of crashing the game.

## Known events

Named events between plugins (API v3, `on_event` / `emit_event`; list in `include/v3x.h`):

| event | emitter, thread | data | subscribers |
|---|---|---|---|
| `load_transition` | fast_load, main, when a load-state transition ends | `const V3X_LoadTransition *` {`from`, `to`, `seconds`} (may be NULL) | alloc_tracker, devtools, lazy_mesh, map_cache, mem_tuner, sim_opt, tick_profiler |
| `load_transition_begin` | fast_load, main, when a transition begins | `const V3X_LoadTransition *` (`seconds` = 0; may be NULL) | devtools |
| `save_begin` | enhance, the saving thread (main), right before `SaveGameState` | `const V3X_SaveBegin *` {`name`, may be NULL} | sim_opt |
| `save_end` | enhance, same thread, right after that save returned | the same `const V3X_SaveBegin *` | - |

## Layout

```
include/v3x.h        public plugin API (C ABI, versioned)
include/v3x_rtti.h   MSVC RTTI lookup helpers for plugins
src/proxy.c          version.dll export forwarding
src/core.c           boot, plugin loader, scanning, hooks, frame tick, console, events
plugins/*            plugins (see above)
test/*.c             tests without the game (see below)
tools/*.py           reverse-engineering helpers: pestrings (strings + RVA),
                     pexref (code xrefs + disassembly), peptr (pointer refs), re_* (per topic)
docs/re/*.md         reverse-engineering notes
third_party/minhook  inline hooking (BSD-2)
third_party/zydis    x86 disassembler, amalgamated (MIT); used by devconsole
```

## Tests (no game needed)

C: is often full on this machine, so every test writes only under `D:/v3x_tmp`, and the
compiler's temp and cache directories go there too. Run this once per shell, from `v3x/`:

```sh
export TEMP=D:/v3x_tmp TMP=D:/v3x_tmp ZIG_GLOBAL_CACHE_DIR=D:/v3x_tmp/zcache ZIG_LOCAL_CACHE_DIR=D:/v3x_tmp/zcache
CC="python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude"
M=third_party/minhook; EXE=../binaries/victoria3.exe; T=D:/v3x_tmp/t; mkdir -p $T
./build.sh                                   # build/version.dll + build/plugins/*.dll
```

Every test prints one line per check and `ALL OK` at the end (exit code 0). Arguments in `[ ]`
are optional; with `$EXE` a test maps the real exe as an image (no game code runs) and checks the
plugin's anchors against it. Each file's header comment has the full details.

Offline tests (one line each; they `#include` the plugin source or load the built DLL against a
fake `V3X_API`):

```sh
$CC -o $T/att.exe test/alloc_tracker_test.c && $T/att.exe [$EXE build/plugins/alloc_tracker.dll]
$CC -o $T/ast.exe test/asset_stream_test.c && $T/ast.exe plugins/asset_stream/asset_stream.ini.example [$EXE [$T]]
mkdir -p $T/cp && $CC -o $T/cpt.exe test/content_packs_test.c && $T/cpt.exe $T/cp
mkdir -p $T/mm && $CC -o $T/mmt.exe test/mod_menu_test.c && $T/mmt.exe $T/mm [../game/gui/frontend/frontend_main.gui [$EXE]]
$CC -o $T/cst.exe test/cpu_sched_test.c && $T/cst.exe [$EXE|- [build/plugins/cpu_sched.dll]]
mkdir -p $T/enh && $CC -o $T/enht.exe test/enhance_test.c && $T/enht.exe $T/enh [$EXE]
$CC -I$M/include -o $T/flt.exe test/fast_load_test.c $M/src/buffer.c $M/src/hook.c $M/src/trampoline.c $M/src/hde/hde64.c && $T/flt.exe [$EXE]
$CC -o $T/fpt.exe test/frame_pacer_test.c && $T/fpt.exe plugins/frame_pacer/frame_pacer.ini.example [$EXE]
$CC -o $T/gdt.exe test/gfx_driver_test.c && $T/gdt.exe plugins/gfx_driver/gfx_driver.ini.example [$EXE]
$CC -o $T/kht.exe test/keep_history_test.c && $T/kht.exe $EXE build/plugins/keep_history.dll
$CC -o $T/lmt.exe test/lazy_mesh_test.c && $T/lmt.exe [$EXE]                # writes D:/v3x_tmp/lazymesh/t/
$CC -o $T/lft.exe test/log_filter_test.c && $T/lft.exe plugins/log_filter/log_filter.ini.example
$CC -DMC_TEST -o $T/mct.exe test/map_cache_test.c && $T/mct.exe [$EXE]      # writes D:/v3x_tmp/mapmode/t/
$CC -o $T/mtt.exe test/mem_tuner_test.c && $T/mtt.exe [$EXE [build/plugins/mem_tuner.dll [api_version]]]
$CC -o $T/rtti.exe test/rtti_test.c && $T/rtti.exe [$EXE <expected CTickTask_ count>]
$CC -o $T/sot.exe test/sim_opt_test.c && $T/sot.exe
$CC -o $T/cplt.exe test/console_plus_test.c && $T/cplt.exe [$EXE]
$CC -o $T/tpar.exe test/tick_par_test.c && $T/tpar.exe
$CC -o $T/tpt.exe test/tick_profiler_test.c && $T/tpt.exe
```

End-to-end tests link `build/version.dll` like `victoria3.exe` does, so V3X boots and loads
`<dir>/v3x/plugins/*.dll` before `main`. `e2e <test> <plugins...>` builds one into `$T/<test>/`
with only the plugins it needs:

```sh
e2e() { D=$T/$1; mkdir -p $D/v3x/plugins && cp build/version.dll $D/ && $CC -o $D/$1.exe test/$1.c -Lbuild -l:version.dll -luser32 && shift && for p in "$@"; do cp build/plugins/$p.dll $D/v3x/plugins/; done; }
e2e harness ironman_console && (cd $D && V3X_NO_CONSOLE=1 ./harness.exe)                 # loader + ironman_console
e2e devconsole_test devconsole && (cd $D && V3X_NO_CONSOLE=1 ./devconsole_test.exe)      # commands, fault-proof reads, exec_capture
e2e devbridge_test devconsole devbridge && (cd $D && V3X_NO_CONSOLE=1 V3X_DEVBRIDGE_PIPE=v3x_devbridge_test ./devbridge_test.exe)  # private pipe required
e2e devtools_test devtools && echo interval_sec=2 > $D/v3x/devtools.ini && (cd $D && V3X_NO_CONSOLE=1 V3X_PROFILE=1 ./devtools_test.exe)
e2e crash_test crash_logger && (cd $D && V3X_NO_CONSOLE=1 ./crash_test.exe)               # runs copies of itself in child_<mode>/
```

Process-exit test: every plugin plus the test-only `test/shutdown_probe.c`; the runner starts the
child 20 times, each must exit within 10 s with code 42 after `ExitProcess` while other threads
hammer the log lock, heap and CRT (`con` = own hidden console window with the input thread
reading, `nocon` = `V3X_NO_CONSOLE=1`):

```sh
D=$T/shutdown && mkdir -p $D/v3x/plugins && cp build/version.dll $D/ && cp build/plugins/*.dll $D/v3x/plugins/
$CC -shared -o $D/v3x/plugins/shutdown_probe.dll test/shutdown_probe.c
$CC -o $D/shutdown_child.exe test/shutdown_test.c -Lbuild -l:version.dll -luser32
$CC -DSHUTDOWN_RUNNER -o $D/shutdown_test.exe test/shutdown_test.c
$D/shutdown_test.exe $D/shutdown_child.exe 20 con && $D/shutdown_test.exe $D/shutdown_child.exe 20 nocon
```

`test/test_mcp.py` tests the MCP server against `devbridge_test.exe --serve N`.

## Caveats

* Single player only. Native code bypasses the checksum, so don't use it in multiplayer.
* Ironman achievements may still be affected by what your plugins do. That's your call.
* Game updates can change function shapes, even when the string anchors survive. Plugins should
  check what they find and fail gracefully (return non-zero from `v3x_plugin_load`).
