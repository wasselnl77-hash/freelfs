---
tags: [component, app]
---
# flvideo

#component #app

flvideo [--start SECONDS] [--pause] [FILE]

## Source files

- [[docs/graph/files/flvideo/flvideo.c|src/flvideo/flvideo.c]] -- 1344 lines -- 49 fn

## Uses

- component [[docs/graph/components/flui|flui]] (hdr:flui.h, pkg)
- libraries: `dbus-1`, `mpv`

## Talks to

- D-Bus: `org.freedesktop.portal.Desktop`, `org.freedesktop.portal.FileChooser`, `org.freedesktop.portal.Request`

## Used by

- nothing in the repo

## Installed by

- [[docs/graph/packages/flvideo|pkg/flvideo]] (recipe `recipes/flvideo/recipe`)

## Docs

- [[docs/manual/01-introduction|01-introduction]]
- [[docs/manual/05-keyboard-shortcuts|05-keyboard-shortcuts]]
- [[docs/manual/06-apps|06-apps]]
- [[docs/manual/09-troubleshooting|09-troubleshooting]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/17-hardware-drivers|17-hardware-drivers]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/28-roadmap|28-roadmap]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `d3131d5` Round 19: eject/cwd fix, app icons, server-side decorations, stride fix
- `1e1c899` Round 18: shutdown hardening, visible launch errors, honest tests, manual, website
- `6a1e3de` Round 12: RAM trim, live store, desktop icons, small fixes, security ops
- `071cf4c` FreeLFS: import current tree (M10 + hardware fixes + XWayland, ISO 49c5df24)

See also: [src/flvideo/README](../../../src/flvideo/README) · [01-introduction](../../manual/01-introduction.md) · [05-keyboard-shortcuts](../../manual/05-keyboard-shortcuts.md) · [06-apps](../../manual/06-apps.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
