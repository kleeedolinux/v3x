# ADR 0005: API v6: ownership, containment and removal

Status: accepted (2026-11-13). Loader 0.6.0, `V3X_API_VERSION` 6. Companion of
[ADR 0002](0002-trust-model.md) (which plugins may load) and [ADR 0003](0003-hook-lifetime.md) (hooks are
disabled, never freed).

## Context

Up to API v5 the loader trusted every caller of the API equally. That left holes in which one plugin, by
a bug or on purpose, could damage the others or the loader:

* `unhook(NULL)` disabled **every** hook in the process (MinHook's `MH_ALL_HOOKS`), including the
  loader's own entry-point and frame hooks.
* `patch` could write over the loader, another plugin's module or the first bytes of a function another
  plugin had hooked, and it did not suspend the other threads while it wrote.
* A plugin could fill the shared tables (64 frame callbacks, 64 event subscriptions, 128 commands) and
  starve every plugin loaded after it. Registrations could not be removed.
* Any plugin could `emit_event("process_exit", ...)` and make every subscriber run its shutdown code.
* `pattern_scan` read a bad token as `00` and cut long patterns, so a typo produced a different, valid
  looking pattern.
* Plugins that call other plugins' code (script_ext keywords) had no way to learn that a plugin was
  switched off after a fault, and no signal that loading had finished.
* `game_dir()` / `v3x_dir()` are ANSI and lossy outside the ANSI code page.

## Decision

### Ownership

Every hook, patch and registration belongs to the **module** that called the API, found from the return
address. A call whose return address lies in the loader (a tail call from a callback the loader started)
is attributed to the plugin whose callback runs on this thread. A call from code in no module (heap,
generated code) with no plugin callback running has the **unknown** owner, which owns nothing:
`hook`, `hook_safe`, `unhook` return `V3X_EPERM`, `patch` returns `ERROR_ACCESS_DENIED`. The loader's own
hooks belong to the **core**; no plugin can remove them (`V3X_EHOOKBUSY`).

### Hooks

* `unhook(NULL)` returns `V3X_EARG` and does nothing.
* `unhook` of a target the loader never hooked returns `MH_ERROR_NOT_CREATED` and touches nothing.
* `unhook` removes only the caller's own raw hook or `hook_safe` subscriptions (unchanged from v5).
* The `original` out-pointer is written before the hook is enabled and outside the loader's lock, so a
  bad pointer faults in the caller, not under a loader lock.

### Patches

* The whole range `[addr, addr+n)` must be committed and accessible (`ERROR_INVALID_ADDRESS`, nothing
  written); `n <= V3X_PATCH_MAX` (65536; `ERROR_INVALID_PARAMETER`).
* Refused with `ERROR_ACCESS_DENIED`: the loader's memory (`version.dll`, its API table, its hook stub
  pool), another plugin's module, the first 16 bytes of a function hooked by another module or the
  core, the unknown owner, a plugin whose manifest declares capabilities without `patch`.
* Pages get the least rights needed (read-write for data; read-write-execute only for code pages and
  only while the bytes are copied) and their original protection back.
* Other threads are suspended with the same Toolhelp32 + `SuspendThread` scheme MinHook uses, under the
  same lock, so a hook and a patch never suspend each other.
* Every patch is recorded for rollback. When the table (512 live patches) is full the patch is refused
  (`ERROR_NOT_ENOUGH_MEMORY`) instead of being applied untracked.

### Capabilities

The manifest field `capabilities` (appended to `V3X_PluginManifest`, offset 56): `hook`, `patch` and
`exec` are enforced, `fs`, `net`, `script` advisory; undeclared = everything allowed. Details in
ADR 0002.

### Quotas and removal

* Per plugin module at most `V3X_QUOTA_FRAME` (16) `on_frame` callbacks, `V3X_QUOTA_EVENTS` (24) event
  subscriptions and `V3X_QUOTA_COMMANDS` (24) commands; above that the call returns -1 like a full table
  and `v3x.log` names the quota. The global table sizes stay.
* New API fields `off_frame`, `off_event`, `unregister_command` remove the caller's own registration
  (`V3X_ENOTFOUND` if there is none, `V3X_EPERM` if it belongs to another module or the core). A call in
  progress may finish; no new call starts after the removal returned.

### Events

* `process_exit`, `plugin_disabled` and `plugins_loaded` are **reserved**: only the core emits them;
  `emit_event` of these names returns -1 and calls nobody.
* `plugin_disabled` (data `const V3X_PluginDisabled *` {`struct_size`, `reserved`, `name`, `module`,
  `reason`}, 32 bytes): emitted synchronously on the thread that disabled a plugin (fault guard,
  rollback), before any of its callbacks can run again. Registries that call plugin code stop calling
  `module`.
* `plugins_loaded` (data NULL): main thread, once, after the last `v3x_plugin_load` returned.
* Rule for every event payload from v6 on: it starts with `uint32_t struct_size`, so fields can be
  appended and read only when `struct_size` covers them.

### Strict scanning and Unicode paths

* `pattern_scan` accepts only two-hex-digit tokens and `?` / `??`, separated by spaces or tabs, up to
  256 bytes; anything else returns NULL and logs a WARNING.
* `find_xref` with `index < 0` returns NULL.
* New fields `game_dir_w`, `v3x_dir_w` (UTF-16) and `game_dir_utf8`, `v3x_dir_utf8` (UTF-8), correct for
  any folder name.

### Return codes

`V3X_EARG` (-1, the value of `V3X_EFAIL`: functions that returned -1 for bad arguments keep doing so),
`V3X_ENOTFOUND` (-3) and `V3X_EPERM` (-4), returned only where documented.

## Consequences

* Plugins built for API v1 to v5 load unchanged. Behaviour changes only where the old behaviour was the
  hole: `unhook(NULL)`, patches over foreign memory, emitting the core's events, malformed patterns,
  registrations beyond the quota.
* A plugin that relied on patching another module, or on malformed patterns, now fails closed and says
  so in `v3x.log`.
* `V3X_PluginManifest` grows from 56 to 64 bytes (read through `struct_size`; older loaders read only up
  to `game_builds`). `test/abi_test.c` pins the new layout; `test/compat_test.c` loads plugins built
  against the frozen v1 to v5 headers.
* Not a sandbox: a plugin can still call `VirtualProtect`, MinHook or the OS directly. The rules contain
  bugs and well-behaved plugins that go through the API; ADR 0002 limits which plugins run at all.
