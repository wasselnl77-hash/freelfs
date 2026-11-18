---
tags: [component, app]
---
# flauth

#component #app

the password prompt for root actions (setuid helper + flui dialog)

## Source files

- [[docs/graph/files/flauth/flauth-check.h|src/flauth/flauth-check.h]] -- 39 lines -- 1 fn
- [[docs/graph/files/flauth/flauth-dialog.c|src/flauth/flauth-dialog.c]] -- 146 lines -- 4 fn
- [[docs/graph/files/flauth/flauth.c|src/flauth/flauth.c]] -- 272 lines -- 9 fn

## Uses

- component [[docs/graph/components/flui|flui]] (hdr:flui.h, pkg)
- libraries: `xkbcommon`

## Talks to

- none found

## Used by

- [[docs/graph/components/flpackages|flpackages]] (cli)
- [[docs/graph/components/flsnap|flsnap]] (cli)
- [[docs/graph/components/flsplash|flsplash]] (cli)

## Installed by

- [[docs/graph/packages/flauth|pkg/flauth]] (recipe `recipes/flauth/recipe`)

## Docs

- [[docs/manual/06-apps|06-apps]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/12-filesystem-layout|12-filesystem-layout]]
- [[docs/manual/15-users-security|15-users-security]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/28-roadmap|28-roadmap]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `ebb6a28` Round 16: laptop-test fixes, security, USB drives, persistence, desktop QoL

See also: [06-apps](../../manual/06-apps.md) · [10-architecture](../../manual/10-architecture.md) · [12-filesystem-layout](../../manual/12-filesystem-layout.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
