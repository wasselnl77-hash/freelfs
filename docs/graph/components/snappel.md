---
tags: [component, app]
---
# snappel

#component #app

Takes pictures of the screen through flcomp's fl_screencopy protocol, lets you annotate them, and saves them to ~/pictures/screenshots (snappel-YYYY-MM-DD-HHMMSS.png) or the clipboard.

## Source files

- [[docs/graph/files/snappel/cap.c|src/snappel/cap.c]] -- 289 lines -- 19 fn
- [[docs/graph/files/snappel/cap.h|src/snappel/cap.h]] -- 28 lines
- [[docs/graph/files/snappel/fl-screencopy-client-protocol.h|src/snappel/fl-screencopy-client-protocol.h]] -- 366 lines (generated)
- [[docs/graph/files/snappel/fl-screencopy-protocol.c|src/snappel/fl-screencopy-protocol.c]] -- 78 lines (generated)
- [[docs/graph/files/snappel/img.c|src/snappel/img.c]] -- 314 lines -- 19 fn
- [[docs/graph/files/snappel/img.h|src/snappel/img.h]] -- 35 lines
- [[docs/graph/files/snappel/snappel.c|src/snappel/snappel.c]] -- 1180 lines -- 47 fn

## Uses

- component [[docs/graph/components/flui|flui]] (hdr:flui.h, pkg)
- libraries: `libpng`, `wayland-client`

## Talks to

- CLI [[docs/graph/components/flpanel|flpanel]]: `flnotify`
- CLI [[docs/graph/components/flview|flview]]: `flview`
- Wayland: `fl_screencopy_frame`, `fl_screencopy_manager`, `wl_data_device_manager`, `wl_seat`, `wl_shm`

## Used by

- [[docs/graph/components/flcomp|flcomp]] (cli)

## Installed by

- [[docs/graph/packages/snappel|pkg/snappel]] (recipe `recipes/snappel/recipe`)

## Docs

- [[docs/manual/05-keyboard-shortcuts|05-keyboard-shortcuts]]
- [[docs/manual/06-apps|06-apps]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/23-compositor-protocols|23-compositor-protocols]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/27-freebws-internals|27-freebws-internals]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `b469866` Round 22: flpkg one command for every source, signed online repo, fixes, new docs
- `d3131d5` Round 19: eject/cwd fix, app icons, server-side decorations, stride fix
- `071cf4c` FreeLFS: import current tree (M10 + hardware fixes + XWayland, ISO 49c5df24)

See also: [src/snappel/README](../../../src/snappel/README) · [05-keyboard-shortcuts](../../manual/05-keyboard-shortcuts.md) · [06-apps](../../manual/06-apps.md) · [10-architecture](../../manual/10-architecture.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
