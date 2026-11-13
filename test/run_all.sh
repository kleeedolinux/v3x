#!/usr/bin/env bash
# Builds V3X (./build.sh, no install) and runs every offline test; prints one summary table.
# Exit code = number of failed tests (0 = all passed).
#
#   test/run_all.sh                 everything (the shutdown test runs 2 x 20 child processes, ~1 min)
#   QUICK=1 test/run_all.sh         skip the shutdown test
#   ONLY="abi compat" test/run_all.sh   run only the named tests (names as in the table)
#   V3X_TMP=D:/v3x_tmp              scratch root (default $TEMP/v3x); compiler caches go there too
#
# With ../binaries/victoria3.exe present, tests that can check anchors against the real exe (mapped
# as an image; no game code runs) get it as an argument. Nothing is installed into ../binaries.
set -uo pipefail
cd "$(dirname "$0")/.."
S=${V3X_TMP:-${TEMP:-/tmp}/v3x}
mkdir -p "$S"
export TEMP=$S TMP=$S ZIG_GLOBAL_CACHE_DIR=$S/zcache ZIG_LOCAL_CACHE_DIR=$S/zcache
CC="python -m ziglang cc -target x86_64-windows-gnu -O2 -Iinclude -Isdk"
M=third_party/minhook
MH="$M/src/buffer.c $M/src/hook.c $M/src/trampoline.c $M/src/hde/hde64.c"
T=$S/t
mkdir -p "$T"
EXE=../binaries/victoria3.exe
[[ -f $EXE ]] || EXE=
LOGDIR=$T/run_all_logs
rm -rf "$LOGDIR"; mkdir -p "$LOGDIR"

names=(); results=(); details=()
want() { [[ -z ${ONLY:-} ]] || [[ " $ONLY " == *" $1 "* ]]; }
# t <name> <shell command>: runs it, PASS = exit 0. The detail column is the test's "N checks, M failed" line.
t() {
    local name=$1; shift
    want "$name" || return 0
    local log=$LOGDIR/$name.log
    printf '%-22s ... ' "$name"
    ( eval "$*" ) > "$log" 2>&1
    local rc=$?
    local d
    d=$(grep -aE '[0-9]+ checks?, [0-9]+ failed|RESULT:|test (passed|FAILED)' "$log" | tail -1 | tr -d '\r')
    [[ -z $d ]] && d=$(tail -1 "$log" | tr -d '\r' | cut -c1-70)
    if [[ $rc == 0 ]]; then results+=(PASS); echo PASS; else results+=("FAIL($rc)"); echo "FAIL (exit $rc, log $log)"; fi
    names+=("$name"); details+=("$d")
}
e2e() {   # e2e <dir> <test source> <plugins...>: an exe linked against build/version.dll + those plugins
    local D=$T/$1 src=$2; shift 2
    rm -rf "$D"; mkdir -p "$D/v3x/plugins" && cp build/version.dll "$D/" &&
        $CC -o "$D/$(basename "$src" .c).exe" "$src" -Lbuild -l:version.dll -luser32 || return 1
    for p in "$@"; do cp "build/plugins/$p.dll" "$D/v3x/plugins/" || return 1; done
}

echo "== build"
if ! ./build.sh > "$LOGDIR/build.log" 2>&1; then echo "BUILD FAILED (see $LOGDIR/build.log)"; tail -20 "$LOGDIR/build.log"; exit 100; fi
grep -a 'warning:' "$LOGDIR/build.log" | head -5
echo "== tests (logs in $LOGDIR)"

# ---- ABI / API stability
t abi "$CC -o $T/abi.exe test/abi_test.c && $T/abi.exe"
t abi_negative "mkdir -p $T/abineg && python -c \"
s=open('include/v3x.h').read(); a='    uintptr_t (*image_base)(void);\n'; b='    size_t (*image_size)(void);\n'
assert a+b in s; open('$T/abineg/v3x.h','w').write(s.replace(a+b, b+a))\" &&
  ! python -m ziglang cc -target x86_64-windows-gnu -I$T/abineg -fsyntax-only test/abi_test.c 2>$T/abineg/err.txt &&
  grep -c 'static assertion failed' $T/abineg/err.txt && echo '1 checks, 0 failed (swapped fields are rejected)'"
