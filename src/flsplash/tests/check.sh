#!/bin/sh
# flsplash tests: the two styles render, the config picks one, flsplash-set checks its argument,
# writes the file and refuses what it must. No screen and no root needed. Run by make check.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/flsplash-test.XXXXXX")
trap 'rm -rf "$T"' EXIT
FL="$here/flsplash"
SET="$here/flsplash-set-test"
fail() { echo "FAIL: $*"; exit 1; }

# 1. both styles render a 1920 x 1080 frame; the flat background is #0d1110 in the corner
"$FL" --render 3000 1920 1080 "$T/flat.ppm" flat || fail "render flat"
"$FL" --render 3000 1920 1080 "$T/legacy.ppm" legacy || fail "render legacy"
[ "$(head -n 2 "$T/flat.ppm" | tr '\n' ' ')" = "P6 1920 1080 " ] || fail "flat header"
[ "$(od -An -tu1 -j 17 -N 3 "$T/flat.ppm" | tr -s ' ')" = " 13 17 16" ] || fail "flat background colour"
cmp -s "$T/flat.ppm" "$T/legacy.ppm" && fail "flat and legacy look the same"
echo "ok    both styles render"

# 2. the config: no file, garbage and splash=flat are flat; splash=legacy is legacy
FLSPLASH_CONF="$T/none" "$FL" --render 3000 1920 1080 "$T/def.ppm" || fail "render no config"
cmp -s "$T/def.ppm" "$T/flat.ppm" || fail "missing config is not flat"
printf 'splash=bogus\n' > "$T/bad"
FLSPLASH_CONF="$T/bad" "$FL" --render 3000 1920 1080 "$T/bad.ppm" || fail "render bad config"
cmp -s "$T/bad.ppm" "$T/flat.ppm" || fail "garbage config is not flat"
printf 'splash=legacy\n' > "$T/leg.conf"
FLSPLASH_CONF="$T/leg.conf" "$FL" --render 3000 1920 1080 "$T/leg2.ppm" || fail "render legacy config"
cmp -s "$T/leg2.ppm" "$T/legacy.ppm" || fail "splash=legacy is not legacy"
echo "ok    config picks the style; missing or bad means flat"

# 3. flsplash-set: arguments, the write, the read back, the refusals
export FLSPLASH_SET_DIR="$T/etc" FLSPLASH_SET_CONF="$T/etc/splash.conf"
"$SET" flat || fail "set flat"
grep -qx 'splash=flat' "$T/etc/splash.conf" || fail "file says flat"
"$SET" legacy || fail "set legacy"
grep -qx 'splash=legacy' "$T/etc/splash.conf" || fail "file says legacy"
FLSPLASH_CONF="$T/etc/splash.conf" "$FL" --render 3000 1920 1080 "$T/set.ppm" || fail "render written config"
cmp -s "$T/set.ppm" "$T/legacy.ppm" || fail "written config is read back as legacy"
set +e
"$SET" bogus 2>/dev/null; [ $? = 2 ] || fail "bogus style is refused"
"$SET" 2>/dev/null; [ $? = 2 ] || fail "no argument is refused"
"$SET" flat extra 2>/dev/null; [ $? = 2 ] || fail "two arguments are refused"
"$SET" "flat;rm" 2>/dev/null; [ $? = 2 ] || fail "odd text is refused"
set -e
ln -s "$T/target" "$T/link.conf"
FLSPLASH_SET_CONF="$T/link.conf" "$SET" flat 2>/dev/null && fail "symlink was written"
[ ! -e "$T/target" ] || fail "symlink target was created"
echo "ok    flsplash-set: arguments, write, read back, refusals"
echo "all flsplash tests passed"
