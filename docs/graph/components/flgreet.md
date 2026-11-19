---
tags: [component, system]
---
# flgreet

#component #system

inittab runs /usr/sbin/flgreet on tty1 instead of agetty.

## Source files

- [[docs/graph/files/flgreet/flgreet-ui.c|src/flgreet/flgreet-ui.c]] -- 686 lines -- 20 fn
- [[docs/graph/files/flgreet/flgreet.c|src/flgreet/flgreet.c]] -- 235 lines -- 8 fn

## Uses

- component [[docs/graph/components/flui|flui]] (hdr:flui.h, pkg)
- libraries: none

## Talks to

- CLI [[docs/graph/components/flcomp|flcomp]]: `flcomp`
- CLI [[docs/graph/components/flpanel|flpanel]]: `flpower`
- CLI [[docs/graph/components/flsplash|flsplash]]: `flsplash`

## Used by

- nothing in the repo

## Installed by

- [[docs/graph/packages/flgreet|pkg/flgreet]] (recipe `recipes/flgreet/recipe`)

## Docs

- [[docs/manual/01-introduction|01-introduction]]
- [[docs/manual/06-apps|06-apps]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/11-boot-process|11-boot-process]]
- [[docs/manual/12-filesystem-layout|12-filesystem-layout]]
- [[docs/manual/14-services-init|14-services-init]]
- [[docs/manual/15-users-security|15-users-security]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/23-compositor-protocols|23-compositor-protocols]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `4b7a9cc` Round 23: security hardening
- `27f10f9` Round 20: Wi-Fi drivers, NVIDIA phase 1, flat splash, FreeBWS watchdogs, fixes
- `1e1c899` Round 18: shutdown hardening, visible launch errors, honest tests, manual, website
- `ebb6a28` Round 16: laptop-test fixes, security, USB drives, persistence, desktop QoL
- `071cf4c` FreeLFS: import current tree (M10 + hardware fixes + XWayland, ISO 49c5df24)

See also: [src/flgreet/README](../../../src/flgreet/README) · [01-introduction](../../manual/01-introduction.md) · [06-apps](../../manual/06-apps.md) · [10-architecture](../../manual/10-architecture.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
