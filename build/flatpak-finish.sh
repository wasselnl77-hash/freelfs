#!/bin/bash
# Flatpak + Flathub, everything after the WebKit build, unattended. Waits until
# no build/m8-finish.sh is running (it ends with the image and the ISO), then
# (always through gentle.sh, one job): fetches the new sources, builds the new
# recipes in dependency order, rebuilds flcomp, flpanel and flpackages (and
# the kernel, which needs CONFIG_FUSE_FS: FLATPAK_SKIP_KERNEL=1 leaves it out),
# makes the image and the ISO. Stops at the first failure ("FAILED:" in the
# log); every step is resumable by starting the script again.
# Log: $FLFS_WORK/logs/flatpak-finish.log
#
#   nohup ./build/flatpak-finish.sh > /dev/null 2>&1 &
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/config.sh"
. "$here/lib.sh"
log=$FLFS_WORK/logs/flatpak-finish.log
mkdir -p "$FLFS_WORK/logs"
exec >> "$log" 2>&1
export FLFS_JOBS=1
step() { echo "=== $(date +%T) $*"; }

step "waiting for build/m8-finish.sh"
while pgrep -f 'build/m8-finish.sh' > /dev/null; do sleep 60; done
# a package build of the other script may still be running (it never overlaps ours)
while pgrep -f 'build/60-packages.sh' > /dev/null; do sleep 60; done

step "sources"
"$here/gentle.sh" "$here/02-fetch.sh" || { step "FAILED: sources (02-fetch.sh)"; exit 1; }

# new recipes; 60-packages.sh orders whatever it is given by depends/makedepends
new=(ca-certificates pyparsing curl libseccomp fuse3 bubblewrap xdg-dbus-proxy json-glib
	libarchive npth libassuan libksba gnupg gpgme libfyaml libxmlb appstream gdk-pixbuf
	ostree flatpak xdg-desktop-portal)
step "packages: ${new[*]}"
"$here/gentle.sh" "$here/60-packages.sh" "${new[@]}" || { step "FAILED: new packages (see logs/pkg-*.log)"; exit 1; }

step "packages: flcomp flpanel flpackages"
"$here/gentle.sh" "$here/60-packages.sh" flcomp flpanel flpackages flgreet || { step "FAILED: flcomp flpanel flpackages"; exit 1; }

if [ -z "${FLATPAK_SKIP_KERNEL:-}" ]; then
	step "kernel (CONFIG_FUSE_FS)"
	"$here/gentle.sh" "$here/60-packages.sh" linux || { step "FAILED: linux (see logs/pkg-linux.log)"; exit 1; }
fi

step "image"
"$here/gentle.sh" "$here/50-image.sh" || { step "FAILED: image"; exit 1; }
step "iso"
"$here/gentle.sh" "$here/53-iso.sh" || { step "FAILED: iso"; exit 1; }
step "done"
