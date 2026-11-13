# Building and testing

## Build

```sh
pip install ziglang                 # the compiler (no Visual Studio needed)
./build.sh                          # build/version.dll + build/plugins/*.dll
./build.sh install                  # ... and copy them into ../binaries (the game)
python sdk/build_plugin.py plugins/<name> [--test] [--install]   # one plugin
```

## Repository layout

```
include/v3x.h        public plugin API (C ABI, versioned, append-only)
include/v3x_rtti.h   MSVC RTTI lookup helpers for plugins
sdk/*.h              header-only helpers: v3x_sdk.h (all), mem, scan, anchor, ini, hook, game, testkit
sdk/build_plugin.py  one-command build / offline test / install of a plugin
templates/           plugin_template: minimal plugin + ini + offline test
src/proxy.c          version.dll export forwarding
src/core.c           boot, plugin loader, scanning, hooks, frame tick, console, events, manifests
src/safe_hook.c      the pass-through thunk behind api->hook_safe (API v4)
plugins/*            the bundled plugins (docs/plugins.md)
test/*.c             tests without the game (see below)
tools/*.py           reverse-engineering helpers (docs/reverse-engineering.md)
mcp/v3x_mcp.py       MCP server for the devbridge pipe
docs/*.md            documentation (docs/README.md); docs/re/*.md reverse-engineering notes
third_party/minhook  inline hooking (BSD-2), git submodule
third_party/zydis    x86 disassembler, amalgamated (MIT); used by devconsole
```


## Tests (no game needed)

Pick a scratch directory with free space (`S`); the compiler's temp and cache directories can
go there too (several GB over time). Run this once per shell, from `v3x/`:

```sh
S=${V3X_TMP:-$TEMP/v3x}                      # e.g. V3X_TMP=D:/v3x_tmp when C: is short on space
export TEMP=$S TMP=$S ZIG_GLOBAL_CACHE_DIR=$S/zcache ZIG_LOCAL_CACHE_DIR=$S/zcache
CC="python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude"
M=third_party/minhook; EXE=../binaries/victoria3.exe; T=$S/t; mkdir -p $T
./build.sh                                   # build/version.dll + build/plugins/*.dll
```

Some older tests still write fixed scratch folders under `D:/v3x_tmp/` (alloc_tracker,
console_plus, fast_load, lazy_mesh, map_cache, mod_loader); they need a `D:` drive until they are
converted.

Every test prints one line per check and `ALL OK` at the end (exit code 0). Arguments in `[ ]`
are optional; with `$EXE` a test maps the real exe as an image (no game code runs) and checks the
plugin's anchors against it. Each file's header comment has the full details.

Core and SDK (the hook_safe thunk with 0-16 args, floats, returns, recursion, threads, stack walk;
the SDK helpers incl. the script-RNG anchor against the real exe; the template plugin):

```sh
$CC -I$M/include -o $T/hst.exe test/hook_safe_test.c $M/src/buffer.c $M/src/hook.c $M/src/trampoline.c $M/src/hde/hde64.c && $T/hst.exe
$CC -Isdk -Wno-unused-function -o $T/sdkt.exe test/sdk_test.c && $T/sdkt.exe [$EXE]
python sdk/build_plugin.py templates/plugin_template --out $T/tpl --test [$EXE]
```

Offline tests (one line each; they `#include` the plugin source or load the built DLL against a
fake `V3X_API`):

