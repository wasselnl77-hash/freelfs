#!/bin/sh
# Headless test of the notification service, the cards, the centre, Do not disturb, lock handling
# and the battery warnings: a private session bus, flcomp without a display, the real flpanel,
# flnotify. Needs ../flcomp built (flcomp, tests/shmclient), dbus-daemon and python3.
# DBUS_DAEMON, DBUS_SEND may name other binaries (default: from PATH). `make check`;
# NOTIFY_OUT=DIR keeps the screenshots (PPM). Nothing outside a scratch directory is touched.
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
here=$(pwd)
comp=$here/../flcomp
dir=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$dir"' EXIT
export XDG_RUNTIME_DIR=$dir HOME=$dir XDG_CONFIG_HOME=$dir/cfg FLUI_CONF=$dir/desktop.conf
mkdir -p "$dir/cfg"
W=${NOTIFY_W:-800}; H=${NOTIFY_H:-500}
export FLCOMP_WALLPAPER= FLCOMP_HEADLESS=${W}x${H} FLCOMP_SESSION= FLCOMP_SCREENSHOT=$dir/shot.ppm
export FLCOMP_TEST_FIFO=$dir/in FLCOMP_KEYBOARD_CONF=$dir/none
export FLCOMP_PANEL=$here/flpanel
export FLCOMP_BACKLIGHT=$dir/none-backlight
mkfifo "$dir/in"
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

