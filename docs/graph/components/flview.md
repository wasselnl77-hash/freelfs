---
tags: [component, app]
---
# flview

#component #app

flview [--rotate N] [--zoom PERCENT] [--no-strip] [FILE|FOLDER]

## Source files

- [[docs/graph/files/flview/flview.c|src/flview/flview.c]] -- 1707 lines -- 53 fn

## Uses

- component [[docs/graph/components/flui|flui]] (hdr:flui.h, pkg)
- libraries: `libjpeg`, `libpng`, `libwebp`, `libwebpdemux`

## Talks to

- CLI [[docs/graph/components/flsettings|flsettings]]: `flsettings`

## Used by

- [[docs/graph/components/snappel|snappel]] (cli)

## Installed by

- [[docs/graph/packages/flview|pkg/flview]] (recipe `recipes/flview/recipe`)

## Docs

- [[docs/manual/01-introduction|01-introduction]]
- [[docs/manual/05-keyboard-shortcuts|05-keyboard-shortcuts]]
- [[docs/manual/06-apps|06-apps]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/21-recipes|21-recipes]]
- [[docs/manual/22-flui-toolkit|22-flui-toolkit]]
- [[docs/manual/24-writing-an-app|24-writing-an-app]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/26-contributing|26-contributing]]
- [[docs/manual/28-roadmap|28-roadmap]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `d3131d5` Round 19: eject/cwd fix, app icons, server-side decorations, stride fix
- `1e1c899` Round 18: shutdown hardening, visible launch errors, honest tests, manual, website
- `18b0ace` Round 17: Bluetooth, printing, display settings, night light, desktop QoL
- `ebb6a28` Round 16: laptop-test fixes, security, USB drives, persistence, desktop QoL
- `071cf4c` FreeLFS: import current tree (M10 + hardware fixes + XWayland, ISO 49c5df24)

See also: [src/flview/README](../../../src/flview/README) · [01-introduction](../../manual/01-introduction.md) · [05-keyboard-shortcuts](../../manual/05-keyboard-shortcuts.md) · [06-apps](../../manual/06-apps.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
