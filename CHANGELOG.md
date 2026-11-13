# Changelog

All notable changes. The loader API version is `V3X_API_VERSION` in `include/v3x.h`; the API
is append-only (plugins built for an older version keep working).

## Unreleased

### Added
- **API v4** (`include/v3x.h`, `src/core.c`, `src/safe_hook.c`):
  - `hook_safe(target, pre, post, user)`: hooks without a hand-written detour. A core-generated
    pass-through thunk forwards 16 integer argument slots and xmm0-3 and keeps the return value;
    callbacks get a `V3X_HookCall` (arguments, return value, skip, scratch). Has unwind info.
    Unit test `test/hook_safe_test.c` (0-16 args, floats, mixed, struct return, recursion,
    threads, stack walk).
  - `main_thread_id()`, `game_build_id()` (PE TimeDateStamp + SizeOfImage), `enum_commands()`.
  - Plugin manifest: optional data export `v3x_plugin_manifest` (`V3X_PLUGIN_MANIFEST` macro):
    name, version, author, description, supported game builds, `min_api`,
    `V3X_MANIFEST_REQUIRE_BUILD`. Shown by the `plugins` command; mismatches warn or refuse.
  - `V3X_HAS(api, field)` to check for newer fields.
- `[plugins]` section in `binaries/v3x/v3x.ini`: `<dll name> = 0` skips a plugin at load.
- `process_exit` event (core, on the thread calling ExitProcess, all threads alive); all API calls
  refuse to do anything once the process is exiting.
- **SDK** (`sdk/`): `v3x_sdk.h` umbrella, `v3x_mem.h` (fault-proof reads), `v3x_scan.h`
  (sections, unique pattern counts, xrefs, callers, rel32/rip targets), `v3x_anchor.h`
  (fail-closed anchor tables), `v3x_ini.h`, `v3x_hook.h` (`V3X_PASS8`, `hook_safe` helpers),
  `v3x_game.h` (script-RNG scope check, main-thread check), `v3x_testkit.h` (offline tests
  against the mapped exe), `build_plugin.py` (one-command build / test / install). Test
  `test/sdk_test.c`.
- `templates/plugin_template/`: minimal plugin with manifest, ini, anchor, `hook_safe`, console
  command and offline test.
- Documentation in `docs/`: getting started, API reference, safety rules, reverse-engineering
  guide, engine map, plugin catalog, troubleshooting, FAQ.
- Plugins added since the first import: console_plus, tick_par, mod_loader, mod_menu (see
  docs/plugins.md for their status).

### Changed
- `content_packs`: the VFS-init detour forwards 8 argument slots (`V3X_PASS8`) instead of 4.
- `plugins` console command shows version, description and author from manifests.
- `version` console command shows the game build id.

### Removed / withdrawn
- `sim_opt stagger_journal` and `enhance fast_speed5`: both crashed (script ran outside a tick
  task's RNG scope); the keys are refused and logged.

## Before this changelog (API v3)

- Loader: version.dll proxy, entry-point hook, plugin loading, frame tick, V3X console with
  commands (API v2), `exec_capture` and named events (API v3).
- Plugins: hello, ironman_console, crash_logger, devtools, devconsole, devbridge, log_filter,
  content_packs, keep_history, tick_profiler, sim_opt, fast_load, mem_tuner, alloc_tracker,
  cpu_sched, frame_pacer, gfx_driver, asset_stream, lazy_mesh, map_cache, enhance.
- MCP bridge `mcp/v3x_mcp.py`, reverse-engineering tools in `tools/`, notes in `docs/re/`.