# a private session bus
cat >"$dir/bus.conf" <<XML
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>session</type><keep_umask/><listen>unix:path=$dir/bus</listen><auth>EXTERNAL</auth>
<policy context="default"><allow send_destination="*" eavesdrop="true"/><allow eavesdrop="true"/><allow own="*"/></policy></busconfig>
XML
${DBUS_DAEMON:-dbus-daemon} --config-file="$dir/bus.conf" --nofork --nopidfile 2>"$dir/bus.log" &
i=0
while [ ! -S "$dir/bus" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
export DBUS_SESSION_BUS_ADDRESS=unix:path=$dir/bus

# fake power supplies, and a wrapper for flpower that only logs
ps=$dir/ps; mkdir -p "$ps/BAT0" "$ps/AC"
echo Battery >"$ps/BAT0/type"; echo 80 >"$ps/BAT0/capacity"; echo Discharging >"$ps/BAT0/status"
echo 40000000 >"$ps/BAT0/energy_now"; echo 50000000 >"$ps/BAT0/energy_full"; echo 10000000 >"$ps/BAT0/power_now"
echo Mains >"$ps/AC/type"; echo 0 >"$ps/AC/online"
export FLPANEL_POWER_SUPPLY=$ps
mkdir "$dir/bin"
printf '#!/bin/sh\necho "$@" >>"%s/flpower.log"\n' "$dir" >"$dir/bin/flpower"
chmod +x "$dir/bin/flpower"
PATH=$dir/bin:$PATH
export PATH
# the lock program of the lock test: a lock screen that unlocks after three seconds
cat >"$dir/fllock" <<SH
#!/bin/sh
exec "$comp/tests/shmclient" -L -c 00ff00 -C "unlock:3" -s 6 >/dev/null
SH
chmod +x "$dir/fllock"
export FLCOMP_LOCK=$dir/fllock

"$comp/flcomp" 2>"$dir/log" &
cpid=$!
i=0
while [ ! -S "$dir/wayland-0" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
sleep 1.2

FN=$here/flnotify
shot() { sleep 0.3; kill -USR1 $cpid; sleep 0.5; [ -n "$NOTIFY_OUT" ] && cp "$dir/shot.ppm" "$NOTIFY_OUT/$1.ppm"; return 0; }
pixel() {
	python3 -I - "$@" <<'PY'
import sys
f = open(sys.argv[1], 'rb'); x = int(sys.argv[2]); y = int(sys.argv[3])
assert f.readline() == b'P6\n'; w, h = map(int, f.readline().split()); f.readline()
d = f.read(); o = (y * w + x) * 3
print('%02x%02x%02x' % tuple(d[o:o + 3]))
PY
}
expect_px() { got=$(pixel "$dir/shot.ppm" "$2" "$3"); [ "$got" = "$4" ] && ok "$1" || bad "$1: ($2,$3) is $got, wanted $4"; }
send() { echo "$1" >"$dir/in"; sleep 0.12; }
park() { send "move 30 $((H - 120))"; }	# the pointer on a card would pause its timer, and tint it
CARD_X=$((W - 16 - 180)); CARD_Y=$((16 + 30))	# inside the first card, away from text and buttons
BG=1d2125; PANEL=272c33

# ---- the service ------------------------------------------------------------------------
n=$(dbus-send --session --print-reply --dest=org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications.GetServerInformation 2>&1 | grep -c "flpanel")
[ "$n" -ge 1 ] && ok "GetServerInformation answers" || bad "GetServerInformation"
caps=$(dbus-send --session --print-reply --dest=org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications.GetCapabilities 2>&1)
echo "$caps" | grep -q '"actions"' && echo "$caps" | grep -q '"body"' && ok "GetCapabilities lists actions and body" || bad "GetCapabilities: $caps"
dbus-send --session --print-reply --dest=org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.DBus.Introspectable.Introspect 2>&1 | grep -q "NotificationClosed" && ok "Introspect" || bad "Introspect"

# ---- a card -------------------------------------------------------------------------------
shot 0-empty
expect_px "no card at first" $CARD_X $CARD_Y $BG
id1=$($FN -a Software -u normal -t 0 "Updates are ready" "3 packages can be updated.")
[ "$id1" = 1 ] && ok "first id is 1" || bad "first id: $id1"
shot 1-card
expect_px "card at the top right" $CARD_X $CARD_Y $PANEL
[ "$($FN --count)" = 1 ] && ok "history has one entry" || bad "count"
# a second one goes on top and shifts the first down
id2=$($FN -a Terminal -i flterm -t 0 "Build finished" "make -j4 finished in 41 s.")
shot 2-two-cards
expect_px "two cards stack" $CARD_X $((16 + 80 + 8 + 20)) $PANEL
# a badge for the two unread ones left of the clock
others=$(python3 -I - "$dir/shot.ppm" <<'PY'
import sys
f = open(sys.argv[1], 'rb'); f.readline(); w, h = map(int, f.readline().split()); f.readline()
d = f.read()
n = sum(1 for y in range(h - 30, h - 14) for x in range(w - 200, w) if d[(y*w+x)*3:(y*w+x)*3+3] == bytes.fromhex("3f9b63"))
print(n)
PY
)
[ "$others" -ge 60 ] && ok "unread badge next to the clock" || bad "no unread badge ($others)"

# dismissing: click the cross of the top card
send "move $((W - 16 - 18)) $((16 + 18))"
send "button 272 1"
send "button 272 0"
park
sleep 0.2
shot 3-one-dismissed
expect_px "the top card is gone, the other moved up" $CARD_X $((16 + 20)) $PANEL
[ "$($FN --count)" = 2 ] && ok "dismissed cards stay in the history" || bad "history after dismiss: $($FN --count)"

# ---- expiry, replace, close --------------------------------------------------------------------
$FN --clear
sleep 0.3
shot 4-cleared
expect_px "Clear all removes the cards" $CARD_X $CARD_Y $BG
[ "$($FN --count)" = 0 ] && ok "history is empty" || bad "not empty"
$FN -t 1200 "Short" "gone in a moment" >/dev/null
sleep 0.4; shot 5-short
expect_px "card shows" $CARD_X $CARD_Y $PANEL
sleep 2.2; shot 6-expired
expect_px "card expired by itself" $CARD_X $CARD_Y $BG
[ "$($FN --count)" = 1 ] && ok "an expired card stays in the history" || bad "expired card lost"
# the reason is reported
out=$($FN -t 800 -w "Wait for me" "wait")
echo "$out" | grep -q "closed 1" && ok "NotificationClosed reason 1 (expired)" || bad "closed reason: $out"
rid=$($FN -t 0 "Original" "first text")
rid2=$($FN -t 0 -r "$rid" "Replaced" "second text")
[ "$rid" = "$rid2" ] && ok "replaces_id keeps the id" || bad "replace: $rid vs $rid2"
dbus-send --session --type=method_call --dest=org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications.CloseNotification uint32:"$rid"
sleep 0.3; shot 7-closed
expect_px "CloseNotification removes the card" $CARD_X $CARD_Y $BG

# ---- the click on a card runs the default action ----------------------------------------------------------
( $FN -w -t 0 -A default=Open "Click me" "default action" >"$dir/wait.out" 2>&1 ) &
sleep 0.7
send "move $CARD_X $CARD_Y"
send "button 272 1"
send "button 272 0"
sleep 0.8
grep -q "action default" "$dir/wait.out" && ok "click: ActionInvoked default" || bad "no ActionInvoked: $(cat "$dir/wait.out")"
grep -q "closed 2" "$dir/wait.out" && ok "click: closed as dismissed" || bad "no closed 2: $(cat "$dir/wait.out")"
park

# ---- Do not disturb -----------------------------------------------------------------------------------------------
$FN --clear
$FN --dnd on
[ "$($FN --dnd status)" = on ] && ok "Do not disturb on" || bad "dnd status"
grep -q notify_dnd=1 "$dir/desktop.conf" && ok "saved in desktop.conf" || bad "not saved"
$FN -t 0 "Quiet" "no card for this" >/dev/null
sleep 0.4; shot 8-dnd
expect_px "no card while Do not disturb is on" $CARD_X $CARD_Y $BG
[ "$($FN --count)" = 1 ] && ok "but it is in the history" || bad "dnd history"
$FN -u critical "Urgent" "this one shows" >/dev/null
sleep 0.4; shot 9-dnd-critical
expect_px "critical notifications still show" $CARD_X $CARD_Y $PANEL
$FN --dnd off
[ "$($FN --dnd status)" = off ] && ok "Do not disturb off" || bad "dnd off"

# ---- the centre ------------------------------------------------------------------------------------------------------------
$FN -t 0 "A" "b" >/dev/null
$FN --clear
for t in "Updates are ready|3 packages can be updated: freebws, flui and linux-firmware." "Build finished|make -j4 in ~/src/flcomp finished in 41 s." "Copy complete|photos.tar was copied to the USB stick."; do
	$FN -a "${t%%|*}" -t 1 "${t%%|*}" "${t#*|}" >/dev/null
done
sleep 0.5
$FN --center
sleep 0.5
shot 10-centre
a=$(pixel "$dir/shot.ppm" $((W - 140)) $((H - 44 - 20)))
[ "$a" != "$BG" ] && ok "the centre opens above the panel" || bad "the centre did not open"
# Esc closes it (keyboard goes to the flyout)
send "key 1 1"; send "key 1 0"
sleep 0.3; shot 11-centre-closed
expect_px "Escape closes the centre" $((W - 100)) $((H - 44 - 100)) $BG
# click on the clock opens it too
send "move $((W - 50)) $((H - 20))"
send "button 272 1"; send "button 272 0"
sleep 0.4; shot 12-clock-click
a=$(pixel "$dir/shot.ppm" $((W - 100)) $((H - 44 - 100)))
[ "$a" != "$BG" ] && ok "click on the clock opens the centre" || bad "clock click did not open it"
send "button 272 1"; send "button 272 0"
send "move 40 60"; send "button 272 1"; send "button 272 0"
sleep 0.3
shot 13-centre-dismissed
expect_px "a click outside closes it" $((W - 100)) $((H - 44 - 100)) $BG
[ "$($FN --count)" = 3 ] && ok "three in the history" || bad "centre count"

# ---- the history survives a restart of the panel -------------------------------------------------------------------------
sleep 1.2
pkill -f "$here/flpanel"
sleep 1.8
[ "$($FN --count)" = 3 ] && ok "history survives a panel restart" || bad "history after restart: $($FN --count)"

# ---- while locked: no cards, and they appear after the unlock -----------------------------------------------------------------
$FN --clear
send "key 125 1"; send "key 38 1"; send "key 38 0"; send "key 125 0"
sleep 0.5
$FN -t 0 "While locked" "you do not see this yet" >/dev/null
shot 14-locked
expect_px "no card on the lock screen" $CARD_X $CARD_Y 00ff00
sleep 3.0; shot 15-unlocked
expect_px "the card is there after the unlock" $CARD_X $CARD_Y $PANEL

# ---- battery: low warnings and power saving -------------------------------------------------------------------------------------
$FN --clear
echo 5 >"$dir/ps/BAT0/capacity"
echo 2500000 >"$dir/ps/BAT0/energy_now"
sleep 6.5
c=$($FN --count)
[ "$c" -ge 1 ] && ok "a warning at 5% (history $c)" || bad "no low battery warning"
shot 16-low
expect_px "the warning is a card" $CARD_X $CARD_Y $PANEL
echo Charging >"$dir/ps/BAT0/status"
echo 1 >"$dir/ps/AC/online"
sleep 6.5
shot 17-charging
expect_px "plugging in closes the critical card" $CARD_X $CARD_Y $BG
echo "power_saver=1" >"$dir/desktop.conf"
sleep 1.0
grep -q "saver on" "$dir/flpower.log" && ok "power saving asks flpower saver on" || bad "flpower not asked: $(cat "$dir/flpower.log" 2>/dev/null)"
echo "power_saver=0" >"$dir/desktop.conf"
sleep 1.0
grep -q "saver off" "$dir/flpower.log" && ok "and saver off" || bad "flpower saver off not asked"

kill -0 $cpid 2>/dev/null && ok "compositor alive" || bad "compositor died"
[ $fails = 0 ] && echo "all tests passed" || { echo "$fails test(s) failed"; echo "--- flcomp log"; cat "$dir/log"; }
exit $fails
