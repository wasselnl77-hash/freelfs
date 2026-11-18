---
tags: [component, system]
---
# flsnap

#component #system

flsnap keeps read-only snapshots of @ (the root subvolume) in @snapshots, which FreeLFS mounts at /.snapshots (build/50-image.sh, config/etc/fstab).

## Source files

- [[docs/graph/files/flsnap/flauth-check.h|src/flsnap/flauth-check.h]] -- 248 lines -- 12 fn
- [[docs/graph/files/flsnap/flsnap.c|src/flsnap/flsnap.c]] -- 929 lines -- 38 fn

## Uses

- libraries: none

## Talks to

- none found

## Used by

- nothing in the repo

## Installed by

- [[docs/graph/packages/flsnap|pkg/flsnap]] (recipe `recipes/flsnap/recipe`)

## Docs

- [[docs/manual/01-introduction|01-introduction]]
- [[docs/manual/06-apps|06-apps]]
- [[docs/manual/08-settings-reference|08-settings-reference]]
- [[docs/manual/09-troubleshooting|09-troubleshooting]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/11-boot-process|11-boot-process]]
- [[docs/manual/12-filesystem-layout|12-filesystem-layout]]
- [[docs/manual/13-packages-flpkg|13-packages-flpkg]]
- [[docs/manual/15-users-security|15-users-security]]
- [[docs/manual/18-performance-ram|18-performance-ram]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/28-roadmap|28-roadmap]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `1e1c899` Round 18: shutdown hardening, visible launch errors, honest tests, manual, website
- `ebb6a28` Round 16: laptop-test fixes, security, USB drives, persistence, desktop QoL
- `6a1e3de` Round 12: RAM trim, live store, desktop icons, small fixes, security ops
- `071cf4c` FreeLFS: import current tree (M10 + hardware fixes + XWayland, ISO 49c5df24)

See also: [src/flsnap/README](../../../src/flsnap/README) · [01-introduction](../../manual/01-introduction.md) · [06-apps](../../manual/06-apps.md) · [08-settings-reference](../../manual/08-settings-reference.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
