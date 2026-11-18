---
tags: [component, app]
---
# flgm

#component #app

general-manager [--tab processes|performance|startup|users|details]

## Source files

- [[docs/graph/files/flgm/gm.c|src/flgm/gm.c]] -- 1578 lines -- 50 fn
- [[docs/graph/files/flgm/gm.h|src/flgm/gm.h]] -- 235 lines -- 2 fn
- [[docs/graph/files/flgm/gmui.h|src/flgm/gmui.h]] -- 107 lines
- [[docs/graph/files/flgm/perf.c|src/flgm/perf.c]] -- 745 lines -- 24 fn
- [[docs/graph/files/flgm/proc.c|src/flgm/proc.c]] -- 765 lines -- 27 fn
- [[docs/graph/files/flgm/rdr.c|src/flgm/rdr.c]] -- 263 lines -- 8 fn
- [[docs/graph/files/flgm/startup.c|src/flgm/startup.c]] -- 275 lines -- 9 fn
- [[docs/graph/files/flgm/sys.c|src/flgm/sys.c]] -- 869 lines -- 36 fn

## Uses

- component [[docs/graph/components/flui|flui]] (hdr:flui.h, pkg)
- libraries: none

## Talks to

- none found

## Used by

- nothing in the repo

## Installed by

- [[docs/graph/packages/flgm|pkg/flgm]] (recipe `recipes/flgm/recipe`)

## Docs

- [[docs/manual/01-introduction|01-introduction]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/23-compositor-protocols|23-compositor-protocols]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/28-roadmap|28-roadmap]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `b469866` Round 22: flpkg one command for every source, signed online repo, fixes, new docs
- `e62d37c` Round 21: boot stick ownership, Load into RAM, FreeBWS GPU freeze fix, flboot entries
- `27f10f9` Round 20: Wi-Fi drivers, NVIDIA phase 1, flat splash, FreeBWS watchdogs, fixes
- `d3131d5` Round 19: eject/cwd fix, app icons, server-side decorations, stride fix
- `071cf4c` FreeLFS: import current tree (M10 + hardware fixes + XWayland, ISO 49c5df24)

See also: [src/flgm/README](../../../src/flgm/README) · [01-introduction](../../manual/01-introduction.md) · [10-architecture](../../manual/10-architecture.md) · [23-compositor-protocols](../../manual/23-compositor-protocols.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
