# API stability contract

What a plugin author can rely on when V3X, the game, or both are updated. The reference for each
function is [api-reference.md](api-reference.md); the rules for touching engine code are in
[safety.md](safety.md), and how to write a plugin that stays safe in
[safe-coding.md](safe-coding.md). This page is the promise; the tests named below enforce it.

## What is covered

| part | stability | enforced by |
|---|---|---|
| `include/v3x.h`: `V3X_API`, `V3X_HookCall`, `V3X_Xmm`, `V3X_PluginManifest`, `V3X_LoadTransition`, `V3X_SaveBegin`, `V3X_PluginDisabled`, every `#define` constant, callback types | **binary (ABI) frozen**, append-only | `test/abi_test.c` (compile-time layout asserts), `test/compat_test.c` |
| loader behaviour documented in api-reference.md (return values, thread rules, load order and pre-load checks, trust, manifest and capability rules, ownership, quotas, event names and payloads) | frozen; changes only as fixes that make the documented behaviour true, or as containment fixes listed in the CHANGELOG | `test/api_v4_test.c`, `test/api_v5_test.c`, `test/api_v6_test.c`, `test/compat_test.c` |
| `sdk/*.h` (header-only helpers) | **source** compatible within a `V3X_SDK_VERSION`; compiled into your DLL, so a new SDK never changes an already built plugin | `test/sdk_test.c`, template test |
| `include/v3x_rtti.h` | source compatible (header-only) | `test/rtti_test.c` |
| `sdk/build_plugin.py` command line, `check` exit codes and `--json` keys | stable; options are only added | `test/compat_check.py` |
| the engine (victoria3.exe) | **nothing**: every game patch may move or change any function | anchors that fail closed (below) |

Not covered: the bundled plugins' own console commands, ini keys and report formats (each plugin
documents its own), log line wording other than the `ERROR:` / `WARNING:` prefixes, and anything
marked "test only" or "not verified in game".

## Three version numbers

