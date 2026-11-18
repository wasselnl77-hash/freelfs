#!/bin/bash
# flpkg sources -- one command for all software (round 22). flpkg runs this for
#   install, remove, upgrade, search, list
# when a person types them; /usr/bin/flpkg (the packages of the FreeLFS repo) stays the engine
# for the FreeLFS source. The other sources:
#   flatpak   Flathub, per user (flatpak --user: no root)
#   nix       nixpkgs, if the package "nix" is installed
#   apt, pacman, dnf   Distrobox containers (Ubuntu, Arch, Fedora), if "distrobox" is installed
# Order everywhere: FreeLFS, Flathub, Nix, Ubuntu, Arch, Fedora.
#
#   flpkg install [--from=freelfs|flatpak|nix|apt|pacman|dnf] [--yes] NAME...
#   flpkg remove  [--from=...] NAME...   uses the source that installed NAME
#   flpkg upgrade                        every source's own update
#   flpkg search TEXT                    all sources (FreeLFS and Flathub first)
#   flpkg list                           what is installed, from every source
#
# Which source installed what is written to $STATE/sources.list (per person; the FreeLFS packages are
# in flpkg's own database and need no entry). Tools that read flpkg's output (the Store, Packages) get
# the plain FreeLFS output for search and list when stdout is not a terminal; --all forces all sources.
#
# Test hooks (environment): FLPKG_NATIVE (the real flpkg), FLPKG_SEAT, FLPKG_FLATPAK, FLPKG_NIX,
# FLPKG_DISTROBOX, FLPKG_STATE (state directory), FLPKG_LIVE_DIR (default /run/live), FLPKG_ASSUME_TTY=1.
# SPDX-License-Identifier: MIT

NATIVE=${FLPKG_NATIVE:-/usr/bin/flpkg}
SEAT=${FLPKG_SEAT:-/usr/bin/flpkg-seat}
FLATPAK=${FLPKG_FLATPAK:-flatpak}
NIX=${FLPKG_NIX:-nix}
DBX=${FLPKG_DISTROBOX:-distrobox}
LIVE=${FLPKG_LIVE_DIR:-/run/live}
STATE=${FLPKG_STATE:-${XDG_STATE_HOME:-${HOME:-/tmp}/.local/state}/flpkg}
export FLPKG_NO_SOURCES=1

# the containers: backend name, box name, image (pinned to a release, not "latest" where there is a choice)
box_name() { echo "freelfs-$1"; }
box_image() {
	case $1 in
		apt) echo docker.io/library/ubuntu:24.04 ;;
		pacman) echo docker.io/library/archlinux:latest ;;
		dnf) echo registry.fedoraproject.org/fedora:42 ;;
	esac
}
box_label() { case $1 in apt) echo "Ubuntu (apt)" ;; pacman) echo "Arch (pacman)" ;; dnf) echo "Fedora (dnf)" ;; esac; }
box_size() { case $1 in apt) echo "about 80 MB" ;; pacman) echo "about 150 MB" ;; dnf) echo "about 200 MB" ;; esac; }
label() {
	case $1 in
		freelfs) echo "FreeLFS repository" ;; flatpak) echo "Flathub (Flatpak)" ;; nix) echo "Nix (nixpkgs)" ;;
		apt | pacman | dnf) box_label "$1" ;;
	esac
}
ORDER="freelfs flatpak nix apt pacman dnf"

say() { printf 'flpkg: %s\n' "$*" >&2; }
die() { say "$*"; exit 1; }
have() { command -v "$1" > /dev/null 2>&1; }
is_tty() { [ -n "${FLPKG_ASSUME_TTY:-}" ] || [ -t 0 ]; }
out_tty() { [ -n "${FLPKG_ASSUME_TTY:-}" ] || [ -t 1 ]; }

