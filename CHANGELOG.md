# Changelog

All notable changes. The loader API version is `V3X_API_VERSION` in `include/v3x.h`; the API
is append-only (plugins built for an older version keep working).

## Unreleased

### Loader 0.5.0, API v5 (stability release)

The loader now has a version of its own (`loader_version()`, semver, "0.5.0"), separate from the
API version (5) and the game build. The contract is written down in `docs/api-stability.md`.

#### Added
- **ABI freeze, enforced**: `test/abi_test.c` pins every offset, type and constant of
  `include/v3x.h` with `_Static_assert` (a layout change fails to compile; `test/run_all.sh` also
  proves a swapped field is rejected). `test/compat_test.c` loads plugins built against frozen
  copies of the v1-v4 headers (`test/compat_v1.h` .. `compat_v4.h`) and a v5 plugin into the
  current loader, and the v5 plugin into the real v4 loader from git 317e574 (degrades via `V3X_HAS`).
- **API v5** fields (append-only, check with `V3X_HAS`): `log_at(level, tag, fmt, ...)`,
  `log_level(tag)`, `loader_version()`, `build_matches(list)`.
- Named return codes `V3X_OK`, `V3X_EFAIL`, `V3X_EEXIST`, `V3X_EMH_*` (the values of v1-v4,
  unchanged), log levels `V3X_LOG_ERROR..TRACE`, event-name macros `V3X_EVENT_*`.
- Log levels: `[log]` section in `v3x.ini` (`level = debug`, `<tag> = trace`), env
  `V3X_LOG_LEVEL`, console command `loglevel [tag] [level]`.
- Console command `reload [plugin]` and core event `config_reload` (main thread; data = plugin
  name or NULL).
- **Fault isolation**: `v3x_plugin_load`, `on_frame`, `run_on_main`, commands and event
  subscribers run inside an SEH guard; a fault logs module+offset and disables that plugin's
  callbacks, the game continues. Hook callbacks are not covered (documented).
- **Rollback**: a `v3x_plugin_load` that returns non-zero or faults has its hooks removed and its
  patches restored (`api->patch` now records the original bytes per plugin;
  `v3x_hook_remove_owner` in `src/safe_hook.c`).
- **Crash-loop guard**: `v3x/loading.marker` names the plugin being loaded; if the game died
  there, that plugin is skipped on the next start (once). The last `UNHANDLED EXCEPTION` line of
  the previous `crash.log` is logged at start.
- **Unknown-build gate**: on a `victoria3.exe` build the loader was not tested on, only plugins
  whose manifest declares the build (`*`, a prefix, or the exact build id) load;
  `unknown_build = load` in `v3x.ini` (or env `V3X_UNKNOWN_BUILD=load`) loads everything.
- version.dll data exports `v3x_loader_api`, `v3x_loader_semver`, `v3x_loader_known_builds`.
- `sdk/build_plugin.py check <dll> [--json]`: validates a plugin DLL without loading it (x64 DLL,
  exports, manifest, `min_api` vs the installed loader, `game_builds` vs the installed exe, the
  unknown-build gate, debug-CRT imports). Test `test/compat_check.py`.
- `sdk/v3x_util.h` (SDK version 2): `v3x_plugin_file`, `v3x_path`, `v3x_file_write_atomic`,
  `v3x_qpc` / `v3x_now_ms` / `v3x_now_s`, `V3X_LOGE..T` with a fallback for older loaders,
  `v3x_reload_is_for`. The testkit fills the v5 fields.
- Every bundled plugin has a `V3X_PLUGIN_MANIFEST` (`crash_logger`, `hello`: `"*"`; the others
  `release/1.15.1`).
- `test/run_all.sh`: builds and runs every offline and end-to-end test, prints a summary table,
  exit code = failed tests. `test/api_v5_test.c` + `test/api_v5_plugin.c`.

#### Changed
- `api_version` is 5; `struct_size` 240 (was 208).
- `version` command prints the loader version.
- `emit_event` returns the number of subscribers actually called (faulted plugins are skipped).
- `exec_command` returns -1 for a command whose plugin faulted.
- `pattern_scan(NULL)` / `find_string(NULL or "")` return NULL, `on_frame(NULL)` returns -1,
  `patch(NULL / 0 bytes)` returns `ERROR_INVALID_PARAMETER` (all used to crash or misbehave).
- The core is built with `-fms-extensions` (SEH); `build.sh` skips plugin folders without `.c` files.
- `sdk/v3x_ini.h`: `V3xIniKv` is declared at file scope (the header now compiles as C++ too).
- Template: `v3x_util.h` timing, `V3X_LOGD`, `reload my_plugin`; README shows `check`.

### Added (API v4)
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
