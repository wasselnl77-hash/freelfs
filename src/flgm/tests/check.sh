#!/bin/sh
# General Manager data-layer tests: sys.c, proc.c and startup.c against the host's /proc and /sys and a
# scratch autostart directory (HOME and XDG_* point into a temp dir, nothing outside it is touched), then
# against a fake /proc and /sys tree built in the same scratch directory (fake-test.c).
# Needs only a C compiler; CC may carry --sysroot. For the window itself see tests/e2e.sh.
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
export HOME=$dir XDG_CONFIG_HOME=$dir/cfg XDG_CONFIG_DIRS=$dir/etc XDG_DATA_HOME=$dir/data XDG_DATA_DIRS=$dir/share
mkdir -p "$dir/cfg/autostart" "$dir/etc/autostart"
printf '[Desktop Entry]\nType=Application\nName=Alpha\nExec=alpha --serve\n' >"$dir/cfg/autostart/alpha.desktop"
printf '[Desktop Entry]\nType=Application\nName=Beta\nExec=beta\nHidden=true\n' >"$dir/cfg/autostart/beta.desktop"
printf '[Desktop Entry]\nType=Application\nName=System one\nExec=sysone\n' >"$dir/etc/autostart/sys-one.desktop"
CFLAGS_T="-std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -O1"
${CC:-cc} $CFLAGS_T -o "$dir/data-test" tests/data-test.c sys.c proc.c startup.c -lm || exit 1
"$dir/data-test" || exit 1
# the fake tree: PROC_DIR and SYS_DIR are compile-time roots, so the test is built against scratch paths
mkdir -p "$dir/fake/proc" "$dir/fake/sys"
${CC:-cc} $CFLAGS_T "-DPROC_DIR=\"$dir/fake/proc\"" "-DSYS_DIR=\"$dir/fake/sys\"" -o "$dir/fake-test" \
	tests/fake-test.c sys.c proc.c startup.c -lm || exit 1
"$dir/fake-test"
