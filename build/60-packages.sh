#!/bin/bash
# M3: the base system as packages. In the rootless chroot (the M2 system is
# the build host): build flpkg, make a bootstrap signing key, build every
# recipe in recipes/ into packages, sign them into a local repo, and install
# the "base" group into an empty root with flpkg. That root then becomes the
# disk image:  FLFS_IMAGE_ROOT=$FLFS/.bootstrap/pkg/root ./build/50-image.sh
# Resumable: each package is a stamped step.
#
# usage: 60-packages.sh [RECIPE...]   (as your normal user, after M2)
#   With RECIPE names, only rebuild those packages (and the repo and root).

set -euo pipefail
. "$(dirname "$0")/config.sh"
. "$(dirname "$0")/lib.sh"
chroot_env "$@"

[ -e "$FLFS_STAMPS/10.3-kernel" ] || die "build the M2 system first (stages 20 to 41)"

WORK=/.bootstrap/pkg # not part of any image
OUT=$WORK/out
REPO=$WORK/repo
KEY=$WORK/keys/bootstrap
ROOT=$WORK/root
CONF=$WORK/flpkg.conf
export FLPKG_LIBDIR=$WORK/flpkg-src
FLPKG=$FLPKG_LIBDIR/flpkg

flpkg() { "$FLPKG" --config "$CONF" "$@"; }

tool_step() {
	mkdir -p "$WORK"
	rm -rf "$FLPKG_LIBDIR"
	cp -r "$FLFS_TOP/src/flpkg" "$FLPKG_LIBDIR"
	make -C "$FLPKG_LIBDIR" clean
	make -C "$FLPKG_LIBDIR" flpkg
}

setup_step() {
	mkdir -p "$OUT" "$REPO" "${KEY%/*}"
	[ -e "$KEY.key" ] || "$FLPKG" key generate "$KEY"
	cat > "$CONF" <<-EOF
		builddir = $WORK/build
		cache = /var/cache/flpkg
		keys = ${KEY%/*}
		jobs = $FLFS_JOBS
		repo base file://$REPO
	EOF
}

recipe_list() { # recipe_list NAME VAR... -- print a recipe's arrays
	bash -c '. "$1/recipe"; shift; for v; do eval "printf \"%s\n\" \"\${$v[@]}\""; done' \
		_ "$FLFS_TOP/recipes/$1" "${@:2}" 2> /dev/null
}

# order_recipes NAME... -- the same names, each after what it depends on
order_recipes() {
	local r d
	{
		for r in "$@"; do
			echo "$r $r"
			for d in $(recipe_list "$r" depends makedepends); do
				d=${d%%[<>=]*}
				case " $* " in *" $d "*) echo "$d $r" ;; esac
			done
		done
	} | tsort
}

build_one() {
	rm -f "$OUT/$1"-[0-9]*.flpkg
	flpkg build --nocheck --sources /sources -o "$OUT" "$FLFS_TOP/recipes/$1"
	# The base system is the build system itself; from M6 on, desktop packages
	# build on each other, so each one goes into it too (and so do the live ISO
	# tools, group "live": build/53-iso.sh runs them in the chroot).
	if recipe_list "$1" groups | grep -Eqx "desktop|live"; then
		flpkg install -y --nodeps --unsigned "$OUT/$1"-[0-9]*.flpkg
	fi
}

repo_step() {
	rm -rf "$REPO"
	flpkg repo add --key "$KEY.key" "$REPO" "$OUT"/*.flpkg
}

root_step() {
	rm -rf "$ROOT"
	mkdir -p "$ROOT"
	flpkg --root "$ROOT" sync
	flpkg --root "$ROOT" install -y base desktop
}

# What the installer will do (M9): shadow files, time zone, keys, and for
# development images a root login without password.
firstboot_step() {
	chroot "$ROOT" /usr/sbin/pwconv
	chroot "$ROOT" /usr/sbin/grpconv
	chroot "$ROOT" /usr/bin/passwd -d root
	sed -i '/^FreeLFS/a Development image: log in as root, there is no password.' "$ROOT/etc/issue"
	ln -sfn /usr/share/zoneinfo/UTC "$ROOT/etc/localtime"
	touch "$ROOT"/etc/sub{u,g}id
	install -m644 "$KEY.pub" "$ROOT/etc/flpkg/keys/"
	rm -rf "$ROOT/var/cache/flpkg/pkg" # downloaded copies of the packages
}

run_step p.0-flpkg tool_step
run_step p.1-setup setup_step

recipes=("$@")
if [ ${#recipes[@]} -eq 0 ]; then
	for r in "$FLFS_TOP"/recipes/*/; do
		r=$(basename "$r")
		# group "deferred": recipes kept for later that cannot build here yet (named, they still build)
		recipe_list "$r" groups | grep -qx deferred && continue
		recipes+=("$r")
	done
else
	for r in "${recipes[@]}"; do
		rm -f "$FLFS_STAMPS/pkg-$r"
	done
	rm -f "$FLFS_STAMPS"/p.9-*
fi
for r in "${recipes[@]}"; do
	[ -f "$FLFS_TOP/recipes/$r/recipe" ] || die "no recipe $r"
done
mapfile -t recipes < <(order_recipes "${recipes[@]}")
for r in "${recipes[@]}"; do
	run_step "pkg-$r" build_one "$r"
done

run_step p.9-repo repo_step
run_step p.9-root root_step
run_step p.9-firstboot firstboot_step

msg "base system from packages: $(flpkg --root "$ROOT" list | wc -l) packages in $ROOT"
msg "make the image with: FLFS_IMAGE_ROOT=\$FLFS$ROOT ./build/50-image.sh"
