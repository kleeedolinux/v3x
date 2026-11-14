#!/usr/bin/env bash
# CI wrapper around test/run_all.sh (GitHub runners have no Victoria 3, so ../binaries/victoria3.exe
# is missing and run_all.sh passes no exe to the tests).
#  1. runs test/run_all.sh (every offline test; new tests are picked up automatically)
#  2. re-runs the failed ones once (some e2e tests are timing-sensitive on a busy machine)
#  3. fails the job for every test still failing, except the ones in NEEDS_EXE: those check
#     anchors against the real exe and are only reported as warnings here.
# Env: V3X_TMP (default $RUNNER_TEMP/v3x), QUICK=1 skips the shutdown test (see run_all.sh).
set -uo pipefail
cd "$(dirname "$0")/../.."

# tests that cannot pass without the real victoria3.exe (measured on a copy of the repo without ../binaries)
NEEDS_EXE=" "   # none today; run_all.sh skips keep_history and ai_par without the exe
# TEMPORARY (2026-11-14): tests not yet portable to GitHub runners (hard-coded D:/v3x_tmp paths,
# console child, file/locale assumptions). Pass locally; reported as warnings until fixed.
NOT_PORTABLE=" "

tmp=${V3X_TMP:-${RUNNER_TEMP:-${TEMP:-/tmp}}/v3x}
command -v cygpath > /dev/null && tmp=$(cygpath -m "$tmp")
export V3X_TMP=$tmp
mkdir -p "$V3X_TMP"
if [[ -f ../binaries/victoria3.exe ]]; then echo "note: ../binaries/victoria3.exe exists, tests get the real exe"; fi

failed_of() {   # names of FAIL rows in a run_all.sh summary
    awk '/^== summary/{s=1; next} s && $2 ~ /^FAIL/ {print $1}' "$1"
}

out1=$V3X_TMP/ci_run1.txt
bash test/run_all.sh 2>&1 | tee "$out1"
rc=${PIPESTATUS[0]}
if [[ $rc == 100 ]] || grep -q '^BUILD FAILED' "$out1"; then echo "::error::build failed"; exit 1; fi
if ! grep -q '^== summary' "$out1"; then echo "::error::test/run_all.sh did not finish (exit $rc)"; exit 1; fi
failed=$(failed_of "$out1" | tr '\n' ' ')

if [[ -n ${failed// /} ]]; then
    echo
    echo "== re-running failed tests once: $failed"
    out2=$V3X_TMP/ci_run2.txt
    ONLY="$failed" bash test/run_all.sh 2>&1 | tee "$out2"
    failed=$(failed_of "$out2" | tr '\n' ' ')
fi

bad=0
for n in $failed; do
    if [[ $NEEDS_EXE == *" $n "* ]]; then
        echo "::warning::$n failed: it needs ../binaries/victoria3.exe (not on CI)"
    elif [[ $NOT_PORTABLE == *" $n "* ]]; then
        echo "::warning::$n failed: test not yet portable to CI runners (passes locally; being fixed)"
    else
        echo "::error::test $n failed (log: \$V3X_TMP/t/run_all_logs/$n.log)"
        bad=$((bad + 1))
    fi
done
echo "ci_tests: $bad blocking failure(s); still failing: ${failed:-none}"
exit $((bad > 0))
