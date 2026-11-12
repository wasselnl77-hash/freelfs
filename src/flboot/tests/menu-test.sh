#!/bin/bash
# Headless test of the boot menu: flboot's real menu code on a stand-in firmware
# (tests/hostmenu.c). Writes screenshots when given a directory.
# usage: tests/menu-test.sh [SHOTDIR]   (from src/flboot; FONT= the psfu font)
# SPDX-License-Identifier: MIT
set -u
cd "$(dirname "$0")/.."
shots=${1:-}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fails=0
ok() { printf '  %-62s ok\n' "$1"; }
bad() { printf '  %-62s FAIL\n' "$1"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

FONT=${FONT:-/usr/share/consolefonts/Lat2-Terminus16.psfu.gz}
python3 mkfont.py "$FONT" > "$work/font.c" || { echo "no font"; exit 1; }
ren="-Dmemcpy=fl_memcpy -Dmemmove=fl_memmove -Dmemset=fl_memset -Dmemcmp=fl_memcmp -Dstrlen=fl_strlen"
for f in lib conf gfx menu; do
	gcc -O1 -g -std=c17 -fshort-wchar -ffreestanding -fno-builtin -fno-strict-aliasing -fPIC -Wall -Wno-unused-parameter $ren -I. -c -o "$work/$f.o" $f.c || exit 1
done
gcc -O1 -std=c17 -fshort-wchar -fPIC -ffreestanding -fno-builtin -c -o "$work/font.o" "$work/font.c" || exit 1
gcc -O1 -g -std=gnu17 -fshort-wchar -Wall -Wno-unused-parameter $ren -I. -o "$work/hostmenu" tests/hostmenu.c "$work"/{lib,conf,gfx,menu,font}.o || exit 1
hm() { "$work/hostmenu" "$@"; }

esp=$work/esp
mkdir -p "$esp/flboot"
: > "$esp/vmlinuz-6.12.1"; : > "$esp/vmlinuz-6.14.0"
printf 'timeout 5\ncmdline root=PARTUUID=1234 rootwait ro rootflags=subvol=@ quiet\n' > "$esp/flboot/flboot.conf"

# no snapshots: the menu has no "earlier state" row
out=$(FLBOOT_TEXT=1 hm "$esp" 800x600 e)
check "without snapshots there is no earlier-state entry" '! grep -q "earlier state" <<< "$out" && grep -q "chosen: 0" <<< "$out"'

cat > "$esp/flboot/snapshots" <<'EOS'
# Written by flsnap
@snapshots/9 2026-11-10 23:27  before going back to #7
@snapshots/8 2026-11-10 22:10  flpkg: upgrade glibc, install gdb
@snapshots/7 2026-11-09 08:55  flpkg: upgrade linux, flboot, flsnap, flpackages and a very long list of further packages
@snapshots/6 2026-11-08 19:03  manual
EOS
out=$(FLBOOT_TEXT=1 hm "$esp" 800x600 e)
check "main menu: two kernels and the earlier-state entry" 'grep -q "FreeLFS (kernel 6.14.0)" <<< "$out" && grep -q "older kernel 6.12.1" <<< "$out" && grep -q "Start an earlier state" <<< "$out"'
check "snapshots are not in the main list" '! grep -q "#9" <<< "$out"'
check "Enter on the first kernel boots entry 0" 'grep -q "chosen: 0" <<< "$out"'
out=$(FLBOOT_TEXT=1 hm "$esp" 800x600 t t t t t)
check "the countdown boots the default" 'grep -q "chosen: 0" <<< "$out"'

# up from the first row wraps to the earlier-state entry; Enter opens the list
out=$(FLBOOT_TEXT=1 hm "$esp" 800x600 u e)
check "Up from the top selects 'Start an earlier state', Enter lists snapshots" 'grep -q "Choose a snapshot of the system" <<< "$out" && grep -q "#8  2026-11-10 22:10  flpkg: upgrade glibc" <<< "$out"'
out=$(FLBOOT_TEXT=1 hm "$esp" 800x600 u e e e)
check "Enter on #9 shows the notice" 'grep -q "read-only: nothing you change there is kept" <<< "$out" && grep -q "Make this the current state" <<< "$out"'
# entries: 0 kernel, 1 older kernel, 2.. snapshots in file order (#9 = 2)
out=$(FLBOOT_TEXT=1 hm "$esp" 800x600 u e d e e)
check "second snapshot (#8) is entry 3" 'grep -q "chosen: 3" <<< "$out"'
out=$(FLBOOT_TEXT=1 hm "$esp" 800x600 u e e d e e e)
check "Back in the notice returns to the list; nothing booted until confirmed" '[ "$(grep -c "chosen" <<< "$out")" = 1 ] && grep -q "chosen: 2" <<< "$out"'
out=$(FLBOOT_TEXT=1 hm "$esp" 800x600 u e x d e)
check "Esc goes back to the main menu" 'grep -q "chosen: 0" <<< "$out"'
out=$(FLBOOT_TEXT=1 hm "$esp" 800x600 u e u e d e)
check "Back row of the list returns to the main menu" 'grep -q "chosen: 0" <<< "$out"'

# the command line of a snapshot: rootflags replaced, the newest kernel
out=$(FLBOOT_TEXT=1 hm "$esp" 800x600 u e d e e)
check "snapshot #8 boots with rootflags=subvol=@snapshots/8, not @" 'grep -q "^cmdline: root=PARTUUID=1234 rootwait ro quiet rootflags=subvol=@snapshots/8$" <<< "$out"'
check "... and its title is #8 with date and reason" 'grep -q "^title: #8  2026-11-10 22:10  flpkg: upgrade glibc, install gdb$" <<< "$out"'
out=$(FLBOOT_TEXT=1 hm "$esp" 800x600 e)
check "a normal entry keeps rootflags=subvol=@" 'grep -q "rootflags=subvol=@ quiet" <<< "$out"'

# the LTS kernel (package linux-lts: vmlinuz-VERSION-freelfs-lts) is a secondary entry, whatever its
# version number; the default and the snapshots use the newest of the others
esp2=$work/esp2
mkdir -p "$esp2/flboot"
: > "$esp2/vmlinuz-7.3.4-freelfs"; : > "$esp2/vmlinuz-6.18.63-freelfs-lts"; : > "$esp2/vmlinuz-7.2.15-freelfs"
cp "$esp/flboot/flboot.conf" "$esp/flboot/snapshots" "$esp2/flboot/"
out=$(FLBOOT_TEXT=1 hm "$esp2" 800x600 e)
check "LTS: the default is the newest normal kernel" 'grep -q "^title: FreeLFS (kernel 7.3.4-freelfs)$" <<< "$out"'
out=$(FLBOOT_TEXT=1 hm "$esp2" 800x600 d d e)
check "LTS: the older normal kernel comes second, the LTS kernel third" 'grep -q "^title: FreeLFS (LTS kernel 6.18.63-freelfs-lts)$" <<< "$out" && grep -q "chosen: 2" <<< "$out"'
out=$(FLBOOT_TEXT=1 hm "$esp2" 800x600 d e)
check "LTS: second entry is the older normal kernel" 'grep -q "^title: FreeLFS (older kernel 7.2.15-freelfs)$" <<< "$out"'

if [ -n "$shots" ]; then
	mkdir -p "$shots"
	P=$shots/snap-boot
	hm "$esp" 1024x768 s:$P-menu u s:$P-menu-earlier e s:$P-list d d s:$P-list-2 e s:$P-notice e > /dev/null
	hm "$esp" 640x480 u e s:$P-list-640 e s:$P-notice-640 e > /dev/null
	for f in "$shots"/snap-boot-*.ppm; do
		python3 tests/ppm2png.py "$f" "${f%.ppm}.png" && rm -f "$f"
	done
	ls "$shots"/snap-boot-*.png
fi
if [ "$fails" -ne 0 ]; then echo "$fails flboot menu test(s) failed"; exit 1; fi
echo "flboot menu tests passed"
