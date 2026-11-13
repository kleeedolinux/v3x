---
title: "Maturity assessment"
summary: "How mature V3X is as a platform, area by area, with the evidence for each score."
group: engine
order: 23
source: "docs/maturity.md"
sourceUrl: "https://github.com/kleeedolinux/v3x/blob/main/docs/maturity.md"
updated: "2026-11-13"
---
Status of V3X as a platform (loader, bundled plugins, SDK, plugin API, tests and releases).
Scores are out of 5. **Overall: about 2.7 / 5 = alpha** (independent review, 2026-11-13, before the
loader 0.5.0 hardening). Hardening work is in progress (see "In progress"); the scores below are the
review's and have **not** been updated for it. `@main` updates the "after verification" column once
the hardening is verified.

How to read the evidence: every claim cites a file:line or a test name, as of 2026-11-13.
**[unverified]** marks claims that were not re-checked when this page was written. Line numbers in
`src/core.c` and `include/v3x.h` may move: the sdk worker is still editing them.

## Scores

| area | review (pre-hardening) | after verification | main reasons in the review |
|---|---|---|---|
| Loader | 3 / 5 | *TBD by @main* | no fault isolation; no global build gate; load order = `FindFirstFile` order; a failed plugin was not rolled back |
| Bundled plugins | 2.5 / 5 | *TBD* | many measure-only or smoke-tested plugins; crash history; default-on plugins with smoke tests only |
| SDK | 3 / 5 | *TBD* | clone + zig unpinned; assumes reverse-engineering skill |
| Plugin API | 3 / 5 | *TBD* | no unregister; no game-level abstractions; script layer (b) exists now, layers (a) and (c) do not |
| Tests and releases | 2 / 5 | *TBD* | no test runner, no CI, no releases (an installer exists now) |
| **Overall** | **~2.7 = alpha** | *TBD* | |

## Loader (3 / 5)

What is solid:
* `version.dll` proxy that forwards the real exports and hooks the entry point; plugins load before
  the game's CRT (README "How the loader works", `src/proxy.c`, `src/core.c`). End-to-end tests
  `test/harness.c`, `test/api_v4_test.c`, `test/shutdown_test.c` (20 runs x 2 modes, every plugin,
  exit within 10 s).
* Per-plugin manifests with `min_api` / `REQUIRE_BUILD` refusals (`test/api_v4_test.c`,
  `test/v4_plugin.c`), `[plugins]` skip list in `v3x.ini` (CHANGELOG.md, API v4 "Added").
* Exit safety: all API calls refuse to act once the process is exiting (CHANGELOG.md, `process_exit` entry;
  `test/shutdown_probe.c`).

Gaps found by the review (pre-hardening):
1. **No fault isolation**: a fault in a plugin's `v3x_plugin_load`, `on_frame`, command, event
   subscriber or `run_on_main` callback took the game down. Hardening adds this (see "In progress").
2. **No global build gate**: on an unknown game build every plugin loaded; only 3 plugins had a
   manifest at the time (memory note of the review). Hardening adds the gate and manifests.
3. **Load order = `FindFirstFile` order**: plugins are enumerated with `FindFirstFileA` /
   `FindNextFileA` (`src/core.c:1383`, loop to `src/core.c:1464`), i.e. directory order (NTFS: name
   order, not guaranteed). There is no explicit order or dependency declaration. **Still open** (not
   part of the hardening list).
4. **Failed plugin not rolled back**: hooks and patches of a plugin whose `v3x_plugin_load` returned
   non-zero stayed in place. Hardening adds rollback.

## Bundled plugins (2.5 / 5)

