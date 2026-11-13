# Building and testing

## Build

```sh
pip install ziglang                 # the compiler (no Visual Studio needed)
./build.sh                          # build/version.dll + build/plugins/*.dll
./build.sh install                  # ... and copy them into ../binaries (the game)
python sdk/build_plugin.py plugins/<name> [--test] [--install]   # one plugin
python installer/make_dist.py       # build + build/dist/V3X-Setup.exe (player installer, docs/install.md)
```

## Repository layout

```
include/v3x.h        public plugin API (C ABI, versioned, append-only)
include/v3x_rtti.h   MSVC RTTI lookup helpers for plugins
sdk/*.h              header-only helpers: v3x_sdk.h (all), mem, scan, anchor, ini, hook, game, util, script, testkit
sdk/build_plugin.py  one-command build / offline test / install / check of a plugin
templates/           plugin_template: minimal plugin + ini + offline test
src/proxy.c          version.dll export forwarding
src/core.c           boot, plugin loader, scanning, hooks, frame tick, console, events, manifests
src/safe_hook.c      the pass-through thunk behind api->hook_safe (API v4)
plugins/*            the bundled plugins (docs/plugins.md)
installer/           player installer V3X-Setup.exe (setup.c, make_dist.py, profile.txt, test_installer.py)
test/*.c, test/*.py  tests without the game (this page)
tools/*.py           reverse-engineering helpers (docs/reverse-engineering.md)
mcp/v3x_mcp.py       MCP server for the devbridge pipe
docs/*.md            documentation (docs/README.md); docs/re/*.md reverse-engineering notes
third_party/minhook  inline hooking (BSD-2), git submodule
third_party/zydis    x86 disassembler, amalgamated (MIT); used by devconsole
```

## Test runner

There is no CI. A single runner `test/run_all.sh` is being written by the sdk worker (2026-11-13; the
file does not exist yet at the time of writing). `test/api_v5_test.c`, `test/compat_test.c` (mode
`old`) and `test/compat_check.py` say in their headers that `run_all.sh` builds their setup. Until it
lands, run each test by hand as below.

## Setup (once per shell)

Pick a scratch directory with free space (`S`); the compiler's temp and cache directories can
go there too (several GB over time). From `v3x/`:

```sh
S=${V3X_TMP:-$TEMP/v3x}                      # e.g. V3X_TMP=D:/v3x_tmp when C: is short on space
export TEMP=$S TMP=$S ZIG_GLOBAL_CACHE_DIR=$S/zcache ZIG_LOCAL_CACHE_DIR=$S/zcache
CC="python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude"
M=third_party/minhook; MH="$M/src/buffer.c $M/src/hook.c $M/src/trampoline.c $M/src/hde/hde64.c"
EXE=../binaries/victoria3.exe; T=$S/t; mkdir -p $T
./build.sh                                   # build/version.dll + build/plugins/*.dll
```

Some older tests write fixed scratch folders under `D:/v3x_tmp/` (alloc_tracker, console_plus,
fast_load, lazy_gfx, lazy_mesh, map_cache, mod_loader: see their header comments); they need a `D:`
drive until they are converted.

Every test prints one line per check and `ALL OK` at the end (exit code 0). Arguments in `[ ]`
are optional; with `$EXE` a test maps the real exe as an image (no game code runs) and checks the
plugin's anchors against it. The build line of each test is copied from its header comment
(`test/<file>`, line 1 on), which has the full details.

## Every test

