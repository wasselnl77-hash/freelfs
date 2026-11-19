---
tags: [component, app]
---
# flstore

#component #app

One window: Explore (home with featured apps, recently updated apps, eight categories, search, FreeLFS packages), Installed and Updates. It replaces the Packages app (src/flpackages), which stays only as the provider of flpkg-seat and...

## Source files

- [[docs/graph/files/flstore/flstore.c|src/flstore/flstore.c]] -- 2384 lines -- 103 fn
- [[docs/graph/files/flstore/flstore.h|src/flstore/flstore.h]] -- 222 lines
- [[docs/graph/files/flstore/img.c|src/flstore/img.c]] -- 312 lines -- 9 fn
- [[docs/graph/files/flstore/index.c|src/flstore/index.c]] -- 690 lines -- 22 fn
- [[docs/graph/files/flstore/store.h|src/flstore/store.h]] -- 67 lines
- [[docs/graph/files/flstore/views.c|src/flstore/views.c]] -- 1128 lines -- 36 fn

## Uses

- component [[docs/graph/components/flui|flui]] (hdr:flui.h, pkg)
- libraries: `libjpeg`, `libpng`, `libwebp`, `libxml-2.0`, `zlib`

## Talks to

- CLI [[docs/graph/components/flinstall|flinstall]]: `flinstall`

## Used by

- nothing in the repo

## Installed by

- [[docs/graph/packages/flstore|pkg/flstore]] (recipe `recipes/flstore/recipe`)

## Docs

- [[docs/manual/06-apps|06-apps]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/13-packages-flpkg|13-packages-flpkg]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/28-roadmap|28-roadmap]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `b469866` Round 22: flpkg one command for every source, signed online repo, fixes, new docs
- `d3131d5` Round 19: eject/cwd fix, app icons, server-side decorations, stride fix
- `6a1e3de` Round 12: RAM trim, live store, desktop icons, small fixes, security ops
- `071cf4c` FreeLFS: import current tree (M10 + hardware fixes + XWayland, ISO 49c5df24)

See also: [src/flstore/README](../../../src/flstore/README) · [06-apps](../../manual/06-apps.md) · [10-architecture](../../manual/10-architecture.md) · [13-packages-flpkg](../../manual/13-packages-flpkg.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
