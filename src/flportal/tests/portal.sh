#!/bin/bash
# Headless test of flportal behind the real xdg-desktop-portal: a private session bus, the
# frontend D-Bus activated from a root file system you built (ROOTFS, default
# ~/freelfs-work/rootfs-x86_64), flportal compiled here against that root's headers, the dialogs
# rendered to PPM files (FLPORTAL_SHOT, no compositor) and the answers checked with
# tests/portal-test.py (python3 with PyGObject, which only talks to the bus).
#
#   tests/portal.sh [SCENARIO...]      default: all.  PORTAL_OUT=DIR keeps the PPMs
#
# Everything lives in a scratch directory; HOME, XDG_* and the bus are private. Nothing outside
# is written. Needs gcc, pkg-config, make and python3 on the host.
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
here=$(pwd)
R=${ROOTFS:-$HOME/freelfs-work/rootfs-x86_64}
[ -x "$R/lib64/ld-linux-x86-64.so.2" ] || { echo "no root file system at $R (ROOTFS=...)"; exit 2; }
T=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; if [ -n "${PORTAL_KEEP:-}" ]; then echo "kept $T"; else rm -rf "$T"; fi' EXIT
LD="$R/lib64/ld-linux-x86-64.so.2 --library-path $T/flui:$R/usr/lib"