# ---- arguments ---------------------------------------------------------------------------
CMD=$1
shift
FROM=
YES=
ALL=
NAMES=()
PASS=()
# options of the native flpkg that are passed on to it (FreeLFS source only); anything else is refused
while [ $# -gt 0 ]; do
	a=$1
	shift
	case $a in
		--from=*) FROM=${a#--from=} ;;
		--yes | -y) YES=1 ;;
		--all) ALL=1 ;;
		--sync) ;; # upgrade always syncs
		--overwrite) [ $# -gt 0 ] || die "--overwrite needs a GLOB"; PASS+=("$a" "$1"); shift ;;
		--nodeps | --unsigned | --needed | --asdeps | --cascade | --recursive) PASS+=("$a") ;;
		-*) die "unknown option $a for flpkg $CMD (see 'flpkg help')" ;;
		*) NAMES+=("$a") ;;
	esac
done
case $FROM in
	'' | freelfs | flatpak | nix | apt | pacman | dnf) ;;
	flathub) FROM=flatpak ;;
	*) die "unknown source '$FROM' (freelfs, flatpak, nix, apt, pacman, dnf)" ;;
esac

# ---- the record of what installed what -----------------------------------------------------
# lines: BACKEND TAB ID TAB ALIAS
record_add() {
	mkdir -p "$STATE" 2> /dev/null || return 0
	record_del "$1" "$2"
	printf '%s\t%s\t%s\n' "$1" "$2" "$3" >> "$STATE/sources.list"
}
record_del() {
	[ -f "$STATE/sources.list" ] || return 0
	awk -F'\t' -v b="$1" -v i="$2" '!($1 == b && $2 == i)' "$STATE/sources.list" > "$STATE/sources.list.new" &&
		mv "$STATE/sources.list.new" "$STATE/sources.list"
}
record_find() { # record_find NAME -> "backend<TAB>id" lines
	[ -f "$STATE/sources.list" ] || return 0
	awk -F'\t' -v n="$1" '$3 == n || $2 == n { print $1 "\t" $2 }' "$STATE/sources.list"
}

# ---- FreeLFS (the native flpkg) -----------------------------------------------------------
native_priv() { # install|remove NAME... | upgrade
	if [ "$(id -u)" = 0 ]; then
		case $1 in
			upgrade) "$NATIVE" upgrade -y --sync ${PASS[@]+"${PASS[@]}"} ;;
			*) local c=$1; shift; "$NATIVE" "$c" -y ${PASS[@]+"${PASS[@]}"} -- "$@" ;;
		esac
	elif [ "${#PASS[@]}" -gt 0 ]; then
		die "${PASS[*]}: these options need root (flpkg-seat, the password-free route for a person, takes package names only). Run as root."
	elif [ -x "$SEAT" ]; then
		"$SEAT" "$@" # asks once for the password through flauth (none on the live tester)
	else
		die "installing FreeLFS packages needs root and $SEAT is not installed"
	fi
}
native_installed() { "$NATIVE" list 2> /dev/null | awk -v n="$1" '$1 == n { f = 1 } END { exit !f }'; }

# search_freelfs TERM -> "freelfs<TAB>id<TAB>version<TAB>summary"
search_freelfs() {
	"$NATIVE" search "$1" 2> /dev/null | awk -v t="$1" '
		function emit() { if (id != "") print "freelfs\t" id "\t" ver "\t" sum }
		/^[^ ]/ { emit(); split($1, p, "/"); id = p[2]; ver = $2; sum = ""; next }
		/^ / { sub(/^ +/, ""); sum = $0 }
		END { emit() }' | exact_first "$1"
}
# exact name matches only when there are any; else the first five
exact_first() {
	local t=$1 all
	all=$(cat)
	[ -n "$all" ] || return 0
	local ex
	ex=$(printf '%s\n' "$all" | awk -F'\t' -v t="$t" 'tolower($2) == tolower(t)')
	if [ -n "$ex" ]; then printf '%s\n' "$ex"; else printf '%s\n' "$all" | head -n 5; fi
}

