#!/usr/bin/env bash
# Runs every test binary of a build directory and prints a compact summary.
# Usage: scripts/run-tests.sh [build-dir]
set -u
BUILD_DIR="${1:-build/dev}"
export QT_QPA_PLATFORM=offscreen
status=0
for exe in "$BUILD_DIR"/bin/tst_*; do
    [ -f "$exe" ] || continue
    [ -x "$exe" ] || continue
    case "$exe" in *.exe.*) continue;; esac
    out="$(mktemp)"
    "$exe" -o "$out,txt" >/dev/null 2>&1
    rc=$?
    grep -E "^(FAIL|XPASS|QFATAL)|Totals" "$out" | sed "s|^|$(basename "$exe"): |"
    grep -A2 -E "^FAIL" "$out" | grep "failure location" | sed 's|^|    |'
    rm -f "$out"
    [ $rc -ne 0 ] && status=1
done
exit $status
