#!/bin/bash
# Tests for the pre-transaction hooks (snapshot.c): which scripts run, in what
# order, with what environment, and that a failing one does not stop flpkg.
# usage: tests/test-hook.sh   (from src/flpkg, after make)
# SPDX-License-Identifier: MIT
set -u
cd "$(dirname "$0")/.."
export FLPKG_LIBDIR=$PWD
flpkg=$PWD/flpkg
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
conf=$work/flpkg.conf
printf 'builddir = %s/build\ncache = %s/cache\nsnapshots = 3\nsignatures = never\n' "$work" "$work" > "$conf"
root=$work/root
hooks=$work/hooks
mkdir -p "$root" "$hooks" "$work/pkg1" "$work/pkg2"
fails=0
ok() { printf '  %-58s ok\n' "$1"; }
bad() { printf '  %-58s FAIL\n' "$1"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
fl() { FLPKG_HOOKDIR=$hooks "$flpkg" --config "$conf" --root "$root" "$@" > "$work/out" 2>&1; }

"$flpkg" --config "$conf" build -o "$work/pkg1" tests/recipes/demo1 > /dev/null 2>&1 &&
	"$flpkg" --config "$conf" build -o "$work/pkg2" tests/recipes/demo2 > /dev/null 2>&1 ||
	{ echo "cannot build the test packages"; exit 1; }
demo=$work/pkg1/demo-1.0-1-any.flpkg
demo2=$work/pkg2/demo-2.0-1-any.flpkg

cat > "$hooks/20-record" <<EOS
#!/bin/sh
{ echo "root=\$FLPKG_ROOT"; echo "summary=\$FLPKG_SUMMARY"; echo "snapshots=\$FLPKG_SNAPSHOTS"
  echo "[packages]"; cat "\$FLPKG_PACKAGES"; echo "file=\$FLPKG_PACKAGES"; } >> "$work/record"
echo second >> "$work/order"
EOS
printf '#!/bin/sh\necho first >> "%s/order"\nexit 3\n' "$work" > "$hooks/10-fails"
printf '#!/bin/sh\necho skipped >> "%s/order"\n' "$work" > "$hooks/30-notexec"
printf '#!/bin/sh\necho hidden >> "%s/order"\n' "$work" > "$hooks/.hidden"
chmod +x "$hooks/20-record" "$hooks/10-fails" "$hooks/.hidden"

fl install -y "$demo"
check "install works although a hook failed" '[ $? -eq 0 ] && "$flpkg" --config "$conf" --root "$root" list | grep -qx "demo 1.0-1"'
check "the failing hook is reported as a warning" 'grep -q "10-fails failed" "$work/out"'
check "hooks run in name order, skipping non-executable and hidden" '[ "$(cat "$work/order")" = "$(printf "first\nsecond")" ]'
check "environment: root, summary, snapshots" 'grep -qx "root=$root" "$work/record" && grep -qx "summary=install demo" "$work/record" && grep -qx "snapshots=3" "$work/record"'
check "package list is empty before the first install" '[ "$(sed -n "/^\[packages\]/,/^file=/p" "$work/record" | wc -l)" = 2 ]'
check "the package list file is gone afterwards" '! [ -e "$(sed -n "s/^file=//p" "$work/record" | head -1)" ]'

: > "$work/record"
fl install -y "$demo2"
check "an upgrade sees what is installed, names itself" 'sed -n "/^\[packages\]/,/^file=/p" "$work/record" | grep -qx "demo 1.0-1" && grep -qx "summary=upgrade demo" "$work/record"'

: > "$work/record"
fl remove -y demo
check "a removal runs the hooks with its summary" 'grep -qx "summary=remove demo" "$work/record"'

# no hooks for another root unless FLPKG_HOOKDIR says so
: > "$work/order"
"$flpkg" --config "$conf" --root "$root" install -y "$demo" > "$work/out" 2>&1
check "other --root without FLPKG_HOOKDIR: no hooks" '[ ! -s "$work/order" ]'

if [ "$fails" -ne 0 ]; then echo "$fails hook test(s) failed"; exit 1; fi
echo "hook tests passed"