# ---- Flathub ------------------------------------------------------------------------------
fp_ok() { have "$FLATPAK"; }
fp_remote() {
	"$FLATPAK" remotes --user --columns=name 2> /dev/null | grep -qx flathub && return 0
	say "adding the Flathub remote for your account (flatpak --user)"
	"$FLATPAK" remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
}
# "flatpak<TAB>app.id<TAB>version<TAB>name: description"; a match on the app's last name part or its name
search_flatpak() {
	fp_ok || return 0
	fp_remote > /dev/null 2>&1 || return 0
	"$FLATPAK" search --user --columns=application,name,version,description "$1" 2> /dev/null |
		awk -F'\t' -v t="$1" '$1 ~ /\./ { print "flatpak\t" $1 "\t" $3 "\t" $2 " - " $4 }' |
		exact_flatpak "$1"
}
exact_flatpak() {
	local t=$1 all ex
	all=$(cat)
	[ -n "$all" ] || return 0
	ex=$(printf '%s\n' "$all" | awk -F'\t' -v t="$t" '{ n = split($2, p, "."); if (tolower(p[n]) == tolower(t)) print }')
	if [ -n "$ex" ]; then printf '%s\n' "$ex"; else printf '%s\n' "$all" | head -n 5; fi
}
# live system without persistence: Flatpak's data lives in RAM until shutdown
live_ram() { [ -e "$LIVE" ] && ! mountpoint -q "$LIVE/persist" 2> /dev/null && ! grep -q " $LIVE/persist " /proc/mounts 2> /dev/null; }
fp_size() { # installed size of an app, as flatpak prints it ("1.2 GB"), or empty
	"$FLATPAK" remote-info --user flathub "$1" 2> /dev/null | awk -F: '/^ *Installed/ { gsub(/^ +/, "", $2); print $2; exit }'
}
warn_ram() { # warn_ram NAME SIZE
	say "this is a live system without persistence: $1 (${2:-size unknown, often a few GB}) is installed into RAM and is gone at shutdown."
	say "free RAM now: $(awk '/MemAvailable/ { printf "%d MB", $2 / 1024 }' /proc/meminfo)"
	if ! confirm "Install anyway?" n; then die "cancelled"; fi
}

# ---- Nix ------------------------------------------------------------------------------------
nix_ok() { have "$NIX"; }
nixc() { "$NIX" --extra-experimental-features 'nix-command flakes' "$@"; }
search_nix() {
	nix_ok || return 0
	nixc search nixpkgs "$1" --json 2> /dev/null | grep -o '"legacyPackages\.[^"]*":{[^}]*}' | head -n 20 |
		sed -E 's/^"legacyPackages\.[^.]*\.([^"]*)":\{.*"version":"([^"]*)".*"description":"([^"]*)".*/nix\t\1\t\2\t\3/' |
		grep -P '^nix\t' | exact_first_col "$1"
}
exact_first_col() { # like exact_first, for lines already starting with the backend
	local t=$1 all ex
	all=$(cat)
	[ -n "$all" ] || return 0
	ex=$(printf '%s\n' "$all" | awk -F'\t' -v t="$t" 'tolower($2) == tolower(t)')
	if [ -n "$ex" ]; then printf '%s\n' "$ex"; else printf '%s\n' "$all" | head -n 5; fi
}

