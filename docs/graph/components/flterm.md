---
tags: [component, app]
---
# flterm

#component #app

flterm [-e COMMAND [ARGS...]] runs flsh, or the command

## Source files

- [[docs/graph/files/flterm/flterm.h|src/flterm/flterm.h]] -- 75 lines
- [[docs/graph/files/flterm/main.c|src/flterm/main.c]] -- 603 lines -- 17 fn
- [[docs/graph/files/flterm/term.c|src/flterm/term.c]] -- 916 lines -- 36 fn

## Uses

- component [[docs/graph/components/flui|flui]] (hdr:flui.h, pkg)
- libraries: none

## Talks to

- CLI [[docs/graph/components/flsh|flsh]]: `flsh`

## Used by

- [[docs/graph/components/flfiles|flfiles]] (cli)
- [[docs/graph/components/flpanel|flpanel]] (cli)

## Installed by

- [[docs/graph/packages/flterm|pkg/flterm]] (recipe `recipes/flterm/recipe`)

## Docs

- [[docs/manual/01-introduction|01-introduction]]
- [[docs/manual/05-keyboard-shortcuts|05-keyboard-shortcuts]]
- [[docs/manual/06-apps|06-apps]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/22-flui-toolkit|22-flui-toolkit]]
- [[docs/manual/23-compositor-protocols|23-compositor-protocols]]
- [[docs/manual/24-writing-an-app|24-writing-an-app]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `d3131d5` Round 19: eject/cwd fix, app icons, server-side decorations, stride fix
- `2c2183d` Round 14: security updates, kernel 7.3.4 (rolling), WebKit sandbox; Intel/AMD prep
- `071cf4c` FreeLFS: import current tree (M10 + hardware fixes + XWayland, ISO 49c5df24)

See also: [src/flterm/README](../../../src/flterm/README) · [01-introduction](../../manual/01-introduction.md) · [05-keyboard-shortcuts](../../manual/05-keyboard-shortcuts.md) · [06-apps](../../manual/06-apps.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
