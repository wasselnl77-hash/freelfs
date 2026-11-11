#!/bin/sh
# fllock-check against a made-up shadow file: right and wrong passwords, locked, expired and
# empty accounts, an over-long line, arguments, and that wrong tries take turns (one a second).
# Not root: the checker reads FLLOCK_SHADOW and FLLOCK_LOCKFILE only when it is not privileged.
# Needs openssl for the test hash. `make check`.
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
me=$(id -un)
hash=$(openssl passwd -6 -salt abcdefgh 'correct horse') || { echo "no openssl"; exit 1; }
export FLLOCK_LOCKFILE=$dir/lock
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
shadow() { printf '%s:%s:19000:0:99999:7:::\n' "$me" "$1" >"$dir/shadow"; export FLLOCK_SHADOW=$dir/shadow; }
run() { printf '%s\n' "$1" | ./fllock-check; echo $?; }

shadow "$hash"
[ "$(run 'correct horse')" = 0 ] && ok "right password" || bad "right password"
[ "$(run 'wrong')" = 1 ] && ok "wrong password" || bad "wrong password"
[ "$(run '')" = 1 ] && ok "empty line against a real password" || bad "empty line"
[ "$(run 'correct horse ')" = 1 ] && ok "trailing space counts" || bad "trailing space"
long=$(head -c 600 /dev/zero | tr '\0' a)
[ "$(run "$long")" = 1 ] && ok "over-long line" || bad "over-long line"
shadow ""
[ "$(run 'anything')" = 0 ] && ok "empty password unlocks" || bad "empty password"
shadow "!$hash"
[ "$(run 'correct horse')" = 2 ] && ok "locked account: no password unlocks" || bad "locked account"
shadow "*"
[ "$(run 'x')" = 2 ] && ok "no-login account" || bad "no-login account"
printf '%s:%s:19000:0:99999:7::1:\n' "$me" "$hash" >"$dir/shadow"
[ "$(run 'correct horse')" = 2 ] && ok "expired account" || bad "expired account"
printf 'nobody-else:%s:19000:0:99999:7:::\n' "$hash" >"$dir/shadow"
[ "$(run 'correct horse')" = 2 ] && ok "no entry for the caller (other accounts are never tried)" || bad "other account's entry used"
shadow "$hash"
./fllock-check foo </dev/null; [ $? = 3 ] && ok "arguments refused" || bad "arguments"
# guessing is serialised: three wrong tries at once take at least three seconds
t0=$(date +%s.%N)
for i in 1 2 3; do (printf 'no\n' | ./fllock-check >/dev/null) & done
wait
t1=$(date +%s.%N)
el=$(echo "$t1 $t0" | awk '{printf "%d", ($1 - $2) * 10}')
[ "$el" -ge 28 ] && ok "three parallel wrong tries took $((el / 10)).$((el % 10)) s" || bad "wrong tries not serialised ($el/10 s)"
# a right password does not wait
t0=$(date +%s.%N)
printf 'correct horse\n' | ./fllock-check
t1=$(date +%s.%N)
el=$(echo "$t1 $t0" | awk '{printf "%d", ($1 - $2) * 10}')
[ "$el" -le 5 ] && ok "a right password does not wait" || bad "right password was slow"
[ $fails = 0 ] && echo "all tests passed" || echo "$fails test(s) failed"
exit $fails
