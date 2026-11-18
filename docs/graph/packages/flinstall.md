---
tags: [package]
---
# pkg/flinstall

#package

Version 0.1, release 7. FreeLFS installer: language, keyboard, Wi-Fi, disk, account, time zone, install

- License: MIT; URL: none
- Groups: [[docs/graph/groups/desktop|desktop]]
- Component: [[docs/graph/components/flinstall|flinstall]]
- Depends: [[docs/graph/packages/flauth|pkg/flauth]], [[docs/graph/packages/glibc|pkg/glibc]], [[docs/graph/packages/flui|pkg/flui]], [[docs/graph/packages/wayland|pkg/wayland]], [[docs/graph/packages/dbus|pkg/dbus]], [[docs/graph/packages/libxcrypt|pkg/libxcrypt]], [[docs/graph/packages/util-linux|pkg/util-linux]], [[docs/graph/packages/btrfs-progs|pkg/btrfs-progs]], [[docs/graph/packages/dosfstools|pkg/dosfstools]], [[docs/graph/packages/mtools|pkg/mtools]], [[docs/graph/packages/e2fsprogs|pkg/e2fsprogs]], [[docs/graph/packages/flboot|pkg/flboot]], [[docs/graph/packages/flpanel|pkg/flpanel]]
- Makedepends: [[docs/graph/packages/flui|pkg/flui]], [[docs/graph/packages/dbus|pkg/dbus]], [[docs/graph/packages/libxcrypt|pkg/libxcrypt]]

Recipe: `recipes/flinstall/recipe`

See also: [recipes/flinstall/recipe](../../../recipes/flinstall/recipe) · [flinstall component](../components/flinstall.md) · [INDEX](../INDEX.md)
