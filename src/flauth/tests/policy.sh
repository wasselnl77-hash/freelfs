#!/bin/sh
# Tests for the password policy (flauth-check.h and flauth): the stamp is per class and per calling
# executable, the destructive classes are one-shot, stamps go stale after 2 minutes, a stamp must be
# private to its owner, an empty password passes only for the live session's tester on the live system.
# Needs no root: the test hooks FLAUTH_DIR, FLAUTH_SHADOW, FLAUTH_DIALOG, FLAUTH_LIVE and FLAUTH_USER
# replace /run/flauth, /etc/shadow, the dialog, /run/live and the account name (a setuid copy ignores them).
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
cc -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -o "$dir/flauth" flauth.c -lcrypt || exit 1
cc -std=c17 -D_GNU_SOURCE -Wall -Wextra -o "$dir/probe" tests/gate-probe.c || exit 1
cc -std=c17 -D_GNU_SOURCE -Wall -Wextra -DFLAUTH_BIN="\"$dir/flauth\"" -o "$dir/probe-gate" tests/gate-probe.c || exit 1

pw=$(openssl passwd -6 -salt testsalt secret) || exit 1
cat > "$dir/shadow" <<S
alice:$pw:19000:0:99999:7:::
tester::19000:0:99999:7:::
carol::19000:0:99999:7:::
S
printf '#!/bin/sh\nprintf "%%s\\n" "$TEST_PW"\n' > "$dir/dialog"
chmod +x "$dir/dialog"
touch "$dir/live"

export FLAUTH_DIR="$dir/run" FLAUTH_SHADOW="$dir/shadow" FLAUTH_DIALOG="$dir/dialog"
export FLAUTH_LIVE="$dir/nolive" FLAUTH_USER=alice TEST_PW=secret
uid=$(id -u)
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
expect() { # expect DESC WANT GOT
	if [ "$3" = "$2" ]; then ok "$1"; else bad "$1 (want $2, got $3)"; fi
}
probe() { "$dir/probe" check "$1"; }
spawn() { "$dir/probe" spawn "$1"; }
flauth() { "$dir/flauth" "$@" >/dev/null 2>&1; }
shell_exe=$(readlink /proc/$$/exe)

# usage: a class is required and must be a known one
"$dir/flauth" -- true >/dev/null 2>&1; expect "flauth needs --class" 3 $?
"$dir/flauth" --class nope -- true >/dev/null 2>&1; expect "unknown class refused" 3 $?

# installed system (no live marker): an empty password is refused, for every class
FLAUTH_USER=carol flauth --class pkg -- true; expect "installed, empty password: flauth refuses" 4 $?
FLAUTH_USER=carol probe pkg | grep -qx 0; expect "installed, empty password: no stamp, check refused" 0 $?
FLAUTH_USER=tester flauth --class disk-write -- true; expect "installed, tester with empty password: refused" 4 $?
FLAUTH_USER=tester probe disk-write | grep -qx 0; expect "installed, tester: disk write refused" 0 $?

# live system: only the tester with an empty password passes, with no dialog and no stamp needed
FLAUTH_LIVE="$dir/live" FLAUTH_USER=carol flauth --class pkg -- true; expect "live, other empty account: refused" 4 $?
FLAUTH_LIVE="$dir/live" FLAUTH_USER=carol probe disk-write | grep -qx 0; expect "live, other empty account: check refused" 0 $?
FLAUTH_LIVE="$dir/live" FLAUTH_USER=tester FLAUTH_DIALOG=/bin/false flauth --class disk-write -- true
expect "live, tester: no dialog, command runs" 0 $?
FLAUTH_LIVE="$dir/live" FLAUTH_USER=tester FLAUTH_DIALOG=/bin/false probe disk-write | grep -qx 1
expect "live, tester: disk write allowed without a stamp" 0 $?

# a right password gives a stamp for that class, for the calling program
rm -rf "$dir/run"
flauth --class pkg -- true; expect "right password: flauth exit 0" 0 $?
probe pkg | grep -qx 1; expect "pkg stamp: the caller is allowed" 0 $?
probe pkg | grep -qx 1; expect "pkg stamp: reused for the same class and caller" 0 $?
spawn pkg | grep -qx 0; expect "pkg stamp: refused for a caller with another executable" 0 $?
probe time | grep -qx 0; expect "pkg stamp: never used for another class" 0 $?
flauth --class time -- true; expect "time: flauth exit 0" 0 $?
probe time | grep -qx 1; expect "time stamp: allowed" 0 $?
probe pkg | grep -qx 1; expect "time stamp does not replace the pkg stamp" 0 $?

