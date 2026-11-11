#!/bin/bash
# Tests for what a signed index may claim and which keys count: expiry,
# rollback/replay (serial), and key rotation with a transition period.
# usage: tests/test-trust.sh   (from src/flpkg, after make)
# SPDX-License-Identifier: MIT

set -u
cd "$(dirname "$0")/.."
export FLPKG_LIBDIR=$PWD
flpkg=$PWD/flpkg
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
pkgs=$work/pkgs
mkdir -p "$pkgs"
fails=0

ok() { printf '  %-62s ok\n' "$1"; }
bad() { printf '  %-62s FAIL\n' "$1"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

# a client: its own root, key directory and config; CLIENT=name sets which
client() {
	c=$work/$1
	mkdir -p "$c/root" "$c/keys"
	printf 'builddir = %s/build\ncache = /var/cache/flpkg\nkeys = %s/keys\nrepo test file://%s\n' \
		"$c" "$c" "$repo" > "$c/conf"
	shift
	for k in "$@"; do cp "$work/$k.pub" "$c/keys/"; done
}
fl() { "$flpkg" --config "$c/conf" --root "$c/root" "$@" > "$work/out" 2>&1; }
state=var/lib/flpkg
# add [--opt value]... FILE...: options first, then the package files; the repo is $repo
add() {
	local pre=()
	while [[ ${1:-} == --* ]]; do pre+=("$1" "$2"); shift 2; done
	"$flpkg" --config /dev/null repo add "${pre[@]}" "$repo" "$@" > "$work/add.out" 2>&1
}
serial_of() { sed -n 's/.*serial:\([0-9]*\).*/\1/p' "$repo/index.zst.sig"; }

"$flpkg" --config /dev/null build -o "$pkgs" tests/recipes/demo1 > "$work/build.log" 2>&1 ||
	{ echo "cannot build the test package"; cat "$work/build.log"; exit 1; }
demo=$pkgs/demo-1.0-1-any.flpkg
for k in a b evil evil2; do "$flpkg" key generate "$work/$k" > /dev/null 2>&1; done

echo "index expiry and serial"
repo=$work/repo
add --key "$work/a.key" "$demo"
cp -r "$repo" "$work/repo.s1"
check "the index carries a serial and valid-until in its signed comment" \
	'grep -q "serial:[0-9]" "$repo/index.zst.sig" && grep -Eq "valid-until:20[0-9-]+T[0-9:]+Z" "$repo/index.zst.sig"'
check "the default validity is 14 days" \
	'u=$(sed -n "s/.*valid-until:\([^\t]*\)\thashed.*/\1/p" "$repo/index.zst.sig"); d=$(( ($(date -u -d "$u" +%s) - $(date +%s)) / 3600 )); [ "$d" -ge 335 ] && [ "$d" -le 336 ]'
client c1 a
check "a good index is accepted" 'fl sync && grep -q "test: 1 packages" "$work/out"'
s1=$(serial_of)
check "the accepted serial is stored in the state dir" '[ "$(cat "$c/root/$state/index-serial.test")" = "$s1" ]'
check "the same index again is fine (same serial)" 'fl sync'

add --key "$work/a.key" --valid-days -1 "$demo"
cp -r "$repo" "$work/repo.expired"
check "an expired index is refused" '! fl sync'
check "  and the error says what happened and what to do" \
	'grep -q "EXPIRED on" "$work/out" && grep -q "re-sign and republish" "$work/out" && grep -q "clock" "$work/out"'
check "  and the stored serial did not change" '[ "$(cat "$c/root/$state/index-serial.test")" = "$s1" ]'
check "  and the old synced index is still there" 'fl search demo && grep -q "test/demo 1.0-1" "$work/out"'
check "check-updates refuses an expired index and prints nothing" \
	'! "$flpkg" --config "$c/conf" --root "$c/root" check-updates > "$work/cu" 2> "$work/cu.err" && [ ! -s "$work/cu" ] && grep -q EXPIRED "$work/cu.err"'
cp "$repo/index.zst.sig" "$c/root/var/cache/flpkg/repos/test/index.zst.sig"
check "commands on an expired cached index warn" 'fl search demo && grep -q "index you have EXPIRED" "$work/out"'

add --key "$work/a.key" "$demo"
s2=$(serial_of)
check "re-signing gives a higher serial" '[ "$s2" -gt "$s1" ]'
check "the fresh index is accepted again" 'fl sync && [ "$(cat "$c/root/$state/index-serial.test")" = "$s2" ]'
cp -r "$repo" "$work/repo.s2"
rm -rf "$repo"; cp -r "$work/repo.s1" "$repo"
check "an older, still unexpired index (replay) is refused" '! fl sync'
check "  and the error names both serials and the way out" \
	'grep -q "ROLLBACK refused" "$work/out" && grep -q "serial $s1 but this machine already accepted serial $s2" "$work/out" && grep -q "must publish a serial above $s2" "$work/out"'
check "  and the stored serial did not go down" '[ "$(cat "$c/root/$state/index-serial.test")" = "$s2" ]'
check "check-updates refuses the replay too" \
	'! "$flpkg" --config "$c/conf" --root "$c/root" check-updates > "$work/cu" 2> "$work/cu.err" && grep -q ROLLBACK "$work/cu.err"'
client c2 a
check "a machine that never saw the newer index takes the older one" 'fl sync'
check "check-updates does not write the serial" \
	'rm -f "$c/root/$state/index-serial.test"; "$flpkg" --config "$c/conf" --root "$c/root" check-updates --count > "$work/cu" 2>&1 && [ ! -e "$c/root/$state/index-serial.test" ]'
rm -rf "$repo"; cp -r "$work/repo.s2" "$repo"
client c1 a
check "the owner's way out: a serial above works again" 'add --key "$work/a.key" --serial $((s2 + 100)) "$demo" && fl sync && [ "$(cat "$c/root/$state/index-serial.test")" = "$((s2 + 100))" ]'
check "--serial lower than the one in the repo dir is possible but not the default" \
	'add --key "$work/a.key" "$demo" && [ "$(serial_of)" -gt "$((s2 + 100))" ]'

echo "key rotation"
rm -rf "$repo" "$work/repo.s1" "$work/repo.s2"
add --key "$work/a.key" "$demo"
client k1 a
check "start: only key A is trusted" 'fl sync && "$flpkg" --config "$c/conf" --root "$c/root" key list | grep -c trusted | grep -qx 1'
"$flpkg" --config /dev/null key introduce --key "$work/a.key" --new "$work/b.pub" --retire-days 30 "$repo" > "$work/intro.out" 2>&1
check "key introduce writes rotate.pub and a signature by the old key" \
	'[ -f "$repo/rotate.pub" ] && [ -f "$repo/rotate.pub.sig" ] && grep -q "retire-old:" "$repo/rotate.pub.sig" && grep -q "introduces" "$work/intro.out"'
check "the private key is not copied or printed anywhere" \
	'! grep -rqF "$(sed -n 2p "$work/a.key")" "$repo" "$work/intro.out" && ! grep -rqF "$(sed -n 2p "$work/b.key")" "$repo" "$work/intro.out"'
# the new key alone cannot be used yet by someone who does not know it
client k2 a
check "transition: sync (index still signed by A) learns B" \
	'fl sync && grep -q "now also trusting the new signing key" "$work/out" && [ -f "$c/root/$state/keys/$(ls "$c/root/$state/keys" | grep pub$ | head -1)" ]'
check "  both keys are listed, A with its retirement date" \
	'"$flpkg" --config "$c/conf" --root "$c/root" key list > "$work/kl" && grep -c "trusted" "$work/kl" | grep -qx 2 && grep -q "trusted until" "$work/kl"'
check "  the state has a retirement for A" '[ "$(wc -l < "$c/root/$state/keys/retire.list")" = 1 ]'
check "  syncing again changes nothing" 'cp "$c/root/$state/keys/retire.list" "$work/r1" && fl sync && ! grep -q "now also trusting" "$work/out" && cmp -s "$work/r1" "$c/root/$state/keys/retire.list"'
add --key "$work/b.key" "$demo"
check "the repo switches to B: the client that learned B accepts it" 'fl sync'
check "a client that has only A, and never synced during the transition, learns B on the way" \
	'client k3 a && fl sync && grep -q "now also trusting" "$work/out"'
check "a client with only A and no rotation statement refuses B" \
	'client k4 a && mv "$repo/rotate.pub" "$work/rp" && mv "$repo/rotate.pub.sig" "$work/rps" && ! fl sync && grep -q "unknown key" "$work/out"; mv "$work/rp" "$repo/rotate.pub"; mv "$work/rps" "$repo/rotate.pub.sig"'
check "check-updates uses the rotation but does not save the key" \
	'client k5 a && "$flpkg" --config "$c/conf" --root "$c/root" check-updates --count > "$work/cu" 2> "$work/cu.err" && [ "$(cat "$work/cu")" = 0 ] && grep -q "now also trusting" "$work/cu.err" && [ ! -e "$c/root/$state/keys" ]'
check "a client that trusts both keys needs no state to accept B" \
	'client k6 a b && fl sync && ! grep -q "now also trusting" "$work/out" && [ "$(wc -l < "$c/root/$state/keys/retire.list")" = 1 ]'
check "  (and still retires A from the statement: the image shipped both keys)" \
	'"$flpkg" --config "$c/conf" --root "$c/root" key list | grep -q "trusted until"'

echo "rotation: refusals"
client k7 a
cp "$repo/rotate.pub" "$work/rp.good"; cp "$repo/rotate.pub.sig" "$work/rps.good"
printf 'x' >> "$repo/rotate.pub"
check "a tampered rotation statement is ignored with a warning" \
	'! fl sync && grep -q "not signed by a key you trust" "$work/out" && [ ! -e "$c/root/$state/keys" ]'
cp "$work/rp.good" "$repo/rotate.pub"
"$flpkg" --config /dev/null key introduce --key "$work/evil.key" --new "$work/evil2.pub" "$repo" > /dev/null 2>&1
check "a statement signed by an untrusted key does not add a key" \
	'client k8 a && ! fl sync && grep -q "not signed by a key you trust" "$work/out" && [ ! -e "$c/root/$state/keys" ]'
check "a statement cannot be moved onto another key file (the file is signed)" \
	'cp "$work/evil2.pub" "$repo/rotate.pub" && cp "$work/rps.good" "$repo/rotate.pub.sig" && client k9 a && ! fl sync && [ ! -e "$c/root/$state/keys" ]'
cp "$work/rp.good" "$repo/rotate.pub"; cp "$work/rps.good" "$repo/rotate.pub.sig"
check "a restored good statement works again" 'client k10 a && fl sync'

echo "rotation: the transition ends"
"$flpkg" --config /dev/null key introduce --key "$work/a.key" --new "$work/b.pub" --retire-days -1 "$repo" > /dev/null 2>&1
add --key "$work/a.key" "$demo"
check "after the old key retired, an index signed by it is refused" \
	'client k11 a && ! fl sync && grep -q "unknown key" "$work/out" && grep -q "was retired on" "$work/out" && grep -q "install the newest flpkg package" "$work/out"'
add --key "$work/b.key" "$demo"
check "  and one signed by the new key is accepted" 'client k12 a && fl sync'
check "  key list shows A as retired" '"$flpkg" --config "$c/conf" --root "$c/root" key list | grep -q "RETIRED since"'
check "packages signed by the retired key are refused too" \
	'client k13 a && fl sync && "$flpkg" --config "$c/conf" --root "$c/root" repo add --key "$work/a.key" "$work/side" "$demo" > /dev/null 2>&1 && cp "$work/side/demo-1.0-1-any.flpkg.sig" "$repo/" && ! fl install -y demo && grep -q "unknown key" "$work/out"'

echo "trust tests: $fails failed"
[ "$fails" -eq 0 ]
