#!/bin/bash
# m22: the online repo, end to end, over real HTTP (a local server stands in for GitHub Releases): publish
# staging, sync, install, a bumped package arriving with `flpkg upgrade --sync`, and the refusals (tampered
# index, tampered package, replayed old index, unknown signer). Host-side: the real flpkg binary and curl.
#   build/m22/repo-update.sh [FLPKG_BINARY]      (default src/flpkg/flpkg, built with the sysroot)
here=$(cd "$(dirname "$0")/../.." && pwd)
flpkg=${1:-$here/src/flpkg/flpkg}
export FLPKG_NO_SOURCES=1 FLPKG_LIBDIR=$here/src/flpkg
d=$(mktemp -d); trap 'kill $srv 2>/dev/null; rm -rf "$d"' EXIT
mkdir -p "$d"/{repo,keys,root,pkgs,other}
fails=0
ok() { printf '  %-66s ok\n' "$1"; }
bad() { printf '  %-66s FAIL\n' "$1"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
conf=$d/flpkg.conf
fl() { "$flpkg" --config "$conf" --root "$d/root" "$@" > "$d/out" 2>&1; }
build() { "$flpkg" --config /dev/null build -o "$d/pkgs" "$here/src/flpkg/tests/recipes/$1" > "$d/b-$1.log" 2>&1 || { echo "cannot build $1"; cat "$d/b-$1.log"; exit 1; }; }
"$flpkg" key generate "$d/k" > /dev/null; "$flpkg" key generate "$d/other/evil" > /dev/null
cp "$d/k.pub" "$d/keys/"
build demo1; build demo2
add() { "$flpkg" --config /dev/null repo add --key "${KEY:-$d/k.key}" "$d/repo" "$@" > /dev/null 2>&1; }
add "$d/pkgs"/demo-1.0-1-any.flpkg
# stage the way a release is staged: build/70-publish-repo.sh (only reads the repo, writes the staging dir)
export FLFS_REPO_DIR=$d/repo FLFS_PUBLISH_DIR=$d/publish
echo "publish staging"
check "70-publish-repo.sh stages the repo and uploads nothing" "bash '$here/build/70-publish-repo.sh' > '$d/pub.log' 2>&1 && grep -q 'nothing has been uploaded' '$d/pub.log' && [ -s '$d/publish/repo-x86_64/index.zst.sig' ]"
check "the staged files are the repo's files, no key among them" '! ls "$d/publish/repo-x86_64" | grep -qE "\.key$|\.sec$" && [ "$(ls "$d/publish/repo-x86_64" | wc -l)" = "$(ls "$d/repo" | wc -l)" ]'
check "upload.sh names the release tag and sends the index last" 'grep -q "gh release upload repo-x86_64" "$d/publish/upload.sh" && tail -5 "$d/publish/upload.sh" | grep -q "index.zst index.zst.sig"; [ "$(grep -n "index.zst index.zst.sig" "$d/publish/upload.sh" | head -1 | cut -d: -f1)" -gt "$(grep -n "demo-1.0-1" "$d/publish/upload.sh" | head -1 | cut -d: -f1)" ]'
check "the real private key is not in the staging dir or in git" '! find "$d/publish" -name "*.key" | grep -q . && ! git -C "$here" ls-files | grep -qE "\.key$|repo-key" && git -C "$here" check-ignore -q repo-key/freelfs.key'
# serve the staging directory over HTTP, like a release's download URL
port=$((20000 + RANDOM % 20000))
serve() { kill $srv 2>/dev/null; (cd "$d/publish/repo-x86_64" && exec python3 -m http.server "$port" --bind 127.0.0.1 > /dev/null 2>&1) & srv=$!; sleep 1; }
serve
cat > "$conf" <<CONF
builddir = $d/build
cache = /var/cache/flpkg
keys = $d/keys
signatures = required
repo freelfs http://127.0.0.1:$port
CONF
echo "sync, install, update over HTTP"
check "sync over http accepts the signed index" 'fl sync && grep -q "freelfs: 1 packages" "$d/out"'
check "install by name downloads and installs demo 1.0" 'fl install -y demo && fl list && grep -qx "demo 1.0-1" "$d/out"'
cp "$d/publish/repo-x86_64/index.zst" "$d/index.v1"; cp "$d/publish/repo-x86_64/index.zst.sig" "$d/index.v1.sig"
mv "$d/publish/repo-x86_64.sha256.staged" "$d/publish/repo-x86_64.sha256"   # what upload.sh tells you to do after uploading
add "$d/pkgs"/demo-2.0-1-any.flpkg                      # the bump
bash "$here/build/70-publish-repo.sh" > "$d/pub2.log" 2>&1
check "the second staging uploads only what changed (demo 2.0 and its .sig, index last)" 'grep -qx "demo-2.0-1-any.flpkg" "$d/publish/repo-x86_64.upload" && ! grep -qx "demo-1.0-1-any.flpkg" "$d/publish/repo-x86_64.upload" && [ "$(tail -1 "$d/publish/repo-x86_64.upload")" = index.zst.sig ]'
serve
check "check-updates sees the bump after a sync" 'fl sync && fl check-updates && cut -f1,2,3 "$d/out" | tr "\t" " " | grep -qx "demo 1.0-1 2.0-1"'
check "flpkg upgrade --sync installs the new version" 'fl upgrade -y --sync && fl list && grep -qx "demo 2.0-1" "$d/out"'
echo "refusals"
rm -rf "$d/root"/var/lib/flpkg/index-serial.* 2>/dev/null
# tampered index (the staged files are hard links to the repo: break the link before changing a byte)
P=$d/publish/repo-x86_64
tamper() { cp "$P/$1" "$P/$1.t" && printf x >> "$P/$1.t" && mv "$P/$1.t" "$P/$1"; }
heal() { ln -f "$d/repo/$1" "$P/$1"; }
tamper index.zst
check "a tampered index is refused" '! fl sync && grep -q "BAD SIGNATURE" "$d/out"'
heal index.zst
check "the untouched index is accepted again" 'fl sync'
# replay of the older signed index (valid signature, lower serial)
rm -f "$P/index.zst" "$P/index.zst.sig"; cp "$d/index.v1" "$P/index.zst"; cp "$d/index.v1.sig" "$P/index.zst.sig"
check "an older (replayed) index is refused" '! fl sync && grep -qi "older\|serial\|rollback" "$d/out"'
heal index.zst; heal index.zst.sig
# tampered package
build newdemo
add "$d/pkgs"/newdemo-3.0-1-any.flpkg; bash "$here/build/70-publish-repo.sh" > /dev/null 2>&1; serve
tamper newdemo-3.0-1-any.flpkg
check "a package changed after signing is refused at install" 'fl sync; ! fl install -y newdemo && grep -qi "does not match\|BAD SIGNATURE" "$d/out" && ! { fl list; grep -q "^newdemo " "$d/out"; }'
# an index signed by another key
rm -rf "$d/publish" "$d/repo2"; mkdir "$d/repo2"; "$flpkg" --config /dev/null repo add --key "$d/other/evil.key" "$d/repo2" "$d/pkgs"/demo-2.0-1-any.flpkg > /dev/null 2>&1
mkdir -p "$d/publish/repo-x86_64"; cp "$d/repo2"/* "$d/publish/repo-x86_64/"; serve
check "an index signed by an unknown key is refused" '! fl sync && grep -qi "unknown key" "$d/out"'
echo
[ -n "${KEEP:-}" ] && cp -r "$d" /tmp/m22keep; [ "$fails" = 0 ] && echo "m22 repo update: all passed" || { echo "m22 repo update: $fails FAILED"; exit 1; }