| test file | covers | kind |
|---|---|---|
| `test/abi_test.c` | ABI freeze of `include/v3x.h` (struct layouts, offsets, constants) | compile-time + unit |
| `test/hook_safe_test.c` | `src/safe_hook.c`: 0-16 args, floats, struct return, recursion, threads, unwind, registers | unit |
| `test/hook_chain_test.c` | several `hook_safe` subscribers on one target, exclusive raw hooks, per-owner unhook | unit |
| `test/sdk_test.c` | `sdk/` helpers (ini, mem, scan, anchor, hook, manifest, util); script-RNG anchor with `$EXE` | unit |
| `test/rtti_test.c` | `include/v3x_rtti.h` on fake RTTI; with `$EXE`, the CTickTask_ count | unit |
| `test/harness.c` | loader end to end + `ironman_console` | e2e |
| `test/api_v4_test.c` + `test/v4_plugin.c` | API v4 via the loader: hook_safe, ids, enum_commands, manifests, `[plugins]` skip list | e2e |
| `test/api_v5_test.c` + `test/api_v5_plugin.c` | API v5 + loader hardening: log levels, `reload`, fault isolation, rollback, crash-loop marker, build gate | e2e (in progress, sdk worker) |
| `test/compat_test.c` + `test/compat_plugin.c` + `test/compat_v1..v4.h` | plugins built for API v1-v5 load on the current loader; v5 plugin degrades on a v4 loader | e2e |
| `test/compat_check.py` | `sdk/build_plugin.py check` (DLL validation without loading) | python |
| `test/shutdown_test.c` + `test/shutdown_probe.c` | every plugin lets the process exit within 10 s, 20 runs x 2 modes | e2e |
| `test/crash_test.c` | crash_logger: C++ throws, AV, abort, stack overflow | e2e |
| `test/devconsole_test.c` | devconsole commands, fault-proof reads, `exec_capture` | e2e |
| `test/devbridge_test.c` | devbridge pipe protocol | e2e |
| `test/test_mcp.py` | `mcp/v3x_mcp.py` against `devbridge_test.exe --serve N` | python |
| `test/devtools_test.c` | devtools thread / profiler / memory reports, per-phase CSVs | e2e |
| `test/ai_par_test.c` | ai_par logic, RRP hook + lever, anchors on `$EXE` | offline |
| `test/alloc_tracker_test.c` | alloc_tracker tables, sampler; built DLL against `$EXE` | offline |
| `test/asset_stream_test.c` | asset_stream config, anchors, detour end to end | offline |
| `test/console_plus_test.c` | console_plus completion logic, fake registry, anchors, console-window Tab | offline |
| `test/content_packs_test.c` | content_packs decoders, scan / order / mount logic | offline |
| `test/cpu_sched_test.c` | cpu_sched P/E classification, anchors, built DLL detours | offline |
| `test/enhance_test.c` | enhance anchors, patches, `speed`, save events, withdrawn `fast_speed5` | offline |
| `test/fast_load_test.c` | fast_load fl_core + mesh thunk stub (MinHook) | offline |
| `test/fast_lookup_test.c` | fast_lookup caches vs the real engine code on `$EXE`, hooked = unhooked | offline |
| `test/frame_pacer_test.c` | frame_pacer config, policy, anchors | offline |
| `test/gfx_driver_test.c` | gfx_driver ini, retry path, IAT slot in `$EXE` | offline |
| `test/keep_history_test.c` | keep_history: 11 patch sites + 2 record_all hooks on `$EXE` | offline (needs `$EXE`) |
| `test/lazy_gfx_test.c` | lazy_gfx stages, manifest, prefetch, png_cache, 7 hook_safe calls on `$EXE` | offline |
| `test/lazy_mesh_test.c` | lazy_mesh table, `lm_ensure`, hooks against fake engine, 6 anchors | offline |
| `test/loc_fallback_test.c` | loc_fallback parser, missing-key computation, file generation | offline |
| `test/log_filter_test.c` | log_filter rule engine | offline |
| `test/map_cache_test.c` | map_cache pin table, detours vs a fake weak texture cache, 3 anchors | offline |
| `test/mem_tuner_test.c` | mem_tuner option table walk, patterns, built DLL against `$EXE` | offline |
| `test/mod_loader_test.c` | mod_loader index, predictions, `mode=on` skips, verify sampling | offline |
| `test/mod_menu_test.c` | mod_menu ini rewrite, GUI generation, registrar detour, anchors | offline |
| `test/par_load_test.c` | par_load thunks, db_loop patching, ordered fork/join determinism, anchors | offline |
| `test/script_ext_test.c` | script_ext pack writing, BOM, manifest cleanup, RE anchors of docs/re/script.md | offline |
| `test/sim_opt_test.c` | sim_opt anchors, diag, throttle, diag_trades / diag_journal, 6-arg pass-through | offline |
| `test/tick_par_test.c` | tick_par scheduling logic, stub + call-site patch, `mode=on` refused | offline |
| `test/tick_profiler_test.c` | tick_profiler detour, report, CSV, process exit | offline |
| `installer/test_installer.py` | `V3X-Setup.exe` end to end on fake game folders | python |
| `templates/plugin_template/test_my_plugin.c` | the template plugin (via `build_plugin.py --test`) | offline |

