---
tags: [component, system]
---
# flpkg

#component #system

the FreeLFS package manager

## Source files

- [[docs/graph/files/flpkg/build.c|src/flpkg/build.c]] -- 629 lines -- 23 fn
- [[docs/graph/files/flpkg/cmd_pkg.c|src/flpkg/cmd_pkg.c]] -- 578 lines -- 18 fn
- [[docs/graph/files/flpkg/config.c|src/flpkg/config.c]] -- 145 lines -- 6 fn
- [[docs/graph/files/flpkg/db.c|src/flpkg/db.c]] -- 449 lines -- 31 fn
- [[docs/graph/files/flpkg/db.h|src/flpkg/db.h]] -- 54 lines
- [[docs/graph/files/flpkg/flpkg.h|src/flpkg/flpkg.h]] -- 292 lines
- [[docs/graph/files/flpkg/hash.c|src/flpkg/hash.c]] -- 89 lines -- 7 fn
- [[docs/graph/files/flpkg/main.c|src/flpkg/main.c]] -- 253 lines -- 7 fn
- [[docs/graph/files/flpkg/pack.c|src/flpkg/pack.c]] -- 256 lines -- 5 fn
- [[docs/graph/files/flpkg/pkgfile.c|src/flpkg/pkgfile.c]] -- 84 lines -- 4 fn
- [[docs/graph/files/flpkg/pkginfo.c|src/flpkg/pkginfo.c]] -- 366 lines -- 17 fn
- [[docs/graph/files/flpkg/repo.c|src/flpkg/repo.c]] -- 641 lines -- 24 fn
- [[docs/graph/files/flpkg/repo.h|src/flpkg/repo.h]] -- 52 lines
- [[docs/graph/files/flpkg/sign.c|src/flpkg/sign.c]] -- 424 lines -- 18 fn
- [[docs/graph/files/flpkg/sign.h|src/flpkg/sign.h]] -- 80 lines
- [[docs/graph/files/flpkg/snapshot.c|src/flpkg/snapshot.c]] -- 115 lines -- 4 fn
- [[docs/graph/files/flpkg/solve.c|src/flpkg/solve.c]] -- 190 lines -- 10 fn
- [[docs/graph/files/flpkg/spawn.c|src/flpkg/spawn.c]] -- 96 lines -- 5 fn
- [[docs/graph/files/flpkg/sysusers.c|src/flpkg/sysusers.c]] -- 112 lines -- 4 fn
- [[docs/graph/files/flpkg/tar.c|src/flpkg/tar.c]] -- 482 lines -- 25 fn
- [[docs/graph/files/flpkg/triggers.c|src/flpkg/triggers.c]] -- 109 lines -- 3 fn
- [[docs/graph/files/flpkg/trust.c|src/flpkg/trust.c]] -- 518 lines -- 23 fn
- [[docs/graph/files/flpkg/txn.c|src/flpkg/txn.c]] -- 1078 lines -- 48 fn
- [[docs/graph/files/flpkg/txn.h|src/flpkg/txn.h]] -- 45 lines
- [[docs/graph/files/flpkg/util.c|src/flpkg/util.c]] -- 355 lines -- 34 fn
- [[docs/graph/files/flpkg/version.c|src/flpkg/version.c]] -- 219 lines -- 11 fn
- [[docs/graph/files/flpkg/zst.c|src/flpkg/zst.c]] -- 123 lines -- 7 fn

## Uses

- libraries: `-lcrypto`, `-lsqlite3`, `-lzstd`

## Talks to

- none found

## Used by

- nothing in the repo

## Installed by

- [[docs/graph/packages/flpkg|pkg/flpkg]] (recipe `recipes/flpkg/recipe`)

## Docs

- [[docs/manual/01-introduction|01-introduction]]
- [[docs/manual/03-installing|03-installing]]
- [[docs/manual/06-apps|06-apps]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/11-boot-process|11-boot-process]]
- [[docs/manual/12-filesystem-layout|12-filesystem-layout]]
- [[docs/manual/13-packages-flpkg|13-packages-flpkg]]
- [[docs/manual/14-services-init|14-services-init]]
- [[docs/manual/15-users-security|15-users-security]]
- [[docs/manual/16-networking|16-networking]]
- [[docs/manual/18-performance-ram|18-performance-ram]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/20-building-from-source|20-building-from-source]]
- [[docs/manual/21-recipes|21-recipes]]
- [[docs/manual/24-writing-an-app|24-writing-an-app]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/26-contributing|26-contributing]]
- [[docs/manual/28-roadmap|28-roadmap]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `a38b8f5` Code/docs graph for Obsidian, per-package build jobs, release links
- `b469866` Round 22: flpkg one command for every source, signed online repo, fixes, new docs
- `1e1c899` Round 18: shutdown hardening, visible launch errors, honest tests, manual, website
- `ebb6a28` Round 16: laptop-test fixes, security, USB drives, persistence, desktop QoL
- `b0715e7` Round 13: compositor animations, Themes app (fltheme), light scheme

See also: [01-introduction](../../manual/01-introduction.md) · [03-installing](../../manual/03-installing.md) · [06-apps](../../manual/06-apps.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
