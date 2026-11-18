---
tags: [component, daemon]
---
# flportal

#component #daemon

Flatpak apps reach the desktop through xdg-desktop-portal (the "frontend", D-Bus activated in the session). The frontend does the sandbox bookkeeping and asks a backend for what the desktop shows or knows. flportal is that backend...

## Source files

- [[docs/graph/files/flportal/appchooser.c|src/flportal/appchooser.c]] -- 416 lines -- 15 fn
- [[docs/graph/files/flportal/filechooser.c|src/flportal/filechooser.c]] -- 1598 lines -- 46 fn
- [[docs/graph/files/flportal/flportal.h|src/flportal/flportal.h]] -- 66 lines
- [[docs/graph/files/flportal/main.c|src/flportal/main.c]] -- 1087 lines -- 44 fn
- [[docs/graph/files/flportal/ui.c|src/flportal/ui.c]] -- 101 lines -- 6 fn

## Uses

- component [[docs/graph/components/flui|flui]] (hdr:flui.h, pkg)
- libraries: `gio-2.0`, `gio-unix-2.0`

## Talks to

- D-Bus: `org.freedesktop.Notifications`, `org.freedesktop.appearance`, `org.freedesktop.impl.portal`, `org.freedesktop.impl.portal.FileChooser`, `org.freedesktop.impl.portal.Notification`, `org.freedesktop.impl.portal.Screenshot`, `org.freedesktop.impl.portal.Settings`, `org.freedesktop.impl.portal.desktop.freelfs`, `org.freedesktop.portal.Desktop`, `org.freedesktop.portal.Error.NotFound`

## Used by

- nothing in the repo

## Installed by

- [[docs/graph/packages/flportal|pkg/flportal]] (recipe `recipes/flportal/recipe`)

## Docs

- [[docs/manual/06-apps|06-apps]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/13-packages-flpkg|13-packages-flpkg]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/27-freebws-internals|27-freebws-internals]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `b469866` Round 22: flpkg one command for every source, signed online repo, fixes, new docs
- `e62d37c` Round 21: boot stick ownership, Load into RAM, FreeBWS GPU freeze fix, flboot entries
- `ebb6a28` Round 16: laptop-test fixes, security, USB drives, persistence, desktop QoL
- `071cf4c` FreeLFS: import current tree (M10 + hardware fixes + XWayland, ISO 49c5df24)

See also: [src/flportal/README](../../../src/flportal/README) · [06-apps](../../manual/06-apps.md) · [10-architecture](../../manual/10-architecture.md) · [13-packages-flpkg](../../manual/13-packages-flpkg.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
