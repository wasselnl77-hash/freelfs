#!/bin/bash
# Integration tests for transactions: install, conflicts, dependencies,
# upgrade, removal, system users, ownership, and recovery from a crash at
# every stage. Everything happens in a throwaway root (--root).
# usage: tests/test-txn.sh   (from src/flpkg, after make)
# SPDX-License-Identifier: MIT

set -u
cd "$(dirname "$0")/.."
export FLPKG_LIBDIR=$PWD
flpkg=$PWD/flpkg
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
conf=$work/flpkg.conf
printf 'builddir = %s/build\ncache = %s/cache\n' "$work" "$work" > "$conf"
root=$work/root
pkgs=$work/pkgs
mkdir -p "$root" "$pkgs"
fails=0

ok() { printf '  %-58s ok\n' "$1"; }
bad() { printf '  %-58s FAIL\n' "$1"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
fl() { "$flpkg" --config "$conf" --root "$root" "$@" > "$work/out" 2>&1; }

for r in demo1 demo2 app clash untracked layout; do
	mkdir -p "$pkgs/$r"
	"$flpkg" --config "$conf" build -o "$pkgs/$r" "tests/recipes/$r" > "$work/build-$r.log" 2>&1 ||
		{ echo "cannot build $r"; cat "$work/build-$r.log"; exit 1; }
done
demo1=$pkgs/demo1/demo-1.0-1-any.flpkg
demo2=$pkgs/demo2/demo-2.0-1-any.flpkg
app=$pkgs/app/app-1.0-1-any.flpkg
clash=$pkgs/clash/clash-1.0-1-any.flpkg
untracked=$pkgs/untracked/untracked-1.0-1-any.flpkg
layout=$pkgs/layout/layout-1.0-1-any.flpkg
leftovers() { find "$root" -name '.flpkg.*' | grep -q .; }
snapshot() { (cd "$root" && find . -path ./var/lib/flpkg -prune -o -print0 | sort -z | xargs -0 stat -c '%n %s %a' 2> /dev/null; cat usr/share/demo/a.txt 2> /dev/null) | sha256sum; }

echo "install"
check "install demo 1 into an empty root" 'fl install -y "$demo1"'
check "its files are there" '[ "$(cat "$root/usr/share/demo/a.txt")" = v1 ] && [ -x "$root/usr/bin/demo" ]'
check "setgid mode applied" '[ "$(stat -c %a "$root/usr/bin/demo")" = 2755 ]'
check "group 5 deferred to fix-owners (one-uid namespace)" 'fl verify; grep -q "wait for .flpkg fix-owners" "$work/out" || [ "$(stat -c %g "$root/usr/bin/demo")" = 5 ]'
check "mtime is SOURCE_DATE_EPOCH" '[ "$(stat -c %Y "$root/usr/share/demo/a.txt")" = 1767225600 ]'
check "listed as installed" 'fl list && grep -qx "demo 1.0-1" "$work/out"'
check "owns" 'fl owns /usr/share/demo/a.txt && grep -q "owned by demo" "$work/out"'
check "verify finds no problems" 'fl verify && grep -q "^0 problems" "$work/out"'
check "no temporary files left" '! leftovers'

echo "conflicts and dependencies"
echo untracked > "$root/usr/bin/untracked"
before=$(snapshot)
check "a file owned by another package is refused" '! fl install -y "$clash" && grep -q "owned by demo" "$work/out"'
check "unmet dependency is refused" '! fl install -y "$app" && grep -q "requires demo>=2" "$work/out"'
check "an untracked file is refused" '! fl install -y "$untracked" && grep -q "exists in the file system" "$work/out"'
check "nothing changed by refused transactions" '[ "$(snapshot)" = "$before" ] && ! leftovers'
check "--overwrite allows it" 'fl install -y --overwrite "/usr/bin/untr*" "$untracked" && [ "$(cat "$root/usr/bin/untracked")" = packaged ]'

echo "upgrade"
echo "setting=mine" > "$root/etc/demo.conf"
check "upgrade demo to 2" 'fl install -y "$demo2"'
check "changed file updated" '[ "$(cat "$root/usr/share/demo/a.txt")" = v2 ]'
check "file dropped by the new version is gone" '[ ! -e "$root/usr/share/demo/old.txt" ]'
check "new file is there" '[ -e "$root/usr/share/demo/new.txt" ]'
check "config file replaced by the new version" '[ "$(cat "$root/etc/demo.conf")" = setting=2 ]'
check "history records the upgrade" 'fl history && grep -q "upgrade   demo (1.0-1 -> 2.0-1)" "$work/out"'
check "dependency now met" 'fl install -y "$app"'
check "system group and user created" 'grep -qx "appgrp:x:555:" "$root/etc/group" && grep -qx "appusr:x:555:555:App Daemon:/var/lib/app:/usr/bin/false" "$root/etc/passwd"'

echo "remove"
check "removing a needed package is refused" '! fl remove -y demo && grep -q "app 1.0-1 requires demo>=2" "$work/out"'
check "--cascade removes it with its dependents" 'fl remove -y --cascade demo && fl list && ! grep -q "^demo\|^app" "$work/out"'
check "their files and empty directories are gone" '[ ! -e "$root/usr/share/demo" ] && [ ! -e "$root/usr/share/app" ] && [ -d "$root/usr/bin" ]'
check "verify still clean" 'fl verify && grep -q "^0 problems" "$work/out"'

echo "crash recovery"
fl install -y "$demo1"
before=$(snapshot)
for point in staged applied; do
	FLPKG_TEST_CRASH=$point fl install -y "$demo2"
	check "crash after '$point': next run rolls back" 'fl list && grep -qx "demo 1.0-1" "$work/out" && [ "$(snapshot)" = "$before" ] && ! leftovers'
done
FLPKG_TEST_CRASH=recorded fl install -y "$demo2"
check "crash after 'recorded': next run finishes" 'fl list && grep -qx "demo 2.0-1" "$work/out" && [ "$(cat "$root/usr/share/demo/a.txt")" = v2 ] && [ ! -e "$root/usr/share/demo/old.txt" ] && ! leftovers'
check "verify clean after recovery" 'fl verify && grep -q "^0 problems" "$work/out"'

echo "layout change"
mkdir -p "$root/var/log" && echo kept > "$root/var/log/old.log"
check "a real /var becomes a link to sysflfs/data" 'fl install -y "$layout" && [ "$(readlink "$root/var")" = sysflfs/data ]'
check "its contents moved along" '[ "$(cat "$root/sysflfs/data/log/old.log")" = kept ] && [ -d "$root/sysflfs/data/lib/flpkg" ]'
check "the database still works through the link" 'fl list && grep -qx "layout 1.0-1" "$work/out"'
check "reinstalling over the identical link is fine" 'fl install -y "$layout"'
check "verify clean after the move" 'fl verify && grep -q "^0 problems" "$work/out"'
root2=$work/root2
check "into an empty root, one transaction: the link comes first" '"$flpkg" --config "$conf" --root "$root2" install -y "$layout" "$demo1" > "$work/out" 2>&1 && [ "$(readlink "$root2/etc")" = sysflfs/settings ] && [ -f "$root2/sysflfs/settings/demo.conf" ]'
check "a package's files moved with /etc stay its own" 'fl verify && grep -q "^0 problems" "$work/out" && fl owns /etc/demo.conf && grep -q "owned by demo" "$work/out"'

echo "transaction tests: $fails failed"
[ "$fails" -eq 0 ]