```sh
$CC -o $T/att.exe test/alloc_tracker_test.c && $T/att.exe [$EXE build/plugins/alloc_tracker.dll]
$CC -o $T/ast.exe test/asset_stream_test.c && $T/ast.exe plugins/asset_stream/asset_stream.ini.example [$EXE [$T]]
mkdir -p $T/cp && $CC -o $T/cpt.exe test/content_packs_test.c && $T/cpt.exe $T/cp
mkdir -p $T/mm && $CC -o $T/mmt.exe test/mod_menu_test.c && $T/mmt.exe $T/mm [../game/gui/frontend/frontend_main.gui [$EXE]]
$CC -o $T/cst.exe test/cpu_sched_test.c && $T/cst.exe [$EXE|- [build/plugins/cpu_sched.dll]]
mkdir -p $T/enh && $CC -o $T/enht.exe test/enhance_test.c && $T/enht.exe $T/enh [$EXE]
$CC -I$M/include -o $T/flt.exe test/fast_load_test.c $M/src/buffer.c $M/src/hook.c $M/src/trampoline.c $M/src/hde/hde64.c && $T/flt.exe [$EXE]
$CC -o $T/fpt.exe test/frame_pacer_test.c && $T/fpt.exe plugins/frame_pacer/frame_pacer.ini.example [$EXE]
$CC -o $T/gdt.exe test/gfx_driver_test.c && $T/gdt.exe plugins/gfx_driver/gfx_driver.ini.example [$EXE]
$CC -o $T/kht.exe test/keep_history_test.c && $T/kht.exe $EXE build/plugins/keep_history.dll
$CC -o $T/lmt.exe test/lazy_mesh_test.c && $T/lmt.exe [$EXE]                # writes D:/v3x_tmp/lazymesh/t/
$CC -o $T/lft.exe test/log_filter_test.c && $T/lft.exe plugins/log_filter/log_filter.ini.example
$CC -DMC_TEST -o $T/mct.exe test/map_cache_test.c && $T/mct.exe [$EXE]      # writes D:/v3x_tmp/mapmode/t/
$CC -o $T/mtt.exe test/mem_tuner_test.c && $T/mtt.exe [$EXE [build/plugins/mem_tuner.dll [api_version]]]
$CC -o $T/rtti.exe test/rtti_test.c && $T/rtti.exe [$EXE <expected CTickTask_ count>]
$CC -o $T/sot.exe test/sim_opt_test.c && $T/sot.exe
$CC -o $T/cplt.exe test/console_plus_test.c && $T/cplt.exe [$EXE]
$CC -o $T/tpar.exe test/tick_par_test.c && $T/tpar.exe
$CC -o $T/tpt.exe test/tick_profiler_test.c && $T/tpt.exe
```

End-to-end tests link `build/version.dll` like `victoria3.exe` does, so V3X boots and loads
`<dir>/v3x/plugins/*.dll` before `main`. `e2e <test> <plugins...>` builds one into `$T/<test>/`
with only the plugins it needs:

```sh
e2e() { D=$T/$1; mkdir -p $D/v3x/plugins && cp build/version.dll $D/ && $CC -o $D/$1.exe test/$1.c -Lbuild -l:version.dll -luser32 && shift && for p in "$@"; do cp build/plugins/$p.dll $D/v3x/plugins/; done; }
e2e harness ironman_console && (cd $D && V3X_NO_CONSOLE=1 ./harness.exe)                 # loader + ironman_console
e2e devconsole_test devconsole && (cd $D && V3X_NO_CONSOLE=1 ./devconsole_test.exe)      # commands, fault-proof reads, exec_capture
e2e devbridge_test devconsole devbridge && (cd $D && V3X_NO_CONSOLE=1 V3X_DEVBRIDGE_PIPE=v3x_devbridge_test ./devbridge_test.exe)  # private pipe required
e2e devtools_test devtools && echo interval_sec=2 > $D/v3x/devtools.ini && (cd $D && V3X_NO_CONSOLE=1 V3X_PROFILE=1 ./devtools_test.exe)
e2e crash_test crash_logger && (cd $D && V3X_NO_CONSOLE=1 ./crash_test.exe)               # runs copies of itself in child_<mode>/
```

API v4 end-to-end (hook_safe through the loader, main_thread_id, game_build_id, enum_commands,
manifests incl. min_api / REQUIRE_BUILD refusals, the `[plugins]` skip list):

```sh
D=$T/api_v4_test; mkdir -p $D/v3x/plugins && cp build/version.dll $D/
$CC -shared -o $D/v3x/plugins/v4_ok.dll test/v4_plugin.c
$CC -shared -DV4_REQUIRE -o $D/v3x/plugins/v4_require.dll test/v4_plugin.c
$CC -shared -DV4_MINAPI=99 -o $D/v3x/plugins/v4_minapi.dll test/v4_plugin.c
$CC -shared -DV4_WARN -o $D/v3x/plugins/v4_warn.dll test/v4_plugin.c
cp build/plugins/hello.dll $D/v3x/plugins/ && printf '[plugins]\nHello = 0\nv4_ok=1\n' > $D/v3x/v3x.ini
$CC -o $D/api_v4_test.exe test/api_v4_test.c -Lbuild -l:version.dll && (cd $D && V3X_NO_CONSOLE=1 ./api_v4_test.exe)
```

Process-exit test: every plugin plus the test-only `test/shutdown_probe.c`; the runner starts the
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

`test/test_mcp.py` tests the MCP server against `devbridge_test.exe --serve N`.