| number | where | changes when | a plugin uses it to |
|---|---|---|---|
| **API version** `V3X_API_VERSION` / `api->api_version` (6) | `include/v3x.h`; loader export `v3x_loader_api` | a field or event is appended to the API (never for fixes) | require features: `V3X_HAS(api, field)` at run time, `min_api` in the manifest |
| **loader version** (semver) `api->loader_version()` ("0.6.0") | `src/core.c` `V3X_LOADER_SEMVER`; loader export `v3x_loader_semver` | patch: fixes; minor: new API version, new commands/ini keys, hardening; major: reserved, see [Deprecation](#deprecation) | report it in bug reports; never gate features on it (use `V3X_HAS`) |
| **game build** `api->game_version()` ("release/1.15.1") and `api->game_build_id()` ("6af3094f-60fb000") | caligula_branch.txt; the exe's PE header | every game patch (the build id also for hotfixes that keep the branch name) | `game_builds` in the manifest, `api->build_matches()`, and anchors |

The API version and the loader version are independent of the game: the same plugin binary keeps
loading after a game patch; whether it still *works* depends on its anchors.

## ABI rules (append-only)

1. `V3X_API` is a table of 8-byte function pointers after two `uint32_t` (`api_version`,
   `struct_size`). Slot N of API vN stays at the same offset with the same signature forever.
   New fields are added **only at the end**, in a new API version, and the loader reports that
   version only when it fills every field of it (`test/compat_test.c` checks that no pointer up to
   `struct_size` is NULL).
2. Never reordered, removed, resized or retyped: fields of `V3X_API`, `V3X_HookCall` (0x110 bytes,
   the thunk writes it at fixed offsets; `reserved` stays 0 for you), `V3X_Xmm` (16),
   `V3X_PluginManifest` (64 since v6, 56 before; grows only at the end, read through its
   `struct_size`), `V3X_LoadTransition` (24), `V3X_SaveBegin` (8), `V3X_PluginDisabled` (32).
   `V3X_API` is 120 bytes in v1, 240 in v5 and 296 in v6 (`test/abi_test.c` `V3X_ABI_V*_SIZE`).
3. Constants keep their values: `V3X_HOOK_MAX_ARGS` 16, `V3X_HOOK_MAX_SUBSCRIBERS` 8,
   `V3X_EHOOKBUSY` 100, `V3X_MANIFEST_REQUIRE_BUILD` 1, `V3X_QUOTA_FRAME` 16, `V3X_QUOTA_EVENTS` 24,
   `V3X_QUOTA_COMMANDS` 24, `V3X_PATCH_MAX` 65536, the return codes and log levels below, the event
   names.
4. Calling convention: Win64 C. Strings are UTF-8 / ASCII, NUL-terminated, owned by whoever
   passes them (the core copies what it keeps).
5. A plugin built against any older header keeps working: it sees a prefix of today's table.
   Proven by `test/compat_test.c`: plugins compiled against the frozen headers
   `test/compat_v1.h` .. `test/compat_v5.h` (v4 is the 317e574 header verbatim, v1-v3 are that
   header cut at the documented version boundaries, v5 is the frozen API v5 header) load into the
   current loader and every call they make works; and a newer plugin loads into the real v4 loader
   built from git 317e574 and degrades (`compat_test.exe old`).
6. A plugin built against a newer header runs on an older loader if it checks before using newer
   fields: `if (V3X_HAS(api, log_at)) ...` (`struct_size` covers the field and the pointer is
   set), or declares `min_api` so the older loader refuses it cleanly.
7. Event payloads added from API v6 on start with `uint32_t struct_size`; a field is read only when
   `struct_size` covers it (`V3X_LoadTransition` and `V3X_SaveBegin` predate the rule).

Changing `include/v3x.h` therefore means: append, bump `V3X_API_VERSION`, pin the new slot in
`test/abi_test.c` (it fails to compile until you do), fill it in `src/core.c` `g_api`, and add it to
the tables in api-reference.md and the CHANGELOG.

## Return codes

The values of v1-v4 are unchanged; API v5 names them, API v6 adds three. `0` is success everywhere.

| code | value | meaning |
|---|---|---|
| `V3X_OK` | 0 | success |
| `V3X_EFAIL` | -1 | bad arguments, a table or queue is full, unknown (or disabled) command, the process is exiting. Also MinHook's `MH_UNKNOWN` |
| `V3X_EARG` | -1 | v6 name for bad arguments (same value as `V3X_EFAIL`) |
| `V3X_EEXIST` | -2 | the name / subscription already exists (`register_command`, `on_event`) |
| `V3X_ENOTFOUND` | -3 | v6: `off_frame` / `off_event` / `unregister_command` found no such registration |
| `V3X_EPERM` | -4 | v6: the caller may not (unknown owner, another module's or the core's registration or hook, capability not declared) |
| `V3X_EMH_*` | 2..10 | MinHook status from `hook` / `unhook` / `hook_safe` (see api-reference.md) |
| `V3X_EHOOKBUSY` | 100 | another plugin or the core holds the hook target; v3x.log names it |
| Win32 error | > 0 | `patch` only (`ERROR_INVALID_PARAMETER`, `ERROR_INVALID_ADDRESS`, `ERROR_ACCESS_DENIED`, `ERROR_NOT_ENOUGH_MEMORY`, or `GetLastError()` of `VirtualProtect`) |
| counts | >= 0 | `print` (chars), `emit_event` (subscribers called), `enum_commands` (commands) |

Functions without a result (`log`, `log_at`) and getters never fail; getters return a valid
string (`"unknown"` when the value is unknown), never NULL.

## Thread rules

| function | callable from | callbacks run on |
|---|---|---|
| `log`, `log_at`, `print` | any thread | - |
| `log_level`, `loader_version`, `build_matches`, `image_*`, `game_*`, `*_dir`, `*_dir_w`, `*_dir_utf8`, `main_thread_id` | any thread, lock-free, constant after boot (`log_level` changes only by `loglevel`) | - |
| `pattern_scan`, `find_string`, `find_xref`, `function_start` | any thread (read-only) | - |
| `hook_safe`, `hook`, `unhook` | any thread, best at load (MinHook briefly suspends other threads) | detour / pre / post: the thread calling the hooked function |
| `patch` | load time, or while the code cannot run (other threads are suspended while it writes) | - |
| `on_frame`, `off_frame` | any thread (atomic registration and removal) | main thread, every message-pump iteration |
| `run_on_main` | any thread | main thread, next frame tick |
| `register_command`, `unregister_command` | any thread | console input thread, or the caller of `exec_command` / `exec_capture` |
| `exec_command`, `exec_capture`, `emit_event`, `enum_commands` | any thread | synchronously on the caller; command handlers are serialised (one recursive lock, 5 s wait, then -1) |
| `on_event`, `off_event` | any thread | the emitting thread; `config_reload`, `plugins_loaded` and `load_transition*` on the main thread; `plugin_disabled` on the thread that disabled the plugin |

Process exit: once `ExitProcess` has started killing threads, every function returns at once
without effect; do nothing in `DllMain(DLL_PROCESS_DETACH)`.

## Behaviour the loader guarantees (0.5.0, extended in 0.6.0)

Since 0.6.0 ([ADR 0002](adr/0002-trust-model.md), [ADR 0005](adr/0005-api-v6-containment.md)):

* **Trust**: a plugin DLL loads only if it is official (listed with its SHA-256 in a `plugins.json`
  whose `plugins.json.sig` verifies with the release key), in the per-user allowlist
  `%LOCALAPPDATA%\V3X\trusted_plugins.txt` (console `trust <plugin>`), or env `V3X_TRUST=dev` is set.
  Authenticity and consent, not a sandbox.
* **Pre-load checks**: trust, PE validity, the `v3x_plugin_load` export, `min_api`, `REQUIRE_BUILD` and
  the unknown-build gate are decided on the file, before `LoadLibrary`; a refused plugin never runs.
  The file is held without write/delete sharing until it is mapped.
* **Ownership**: hooks, patches and registrations belong to the calling module; no plugin can remove
  another plugin's or the core's. `unhook(NULL)` does nothing (`V3X_EARG`).
* **Patch rules**: only committed, accessible ranges up to `V3X_PATCH_MAX`, never over the loader,
  another plugin's module or another owner's hooked prologue; least page rights; other threads
  suspended; never untracked.
* **Capabilities** declared in the manifest are enforced for `hook`, `patch`, `exec`.
* **Quotas** per plugin; **removal** with `off_frame`, `off_event`, `unregister_command`.
* **Reserved events**: only the core emits `process_exit`, `plugin_disabled`, `plugins_loaded`.
* **Strict `pattern_scan`**: a malformed pattern returns NULL, never a different pattern.

Since 0.5.0:

* **Fault isolation**: a hardware fault or escaping C++ exception inside `v3x_plugin_load`,
  `on_frame`, `run_on_main`, a command or an event subscriber is caught; that plugin's callbacks
  are skipped from then on and the game continues (logged as `FAULT in plugin ...`). Faults in
  hook detours / `hook_safe` pre/post, in engine code and in your own threads are **not** caught:
  they crash the game exactly as without V3X (crash_logger records them).
* **Rollback**: `v3x_plugin_load` returning non-zero or faulting disables every hook the plugin
  installed (trampolines are never freed: [ADR 0003](adr/0003-hook-lifetime.md)) and restores its
  patches; its callbacks never run; `plugin_disabled` is emitted. Return non-zero whenever you
  could not install everything you need: half-installed plugins do not exist.
* **Crash-loop guard**: if the game dies inside a plugin's load, that plugin is skipped on the
  next start (once) and the console says so.
* **Unknown-build gate**: see [Game patches](#game-patches).
* The `plugins` command always shows why a plugin is not running.
* **Hook lifetime** (security hardening): `unhook` and rollback only disable; no trampoline or
  hook stub is ever freed while the process runs. The hook stub pool is W^X (stubs read-execute,
  their data read-write, never both).
* **Command threading rule**: command handlers never run concurrently (see the thread rules).
* **Proxy fails closed**: if `System32ersion.dll` cannot be loaded, the forwarded `version.dll`
  functions return their failure value (`FALSE` / 0, `GetLastError` = `ERROR_MOD_NOT_FOUND`) instead
  of crashing.
* **Paths**: the loader opens its own files and loads plugins through wide (UTF-16) paths, so a
  game folder outside the ANSI code page works. `game_dir()` / `v3x_dir()` stay ANSI (frozen
  `const char *`); boot logs a WARNING when the folder cannot be represented in the ANSI code page.
  API v6 adds `game_dir_w` / `v3x_dir_w` (UTF-16) and `game_dir_utf8` / `v3x_dir_utf8`.
* **printf checking**: `log`, `print` and `log_at` carry `V3X_PRINTF` format attributes (GCC/clang);
  no layout change (`test/abi_test.c`).

## Game patches

A game patch can change every engine function. V3X never hardcodes engine addresses; a plugin
must not either. What happens on a patch:

1. **The loader** knows the builds it was tested on (`v3x_loader_known_builds`, 0.5.0:
   `6af3094f-60fb000` = 1.15.1). On any other `victoria3.exe` it starts in **safe mode**
   (default `unknown_build = safe`): only plugins whose manifest declares the running build are
   loaded. Declaring means `"*"` (the plugin does not depend on engine internals), a prefix
   (`"release/1.15*"`, your claim that it survives patches of that line) or the exact build id.
   An exact `game_version` string such as `"release/1.15.1"` does **not** count, because a hotfix
   keeps the branch name. Players can force everything with `unknown_build = load` at the top of
   `v3x/v3x.ini`. The console and v3x.log say what happened.
2. **The plugin's anchors** (`sdk/v3x_anchor.h`) are the second line: each engine function is
   found from strings and byte shapes and verified (unique match, prologue, call targets). If any
   check fails, the plugin installs **nothing** for that feature, logs `anchor X FAILED: ...`
   and returns non-zero from `v3x_plugin_load` (which rolls back anything already installed).
   Never fall back to a guessed address or prototype.
3. **The author** re-runs the offline test against the new exe
   (`python sdk/build_plugin.py <dir> --test path/to/victoria3.exe`), fixes anchors that moved,
   adds the new build id or version to `game_builds`, and checks the result with
   `python sdk/build_plugin.py check <dll>`.

Use `V3X_MANIFEST_REQUIRE_BUILD` for plugins that patch data layouts (structure offsets) rather
than functions: there an anchor cannot prove the layout, so refuse unknown builds outright.

## Deprecation

* A field of `V3X_API` is **never removed** and never changes meaning. A deprecated function stays
  in the table and keeps working; it is marked `/* deprecated: use X */` in `v3x.h`,
  api-reference.md and the CHANGELOG, at least one minor loader release before any behaviour
  change.
* If keeping a function working would be unsafe (for example it turns out it cannot be made
  thread-safe), it may become a stub that returns `V3X_EFAIL` and logs one `WARNING` naming the
  calling plugin. That is the only allowed behaviour change, it is announced in the CHANGELOG
  first, and it never crashes or blocks.
* An incompatible table would be a new generation with a new export name (e.g. `v3x_plugin_load2`)
  loaded side by side with the old one, together with loader major version 1+; there is no plan
  for one.
* SDK headers: a helper's signature does not change within a `V3X_SDK_VERSION`; replaced helpers
  keep compiling for at least one SDK version with a deprecation comment.

## Support matrix

| | supported | tested by |
|---|---|---|
| game | Victoria 3 1.15.1 (`release/1.15.1`, build `6af3094f-60fb000`); other builds in safe mode | offline anchor tests against the real exe, in-game sessions |
| OS | Windows 10 / 11 x64 (the game's own requirement) | Windows 10 LTSC 21H2 |
| loader 0.6.0 (API v6) + plugins built for API v1 to v6 | yes (the DLL must be trusted: official, allowlisted or `V3X_TRUST=dev`) | `test/compat_test.c` |
| older loader (API v4, v5) + plugin built for v6 | yes, if the plugin uses `V3X_HAS` (or `min_api` refuses it); older loaders ignore `capabilities` | `compat_test.exe old` |
| plugin compilers | any C ABI x64 compiler: zig cc (tested), clang, MSVC, mingw gcc. `v3x.h` and the SDK compile as C and C++ | `test/run_all.sh` (zig cc; `cxx_headers` with zig c++) |
| Proton / Wine | not tested | - |

## SDK audit (what plugin authors kept re-implementing)

Grep of the 28 bundled plugins (2026-11-13) and what the platform now offers instead:

| re-implemented in | what | now |
|---|---|---|
| 25 plugins, ~40 sites | `snprintf(path, "%s<name>.ini", api->v3x_dir())` and report paths | `v3x_plugin_file`, `v3x_path` (sdk/v3x_util.h) |
| 15 plugins | a private QueryPerformanceCounter clock (`now_ms`, `qpc_freq`, ...) | `v3x_qpc`, `v3x_now_ms`, `v3x_now_s`, `v3x_qpc_ms` |
| 8 plugins | `.tmp` + `MoveFileEx` report writes | `v3x_file_write_atomic` |
| 0 plugins (none had it) | log levels; every plugin logs everything or has its own `verbose` key | `log_at` / `log_level` (API v5), `V3X_LOG*` macros with fallback, `[log]` in v3x.ini, `loglevel` command |
| 1 plugin (map_cache) | re-reading its ini at run time | `reload [plugin]` command + `config_reload` event (API v5), `v3x_reload_is_for` |
| 5 plugins | own main-thread id capture | `main_thread_id` (v4), `v3x_on_main_thread` (sdk/v3x_game.h) |
| 14 own ini parsers + 7 `GetPrivateProfile*` | config parsing | `v3x_ini.h` (exists since API v4; migration of the plugins is up to their owners) |
| 13 plugins | `VirtualQuery` / `ReadProcessMemory` guarded reads | `v3x_mem.h` (exists) |
| 0 | manifest | all bundled plugins now declare one (`crash_logger`, `hello`: `"*"`; the rest `release/1.15.1`) |
| plugins that join a name read from a file or mod onto a folder | `snprintf(path, "%s%s", dir, name)` | `v3x_path_join_checked(out, cap, dir, name, V3X_PATH_SINGLE \| V3X_PATH_CHECK_REPARSE)` (sdk/v3x_safe.h): no `..`, drive letters, device names or reparse points |

## Release checklist for a plugin

1. `V3X_PLUGIN_MANIFEST_CAPS` with a version, the builds you tested (`game_builds`) and the
   **capabilities** you use, nothing more (e.g. `"hook fs"`).
2. Every engine address comes from a `V3xAnchor`; any failure = install nothing, return non-zero.
   Hooks are `hook_safe`; every raw `hook` / `patch` has a written reason and the containment from
   its Unsafe box in [api-reference.md](api-reference.md).
3. Optional fields behind `V3X_HAS`; or `min_api` in the manifest.
4. Builds clean with warnings as errors: `python sdk/build_plugin.py <dir> --werror`.
5. Offline test passes against the real exe, also with UBSan:
   `python sdk/build_plugin.py <dir> --werror --sanitize --test`.
6. No banned idiom: `bash tools/check_unsafe.sh plugins/<name>` exits 0 (justified exceptions carry
   `v3x-unsafe-ok: <reason>`).
7. Every parser of file, mod or network data has a fuzz harness `test/fuzz/fuzz_<name>.c`
   ([safe-coding.md](safe-coding.md#testing)).
8. `python sdk/build_plugin.py check build/plugins/<name>.dll` says `compatible`.
9. Tell players how to allow it: a plugin that is not in V3X's signed `plugins.json` loads only after
   `trust <name>` in the V3X console (once per version).
