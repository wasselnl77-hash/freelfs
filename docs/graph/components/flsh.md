---
tags: [component, system]
---
# flsh

#component #system

flsh is the interactive shell of FreeLFS, written from scratch in C. It runs POSIX shell syntax, so commands from tutorials and ~/.profile work, and adds comforts for typing:

## Source files

- [[docs/graph/files/flsh/arith.c|src/flsh/arith.c]] -- 362 lines -- 19 fn
- [[docs/graph/files/flsh/builtin.c|src/flsh/builtin.c]] -- 1593 lines -- 56 fn
- [[docs/graph/files/flsh/complete.c|src/flsh/complete.c]] -- 387 lines -- 10 fn
- [[docs/graph/files/flsh/edit.c|src/flsh/edit.c]] -- 1006 lines -- 42 fn
- [[docs/graph/files/flsh/exec.c|src/flsh/exec.c]] -- 1397 lines -- 58 fn
- [[docs/graph/files/flsh/expand.c|src/flsh/expand.c]] -- 1008 lines -- 33 fn
- [[docs/graph/files/flsh/flsh.h|src/flsh/flsh.h]] -- 462 lines
- [[docs/graph/files/flsh/guard.c|src/flsh/guard.c]] -- 69 lines -- 1 fn
- [[docs/graph/files/flsh/highlight.c|src/flsh/highlight.c]] -- 435 lines -- 16 fn
- [[docs/graph/files/flsh/hist.c|src/flsh/hist.c]] -- 211 lines -- 11 fn
- [[docs/graph/files/flsh/job.c|src/flsh/job.c]] -- 540 lines -- 28 fn
- [[docs/graph/files/flsh/main.c|src/flsh/main.c]] -- 324 lines -- 9 fn
- [[docs/graph/files/flsh/parse.c|src/flsh/parse.c]] -- 1116 lines -- 43 fn
- [[docs/graph/files/flsh/prompt.c|src/flsh/prompt.c]] -- 271 lines -- 10 fn
- [[docs/graph/files/flsh/test.c|src/flsh/test.c]] -- 244 lines -- 13 fn
- [[docs/graph/files/flsh/util.c|src/flsh/util.c]] -- 315 lines -- 30 fn
- [[docs/graph/files/flsh/var.c|src/flsh/var.c]] -- 293 lines -- 22 fn

## Uses

- libraries: none

## Talks to

- CLI [[docs/graph/components/flcrash|flcrash]]: `flcrash`

## Used by

- [[docs/graph/components/flterm|flterm]] (cli)

## Installed by

- [[docs/graph/packages/flsh|pkg/flsh]] (recipe `recipes/flsh/recipe`)

## Docs

- [[docs/manual/01-introduction|01-introduction]]
- [[docs/manual/02-getting-started|02-getting-started]]
- [[docs/manual/05-keyboard-shortcuts|05-keyboard-shortcuts]]
- [[docs/manual/06-apps|06-apps]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/12-filesystem-layout|12-filesystem-layout]]
- [[docs/manual/15-users-security|15-users-security]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/26-contributing|26-contributing]]
- [[docs/manual/28-roadmap|28-roadmap]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `071cf4c` FreeLFS: import current tree (M10 + hardware fixes + XWayland, ISO 49c5df24)

See also: [src/flsh/README](../../../src/flsh/README) · [01-introduction](../../manual/01-introduction.md) · [02-getting-started](../../manual/02-getting-started.md) · [05-keyboard-shortcuts](../../manual/05-keyboard-shortcuts.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
