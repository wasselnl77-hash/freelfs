#!/bin/bash
# Round 18 honest tests (QEMU, KVM): the live session as the person at the laptop uses it. Needs the SERIAL TWIN of the
# live ISO (./build/53-iso.sh --cmdline console=ttyS0,115200, copied away). Everything goes through the real UI: the
# login screen's Shut down button, the panel's power dialog, a double click in Files. A step that does not do what it
# should makes live-run.py exit 1 (it reads results, it never prints "ok" by itself).
#
#   build/m18-honest.sh TWIN.iso      STEPS="greeter-shutdown ..." to run some
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
twin=${1:?usage: m18-honest.sh TWIN.iso}
out=${OUT:-$HOME/freelfs-work/scratch/m18}
only=${STEPS:-}
want() { [ -z "$only" ] || [[ " $only " == *" $1 "* ]]; }
mkdir -p "$out"
rc=0
run() { # run NAME [QEMU args]
	local n=$1; shift
	sed "s|@OUT@|$out|g; s|@HERE@|$here/m18|g" "$here/m18/$n.cmds" > "$out/$n.run"
	echo "== $n"
	nice -n 10 python3 -u "$here/live-run.py" --iso "$twin" --mem 1G "$out/$n.run" -- -device usb-tablet "$@" > "$out/$n.log" 2>&1
	local r=$?
	tail -n 40 "$out/$n.log" | grep -E "^(FAIL|NOT ok|RESULT|@poweroffwait|colours)" 
	[ $r -eq 0 ] || { echo "-> $n FAILED (exit $r)"; rc=1; }
}
want greeter-fresh && run greeter-fresh
want forced-shutdown && run forced-shutdown
want greeter-shutdown && run greeter-shutdown
want panel-shutdown && run panel-shutdown
want files-open && run files-open -drive file="$HOME/freelfs-work/m10-samples.img",format=raw,if=virtio,readonly=on
echo "pictures and logs in $out"
exit $rc
