---
tags: [component, app]
---
# flfiles

#component #app

flfiles [FOLDER]

## Source files

- [[docs/graph/files/flfiles/flfiles.c|src/flfiles/flfiles.c]] -- 4026 lines -- 139 fn

## Uses

- component [[docs/graph/components/flui|flui]] (hdr:flui.h, pkg)
- libraries: none

## Talks to

- CLI [[docs/graph/components/flarchive|flarchive]]: `flarchive`
- CLI [[docs/graph/components/flcrash|flcrash]]: `flcrash`
- CLI [[docs/graph/components/fledit|fledit]]: `fledit`
- CLI [[docs/graph/components/flmount|flmount]]: `flmount`
- CLI [[docs/graph/components/flterm|flterm]]: `flterm`

## Used by

- [[docs/graph/components/flarchive|flarchive]] (cli)

## Installed by

- [[docs/graph/packages/flfiles|pkg/flfiles]] (recipe `recipes/flfiles/recipe`)

## Docs

- [[docs/manual/01-introduction|01-introduction]]
- [[docs/manual/05-keyboard-shortcuts|05-keyboard-shortcuts]]
- [[docs/manual/06-apps|06-apps]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/22-flui-toolkit|22-flui-toolkit]]
- [[docs/manual/23-compositor-protocols|23-compositor-protocols]]
- [[docs/manual/24-writing-an-app|24-writing-an-app]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/28-roadmap|28-roadmap]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `b469866` Round 22: flpkg one command for every source, signed online repo, fixes, new docs
- `e62d37c` Round 21: boot stick ownership, Load into RAM, FreeBWS GPU freeze fix, flboot entries
- `27f10f9` Round 20: Wi-Fi drivers, NVIDIA phase 1, flat splash, FreeBWS watchdogs, fixes
- `d3131d5` Round 19: eject/cwd fix, app icons, server-side decorations, stride fix
- `1e1c899` Round 18: shutdown hardening, visible launch errors, honest tests, manual, website

See also: [src/flfiles/README](../../../src/flfiles/README) · [01-introduction](../../manual/01-introduction.md) · [05-keyboard-shortcuts](../../manual/05-keyboard-shortcuts.md) · [06-apps](../../manual/06-apps.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