# ---- Distrobox ------------------------------------------------------------------------------
dbx_ok() { have "$DBX"; }
box_exists() { dbx_ok && "$DBX" list --no-color 2> /dev/null | awk -F'|' -v n="$(box_name "$1")" '{ gsub(/ /, "", $2) } $2 == n { f = 1 } END { exit !f }'; }
box_run() { local b=$1; shift; "$DBX" enter -n "$(box_name "$b")" -- "$@"; }
box_search_cmd() {
	case $1 in
		apt) echo "apt-cache search --names-only" ;;
		pacman) echo "pacman -Ss" ;;
		dnf) echo "dnf -q search" ;;
	esac
}
search_box() { # search_box BACKEND TERM -> "backend<TAB>name<TAB><TAB>summary" (existing boxes only)
	box_exists "$1" || return 0
	# shellcheck disable=SC2046
	box_run "$1" $(box_search_cmd "$1") "$2" 2> /dev/null | awk -v b="$1" '
		b == "apt" { n = $1; $1 = ""; $2 = ""; print b "\t" n "\t\t" $0 }
		b == "pacman" && /^[a-z]+\// { split($1, p, "/"); n = p[2]; v = $2; getline s; print b "\t" n "\t" v "\t" s }
		b == "dnf" && /^[A-Za-z0-9._+-]+\.[a-z0-9_]+ *:/ { split($1, p, "."); print b "\t" p[1] "\t\t" substr($0, index($0, ":") + 2) }' |
		sed -E 's/\t +/\t/' | exact_first_col "$2"
}
box_create() { # box_create BACKEND: asks first
	local b=$1 img
	box_exists "$b" && return 0
	dbx_ok || die "$(label "$b") needs the package distrobox (flpkg install distrobox) and podman"
	img=$(box_image "$b")
	say "no $(label "$b") container yet. Creating one downloads $img ($(box_size "$b")) and sets it up inside your home."
	confirm "Create the $(label "$b") container?" n || die "cancelled"
	"$DBX" create --yes -n "$(box_name "$b")" -i "$img" || die "creating the container failed"
}
box_install() { # box_install BACKEND PKG
	local b=$1 p=$2
	box_create "$b" || return 1
	case $b in
		apt) box_run "$b" sudo apt-get install -y "$p" ;;
		pacman) box_run "$b" sudo pacman -S --noconfirm "$p" ;;
		dnf) box_run "$b" sudo dnf install -y "$p" ;;
	esac || return 1
	# apps into the start menu (a package with a .desktop file), commands into ~/.local/bin
	box_run "$b" distrobox-export --app "$p" > /dev/null 2>&1 || true
	box_run "$b" distrobox-export --bin "$(box_run "$b" command -v "$p" 2> /dev/null)" --export-path "${HOME:-/tmp}/.local/bin" > /dev/null 2>&1 || true
}
box_remove() {
	local b=$1 p=$2
	box_run "$b" distrobox-export --app "$p" --delete > /dev/null 2>&1 || true
	case $b in
		apt) box_run "$b" sudo apt-get remove -y "$p" ;;
		pacman) box_run "$b" sudo pacman -R --noconfirm "$p" ;;
		dnf) box_run "$b" sudo dnf remove -y "$p" ;;
	esac
}

# ---- asking --------------------------------------------------------------------------------
confirm() { # confirm QUESTION [default y|n]
	local d=${2:-y} r
	[ -n "$YES" ] && return 0
	is_tty || { [ "$d" = y ]; return; }
	read -r -p "$1 [$([ "$d" = y ] && echo Y/n || echo y/N)] " r
	case ${r:-$d} in y | Y | yes) return 0 ;; *) return 1 ;; esac
}

# choose WHAT LINES -> the chosen line; LINES are "backend<TAB>id<TAB>version<TAB>text"
choose() {
	local what=$1 lines=$2 n i=1 line pick=1 first
	n=$(printf '%s\n' "$lines" | wc -l)
	if [ "$n" -eq 1 ]; then printf '%s\n' "$lines"; return; fi
	say "'${NAMES[0]:-?}' can be $what from $n places:" >&2
	while IFS=$'\t' read -r b id ver txt; do
		printf '  %d) %-20s %-34s %s%s\n' "$i" "$(label "$b")" "$id${ver:+ $ver}" "$([ "$i" = 1 ] && echo '(preselected) ')" "${txt:0:50}" >&2
		i=$((i + 1))
	done <<< "$lines"
	if is_tty && [ -z "$YES" ]; then
		read -r -p "Which one? [1] " pick
		pick=${pick:-1}
		case $pick in '' | *[!0-9]*) die "not a number" ;; esac
		[ "$pick" -ge 1 ] && [ "$pick" -le "$n" ] || die "no such choice"
	fi
	line=$(printf '%s\n' "$lines" | sed -n "${pick}p")
	first=$(printf '%s\n' "$lines" | sed -n 1p)
	[ -n "$line" ] || line=$first
	printf '%s\n' "$line"
}

# ---- gathering -----------------------------------------------------------------------------
# gather TERM -> lines in source order. With --from only that source (a box is created when asked for).
gather() {
	local t=$1 b
	for b in $ORDER; do
		[ -z "$FROM" ] || [ "$FROM" = "$b" ] || continue
		case $b in
			freelfs) search_freelfs "$t" ;;
			flatpak) search_flatpak "$t" ;;
			nix) search_nix "$t" ;;
			*) search_box "$b" "$t" ;;
		esac
	done
}

