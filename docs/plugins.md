# Plugin catalog

The 25 plugins in `plugins/`. Each finds its targets by string / RTTI anchors plus byte checks
(reverse-engineered on 1.15.1) and **fails closed**: if an anchor does not match, that plugin or
feature changes nothing and says why in `binaries/v3x/v3x.log`.

Config: `binaries/v3x/<plugin>.ini` (every key optional; `plugins/<p>/<p>.ini.example` holds the
defaults with comments, [configuration.md](configuration.md) lists every key). Disable a plugin without
deleting it: `[plugins]` `<plugin> = 0` in `binaries/v3x/v3x.ini`.

Status columns: **offline** = builds and its offline test passes; **in game** = how far it has been
run in the real game (1.15.1). "loads (smoke test)" means it was loaded in a short run without
exercising its features; "not run" means it has never been loaded in the game.

## Diagnostics and developer tools (read-only)

| plugin | offline | in game | what it does | default | console |
|---|---|---|---|---|---|
| `hello` | yes | played (4h) | minimal example: logs on load and every 3000 frames | on | - |
| `crash_logger` | yes | played (4h) | `v3x/crash.log` (previous run: `crash.prev.log`): C++ throws with type names, fatal SEH exceptions with registers and stack, abort, big memory changes | on | - |
| `devtools` | yes | played (4h) | CPU per thread, optional ~1 kHz sampling profiler, memory and GPU-memory reports in `v3x/devtools.log`, profile CSVs (per load phase) | profiler off | `profile` |
| `devconsole` | yes | loads (smoke test) | RE commands with Zydis disassembly; reads memory only through fault-proof copies | on | `dis` `fn` `xref` `scan` `mem` `mods` |
| `devbridge` | yes | played (4h), MCP answered live | serves the console over the named pipe `\\.\pipe\v3x_devbridge` for `mcp/v3x_mcp.py` | on | - |
| `tick_profiler` | yes | played (4h) | wall time per tick task (hook on `RunTask`); top-25 every 60 s, `v3x/tick_profile.csv` | on (`V3X_NO_TICKPROF=1` = off) | `ticks` |
| `alloc_tracker` | yes | played (4h) | hooks mimalloc `mi_new`/`mi_malloc`/`mi_free`: live and total bytes per call site, `v3x/alloc_report.txt` | off | `allocs` |
| `tick_par` | yes | not run | fork/join analysis of the tick: due list, per-task time, what a parallel executor could save; `mode=on` is refused (no task pair is provably safe) | `mode=measure` | `tickpar` |
| `map_cache` | yes | loads (smoke test) | measures repeated texture disk reads behind map-mode / GUI lag; `mode=on` keeps re-read textures alive (LRU) | `measure` | `mapcache` |
| `mod_loader` | yes | not run | measures the per-file VFS cost of mods (per-root probe counts, index predictions checked against the engine), `v3x/mod_report.txt`; `mode=on` answers predicted misses without syscalls | `mode=measure` | `modload` |

## Quality of life

| plugin | offline | in game | what it does | default | console |
|---|---|---|---|---|---|
| `ironman_console` | yes | played (4h) | the developer console works in ironman: clears only the ironman reason of the "console blocked" predicate (multiplayer stays blocked) | on | - |
| `console_plus` | yes | not run | Tab completion of command *arguments* in the in-game console (country tags, states, laws, ... from the game databases) and Tab completion in the V3X console window | on | `complete` |
| `content_packs` | yes | played (4h), no packs tested | mounts every folder in `v3x/content_packs/` on top of the virtual file system (file overrides without a mod; `order.txt`; `_name` = disabled) | on | `packs` |
| `keep_history` | yes | played (4h) | country graphs are no longer wiped when you stop playing a country (NOPs 11 `Trend::Clear` calls); `record_all` samples AI countries like players | on | - |
| `mod_menu` | yes | not run | a "V3X" button on the main menu (needs content_packs) to enable/disable plugins for the next start (writes the `[plugins]` section of `v3x.ini`); fails closed to the vanilla menu | on | `modmenu` |

## Performance

| plugin | offline | in game | what it does | default | console |
|---|---|---|---|---|---|
| `log_filter` | yes | played (4h) | drops / dedupes noisy engine log messages in `spdlog::logger::sink_it_`; stats in `v3x/log_filter_stats.txt` | on, 5 noise rules | - |
| `frame_pacer` | yes | played (4h) | replaces the frame-cap busy-wait with a timer sleep; lower caps when idle, paused, in background or minimised | on | `frame_pacer` |
| `cpu_sched` | yes | played (4h) | on hybrid (P/E) CPUs: main thread and ABOVE_NORMAL workers on P-cores, BELOW_NORMAL workers on E-cores (CPU sets); nothing on other CPUs | on | `cpusched` |
| `gfx_driver` | yes | played (4h) | creates the D3D11 device with `PREVENT_INTERNAL_THREADING_OPTIMIZATIONS` (fewer driver threads on iGPUs); retries without it on failure | on | - |
| `mem_tuner` | yes | played (4h); newer features loads only | tunes mimalloc options before CRT init (lower commit), `mi_collect` after loads and on commit growth, one OOM rescue, commit census | on | `memcollect` `memstat` |
| `fast_load` | yes | played (4h); `pump_yield`/`worker_spin` loads only | load-time measurement, `v3x/load_timeline.txt`, emits `load_transition(_begin)`; optional `mesh_threads`, `pump_yield`, `worker_spin` | timeline on | - |
| `asset_stream` | yes | played (4h) | forces the engine's own texture streamer on (every quality preset turns it off), budget `cache_mb` | off | `assetstream` |
| `lazy_mesh` | yes | played (4h) incl. `lazy`; prefetch / merge loads only | `measure` records which mesh types are used; `lazy` loads deferred mesh types on first use | off | `lazymesh` |

## Simulation and engine changes (single player only)

| plugin | offline | in game | what it does | default | console |
|---|---|---|---|---|---|
| `sim_opt` | yes | loads (smoke test) | tick diagnostics via a slot-6 wrapper on every `CTickTask_` vtable; opt-in `throttle` (skip 1 of N runs of chosen tasks). `stagger_journal` was withdrawn (it crashed in saves) and is refused at load | only `diag_trades` (read-only) | `simopt` |
| `enhance` | yes | loads (smoke test) | `binary_saves_in_debug`, `deflate_level`, `speed_table`, `save_timing`, `lean_debug`; always emits `save_begin`/`save_end`. `fast_speed5` was withdrawn (it crashed) | binary saves + save log on | `enhance` `speed` `lean` |

## Events between plugins

| event | emitter | subscribers |
|---|---|---|
| `load_transition` | fast_load | alloc_tracker, devtools, lazy_mesh, map_cache, mem_tuner, sim_opt, tick_profiler (and others) |
| `load_transition_begin` | fast_load | devtools |
| `save_begin` / `save_end` | enhance | sim_opt |
| `process_exit` | core (API v4) | devtools and plugins that write final reports |

## Hooks that cannot be shared

MinHook allows one inline hook per function. Known owners in this repo: `RunTask` (tick_profiler;
tick_par chains through a call-site stub instead), `SaveGameState` (enhance), `spdlog::logger::sink_it_`
(log_filter), mimalloc `mi_new`/`mi_malloc`/`mi_free` (alloc_tracker), the frame-cap function and
`RenderFrameIfNeeded` guard (frame_pacer), the scheduler init and worker entry (cpu_sched).
gfx_driver redirects the exe's IAT slot of `D3D11CreateDevice` instead of an inline hook.
If your plugin needs one of those, coordinate (an event, or a feature in the owning plugin).
