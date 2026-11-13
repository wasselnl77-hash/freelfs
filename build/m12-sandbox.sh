#!/bin/bash
# Security round: the FreeBWS web process sandbox in the real image (docs/INTEGRATION-TODO.md "Sandbox test steps"):
# `freebws --sandbox-check`, bwrap in front of WPEWebProcess, seccomp and namespaces, /dev/dri, a page that draws,
# a download from the host (a small http server on 127.0.0.1, reached by the guest as 10.0.2.2), the fallback.
#
#   ./build/m12-sandbox.sh [1G [off]]      (needs the image; throwaway copy, -snapshot)
#
# Log: $FLFS_WORK/logs/m12-sandbox-<mem>.log; pictures docs/m7/m12-sandbox-*-<mem>.png.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/config.sh"
. "$here/lib.sh"
mem=${1:-1G} sbx=${2:-default}   # sbx=off: the same run with sandbox=off in the user's config, to compare; diag: log the policy decisions; gpu: a virgl GPU (render node inside the sandbox)
work=$FLFS_WORK/scratch/m12-$mem-$sbx
shots=$work/shots
srv=$work/srv
rm -rf "$work"; mkdir -p "$shots" "$srv"
head -c 3145728 /dev/urandom > "$srv/sample.bin"
echo "host sha256: $(sha256sum "$srv/sample.bin" | cut -d' ' -f1)"
port=8099
python3 -m http.server $port --bind 127.0.0.1 --directory "$srv" > "$work/http.log" 2>&1 &
httppid=$!
trap 'kill $httppid 2> /dev/null || true' EXIT
tty=$work/steps.tty
sed -e "s|@S@|$shots|g" -e "s|@HOSTURL@|http://10.0.2.2:$port|g" "$here/m12-sandbox.tty" > "$tty"
cmds=$work/steps.cmds
cp "$here/m12-sandbox.cmds" "$cmds"
[ "$sbx" != gpu ] || export FLFS_VM_VGA="-device virtio-vga-gl -display egl-headless,gl=on"	# a virgl GPU: the render node must be visible inside the sandbox
if [ "$sbx" = diag ]; then # a wrapper that logs the policy decisions and GLib's debug output to /tmp/freebws.err
	echo "mv /usr/bin/freebws /usr/bin/freebws.real && printf '%s\\n' '#!/bin/sh' 'FREEBWS_POLICY_LOG=1 G_MESSAGES_DEBUG=all exec /usr/bin/freebws.real \"\$@\" 2>>/tmp/freebws.err' > /usr/bin/freebws && chmod 755 /usr/bin/freebws && echo wrapper" >> "$cmds"
fi
[ "$sbx" != off ] || echo "mkdir -p /home/tester/.config/freebws && echo sandbox=off > /home/tester/.config/freebws/config && chown -R tester: /home/tester/.config" >> "$cmds"
FLFS_VM_MEM=$mem FLFS_VM_TTY=$tty "$here/gentle.sh" "$here/vm-run.py" "$cmds" -snapshot -usb -device usb-tablet \
	2>&1 | tee "$FLFS_WORK/logs/m12-sandbox-$mem-$sbx.log"
for f in "$shots"/*.ppm; do
	[ -e "$f" ] || continue
	magick "$f" "$FLFS_TOP/docs/m7/m12-sandbox-$(basename "$f" .ppm)-$mem-$sbx.png"
done
echo "server requests: $(grep -c 'GET /sample.bin' "$work/http.log")"
