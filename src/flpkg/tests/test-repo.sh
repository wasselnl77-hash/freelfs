#!/bin/bash
# Integration tests for signing, repos and dependency resolution: signed
# indexes, untrusted or tampered repos, install by name with dependencies,
# providers, groups, --needed, upgrade with replaces, remove --recursive.
# usage: tests/test-repo.sh   (from src/flpkg, after make)
# SPDX-License-Identifier: MIT

set -u
cd "$(dirname "$0")/.."
export FLPKG_LIBDIR=$PWD
flpkg=$PWD/flpkg
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
root=$work/root repo=$work/repo pkgs=$work/pkgs keys=$work/keys
mkdir -p "$root" "$pkgs" "$keys" "$work/other"
conf=$work/flpkg.conf
cat > "$conf" << CONF
builddir = $work/build
cache = /var/cache/flpkg
keys = $keys
repo test file://$repo
CONF
fails=0

ok() { printf '  %-58s ok\n' "$1"; }
bad() { printf '  %-58s FAIL\n' "$1"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
fl() { "$flpkg" --config "$conf" --root "$root" "$@" > "$work/out" 2>&1; }
installed() { fl list && grep -q "^$1 " "$work/out"; }

for r in demo1 demo2 app libfoo consumer g1 g2 newdemo; do
	"$flpkg" --config "$conf" build -o "$pkgs" "tests/recipes/$r" > "$work/build-$r.log" 2>&1 ||
		{ echo "cannot build $r"; cat "$work/build-$r.log"; exit 1; }
done
"$flpkg" key generate "$work/test" > /dev/null
"$flpkg" key generate "$work/other/evil" > /dev/null
cp "$work/test.pub" "$keys/"

echo "signing and sync"
check "repo add signs packages and the index" '"$flpkg" --config "$conf" repo add --key "$work/test.key" "$repo" "$pkgs"/demo-1.0-1-any.flpkg "$pkgs"/app-1.0-1-any.flpkg "$pkgs"/libfoo-1.0-1-any.flpkg "$pkgs"/consumer-1.0-1-any.flpkg "$pkgs"/g1-1.0-1-any.flpkg "$pkgs"/g2-1.0-1-any.flpkg > /dev/null 2>&1 && [ -f "$repo/index.zst.sig" ] && [ -f "$repo/demo-1.0-1-any.flpkg.sig" ]'
check "sync accepts the signed index" 'fl sync && grep -q "test: 6 packages" "$work/out"'
check "search finds packages" 'fl search provider && grep -q "test/libfoo 1.0-1" "$work/out"'
check "info --repo shows a repo package" 'fl info --repo app && grep -q "^Repository *: test" "$work/out"'
cp -r "$repo" "$work/repo.good"
"$flpkg" --config "$conf" repo add --key "$work/other/evil.key" "$repo" > /dev/null 2>&1
check "an index signed by an unknown key is refused" '! fl sync && grep -q "unknown key" "$work/out"'
rm -rf "$repo" && cp -r "$work/repo.good" "$repo"
printf x >> "$repo/index.zst"
check "a tampered index is refused" '! fl sync && grep -q "BAD SIGNATURE" "$work/out"'
rm -rf "$repo" && cp -r "$work/repo.good" "$repo"
fl sync

echo "dependency resolution"
check "a dependency no repo can meet stops the install" '! fl install -y app && grep -q "cannot find demo>=2, which app needs" "$work/out" && ! installed app'
"$flpkg" --config "$conf" repo add --key "$work/test.key" "$repo" "$pkgs"/demo-2.0-1-any.flpkg > /dev/null 2>&1
fl sync
check "install by name pulls in its dependency" 'fl install -y app && installed app && installed demo && grep -q "demo 2.0-1" "$work/out"'
check "the dependency is marked as such" 'fl list --deps && grep -qx "demo 2.0-1" "$work/out" && fl list --explicit && grep -qx "app 1.0-1" "$work/out"'
check "a provided name is resolved to its provider" 'fl install -y consumer && installed libfoo'
check "a group installs all its members" 'fl install -y testgrp && installed g1 && installed g2'
check "--needed skips what is up to date" 'fl install -y --needed app && grep -q "up to date" "$work/out"'
check "packages came from the cache, signature checked" '[ -f "$root/var/cache/flpkg/pkg/app-1.0-1-any.flpkg" ]'

echo "upgrade and remove"
check "upgrade with nothing new" 'fl upgrade -y && grep -q "everything is up to date" "$work/out"'
"$flpkg" --config "$conf" repo add --key "$work/test.key" "$repo" "$pkgs"/newdemo-3.0-1-any.flpkg > /dev/null 2>&1
cu() { "$flpkg" --config "$conf" --root "$root" check-updates "$@" > "$work/cu" 2> "$work/cu.err"; }
check "check-updates lists what upgrade would do, tab-separated" 'cu && [ "$(wc -l < "$work/cu")" = 1 ] && [ "$(cut -f1-3,5 "$work/cu" | tr "\t" " ")" = "newdemo - 3.0-1 test" ] && [ "$(cut -f4 "$work/cu")" -gt 0 ]'
check "check-updates --count" 'cu --count && [ "$(cat "$work/cu")" = 1 ]'
check "check-updates leaves the synced cache alone" '! fl search newdemo || ! grep -q newdemo "$work/out"'
check "check-updates --offline uses the last sync" 'cu --offline --count && [ "$(cat "$work/cu")" = 0 ]'
rm -rf "$repo" && cp -r "$work/repo.good" "$repo"; printf x >> "$repo/index.zst"
check "check-updates refuses a tampered index, prints nothing" '! cu && [ ! -s "$work/cu" ] && grep -q "BAD SIGNATURE" "$work/cu.err"'
rm -rf "$repo" && cp -r "$work/repo.good" "$repo"
"$flpkg" --config "$conf" repo add --key "$work/test.key" "$repo" "$pkgs"/newdemo-3.0-1-any.flpkg > /dev/null 2>&1
check "upgrade --sync replaces demo with newdemo" 'fl upgrade --sync -y && installed newdemo && ! installed demo'
check "app still satisfied (newdemo provides demo=3.0)" 'fl verify && grep -q "^0 problems" "$work/out"'
check "remove --recursive also takes unneeded dependencies" 'fl remove -y --recursive app consumer && ! installed app && ! installed newdemo && ! installed libfoo'
check "explicitly installed group members stay" 'installed g1 && installed g2'
check "pacman-style -Qi works" '"$flpkg" --config "$conf" --root "$root" -Qi g1 > "$work/out" 2>&1 && grep -q "^Name *: g1" "$work/out"'

echo "http repo"
if have_http=$(command -v curl && command -v python3); then
	port=$((20000 + RANDOM % 20000))
	(cd "$repo" && exec python3 -m http.server "$port" --bind 127.0.0.1 > "$work/http.log" 2>&1) &
	srv=$!
	trap 'kill $srv 2>/dev/null; rm -rf "$work"' EXIT
	sleep 1
	root2=$work/root2 conf2=$work/flpkg2.conf
	mkdir -p "$root2"
	sed "s|^repo test .*|repo web http://127.0.0.1:$port|" "$conf" > "$conf2"
	fl2() { "$flpkg" --config "$conf2" --root "$root2" "$@" > "$work/out" 2>&1; }
	check "sync over http" 'fl2 sync && grep -q "web: 7 packages" "$work/out"'
	check "install over http (sha256 and signature checked)" 'fl2 install -y app && fl2 list && grep -q "^app " "$work/out"'
	check "check-updates over http" '"$flpkg" --config "$conf2" --root "$root2" check-updates --count > "$work/cu" 2>&1 && [ "$(cat "$work/cu")" = 0 ]'
	kill $srv 2>/dev/null
	check "check-updates fails cleanly when the server is gone" '! "$flpkg" --config "$conf2" --root "$root2" check-updates > "$work/cu" 2>/dev/null && [ ! -s "$work/cu" ]'
else
	echo "  (curl or python3 missing: skipped)"
fi

echo "repo tests: $fails failed"
[ "$fails" -eq 0 ]
