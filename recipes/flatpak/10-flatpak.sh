# Flatpak: let programs find what Flatpak exports (menu entries, icons, D-Bus
# services) in XDG_DATA_DIRS, for the person's own installs and for
# system-wide ones. SPDX-License-Identifier: MIT
#
# Read by /etc/profile for every login shell. The name sorts before
# desktop.sh on purpose: that file starts the desktop, and nothing after it
# reaches the desktop's environment (the panel's start menu and every program
# it starts, and the session's D-Bus services).
if [ -x /usr/bin/flatpak ]; then
	case ":${XDG_DATA_DIRS:-}:" in
	*":/var/lib/flatpak/exports/share:"*) ;;
	*)
		XDG_DATA_DIRS=${XDG_DATA_HOME:-$HOME/.local/share}/flatpak/exports/share:/var/lib/flatpak/exports/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}
		export XDG_DATA_DIRS
		;;
	esac
fi