t cxx_headers "printf '#include \"v3x_sdk.h\"\n#include \"v3x_util.h\"\nint main(){return V3X_API_VERSION==5?0:1;}\n' > $T/cxx.cpp &&
  python -m ziglang c++ -target x86_64-windows-gnu -Iinclude -Isdk -Wno-unused-function -c -o $T/cxx.o $T/cxx.cpp && echo '1 checks, 0 failed (headers compile as C++)'"
t compat "D=$T/compat; rm -rf \$D; mkdir -p \$D/v3x/plugins && cp build/version.dll \$D/ &&
  for v in 1 2 3 4 5; do $CC -shared -DCOMPAT_V=\$v -o \$D/v3x/plugins/compat_v\$v.dll test/compat_plugin.c || exit 1; done &&
  $CC -o \$D/compat_test.exe test/compat_test.c -Lbuild -l:version.dll -luser32 && cd \$D && V3X_NO_CONSOLE=1 ./compat_test.exe"
t compat_old_loader "O=$T/oldloader; rm -rf \$O; mkdir -p \$O/src \$O/include &&
  for f in src/core.c src/proxy.c src/safe_hook.c include/v3x.h; do git show 317e574:\$f > \$O/\$f || exit 1; done &&
  python -m ziglang cc -target x86_64-windows-gnu -O2 -shared -o \$O/version.dll -I$M/include \$O/src/core.c \$O/src/proxy.c \$O/src/safe_hook.c $MH -lkernel32 -luser32 &&
  D=$T/compat_old; rm -rf \$D; mkdir -p \$D/v3x/plugins && cp \$O/version.dll \$D/ && cp $T/compat/v3x/plugins/*.dll \$D/v3x/plugins/ &&
  cp $T/compat/compat_test.exe \$D/ && cd \$D && V3X_NO_CONSOLE=1 ./compat_test.exe old"

# ---- core and SDK
t hook_safe "$CC -I$M/include -o $T/hst.exe test/hook_safe_test.c $MH && $T/hst.exe"
t hook_chain "$CC -I$M/include -o $T/hct.exe test/hook_chain_test.c $MH && $T/hct.exe"
t sdk "$CC -Isdk -Wno-unused-function -o $T/sdkt.exe test/sdk_test.c && $T/sdkt.exe $EXE"
t template "python sdk/build_plugin.py templates/plugin_template --out $T/tpl --test $EXE"
t api_v4 "D=$T/api_v4_test; rm -rf \$D; mkdir -p \$D/v3x/plugins && cp build/version.dll \$D/ &&
  $CC -shared -o \$D/v3x/plugins/v4_ok.dll test/v4_plugin.c && $CC -shared -DV4_REQUIRE -o \$D/v3x/plugins/v4_require.dll test/v4_plugin.c &&
  $CC -shared -DV4_MINAPI=99 -o \$D/v3x/plugins/v4_minapi.dll test/v4_plugin.c && $CC -shared -DV4_WARN -o \$D/v3x/plugins/v4_warn.dll test/v4_plugin.c &&
  cp build/plugins/hello.dll \$D/v3x/plugins/ && printf '[plugins]\nHello = 0\nv4_ok=1\n' > \$D/v3x/v3x.ini &&
  $CC -o \$D/api_v4_test.exe test/api_v4_test.c -Lbuild -l:version.dll && cd \$D && V3X_NO_CONSOLE=1 ./api_v4_test.exe"
t api_v5 "D=$T/api_v5; rm -rf \$D; mkdir -p \$D/v3x/plugins && cp build/version.dll \$D/ &&
  for v in FRAME_FAULT CMD_FAULT EVENT_FAULT MAIN_FAULT LOAD_FAULT LOAD_FAIL RELOAD MARKED; do
    $CC -shared -DV5_\$v -o \$D/v3x/plugins/v5_\$(echo \$v | tr A-Z a-z).dll test/api_v5_plugin.c || exit 1; done &&
  printf '[log]\nlevel = info\nv5dbg = debug\n' > \$D/v3x/v3x.ini && printf 'v5_marked' > \$D/v3x/loading.marker &&
  printf 'crash_logger started\nUNHANDLED EXCEPTION 0xc0000005 at fake_mod.dll+0x1234 - process is going down\n' > \$D/v3x/crash.log &&
  $CC -o \$D/api_v5_test.exe test/api_v5_test.c -Lbuild -l:version.dll -luser32 && cd \$D && V3X_NO_CONSOLE=1 ./api_v5_test.exe"
gate_dir() {
    local D=$T/$1
    rm -rf "$D"; mkdir -p "$D/v3x/plugins" && cp build/version.dll "$D/" || return 1
    for v in GATE_STAR GATE_VER GATE_PREFIX GATE_NONE; do
        $CC -shared -DV5_$v -o "$D/v3x/plugins/v5_$(echo $v | tr A-Z a-z).dll" test/api_v5_plugin.c || return 1
    done
    $CC -o "$D/api_v5_test.exe" test/api_v5_test.c -Lbuild -l:version.dll -luser32
}
t api_v5_gate_safe "gate_dir g1 && cd $T/g1 && V3X_NO_CONSOLE=1 V3X_BUILD_GATE=1 ./api_v5_test.exe gate"
t api_v5_gate_load_env "gate_dir g2 && cd $T/g2 && V3X_NO_CONSOLE=1 V3X_BUILD_GATE=1 V3X_UNKNOWN_BUILD=load ./api_v5_test.exe gateload"
t api_v5_gate_load_ini "gate_dir g3 && printf 'console=0\nunknown_build = load\n[plugins]\n' > $T/g3/v3x/v3x.ini && cd $T/g3 && V3X_NO_CONSOLE=1 V3X_BUILD_GATE=1 ./api_v5_test.exe gateload"
t check_tool "python test/compat_check.py $T"

# ---- plugin offline tests (docs/testing.md)
t alloc_tracker "$CC -o $T/att.exe test/alloc_tracker_test.c && $T/att.exe ${EXE:+$EXE build/plugins/alloc_tracker.dll}"
t asset_stream "$CC -o $T/ast.exe test/asset_stream_test.c && $T/ast.exe plugins/asset_stream/asset_stream.ini.example ${EXE:+$EXE $T}"
t content_packs "mkdir -p $T/cp && $CC -o $T/cpt.exe test/content_packs_test.c && $T/cpt.exe $T/cp"
t mod_menu "mkdir -p $T/mm && $CC -o $T/mmt.exe test/mod_menu_test.c && $T/mmt.exe $T/mm ${EXE:+../game/gui/frontend/frontend_main.gui $EXE}"
t cpu_sched "$CC -o $T/cst.exe test/cpu_sched_test.c && $T/cst.exe ${EXE:+$EXE build/plugins/cpu_sched.dll}"
t enhance "mkdir -p $T/enh && $CC -o $T/enht.exe test/enhance_test.c && $T/enht.exe $T/enh $EXE"
t fast_load "$CC -I$M/include -o $T/flt.exe test/fast_load_test.c $MH && $T/flt.exe $EXE"
t fast_lookup "$CC -I$M/include -o $T/fkt.exe test/fast_lookup_test.c $MH && $T/fkt.exe $EXE"
t frame_pacer "$CC -o $T/fpt.exe test/frame_pacer_test.c && $T/fpt.exe plugins/frame_pacer/frame_pacer.ini.example $EXE"
t gfx_driver "$CC -o $T/gdt.exe test/gfx_driver_test.c && $T/gdt.exe plugins/gfx_driver/gfx_driver.ini.example $EXE"
[[ -n $EXE ]] && t keep_history "$CC -o $T/kht.exe test/keep_history_test.c && $T/kht.exe $EXE build/plugins/keep_history.dll"
t lazy_mesh "$CC -o $T/lmt.exe test/lazy_mesh_test.c && $T/lmt.exe $EXE"
t log_filter "$CC -o $T/lft.exe test/log_filter_test.c && $T/lft.exe plugins/log_filter/log_filter.ini.example"
t map_cache "$CC -DMC_TEST -o $T/mct.exe test/map_cache_test.c && $T/mct.exe $EXE"
t mem_tuner "$CC -o $T/mtt.exe test/mem_tuner_test.c && $T/mtt.exe ${EXE:+$EXE build/plugins/mem_tuner.dll}"
t rtti "$CC -o $T/rtti.exe test/rtti_test.c && $T/rtti.exe"
t sim_opt "$CC -o $T/sot.exe test/sim_opt_test.c && $T/sot.exe"
t console_plus "$CC -o $T/cplt.exe test/console_plus_test.c && $T/cplt.exe $EXE"
t tick_par "$CC -o $T/tpar.exe test/tick_par_test.c && $T/tpar.exe"
t tick_profiler "$CC -o $T/tpt.exe test/tick_profiler_test.c && $T/tpt.exe"
t lazy_gfx "$CC -Isdk -o $T/lgt.exe test/lazy_gfx_test.c && $T/lgt.exe $EXE"
t par_load "$CC -Isdk -o $T/plt.exe test/par_load_test.c && $T/plt.exe $EXE"
t mod_loader "$CC -o $T/mlt.exe test/mod_loader_test.c && $T/mlt.exe $EXE"
t loc_fallback "rm -rf $T/lf && mkdir -p $T/lf && $CC -Isdk -o $T/lft2.exe test/loc_fallback_test.c && $T/lft2.exe $T/lf"
[[ -f test/script_ext_test.c ]] && t script_ext "rm -rf $T/sx && mkdir -p $T/sx && $CC -Isdk -o $T/sxt.exe test/script_ext_test.c && $T/sxt.exe $T/sx $EXE"
[[ -n $EXE && -f test/ai_par_test.c && -f plugins/ai_par/ai_par.c ]] && t ai_par "$CC -Isdk -o $T/aipar.exe test/ai_par_test.c && $T/aipar.exe ${EXE:-} $T/"
t updater "D=$T/upd; rm -rf \$D; mkdir -p \$D/binaries/v3x/plugins && cp build/version.dll \$D/binaries/ && cp build/plugins/updater.dll \$D/binaries/v3x/plugins/ && $CC -DUPDATER_CHILD -o \$D/binaries/upd_child.exe test/updater_test.c -Lbuild -l:version.dll && $CC -o $T/updt.exe test/updater_test.c -lshell32 && $T/updt.exe \$D"
t doctor "D=$T/dr; rm -rf \$D; mkdir -p \$D/e2e/v3x/plugins && $CC -o \$D/drt.exe test/doctor_test.c && \$D/drt.exe \$D ../binaries/v3x && cp build/version.dll \$D/e2e/ && cp build/plugins/doctor.dll \$D/e2e/v3x/plugins/ && printf '# my settings\r\nconsole=0\r\nunknown_build = load ; was load\r\n\r\n[plugins]\r\nghost=1\r\n' > \$D/e2e/v3x/v3x.ini && $CC -DDOCTOR_E2E -o \$D/e2e/doctor_e2e.exe test/doctor_test.c -Lbuild -l:version.dll -luser32 && cd \$D/e2e && V3X_NO_CONSOLE=1 ./doctor_e2e.exe"

# ---- end-to-end (link build/version.dll like victoria3.exe)
t harness "e2e harness test/harness.c ironman_console && cd $T/harness && V3X_NO_CONSOLE=1 ./harness.exe"
t devconsole "e2e devconsole_test test/devconsole_test.c devconsole && cd $T/devconsole_test && V3X_NO_CONSOLE=1 ./devconsole_test.exe"
t devbridge "e2e devbridge_test test/devbridge_test.c devconsole devbridge && cd $T/devbridge_test && V3X_NO_CONSOLE=1 V3X_DEVBRIDGE_PIPE=v3x_devbridge_test ./devbridge_test.exe"
t devtools "e2e devtools_test test/devtools_test.c devtools && echo interval_sec=2 > $T/devtools_test/v3x/devtools.ini && cd $T/devtools_test && V3X_NO_CONSOLE=1 V3X_PROFILE=1 ./devtools_test.exe"
t crash "e2e crash_test test/crash_test.c crash_logger && cd $T/crash_test && V3X_NO_CONSOLE=1 ./crash_test.exe"
if [[ -z ${QUICK:-} ]]; then
    t shutdown "D=$T/shutdown; rm -rf \$D; mkdir -p \$D/v3x/plugins && cp build/version.dll \$D/ && cp build/plugins/*.dll \$D/v3x/plugins/ &&
      $CC -shared -o \$D/v3x/plugins/shutdown_probe.dll test/shutdown_probe.c &&
      $CC -o \$D/shutdown_child.exe test/shutdown_test.c -Lbuild -l:version.dll -luser32 && $CC -DSHUTDOWN_RUNNER -o \$D/shutdown_test.exe test/shutdown_test.c &&
      \$D/shutdown_test.exe \$D/shutdown_child.exe 20 con && \$D/shutdown_test.exe \$D/shutdown_child.exe 20 nocon"
fi

echo
echo "== summary"
printf '%-22s %-9s %s\n' TEST RESULT DETAIL
nf=0
for i in "${!names[@]}"; do
    printf '%-22s %-9s %s\n' "${names[$i]}" "${results[$i]}" "${details[$i]}"
    [[ ${results[$i]} == PASS ]] || nf=$((nf + 1))
done
echo "${#names[@]} tests, $nf failed"
exit $((nf > 125 ? 125 : nf))