do_install_one() { # do_install_one NAME
	local name=$1 lines line b id native_hit fast=
	case $name in
		*.flpkg) # a package file: only the native flpkg installs it, and only as root
			[ "$(id -u)" = 0 ] || die "$name: installing a package file needs root (flpkg-seat takes repo package names only)"
			"$NATIVE" install -y ${PASS[@]+"${PASS[@]}"} -- "$name"
			return ;;
	esac
	if [ -z "$FROM" ]; then
		# a name the FreeLFS repo has exactly, and no terminal to ask on (the Store, scripts): no network search
		lines=$(search_freelfs "$name" | awk -F'\t' -v t="$name" 'tolower($2) == tolower(t)')
		if [ -n "$lines" ] && ! is_tty; then fast=1; fi
	fi
	if [ -z "$fast" ]; then
		if [ -n "$FROM" ] && [ "$FROM" != freelfs ] && [ "$FROM" != flatpak ] && [ "$FROM" != nix ]; then
			# --from=apt|pacman|dnf: the package name as typed, in a box that is created if needed
			lines=$(printf '%s\t%s\t\t\n' "$FROM" "$name")
			box_exists "$FROM" && lines=$(search_box "$FROM" "$name" | awk -F'\t' -v t="$name" 'tolower($2) == tolower(t)')
			[ -n "$lines" ] || lines=$(printf '%s\t%s\t\t\n' "$FROM" "$name")
		else
			lines=$(gather "$name")
			if [ -z "$lines" ] && [ -z "$FROM" ] && dbx_ok && is_tty; then
				say "$name: not found in FreeLFS, Flathub or Nix."
				for b in apt pacman dnf; do
					lines=$(printf '%s\t%s\t\t\n' "$b" "$name")
					confirm "Try $(label "$b")?" n && break
					lines=
				done
			fi
		fi
	else
		lines=$(printf 'freelfs\t%s\t\t\n' "$name")
	fi
	[ -n "$lines" ] || { say "$name: nothing found${FROM:+ in $(label "$FROM")}"; return 1; }
	line=$(choose installed "$lines") || return 1
	IFS=$'\t' read -r b id _ < <(printf '%s\n' "$line")
	say "installing $id from $(label "$b")"
	case $b in
		freelfs) native_priv install "$id" ;;
		flatpak)
			fp_ok || die "Flatpak is not installed"
			fp_remote || die "cannot add Flathub"
			if live_ram; then warn_ram "$id" "$(fp_size "$id")"; fi
			"$FLATPAK" install --user -y --noninteractive flathub "$id"
			;;
		nix) nix_ok || die "Nix is not installed (flpkg install nix)"; nixc profile install "nixpkgs#$id" ;;
		*) box_install "$b" "$id" ;;
	esac || { say "$id: the install failed"; return 1; }
	[ "$b" = freelfs ] || record_add "$b" "$id" "$name"
}

# remove: the source that installed it
do_remove_one() {
	local name=$1 cands="" b id line
	if [ -z "$FROM" ] || [ "$FROM" = freelfs ]; then
		native_installed "$name" && cands+=$(printf 'freelfs\t%s\t\t\n' "$name")$'\n'
	fi
	while IFS=$'\t' read -r b id; do
		[ -n "$b" ] || continue
		[ -z "$FROM" ] || [ "$FROM" = "$b" ] || continue
		cands+=$(printf '%s\t%s\t\t\n' "$b" "$id")$'\n'
	done < <(record_find "$name")
	# Flatpak apps installed some other way (the Store): the app whose last name part is NAME
	if [ -z "$cands" ] && { [ -z "$FROM" ] || [ "$FROM" = flatpak ]; } && fp_ok; then
		id=$("$FLATPAK" list --user --app --columns=application 2> /dev/null | awk -F. -v n="$name" 'tolower($NF) == tolower(n) { print; exit }')
		[ -n "$id" ] && cands+=$(printf 'flatpak\t%s\t\t\n' "$id")$'\n'
	fi
	cands=$(printf '%s' "$cands" | sed '/^$/d' | awk '!seen[$0]++')
	[ -n "$cands" ] || { say "$name is not installed${FROM:+ from $(label "$FROM")}"; return 1; }
	line=$(choose removed "$cands") || return 1
	IFS=$'\t' read -r b id _ < <(printf '%s\n' "$line")
	say "removing $id (installed from $(label "$b"))"
	case $b in
		freelfs) native_priv remove "$id" ;;
		flatpak) "$FLATPAK" uninstall --user -y --noninteractive "$id" ;;
		nix) nixc profile remove "$id" ;;
		*) box_remove "$b" "$id" ;;
	esac || { say "$id: removing failed"; return 1; }
	record_del "$b" "$id"
}