# one-shot: a destructive class is used up by the first check that passes
flauth --class disk-write -- true; expect "disk-write: flauth exit 0" 0 $?
spawn disk-write | grep -qx 0; expect "disk-write: another executable does not use it" 0 $?
probe disk-write | grep -qx 1; expect "disk-write: the first check uses it" 0 $?
probe disk-write | grep -qx 0; expect "disk-write: a second check is refused" 0 $?
[ ! -e "$dir/run/ok-$uid.disk-write" ]; expect "disk-write: the stamp file is gone" 0 $?
flauth --class snap-rollback -- true; expect "snap-rollback: flauth exit 0" 0 $?
probe snap-rollback | grep -qx 1; expect "snap-rollback: allowed once" 0 $?
probe snap-rollback | grep -qx 0; expect "snap-rollback: not twice" 0 $?

# a wrong password gives no stamp
TEST_PW=wrong flauth --class snap-delete -- true; expect "wrong password: flauth exit 1" 1 $?
probe snap-delete | grep -qx 0; expect "wrong password: no stamp" 0 $?
rm -f "$dir/run"/fail-*   # three wrong tries lock flauth for 30 s: the later tests start with a clean count

# stamps that are old, private to the owner, or made for another executable do not count
mkstamp() { # mkstamp CLASS EXE AGE MODE
	printf '%s\n%s\n%s\n' "$1" "$2" "$(( $(date +%s) - $3 ))" > "$dir/run/ok-$uid.$1"
	chmod "$4" "$dir/run/ok-$uid.$1"
}
mkstamp pkg "$shell_exe" 0 600; probe pkg | grep -qx 1; expect "hand-made fresh stamp, right executable: allowed" 0 $?
mkstamp pkg "$shell_exe" 121 600; probe pkg | grep -qx 0; expect "stamp older than 120 s: refused" 0 $?
mkstamp pkg "$shell_exe" 0 620; probe pkg | grep -qx 0; expect "stamp readable by others: refused" 0 $?
mkstamp pkg /bin/false 0 600; probe pkg | grep -qx 0; expect "stamp for another executable: refused" 0 $?
mkstamp time "$shell_exe" 0 600; mv "$dir/run/ok-$uid.time" "$dir/run/ok-$uid.pkg"
probe pkg | grep -qx 0; expect "pkg file holding a time stamp: refused" 0 $?
rm -f "$dir/run/ok-$uid.pkg"
mkstamp disk-write "$shell_exe" 0 600
spawn disk-write | grep -qx 0; expect "hand-made one-shot stamp: wrong caller refused" 0 $?
probe disk-write | grep -qx 1; expect "hand-made one-shot stamp: right caller used once" 0 $?
probe disk-write | grep -qx 0; expect "hand-made one-shot stamp: gone after use" 0 $?

# the helper's own path: no stamp, flauth asks, the same program runs again and is allowed; a wrong password
# or a second round (FLAUTH_TRIED already set) does not run it
rm -rf "$dir/run"
out=$(FLAUTH_USER=alice TEST_PW=secret "$dir/probe-gate" gate pkg 2>/dev/null); expect "gate: asks, then the helper runs again and is allowed" 1 "$out"
out=$(FLAUTH_USER=alice FLAUTH_DIALOG=/bin/false "$dir/probe-gate" gate pkg 2>/dev/null); expect "gate: a fresh stamp needs no question" 1 "$out"
out=$(FLAUTH_USER=alice TEST_PW=wrong "$dir/probe-gate" gate disk-write 2>/dev/null); rc=$?
expect "gate: wrong password stops before the action" 1 "$rc"; expect "gate: nothing printed on a wrong password" "" "$out"
rm -f "$dir/run"/fail-*
out=$(FLAUTH_TRIED=1 FLAUTH_USER=alice FLAUTH_DIALOG=/bin/false "$dir/probe-gate" gate snap-delete 2>/dev/null)
expect "gate: a second round without a stamp is refused, no question" 0 "$out"
out=$(FLAUTH_USER=alice TEST_PW=secret "$dir/probe-gate" gate disk-write 2>/dev/null)
expect "gate: one-shot class asked and allowed once" 1 "$out"
out=$(FLAUTH_USER=alice FLAUTH_DIALOG=/bin/false "$dir/probe-gate" gate disk-write 2>/dev/null); rc=$?
expect "gate: used-up one-shot stamp: the next run asks again (no answer: cancelled)" 2 "$rc"
expect "gate: cancelled: the helper does not run" "" "$out"

echo
[ "$fails" -eq 0 ] && echo "all policy tests passed" || echo "$fails policy test(s) failed"
[ "$fails" -eq 0 ]