`hello` has no test of its own (`test/api_v4_test.c` loads `hello.dll` to test the skip list).

## Core, API and SDK

```sh
$CC -o $T/abi.exe test/abi_test.c && $T/abi.exe
$CC -I$M/include -o $T/hst.exe test/hook_safe_test.c $MH && $T/hst.exe
$CC -I$M/include -o $T/hct.exe test/hook_chain_test.c $MH && $T/hct.exe
$CC -Isdk -Wno-unused-function -o $T/sdkt.exe test/sdk_test.c && $T/sdkt.exe [$EXE]
$CC -o $T/rtti.exe test/rtti_test.c && $T/rtti.exe [$EXE <expected CTickTask_ count>]
python sdk/build_plugin.py templates/plugin_template --out $T/tpl --test [$EXE]
```

## Plugin offline tests

They `#include` the plugin source or load the built DLL against a fake `V3X_API`.

```sh
$CC -Isdk -o $T/aipar.exe test/ai_par_test.c && $T/aipar.exe [$EXE $T/]
$CC -o $T/att.exe test/alloc_tracker_test.c && $T/att.exe [$EXE build/plugins/alloc_tracker.dll]
$CC -o $T/ast.exe test/asset_stream_test.c && $T/ast.exe plugins/asset_stream/asset_stream.ini.example [$EXE [$T]]
$CC -o $T/cplt.exe test/console_plus_test.c && $T/cplt.exe [$EXE]
mkdir -p $T/cp && $CC -o $T/cpt.exe test/content_packs_test.c && $T/cpt.exe $T/cp
$CC -o $T/cst.exe test/cpu_sched_test.c && $T/cst.exe [$EXE|- [build/plugins/cpu_sched.dll]]
mkdir -p $T/enh && $CC -o $T/enht.exe test/enhance_test.c && $T/enht.exe $T/enh [$EXE]
$CC -I$M/include -o $T/flt.exe test/fast_load_test.c $MH && $T/flt.exe [$EXE]
$CC -I$M/include -o $T/fkt.exe test/fast_lookup_test.c $MH && $T/fkt.exe [$EXE [$T]]
$CC -o $T/fpt.exe test/frame_pacer_test.c && $T/fpt.exe plugins/frame_pacer/frame_pacer.ini.example [$EXE]
$CC -o $T/gdt.exe test/gfx_driver_test.c && $T/gdt.exe plugins/gfx_driver/gfx_driver.ini.example [$EXE]
$CC -o $T/kht.exe test/keep_history_test.c && $T/kht.exe $EXE build/plugins/keep_history.dll
$CC -Isdk -o $T/lgt.exe test/lazy_gfx_test.c && $T/lgt.exe [$EXE]           # writes D:/v3x_tmp/lazygfx/t*
$CC -o $T/lmt.exe test/lazy_mesh_test.c && $T/lmt.exe [$EXE]                # writes D:/v3x_tmp/lazymesh/t/
mkdir -p $T/lf && $CC -o $T/lft2.exe test/loc_fallback_test.c && $T/lft2.exe $T/lf [<game root> <docs dir> [<lang> [build/plugins/loc_fallback.dll]]]
$CC -o $T/lft.exe test/log_filter_test.c && $T/lft.exe plugins/log_filter/log_filter.ini.example
$CC -DMC_TEST -o $T/mct.exe test/map_cache_test.c && $T/mct.exe [$EXE]      # writes D:/v3x_tmp/mapmode/t/
$CC -o $T/mtt.exe test/mem_tuner_test.c && $T/mtt.exe [$EXE [build/plugins/mem_tuner.dll [api_version]]]
$CC -o $T/mlt.exe test/mod_loader_test.c && $T/mlt.exe [$EXE]               # writes D:/v3x_tmp/modload/t
mkdir -p $T/mm && $CC -o $T/mmt.exe test/mod_menu_test.c && $T/mmt.exe $T/mm [../game/gui/frontend/frontend_main.gui [$EXE]]
$CC -Isdk -o $T/plt.exe test/par_load_test.c && $T/plt.exe [$EXE]
mkdir -p $T/sx && $CC -Isdk -o $T/sxt.exe test/script_ext_test.c && $T/sxt.exe $T/sx [$EXE]
$CC -o $T/sot.exe test/sim_opt_test.c && $T/sot.exe
$CC -o $T/tpar.exe test/tick_par_test.c && $T/tpar.exe
$CC -o $T/tpt.exe test/tick_profiler_test.c && $T/tpt.exe
```

