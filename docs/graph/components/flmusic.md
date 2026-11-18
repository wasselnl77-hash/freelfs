---
tags: [component, app]
---
# flmusic

#component #app

flmusic [--tab albums|artists|songs] [--search TEXT] [FOLDER | FILE...] flmusic --ctl play|pause|play-pause|stop|next|previous|status

## Source files

- [[docs/graph/files/flmusic/flmusic.c|src/flmusic/flmusic.c]] -- 1642 lines -- 72 fn
- [[docs/graph/files/flmusic/mpris.c|src/flmusic/mpris.c]] -- 539 lines -- 16 fn
- [[docs/graph/files/flmusic/mpris.h|src/flmusic/mpris.h]] -- 58 lines

## Uses

- component [[docs/graph/components/flui|flui]] (hdr:flui.h, pkg)
- libraries: `dbus-1`, `gstreamer-1.0`, `gstreamer-pbutils-1.0`

## Talks to

- D-Bus: `org.mpris.MediaPlayer2`

## Used by

- nothing in the repo

## Installed by

- [[docs/graph/packages/flmusic|pkg/flmusic]] (recipe `recipes/flmusic/recipe`)

## Docs

- [[docs/manual/01-introduction|01-introduction]]
- [[docs/manual/05-keyboard-shortcuts|05-keyboard-shortcuts]]
- [[docs/manual/06-apps|06-apps]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/17-hardware-drivers|17-hardware-drivers]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/23-compositor-protocols|23-compositor-protocols]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/28-roadmap|28-roadmap]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `d3131d5` Round 19: eject/cwd fix, app icons, server-side decorations, stride fix
- `1e1c899` Round 18: shutdown hardening, visible launch errors, honest tests, manual, website
- `071cf4c` FreeLFS: import current tree (M10 + hardware fixes + XWayland, ISO 49c5df24)

See also: [src/flmusic/README](../../../src/flmusic/README) · [01-introduction](../../manual/01-introduction.md) · [05-keyboard-shortcuts](../../manual/05-keyboard-shortcuts.md) · [06-apps](../../manual/06-apps.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
