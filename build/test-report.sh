#!/bin/bash
# List the package test suites that failed (see run_check in lib.sh). A
# failure there does not stop the build; this shows what to look at.

set -euo pipefail
. "$(dirname "$0")/config.sh"
. "$(dirname "$0")/lib.sh"

cd "$FLFS_LOGS"
ran=$(grep -l '^==> running tests:' -- *.log 2>/dev/null | wc -l)
failed=$(grep -H '^==> TESTS FAILED' -- *.log 2>/dev/null || true)
msg "$ran test suites ran, $(grep -c . <<< "$failed" || true) failed"
[ -z "$failed" ] || sed 's/\.log:==> TESTS FAILED/:/' <<< "$failed"
