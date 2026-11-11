#!/bin/bash
# Tests for flsnap on its mock backend (plain directories instead of btrfs
# subvolumes: the real thing needs root and a mounted btrfs, see README).
# usage: tests/test-flsnap.sh   (from src/flsnap, after make)
# SPDX-License-Identifier: MIT
set -u
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export FLSNAP_MOCK=$work/m FLSNAP_CONF=$work/flsnap.conf FLSNAP_FLPKG=$work/flpkg
flsnap=$PWD/flsnap
fails=0
ok() { printf '  %-62s ok\n' "$1"; }
bad() { printf '  %-62s FAIL\n' "$1"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
fs() { "$flsnap" "$@" > "$work/out" 2>&1; }

mkdir -p "$FLSNAP_MOCK"/top/@/etc "$FLSNAP_MOCK"/top/@snapshots "$FLSNAP_MOCK"/esp/flboot
echo one > "$FLSNAP_MOCK"/top/@/etc/version
printf '#!/bin/sh\necho "glibc 2.40-1"\necho "demo 1.0-1"\n' > "$work/flpkg"
chmod +x "$work/flpkg"
snaps=$FLSNAP_MOCK/top/@snapshots
cur=$FLSNAP_MOCK/top/@
boot=$FLSNAP_MOCK/esp/flboot/snapshots
echo "keep = 5" > "$FLSNAP_CONF"

fs list
check "empty list says so" 'grep -q "no snapshots yet" "$work/out"'
fs status
check "status: supported, keep 5, none booted" 'grep -qx "supported=1" "$work/out" && grep -qx "keep=5" "$work/out" && grep -qx "booted=0" "$work/out"'

fs create --reason "flpkg: upgrade demo"
check "create works" '[ $? -eq 0 ] && [ -f "$snaps/1/etc/version" ] && [ -f "$snaps/1.info" ]'
check "snapshot is marked read-only" '[ -f "$snaps/1/.mock-ro" ]'
check "info has date, reason and packages" 'grep -q "^date=20" "$snaps/1.info" && grep -qx "reason=flpkg: upgrade demo" "$snaps/1.info" && grep -qx "demo 1.0-1" "$snaps/1.info"'
check "boot list for flboot" 'grep -q "^@snapshots/1 20.*flpkg: upgrade demo" "$boot"'
fs show 1
check "show prints the packages" 'grep -q "packages (2)" "$work/out" && grep -q "glibc 2.40-1" "$work/out"'

printf 'glibc 9\nx 1\n' > "$work/pk"
fs create --reason "$(printf 'a\nb\tc')" --packages "$work/pk"
check "reason is one clean line, packages from a file" 'grep -qx "reason=a b c" "$snaps/2.info" && grep -qx "x 1" "$snaps/2.info"'

for i in 3 4 5 6 7 8; do echo "v$i" > "$cur/etc/version"; fs create --reason "step $i"; done
check "keeps the last 5 (3..8)" '[ "$(ls "$snaps" | grep -c "^[0-9]*$")" = 5 ] && [ -d "$snaps/8" ] && [ -d "$snaps/4" ] && [ ! -e "$snaps/3" ] && [ ! -e "$snaps/3.info" ]'
check "boot list follows (newest first, 5 lines)" '[ "$(grep -c ^@snapshots "$boot")" = 5 ] && [ "$(grep ^@ "$boot" | head -1 | cut -d" " -f1)" = "@snapshots/8" ]'
fs list --machine
check "machine list: tab separated, newest first" '[ "$(head -1 "$work/out" | cut -f1,4,6)" = "$(printf "8\tstep 8\t0")" ] && [ "$(wc -l < "$work/out")" = 5 ]'

# numbers are not reused
fs delete 8
fs create --reason after-delete
check "numbers keep counting after a delete" '[ -d "$snaps/9" ] && [ ! -e "$snaps/8" ]'

fs prune --keep 2
check "prune --keep 2" '[ "$(ls "$snaps" | grep -c "^[0-9]*$")" = 2 ] && [ -d "$snaps/9" ] && [ -d "$snaps/7" ]'

# refused arguments
for bad in 0 -1 +2 01 1x ../x "" 1000000000 "1 2"; do
	fs delete "$bad"
	check "delete '$bad' refused" '[ $? -ne 0 ] && grep -q "not a snapshot number" "$work/out"'
done
fs create --bogus
check "create with unknown option refused" '[ $? -ne 0 ]'
fs rollback 77
check "rollback to a missing snapshot refused" '[ $? -ne 0 ] && grep -q "no snapshot #77" "$work/out"'
fs rollback
check "rollback without N on the main system refused" '[ $? -ne 0 ]'

# rollback
echo "broken update" > "$cur/etc/version"
touch "$cur/etc/new-file"
fs rollback 7
check "rollback ok" '[ $? -eq 0 ] && grep -q "Restart" "$work/out"'
check "@ is now snapshot 7 (writable copy)" '[ "$(cat "$cur/etc/version")" = v7 ] && [ ! -e "$cur/etc/new-file" ] && [ ! -e "$cur/.mock-ro" ]'
check "default subvolume is @" '[ "$(cat "$FLSNAP_MOCK/default")" = @ ]'
check "the state left is a snapshot" 'grep -l "before going back to #7" "$snaps"/*.info | head -1 | grep -q info'
old=$(grep -l "before going back to #7" "$snaps"/*.info | head -1); old=${old%.info}
check "... with the broken contents" '[ "$(cat "$old/etc/version")" = "broken update" ] && [ -f "$old/etc/new-file" ]'
check "... and its packages (the running system's)" 'grep -qx "demo 1.0-1" "$old.info"'
check "snapshot 7 is still there" '[ -d "$snaps/7" ]'
check "still at most 5 snapshots" '[ "$(ls "$snaps" | grep -c "^[0-9]*$")" -le 5 ]'
check "boot list lists the new one" 'grep -q "before going back to #7" "$boot"'

# running from a snapshot
echo "@snapshots/7" > "$FLSNAP_MOCK/booted"
fs status
check "status says which snapshot runs" 'grep -qx "booted=7" "$work/out"'
fs create
check "no new snapshot while running an old state" '[ $? -ne 0 ] && grep -q "earlier state" "$work/out"'
fs delete 7
check "the running snapshot cannot be deleted" '[ $? -ne 0 ] && [ -d "$snaps/7" ]'
fs prune --keep 1
check "prune spares the running snapshot" '[ -d "$snaps/7" ]'
echo "tweaked in the snapshot" > "$cur/etc/version"
fs rollback
check "rollback without N uses the running snapshot" '[ $? -eq 0 ] && [ "$(cat "$cur/etc/version")" = v7 ]'
rm -f "$FLSNAP_MOCK/booted"

# a failure leaves @ as it was: make the copy fail (cp that always fails)
mkdir "$work/bin"; printf '#!/bin/sh\nexit 1\n' > "$work/bin/cp"; chmod +x "$work/bin/cp"
echo "keep me" > "$cur/etc/version"
before=$(ls "$snaps" | grep -c '^[0-9]*$')
sed 's#"/usr/bin/cp"#"'"$work"'/bin/cp"#g' flsnap.c > "$work/flsnap-nocp.c"
cc -std=c17 -D_GNU_SOURCE -o "$work/flsnap-nocp" "$work/flsnap-nocp.c" 2> /dev/null
"$work/flsnap-nocp" rollback 7 > "$work/out" 2>&1
rc=$?
check "failed rollback reports it" '[ $rc -ne 0 ] && grep -q "cannot make the new @" "$work/out"'
check "failed rollback: @ untouched, nothing left behind" '[ "$(cat "$cur/etc/version")" = "keep me" ] && [ "$(ls "$snaps" | grep -c "^[0-9]*$")" = "$before" ]'

# setuid: the environment must not matter (simulated by the test being unable to
# setuid; just check the usage and that unknown commands fail)
fs bogus
check "unknown command refused" '[ $? -eq 2 ]'

if [ "$fails" -ne 0 ]; then echo "$fails flsnap test(s) failed"; exit 1; fi
echo "flsnap: all tests passed"
