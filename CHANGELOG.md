# Changelog

All notable changes. The loader API version is `V3X_API_VERSION` in `include/v3x.h`; the API
is append-only (plugins built for an older version keep working).

## Unreleased

Nothing yet.

## Loader 0.6.0, API v6 (security hardening and containment)

API v6 only appends (seven `V3X_API` fields, one manifest field, new constants and events): plugins
built for API v1-v5 load unchanged ([ADR 0005](docs/adr/0005-api-v6-containment.md)). Behaviour changes
only where the old behaviour let one plugin damage others or the loader (listed under "Changed").
**Plugin DLLs must now be trusted** ([ADR 0002](docs/adr/0002-trust-model.md)): the plugins installed by
`V3X-Setup.exe` are; for your own builds use `trust <plugin>` in the console or `V3X_TRUST=dev`.

### API v6: trust and containment

#### Added
- **Plugin trust** (ADR 0002): a plugin DLL loads only if its SHA-256 is listed in
  `binaries/v3x/plugins.json` with a valid release signature `plugins.json.sig` ("official"), or is in
  the per-user allowlist `%LOCALAPPDATA%\V3X\trusted_plugins.txt` ("trusted"), or env `V3X_TRUST=dev` is
  set ("dev"). Other DLLs are never mapped. New console command `trust [plugin]` lists the refused
  plugins and allows one (its exact hash, next start). `plugins` shows each plugin's trust and
  capabilities. The installer writes `plugins.json` (`installer/make_dist.py`).
- **Pre-load checks on the file**: the manifest and the `v3x_plugin_load` / `v3x_plugin_name` exports are
  read from the DLL file with a bounded PE reader (`src/v3x_pe.h`) before `LoadLibrary`; a plugin refused
  by trust, `min_api`, `V3X_MANIFEST_REQUIRE_BUILD`, the unknown-build gate, a malformed PE or a missing
  `v3x_plugin_load` never runs (not even `DllMain`). The file is held open without write/delete sharing
  from the hash until `LoadLibrary` returned.
- **Capabilities**: `V3X_PluginManifest.capabilities` (appended, offset 56; struct 64 bytes) and
  `V3X_PLUGIN_MANIFEST_CAPS(..., "hook fs")`. Declared `hook`, `patch`, `exec` are enforced (`hook` /
  `hook_safe` / `unhook`, `patch`, `exec_command` / `exec_capture` without the word return `V3X_EPERM` /
  `ERROR_ACCESS_DENIED`, logged once); `fs`, `net`, `script` are advisory. Undeclared = all allowed.
- **Quotas** per plugin module: `V3X_QUOTA_FRAME` (16) `on_frame` callbacks, `V3X_QUOTA_EVENTS` (24) event
  subscriptions, `V3X_QUOTA_COMMANDS` (24) commands (-1 above, `v3x.log` names the quota).
- **Removal**: `off_frame`, `off_event`, `unregister_command` (remove only the caller's own
  registration; `V3X_ENOTFOUND`, `V3X_EPERM`).
- **Unicode paths**: `game_dir_w`, `v3x_dir_w` (UTF-16) and `game_dir_utf8`, `v3x_dir_utf8`.
- **Events** `plugin_disabled` (data `const V3X_PluginDisabled *`: `struct_size`, `name`, `module`,
  `reason`; synchronously when a plugin is disabled after a fault or rollback) and `plugins_loaded`
  (main thread, once after the last `v3x_plugin_load`). Event payloads from v6 on start with
  `struct_size`.
- Return codes `V3X_EARG` (-1), `V3X_ENOTFOUND` (-3), `V3X_EPERM` (-4); `V3X_PATCH_MAX` (65536).
- Contracts block in `include/v3x.h`: ownership, pointer rules, lifetimes, limits, quotas, guards.
- `src/core_parse.h`: the loader's parsers for `plugins.json`, the allowlist, capabilities and patterns,
  stateless, for unit tests and fuzzing.

#### Changed
- **Ownership**: every hook, patch and registration belongs to the calling module (return address; a
  tail call from a loader-started callback counts for that plugin). Code in no module with no plugin
  callback running is the "unknown" owner and owns nothing (`V3X_EPERM`). The loader's own hooks
  (entry point, `PeekMessageW`, `RtlExitUserProcess`) belong to the core and cannot be removed.
- **Patch rules**: the whole range must be committed and accessible (`ERROR_INVALID_ADDRESS`), at most
  `V3X_PATCH_MAX` bytes (`ERROR_INVALID_PARAMETER`); refused with `ERROR_ACCESS_DENIED` over the loader,
  another plugin's module, the first 16 bytes of a function hooked by another module or the core, for
  the unknown owner and without the `patch` capability. Least page rights (RWX only for code pages, only
  while copying), other threads suspended like MinHook under the same lock; a full record table (512)
  refuses the patch instead of applying it untracked.
- **Reserved events**: `emit_event` of `process_exit`, `plugin_disabled` or `plugins_loaded` returns -1
  and calls nobody; only the core emits them.
- **Strict `pattern_scan`**: tokens must be two hex digits or `?` / `??`; a bad token or more than 256
  bytes returns NULL with a WARNING (a bad token used to be read as `00`, long patterns were cut).
  `find_xref` with a negative index returns NULL.
