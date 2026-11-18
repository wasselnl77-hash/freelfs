---
tags: [component, app]
---
# flarchive

#component #app

flarchive [ARCHIVE] browse an archive flarchive --extract-here ARCHIVE... extract next to the archive, show progress, close when done flarchive --compress PATH... the new-archive page with these files and folders in it flarchive --new...

## Source files

- [[docs/graph/files/flarchive/flarchive.c|src/flarchive/flarchive.c]] -- 2070 lines -- 66 fn

## Uses

- component [[docs/graph/components/flui|flui]] (hdr:flui.h, pkg)
- libraries: `libarchive`

## Talks to

- CLI [[docs/graph/components/flfiles|flfiles]]: `flfiles`

## Used by

- [[docs/graph/components/flfiles|flfiles]] (cli)

## Installed by

- [[docs/graph/packages/flarchive|pkg/flarchive]] (recipe `recipes/flarchive/recipe`)

## Docs

- [[docs/manual/01-introduction|01-introduction]]
- [[docs/manual/05-keyboard-shortcuts|05-keyboard-shortcuts]]
- [[docs/manual/06-apps|06-apps]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/24-writing-an-app|24-writing-an-app]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/28-roadmap|28-roadmap]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `d3131d5` Round 19: eject/cwd fix, app icons, server-side decorations, stride fix
- `ebb6a28` Round 16: laptop-test fixes, security, USB drives, persistence, desktop QoL
- `071cf4c` FreeLFS: import current tree (M10 + hardware fixes + XWayland, ISO 49c5df24)

See also: [src/flarchive/README](../../../src/flarchive/README) · [01-introduction](../../manual/01-introduction.md) · [05-keyboard-shortcuts](../../manual/05-keyboard-shortcuts.md) · [06-apps](../../manual/06-apps.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
