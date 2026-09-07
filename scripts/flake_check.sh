#!/usr/bin/env bash
# Runs the suite repeatedly and fails if any run differs from any other.
#
# The second recurring defect class in this project, after vacuous tests,
# is assertions bounded by something the environment controls: a short
# write that only happens when kernel buffers are small enough, a cycle
# budget that expires under CPU contention, a queue depth sampled at one
# instant. Four separate instances across five phases, each found by
# noticing a run had failed rather than by anything systematic.
#
# A single green run cannot distinguish "correct" from "correct on this
# machine when nothing else was happening". Repetition can, cheaply.
#
#   scripts/flake_check.sh [runs] [build-dir]
set -uo pipefail

RUNS="${1:-10}"
BUILD="${2:-build}"

if [ ! -d "$BUILD" ]; then
    echo "no build directory at $BUILD (run: make build)" >&2
    exit 1
fi

echo "running the suite $RUNS times in $BUILD"
failures=0
declare -a failed_tests=()

for i in $(seq 1 "$RUNS"); do
    out=$(ctest --test-dir "$BUILD" 2>&1)
    if echo "$out" | grep -q "100% tests passed"; then
        printf "."
    else
        printf "F"
        failures=$((failures + 1))
        while read -r line; do
            failed_tests+=("$line")
        done < <(echo "$out" | grep -E "^\s+[0-9]+ - " | sed 's/^[[:space:]]*//')
    fi
done
echo

if [ "$failures" -eq 0 ]; then
    echo "OK: $RUNS/$RUNS runs green"
    exit 0
fi

echo "FLAKY: $failures of $RUNS runs failed"
echo "tests involved:"
printf '%s\n' "${failed_tests[@]}" | sort | uniq -c | sort -rn
echo
echo "A test that fails intermittently is usually bounded by something"
echo "the environment controls rather than by the code. See"
echo "docs/testing-standards.md."
exit 1
