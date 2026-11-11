#!/bin/sh
# fltheme without a window: presets, the user's own preset files and the GTK hints, in a scratch
# HOME (nothing outside it is written). `make check`.
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
export HOME=$d XDG_CONFIG_HOME=$d/cfg XDG_DATA_HOME=$d/data
unset FLUI_CONF WAYLAND_DISPLAY
mkdir -p "$d/cfg/freelfs"
conf=$d/cfg/freelfs/desktop.conf
ft=./fltheme
[ -n "$FLUI" ] || FLUI=../flui
export LD_LIBRARY_PATH=$FLUI
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
has() { grep -qx "$2" "$1" && ok "$3" || bad "$3 ($(tr '\n' '|' <"$1" 2>/dev/null))"; }
hasnt() { grep -q "$2" "$1" 2>/dev/null && bad "$3 ($(tr '\n' '|' <"$1"))" || ok "$3"; }

printf 'sounds=0\naccent=blue\nwallpaper=blue\n' >"$conf"
$ft --apply "FreeLFS Light" && ok "apply FreeLFS Light" || bad "apply FreeLFS Light"
has "$conf" theme=light "theme=light is written"
hasnt "$conf" '^accent=' "the light preset puts the accent back to the default"
has "$conf" sounds=0 "keys that are not the theme's stay (sounds)"
has "$conf" wallpaper=blue "wallpaper is not a themed key and stays"
has "$d/cfg/gtk-3.0/settings.ini" gtk-application-prefer-dark-theme=false "GTK 3 hint: light"
has "$d/cfg/gtk-4.0/settings.ini" gtk-application-prefer-dark-theme=false "GTK 4 hint: light"
has "$d/cfg/gtk-3.0/settings.ini" "gtk-font-name=IBM Plex Sans 10" "GTK font from font_size 13"

printf '[Settings]\ngtk-cursor-theme-name=Adwaita\n[Other]\nx=1\n' >"$d/cfg/gtk-3.0/settings.ini"
$ft --hints
has "$d/cfg/gtk-3.0/settings.ini" gtk-cursor-theme-name=Adwaita "an existing line of settings.ini stays"
has "$d/cfg/gtk-3.0/settings.ini" x=1 "other sections stay"
[ "$(grep -c 'prefer-dark' "$d/cfg/gtk-3.0/settings.ini")" = 1 ] && ok "no duplicate keys after a second write" || bad "duplicate keys"

printf 'accent=blue\ntheme=light\n' >"$conf"
$ft --apply Compact
has "$conf" theme=light "Compact keeps the colour scheme"
has "$conf" accent=blue "Compact keeps the accent"
has "$conf" font_size=12 "Compact: font size"
has "$conf" titlebar=compact "Compact: title bar"
has "$conf" panel_height=38 "Compact: panel height"
$ft --list | grep -q '^Compact \*$' && ok "--list marks the preset that is in force" || bad "--list marker: $($ft --list | tr '\n' '|')"

$ft --apply Large && has "$conf" font_size=15 "Large: font size"
$ft --save "My look" && ok "save a user preset" || bad "save a user preset"
[ -f "$d/cfg/freelfs/themes/My look.theme" ] && ok "the file is in themes/ next to desktop.conf" || bad "no themes/My look.theme"
$ft --reset
hasnt "$conf" '^font_size=' "reset removes font_size"
hasnt "$conf" '^panel_height=' "reset removes panel_height"
hasnt "$conf" '^accent=' "reset puts the accent back to the default too"
$ft --apply "My look" && has "$conf" font_size=15 "load a user preset" || bad "load a user preset"
$ft --list | grep -q 'My look (yours)' && ok "--list shows it" || bad "--list does not show My look"

$ft --save "../evil" 2>/dev/null && bad "a path in a preset name is refused" || ok "a path in a preset name is refused"
$ft --save "" 2>/dev/null && bad "an empty name is refused" || ok "an empty name is refused"
printf 'theme=light\nevil=1\nwallpaper=/etc/passwd\nfont=Bad\nfont_size=14\n' >"$d/cfg/freelfs/themes/Hostile.theme"
$ft --apply Hostile
hasnt "$conf" evil "unknown keys of a theme file are ignored"
hasnt "$conf" '^wallpaper=/etc' "wallpaper= from a theme file is ignored"
has "$conf" font_size=14 "known keys of a theme file are loaded"
printf 'nothing useful\n' >"$d/cfg/freelfs/themes/Junk.theme"
$ft --apply Junk 2>/dev/null && bad "a file without theme keys is refused" || ok "a file without theme keys is refused"
$ft --apply "No such preset" 2>/dev/null && bad "an unknown name fails" || ok "an unknown name fails"
[ -z "$(find "$HOME" -newer "$conf" -name '*.tmp*' 2>/dev/null)" ] && ok "no temporary files left" || bad "temporary files left"

[ $fails = 0 ] && echo "all tests passed" || echo "$fails test(s) failed"
exit $fails
