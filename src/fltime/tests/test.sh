#!/bin/bash
# Tests for fltime-set and fl-sntp that need no privileges: the helper is built with
# a scratch zoneinfo/etc/init.d (-DFLTIME_TEST -DNO_CLOCK), fl-sntp talks to a fake
# NTP server on localhost.
#   tests/test.sh [ZONEINFO]    (default /usr/share/zoneinfo; CC may carry --sysroot)
set -u
cd "$(dirname "$0")/.."
CC=${CC:-cc}
ZI=${1:-/usr/share/zoneinfo}
T=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$T"' EXIT
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

mkdir -p "$T/etc" "$T/init.d"
cat > "$T/init.d/ntp" <<'EOS'
#!/bin/sh
echo "$1" >> "$(dirname "$0")/../service.log"
EOS
chmod +x "$T/init.d/ntp"
$CC -std=c17 -D_GNU_SOURCE -O1 -Wall -DFLTIME_TEST -DNO_CLOCK -DZONEINFO="\"$ZI\"" -DETCDIR="\"$T/etc\"" \
	-DINITD="\"$T/init.d\"" -DHWCLOCK="\"/bin/true\"" -o "$T/set" fltime-set.c || exit 1
S="$T/set"

$S zone Europe/Berlin; check "zone: set Europe/Berlin" '[ "$(readlink $T/etc/localtime)" = "$ZI/Europe/Berlin" ] && [ "$(cat $T/etc/timezone)" = Europe/Berlin ]'
$S zone America/Argentina/Buenos_Aires; check "zone: three-part name" '[ "$(cat $T/etc/timezone)" = America/Argentina/Buenos_Aires ]'
$S zone UTC; check "zone: UTC" '[ "$(cat $T/etc/timezone)" = UTC ]'
for bad in "" ../etc/passwd /etc/passwd "Europe/../../etc/passwd" Europe//Berlin Europe/ .hidden "Europe/.x" "posix/Europe/Berlin" \
	"right/UTC" posixrules zone.tab iso3166.tab "Europe/Berl in" 'Europe/Berlin;id' 'a$(id)' Nowhere/City Europe \
	"$(printf 'A%.0s' $(seq 60))" "Europe/Berlin
x"; do
	before=$(cat "$T/etc/timezone"); $S zone "$bad" 2>/dev/null; rc=$?
	check "zone: refuses '${bad:0:30}'" '[ $rc -eq 3 ] && [ "$(cat $T/etc/timezone)" = "$before" ]'
done
# a file in zoneinfo that is no TZif file, and a symlink out of the tree
echo junk > "$T/fake"; mkdir -p "$T/zi"; cp -r "$ZI/Europe" "$T/zi/"; echo notzif > "$T/zi/Junk"; ln -s /etc/passwd "$T/zi/Out"
$CC -std=c17 -D_GNU_SOURCE -DFLTIME_TEST -DNO_CLOCK -DZONEINFO="\"$T/zi\"" -DETCDIR="\"$T/etc\"" -DINITD="\"$T/init.d\"" -o "$T/set2" fltime-set.c
"$T/set2" zone Junk 2>/dev/null; check "zone: file without TZif magic refused" '[ $? -eq 3 ]'
"$T/set2" zone Out 2>/dev/null; check "zone: symlink out of the tree refused" '[ $? -eq 3 ]'
"$T/set2" zone Europe/Paris; check "zone: other tree works" '[ "$(cat $T/etc/timezone)" = Europe/Paris ]'
$S 2>/dev/null; check "usage without arguments" '[ $? -eq 2 ]'
$S bogus x 2>/dev/null; check "usage unknown command" '[ $? -eq 2 ]'

$S ntp off; check "ntp off: file and service" 'grep -qx NTP=0 $T/etc/fltime.conf && [ "$(tail -1 $T/service.log)" = stop ]'
echo 'SERVERS="a b"' >> "$T/etc/fltime.conf"
$S ntp on; check "ntp on: keeps SERVERS, one NTP line" 'grep -qx NTP=1 $T/etc/fltime.conf && grep -qx "SERVERS=\"a b\"" $T/etc/fltime.conf && [ $(grep -c ^NTP= $T/etc/fltime.conf) -eq 1 ] && [ "$(tail -1 $T/service.log)" = restart ]'
$S ntp maybe 2>/dev/null; check "ntp: refuses other words" '[ $? -eq 3 ]'
$S time 1900000000 2>/dev/null; check "time: refused while ntp is on" '[ $? -eq 4 ]'
$S ntp off
$S time 1900000000; check "time: accepted when ntp is off" '[ $? -eq 0 ]'
for bad in 5 99999999999999 -1 1e9 "1900000000;" 4200000000 ""; do $S time "$bad" 2>/dev/null; check "time: refuses '$bad'" '[ $? -eq 3 ]'; done
$S sync 2>/dev/null; check "sync: refused when ntp is off" '[ $? -eq 4 ]'
$S ntp on; $S sync; check "sync: restarts the service" '[ "$(tail -1 $T/service.log)" = restart ]'

# the SNTP client against the fake server: +1.5 s ahead
$CC -std=c17 -D_GNU_SOURCE -O1 -Wall -o "$T/sntp" fl-sntp.c || exit 1
python3 -I tests/fakentp.py 12399 1.5 6 & sleep 0.5
out=$("$T/sntp" -q 127.0.0.1:12399); echo "     $out"
off=${out#offset_ms=}; off=${off%% *}
check "sntp: offset about +1500 ms" 'python3 -I -c "import sys; sys.exit(0 if abs($off-1500)<50 else 1)"'
kill %1 2>/dev/null
"$T/sntp" -q 127.0.0.1:12398 2>/dev/null; check "sntp: no server, no answer" '[ $? -eq 1 ]'
# a server that answers with the wrong originate time is not believed
python3 -I - <<'EOP' &
import socket, struct, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("127.0.0.1", 12397))
for _ in range(3):
    d, a = s.recvfrom(512)
    s.sendto(struct.pack(">BBbb", 0x24, 2, 4, -20) + b"\0" * 12 + b"\0" * 8 + b"\1" * 8 + b"\2" * 8 + b"\3" * 8, a)
EOP
sleep 0.4; "$T/sntp" -q 127.0.0.1:12397 2>/dev/null; check "sntp: reply to somebody else's question refused" '[ $? -eq 1 ]'
echo "fails: $fails"
exit $((fails > 0))