Notes: the header of `loc_fallback_test.c` has only the usage line; the build line above was
checked on 2026-11-13 (ALL OK without the optional arguments). `loc_fallback_test.c`,
`mod_menu_test.c` and `script_ext_test.c` want an *empty* temp dir.

## End-to-end tests

They link `build/version.dll` like `victoria3.exe` does, so V3X boots and loads
`<dir>/v3x/plugins/*.dll` before `main`. `e2e <test> <plugins...>` builds one into `$T/<test>/`
with only the plugins it needs:

```sh
e2e() { D=$T/$1; mkdir -p $D/v3x/plugins && cp build/version.dll $D/ && $CC -o $D/$1.exe test/$1.c -Lbuild -l:version.dll -luser32 && shift && for p in "$@"; do cp build/plugins/$p.dll $D/v3x/plugins/; done; }
e2e harness ironman_console && (cd $D && V3X_NO_CONSOLE=1 ./harness.exe)                 # loader + ironman_console
e2e devconsole_test devconsole && (cd $D && V3X_NO_CONSOLE=1 ./devconsole_test.exe)      # commands, fault-proof reads, exec_capture
e2e devbridge_test devconsole devbridge && (cd $D && V3X_NO_CONSOLE=1 V3X_DEVBRIDGE_PIPE=v3x_devbridge_test ./devbridge_test.exe)  # private pipe required
python test/test_mcp.py $T/devbridge_test/devbridge_test.exe                               # MCP server against devbridge_test --serve
e2e devtools_test devtools && echo interval_sec=2 > $D/v3x/devtools.ini && (cd $D && V3X_NO_CONSOLE=1 V3X_PROFILE=1 ./devtools_test.exe)
e2e crash_test crash_logger && (cd $D && V3X_NO_CONSOLE=1 ./crash_test.exe)               # runs copies of itself in child_<mode>/
```

API v4 (hook_safe through the loader, main_thread_id, game_build_id, enum_commands, manifests incl.
min_api / REQUIRE_BUILD refusals, the `[plugins]` skip list), from `test/api_v4_test.c`:

