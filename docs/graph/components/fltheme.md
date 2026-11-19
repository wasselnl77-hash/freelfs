---
tags: [component, app]
---
# fltheme

#component #app

One window for everything about how the desktop looks. Each control writes ~/.config/freelfs/desktop.conf and the running desktop follows at once (libflui apps and the panel watch the file, flcomp reads it for its own colours, the X...

## Source files

- [[docs/graph/files/fltheme/fltheme.c|src/fltheme/fltheme.c]] -- 901 lines -- 28 fn
- [[docs/graph/files/fltheme/fltheme.h|src/fltheme/fltheme.h]] -- 42 lines
- [[docs/graph/files/fltheme/hints.c|src/fltheme/hints.c]] -- 133 lines -- 4 fn
- [[docs/graph/files/fltheme/presets.c|src/fltheme/presets.c]] -- 271 lines -- 15 fn

## Uses

- component [[docs/graph/components/flui|flui]] (hdr:flpalette.h, hdr:flui.h, pkg)
- libraries: `--libs`, `-lm`, `pkg-config`, `wayland-client`, `xkbcommon`

## Talks to

- CLI [[docs/graph/components/flsettings|flsettings]]: `flsettings`
- CLI [[docs/graph/components/flsplash|flsplash]]: `flsplash-set`

## Used by

- nothing in the repo

## Installed by

- [[docs/graph/packages/fltheme|pkg/fltheme]] (recipe `recipes/fltheme/recipe`)

## Docs

- [[docs/manual/05-keyboard-shortcuts|05-keyboard-shortcuts]]
- [[docs/manual/06-apps|06-apps]]
- [[docs/manual/08-settings-reference|08-settings-reference]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/12-filesystem-layout|12-filesystem-layout]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/21-recipes|21-recipes]]
- [[docs/manual/22-flui-toolkit|22-flui-toolkit]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/28-roadmap|28-roadmap]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `4b7a9cc` Round 23: security hardening
- `27f10f9` Round 20: Wi-Fi drivers, NVIDIA phase 1, flat splash, FreeBWS watchdogs, fixes
- `d3131d5` Round 19: eject/cwd fix, app icons, server-side decorations, stride fix
- `1e1c899` Round 18: shutdown hardening, visible launch errors, honest tests, manual, website
- `b0715e7` Round 13: compositor animations, Themes app (fltheme), light scheme

See also: [src/fltheme/README](../../../src/fltheme/README) · [05-keyboard-shortcuts](../../manual/05-keyboard-shortcuts.md) · [06-apps](../../manual/06-apps.md) · [08-settings-reference](../../manual/08-settings-reference.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
