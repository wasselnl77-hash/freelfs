#!/bin/bash
# Boot the FreeLFS disk image in QEMU the way a PC boots it: UEFI firmware
# (OVMF) starts flboot from the EFI system partition, flboot starts the
# kernel. KVM is used when /dev/kvm is usable. The serial console is this
# terminal.
#
#   ./build/51-qemu.sh                  graphical window + serial console
#   ./build/51-qemu.sh --serial         serial console only (no window)
#   ./build/51-qemu.sh --iso FILE       boot an ISO (the live ISO) as a CD, no disk
#   ./build/51-qemu.sh -- QEMU-ARGS...  extra arguments for QEMU
#
# FLFS_VM_MEM (default 2G) and FLFS_VM_CPUS (default 2) size the VM;
# FLFS_VM_SERIAL (default mon:stdio) is the -serial backend; FLFS_VM_VGA (default
# "-vga std") the display adapter, e.g. "-device virtio-vga-gl -display egl-headless,gl=on"
# to give the guest a GL GPU (virgl); FLFS_VM_NIC the -nic argument. The firmware's
# settings persist in $FLFS_WORK/ovmf-vars.fd (FLFS_VM_VARS: another file;
# a missing one starts from the firmware defaults). In serial mode, Ctrl-A X
# quits QEMU.

set -euo pipefail
. "$(dirname "$0")/config.sh"
. "$(dirname "$0")/lib.sh"

display=() iso=
while [ $# -gt 0 ]; do
	case $1 in
		--serial) display=(-display none) ;;
		--iso) iso=${2:?usage: --iso FILE}; shift ;;
		--) shift; break ;;
		*) die "usage: $0 [--serial] [-- QEMU-ARGS...]" ;;
	esac
	shift
done

# a display backend of its own in FLFS_VM_VGA (egl-headless for virgl) replaces the "none" of --serial
case ${FLFS_VM_VGA:-} in *-display*) display=() ;; esac

if [ -n "$iso" ]; then
	[ -f "$iso" ] || die "no ISO $iso"
else
	[ -f "$FLFS_IMG" ] || die "no image (run ./build/50-image.sh)"
fi
# "CODE:VARS" pairs, first match wins: Debian/Ubuntu, Arch/Fedora, openSUSE
code= vars_template=
for pair in \
	/usr/share/OVMF/OVMF_CODE_4M.fd:/usr/share/OVMF/OVMF_VARS_4M.fd \
	/usr/share/OVMF/OVMF_CODE.fd:/usr/share/OVMF/OVMF_VARS.fd \
	/usr/share/edk2/x64/OVMF_CODE.4m.fd:/usr/share/edk2/x64/OVMF_VARS.4m.fd \
	/usr/share/edk2/x64/OVMF_CODE.fd:/usr/share/edk2/x64/OVMF_VARS.fd \
	/usr/share/edk2-ovmf/x64/OVMF_CODE.fd:/usr/share/edk2-ovmf/x64/OVMF_VARS.fd \
	/usr/share/qemu/ovmf-x86_64-4m-code.bin:/usr/share/qemu/ovmf-x86_64-4m-vars.bin \
	/usr/share/qemu/ovmf-x86_64-code.bin:/usr/share/qemu/ovmf-x86_64-vars.bin \
	/usr/share/qemu/edk2-x86_64-code.fd:/usr/share/qemu/edk2-i386-vars.fd \
	/usr/share/qemu/OVMF_CODE_4M.fd:/usr/share/qemu/OVMF_VARS_4M.fd \
	/usr/share/qemu/OVMF_CODE.fd:/usr/share/qemu/OVMF_VARS.fd; do
	if [ -z "$code" ] && [ -f "${pair%%:*}" ] && [ -f "${pair##*:}" ]; then
		code=${pair%%:*} vars_template=${pair##*:}
	fi
done
[ -n "$code" ] || die "no OVMF firmware found (install the ovmf package on the host)"
vars=${FLFS_VM_VARS:-$FLFS_WORK/ovmf-vars.fd}
[ -f "$vars" ] || cp "$vars_template" "$vars"

drive=(-drive "file=$FLFS_IMG,format=raw,if=virtio")
[ -z "$iso" ] || drive=(-drive "file=$iso,format=raw,media=cdrom,readonly=on")
accel=()
[ -r /dev/kvm ] && [ -w /dev/kvm ] && accel=(-enable-kvm -cpu host)

exec "qemu-system-$FLFS_ARCH" "${accel[@]}" \
	-m "${FLFS_VM_MEM:-2G}" -smp "${FLFS_VM_CPUS:-2}" \
	-drive "if=pflash,format=raw,readonly=on,file=$code" \
	-drive "if=pflash,format=raw,file=$vars" \
	"${drive[@]}" \
	-nic "${FLFS_VM_NIC:-user,model=virtio-net-pci}" \
	${FLFS_VM_VGA:--vga std} \
	-serial "${FLFS_VM_SERIAL:-mon:stdio}" \
	"${display[@]}" "$@"
