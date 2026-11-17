#!/bin/bash
# startvm.sh ISO|--stick IMG MEM [gl|std]  -> a persistent VM: sockets ser, mon, qmp in this dir
cd "$(dirname "$0")"
src=("$1"); [ "$1" = --stick ] && { src=(--stick "$2"); shift; } || src=(--iso "$1")
mem=$2; vga=${3:-gl}
rm -f ser mon qmp vars.fd
[ "$vga" = gl ] && export FLFS_VM_VGA="-device virtio-vga-gl,xres=${XRES:-1280},yres=${YRES:-800} -display egl-headless,gl=on" || export FLFS_VM_VGA="-vga std"
export FLFS_VM_MEM=$mem FLFS_VM_VARS=$PWD/vars.fd FLFS_VM_SERIAL=unix:$PWD/ser,server=on,wait=off
exec "${FLFS_BUILD:?}/51-qemu.sh" --serial "${src[@]}" -- -monitor unix:$PWD/mon,server=on,wait=off -qmp unix:$PWD/qmp,server=on,wait=off -usb -device usb-tablet
