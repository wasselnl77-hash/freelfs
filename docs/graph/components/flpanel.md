---
tags: [component, system]
---
# flpanel

#component #system

The panel (44 px at the bottom by default; panel_position=top, panel_height=36..64 and panel_icon_size=16..28 in desktop.conf, all live, set in fltheme) and the start menu popup. One process, one fl_shell binding (protocol...

## Source files

- [[docs/graph/files/flpanel/battery.c|src/flpanel/battery.c]] -- 438 lines -- 17 fn
- [[docs/graph/files/flpanel/ctxmenu.c|src/flpanel/ctxmenu.c]] -- 160 lines -- 6 fn
- [[docs/graph/files/flpanel/deskicons.c|src/flpanel/deskicons.c]] -- 994 lines -- 39 fn
- [[docs/graph/files/flpanel/dialog.c|src/flpanel/dialog.c]] -- 368 lines -- 14 fn
- [[docs/graph/files/flpanel/fileinfo.c|src/flpanel/fileinfo.c]] -- 611 lines -- 21 fn
- [[docs/graph/files/flpanel/fl-shell-client-protocol.h|src/flpanel/fl-shell-client-protocol.h]] -- 699 lines (generated)
- [[docs/graph/files/flpanel/fl-shell-protocol.c|src/flpanel/fl-shell-protocol.c]] -- 87 lines (generated)
- [[docs/graph/files/flpanel/flnotify.c|src/flpanel/flnotify.c]] -- 264 lines -- 4 fn
- [[docs/graph/files/flpanel/flpanel.h|src/flpanel/flpanel.h]] -- 223 lines -- 2 fn
- [[docs/graph/files/flpanel/flpower.c|src/flpanel/flpower.c]] -- 300 lines -- 12 fn
- [[docs/graph/files/flpanel/flyout.c|src/flpanel/flyout.c]] -- 100 lines -- 2 fn
- [[docs/graph/files/flpanel/icons.c|src/flpanel/icons.c]] -- 372 lines -- 13 fn
- [[docs/graph/files/flpanel/icons.h|src/flpanel/icons.h]] -- 20 lines
- [[docs/graph/files/flpanel/menu.c|src/flpanel/menu.c]] -- 563 lines -- 16 fn
- [[docs/graph/files/flpanel/notify.c|src/flpanel/notify.c]] -- 1490 lines -- 56 fn
- [[docs/graph/files/flpanel/panel.c|src/flpanel/panel.c]] -- 1192 lines -- 46 fn
- [[docs/graph/files/flpanel/perfmode.c|src/flpanel/perfmode.c]] -- 43 lines -- 4 fn
- [[docs/graph/files/flpanel/perfmode.h|src/flpanel/perfmode.h]] -- 11 lines
- [[docs/graph/files/flpanel/quick.c|src/flpanel/quick.c]] -- 545 lines -- 27 fn
- [[docs/graph/files/flpanel/sound.h|src/flpanel/sound.h]] -- 26 lines -- 1 fn
- [[docs/graph/files/flpanel/wsmenu.c|src/flpanel/wsmenu.c]] -- 230 lines -- 13 fn

## Uses

- component [[docs/graph/components/flui|flui]] (hdr:flui.h, pkg)
- libraries: `dbus-1`, `libpng`, `wayland-client`

## Talks to

- CLI [[docs/graph/components/fldisplay|fldisplay]]: `fldisplay`
- CLI [[docs/graph/components/flsound|flsound]]: `flsound`
- CLI [[docs/graph/components/flterm|flterm]]: `flterm`
- D-Bus: `org.freedesktop.Notifications`, `org.freelfs.Notifications`
- Wayland: `fl_shell`

## Used by

- [[docs/graph/components/flcrash|flcrash]] (cli)
- [[docs/graph/components/flgreet|flgreet]] (cli)
- [[docs/graph/components/snappel|snappel]] (cli)

## Installed by

- [[docs/graph/packages/flpanel|pkg/flpanel]] (recipe `recipes/flpanel/recipe`)

## Docs

- [[docs/manual/01-introduction|01-introduction]]
- [[docs/manual/04-the-desktop|04-the-desktop]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/11-boot-process|11-boot-process]]
- [[docs/manual/12-filesystem-layout|12-filesystem-layout]]
- [[docs/manual/14-services-init|14-services-init]]
- [[docs/manual/15-users-security|15-users-security]]
- [[docs/manual/17-hardware-drivers|17-hardware-drivers]]
- [[docs/manual/18-performance-ram|18-performance-ram]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/21-recipes|21-recipes]]
- [[docs/manual/22-flui-toolkit|22-flui-toolkit]]
- [[docs/manual/23-compositor-protocols|23-compositor-protocols]]
- [[docs/manual/24-writing-an-app|24-writing-an-app]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/28-roadmap|28-roadmap]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `b469866` Round 22: flpkg one command for every source, signed online repo, fixes, new docs
- `e62d37c` Round 21: boot stick ownership, Load into RAM, FreeBWS GPU freeze fix, flboot entries
- `27f10f9` Round 20: Wi-Fi drivers, NVIDIA phase 1, flat splash, FreeBWS watchdogs, fixes
- `d3131d5` Round 19: eject/cwd fix, app icons, server-side decorations, stride fix
- `1e1c899` Round 18: shutdown hardening, visible launch errors, honest tests, manual, website

See also: [src/flpanel/README](../../../src/flpanel/README) · [01-introduction](../../manual/01-introduction.md) · [04-the-desktop](../../manual/04-the-desktop.md) · [10-architecture](../../manual/10-architecture.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