- Plugin-supplied pointers (names, help texts, patch bytes, `original`) are read or written before the
  loader takes a lock, so a bad pointer faults in the caller.
- Loader version 0.6.0 (`loader_version()`, `v3x_loader_semver`).

#### Fixed
- `unhook(NULL)` disabled every hook in the process, including the loader's own; it now returns
  `V3X_EARG` and does nothing. `unhook` of a target the loader never hooked returns
  `MH_ERROR_NOT_CREATED` and touches nothing.

### Security hardening (core, SDK, build)

#### Added
- `sdk/v3x_safe.h` (header-only, C99/C++): `V3xStr` bounded string builder (never overruns,
  `truncated` flag, cut at a UTF-8 boundary), `V3xSpan` checked byte reader, checked arithmetic
  (`v3x_add_ok`, `v3x_mul_ok`, `v3x_alloc_array`), `V3xResult` + `V3X_MUST_USE`,
  `v3x_path_rel_ok` / `v3x_path_join_checked` (no `..`, `:`, absolute, device names, optional
  reparse-point refusal), `v3x_sanitize_line`, atomics, `V3X_ASSERT_MAIN`. [ADR 0004](docs/adr/0004-sdk-safety.md)
- `sdk/v3x_util.h` `v3x_config_lock` / `v3x_config_unlock`: the shared `Local\v3x_ini` mutex for
  v3x.ini read-modify-write.
- `V3X_PRINTF(fmt, args)` in `include/v3x.h`; `log`, `print`, `log_at` are format-checked (no ABI change).
- Thread rule of every API entry written in `include/v3x.h`.
- `test/fuzz/`: deterministic in-tree mutational fuzzer (`v3x_fuzz.h` harness API, `fuzz_main.c`
  driver, UBSan-trap, guard-page inputs) with harnesses for `v3x_safe.h`, `core_ini.h`,
  `v3x_ini.h` and `v3x_rtti.h`; `t fuzz` in `test/run_all.sh` (`FUZZ_ITERS` for long runs).
- `test/static_analysis.sh` (clang-tidy / cppcheck if installed, non-blocking report).
- `test/api_proxy_test.c` (proxy fails closed), `harness_unicode` e2e test (non-ANSI game path),
  threading checks in `test/api_v5_test.c`, hook-lifetime and W^X checks in `test/hook_chain_test.c`.
- `SECURITY.md`, `docs/adr/0001`..`0004`.

#### Changed
- Hooks are never `MH_RemoveHook`'ed at runtime: `unhook` and rollback only disable, trampolines
  are never freed (a thread may be about to call one). Raw hooks go through the per-target stub too,
  so re-hooking reuses the same MinHook entry. [ADR 0003](docs/adr/0003-hook-lifetime.md)
- Hook stub pool is W^X: stubs are generated once per page and the page is `PAGE_EXECUTE_READ`; the
  per-hook data lives in separate read-write pages (was one RWX pool).
- `api->on_frame` registration is atomic (any thread); command handlers are serialised by one
  recursive command-run lock (another thread waits up to 5 s, then -1).
- The `version.dll` proxy fails closed: every export starts at a fallback that returns the
  function's failure value with `ERROR_MOD_NOT_FOUND`; `GetSystemDirectoryW` is bounds-checked.
- The core opens its files (v3x.ini, v3x.log, loading.marker, crash.log, caligula_branch.txt) and
  loads plugins through wide paths; `game_dir()` / `v3x_dir()` stay ANSI (WARNING when lossy).
- `include/v3x_rtti.h`: 64-bit bounds arithmetic (no 32-bit RVA wrap), names must end inside the
  image, PE headers checked against the mapped size and read with `memcpy` (found by the fuzzer: a
  crafted unaligned `e_lfanew` was undefined behaviour).
- Build: `-O2 -fstack-protector-strong -D_FORTIFY_SOURCE=2 -ftrivial-auto-var-init=zero
  -fno-delete-null-pointer-checks -Wall -Wextra -Wformat=2 -Wvla -Wshadow` for everything,
  `-Werror` for the core and the SDK headers (C and C++; `v3x_safe.h` also `-Wconversion`);
  plugins with `V3X_WERROR=1`. Same flags in `sdk/build_plugin.py` (`--werror`, `--sanitize`).
  `SANITIZE=1 test/run_all.sh` builds the test executables with UBSan (trap).
- `v3x.ini` scanner of the loader moved to `src/core_ini.h` (unchanged rules) so it can be fuzzed.

#### Fixed
- `test/par_load_test.c`: the fork/join determinism check accumulated more than 1 MB of `snprintf`
  output into a 1 MB buffer (the size argument wrapped, writes ran past the buffer); the output is now
  hashed as it is produced. Found by `SANITIZE=1` (`-fsanitize=bounds`).
- `test/ai_par_test.c`: the fake Step's cost-vector end pointer was formed 1004 elements past the
  end of its 64-entry array; it is now clamped (the plugin already checks the vector length).

## Loader 0.5.0, API v5 (stability release)

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

## API v4

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
