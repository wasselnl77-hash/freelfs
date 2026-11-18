---
tags: [component, app]
---
# flcrash

#component #app

Shown when the desktop session ends badly: flcomp killed by a signal (crash, out of memory) or exiting with a non-zero status. /etc/profile.d/desktop.sh (flcomp package) runs it after flcomp and acts on its exit status.

## Source files

- [[docs/graph/files/flcrash/flcrash.c|src/flcrash/flcrash.c]] -- 1584 lines -- 54 fn

## Uses

- component [[docs/graph/components/flui|flui]] (hdr:flui.h, pkg)
- libraries: `libdrm`, `libinput`, `libseat`

## Talks to

- CLI [[docs/graph/components/flpanel|flpanel]]: `flpower`
- CLI [[docs/graph/components/flsound|flsound]]: `flsound`
- Wayland: `xdg_surface`

## Used by

- [[docs/graph/components/flfiles|flfiles]] (cli)
- [[docs/graph/components/flsh|flsh]] (cli)

## Installed by

- [[docs/graph/packages/flcrash|pkg/flcrash]] (recipe `recipes/flcrash/recipe`)

## Docs

- [[docs/manual/01-introduction|01-introduction]]
- [[docs/manual/06-apps|06-apps]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/11-boot-process|11-boot-process]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/23-compositor-protocols|23-compositor-protocols]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/28-roadmap|28-roadmap]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `c9604b6` Round 15: folder layout (/usr real, /sysflfs alias, homes in /home); hardware build
- `6a1e3de` Round 12: RAM trim, live store, desktop icons, small fixes, security ops
- `071cf4c` FreeLFS: import current tree (M10 + hardware fixes + XWayland, ISO 49c5df24)

See also: [src/flcrash/README](../../../src/flcrash/README) · [01-introduction](../../manual/01-introduction.md) · [06-apps](../../manual/06-apps.md) · [10-architecture](../../manual/10-architecture.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
