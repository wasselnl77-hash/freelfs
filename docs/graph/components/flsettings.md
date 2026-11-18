---
tags: [component, app]
---
# flsettings

#component #app

Pages behind a sidebar: General (theme, panel labels), Display, Mouse & touchpad, Keyboard (pages.c), Workspaces (workspaces.c), Date & time (datetime.c) and Wi-Fi, then Snapshots and About. `flsettings --page...

## Source files

- [[docs/graph/files/flsettings/about.c|src/flsettings/about.c]] -- 414 lines -- 14 fn
- [[docs/graph/files/flsettings/about.h|src/flsettings/about.h]] -- 18 lines
- [[docs/graph/files/flsettings/bluetooth.c|src/flsettings/bluetooth.c]] -- 477 lines -- 16 fn
- [[docs/graph/files/flsettings/bluetooth.h|src/flsettings/bluetooth.h]] -- 18 lines
- [[docs/graph/files/flsettings/cmd.c|src/flsettings/cmd.c]] -- 342 lines -- 13 fn
- [[docs/graph/files/flsettings/cmd.h|src/flsettings/cmd.h]] -- 42 lines
- [[docs/graph/files/flsettings/datetime.c|src/flsettings/datetime.c]] -- 456 lines -- 19 fn
- [[docs/graph/files/flsettings/datetime.h|src/flsettings/datetime.h]] -- 18 lines
- [[docs/graph/files/flsettings/display.c|src/flsettings/display.c]] -- 862 lines -- 34 fn
- [[docs/graph/files/flsettings/display.h|src/flsettings/display.h]] -- 19 lines
- [[docs/graph/files/flsettings/fl-shell-client-protocol.h|src/flsettings/fl-shell-client-protocol.h]] -- 699 lines (generated)
- [[docs/graph/files/flsettings/fl-shell-protocol.c|src/flsettings/fl-shell-protocol.c]] -- 87 lines (generated)
- [[docs/graph/files/flsettings/flsettings.c|src/flsettings/flsettings.c]] -- 500 lines -- 19 fn
- [[docs/graph/files/flsettings/pages.c|src/flsettings/pages.c]] -- 623 lines -- 39 fn
- [[docs/graph/files/flsettings/pages.h|src/flsettings/pages.h]] -- 48 lines
- [[docs/graph/files/flsettings/printers.c|src/flsettings/printers.c]] -- 478 lines -- 15 fn
- [[docs/graph/files/flsettings/printers.h|src/flsettings/printers.h]] -- 18 lines
- [[docs/graph/files/flsettings/snapshots.c|src/flsettings/snapshots.c]] -- 315 lines -- 10 fn
- [[docs/graph/files/flsettings/snapshots.h|src/flsettings/snapshots.h]] -- 18 lines
- [[docs/graph/files/flsettings/wallpaper.c|src/flsettings/wallpaper.c]] -- 766 lines -- 28 fn
- [[docs/graph/files/flsettings/wallpaper.h|src/flsettings/wallpaper.h]] -- 25 lines
- [[docs/graph/files/flsettings/wifi.c|src/flsettings/wifi.c]] -- 1136 lines -- 52 fn
- [[docs/graph/files/flsettings/wifi.h|src/flsettings/wifi.h]] -- 23 lines
- [[docs/graph/files/flsettings/workspaces.c|src/flsettings/workspaces.c]] -- 220 lines -- 9 fn
- [[docs/graph/files/flsettings/workspaces.h|src/flsettings/workspaces.h]] -- 21 lines

## Uses

- component [[docs/graph/components/flui|flui]] (hdr:flui.h, pkg)
- libraries: `dbus-1`, `libjpeg`, `libpng`, `wayland-client`, `zlib`

## Talks to

- CLI [[docs/graph/components/fldisplay|fldisplay]]: `fldisplay`
- D-Bus: `net.connman.iwd`
- Wayland: `fl_shell`

## Used by

- [[docs/graph/components/flinstall|flinstall]] (hdr)
- [[docs/graph/components/fltheme|fltheme]] (cli)
- [[docs/graph/components/flview|flview]] (cli)

## Installed by

- [[docs/graph/packages/flsettings|pkg/flsettings]] (recipe `recipes/flsettings/recipe`)
- also copied from src/flsettings by [[docs/graph/packages/flinstall|pkg/flinstall]]

## Docs

- [[docs/manual/01-introduction|01-introduction]]
- [[docs/manual/05-keyboard-shortcuts|05-keyboard-shortcuts]]
- [[docs/manual/06-apps|06-apps]]
- [[docs/manual/08-settings-reference|08-settings-reference]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/12-filesystem-layout|12-filesystem-layout]]
- [[docs/manual/14-services-init|14-services-init]]
- [[docs/manual/15-users-security|15-users-security]]
- [[docs/manual/16-networking|16-networking]]
- [[docs/manual/17-hardware-drivers|17-hardware-drivers]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/21-recipes|21-recipes]]
- [[docs/manual/22-flui-toolkit|22-flui-toolkit]]
- [[docs/manual/23-compositor-protocols|23-compositor-protocols]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `d3131d5` Round 19: eject/cwd fix, app icons, server-side decorations, stride fix
- `18b0ace` Round 17: Bluetooth, printing, display settings, night light, desktop QoL
- `ebb6a28` Round 16: laptop-test fixes, security, USB drives, persistence, desktop QoL
- `2c2183d` Round 14: security updates, kernel 7.3.4 (rolling), WebKit sandbox; Intel/AMD prep
- `6ebf49f` Workspaces UI: Settings > Workspaces page, pager right-click menu

See also: [src/flsettings/README](../../../src/flsettings/README) · [01-introduction](../../manual/01-introduction.md) · [05-keyboard-shortcuts](../../manual/05-keyboard-shortcuts.md) · [06-apps](../../manual/06-apps.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
