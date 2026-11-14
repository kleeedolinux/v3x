#!/usr/bin/env bash
# Static analysis report (the security review action 14). NON-BLOCKING: always exits 0 unless
# STATIC_STRICT=1 (then exit 1 when a tool reports findings). Uses whatever is installed:
#   clang-tidy   checks bugprone-*, cert-*, clang-analyzer-* (minus noisy style checks)
#   cppcheck     --enable=warning,portability,performance
#   clang --analyze (the static analyzer) when clang-tidy is missing but clang is there
# Nothing installed = a note and exit 0. Output: $OUT (default build/static_analysis/) with one log per
# tool and a summary line "static analysis: <tool> <N> findings".
#
#   bash test/static_analysis.sh                 report
#   STATIC_STRICT=1 bash test/static_analysis.sh gate (once the baseline is clean)
#
# CI (ubuntu or windows runner with LLVM; see .github/workflows): install clang-tidy + cppcheck, then
#   bash test/static_analysis.sh   (continue-on-error: true until the baseline is triaged)
# The analysis targets x86_64-windows-gnu headers through --target; on a Linux runner without
# mingw headers set STATIC_SYSROOT to a mingw-w64 sysroot (e.g. /usr/x86_64-w64-mingw32).
set -uo pipefail
cd "$(dirname "$0")/.."
OUT=${OUT:-build/static_analysis}
mkdir -p "$OUT"
SRC_CORE=(src/core.c src/proxy.c src/safe_hook.c)
# SDK headers are analysed through small TUs that include them; plugins one TU per .c (no tests)
mapfile -t SRC_PLUG < <(ls plugins/*/*.c 2>/dev/null | grep -vE '(_test|/test_)[^/]*\.c$')
printf '#include "v3x_sdk.h"\n#include "v3x_safe.h"\nint main(void){return 0;}\n' > "$OUT/sdk_tu.c"
TARGET=(--target=x86_64-w64-windows-gnu)
INC=(-Iinclude -Isdk -Ithird_party/minhook/include)
[[ -n ${STATIC_SYSROOT:-} ]] && INC+=(--sysroot="$STATIC_SYSROOT")
DEFS=(-D_WIN32 -D_WIN64 -DWIN32_LEAN_AND_MEAN)
total=0 ran=0

if command -v clang-tidy > /dev/null; then
    ran=1
    CHECKS='-*,bugprone-*,cert-*,clang-analyzer-*,-bugprone-easily-swappable-parameters,-bugprone-reserved-identifier,-cert-dcl37-c,-cert-dcl51-cpp,-bugprone-assignment-in-if-condition,-cert-err33-c,-bugprone-narrowing-conversions,-bugprone-multi-level-implicit-pointer-conversion'
    : > "$OUT/clang-tidy.log"
    for f in "${SRC_CORE[@]}" "$OUT/sdk_tu.c" "${SRC_PLUG[@]}"; do
        extra=(); [[ $f == src/core.c ]] && extra=(-fms-extensions)
        clang-tidy --quiet -checks="$CHECKS" "$f" -- "${TARGET[@]}" "${INC[@]}" "${DEFS[@]}" "${extra[@]}" >> "$OUT/clang-tidy.log" 2>&1
    done
    n=$(grep -cE ': (warning|error):' "$OUT/clang-tidy.log")
    echo "static analysis: clang-tidy $n findings ($OUT/clang-tidy.log)"
    total=$((total + n))
elif command -v clang > /dev/null; then
    ran=1
    : > "$OUT/clang-analyze.log"
    for f in "${SRC_CORE[@]}" "$OUT/sdk_tu.c" "${SRC_PLUG[@]}"; do
        extra=(); [[ $f == src/core.c ]] && extra=(-fms-extensions)
        clang --analyze -Xanalyzer -analyzer-output=text "${TARGET[@]}" "${INC[@]}" "${DEFS[@]}" "${extra[@]}" -o /dev/null "$f" >> "$OUT/clang-analyze.log" 2>&1
    done
    n=$(grep -cE ': warning:' "$OUT/clang-analyze.log")
    echo "static analysis: clang --analyze $n findings ($OUT/clang-analyze.log)"
    total=$((total + n))
fi

if command -v cppcheck > /dev/null; then
    ran=1
    cppcheck --quiet --enable=warning,portability,performance --inline-suppr --std=c11 --platform=win64 \
        --suppress=missingIncludeSystem -Iinclude -Isdk -Ithird_party/minhook/include -D_WIN32 -D_WIN64 \
        "${SRC_CORE[@]}" "${SRC_PLUG[@]}" "$OUT/sdk_tu.c" > "$OUT/cppcheck.log" 2>&1
    n=$(grep -cE '(error|warning|portability|performance)[:\]]' "$OUT/cppcheck.log")
    echo "static analysis: cppcheck $n findings ($OUT/cppcheck.log)"
    total=$((total + n))
fi

if [[ $ran == 0 ]]; then
    echo "static analysis: no tool found (install clang-tidy and/or cppcheck); nothing checked"
    exit 0
fi
echo "static analysis: $total findings in total (non-blocking unless STATIC_STRICT=1)"
[[ ${STATIC_STRICT:-} == 1 && $total -gt 0 ]] && exit 1
exit 0
