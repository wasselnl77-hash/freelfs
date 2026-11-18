---
tags: [component, system]
---
# flboot

#component #system

flboot lives on the EFI system partition (ESP), which FreeLFS mounts at /boot, as EFI/BOOT/BOOTX64.EFI (BOOTAA64.EFI on aarch64): the file UEFI firmware starts by default, so no boot variables are needed. `flboot-install` puts it there...

## Source files

- [[docs/graph/files/flboot/boot.c|src/flboot/boot.c]] -- 37 lines -- 1 fn
- [[docs/graph/files/flboot/conf.c|src/flboot/conf.c]] -- 266 lines -- 14 fn
- [[docs/graph/files/flboot/efi.h|src/flboot/efi.h]] -- 265 lines
- [[docs/graph/files/flboot/flboot.h|src/flboot/flboot.h]] -- 85 lines
- [[docs/graph/files/flboot/fs.c|src/flboot/fs.c]] -- 119 lines -- 5 fn
- [[docs/graph/files/flboot/gfx.c|src/flboot/gfx.c]] -- 94 lines -- 8 fn
- [[docs/graph/files/flboot/lib.c|src/flboot/lib.c]] -- 192 lines -- 18 fn
- [[docs/graph/files/flboot/main.c|src/flboot/main.c]] -- 36 lines -- 1 fn
- [[docs/graph/files/flboot/menu.c|src/flboot/menu.c]] -- 313 lines -- 13 fn
- [[docs/graph/files/flboot/other.c|src/flboot/other.c]] -- 196 lines -- 8 fn
- [[docs/graph/files/flboot/start.c|src/flboot/start.c]] -- 48 lines -- 2 fn

## Uses

- libraries: none

## Talks to

- none found

## Used by

- nothing in the repo

## Installed by

- [[docs/graph/packages/flboot|pkg/flboot]] (recipe `recipes/flboot/recipe`)

## Docs

- [[docs/manual/01-introduction|01-introduction]]
- [[docs/manual/02-getting-started|02-getting-started]]
- [[docs/manual/03-installing|03-installing]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/11-boot-process|11-boot-process]]
- [[docs/manual/12-filesystem-layout|12-filesystem-layout]]
- [[docs/manual/13-packages-flpkg|13-packages-flpkg]]
- [[docs/manual/15-users-security|15-users-security]]
- [[docs/manual/17-hardware-drivers|17-hardware-drivers]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/20-building-from-source|20-building-from-source]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/28-roadmap|28-roadmap]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `e62d37c` Round 21: boot stick ownership, Load into RAM, FreeBWS GPU freeze fix, flboot entries
- `27f10f9` Round 20: Wi-Fi drivers, NVIDIA phase 1, flat splash, FreeBWS watchdogs, fixes
- `2c2183d` Round 14: security updates, kernel 7.3.4 (rolling), WebKit sandbox; Intel/AMD prep
- `071cf4c` FreeLFS: import current tree (M10 + hardware fixes + XWayland, ISO 49c5df24)

See also: [src/flboot/README](../../../src/flboot/README) · [01-introduction](../../manual/01-introduction.md) · [02-getting-started](../../manual/02-getting-started.md) · [03-installing](../../manual/03-installing.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
