#!/bin/sh
# Tests for flpkg-seat: builds it with a stand-in for flpkg that prints what
# it got, and feeds it good and bad names. Needs no root (the build skips the
# setuid steps). SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
cat > "$dir/fake" <<'F'
#!/bin/sh
echo "ARGS: $*"
echo "ENV: $(env | sort | tr '\n' ' ')"
echo "CWD: $(pwd)"
F
chmod +x "$dir/fake"
cc -std=c17 -D_GNU_SOURCE -Wall -Wextra -DFLPKG_SEAT_TEST -DFLPKG_BIN="\"$dir/fake\"" \
	-o "$dir/seat" flpkg-seat.c || exit 1
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
rejects() { # rejects DESC ARG...
	d=$1
	shift
	if "$dir/seat" "$@" > "$dir/out" 2>&1; then bad "$d"; elif grep -q ARGS "$dir/out"; then bad "$d (flpkg ran)"; else ok "$d"; fi
}
SECRET=1 LD_PRELOAD=/nonexistent "$dir/seat" install vim gcc++ lib_x-1.0 > "$dir/out" 2>&1
grep -qx 'ARGS: install -y -- vim gcc++ lib_x-1.0' "$dir/out" && ok "install passes -y and the names" || bad "install args"
grep -q "SECRET=\|LD_PRELOAD=" "$dir/out" && bad "environment is cleared" || ok "environment is cleared"
grep -q 'ENV: PATH=/usr/sbin:/usr/bin ' "$dir/out" && ok "only PATH is set" || bad "only PATH is set"
grep -qx 'CWD: /' "$dir/out" && ok "runs in /" || bad "runs in /"
"$dir/seat" remove vim > "$dir/out" 2>&1
grep -qx 'ARGS: remove -y -- vim' "$dir/out" && ok "remove args" || bad "remove args"
rejects "no arguments"
rejects "no names" install
rejects "other verb" upgrade vim
rejects "sync" sync
rejects "option as verb" -y install vim
rejects "option as name" install --nodeps
rejects "name starting with -" install -rf
rejects "name with slash" install ../x
rejects "path" install /tmp/x
rejects "file" install x.flpkg
rejects "uppercase" install Vim
rejects "space" install "a b"
rejects "newline" install "$(printf 'a\nb')"
rejects "semicolon" install 'a;b'
rejects "dollar" install '$HOME'
rejects "empty name" install ""
rejects "dot first" install .hidden
rejects "one bad among good" install vim 'x y' gcc
rejects "too long" install "$(printf 'a%.0s' $(seq 1 65))"
rejects "too many" install $(seq 1 40)
[ "$fails" = 0 ] && echo "all tests passed" || echo "$fails test(s) failed"
exit "$fails"
