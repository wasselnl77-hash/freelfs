#!/bin/sh
# Differential test runner for flsh.
#   tests/run.sh                      run everything
#   tests/run.sh tests/posix/05-x.sh  run single test(s)
# posix/*.sh are compared against dash, bash/*.sh against bash.

dir=$(cd "$(dirname "$0")/.." && pwd)
flsh="$dir/flsh"
pass=0
fail=0
work=$(mktemp -d) || exit 2
trap 'rm -rf "$work"' EXIT INT TERM

if [ ! -x "$flsh" ]; then
    echo "flsh binary not found at $flsh (run make)" >&2
    exit 2
fi

# run_one REF_SHELL TESTFILE
run_one() {
    ref=$1
    t=$2
    name=${t#"$dir"/}
    name=${name#./}
    (cd "$work" && timeout 10 "$ref" "$t" </dev/null >"$work/exp.out" 2>"$work/exp.err")
    echo $? >"$work/exp.st"
    (cd "$work" && timeout 10 "$flsh" "$t" </dev/null >"$work/act.out" 2>"$work/act.err")
    echo $? >"$work/act.st"
    { cat "$work/exp.out"; echo "[exit status: $(cat "$work/exp.st")]"; } >"$work/exp.cmp"
    { cat "$work/act.out"; echo "[exit status: $(cat "$work/act.st")]"; } >"$work/act.cmp"
    if cmp -s "$work/exp.cmp" "$work/act.cmp"; then
        echo "ok   $name"
        pass=$((pass + 1))
    else
        echo "FAIL $name"
        diff -u --label "expected($ref)" --label "actual(flsh)" "$work/exp.cmp" "$work/act.cmp"
        if [ -s "$work/act.err" ]; then
            echo "--- flsh stderr (first lines):"
            head -n 5 "$work/act.err"
        fi
        fail=$((fail + 1))
    fi
}

abspath() {
    case $1 in
    /*) echo "$1" ;;
    *) echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")" ;;
    esac
}

# group REF_SHELL FILES...
run_group() {
    ref=$1
    shift
    if ! command -v "$ref" >/dev/null 2>&1; then
        echo "note: $ref not found, skipping its tests"
        return
    fi
    for t in "$@"; do
        [ -f "$t" ] || continue
        run_one "$ref" "$(abspath "$t")"
    done
}

if [ $# -gt 0 ]; then
    for t in "$@"; do
        case $(abspath "$t") in
        */tests/bash/*) run_group bash "$t" ;;
        *) run_group dash "$t" ;;
        esac
    done
else
    run_group dash "$dir"/tests/posix/*.sh
    run_group bash "$dir"/tests/bash/*.sh
fi

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
exit 0
