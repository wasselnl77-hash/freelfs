#!/bin/bash
# m22: the flpkg front end (src/flpkg/sources.sh) against stand-ins for flpkg, flpkg-seat, flatpak, nix and
# distrobox that log what they were asked. Host-side, no root, no network. Real script, real prompts:
# only the programs it calls are replaced. Every check can fail.
here=$(cd "$(dirname "$0")/../.." && pwd)
S=$here/src/flpkg/sources.sh
d=$(mktemp -d); trap 'rm -rf "$d"' EXIT
mkdir -p "$d/bin" "$d/state" "$d/live"
LOG=$d/log
cat > "$d/bin/native" <<'F'
#!/bin/sh
echo "native $*" >> "$LOG"
case $1 in
search) case $2 in firefox) printf 'freelfs/firefox 140.0-1\n    Web browser\n' ;; hello) printf 'freelfs/hello 2.12-1\n    Greeter\n' ;; esac ;;
list) cat "$LOGDIR/native-installed" 2>/dev/null ;;
esac
exit 0
F
cat > "$d/bin/seat" <<'F'
#!/bin/sh
echo "seat $*" >> "$LOG"
[ "$1" = install ] && echo "$2 1.0" >> "$LOGDIR/native-installed"
[ "$1" = remove ] && : > "$LOGDIR/native-installed"
exit 0
F
cat > "$d/bin/flatpak" <<'F'
#!/bin/sh
echo "flatpak $*" >> "$LOG"
case $1 in
remotes) echo flathub ;;
search) printf 'org.mozilla.firefox\tFirefox\t140.0\tWeb browser\n' ;;
remote-info) printf '        ID: org.mozilla.firefox\n Installed: 1.2 GB\n' ;;
install) echo "$6${6:+}" > /dev/null; for a; do last=$a; done; echo "$last" >> "$LOGDIR/fp-installed" ;;
uninstall) : > "$LOGDIR/fp-installed" ;;
list) while read -r i; do printf '%s\t1\n' "$i"; done < "$LOGDIR/fp-installed" 2>/dev/null ;;
esac
exit 0
F
cat > "$d/bin/nix" <<'F'
#!/bin/sh
echo "nix $*" >> "$LOG"; exit 0
F
cat > "$d/bin/distrobox" <<'F'
#!/bin/sh
echo "distrobox $*" >> "$LOG"; exit 0
F
chmod +x "$d"/bin/*
export LOG LOGDIR=$d FLPKG_NATIVE=$d/bin/native FLPKG_SEAT=$d/bin/seat FLPKG_FLATPAK=$d/bin/flatpak \
	FLPKG_NIX=$d/bin/nix FLPKG_DISTROBOX=$d/bin/distrobox FLPKG_STATE=$d/state FLPKG_LIVE_DIR=$d/nolive \
	FLPKG_ASSUME_TTY=1
fails=0
ok() { printf '  %-62s ok\n' "$1"; }
bad() { printf '  %-62s FAIL\n' "$1"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
reset() { : > "$LOG"; rm -f "$d/native-installed" "$d/fp-installed" "$d/state/sources.list"; }
run() { local in=$1; shift; printf '%s' "$in" | bash "$S" "$@" > "$d/out" 2>&1; echo $? > "$d/rc"; }

echo "install: the choice prompt"
reset; run $'\n' install firefox
check "two sources: a numbered list, FreeLFS preselected" 'grep -q "1) FreeLFS repository.*(preselected)" "$d/out" && grep -q "2) Flathub" "$d/out"'
check "Enter takes the preselected FreeLFS package (through flpkg-seat)" 'grep -qx "seat install firefox" "$LOG" && ! grep -q "^flatpak install" "$LOG"'
check "a FreeLFS install is not in the source record (flpkg owns it)" '[ ! -s "$d/state/sources.list" ]'
reset; run $'2\n' install firefox
check "choice 2 installs from Flathub (flatpak --user)" 'grep -q "^flatpak install --user -y --noninteractive flathub org.mozilla.firefox" "$LOG" && ! grep -q "^seat" "$LOG"'
check "the Flathub install is recorded" 'grep -q "^flatpak	org.mozilla.firefox	firefox" "$d/state/sources.list"'
reset; run $'7\n' install firefox
check "a choice out of range is refused and installs nothing" '[ "$(cat "$d/rc")" != 0 ] && ! grep -q "install" "$LOG"'

echo "install: --from"
reset; run '' install --from=flatpak firefox
check "--from=flatpak skips the question" '! grep -q "preselected" "$d/out" && grep -q "^flatpak install .* org.mozilla.firefox" "$LOG"'
reset; run '' install --from=freelfs firefox
check "--from=freelfs never calls flatpak install" 'grep -qx "seat install firefox" "$LOG" && ! grep -q "^flatpak install" "$LOG"'
reset; run '' install --from=bogus firefox
check "an unknown --from is an error" '[ "$(cat "$d/rc")" != 0 ] && grep -q "unknown source" "$d/out"'
reset; run '' install --from=apt hello
check "--from=apt without distrobox/box asks before creating" 'grep -qi "creates\|Creating\|Create the" "$d/out" && ! grep -q "^distrobox create" "$LOG"'
reset; run $'y\n' install --from=apt hello
check "--from=apt, yes: creates the box, installs, exports the app" 'grep -q "^distrobox create --yes -n freelfs-apt" "$LOG" && grep -q "apt-get install -y hello" "$LOG" && grep -q "distrobox-export --app hello" "$LOG"'
check "the box install is recorded as apt" 'grep -q "^apt	hello" "$d/state/sources.list"'

echo "install: no terminal (the Store, scripts)"
reset
( unset FLPKG_ASSUME_TTY; bash "$S" install firefox < /dev/null > "$d/out" 2>&1 )
check "a name the FreeLFS repo has, no terminal: FreeLFS, no Flathub search" 'grep -qx "seat install firefox" "$LOG" && ! grep -q "^flatpak search" "$LOG"'

echo "remove routes to the source that installed it"
reset; run $'2\n' install firefox; : > "$LOG"; run '' remove firefox
check "installed from Flathub: remove calls flatpak uninstall, not flpkg" 'grep -q "^flatpak uninstall --user -y --noninteractive org.mozilla.firefox" "$LOG" && ! grep -q "^seat" "$LOG"'
check "the record entry is gone afterwards" '! grep -q firefox "$d/state/sources.list"'
reset; run '' install --from=freelfs firefox; : > "$LOG"; run '' remove firefox
check "installed from FreeLFS: remove calls flpkg-seat remove" 'grep -qx "seat remove firefox" "$LOG" && ! grep -q "^flatpak uninstall" "$LOG"'
reset; run '' install --from=freelfs firefox; run '' install --from=flatpak firefox; : > "$LOG"; run $'2\n' remove firefox
check "installed twice: remove asks which one" 'grep -q "can be removed from 2" "$d/out" && grep -q "^flatpak uninstall" "$LOG"'
reset; run '' remove nothing-here
check "remove of something not installed fails clearly" '[ "$(cat "$d/rc")" != 0 ] && grep -q "is not installed" "$d/out"'
reset; run $'y\n' install --from=apt hello; : > "$LOG"; run '' remove hello
check "installed in the Ubuntu box: remove uses apt-get remove" 'grep -q "apt-get remove -y hello" "$LOG" && ! grep -q "^seat" "$LOG"'

echo "upgrade runs every backend"
reset; run $'y\n' install --from=apt hello; run '' install --from=flatpak firefox; : > "$LOG"
cat > "$d/bin/distrobox" <<'F'
#!/bin/sh
echo "distrobox $*" >> "$LOG"
[ "$1" = list ] && echo "ID | NAME | STATUS | IMAGE" && echo "1 | freelfs-apt | Up | ubuntu"
exit 0
F
run '' upgrade
check "FreeLFS: flpkg-seat upgrade" 'grep -qx "seat upgrade" "$LOG"'
check "Flathub: flatpak update" 'grep -q "^flatpak update --user -y --noninteractive" "$LOG"'
check "Distrobox: upgrade of the existing box" 'grep -q "^distrobox upgrade -n freelfs-apt" "$LOG"'

echo "live system without persistence"
reset; mkdir -p "$d/live"; export FLPKG_LIVE_DIR=$d/live
run $'n\n' install --from=flatpak firefox
check "warns that Flatpak goes into RAM, with the size" 'grep -q "installed into RAM" "$d/out" && grep -q "1.2 GB" "$d/out"'
check "answering no installs nothing" '! grep -q "^flatpak install" "$LOG"'
reset; run $'y\n' install --from=flatpak firefox
check "answering yes installs" 'grep -q "^flatpak install" "$LOG"'
reset; run '' install --from=freelfs firefox
check "a FreeLFS package is not warned about by this script (flpkg-seat path)" '! grep -q "into RAM" "$d/out"'
export FLPKG_LIVE_DIR=$d/nolive
echo "options are forwarded or refused, never dropped"
reset; echo "firefox 1.0" > "$d/native-installed"; run '' remove --cascade --from=freelfs firefox
check "as a person, options the seat cannot take are refused clearly" '[ "$(cat "$d/rc")" != 0 ] && grep -q "need root" "$d/out"'
run '' install --frobnicate firefox
check "an unknown option is an error, not ignored" '[ "$(cat "$d/rc")" != 0 ] && grep -q "unknown option --frobnicate" "$d/out"'
run '' install ./x.flpkg
check "a package file as a person is refused with the reason" '[ "$(cat "$d/rc")" != 0 ] && grep -q "needs root" "$d/out"'
cat > "$d/bin/id" <<'F'
#!/bin/sh
[ "$1" = -u ] && { echo 0; exit 0; }
exec /usr/bin/id "$@"
F
chmod +x "$d/bin/id"
reset; echo "firefox 1.0" > "$d/native-installed"; PATH=$d/bin:$PATH run '' remove --cascade --from=freelfs firefox
check "as root: --cascade reaches the native flpkg (remove -y --cascade -- NAME)" 'grep -q "^native remove -y --cascade -- firefox" "$LOG"'
reset; PATH=$d/bin:$PATH run $'\n' install --from=freelfs --needed --overwrite "/etc/*" firefox
check "as root: --needed and --overwrite GLOB are forwarded, GLOB is not a package" 'grep -q "^native install -y --needed --overwrite /etc/\* -- firefox" "$LOG"'
reset; PATH=$d/bin:$PATH run '' install ./pkg.flpkg
check "as root: a package file goes to the native flpkg" 'grep -q "^native install -y -- ./pkg.flpkg" "$LOG"'
rm -f "$d/bin/id"
echo "search and list"
reset; run '' search firefox
check "search shows FreeLFS and Flathub" 'grep -q "^freelfs/firefox" "$d/out" && grep -q "^flathub/org.mozilla.firefox" "$d/out"'
run '' install --from=flatpak firefox; run '' list
check "list shows installed Flathub apps" 'grep -q "^flatpak/org.mozilla.firefox" "$d/out"'

echo "the C hook: when flpkg hands over to the script"
if [ -x "$here/src/flpkg/flpkg" ] || [ -n "${FLPKG_HOST_BIN:-}" ]; then
	B=${FLPKG_HOST_BIN:-$here/src/flpkg/flpkg}
	printf '#!/bin/sh\necho SOURCES "$@" > "%s/hook"\n' "$d" > "$d/bin/marker"; chmod +x "$d/bin/marker"
	unset FLPKG_NO_SOURCES
	hook() { rm -f "$d/hook"; FLPKG_SOURCES=$d/bin/marker "$@" > /dev/null 2>&1 < /dev/null; }
	hook "$B" install foo;                  check "install NAME goes to the front end" 'grep -q "install foo" "$d/hook"'
	hook "$B" upgrade --sync;               check "upgrade --sync (a person typing it) goes to the front end" 'grep -q "upgrade --sync" "$d/hook"'
	hook "$B" install -y foo;               check "install -y as a person (not root) goes to the front end (then flpkg-seat)" 'grep -q "install -y foo" "$d/hook"'
	hook "$B" --root /tmp/x install foo;    check "--root stays native" '[ ! -e "$d/hook" ]'
	hook "$B" install ./a.flpkg;            check "a package file as a person goes to the front end (it explains root is needed)" 'grep -q "a.flpkg" "$d/hook"'
	env -u FLPKG_ASSUME_TTY FLPKG_SOURCES=$d/bin/marker bash -c 'rm -f "$1/hook"; "$2" list < /dev/null 2>&1 | cat > /dev/null' _ "$d" "$B"; check "list into a pipe stays native (Store, Packages)" '[ ! -e "$d/hook" ]'
	FLPKG_ASSUME_TTY=1 hook "$B" list;      check "list on a terminal goes to the front end" 'grep -q "list" "$d/hook"'
	FLPKG_ASSUME_TTY=1 hook "$B" search "";  check "search of nothing stays native" '[ ! -e "$d/hook" ]'
	FLPKG_NO_SOURCES=1 hook "$B" install foo; check "FLPKG_NO_SOURCES=1 stays native" '[ ! -e "$d/hook" ]'
else
	echo "  (no host-built flpkg binary: the C hook checks were NOT RUN)"
fi
echo
[ "$fails" = 0 ] && echo "m22 flpkg sources: all passed" || { echo "m22 flpkg sources: $fails FAILED"; exit 1; }
