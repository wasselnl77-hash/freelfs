#!/bin/bash
# Round 24b: the m24b tests, one table, PASS / FAIL / NOT RUN.
#
#   build/m24b-all.sh [TWIN.iso]
#
# Host-side (no root, no network): build/m24b/nix-backend.sh (src/flpkg/sources.sh + sources-nix.sh against a stand-in nix).
# QEMU (needs TWIN.iso: the serial twin of an ISO that has the nix package, the rc service "nix" and the flpkg front end):
# build/m24b/live.cmds through build/live-run.py as the live tester, with a real network. Without the ISO the QEMU row
# says NOT RUN. Logs and table: $OUT (default ~/freelfs-work/scratch/m24b), table: $OUT/table.txt.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
cd "$here/.." || exit 1
twin=${1:-}
out=${OUT:-$HOME/freelfs-work/scratch/m24b}
mkdir -p "$out"
: > "$out/table.txt"
suite() { # suite NAME CMD...
	local n=$1; shift
	echo "== $n" >&2
	local t0=$SECONDS r
	if "$@" > "$out/$n.log" 2>&1; then r=PASS; else r="FAIL (exit $?)"; fi
	printf '%-28s %-14s %4d s   %s\n' "$n" "$r" $((SECONDS - t0)) "$out/$n.log" | tee -a "$out/table.txt"
}
suite host-nix-backend bash build/m24b/nix-backend.sh
if [ -n "$twin" ] && [ -f "$twin" ]; then
	suite qemu-live nice -n 10 python3 -u build/live-run.py --iso "$twin" --mem "${M24B_MEM:-1G}" "build/m24b/live.cmds" -- -device usb-tablet
else
	printf '%-28s %-14s %4s     %s\n' qemu-live "NOT RUN" 0 "no TWIN.iso given" | tee -a "$out/table.txt"
fi
