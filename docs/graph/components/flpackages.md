---
tags: [component, system]
---
# flpackages

#component #system

A window like flfiles: Installed and Available tabs, a search field, the package list, details with Install and Remove, a log strip with flpkg's output while a change runs, and a status bar.

## Source files

- [[docs/graph/files/flpackages/flauth-check.h|src/flpackages/flauth-check.h]] -- 248 lines -- 12 fn
- [[docs/graph/files/flpackages/flpackages.c|src/flpackages/flpackages.c]] -- 1812 lines -- 68 fn
- [[docs/graph/files/flpackages/flpkg-seat.c|src/flpackages/flpkg-seat.c]] -- 134 lines -- 2 fn

## Uses

- component [[docs/graph/components/flui|flui]] (hdr:flui.h, pkg)
- libraries: none

## Talks to

- none found

## Used by

- nothing in the repo

## Installed by

- [[docs/graph/packages/flpackages|pkg/flpackages]] (recipe `recipes/flpackages/recipe`)

## Docs

- [[docs/manual/06-apps|06-apps]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/13-packages-flpkg|13-packages-flpkg]]
- [[docs/manual/15-users-security|15-users-security]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/20-building-from-source|20-building-from-source]]
- [[docs/manual/21-recipes|21-recipes]]
- [[docs/manual/23-compositor-protocols|23-compositor-protocols]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `4b7a9cc` Round 23: security hardening
- `b469866` Round 22: flpkg one command for every source, signed online repo, fixes, new docs
- `d3131d5` Round 19: eject/cwd fix, app icons, server-side decorations, stride fix
- `ebb6a28` Round 16: laptop-test fixes, security, USB drives, persistence, desktop QoL
- `071cf4c` FreeLFS: import current tree (M10 + hardware fixes + XWayland, ISO 49c5df24)

See also: [src/flpackages/README](../../../src/flpackages/README) · [06-apps](../../manual/06-apps.md) · [10-architecture](../../manual/10-architecture.md) · [13-packages-flpkg](../../manual/13-packages-flpkg.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