# ---- build libflui and flportal against the root's headers and libraries ----
mkdir -p "$T/flui" "$T/bin" "$T/build"
cp -p "$here"/../flui/*.c "$here"/../flui/*.h "$here"/../flui/Makefile "$here"/../flui/flui.pc.in \
	"$here"/../flui/README "$T/flui/" 2>/dev/null
cat >"$T/bin/pkg-config" <<EOF
#!/bin/sh
export PKG_CONFIG_LIBDIR=$R/usr/lib/pkgconfig:$R/usr/share/pkgconfig PKG_CONFIG_SYSROOT_DIR=$R
exec /usr/bin/pkg-config "\$@"
EOF
chmod +x "$T/bin/pkg-config"
export PATH=$T/bin:$PATH
nice -n 19 make -C "$T/flui" -j3 CC="gcc --sysroot=$R" >"$T/flui.log" 2>&1 || { tail -20 "$T/flui.log"; echo "FAIL building libflui"; exit 1; }
cp "$here"/*.c "$here"/*.h "$here"/Makefile "$here"/*.in "$T/build/"
# libflui comes from the scratch build above, not from the root (whose copy may be older)
CFLAGS="-O2 -g -I$T/flui" nice -n 19 make -C "$T/build" -j3 CC="gcc --sysroot=$R" PKGS="gio-2.0 gio-unix-2.0" \
	LDFLAGS="-L$T/flui" LDLIBS="-lflui" >"$T/build.log" 2>&1 || { cat "$T/build.log"; echo "FAIL building flportal"; exit 1; }
if grep -q warning "$T/build.log"; then grep -B1 -A3 warning "$T/build.log" | head -40; fi

# ---- fonts for the pictures (fontconfig reads the root's fonts, not the host's) ----
cat >"$T/fonts.conf" <<EOF
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig><dir>$R/usr/share/fonts</dir><cachedir>$T/fontcache</cachedir></fontconfig>
EOF

# ---- the "installed" files ----
mkdir -p "$T/share/dbus-1/services" "$T/share/xdg-desktop-portal/portals" "$T/home" "$T/rt" "$T/cfg" "$T/shots"
chmod 700 "$T/rt"
cp "$here/freelfs.portal" "$T/share/xdg-desktop-portal/portals/"
mkdir -p "$T/cfg/xdg-desktop-portal"
cp "$here/freelfs-portals.conf" "$T/share/xdg-desktop-portal/portals/freelfs-portals.conf" # XDG_DESKTOP_PORTAL_DIR holds the configs too
wrap() { # NAME BINARY [ARGS]: a D-Bus service whose program runs through the root's loader
	local svc=$1 bin=$2
	printf '#!/bin/sh\nexec %s %s "$@"\n' "$LD" "$bin" >"$T/bin/run-$svc"
	chmod +x "$T/bin/run-$svc"
	printf '[D-BUS Service]\nName=%s\nExec=%s/bin/run-%s\n' "$svc" "$T" "$svc" >"$T/share/dbus-1/services/$svc.service"
}
wrap org.freedesktop.portal.Desktop "$R/usr/libexec/xdg-desktop-portal -v"
wrap org.freedesktop.impl.portal.PermissionStore "$R/usr/libexec/xdg-permission-store"
wrap org.freedesktop.impl.portal.desktop.freelfs "$T/build/flportal -v"
# the document portal needs FUSE; the file chooser works without it for programs outside a sandbox
cat >"$T/bus.conf" <<XML
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>session</type><keep_umask/><listen>unix:path=$T/bus</listen><auth>EXTERNAL</auth>
<servicedir>$T/share/dbus-1/services</servicedir>
<policy context="default"><allow send_destination="*" eavesdrop="true"/><allow eavesdrop="true"/><allow own="*"/></policy></busconfig>
XML

# a fake snappel, and a folder with known contents for the file dialogs
mkdir -p "$T/snapbin"
printf '#!/bin/sh\nwhile [ $# -gt 0 ]; do case $1 in --output) out=$2; shift;; esac; shift; done\nprintf "\\211PNG fake" > "$out"\n' >"$T/snapbin/snappel"
chmod +x "$T/snapbin/snappel"
D=$T/home/Documents
mkdir -p "$D/Reports" "$D/Photos 2026" "$T/home/Downloads" "$T/home/Pictures" "$T/home/Music"
for f in notes.txt todo.md budget.csv; do echo x >"$D/$f"; done
head -c 48211 /dev/zero >"$D/holiday.png"; head -c 912 /dev/zero >"$D/logo.svg"
head -c 3400000 /dev/zero >"$D/report-2026.pdf"; head -c 200 /dev/zero >"$D/.hidden-config"
cat >"$T/cfg/user-dirs.dirs" <<EOF
XDG_DOCUMENTS_DIR="\$HOME/Documents"
XDG_DOWNLOAD_DIR="\$HOME/Downloads"
XDG_PICTURES_DIR="\$HOME/Pictures"
XDG_MUSIC_DIR="\$HOME/Music"
EOF
touch -d '2026-11-02 14:20' "$D/notes.txt" "$D/holiday.png"; touch -d '2026-10-12 09:05' "$D/todo.md" "$D/Reports"
touch -d '2026-08-30 18:44' "$D/budget.csv" "$D/report-2026.pdf" "$D/Photos 2026"; touch -d '2026-06-01 10:00' "$D/logo.svg"

fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

start_bus() { # [ENV=VALUE ...]: a fresh bus, the same for every scenario
	[ -n "$busp" ] && { kill "$busp" 2>/dev/null; wait "$busp" 2>/dev/null; }
	rm -f "$T/bus"
	env "$@" $LD "$R/usr/bin/dbus-daemon" --config-file="$T/bus.conf" --nofork --nopidfile 2>"$T/bus.log" &
	busp=$!
	local i=0
	while [ ! -S "$T/bus" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
}
busp=
export HOME=$T/home XDG_CONFIG_HOME=$T/cfg XDG_DATA_HOME=$T/data XDG_CACHE_HOME=$T/cache XDG_RUNTIME_DIR=$T/rt
export XDG_DATA_DIRS=$T/share:$R/usr/share XDG_DESKTOP_PORTAL_DIR=$T/share/xdg-desktop-portal/portals
export XDG_CURRENT_DESKTOP=FreeLFS FLUI_CONF=$T/desktop.conf FONTCONFIG_FILE=$T/fonts.conf
export DBUS_SESSION_BUS_ADDRESS=unix:path=$T/bus FLPORTAL_VERBOSE=1
export TEST_TMP=$T LD_FOR_TEST="$LD"

# a few programs for the app chooser: the repo's own .desktop files, FreeBWS handling https
mkdir -p "$T/share/applications"
cp "$here"/../fledit/fledit.desktop "$here"/../flfiles/flfiles.desktop "$here"/../flterm/*.desktop "$T/share/applications/" 2>/dev/null
cp "$here"/../freebws/*.desktop "$T/share/applications/" 2>/dev/null
printf '[MIME Cache]\nx-scheme-handler/https=freebws.desktop;\nx-scheme-handler/http=freebws.desktop;\n' >"$T/share/applications/mimeinfo.cache"
# GLib hides programs whose command is not in PATH: stand-ins for them
mkdir -p "$T/appbin"
for p in fledit flfiles flterm freebws; do printf '#!/bin/sh\n' >"$T/appbin/$p"; chmod +x "$T/appbin/$p"; done
[ -e "$T/share/applications/freebws.desktop" ] ||
	printf '[Desktop Entry]\nType=Application\nName=FreeBWS\nComment=The FreeLFS web browser\nExec=freebws %%u\nMimeType=x-scheme-handler/https;x-scheme-handler/http;\n' >"$T/share/applications/freebws.desktop"

want=("$@")
[ ${#want[@]} -eq 0 ] && want=(settings notification shots overlay-confirm overlay-popup overlay-prompt open-multiple save-file save-files open-folder appchooser screenshot screenshot-none access-deny)
for sc in "${want[@]}"; do
	env_extra=(FLPORTAL_SHOT="$T/shots/$sc")
	case $sc in
	open-multiple) env_extra+=(FLPORTAL_TEST_ACCEPT=1 FLPORTAL_TEST_SELECT=notes.txt,todo.md) ;;
	save-file) env_extra+=(FLPORTAL_TEST_ACCEPT=1 FLPORTAL_TEST_NAME=minutes.txt) ;;
	save-files | open-folder | appchooser | screenshot) env_extra+=(FLPORTAL_TEST_ACCEPT=1) ;;
	esac
	env_extra+=(PATH="$T/appbin:/usr/bin:/bin")
	[ "$sc" = screenshot ] && env_extra+=(PATH="$T/snapbin:$T/appbin:/usr/bin:/bin")
	[ "$sc" = open-folder ] && env_extra+=(FLPORTAL_TEST_SELECT=Reports)
	[ "$sc" = screenshot-none ] && env_extra+=(FLPORTAL_TEST_ACCEPT=1)
	case $sc in
	overlay-confirm) env_extra+=(FLPORTAL_TEST_OVERLAY=confirm FLPORTAL_TEST_NAME=notes.txt) ;;
	overlay-popup) env_extra+=(FLPORTAL_TEST_OVERLAY=popup) ;;
	overlay-prompt) env_extra+=(FLPORTAL_TEST_OVERLAY=prompt) ;;
	esac
	export XDG_DATA_HOME=$T/data-$sc
	rm -f "$T/desktop.conf"
	start_bus "${env_extra[@]}"
	python3 -I "$here/tests/portal-test.py" "$sc" 2>"$T/test-$sc.err"
	rc=$?
	if [ $rc -eq 0 ]; then ok "$sc"; else bad "$sc"; sed 's/^/     /' "$T/test-$sc.err" | tail -15; grep -h "flportal\|xdg-desktop-portal" "$T/bus.log" 2>/dev/null | tail -5; fi
done
[ -n "$busp" ] && { kill "$busp" 2>/dev/null; }

if [ -n "${PORTAL_OUT:-}" ]; then
	mkdir -p "$PORTAL_OUT"
	cp "$T"/shots/* "$PORTAL_OUT/" 2>/dev/null
fi
echo "$fails failed"
[ "$fails" -eq 0 ]
