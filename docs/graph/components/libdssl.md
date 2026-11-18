---
tags: [component, library]
---
# libdssl

#component #library

DSSL (huggingface.co/EHS-Studio/dssl, MIT) is a style language, an alternative to CSS. Today it runs as a JavaScript engine that writes inline styles. Version 0.16.3 added native/spec: a written specification for implementations that do...

## Source files

- [[docs/graph/files/libdssl/dssl.h|src/libdssl/dssl.h]] -- 120 lines
- [[docs/graph/files/libdssl/emit_css.c|src/libdssl/emit_css.c]] -- 250 lines -- 7 fn
- [[docs/graph/files/libdssl/emit_json.c|src/libdssl/emit_json.c]] -- 328 lines -- 16 fn
- [[docs/graph/files/libdssl/gradient.c|src/libdssl/gradient.c]] -- 684 lines -- 20 fn
- [[docs/graph/files/libdssl/internal.h|src/libdssl/internal.h]] -- 273 lines -- 15 fn
- [[docs/graph/files/libdssl/main.c|src/libdssl/main.c]] -- 120 lines -- 3 fn
- [[docs/graph/files/libdssl/parse.c|src/libdssl/parse.c]] -- 627 lines -- 23 fn
- [[docs/graph/files/libdssl/props.c|src/libdssl/props.c]] -- 1241 lines -- 63 fn
- [[docs/graph/files/libdssl/resolve.c|src/libdssl/resolve.c]] -- 1769 lines -- 74 fn
- [[docs/graph/files/libdssl/util.c|src/libdssl/util.c]] -- 515 lines -- 41 fn
- [[docs/graph/files/libdssl/value.c|src/libdssl/value.c]] -- 366 lines -- 14 fn
- [[docs/graph/files/libdssl/vocab.c|src/libdssl/vocab.c]] -- 2421 lines -- 1 fn
- [[docs/graph/files/libdssl/vocab.h|src/libdssl/vocab.h]] -- 27 lines

## Uses

- libraries: none

## Talks to

- none found

## Used by

- [[docs/graph/components/freebws|freebws]] (hdr)

## Installed by

- [[docs/graph/packages/libdssl|pkg/libdssl]] (recipe `recipes/libdssl/recipe`)

## Docs

- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/26-contributing|26-contributing]]
- [[docs/manual/27-freebws-internals|27-freebws-internals]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `071cf4c` FreeLFS: import current tree (M10 + hardware fixes + XWayland, ISO 49c5df24)

See also: [src/libdssl/README](../../../src/libdssl/README) · [10-architecture](../../manual/10-architecture.md) · [19-admin-reference](../../manual/19-admin-reference.md) · [25-testing](../../manual/25-testing.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
