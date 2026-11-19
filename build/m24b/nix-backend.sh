#!/bin/bash
# m24b, host side: src/flpkg/sources.sh + sources-nix.sh against a stand-in "nix" that logs what it is asked and
# answers like nix 2.35 does (search --json, build --dry-run, profile install/remove/list). The native flpkg and
# the daemon socket are stand-ins too (FLPKG_NIX_NO_SERVICE_CHECK). Real script, real argument handling, real
# prompts (no tty: the live-RAM question is answered by --yes or it cancels). Every check can fail.
here=$(cd "$(dirname "$0")/../.." && pwd)
S=$here/src/flpkg/sources.sh
d=$(mktemp -d); trap 'rm -rf "$d"' EXIT
mkdir -p "$d/bin" "$d/state" "$d/live"
LOG=$d/log
cat > "$d/bin/nix" <<'F'
#!/bin/sh
echo "nix $*" >> "$LOG"
case "$*" in
  *"search nixpkgs"*)
    printf '%s' '{"legacyPackages.x86_64-linux.hello":{"description":"A program that produces a familiar, friendly greeting","pname":"hello","version":"2.12.2"},"legacyPackages.x86_64-linux.ripgrep":{"description":"Line-oriented search tool","pname":"ripgrep","version":"14.1.1"},"legacyPackages.x86_64-linux.python3Packages.requests":{"description":"HTTP library","version":"2.32.3"}}' ;;
  *"build --dry-run"*) echo "these 1 paths will be fetched (0.25 MiB download, 0.92 MiB unpacked):" >&2 ;;
  *"profile list"*) printf 'Name:               hello\nFlake attribute:    legacyPackages.x86_64-linux.hello\n' ;;
esac
exit 0
F
cat > "$d/bin/native" <<'F'
#!/bin/sh
echo "native $*" >> "$LOG"
exit 0
F
chmod 755 "$d/bin/nix" "$d/bin/native"
# round 24a: search reads a cached list (the real one is built from the channel's packages.json.br); a stand-in list here
printf 'hello\t2.12.2\tA program that produces a familiar, friendly greeting\nripgrep\t14.1.1\tLine-oriented search tool\npython3Packages.requests\t2.32.3\tHTTP library\n' > "$d/nix-index.tsv"
export FLPKG_NIX_INDEX_FILE=$d/nix-index.tsv
export LOG
run() { env FLPKG_NIX="$d/bin/nix" FLPKG_NATIVE="$d/bin/native" FLPKG_NIX_NO_SERVICE_CHECK=1 \
	FLPKG_STATE="$d/state" FLPKG_LIVE_DIR="$d/nolive" FLPKG_ASSUME_TTY= bash "$S" "$@" < /dev/null; }
fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }

# 1. search: JSON from nix becomes "nix<TAB>attr<TAB>version<TAB>text"; the exact name comes first
out=$(run search hello 2>&1)
check "search lists hello from nix" 'printf "%s" "$out" | grep -q "^nix/hello 2.12.2"'
check "search keeps the nixpkgs attribute of a nested name" 'run search requests 2>&1 | grep -q "requests"'
: > "$LOG"
out=$(run install --from=nix --yes hello 2>&1)
check "install --from=nix runs nix profile install nixpkgs#hello" 'grep -q "profile install nixpkgs#hello" "$LOG"'

# 2. live system without persistence: the first install asks, with nix's own size; no answer = cancelled
: > "$LOG"; mkdir -p "$d/live"
out=$(env FLPKG_NIX="$d/bin/nix" FLPKG_NATIVE="$d/bin/native" FLPKG_NIX_NO_SERVICE_CHECK=1 FLPKG_STATE="$d/state" \
	FLPKG_LIVE_DIR="$d/live" bash "$S" install --from=nix hello < /dev/null 2>&1)
check "live: the warning shows the size nix predicts" 'printf "%s" "$out" | grep -q "0.25 MiB download"'
check "live: no answer cancels, nothing is installed" '! grep -q "profile install" "$LOG"'
: > "$LOG"
out=$(env FLPKG_NIX="$d/bin/nix" FLPKG_NATIVE="$d/bin/native" FLPKG_NIX_NO_SERVICE_CHECK=1 FLPKG_STATE="$d/state" \
	FLPKG_LIVE_DIR="$d/live" bash "$S" install --from=nix --yes hello < /dev/null 2>&1)
check "live: --yes (the Store asked already) installs" 'grep -q "profile install nixpkgs#hello" "$LOG"'

# 3. list and remove go to nix profile
out=$(run list 2>&1)
check "list shows nix/hello from nix profile list" 'printf "%s" "$out" | grep -q "^nix/hello"'
: > "$LOG"
run remove --from=nix hello > /dev/null 2>&1
check "remove runs nix profile remove hello" 'grep -q "profile remove hello" "$LOG"'

# 4. without nix on the machine, nothing is searched and nothing is installed
out=$(env FLPKG_NIX=/nonexistent FLPKG_NATIVE="$d/bin/native" FLPKG_NIX_NO_SERVICE_CHECK=1 FLPKG_STATE="$d/state" \
	bash "$S" search --from=nix hello < /dev/null 2>&1)
check "no nix: search is quiet and says nothing found" 'printf "%s" "$out" | grep -q "nothing found"'

[ "$fail" = 0 ] && echo "m24b nix-backend: PASS" || { echo "m24b nix-backend: FAIL"; exit 1; }
