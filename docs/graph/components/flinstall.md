---
tags: [component, system]
---
# flinstall

#component #system

Two programs, C17, MIT.

## Source files

- [[docs/graph/files/flinstall/common.h|src/flinstall/common.h]] -- 160 lines -- 8 fn
- [[docs/graph/files/flinstall/flinstall.c|src/flinstall/flinstall.c]] -- 1929 lines -- 59 fn
- [[docs/graph/files/flinstall/helper.c|src/flinstall/helper.c]] -- 2431 lines -- 70 fn

## Uses

- component [[docs/graph/components/flsettings|flsettings]] (hdr:wifi.h)
- component [[docs/graph/components/flui|flui]] (hdr:flui.h, pkg)
- libraries: `dbus-1`

## Talks to

- none found

## Used by

- [[docs/graph/components/flstore|flstore]] (cli)

## Installed by

- [[docs/graph/packages/flinstall|pkg/flinstall]] (recipe `recipes/flinstall/recipe`)

## Docs

- [[docs/manual/06-apps|06-apps]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/11-boot-process|11-boot-process]]
- [[docs/manual/12-filesystem-layout|12-filesystem-layout]]
- [[docs/manual/15-users-security|15-users-security]]
- [[docs/manual/16-networking|16-networking]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/28-roadmap|28-roadmap]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `d3131d5` Round 19: eject/cwd fix, app icons, server-side decorations, stride fix
- `ebb6a28` Round 16: laptop-test fixes, security, USB drives, persistence, desktop QoL
- `c9604b6` Round 15: folder layout (/usr real, /sysflfs alias, homes in /home); hardware build
- `2c2183d` Round 14: security updates, kernel 7.3.4 (rolling), WebKit sandbox; Intel/AMD prep
- `6a1e3de` Round 12: RAM trim, live store, desktop icons, small fixes, security ops

See also: [src/flinstall/README](../../../src/flinstall/README) · [06-apps](../../manual/06-apps.md) · [10-architecture](../../manual/10-architecture.md) · [11-boot-process](../../manual/11-boot-process.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
