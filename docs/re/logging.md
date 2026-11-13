# Engine logging (Victoria 3 1.15.1) and the log_filter plugin

## How the engine logs

The Clausewitz/Jomini engine logs through **spdlog**, version 1.12 or newer (the binary has
RTTI for `spdlog::logger`, the `mdc_formatter` flag formatters, and the `basic_file_sink`,
`dist_sink`, `dup_filter_sink` and `msvc_sink` sinks). The Paradox wrapper is `NLog::CCategoryLogger`
(`pdx_core/log/log.cpp`, `jomini_log.cpp`). It adds its own sinks: `NLog::pdx_rotating_file_sink`,
`PdxTools::CLogViewerSink` (the in-game log viewer in debug mode) and
`NJominiScriptDebugger::CDebuggerLogSink`.

Categories, sinks and patterns are data, not code: `platform_specific_game_data/log_settings_live.json`
(defaults are in `default_log_settings_live.json`, and the `-log-settings` command-line option points
to a custom file). Each category (`cw`, `cw_fs`, `cw_graphics`, `text_warnings`, ...) has sinks such as
`{"type": "rotating_file", "file_name": "error", "log_level": "error", "format_pattern": "[%T][%s:%#]: %v",
"max_rotating_files": 5, "max_rotating_file_size": 524288}`. That pattern is where error.log's
`[16:03:23][jomini_eventmanager.cpp:376]: ...` comes from: `%s` is the source basename and `%#` is the
line. The rotation produces `error.1.log` to `error.5.log` (512 KB each). The JSON parser also knows
`duplication_filter`, spdlog's `dup_filter_sink`, which drops only *consecutive* duplicates. Editing the
JSON is a game-file change that Steam "Verify files" reverts, so the plugin approach is used instead.

Call path: `logger.log(source_loc{__FILE__, __LINE__, func}, level, fmt, args...)` formats the payload
with fmt, builds a `details::log_msg`, and calls `logger::log_it_`, which calls the virtual
`logger::sink_it_(msg)`. That function loops over the sinks: for each one it checks `msg.level >=
sink.level`, then calls `sink->log(msg)`, and the sink applies the pattern formatter and writes the
file. After that it calls `flush_()` if `level >= flush_level`.

### Hook point: `spdlog::logger::sink_it_` (exe+0x43d70f0 in 1.15.1)

Found without hardcoded addresses (`plugins/log_filter/log_filter.c`, `find_vtable`):

| step | 1.15.1 RVA |
|---|---|
| RTTI name `.?AVlogger@spdlog@@`, TypeDescriptor = name - 0x10 | 0x5baa5d8 |
| CompleteObjectLocator in .rdata (sig 1, pTypeDescriptor, pSelf) | 0x5046c90 |
| vtable (qword == COL VA, +8) | 0x463d9b8 |
| slot 2 (`~logger`, `clone`, **`sink_it_`**, `flush_`) | 0x43d70f0 |

Disassembly of `sink_it_`: `sinks_` begin/end are at logger+0x28/+0x30, the loop compares
`[msg+0x10]` (level) with `[sink+8]` and calls `[vtbl+8]` (`sink::log`); `flush_level_` is at
logger+0x44, then a `cmp ecx,6` (level::off) and a call to `[vtbl+0x18]` (`flush_`).

`log_msg` layout (MSVC x64). The offsets were verified from the formatter code: `v_formatter`
reads +0x50/+0x58, `source_linenum_formatter` reads +0x40, `short_filename_formatter` reads +0x38.

| off | field |
|---|---|
| 0x00 | logger_name (string_view) |
| 0x10 | level (0 trace .. 5 critical, 6 off) |
| 0x18 | time (system_clock, 100 ns since 1970) |
| 0x20 | thread_id |
| 0x38 | source.filename (full build path, `C:\mnt\gsg\...\x.cpp`) |
| 0x40 | source.line |
| 0x48 | source.funcname |
| 0x50 | payload.data (formatted text, not NUL-terminated) |
| 0x58 | payload.size |

Why this hook point: the detour sees the source file, line, level and formatted message once per
message, before any sink. Dropping there saves the pattern formatting and the file writes for
*every* sink (error.log and debug.log both). The fmt formatting of the payload has already happened
at that point. Avoiding that cost too would mean hooking each templated `logger::log_` instantiation,
and there are hundreds of them.

The plugin byte-checks `sink_it_` and the three formatters before hooking. If anything differs, it
logs "anchor check failed" and does not hook (fail closed). `tools/re_log_check.py` runs the same
checks offline.

### How errors surface in game

Every sink is fed from `sink_it_`. That covers error.log, debug.log, the in-game log viewer
(`CLogViewerSink`, debug mode), the script debugger sink and the MSVC debug output. A message that
log_filter drops is therefore gone from all of them, which also covers the error notification /
"hoof" counter in debug mode, assuming that is fed by a sink. That last point is NOT verified in game.

## Measured noise (tools/re_log_stats.py on the lead's logs)

These are 7,443 messages (1.3 MB) across error.log and error.1.log to error.5.log. Top locations,
with harmless noise marked N and real problems marked R:

