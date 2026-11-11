#!/bin/bash
# Release 11 round: (x11) an X11 client window through Xwayland in the real image, and flreport's tarball;
# (gpu) FreeBWS loading a real https site on a virgl GPU (virtio-vga-gl through the host's GPU, QEMU's egl-headless),
# so flcomp's dmabuf import and the browser's GPU path run for real.
#
#   ./build/m11-x11-gpu.sh x11|gpu [1G]
#
# Needs the image and the xclient disk image $FLFS_WORK/scratch/xtest/x-samples.img (m10-samples.img plus tests/xclient
# built in the chroot, copied in with mcopy). Throwaway image copy (-snapshot). Pictures: docs/m7/m11-<mode>-<name>-<mem>.png.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/config.sh"
. "$here/lib.sh"
mode=${1:?usage: m11-x11-gpu.sh x11|gpu [MEM]} mem=${2:-1G}
img=$FLFS_WORK/scratch/xtest/x-samples.img
[ -f "$img" ] || die "no $img"
shots=$FLFS_WORK/scratch/m11/$mode-$mem
rm -rf "$shots"; mkdir -p "$shots"
tty=$(mktemp "$FLFS_WORK/scratch/m11/tty.XXXXXX")
sed -e "s/@MEM@/$mem/g" -e "s|@S@|$shots|g" "$here/m11-$mode.tty" > "$tty"
[ "$mode" != gpu ] || export FLFS_VM_VGA="-device virtio-vga-gl -display egl-headless,gl=on"
FLFS_VM_MEM=$mem FLFS_VM_TTY=$tty "$here/gentle.sh" "$here/vm-run.py" "$here/m11-x11-gpu.cmds" -snapshot -usb -device usb-tablet \
	-drive "file=$img,format=raw,if=virtio,readonly=on" \
	2>&1 | tee "$FLFS_WORK/logs/m11-$mode-$mem.log"
rm -f "$tty"
for f in "$shots"/*.ppm; do
	[ -e "$f" ] || continue
	magick "$f" "$FLFS_TOP/docs/m7/m11-$mode-$(basename "$f" .ppm)-$mem.png"
	rm -f "$f"
done
if [ "$mode" = gpu ]; then # the pictures came over the console (Snappel, in order of time)
	python3 -I - "$FLFS_WORK/logs/m11-gpu-$mem.log" "$FLFS_TOP/docs/m7" "$mem" <<'PY'
import base64, re, sys
log, out, mem = sys.argv[1:4]
text = re.sub(r"\x1b\[[?0-9;]*[a-zA-Z]", "", open(log, errors="replace").read()).replace("\r", "")
names = ["desktop", "start", "https", "wiki"]
for i, m in enumerate(re.finditer(r"^BEGINPNG \S+\n(.*?)^ENDPNG", text, re.S | re.M)):
    name = names[i] if i < len(names) else "extra%d" % i
    open(f"{out}/m11-gpu-{name}-{mem}.png", "wb").write(base64.b64decode(m.group(1)))
    print("picture", name)
PY
fi
