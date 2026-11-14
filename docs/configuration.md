# Configuration reference

Every V3X file lives in `binaries/v3x/` (next to the game exe), except your per-user list of trusted
plugins (below). Everything is optional: with no ini files at all you get the defaults below.

## The loader: `binaries/v3x/v3x.ini`

```ini
; no V3X console window (default: the window opens)
console=0

[plugins]
; <dll file name without .dll> = 0 / off / no / false: not loaded at the next start
lazy_mesh = 0
alloc_tracker = 0
```

Comments go on their own line (`;` or `#`); a comment after a value becomes part of the value (`lazy_mesh = 0 ; x` does **not** disable lazy_mesh).

A missing key means the plugin is loaded if it is trusted ([below](#plugin-trust-and-the-allowlist)).
`v3x.ini` has no key that switches the trust check off: it lives in the game folder, which other
programs can write. The `mod_menu` plugin (main menu > **V3X**) and the console
command `modmenu toggle <plugin>` edit the `[plugins]` section for you; changes apply at the next start.

More loader keys: `[log]` `level = <level>` / `<tag> = <level>` (log thresholds) and
`unknown_build = load` at the top (load every plugin on a game build the loader does not know); see
[api-reference.md](api-reference.md#environment-variables-and-v3xini).

Environment variables: `V3X_DISABLE=1` (V3X does nothing for this run), `V3X_NO_CONSOLE=1` (no console
window), `V3X_TRUST=dev` (developers only, see below), plus the per-plugin ones in the table below.

### Writing `v3x.ini` from a plugin or tool

Several writers share `v3x.ini` (`mod_menu`, `doctor fix`, the updater, your tool). Change it the safe
way ([safe-coding.md](safe-coding.md#file-io)):

1. take the shared lock: `HANDLE lk = v3x_config_lock(5000);` (`sdk/v3x_util.h`; NULL = timeout, do not
   write);
2. read the current file, change only your lines, keep comments;
3. write it with `v3x_file_write_atomic` (temp file + rename: a crash never leaves half a file);
4. `v3x_config_unlock(lk)`.

Your plugin's own files: build their paths with `v3x_plugin_file(api, "my_plugin", ".ini", ...)` or
`v3x_path(api, ...)`, never by pasting strings onto `api->v3x_dir()`. A file or folder name that comes
from data (a mod, a cache, another ini) goes through
`v3x_path_join_checked(out, cap, dir, name, V3X_PATH_SINGLE | V3X_PATH_CHECK_REPARSE)` (no `..`, drive
letters, device names or reparse points).

## Plugin trust and the allowlist

Since loader 0.6.0 a plugin DLL in `binaries/v3x/plugins/` is loaded only if it is trusted
([ADR 0002](adr/0002-trust-model.md)). The loader decides from the file, before loading it, so a
refused DLL never runs.

| trust (`plugins` shows) | how a DLL gets it |
|---|---|
| `official` | listed with its SHA-256 in `binaries/v3x/plugins.json`, and `binaries/v3x/plugins.json.sig` is a valid V3X release signature over that file. `V3X-Setup.exe` installs both for the bundled plugins. Do not edit `plugins.json`: any change breaks the signature and the whole list is ignored |
| `trusted (allowlist)` | its SHA-256 is in your per-user file `%LOCALAPPDATA%\V3X\trusted_plugins.txt` |
| `dev (unchecked)` | the game process runs with the environment variable `V3X_TRUST=dev` |

**Allowing a plugin** you got from someone you trust: start the game once with it in the plugins
folder; the console says how many plugins were not loaded. Type `trust` to list them with their hashes,
then `trust <plugin>` (for example `trust my_plugin`). It applies at the next start. `trust` writes
exactly the hash of the file that was refused, so a changed or rebuilt DLL needs a new `trust`.

`trusted_plugins.txt`, one plugin per line (the name is a note for you; only the hash counts):

```text
# lines starting with # are comments
3f5a...e9c1 my_plugin.dll
```

**Removing trust:** delete the plugin's line (or the whole file). A plugin is a program with the
game's rights: trust only plugins whose author you trust. Trust is not a sandbox.

> **Unsafe — when you really need it:** `V3X_TRUST=dev`.
> * When: you develop plugins and rebuild them many times
>   ([getting-started.md](getting-started.md#4-run-it-2-minutes)).
> * What can go wrong: every DLL in the plugins folder loads unchecked in that game process, also one
>   another program put there.
> * How to contain it: set it only for the game process you start for testing, not as a permanent
>   user variable on a machine you also play on; `v3x.log` logs a WARNING at every start while it is on.

## Plugins

Each plugin reads `binaries/v3x/<plugin>.ini`; every key is optional and a missing file means the
defaults below. Where a `plugins/<p>/<p>.ini.example` exists it holds the defaults with comments.

| plugin | keys (default) |
|---|---|
| `devtools` | `profile=0` (or env `V3X_PROFILE=1`; Ctrl+hotkey toggles), `interval_sec=10`, `hotkey_report=F10`, `sample_hz=1000`, `sample_threads=4`, `unwind_depth=8` |
| `devbridge` | `enabled=1`, `pipe=v3x_devbridge` (env `V3X_DEVBRIDGE_PIPE` wins), `allow=` (default: the read-only list below; `allow = *` runs every command). See [devbridge](#devbridge-token-and-allow-list) |
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

`hello`, `ironman_console`, `crash_logger`, `devconsole`, `content_packs`, `tick_profiler` and
`doctor` have no ini. `doctor` only writes ini files when you type `doctor fix <id>` (the first
original is kept as `<file>.doctor-bak`; see [doctor.md](doctor.md)).

### updater: `[update]` in `v3x.ini`

```ini
[update]
check = 1         ; automatic checks for a new release (0 = only the console command `update check`)
auto = 1          ; install a compatible SIGNED update when the game exits (0 = notify only; `update now`)
channel = stable  ; beta = pre-releases too
```

Unsigned releases are only announced, never installed automatically. Env `V3X_NO_UPDATE=1` turns off
network use and installing completely. The installer ships `updater` off (`installer/profile.txt`);
details in [updating.md](updating.md).

### devbridge: token and allow list

`devbridge` serves V3X console commands to a local tool (the MCP server `mcp/v3x_mcp.py`) over the
named pipe `\\.\pipe\v3x_devbridge`. Two things limit what a connecting program can do:

* **Session token**: at every game start the plugin writes a new random token to
  `binaries/v3x/devbridge.token` (readable only by your Windows user). The first line of every
  connection must be `auth <token>`, else the connection is closed. `mcp/v3x_mcp.py` reads the file by
  itself; after a game restart the old token no longer works. No token file = no pipe.
* **Allow list**: `allow =` in `binaries/v3x/devbridge.ini`, comma separated; each entry is the leading
  word(s) of a command line, `name$` = only the bare command without arguments. A command that is
  not allowed is answered with `END -4`. Without an `allow` key the default is read-only inspection:

  ```ini
  allow = help, version, plugins, mods, dis, fn, xref, mem, scan, memstat, profile report, ticks$, doctor$
  ```

  `allow = *` (opt-in) runs every console command, including ones that change the game or write
  files. Only set it when you trust every program that runs as your Windows user.