| count | location | class |
|---|---|---|
| 1026 | pdx_persistent_reader.cpp:268 (GUI/script parse errors) | R |
| 846 | jomini_script_system.cpp:247 "Script system error!" | R (mod bugs at runtime) |
| 682 | jomini_effect.cpp:142 PostValidate of effect returned false | R |
| 508+194 | pdx_gui_factory.cpp:275/268 failed reading property/callback | R |
| 498 | pdx_gui_widget_desc.h:79 Duplicate property | R (minor) |
| 320 | jomini_effect.cpp:1140 variable set but never used | **N** |
| 292 | gamedatabase.h:394 Duplicated key | R (override conflict) |
| 254 | jomini_eventmanager.cpp:376 Event is orphaned | **N** |
| 251 | pdx_localize.cpp:199 Duplicate localization key | grey (opt-in rule) |
| 209 | pdx_gui_localize.cpp:182 Unlocalized text | grey (opt-in rule) |
| 172 | lexer.cpp:285 should be utf8-bom (will try anyways) | **N** |
| 82+86+86 | gfx_dx11 shader compile errors | R |
| 12 | dlc.cpp:822 Could not find item in store backend | **N** |
| 8 | modifier_type_definition_database.cpp:43 script-only modifier type | **N** |

Most of the volume comes from real mod bugs (broken GUI files and scripts). Those are never hidden
by default. Dedupe caps each identical message at 3 copies plus a periodic summary line.

Simulated with the default rules (`re_log_stats.py --ini plugins/log_filter/log_filter.ini.example`),
one file per session:

| file | messages kept | bytes kept |
|---|---|---|
| error.log | 37/49 (75%) | 90% |
| error.1.log | 232/691 (34%) | 35% |
| error.2.log | 2232/2344 (95%) | 96% |
| error.3.log | 169/627 (27%) | 57% |
| error.4.log | 1516/2655 (57%) | 64% |
| error.5.log | 998/1077 (93%) | 92% |
| total | 5184/7443 (70%) | 971 KB/1315 KB (74%) |

### `preset = aggressive`

This preset sets `dedupe_first = 1`, `dedupe_template = 1` and `summary_interval = 0`. The first
message of every (file:line, template) pair is logged. Repeats are only counted, in
log_filter_stats.txt. The template (`lf_template` in `lf_rules.c`, mirrored in `re_log_stats.py`;
C and Python give identical output on all 7,443 logged messages) is built as follows:

* whitespace runs become one space,
* each quoted run becomes `Q`,
* each word that contains a digit or one of `_ . / \ :` becomes `*`.

Words in the error text itself are kept, so `[ Invalid building ]` and
`[ Attempting to manually create auto-placed building ]` stay different templates. Templates are never
hidden completely. They can be coarse, though: every `pdx_persistent_reader.cpp:268` parse error maps
to `* Q in * Q near * *`, so only the first broken file is shown.

| file | default: kept msgs / bytes | aggressive: kept msgs / bytes |
|---|---|---|
| error.log | 37/49 (75%) / 90% | 9/49 (18%) / 22% |
| error.1.log | 232/691 (34%) / 35% | 4/691 (0.6%) / 0.6% |
| error.2.log | 2232/2344 (95%) / 96% | 81/2344 (3.5%) / 3.0% |
| error.3.log | 169/627 (27%) / 57% | 43/627 (6.9%) / 26% |
| error.4.log | 1516/2655 (57%) / 64% | 66/2655 (2.5%) / 1.9% |
| error.5.log | 998/1077 (93%) / 92% | 29/1077 (2.7%) / 2.9% |
| total | 5184/7443 (70%) / 74% | 232/7443 (3.1%) / 59 KB of 1315 KB (4.5%) |

Reproduce with `python tools/re_log_stats.py --top 0 --ini plugins/log_filter/log_filter.ini.example
--set "preset = aggressive" <error.N.log>`.

## log_filter configuration

The config file is `binaries/v3x/log_filter.ini`. The template is
`plugins/log_filter/log_filter.ini.example`, which is byte-identical to the built-in defaults; the
test checks that. Options: `preset` (default|aggressive), `enabled`, `dedupe_first` (3), `dedupe_template` (0), `summary_interval` (60 s), `min_level` (0).
Rules: `drop_loc = LOC`, `drop_msg = MSG`, `drop = LOC :: MSG`, and `keep_*` with the same forms.
Globs use `*` and `?`, are case-sensitive and must match the whole string. LOC is `file.cpp:line`.
A keep rule beats any drop rule and exempts the message from dedupe.

Dedupe keys on exact location plus exact text. The first N copies pass. Later copies are counted,
and every `summary_interval` seconds the plugin writes one line per key through the original
`sink_it_`, using the original level, source and logger:
`[log_filter] suppressed K more like this (T total so far): file:line: text`.

Stats go to `binaries/v3x/log_filter_stats.txt`: totals, hits per rule, top locations and most
repeated messages. The file is written every interval, on `load_transition` Game->Empty and on the
core's `process_exit` event (the exiting thread, before ExitProcess kills the other threads).
Nothing is written at DLL_PROCESS_DETACH: by then other threads are dead and may hold the lock or
the heap, and the `sink_it_` detour passes messages straight through (`RtlDllShutdownInProgress`).

## Tests

* `python tools/re_log_check.py ../binaries/victoria3.exe` prints the RVAs and ALL OK.
* `test/log_filter_test.c` covers globs, parsing, the default rules against real error lines, keep
  precedence, dedupe, templates, presets, `min_level`, table-full behaviour and the stats report. The build command is
  in the file header.
