# Configuration reference

Every V3X file lives in `binaries/v3x/` (next to the game exe). Everything is optional: with no
ini files at all you get the defaults below.

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

A missing key means the plugin is loaded. The `mod_menu` plugin (main menu > **V3X**) and the console
command `modmenu toggle <plugin>` edit the `[plugins]` section for you; changes apply at the next start.

Environment variables: `V3X_DISABLE=1` (V3X does nothing for this run), `V3X_NO_CONSOLE=1` (no console
window), plus the per-plugin ones in the table below.

## Plugins

Each plugin reads `binaries/v3x/<plugin>.ini`; every key is optional and a missing file means the
defaults below. Where a `plugins/<p>/<p>.ini.example` exists it holds the defaults with comments.

| plugin | keys (default) |
|---|---|
| `devtools` | `profile=0` (or env `V3X_PROFILE=1`; Ctrl+hotkey toggles), `interval_sec=10`, `hotkey_report=F10`, `sample_hz=1000`, `sample_threads=4`, `unwind_depth=8` |
| `devbridge` | `enabled=1`, `pipe=v3x_devbridge` (env `V3X_DEVBRIDGE_PIPE` wins) |
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

`hello`, `ironman_console`, `crash_logger`, `devconsole`, `content_packs` and `tick_profiler`
have no ini.

