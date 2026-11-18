---
tags: [component, library]
---
# flui

#component #library

Shared library, header (flui.h) and pkg-config file (flui.pc). C17, MIT. flpanel, flterm, flsetup, flsettings, flfiles, fledit and flgreet-ui are built on it. fl_app_mods() gives the modifiers held right now (ui->mods is only as fresh...

## Source files

- [[docs/graph/files/flui/conf.c|src/flui/conf.c]] -- 335 lines -- 19 fn
- [[docs/graph/files/flui/flpalette.h|src/flui/flpalette.h]] -- 150 lines -- 8 fn
- [[docs/graph/files/flui/flui.h|src/flui/flui.h]] -- 418 lines -- 3 fn
- [[docs/graph/files/flui/gfx.c|src/flui/gfx.c]] -- 755 lines -- 32 fn
- [[docs/graph/files/flui/internal.h|src/flui/internal.h]] -- 39 lines -- 1 fn
- [[docs/graph/files/flui/kbd.c|src/flui/kbd.c]] -- 230 lines -- 9 fn
- [[docs/graph/files/flui/test-scroll.c|src/flui/test-scroll.c]] -- 200 lines -- 16 fn
- [[docs/graph/files/flui/text.c|src/flui/text.c]] -- 546 lines -- 25 fn
- [[docs/graph/files/flui/ui.c|src/flui/ui.c]] -- 643 lines -- 20 fn
- [[docs/graph/files/flui/wl.c|src/flui/wl.c]] -- 1554 lines -- 81 fn
- [[docs/graph/files/flui/worldmap.c|src/flui/worldmap.c]] -- 1067 lines -- 48 fn
- [[docs/graph/files/flui/worldmap_data.c|src/flui/worldmap_data.c]] -- 840 lines
- [[docs/graph/files/flui/xdg-shell-client-protocol.h|src/flui/xdg-shell-client-protocol.h]] -- 2392 lines (generated)
- [[docs/graph/files/flui/xdg-shell-protocol.c|src/flui/xdg-shell-protocol.c]] -- 184 lines (generated)

## Uses

- libraries: `fontconfig`, `freetype2`, `harfbuzz`, `wayland-client`, `xkbcommon`

## Talks to

- Wayland: `fl_shell`, `wl_compositor`, `wl_data_device_manager`, `wl_output`, `wl_seat`, `wl_shm`, `xdg_surface`, `xdg_toplevel`, `xdg_wm_base`

## Used by

- [[docs/graph/components/flarchive|flarchive]] (hdr, pkg)
- [[docs/graph/components/flauth|flauth]] (hdr, pkg)
- [[docs/graph/components/flcomp|flcomp]] (hdr)
- [[docs/graph/components/flcrash|flcrash]] (hdr, pkg)
- [[docs/graph/components/fledit|fledit]] (hdr, pkg)
- [[docs/graph/components/flfiles|flfiles]] (hdr, pkg)
- [[docs/graph/components/flgm|flgm]] (hdr, pkg)
- [[docs/graph/components/flgreet|flgreet]] (hdr, pkg)
- [[docs/graph/components/flinstall|flinstall]] (hdr, pkg)
- [[docs/graph/components/fllock|fllock]] (hdr, pkg)
- [[docs/graph/components/flmusic|flmusic]] (hdr, pkg)
- [[docs/graph/components/flpackages|flpackages]] (hdr, pkg)
- [[docs/graph/components/flpanel|flpanel]] (hdr, pkg)
- [[docs/graph/components/flpdf|flpdf]] (hdr, pkg)
- [[docs/graph/components/flportal|flportal]] (hdr, pkg)
- [[docs/graph/components/flsettings|flsettings]] (hdr, pkg)
- [[docs/graph/components/flsetup|flsetup]] (hdr, pkg)
- [[docs/graph/components/flstore|flstore]] (hdr, pkg)
- [[docs/graph/components/flterm|flterm]] (hdr, pkg)
- [[docs/graph/components/fltheme|fltheme]] (hdr, pkg)
- [[docs/graph/components/flvideo|flvideo]] (hdr, pkg)
- [[docs/graph/components/flview|flview]] (hdr, pkg)
- [[docs/graph/components/freebws|freebws]] (hdr, pkg)
- [[docs/graph/components/snappel|snappel]] (hdr, pkg)

## Installed by

- [[docs/graph/packages/flui|pkg/flui]] (recipe `recipes/flui/recipe`)
- also copied from src/flui by [[docs/graph/packages/flcomp|pkg/flcomp]]

## Docs

- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/20-building-from-source|20-building-from-source]]
- [[docs/manual/21-recipes|21-recipes]]
- [[docs/manual/22-flui-toolkit|22-flui-toolkit]]
- [[docs/manual/23-compositor-protocols|23-compositor-protocols]]
- [[docs/manual/24-writing-an-app|24-writing-an-app]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/26-contributing|26-contributing]]
- [[docs/manual/27-freebws-internals|27-freebws-internals]]
- [[docs/manual/28-roadmap|28-roadmap]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `27f10f9` Round 20: Wi-Fi drivers, NVIDIA phase 1, flat splash, FreeBWS watchdogs, fixes
- `ebb6a28` Round 16: laptop-test fixes, security, USB drives, persistence, desktop QoL
- `b0715e7` Round 13: compositor animations, Themes app (fltheme), light scheme
- `6a1e3de` Round 12: RAM trim, live store, desktop icons, small fixes, security ops
- `071cf4c` FreeLFS: import current tree (M10 + hardware fixes + XWayland, ISO 49c5df24)

See also: [src/flui/README](../../../src/flui/README) · [10-architecture](../../manual/10-architecture.md) · [19-admin-reference](../../manual/19-admin-reference.md) · [20-building-from-source](../../manual/20-building-from-source.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