31 plugins (catalog: [plugins.md](/docs/plugins/)). By class (from `installer/profile.txt` and each
plugin's defaults):
* **11 production** (shipped on by the player installer: `installer/profile.txt:9-18`, `:20`; plus
  `script_ext` at `:19`, an SDK service that hooks nothing): crash_logger,
  content_packs, mod_menu, keep_history, ironman_console, log_filter, frame_pacer, cpu_sched,
  gfx_driver, mem_tuner, fast_load. All but mod_menu were played in the 4-hour session and the
  1 h 44 m session of 2026-11-13 evening (docs/sessions/2026-11-13-evening.md:22-23: 28/28 loaded,
  no crash). mod_menu's button was not recorded as used in game **[unverified]**
  (`installer/profile.txt:11`).
* **6 opt-in** (shipped off: `installer/profile.txt:23-28`): console_plus, loc_fallback, enhance,
  asset_stream, lazy_mesh, sim_opt. The review's gap "enhance / sim_opt default-on with smoke tests
  only" is closed for players by the installer (both `0`); for a developer install (`./build.sh
  install`) both still load by default with read-only or debug-only features on
  (`plugins/enhance/enhance.ini.example:33`, `plugins/sim_opt/sim_opt.ini.example:5-17`).
* **7 measure-only** (not shipped): fast_lookup, mod_loader, map_cache, lazy_gfx, par_load, tick_par,
  ai_par. `on` is refused in tick_par and ai_par, reserved in par_load, and without effect yet in
  map_cache and lazy_gfx (docs/sessions/2026-11-13-evening.md:171-176). fast_lookup's criterion for
  `on` is met (docs/sessions/2026-11-13-evening.md:170) and mod_loader runs `on` in the live install
  with 0 mismatches (docs/sessions/2026-11-13-evening.md:35, :175).
* **6 dev tools** and **1 SDK service** (script_ext).

Crash history (all fixed or withdrawn; details in [plugins.md](/docs/plugins/)):
* sim_opt: 4-argument detour on a 5-argument function (docs/safety.md:12-16); `stagger_journal`
  withdrawn (docs/safety.md:53-56, CHANGELOG.md "Removed / withdrawn").
* enhance: `fast_speed5` withdrawn (docs/re/enhance.md:145-182).
* fast_lookup: crash5, clobbered volatile registers (plugins/fast_lookup/fast_lookup.c:84-87).
* console_plus: crash6, AV in `mi_free` (docs/re/console.md:66).
* devtools: hang at exit with the profiler on (plugins/devtools/devtools.c:31-34).
* A diagnostic plugin hurt play: alloc_tracker's 30 s main-thread report caused ~190-200 stalls of
  ~0.55 s (docs/sessions/2026-11-13-evening.md:78, :83-84); it is off by default
  (`plugins/alloc_tracker/alloc_tracker.ini.example:6`) and not shipped.

What is solid: every plugin fails closed on anchors (docs/safety.md:102), and every plugin has an
offline or end-to-end test except `hello` ([testing.md](/docs/testing/)).

## SDK (3 / 5)

What is solid: header-only helpers in `sdk/` (`v3x_sdk.h` umbrella, mem, scan, anchor, ini, hook,
game, util, script, testkit), `sdk/build_plugin.py` (build / `--test` / `--install` / `check`,
`sdk/build_plugin.py:353`), `templates/plugin_template/` with an offline test, `test/sdk_test.c`.

Gaps:
* **Unpinned toolchain and source**: the quick start clones the default branch
  (`README.md:72`) and installs the latest `ziglang` from pip without a version (`README.md:73`,
  `docs/getting-started.md:14`); `build_plugin.py` runs whatever `python -m ziglang cc` is installed
  (`sdk/build_plugin.py:65`). No tag or release to pin to (`git tag` lists nothing, 2026-11-13).
* **Assumes reverse-engineering skill**: a plugin author must find engine code by strings, xrefs and
  byte shapes themselves (README "A complete plugin", docs/reverse-engineering.md). The SDK offers no
  ready-made handles to game objects (see API).

## Plugin API (3 / 5)

What is solid: C ABI, versioned and append-only (`include/v3x.h:36`, `V3X_API_VERSION 5` at
`include/v3x.h:39`), `V3X_HAS` for newer fields, `hook_safe` that forwards all argument slots
(`test/hook_safe_test.c`) and can be shared (`test/hook_chain_test.c`), named events, frozen layouts
for v1-v4 (`test/compat_v1.h` ... `test/compat_v4.h`, `test/compat_test.c`), ABI freeze test
`test/abi_test.c`.

Gaps:
* **No unregister**: `on_frame`, `register_command` and `on_event` have no removal counterpart in
  `include/v3x.h` (only `unhook`, `include/v3x.h:196`). Plugins cannot be unloaded (mod_menu: "All
  changes need a restart", `plugins/mod_menu/mod_menu.c:19`).
* **No game-level abstractions**: no API for countries, states, the tick or saves beyond events;
  `sdk/v3x_game.h` covers only the script-RNG scope check and the main-thread check (CHANGELOG.md, SDK entry).
* **Script layers**: (b) "ship script, loc and GUI files from a plugin" exists (`plugins/script_ext`,
  `sdk/v3x_script.h`, `test/script_ext_test.c`: 45 checks, 0 failed, re-run 2026-11-13); (a) running
  effect / trigger text from C and (c) C-implemented script keywords are **not implemented**
  (docs/scripting.md:8-13, :128-135). Layer (b) has not run in game (docs/scripting.md:11).

## Tests and releases (2 / 5)

What exists: 39 C test programs (plus 4 test-only plugin sources) and 3 Python tests ([testing.md](/docs/testing/)), each printing `ALL OK`; offline tests
check anchors against the real exe mapped as an image. Player installer `V3X-Setup.exe`
(`installer/`, docs/install.md) with `installer/test_installer.py`: **49/49 passed** (re-run
2026-11-13 after the profile update, on fake game folders).

Gaps:
* **No test runner**: tests are built and run one by one by hand ([testing.md](/docs/testing/)).
  `test/run_all.sh` is in progress and does not exist yet.
* **No CI**: no `.github/` or other CI configuration in the repo (2026-11-13).
* **No releases**: no git tags, no published zip (README.md "Install in 3 steps" says so).
* The installer builds from `build/` (`python installer/make_dist.py --no-build` passed its profile
  check on 2026-11-13 once `ai_par` and `script_ext` were added to `installer/profile.txt`), but it
  has not been published as a release.

## In progress (sdk worker, loader 0.5.0 / API v5)

Being added by the sdk worker at the time of writing. Code for most items is already in
`src/core.c`; **none of it is verified here**: `test/api_v5_test.c` was not run for this page and
`test/run_all.sh` does not exist yet. `@main`: re-score Loader, API and Tests after checking each row.

| item | where (2026-11-13) | test | verified |
|---|---|---|---|
| Crash-loop safe mode: `loading.marker` holds the plugin being loaded; after a crash during load that plugin is skipped for one start | `src/core.c:1261-1265`, message at `src/core.c` "crashed the game while loading" (~line 1376) | `test/api_v5_test.c` (`-DV5_MARKED`) | no |
| Fault isolation: SEH guard around `v3x_plugin_load`, `on_frame`, `run_on_main`, commands, event subscribers; faulted module's callbacks skipped, its hooks removed | `src/core.c:324-379` (guards), `src/core.c:401` (disabled modules), `src/core.c:430` (fault handler), `src/core.c:1036` (frame skip) | `test/api_v5_test.c` (`-DV5_FRAME_FAULT`, `_CMD_FAULT`, `_EVENT_FAULT`, `_MAIN_FAULT`) | no |
| Rollback of a failed / faulting `v3x_plugin_load`: hooks (`v3x_hook_remove_owner`) and patches restored | `src/core.c:988-1000` (`rollback_module`) | `test/api_v5_test.c` (`-DV5_LOAD_FAULT`, `-DV5_LOAD_FAIL`) | no |
| Unknown-build gate: on an unknown build only plugins whose manifest matches (`*`, prefix, exact id) load; `unknown_build = load` restores the old behaviour | `src/core.c:1049` (known builds), `src/core.c:1124-1141`, `src/core.c:1330-1363` | `test/api_v5_test.c gate`, `gateload` | no |
| Manifests in all plugins | `V3X_PLUGIN_MANIFEST` in all 31 `plugins/*/*.c` (grep 2026-11-13); `*` for crash_logger, hello, script_ext | `sdk/build_plugin.py check`, `test/compat_check.py` | partly (grep only) |
| Test runner `test/run_all.sh` | does not exist yet | - | no |
| API v5 additions: `log_at`, `log_level`, `loader_version`, `build_matches`, core event `config_reload` | `include/v3x.h:74`, `src/core.c:1069` | `test/api_v5_test.c`, `test/compat_test.c` | no |

Not on the hardening list: explicit load order (Loader gap 3), unregister / unload (API), CI,
releases, pinned toolchain.

## Top 10 to 1.0

The reviewer's list (verbatim, 2026-11-13, pre-hardening), with the status as of this page. Status
"in progress" = code present, not verified (see the table above).

| # | reviewer's item | status 2026-11-13 |
|---|---|---|
| 1 | Fault isolation around plugin load/frame/command/event callbacks + crash counter / safe mode. | in progress: SEH guards `src/core.c:324-379`, `loading.marker` `src/core.c:1261-1265` |
| 2 | Global build gate: warn/auto-disable on unknown game_build_id; require manifests. | in progress: gate `src/core.c:1330-1363`; all 31 plugins have a manifest (grep) |
| 3 | Default enhance and sim_opt (and fast_lookup cache) to off until each is in-game verified. | for players done: enhance and sim_opt shipped off (`installer/profile.txt:25`, `:28`), fast_lookup not shipped (`:42`). Plugin defaults unchanged (`plugins/enhance/enhance.ini.example:33`, `plugins/sim_opt/sim_opt.ini.example:5`, `plugins/fast_lookup/fast_lookup.ini.example:9` = measure) |
| 4 | Root-cause the console_plus mi_free crash. | root cause found and fixed: crash6, a CString passed by value freed memory owned by the DLL (docs/re/console.md:66, `plugins/console_plus/console_plus.c:977`). One clean 1 h 44 m session since (docs/sessions/2026-11-13-evening.md:17, :22-23); that is not proof |
| 5 | One test runner script + CI building everything and running offline tests. | runner in progress (`test/run_all.sh` not yet in the repo); no CI |
| 6 | Finish v5 consistently (core.c, abi_test.c, compat_test.c, api-stability.md). | in progress (sdk worker): `V3X_API_VERSION 5` (`include/v3x.h:39`), `test/abi_test.c`, `test/compat_test.c` exist; `docs/api-stability.md` not yet in the repo |
| 7 | Plugin dependencies/load ordering and rollback on failed load. | rollback in progress (`src/core.c:988-1000`); ordering/dependencies open (`src/core.c:1383` = `FindFirstFileA` order) |
| 8 | Pinned toolchain, release zip/installer, semver tags. | installer exists (`installer/`, 49/49); loader semver `0.5.0` (`src/core.c:51`); toolchain unpinned, no tags, no release |
| 9 | Docs updated to all plugins. | done 2026-11-13: [plugins.md](/docs/plugins/) covers all 31 |
| 10 | Multiplayer detection + refuse sim-changing features there; faster startup (index scans or show feedback). | open. Only ironman_console keeps multiplayer blocked (`plugins/ironman_console/ironman_console.c:1-8`); sim_opt `throttle` is documented as single-player only (`plugins/sim_opt/sim_opt.c:25-26`) but neither sim_opt nor enhance contains a multiplayer check (grep for `MP`/`multiplay` finds only that comment). Startup feedback: not done |

## Not verified for this page

* The whole "In progress" table (code read, not tested).
* That mod_menu's button and loc_fallback's output work in game.
