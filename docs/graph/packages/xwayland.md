---
tags: [package]
---
# pkg/xwayland

#package

Version 24.1.15, release 1. X11 server on Wayland (rootless, started by flcomp on the first X connection)

- License: MIT; URL: https://www.x.org/wiki/Xwayland/
- Groups: [[docs/graph/groups/desktop|desktop]]
- Component: [[docs/graph/components/flcomp|flcomp]]
- Depends: [[docs/graph/packages/glibc|pkg/glibc]], [[docs/graph/packages/wayland|pkg/wayland]], [[docs/graph/packages/libdrm|pkg/libdrm]], [[docs/graph/packages/libepoxy|pkg/libepoxy]], [[docs/graph/packages/libxshmfence|pkg/libxshmfence]], [[docs/graph/packages/libxcvt|pkg/libxcvt]], [[docs/graph/packages/libXfont2|pkg/libXfont2]], [[docs/graph/packages/libxkbfile|pkg/libxkbfile]], [[docs/graph/packages/pixman|pkg/pixman]], [[docs/graph/packages/openssl|pkg/openssl]], [[docs/graph/packages/xkbcomp|pkg/xkbcomp]], [[docs/graph/packages/xkeyboard-config|pkg/xkeyboard-config]], [[docs/graph/packages/mesa|pkg/mesa]], [[docs/graph/packages/libXau|pkg/libXau]], [[docs/graph/packages/libXdmcp|pkg/libXdmcp]]
- Makedepends: [[docs/graph/packages/meson|pkg/meson]], [[docs/graph/packages/ninja|pkg/ninja]], [[docs/graph/packages/pkgconf|pkg/pkgconf]], [[docs/graph/packages/xorgproto|pkg/xorgproto]], [[docs/graph/packages/xtrans|pkg/xtrans]], [[docs/graph/packages/libX11|pkg/libX11]], [[docs/graph/packages/wayland-protocols|pkg/wayland-protocols]], [[docs/graph/packages/mesa|pkg/mesa]]

Recipe: `recipes/xwayland/recipe`

See also: [recipes/xwayland/recipe](../../../recipes/xwayland/recipe) · [flcomp component](../components/flcomp.md) · [INDEX](../INDEX.md)
