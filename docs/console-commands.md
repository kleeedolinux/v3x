# Console commands and events

## Console commands

Type them into the V3X console window (`help` lists everything that is registered). Commands run
on the console input thread, not the game's main thread.

| command | from | what it does |
|---|---|---|
| `help [command]` | core | list commands |
| `plugins` | core | plugin DLLs and their load status |
| `log <text>` | core | write a line to `v3x.log` |
| `version` | core | loader API version, game version, exe base |
| `quit`, `exit` | core | does not quit; says how to close the game |
| `dis <addr> [count]` | devconsole | disassemble (default 20, max 500 instructions) |
| `fn <addr> [max]` | devconsole | the whole function containing addr (bounds from `.pdata`) |
| `xref <addr\|"string"> [max]` | devconsole | rip-relative `lea`/`mov` in the game's `.text` that reference addr or the exact string |
| `scan [-n max] <ida pattern>` | devconsole | all matches in the game's `.text` (default max 20), e.g. `scan 48 8B 05 ?? ?? ?? ?? 48 85 C0` |
| `mem <addr> [len]` | devconsole | hex + ASCII dump (default 128, max 4096 bytes) |
| `mods [filter]` | devconsole | loaded modules: base, size, name |
| `profile on\|off\|report\|reset` | devtools | sampling profiler |
| `packs` | content_packs | content packs and their mount state |
| `modmenu` | mod_menu | plugin list (now / next start); `modmenu toggle <plugin>` flips one for the next start |
| `ticks [reset]` | tick_profiler | tick task profile |
| `simopt [reset \| throttle on\|off]` | sim_opt | diag / trades report, reset, switch throttling |
| `memcollect` | mem_tuner | return free mimalloc memory to Windows (`mi_collect(true)`) |
| `memstat` | mem_tuner | commit census (mimalloc segments vs other) + periodic collect stats |
| `allocs [top N]` | alloc_tracker | top allocation call sites by live and total bytes |
| `cpusched` | cpu_sched | P/E core map and each worker's CPU set |
| `frame_pacer` | frame_pacer | frame pacer status |
| `assetstream` | asset_stream | texture streamer forcing status |
| `lazymesh [status \| dump \| load <type>]` | lazy_mesh | status, write `v3x/lazy_mesh_used.txt`, load one type |
| `mapcache [status \| report \| flush]` | map_cache | status, write `v3x/map_cache_report.txt`, release pins |
| `enhance` | enhance | which enhancements are active |
| `speed [<1-4> <seconds per tick>]` | enhance | show / set the game-speed table (not saved to the ini) |
| `lean [on\|off]` | enhance | debug_mode without its debug-only GUI / tooltips |
| `fast5 [on\|off]` | enhance | post the next speed-5 tick when the previous one ends |

devconsole addresses can be written as hex (`0x7ff6...`, WinDbg backticks allowed), `+rva` in
the game exe (`+0x3e17820`), or `module+offset` exactly as the commands print them
(`victoria3.exe+0x3e17820`). Game memory is only read through `ReadProcessMemory` after a
`VirtualQuery` check, so a bad address prints an error instead of crashing the game.


## Events between plugins

Named events between plugins (API v3, `on_event` / `emit_event`; list in `include/v3x.h`):

| event | emitter, thread | data | subscribers |
|---|---|---|---|
| `load_transition` | fast_load, main, when a load-state transition ends | `const V3X_LoadTransition *` {`from`, `to`, `seconds`} (may be NULL) | alloc_tracker, devtools, lazy_mesh, map_cache, mem_tuner, sim_opt, tick_profiler |
| `load_transition_begin` | fast_load, main, when a transition begins | `const V3X_LoadTransition *` (`seconds` = 0; may be NULL) | devtools |
| `save_begin` | enhance, the saving thread (main), right before `SaveGameState` | `const V3X_SaveBegin *` {`name`, may be NULL} | sim_opt |
| `save_end` | enhance, same thread, right after that save returned | the same `const V3X_SaveBegin *` | - |
| `process_exit` | core (API v4), the thread calling `ExitProcess`, once, while all threads are still alive | - | devtools and plugins that write final reports |
| `plugin_disabled` | core (API v6), the thread that disabled a plugin (fault guard, rollback), synchronously | `const V3X_PluginDisabled *` {`struct_size`, `name`, `module`, `reason`} | script_ext and anything that calls plugin code itself |

Event data belongs to the emitter and is valid only during the callback; it may be NULL; payloads
from API v6 on start with `struct_size`. A subscriber that checks the pointer and the size, and copies what it
keeps:

<!-- sample: file | static const V3X_API *api; -->
```c
static char g_last_state[32];   /* ours, sized: the event's strings die after the callback */

static void on_transition(const char *name, void *data, void *user)   /* main thread */
{
    (void)name; (void)user;
    const V3X_LoadTransition *t = (const V3X_LoadTransition *)data;
    if (!t || !t->to) return;                     /* data may be NULL */
    V3xStr s;
    v3x_str_init(&s, g_last_state, sizeof g_last_state);
    (void)v3x_str_append(&s, t->to);              /* copied, cut to fit */
}

static void on_disabled(const char *name, void *data, void *user)     /* v6 */
{
    (void)name; (void)user;
    const V3X_PluginDisabled *d = (const V3X_PluginDisabled *)data;
    if (!d || d->struct_size < sizeof *d) return; /* read only what the emitter's struct has */
    char clean[64];
    v3x_sanitize_line(clean, sizeof clean, d->name ? d->name : "?");
    V3X_LOGI(api, "my_plugin", "plugin %s was disabled: stop calling into it", clean);
}

static void subscribe(void)
{
    if (api->on_event(V3X_EVENT_LOAD_TRANSITION, on_transition, NULL) != V3X_OK)
        V3X_LOGW(api, "my_plugin", "load_transition not subscribed");
    if (V3X_HAS(api, off_event) /* v6 loader */ && api->on_event(V3X_EVENT_PLUGIN_DISABLED, on_disabled, NULL) != V3X_OK)
        V3X_LOGW(api, "my_plugin", "plugin_disabled not subscribed");
}
```

Details: [api-reference.md](api-reference.md), [safe-coding.md](safe-coding.md#ownership-and-lifetimes).
