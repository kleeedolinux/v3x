# Plugin catalog

The **33 plugins** in `plugins/` (one folder each). Every plugin finds its engine targets by string /
RTTI anchors plus byte checks (reverse-engineered on 1.15.1) and **fails closed**: if an anchor does
not match, that plugin or feature changes nothing and says why in `binaries/v3x/v3x.log`.

Config: `binaries/v3x/<plugin>.ini` (every key optional; `plugins/<p>/<p>.ini.example` holds the
defaults with comments, [configuration.md](configuration.md) lists the keys). Disable a plugin without
deleting it: `[plugins]` `<plugin> = 0` in `binaries/v3x/v3x.ini`, or the main-menu **V3X** button
(`mod_menu`).

Trust (loader 0.6.0): the plugins `V3X-Setup.exe` installs are listed with their SHA-256 in the signed
`binaries/v3x/plugins.json` and load as `official`. A plugin DLL from anywhere else (one you built, a
dev-only plugin from this repo copied by hand) is **not loaded** until you allow it with the console
command `trust <plugin>` (or a developer sets `V3X_TRUST=dev`); see
[configuration.md](configuration.md#plugin-trust-and-the-allowlist) and
[ADR 0002](adr/0002-trust-model.md). Writing your own: [safe-coding.md](safe-coding.md) ("Writing safe
plugins") first, then [api-reference.md](api-reference.md).

Sources for this page: each plugin's header comment (`plugins/<p>/<p>.c`, line 1 on), its
`*.ini.example`, the player installer profile [`installer/profile.txt`](../installer/profile.txt),
in-game measurements on 1.15.1, [CHANGELOG.md](../CHANGELOG.md), [safety.md](safety.md) and
`docs/re/*.md`. Line numbers in plugin sources may shift by a few lines as the plugins change.

## Columns

* **class**
  * **production**: shipped by the player installer and loaded by default (`installer/profile.txt` = `1`).
  * **opt-in**: shipped but off (`profile.txt` = `0`); the player turns it on knowingly.
  * **measure-only**: default mode only measures and reports (no behaviour change); `on` is refused,
    reserved, or not yet proven. Not shipped to players.
  * **dev tool**: developer / reverse-engineering / diagnostics. Not shipped to players.
  * **SDK service**: a plugin other plugins call (no engine hook of its own).
* **default**: the plugin's own default (built-in default = `*.ini.example`), i.e. what a developer
  install (`./build.sh install`) runs with.
* **installer**: what `V3X-Setup.exe` does with it: `on`, `off` (shipped, `<p>=0`) or `-` (not
  shipped).
* **in game**: how far it has been run in the real game (1.15.1). "4h" = the earlier 4-hour session;
  "S" = a later 1 h 44 m test session, in which 28/28 installed plugins loaded and nothing crashed;
  `tick_par` was not installed.
* **test**: the offline / end-to-end test; how to run it: [testing.md](testing.md).

## Summary

| plugin | class | default | installer | in game | test |
|---|---|---|---|---|---|
| `crash_logger` | production | on | on | 4h, S | `test/crash_test.c` |
| `content_packs` | production | on (inert without packs) | on | 4h, S (no packs tested) | `test/content_packs_test.c` |
| `mod_menu` | production (unverified, see notes) | on | on | S: loaded; button not exercised (unverified) | `test/mod_menu_test.c` |
| `keep_history` | production | on, `record_all=1` | on | 4h, S | `test/keep_history_test.c` |
| `ironman_console` | production | on | on | 4h, S | `test/harness.c` |
| `log_filter` | production | `enabled=1` | on | 4h, S | `test/log_filter_test.c` |
| `frame_pacer` | production | `enabled=1` | on | 4h, S | `test/frame_pacer_test.c` |
| `cpu_sched` | production | `enabled=1` | on | 4h, S | `test/cpu_sched_test.c` |
| `gfx_driver` | production | `prevent_driver_threading=1` | on | 4h, S | `test/gfx_driver_test.c` |
| `mem_tuner` | production | `enabled=1` (all parts) | on | 4h, S | `test/mem_tuner_test.c` |
| `fast_load` | production | `timeline=1`, speedups 0 | on | 4h, S | `test/fast_load_test.c` |
| `console_plus` | opt-in | `enabled=1` | off | S (after the crash6 fix) | `test/console_plus_test.c` |
| `loc_fallback` | opt-in | `enabled=1` | off | S: loaded | `test/loc_fallback_test.c` |
| `enhance` | opt-in | `binary_saves_in_debug=1`, `save_timing=1` | off | S | `test/enhance_test.c` |
| `asset_stream` | opt-in | `enabled=0` | off | 4h, S (streaming 512 MB) | `test/asset_stream_test.c` |
| `lazy_mesh` | opt-in | `mode=off` | off | 4h incl. `lazy`, S `lazy` | `test/lazy_mesh_test.c` |
| `sim_opt` | opt-in | `diag_trades=1`, `diag_journal=1` (read-only), `throttle=0` | off | S (diag only) | `test/sim_opt_test.c` |
| `fast_lookup` | measure-only (criterion for `on` met) | `mode=measure` | - | S: measure, 0 mismatches | `test/fast_lookup_test.c` |
| `mod_loader` | measure-only (runs `on` in the live install) | `mode=measure` | - | S: `mode=on`, 0 mismatches | `test/mod_loader_test.c` |
| `map_cache` | measure-only (`on` exists, no effect yet) | `mode=measure` | - | S: `mode=on`, 0 pin hits | `test/map_cache_test.c` |
| `lazy_gfx` | measure-only | `mode=measure`, `png_cache=off` | - | S: measure | `test/lazy_gfx_test.c` |
| `par_load` | measure-only (`on` reserved) | `mode=measure` | - | S: measure, 13/14 anchors | `test/par_load_test.c` |
| `tick_par` | measure-only (`on` refused) | `mode=measure` | - | not run | `test/tick_par_test.c` |
| `ai_par` | measure-only (`on` refused; opt-in lever) | `mode=measure`, `fine_chunks=0` | - | not run (not in S, inferred) | `test/ai_par_test.c` |
| `alloc_tracker` | dev tool | `enabled=0` | - | 4h, S (was on: caused stalls) | `test/alloc_tracker_test.c` |
| `devtools` | dev tool | monitor on, `profile=0` | - | 4h, S (profiler off) | `test/devtools_test.c` |
| `devconsole` | dev tool | on | - | smoke test, S | `test/devconsole_test.c` |
| `devbridge` | dev tool | `enabled=1` | - | 4h (MCP answered live), S | `test/devbridge_test.c`, `test/test_mcp.py` |
| `tick_profiler` | dev tool | on (`V3X_NO_TICKPROF=1` = off) | - | 4h, S | `test/tick_profiler_test.c` |
| `hello` | dev tool (SDK example) | on | - | 4h, S | none of its own (loaded by `test/api_v4_test.c`) |
| `script_ext` | SDK service (layer b of the script SDK) | on | on | not run (docs/scripting.md:11) | `test/script_ext_test.c` |
| `doctor` | production (diagnostic, read-only) | on | on | not recorded | `test/doctor_test.c`, `test/fuzz/fuzz_doctor.c` |
| `updater` | opt-in | `[update] check=1 auto=1` in `v3x.ini` | off | not recorded (no signed release exists yet) | `test/updater_test.c` |

## Recommended settings

From the 1 h 44 m test session "S" (see Columns) on 1.15.1.

| plugin | recommendation | evidence |
|---|---|---|
| `alloc_tracker` | **`enabled = 0`** (its own default). Turn on only for a dedicated memory run, or set `report_interval_s=0` | its periodic report runs on the main thread every 30 s (`report_interval_s=30`, plugins/alloc_tracker/alloc_tracker.ini.example:14) and caused ~190-200 frame stalls of ~0.55 s, ~106-111 s per 100 min |
| `fast_lookup` | **`mode = on`, `verify_every = 64`** | measure criterion met: 1,722,908 calls, 0 mismatches over 1 h 44 m with the detour live. Gain is small (<= ~0.25 s per save, estimate) |
| `mod_loader` | keep `mode=on` (live install) | 0 mismatches, 0 false hits, 98,786 verified skips |
| `map_cache` | `on` does nothing yet | 0 pin hits; path table full at 6,144 of 8,192; needs a bigger `MC_MAX_PATHS` (plugin work) |
| `lazy_gfx` | stay `measure`, `png_cache=off` | prefetch manifest had 0 files; png_cache saw 7 contradictions. Needs one run with `manifest_game=1` first |
| `par_load` | stay `measure` | `on` changes nothing (no step proven safe); realdir anchor failed |

## Plugins

Each entry: what it does, then default, class, crash history, test, installer.

### Production (shipped on)

**`crash_logger`**: writes `binaries/v3x/crash.log` (previous run `crash.prev.log`): MSVC C++ throws
with type names and `what()`, fatal SEH exceptions with registers and stack, abort, big memory changes;
no heap use on the exception path (plugins/crash_logger/crash_logger.c:1-15). Manifest `"*"`, i.e. it
also loads on unknown builds (crash_logger.c:28). Crashes: none known. Installer: on (read-only, the
evidence a player sends).

**`content_packs`**: mounts every folder in `binaries/v3x/content_packs/` at `/` above `game/` in the
PhysFS search path (file overrides without a mod; `order.txt`; `_name` = disabled); adds search-path
entries only (plugins/content_packs/content_packs.c:1-20). No ini. Needed by `mod_menu`,
`loc_fallback` and `script_ext`. Crashes: none known. Installer: on.

**`mod_menu`**: a "V3X" button on the main menu with a checkbox per plugin; writes the `[plugins]`
section of `v3x.ini` for the next start (temp file + rename); generates its GUI as a content pack and
fails closed to the vanilla menu (plugins/mod_menu/mod_menu.c:1-23). Console `modmenu`. Crashes: none
known. In game: loaded in S; that the button was clicked in game is not recorded (unverified;
installer/profile.txt:11 says "not run in game yet - check"). Installer: on.

**`keep_history`**: country graphs (GDP, population, SoL, ...) are no longer wiped when you stop
playing a country: NOPs 11 `Trend::Clear` calls (a byte patch, see the Unsafe box in
[Hooks that cannot be shared](#hooks-that-cannot-be-shared)); `record_all=1` samples AI countries like players
(plugins/keep_history/keep_history.c:1-35). Presentation-only data. Crashes: none known. Installer: on.

**`ironman_console`**: the developer console works in ironman; only the ironman reason of the
"console blocked" predicate is cleared, multiplayer stays blocked
(plugins/ironman_console/ironman_console.c:1-8). Crashes: none known. Installer: on (set `0` if
achievements matter, installer/profile.txt:13).

**`log_filter`**: drops / dedupes noisy engine log messages in `spdlog::logger::sink_it_` (RTTI anchor);
stats in `v3x/log_filter_stats.txt` (plugins/log_filter/log_filter.c:1-14). Crashes: none known.
Installer: on.

**`frame_pacer`**: replaces the frame-cap busy-wait with a waitable-timer sleep; lower caps when idle,
paused, in background or minimised; `tick_render_hz` sets `GameStateTick.MinLockTime`
(plugins/frame_pacer/frame_pacer.c:1-17). Console `frame_pacer`. Crashes: none known. Installer: on.

**`cpu_sched`**: on hybrid (P/E) CPUs, main thread and ABOVE_NORMAL workers on P-cores, BELOW_NORMAL
workers on E-cores (CPU sets); nothing on other CPUs (plugins/cpu_sched/cpu_sched.c:1-14).
Console `cpusched`. Crashes: none known. Installer: on.

**`gfx_driver`**: adds `D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS` via the exe's
`D3D11CreateDevice` IAT slot; retries without the flag on failure (plugins/gfx_driver/gfx_driver.c:1-13).
FPS gain not measured (A/B pending, same header). Crashes: none known. Installer: on.

**`mem_tuner`**: tunes the engine's mimalloc options before CRT init (lower commit), `mi_collect` after
loads and on commit growth, one out-of-memory rescue, mimalloc error log, commit census, per-thread
collect, heap compact (plugins/mem_tuner/mem_tuner.c:1-34). Consoles `memcollect`, `memstat`. In S the
periodic collect returned 1,548 MiB in 16 runs of <= 16 ms each. Crashes: none known; it
exists because of the New Game out-of-memory crash (docs/re/memory.md). Installer: on.

**`fast_load`**: load-time measurement (`v3x/load_timeline.txt`), emits `load_transition(_begin)` that
other plugins subscribe to; optional `mesh_threads`, `pump_yield`, `worker_spin` stay 0
(plugins/fast_load/fast_load.c:1-25, plugins/fast_load/fast_load.ini.example:2-8). `checksum_cache` is
accepted but fails closed. Crashes: none known. Installer: on.

**`doctor`**: health check of the V3X setup: console `doctor` prints OK / WARN / PROBLEM per check with
one "what to do" line, `doctor fix <id>` applies the safe fix of a check (ini edits only, atomic,
comments kept, first original kept as `<file>.doctor-bak`), `doctor report` writes
`binaries/v3x/doctor_report.txt` for bug reports (user name masked). Runs once by itself on the
first `load_transition` Empty->Game (or 60 s after the first frame without `fast_load`), on a
background thread (plugins/doctor/doctor.c:1-14). Manifest `"*"`, so it also explains the
unknown-build gate. Guide: [doctor.md](doctor.md). Crashes: none known. Installer: on.

### Opt-in (shipped off)

**`console_plus`**: Tab completion of console command *arguments* (country tags, states, laws, ... from
the game databases) in the in-game console, and Tab completion in the V3X console window
(plugins/console_plus/console_plus.c:1-60). Console `complete`. **Crash history: crash6**, an access
violation in `mi_free` (passing the matcher a CString not owned by the engine's mimalloc), fixed
(docs/re/console.md:66, plugins/console_plus/console_plus.c:977). Ran in S without a crash.
Installer: off (installer/profile.txt:23).

**`loc_fallback`**: at load, writes English text for localization keys of enabled mods that no file
of the player's language defines, as a content pack below mods; data only, no engine hook
(plugins/loc_fallback/loc_fallback.c:1-17). Console `locfallback`. Crashes: none known. Loaded in S
; output not checked in game (unverified). Installer: off.

**`enhance`**: `binary_saves_in_debug` (normal zipped saves in debug mode: 156 MB -> 26 MB),
`deflate_level`, `speed_table`, `save_timing`, `lean_debug`; always emits `save_begin` / `save_end`
(plugins/enhance/enhance.c:1-70, plugins/enhance/enhance.ini.example:32-44). **Crash history:**
`fast_speed5` crashed (next tick posted from the network/session thread) and was withdrawn; the key is
read and ignored (docs/re/enhance.md:145-182, docs/safety.md:57-59, CHANGELOG.md "Removed / withdrawn"). Installer: off.

**`asset_stream`**: forces the engine's own texture streamer on (every quality preset turns it off),
budget `cache_mb` (plugins/asset_stream/asset_stream.c:1-15). Default `enabled = 0`
(plugins/asset_stream/asset_stream.ini.example:15). Crashes: none known. Installer: off.

**`lazy_mesh`**: `measure` records which mesh types are used; `lazy` defers chosen mesh types and loads
them on first use (37.5 s -> 6.7 s pass in the 4h session, README) (plugins/lazy_mesh/lazy_mesh.c:1-22).
Default `mode = off` (plugins/lazy_mesh/lazy_mesh.ini.example:11). In S: `lazy`, 22 on-demand loads on
the main thread, 1.47 s total. Crashes: none known. Installer: off.

**`sim_opt`**: tick diagnostics via a slot-6 wrapper on every `CTickTask_` vtable; read-only
`diag_trades` / `diag_journal` probes on by default; opt-in `throttle` (skip 1 of N runs) changes the
simulation, single player only (plugins/sim_opt/sim_opt.c:1-37, plugins/sim_opt/sim_opt.ini.example:5-23).
**Crash history:** (1) a 4-argument detour on a 5-argument function crashed later at an unrelated
address (docs/safety.md:12-16) - the origin of the "forward all arguments" rule; (2) `stagger_journal`
ran script outside a tick task's RNG scope and crashed in saves; withdrawn and refused at load
(docs/safety.md:53-56, docs/re/simulation.md:84, CHANGELOG.md "Removed / withdrawn"). Installer: off.

**`updater`**: looks for V3X releases on GitHub that support the *running* game build (at most one
automatic check per 6 h, on a background thread) and installs a compatible **signed** release after
the game exits; unsigned releases are only announced (`update now` installs them with a warning).
Console `update [status|check|now|cancel]`; `[update]` keys in `v3x.ini`
([configuration.md](configuration.md#updater-update-in-v3xini)); env `V3X_NO_UPDATE=1` disables it
(plugins/updater/updater.c:1-29). Emits `update_available`. Guide: [updating.md](updating.md).
Crashes: none known. Installer: off until the first signed release exists (installer/profile.txt:49).

### Measure-only (not shipped)

**`fast_lookup`**: lock-free side caches for immutable engine lookups: `token_cache` (TokenGetString
without the registry spin lock) and `indent` (text writer WriteIndent); bit-identical answers
(plugins/fast_lookup/fast_lookup.c:1-13). Default `mode = measure`
(plugins/fast_lookup/fast_lookup.ini.example:9). **Crash history: crash5**: a plain C detour clobbered
volatile registers the engine's callers keep live, NULL format string, AV at `exe+0x43c1bb6` on every
job thread; fixed with a register-preserving thunk (plugins/fast_lookup/fast_lookup.c:84-87). Since the
fix it ran 1 h 44 m in S with 0 mismatches. **Criterion for `on` met**.
Installer: not shipped (installer/profile.txt:42).

**`mod_loader`**: measures the per-file VFS cost of mods (every mod / DLC folder is a PhysFS root
probed per file), builds a per-root file index and, in `mode=on`, answers predicted misses without
syscalls (with `verify_every` re-checks); report `v3x/mod_report.txt` (plugins/mod_loader/mod_loader.c:1-30).
Default `mode = measure` (plugins/mod_loader/mod_loader.ini.example:8). **The live install runs
`mode=on`**: 0 mismatches, 0 false hits, 98,786 verified skips, misses 1.43 s for the whole
session. Crashes: none known. Installer: not shipped (installer/profile.txt:39).

**`map_cache`**: measures repeated texture disk reads behind map-mode / GUI lag, logs main-thread
spikes with the loads inside them; `mode=on` keeps re-read textures alive (LRU)
(plugins/map_cache/map_cache.c:1-22). Default `mode=measure` (plugins/map_cache/map_cache.ini.example:7).
In S `mode=on` had 4 pins and 0 pin hits because its path table filled at 6,144. Its 46 s spike
in S is the Game->Empty exit frame, not gameplay. Crashes: none known. Installer: not shipped.

**`lazy_gfx`**: per-texture, per-stage timing of texture loading (opts, getRealDir, decode, pixel
buffer, object, stamp) with `hook_safe` probes filtered by call site; `mode=on` prefetches the previous
run's startup textures into the OS file cache; optional decoded-image cache `png_cache`
(plugins/lazy_gfx/lazy_gfx.c:1-29, plugins/lazy_gfx/lazy_gfx.ini.example:11-38). In S: measure; prefetch
manifest empty, png_cache turned itself off after contradictions. Crashes: none known.
Installer: not shipped.

**`par_load`**: hook_safe timing of the serial game-init steps (GUI load, assets, databases, VFSOpen,
lexer, blend shapes); `mode=on` is reserved and runs as measure because no step is proven safe
(plugins/par_load/par_load.c:1-29). Default `mode = measure` (plugins/par_load/par_load.ini.example:10).
In S 13/14 anchors resolved (realdir failed). Crashes: none known. Installer: not shipped.

**`tick_par`**: fork/join analysis of the simulation tick (due list, per-task time, simulated parallel
makespans); `mode=on` is refused (no pair of tasks is provably safe to co-run)
(plugins/tick_par/tick_par.c:1-33). Default `mode=measure` (plugins/tick_par/tick_par.ini.example:12).
Not run in game (not installed in S). Crashes: none known. Installer: not shipped.

**`ai_par`**: where the AI / economy tick tasks (HAI, UpdateAI, UpdateStateTrades, UpdatePopGrowth)
spend serial vs parallel time, via read-only `hook_safe` probes on RunRangeExecutionParallel; `mode=on`
refused; separate opt-in lever `fine_chunks=1` (grain 1 for listed tasks, restored after each call)
(plugins/ai_par/ai_par.c:1-48, plugins/ai_par/ai_par.ini.example:10-20). RE: docs/re/ai_par.md. New;
not run in game (inferred: not among the plugins in S). Crashes: none known. Installer: not shipped
(installer/profile.txt:38).

### Dev tools (not shipped)

**`alloc_tracker`**: hooks mimalloc `mi_new` / `mi_new_aligned` / `mi_malloc` / `mi_free`: live and
total bytes per call site, mimalloc segment walk, VM reserve owners; `v3x/alloc_report.txt` every
`report_interval_s` and on `load_transition`; console `allocs` (plugins/alloc_tracker/alloc_tracker.c:1-29).
Default `enabled=0` (plugins/alloc_tracker/alloc_tracker.ini.example:6). **Keep it off in normal play**:
see "Recommended settings". Crashes: none known. Installer: not shipped.

**`devtools`**: CPU per thread, optional ~1 kHz sampling profiler, memory and GPU-memory reports in
`v3x/devtools.log`, profile CSVs per load phase; console `profile` (plugins/devtools/devtools.c:1-30).
**Crash history:** the game hung at exit with `profile=1` (a thread left suspended by ExitProcess); fixed
via the `process_exit` event (plugins/devtools/devtools.c:31-34). Installer: not shipped.

**`devconsole`**: RE commands `dis`, `fn`, `xref`, `scan`, `mem`, `mods` with Zydis disassembly; reads
memory only through fault-proof copies (plugins/devconsole/devconsole.c:1-17). Crashes: none known.
Installer: not shipped.

**`devbridge`**: serves V3X console commands over the named pipe `\\.\pipe\v3x_devbridge` for
`mcp/v3x_mcp.py`; adds no commands and no hooks (plugins/devbridge/devbridge.c:1-28). The pipe is
owner-only and local; every connection must first send the per-session token from
`binaries/v3x/devbridge.token` (`auth <token>`), and only commands in the `allow =` list of
`devbridge.ini` run (default: read-only inspection; `allow = *` is an explicit opt-in), see
[configuration.md](configuration.md#devbridge-token-and-allow-list). Crashes: none known.
Installer: not shipped.

**`tick_profiler`**: wall time per tick task (hook on `RunTask`); top-25 every 60 s, on Game->Empty and
console `ticks`; `v3x/tick_profile.csv` (plugins/tick_profiler/tick_profiler.c:1-23). Crashes: none known.
Installer: not shipped.

**`hello`**: minimal SDK example: logs on load and every 3000 frames; manifest `"*"`
(plugins/hello/hello.c:1-24). Installer: not shipped.

### SDK service

**`script_ext`**: layer (b) of the script SDK: exports `v3x_script_ext()` (sdk/v3x_script.h) so other
plugins can write game script, localization and GUI files into `binaries/v3x/content_packs/<pack>/`
from their `v3x_plugin_load`; no engine code hooked, patched or called; console `scriptext`
(plugins/script_ext/script_ext.c:1-7). Manifest `"*"` (script_ext.c:13). Guide:
[scripting.md](scripting.md) (layers a and c not implemented, docs/scripting.md:8-13). Offline test:
45 checks, 0 failed, ALL OK (with the real exe, part E included).
Not run in game. Installer: on (installer/profile.txt:19; it hooks nothing and is needed by
plugins that ship script packs).

## Events between plugins

| event | emitter | subscribers |
|---|---|---|
| `load_transition` | fast_load | ai_par, alloc_tracker, console_plus, devtools, doctor, lazy_gfx, lazy_mesh, log_filter, map_cache, mem_tuner, mod_loader, par_load, sim_opt, tick_par, tick_profiler |
| `load_transition_begin` | fast_load | devtools, lazy_gfx |
| `save_begin` / `save_end` | enhance | fast_lookup (`save_end`) |
| `process_exit` | core (API v3+) | ai_par, devtools, fast_lookup, lazy_gfx, log_filter, par_load, tick_profiler, updater |
| `config_reload` | core (API v5, include/v3x.h:91) | none of the bundled plugins yet |
| `plugin_disabled` | core (API v6, reserved; data `const V3X_PluginDisabled *`) | script_ext (stops calling the keywords of a disabled plugin, plugins/script_ext/script_ext.c:293) |
| `plugins_loaded` | core (API v6, reserved; main thread, once) | none of the bundled plugins yet |
| `update_available` | updater (data: `const V3X_UpdateInfo *`, plugins/updater/updater.h) | none of the bundled plugins yet |

(From `grep on_event( plugins/*/*.c`.)

## Hooks that cannot be shared

**The safe way is `hook_safe`**: any number of plugins (up to 8 subscriptions per target) share one
target, each with its own pre/post (docs/safety.md, `test/hook_chain_test.c`). A raw `api->hook` is
**exclusive**: once a plugin holds one, every other `hook` or `hook_safe` on that function returns
`V3X_EHOOKBUSY` (100) and v3x.log names the holder. Since loader 0.6.0 no plugin can remove another
plugin's hook (`unhook` removes only the caller's own).

> **Unsafe — when you really need it:** the bundled plugins below hold raw hooks or patch bytes.
> * Why each one does: alloc_tracker's mimalloc hooks and log_filter's `sink_it_` are called millions
>   of times (the thunk's overhead matters) or are *replaced*, not observed; tick_profiler, enhance,
>   frame_pacer, cpu_sched and map_cache replace or time functions with exactly proven prototypes.
>   Byte patches (`api->patch`): keep_history NOPs `Trend::Clear` calls, sim_opt writes a vtable slot,
>   tick_par redirects a call site, gfx_driver redirects an IAT slot, enhance rewrites its speed table,
>   two branches and RIP displacements.
> * What can go wrong: a raw hook blocks every other plugin on that function; a detour with the wrong
>   prototype corrupts arguments; a patch written while a thread runs those bytes, or at a wrong
>   address, crashes later and elsewhere. Faults there are not isolated.
> * How it is contained: every target comes from an anchor that checks its bytes (unique match,
>   prologue or instruction shape; gfx_driver also checks that the IAT slot still holds the real
>   function), so a changed game build installs nothing; every result is checked and the feature fails
>   closed; rollback restores the patches if the plugin's load fails. New code should verify the exact
>   old bytes right before `patch` (`v3x_pattern_match_at`, [api-reference.md](api-reference.md)).
>   If your plugin needs one of the functions below, do not add a second raw hook: coordinate (an
>   event, or a feature in the owning plugin).

Known raw-hook owners: `RunTask` (tick_profiler; tick_par
chains through a call-site stub, sim_opt patches vtable slot 6 instead), `SaveGameState` (enhance),
`spdlog::logger::sink_it_` (log_filter), mimalloc `mi_new`/`mi_malloc`/`mi_free` (alloc_tracker), the
frame-cap function and `RenderFrameIfNeeded` guard (frame_pacer), the scheduler init and worker entry
(cpu_sched), `CreateTexture` / `GetOrLoadTexture` / `Reserve` (map_cache; lazy_gfx stays off them),
`VFSOpen` (par_load). gfx_driver redirects the exe's IAT slot of `D3D11CreateDevice` instead of an
inline hook. If your plugin needs one of those, coordinate (an event, or a feature in the owning plugin).

## Open points

* None for the installer profile: `installer/profile.txt` lists all 33 plugins
  (`script_ext` on, `ai_par` not shipped) and `python installer/make_dist.py --no-build` passes its
  profile check (`installer/make_dist.py:88-91`).