do_upgrade() {
	local rc=0
	say "FreeLFS repository"
	native_priv upgrade || rc=1
	if fp_ok && "$FLATPAK" list --user --app 2> /dev/null | grep -q .; then
		say "Flathub (Flatpak)"
		"$FLATPAK" update --user -y --noninteractive || rc=1
	fi
	if nix_ok && nixc profile list 2> /dev/null | grep -q .; then
		say "Nix"
		nixc profile upgrade '.*' || rc=1
	fi
	if dbx_ok; then
		local b
		for b in apt pacman dnf; do
			box_exists "$b" || continue
			say "$(label "$b")"
			"$DBX" upgrade -n "$(box_name "$b")" || rc=1
		done
	fi
	return $rc
}

do_search() {
	[ "${#NAMES[@]}" -gt 0 ] || die "usage: flpkg search TEXT"
	local t=${NAMES[*]} found=0 b lines
	for b in $ORDER; do
		[ -z "$FROM" ] || [ "$FROM" = "$b" ] || continue
		case $b in
			freelfs) lines=$("$NATIVE" search "$t" 2> /dev/null | awk '/^[^ ]/ { printf "\n" $0 " "; next } { sub(/^ +/, ""); printf "%s", $0 }' | sed '/^$/d') ;;
			flatpak) lines=$(fp_ok && fp_remote > /dev/null 2>&1 && "$FLATPAK" search --user --columns=application,version,name "$t" 2> /dev/null | awk -F'\t' '$1 ~ /\./ { print "flathub/" $1 " " $2 " " $3 }') ;;
			nix) lines=$(search_nix "$t" | awk -F'\t' '{ print "nix/" $2 " " $3 " " $4 }') ;;
			*) lines=$(search_box "$b" "$t" | awk -F'\t' -v b="$b" '{ print b "/" $2 " " $3 " " $4 }') ;;
		esac
		[ -n "$lines" ] || continue
		found=1
		printf '%s\n' "$lines" | sed 's/ *$//' | head -n 25
	done
	[ "$found" = 1 ] || { say "nothing found for '$t'"; return 1; }
}

do_list() {
	"$NATIVE" list
	if fp_ok; then "$FLATPAK" list --user --app --columns=application,version 2> /dev/null | awk -F'\t' '$1 ~ /\./ { print "flatpak/" $1 " " $2 }'; fi
	if nix_ok; then nixc profile list 2> /dev/null | awk '/^Name:/ { print "nix/" $2 }'; fi
	if dbx_ok; then
		local b
		for b in apt pacman dnf; do box_exists "$b" && awk -F'\t' -v b="$b" '$1 == b { print b "/" $2 }' "$STATE/sources.list" 2> /dev/null; done
	fi
	return 0
}

# ---- run -----------------------------------------------------------------------------------
rc=0
case $CMD in
	install)
		[ "${#NAMES[@]}" -gt 0 ] || die "usage: flpkg install [--from=SOURCE] NAME..."
		for n in "${NAMES[@]}"; do do_install_one "$n" || rc=1; done
		;;
	remove)
		[ "${#NAMES[@]}" -gt 0 ] || die "usage: flpkg remove [--from=SOURCE] NAME..."
		for n in "${NAMES[@]}"; do do_remove_one "$n" || rc=1; done
		;;
	upgrade) do_upgrade || rc=1 ;;
	search) do_search || rc=1 ;;
	list) do_list ;;
	*) die "internal: $CMD" ;;
esac
exit $rc