```sh
D=$T/api_v4_test; mkdir -p $D/v3x/plugins && cp build/version.dll $D/
$CC -shared -o $D/v3x/plugins/v4_ok.dll test/v4_plugin.c
$CC -shared -DV4_REQUIRE -o $D/v3x/plugins/v4_require.dll test/v4_plugin.c
$CC -shared -DV4_MINAPI=99 -o $D/v3x/plugins/v4_minapi.dll test/v4_plugin.c
$CC -shared -DV4_WARN -o $D/v3x/plugins/v4_warn.dll test/v4_plugin.c
cp build/plugins/hello.dll $D/v3x/plugins/ && printf '[plugins]\nHello = 0\nv4_ok=1\n' > $D/v3x/v3x.ini
$CC -o $D/api_v4_test.exe test/api_v4_test.c -Lbuild -l:version.dll && (cd $D && V3X_NO_CONSOLE=1 ./api_v4_test.exe)
python test/compat_check.py $T                 # needs $T/api_v4_test from above
```

API v5 and loader hardening (`test/api_v5_test.c`, modes: none, `gate`, `gateload`). Its plugin
variants are `-DV5_FRAME_FAULT`, `-DV5_CMD_FAULT`, `-DV5_EVENT_FAULT`, `-DV5_MAIN_FAULT`,
`-DV5_LOAD_FAULT`, `-DV5_LOAD_FAIL`, `-DV5_RELOAD`, `-DV5_MARKED`, `-DV5_GATE_STAR/_VER/_PREFIX/_NONE`
of `test/api_v5_plugin.c`, each built as `v5_<variant>.dll`; the directory also needs a `v3x.ini`,
`loading.marker` and fake `crash.log` (header of `test/api_v5_test.c`). The exact setup is in
`test/run_all.sh` (not written yet), so no hand recipe is given here.

Compatibility (`test/compat_test.c`; mode `old` needs a v4 loader built from git `317e574`, which
`run_all.sh` will build):

```sh
D=$T/compat; mkdir -p $D/v3x/plugins && cp build/version.dll $D/
for v in 1 2 3 4 5; do $CC -shared -DCOMPAT_V=$v -o $D/v3x/plugins/compat_v$v.dll test/compat_plugin.c; done
$CC -o $D/compat_test.exe test/compat_test.c -Lbuild -l:version.dll -luser32 && (cd $D && V3X_NO_CONSOLE=1 ./compat_test.exe)
```

Process exit: every plugin plus the test-only `test/shutdown_probe.c`; the runner starts the
child 20 times, each must exit within 10 s with code 42 after `ExitProcess` while other threads
hammer the log lock, heap and CRT (`con` = own hidden console window with the input thread
reading, `nocon` = `V3X_NO_CONSOLE=1`):

```sh
D=$T/shutdown && mkdir -p $D/v3x/plugins && cp build/version.dll $D/ && cp build/plugins/*.dll $D/v3x/plugins/
$CC -shared -o $D/v3x/plugins/shutdown_probe.dll test/shutdown_probe.c
$CC -o $D/shutdown_child.exe test/shutdown_test.c -Lbuild -l:version.dll -luser32
$CC -DSHUTDOWN_RUNNER -o $D/shutdown_test.exe test/shutdown_test.c
$D/shutdown_test.exe $D/shutdown_child.exe 20 con && $D/shutdown_test.exe $D/shutdown_child.exe 20 nocon
```

## Installer

`installer/test_installer.py` runs `V3X-Setup.exe` (GUI-less `--silent` CLI) against fake game folders
(`<root>/<case>/Victoria 3/` with the first 4 KB of the real exe, so the build id is real); it never
touches the real game. `<root>` is deleted at the end unless `--keep`.

```sh
python installer/make_dist.py                  # builds build/dist/V3X-Setup.exe
python installer/test_installer.py [--exe build/dist/V3X-Setup.exe] [--root D:/v3x_installer_test/run] \
                                   [--real-exe $EXE] [--keep]
```

Last reported result: 49/49 (installer worker, team chat #4374, 2026-11-13; not re-run for this page).
