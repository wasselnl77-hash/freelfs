#!/bin/bash
# Round 21: the FreeBWS login freeze, reproduced and checked in a "laptop clone" VM (run on the host, no account of any kind).
#
#   build/m21-freebws.sh TWIN.iso [N]        N login flows per mode (default 3)
#
# The VM: 4 GB RAM (more than 3 GB: tabs do NOT share a web process, as on the 7 GB laptop), 1920x1080, a virgl GPU through
# the host's (virtio-vga-gl + egl-headless), user networking, the serial twin of the ISO. FreeBWS must report dmabuf buffers.
# A tiny identity provider and client site (build/m21/oidc-testsite.py, python standard library) run on the host; the guest's
# /etc/hosts points client.test, idp.test and sso.test at the host. The flow is Google's: the client page (a cross-origin
# iframe, canvas, WebGL, a heartbeat) opens a popup with window.open; the popup is redirected across hosts (idp -> sso -> idp),
# an account chooser POSTs, /popup-done posts the code to window.opener (postMessage) and window.close()s; the opener fetches
# a token (CORS) and navigates through three more cross-host redirects to a heavy page. The test clicks the Sign in button of
# the real window, then asks the server whether every stage was reached AND the page still paints (requestAnimationFrame
# counter of the heartbeat keeps rising). It also opens and closes a second tab and checks the first still paints.
#
# Needs: qemu with virgl (virtio-vga-gl) and a render node. Exit 0 only if every flow in the GPU (dmabuf) mode passes.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
iso=${1:?usage: m21-freebws.sh TWIN.iso [N]}
n=${2:-3}
work=${WORK:-$HOME/freelfs-work/scratch/m21-freebws}
rm -rf "$work"; mkdir -p "$work"
cp "$here"/m21/vm/* "$work/"
chmod +x "$work"/*.sh "$work"/*.py
( cd "$work" && python3 -I "$here/m21/oidc-testsite.py" --port 8080 --log "$work/oidc.log" > "$work/oidc.out" 2>&1 & echo $! > "$work/oidc.pid" )
sleep 1
cleanup() { kill "$(cat "$work/oidc.pid")" 2> /dev/null; "$work/g.py" poweroff > /dev/null 2>&1; sleep 3; pkill -f "[s]cratch/m21-freebws/ser" 2> /dev/null; }
trap cleanup EXIT
export FLFS_BUILD=$here
cd "$work"
XRES=1920 YRES=1080 ./boot.sh "$iso" 4G gl > boot.out 2>&1 || { cat boot.out; echo "FAIL: the VM did not come up"; exit 1; }
./g.py "echo '10.0.2.2 client.test idp.test sso.test' >> /etc/hosts" > /dev/null
rc=0
echo "mode                              result"
check_mode() { # check_mode LABEL ENV...  -> runs N flows, prints one line
	local label=$1; shift
	local ok=0 bad=0
	for i in $(seq "$n"); do
		out=$(./loop.sh 1 "$@" 2>&1 | tr '\n' ' ')
		if grep -q "run 1: ok" <<< "$out"; then ok=$((ok + 1)); else bad=$((bad + 1)); echo "  $label run $i: $out" | cut -c1-230; fi
	done
	printf '%-34s %d ok, %d FAILED of %d\n' "$label" "$ok" "$bad" "$n"
	[ "$bad" -eq 0 ]
}
# is it really the GPU path?
./flow.sh 1 > /dev/null 2>&1
./g.py "grep -m1 ' started: ' /home/tester/.cache/freebws/log" | tee dmabuf-check.txt
grep -q "dmabuf buffers, share_process=0" dmabuf-check.txt || { echo "FAIL: FreeBWS is not on dmabuf buffers with separate web processes (see above)"; rc=1; }
grep -q "dmabuf through the GPU" <(./g.py "cat /run/user/1000/flcomp.log") || { echo "FAIL: flcomp does not report the GPU dmabuf path"; rc=1; }
check_mode "GPU (dmabuf), tabs not shared" FREEBWS_BUFFERS=dmabuf FREEBWS_SHARE=0 || rc=1
check_mode "shm, tabs not shared" FREEBWS_BUFFERS=shm FREEBWS_SHARE=0 || rc=1
check_mode "GPU (dmabuf), tabs shared (1 GB mode)" FREEBWS_BUFFERS=dmabuf FREEBWS_SHARE=1 || rc=1
for share in 0 1; do
	r=$(./closetab.sh 1 FREEBWS_BUFFERS=dmabuf FREEBWS_SHARE=$share 2>&1 | tail -n 1)
	printf '%-34s %s\n' "GPU, open+close a tab (share=$share)" "$r"
	grep -q PAINTING <<< "$r" || rc=1
done
echo "logs and pictures in $work"
exit $rc
