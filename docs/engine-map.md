# Engine map (Victoria 3 1.15.1)

A one-page summary of what has been reverse-engineered, with links to the detailed notes in
`docs/re/`. All addresses are RVAs in `victoria3.exe` 1.15.1 (PE TimeDateStamp `0x6af3094f`,
SizeOfImage `0x60fb000`, `game_build_id()` = `6af3094f-60fb000`). They **will** move with the next
patch: use the anchor in the "found by" column, never the number.

The engine is Paradox's Clausewitz + Jomini, one static exe (static CRT, mimalloc 2.0.x, spdlog,
PhysFS 3, SDL, D3D11/Vulkan renderers), no symbols, with MSVC RTTI and `.pdata` for every
non-leaf function.

## Process start

| step | what | where |
|---|---|---|
| 1 | Windows loads `binaries/version.dll` (V3X) because the exe imports VERSION.dll | `src/proxy.c` |
| 2 | V3X hooks the exe entry point; plugins load on the main thread before the CRT | `src/core.c` |
| 3 | CRT startup, static initializers (GUI functions, console commands and tick-task types register themselves here) | docs/re/gui.md, docs/re/console.md |
| 4 | WinMain: settings, renderer, VFS init, then the database/asset load (Empty -> Game takes ~170-200 s on the lead's machine) | docs/re/loading.md |

## Simulation tick (docs/re/simulation.md, docs/re/tick_par.md)

| RVA | role | found by |
|---|---|---|
| 01465690 | **GameState tick**, once per game tick | string `Processing Tick: ` (single xref) |
| 014736c0 | tick-task runner: runs the due tasks **strictly one after another** | `Tick Tasks for {}` |
| 0146bce0 | due-list builder (filters the global order by rate) | runner's callee, docs/re/tick_par.md |
| 0146d010 | **RunTask** (Prepare, Execute, range/context workers) | `Context %d` (exactly one xref) |
| 0146d0ee | virtual dispatch of a task's Execute (vtable slot 6) | inside RunTask |
| 01468a60 / 01468d10 | `RenderFrameIfNeeded` guard / helper (frames during long ticks) | `gamestatetick.cpp:262` |

* 272 `CTickTask_<Name>` classes (RTTI). Rates TICK, DAY, WEEK, MONTH, QUARTER, YEAR; priorities
  DEFAULT / LATE. Order: one-time stable sort + dependency DFS (dependencies are *names*, order
  hints only, not a thread-safety contract).
* Parallelism exists only **inside** a task, through range stages (`Step` flags bit0 = parallel
  range). Hot tasks: HandleAutonomousInvestment, UpdateAI, RecalculateModifierNodes,
  UpdatePopGrowth, UpdateStateTrades (an iterative solver whose evaluation is already parallel).
* **Script RNG scope**: one global owner pointer `*0x540ae80` (owner thread id at `+0x10`);
  `ALLOW_RANDOM_IN_SCOPE` sets it, parallel ranges swap it out. Script outside a tick task has no
  owner and crashes (docs/safety.md "Script and RNG scope"; check with `sdk/v3x_game.h`).
* Saves run as session commands on a job, *between* ticks (autosave between the monthly pulse tick
  and the next tick).

## Threads (docs/re/threading.md)

| RVA | role | found by |
|---|---|---|
| 03df8a50 | task scheduler init (`Threading.TaskThreadCount`, -1 = auto) | `Spawning {} normal worker threads` |
| 03df9180 | worker thread entry | lea at 03dfa64c |
| 03dfac40 | priority per worker kind (BELOW_NORMAL / NORMAL / ABOVE_NORMAL) | 3x `SetThreadPriority` |

The game never sets affinity and is not P/E-core aware (counts physical cores with
`GetLogicalProcessorInformation`). Up to 64 workers per pool. `cpu_sched` places threads with CPU
sets.

## Memory (docs/re/memory.md)

* `operator new` = **mimalloc `mi_new` 0x43d95c0** (statically linked, 15k call sites);
  `mi_free` 0x43d94b0 (leaf, no `.pdata`); `mi_malloc` 0x43d8c20. mimalloc **v2.0.x** (32 MiB
  segments). The allocator is already swapped; replacing it is pointless.
* mimalloc options are a static table that can be written before CRT init (mem_tuner does this,
  instead of `MIMALLOC_*` env vars that leak to child processes and GPU drivers).
* Measured: 17.5 GB private commit in a long session; about half outside mimalloc (likely the iGPU
  driver's resource backing). OOM: `bad_alloc` handlers mapped with `tools/re_mem_catch.py`.

## Rendering (docs/re/graphics.md)

* Backends: **DX11** and **Vulkan** (no DX12 game renderer; DX12 strings are SDL's). Launch args
  `-renderer=dx11|vulkan`, `-dx11`, `-vulkan`.
* Present wrapper 0x3c7a8d0 (hook it rather than DXGI vtables). Frame cap is a **busy-wait**
  (0x379a540, `pause` loop at 0x379a710) driven by `owner+0x37/+0x38` (1/fps); `frame_pacer`
  replaces it with a high-resolution timer sleep.
* Long ticks call `RenderFrameIfNeeded` so the UI keeps drawing during simulation.
* XeSS/DLSS/FSR backends are not built into this Windows build.

## Loading and assets (docs/re/loading.md, docs/re/streaming.md, docs/re/lazy_mesh.md, docs/re/mapmode.md)

* Phases: checksum (`Starting checksum calculation`, 0377b120), localization, texture lookup,
  **mesh types** (`IterateTypesThreaded` 0x3ae4d80, `CMeshType` vtable 0x4ace220, Load = slot
  +0x30 0x3adcfd0: every declared mesh type is loaded eagerly), script database parse.
* No generic resource manager: each asset kind has its own loader and cache. Textures:
  GetOrLoadTexture 0x3c5b4f0, global cache map 0x5421820, CreateTexture 0x3c5af20; the engine's
  own texture streamer (`CGfxTextureStreamer`, Start 0x3c613e0) is turned off by every quality
  preset (asset_stream forces it on).
* The texture cache is weak: map modes and GUI views re-read textures from disk (map_cache
  measures and optionally pins them).

## Saves (docs/re/enhance.md)

* `SaveGameState` 0x3662910, reached from autosave 0x0ca9580 through `ExecuteGameCommand`
  0x1085b90 on a session job. enhance hooks it to emit `save_begin` / `save_end`.
* `debug_mode` forces **text** saves (156 MB vs 26 MB binary); serialization, not disk, is the
  ~3.2 s freeze. Writes go to `<name>.v3.tmp`, then rename.
* Game speed table: 16 bytes in `.rdata` 0x4b08610.

## Console (docs/re/console.md)

* `CConsole` (vtable 0x4a90d88), Tab = `CConsole::Autocomplete` 0x36be560; manager getter
  0x3dc0e00, manager global `exe+0x5c72730`, matcher 0x3dc2140.
* Registry entries `SCommandData` (0x210 bytes): `+0xb0` OnExecute, `+0xb8` argument completer,
  `+0xc0` its user pointer. ~300 commands, ~26 with completers (console_plus adds them).
* The console is blocked in ironman by one predicate that references
  `No Console commands in ironman!` (ironman_console clears only that reason).

## Virtual file system and mods (docs/re/vfs.md, docs/re/modloading.md)

| RVA | role | found by |
|---|---|---|
| 0x3e17d10 | vfs_init | `../platform_specific_game_data` |
| 0x3e17820 | MountData | `No subdirs mounted for game dir from candidates: {}` |
| 0x3dff0b0 | mount(unused, dir, mount_point): **prepends** (highest priority) | `45 33 C0 E8` call in MountData |
| 0x3e16bd0 | refresh the cached search path | in MountData |
| 0x3b6d500 | dlc.cpp: mounts one DLC / mod | |

Search order: last mod ... first mod, DLC, game, jomini, clausewitz. Every lookup probes every root
(PhysFS DIR archiver: `GetFileAttributesExW`, `CreateFileW` per root), which costs ~34 failed
syscall pairs per vanilla file with 22 mods + 12 DLC (mod_loader indexes roots). A mod whose
metadata fails to parse is dropped silently.

## Logging (docs/re/logging.md)

spdlog >= 1.12 behind `NLog::CCategoryLogger`; categories and sinks come from
`platform_specific_game_data/log_settings_live.json`. Hook point: the virtual
`spdlog::logger::sink_it_` (exe+0x43d70f0), found through the RTTI of `spdlog::logger` (vtable
slot 2). log_filter drops and dedupes there, before any file sink.

## GUI (docs/re/gui.md)

GUI global functions are registered by name from CRT static initializers (e.g. `OpenWikiUrl`);
before GUI init they queue to a pending list. The main menu is `frontend_main.gui`
(`mainmenu_panel_bottom`); a file with the same VFS path in a content pack overrides it.

## Country history graphs (docs/re/history.md)

Trends are cleared by 11 `Trend::Clear` calls when a country is demoted to AI (0x1465ef0);
keep_history NOPs them.

## Where to start for common goals

| goal | start at |
|---|---|
| per-tick logic / "every day" callbacks | `RunTask` (all tasks) or one task's Execute via RTTI (`CTickTask_<Name>` slot 6) |
| timing a phase of loading | `load_transition` / `load_transition_begin` events (fast_load) |
| before/after a save | `save_begin` / `save_end` events (enhance); never run script there |
| file overrides without a mod | a folder in `binaries/v3x/content_packs/` (content_packs) |
| console features | `register_command` (V3X console) or the engine registry (docs/re/console.md) |
| rendering / frame timing | Present wrapper 0x3c7a8d0, frame cap 0x379a540 |
| memory | mimalloc entry points above; mem_tuner / alloc_tracker as examples |
