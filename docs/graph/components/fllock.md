---
tags: [component, app]
---
# fllock

#component #app

A big clock, the date, your name and a password field on a plain screen. Super+L, the panel's Lock button and closing the lid lock the screen.

## Source files

- [[docs/graph/files/fllock/fl-shell-client-protocol.h|src/fllock/fl-shell-client-protocol.h]] -- 678 lines (generated)
- [[docs/graph/files/fllock/fl-shell-protocol.c|src/fllock/fl-shell-protocol.c]] -- 85 lines (generated)
- [[docs/graph/files/fllock/fllock-check.c|src/fllock/fllock-check.c]] -- 135 lines -- 3 fn
- [[docs/graph/files/fllock/fllock.c|src/fllock/fllock.c]] -- 415 lines -- 10 fn

## Uses

- component [[docs/graph/components/flui|flui]] (hdr:flui.h, pkg)
- libraries: `wayland-client`

## Talks to

- Wayland: `fl_shell`

## Used by

- nothing in the repo

## Installed by

- [[docs/graph/packages/fllock|pkg/fllock]] (recipe `recipes/fllock/recipe`)

## Docs

- [[docs/manual/01-introduction|01-introduction]]
- [[docs/manual/05-keyboard-shortcuts|05-keyboard-shortcuts]]
- [[docs/manual/06-apps|06-apps]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/15-users-security|15-users-security]]
- [[docs/manual/17-hardware-drivers|17-hardware-drivers]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/23-compositor-protocols|23-compositor-protocols]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/28-roadmap|28-roadmap]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `4b7a9cc` Round 23: security hardening
- `6a1e3de` Round 12: RAM trim, live store, desktop icons, small fixes, security ops
- `071cf4c` FreeLFS: import current tree (M10 + hardware fixes + XWayland, ISO 49c5df24)

See also: [src/fllock/README](../../../src/fllock/README) · [01-introduction](../../manual/01-introduction.md) · [05-keyboard-shortcuts](../../manual/05-keyboard-shortcuts.md) · [06-apps](../../manual/06-apps.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
