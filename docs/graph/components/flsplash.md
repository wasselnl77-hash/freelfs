---
tags: [component, system]
---
# flsplash

#component #system

flsplash -- the FreeLFS boot screen

## Source files

- [[docs/graph/files/flsplash/flauth-check.h|src/flsplash/flauth-check.h]] -- 248 lines -- 12 fn
- [[docs/graph/files/flsplash/flsplash-set.c|src/flsplash/flsplash-set.c]] -- 134 lines -- 4 fn
- [[docs/graph/files/flsplash/flsplash.c|src/flsplash/flsplash.c]] -- 1669 lines -- 68 fn
- [[docs/graph/files/flsplash/genart.c|src/flsplash/genart.c]] -- 837 lines -- 28 fn

## Uses

- libraries: `freetype2`

## Talks to

- none found

## Used by

- [[docs/graph/components/flgreet|flgreet]] (cli)
- [[docs/graph/components/fltheme|fltheme]] (cli)

## Installed by

- [[docs/graph/packages/flsplash|pkg/flsplash]] (recipe `recipes/flsplash/recipe`)

## Docs

- [[docs/manual/01-introduction|01-introduction]]
- [[docs/manual/02-getting-started|02-getting-started]]
- [[docs/manual/03-installing|03-installing]]
- [[docs/manual/06-apps|06-apps]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/11-boot-process|11-boot-process]]
- [[docs/manual/14-services-init|14-services-init]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `4b7a9cc` Round 23: security hardening
- `27f10f9` Round 20: Wi-Fi drivers, NVIDIA phase 1, flat splash, FreeBWS watchdogs, fixes
- `d3131d5` Round 19: eject/cwd fix, app icons, server-side decorations, stride fix
- `1e1c899` Round 18: shutdown hardening, visible launch errors, honest tests, manual, website
- `071cf4c` FreeLFS: import current tree (M10 + hardware fixes + XWayland, ISO 49c5df24)

See also: [src/flsplash/README](../../../src/flsplash/README) · [01-introduction](../../manual/01-introduction.md) · [02-getting-started](../../manual/02-getting-started.md) · [03-installing](../../manual/03-installing.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
