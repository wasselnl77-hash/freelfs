---
tags: [component, app]
---
# freebws

#component #app

WPE WebKit (wpewebkit, libwpe, wpebackend-fdo) in a libflui window: a tab row, a toolbar with back, forward, reload and an address bar that also searches (Brave Search by default), star, ad blocker shield, downloads and menu buttons, a...

## Source files

- [[docs/graph/files/freebws/abp.c|src/freebws/abp.c]] -- 974 lines -- 31 fn
- [[docs/graph/files/freebws/abp.h|src/freebws/abp.h]] -- 49 lines
- [[docs/graph/files/freebws/abpconv.c|src/freebws/abpconv.c]] -- 63 lines -- 1 fn
- [[docs/graph/files/freebws/adblock.c|src/freebws/adblock.c]] -- 1192 lines -- 53 fn
- [[docs/graph/files/freebws/adblock.h|src/freebws/adblock.h]] -- 51 lines
- [[docs/graph/files/freebws/bookmarks.c|src/freebws/bookmarks.c]] -- 430 lines -- 23 fn
- [[docs/graph/files/freebws/bookmarks.h|src/freebws/bookmarks.h]] -- 37 lines
- [[docs/graph/files/freebws/config.c|src/freebws/config.c]] -- 150 lines -- 15 fn
- [[docs/graph/files/freebws/config.h|src/freebws/config.h]] -- 30 lines
- [[docs/graph/files/freebws/downloads.c|src/freebws/downloads.c]] -- 457 lines -- 30 fn
- [[docs/graph/files/freebws/downloads.h|src/freebws/downloads.h]] -- 42 lines
- [[docs/graph/files/freebws/freebws.c|src/freebws/freebws.c]] -- 3342 lines -- 139 fn
- [[docs/graph/files/freebws/pages.c|src/freebws/pages.c]] -- 458 lines -- 16 fn
- [[docs/graph/files/freebws/pages.h|src/freebws/pages.h]] -- 44 lines
- [[docs/graph/files/freebws/popup.c|src/freebws/popup.c]] -- 68 lines -- 7 fn
- [[docs/graph/files/freebws/popup.h|src/freebws/popup.h]] -- 24 lines
- [[docs/graph/files/freebws/shm.c|src/freebws/shm.c]] -- 19 lines -- 2 fn
- [[docs/graph/files/freebws/tabstrip.c|src/freebws/tabstrip.c]] -- 111 lines -- 2 fn
- [[docs/graph/files/freebws/tabstrip.h|src/freebws/tabstrip.h]] -- 29 lines

## Uses

- component [[docs/graph/components/flcomp|flcomp]] (hdr:linux-dmabuf-unstable-v1-client-protocol.h)
- component [[docs/graph/components/flui|flui]] (hdr:flui.h, pkg)
- component [[docs/graph/components/libdssl|libdssl]] (hdr:dssl.h)
- libraries: `dssl`, `egl`, `gbm`, `glib-2.0`, `libsoup-3.0`, `wayland-client`, `wpe-webkit-2.0`, `wpebackend-fdo-1.0`

## Talks to

- Wayland: `wl_compositor`, `wl_seat`, `wl_shm`, `wl_subcompositor`, `zwp_linux_buffer_params_v1`, `zwp_linux_dmabuf_v1`

## Used by

- nothing in the repo

## Installed by

- [[docs/graph/packages/freebws|pkg/freebws]] (recipe `recipes/freebws/recipe`)

## Docs

- [[docs/manual/05-keyboard-shortcuts|05-keyboard-shortcuts]]
- [[docs/manual/06-apps|06-apps]]
- [[docs/manual/08-settings-reference|08-settings-reference]]
- [[docs/manual/09-troubleshooting|09-troubleshooting]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/15-users-security|15-users-security]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/21-recipes|21-recipes]]
- [[docs/manual/22-flui-toolkit|22-flui-toolkit]]
- [[docs/manual/23-compositor-protocols|23-compositor-protocols]]
- [[docs/manual/24-writing-an-app|24-writing-an-app]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/26-contributing|26-contributing]]
- [[docs/manual/27-freebws-internals|27-freebws-internals]]
- [[docs/manual/28-roadmap|28-roadmap]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `e62d37c` Round 21: boot stick ownership, Load into RAM, FreeBWS GPU freeze fix, flboot entries
- `27f10f9` Round 20: Wi-Fi drivers, NVIDIA phase 1, flat splash, FreeBWS watchdogs, fixes
- `d3131d5` Round 19: eject/cwd fix, app icons, server-side decorations, stride fix
- `1e1c899` Round 18: shutdown hardening, visible launch errors, honest tests, manual, website
- `ebb6a28` Round 16: laptop-test fixes, security, USB drives, persistence, desktop QoL

See also: [src/freebws/README](../../../src/freebws/README) · [05-keyboard-shortcuts](../../manual/05-keyboard-shortcuts.md) · [06-apps](../../manual/06-apps.md) · [08-settings-reference](../../manual/08-settings-reference.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
