---
tags: [component, app]
---
# flsetup

#component #app

Started by `flcomp --firstboot` when /etc/keyboard.conf is missing. Four layouts (QWERTZ German, QWERTY US, QWERTY UK, AZERTY French), a preview of the keys and a field to try them in (it uses the chosen layout at once, not the...

## Source files

- [[docs/graph/files/flsetup/flsetup.c|src/flsetup/flsetup.c]] -- 115 lines -- 4 fn

## Uses

- component [[docs/graph/components/flui|flui]] (hdr:flui.h, pkg)
- libraries: none

## Talks to

- none found

## Used by

- nothing in the repo

## Installed by

- [[docs/graph/packages/flsetup|pkg/flsetup]] (recipe `recipes/flsetup/recipe`)

## Docs

- [[docs/manual/06-apps|06-apps]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/11-boot-process|11-boot-process]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/22-flui-toolkit|22-flui-toolkit]]
- [[docs/manual/23-compositor-protocols|23-compositor-protocols]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `071cf4c` FreeLFS: import current tree (M10 + hardware fixes + XWayland, ISO 49c5df24)

See also: [src/flsetup/README](../../../src/flsetup/README) · [06-apps](../../manual/06-apps.md) · [10-architecture](../../manual/10-architecture.md) · [11-boot-process](../../manual/11-boot-process.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
