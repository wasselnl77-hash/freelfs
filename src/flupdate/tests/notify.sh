#!/bin/sh
# flupdate-notify against made-up flpkg, flatpak, flnotify and flstore.
# `make check`. Touches only a scratch directory.
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
here=$(pwd)
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
export XDG_RUNTIME_DIR=$dir FLUPDATE_STATE=$dir
export FLUPDATE_FLPKG=$dir/flpkg FLUPDATE_FLATPAK=$dir/flatpak
export FLUPDATE_FLNOTIFY=$dir/flnotify FLUPDATE_STORE=$dir/flstore

# flpkg: N_PKG lines like the real check-updates; "fail" simulates being offline
cat >"$dir/flpkg" <<SH
#!/bin/sh
echo "flpkg \$*" >>"$dir/calls"
[ -e "$dir/pkg-fail" ] && exit 1
i=0
while [ \$i -lt \${N_PKG:-0} ]; do
	printf 'pkg%s\t1.0-1\t1.1-1\t1000\tfreelfs\n' \$i
	i=\$((i + 1))
done
SH
cat >"$dir/flatpak" <<SH
#!/bin/sh
echo "flatpak \$*" >>"$dir/calls"
[ -e "$dir/fp-fail" ] && exit 1
i=0
while [ \$i -lt \${N_FP:-0} ]; do
	echo org.example.App\$i
	i=\$((i + 1))
done
SH
# flnotify: logs its arguments, prints an id, then the click ("action open")
cat >"$dir/flnotify" <<SH
#!/bin/sh
echo "flnotify \$*" >>"$dir/notes"
echo 41
[ -e "$dir/click" ] && echo "action open"
echo "closed 2"
SH
cat >"$dir/flstore" <<SH
#!/bin/sh
echo "flstore \$*" >>"$dir/store"
SH
chmod +x "$dir"/flpkg "$dir"/flatpak "$dir"/flnotify "$dir"/flstore
run() { rm -f "$dir/notes" "$dir/store"; "$here/flupdate-notify" --once; }

N_PKG=2 N_FP=3 run
[ "$(grep -c . "$dir/notes")" = 1 ] && grep -q -- '-w 5 updates available 2 system packages, 3 Flatpak apps' "$dir/notes" \
	&& ok "one notification: N updates available" || bad "one notification: N updates available"
grep -q 'remote-ls --updates --user' "$dir/calls" && grep -q 'flpkg check-updates' "$dir/calls" \
	&& ok "runs flpkg check-updates and flatpak remote-ls --updates --user" || bad "commands"
[ "$(cat "$dir/flupdate-notify.id")" = 41 ] && ok "keeps the notification id" || bad "keeps the notification id"
[ ! -e "$dir/store" ] && ok "no click, no Store" || bad "no click, no Store"

N_PKG=1 N_FP=0 run
grep -q -- '-r 41 .*-w 1 update available 1 system package$' "$dir/notes" \
	&& ok "next check replaces the card (-r 41), singular wording" || bad "replace and singular"

touch "$dir/click"
N_PKG=1 run
sleep 0.3
grep -qx 'flstore --updates' "$dir/store" && ok "the action opens flstore --updates" || bad "the action opens flstore --updates"
rm "$dir/click"

N_PKG=0 N_FP=0 run
[ ! -e "$dir/notes" ] && ok "nothing to update: no notification" || bad "nothing to update: no notification"

touch "$dir/pkg-fail" "$dir/fp-fail"
N_PKG=4 run
[ ! -e "$dir/notes" ] && ok "offline: no notification" || bad "offline: no notification"
rm "$dir/fp-fail"
N_PKG=4 N_FP=2 run
grep -q -- '-w 2 updates available 2 Flatpak apps' "$dir/notes" \
	&& ok "one source failing still reports the other" || bad "one source failing still reports the other"
rm "$dir/pkg-fail"

# the daemon mode: first check after FLUPDATE_FIRST, then every FLUPDATE_INTERVAL; TERM stops it
rm -f "$dir/notes"
N_PKG=1 FLUPDATE_FIRST=1 FLUPDATE_INTERVAL=1 "$here/flupdate-notify" &
pid=$!
sleep 0.4
[ ! -e "$dir/notes" ] && ok "waits before the first check" || bad "waits before the first check"
sleep 2.2
kill "$pid"
wait "$pid" 2>/dev/null
n=$(grep -c . "$dir/notes" 2>/dev/null || echo 0)
[ "$n" -ge 2 ] && ok "checks again after the interval ($n so far)" || bad "checks again after the interval ($n)"
kill -0 "$pid" 2>/dev/null && bad "TERM stops it" || ok "TERM stops it"

echo "flupdate: $fails failed"
[ "$fails" -eq 0 ]
