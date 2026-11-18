---
tags: [component, system]
---
# flsound

#component #system

flsound NAME login, notification, error, tick (volume step), screenshot flsound --force NAME even with sounds=0 (Settings' Test button) flsound --write NAME FILE.wav write the sound as 16 bit mono 48 kHz WAV, play nothing flsound --list

## Source files

- [[docs/graph/files/flsound/flsound.c|src/flsound/flsound.c]] -- 388 lines -- 13 fn

## Uses

- libraries: `libpipewire-0.3`

## Talks to

- none found

## Used by

- [[docs/graph/components/flcomp|flcomp]] (cli)
- [[docs/graph/components/flcrash|flcrash]] (cli)
- [[docs/graph/components/flpanel|flpanel]] (cli)

## Installed by

- [[docs/graph/packages/flsound|pkg/flsound]] (recipe `recipes/flsound/recipe`)

## Docs

- [[docs/manual/06-apps|06-apps]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/17-hardware-drivers|17-hardware-drivers]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/23-compositor-protocols|23-compositor-protocols]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/28-roadmap|28-roadmap]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `071cf4c` FreeLFS: import current tree (M10 + hardware fixes + XWayland, ISO 49c5df24)

See also: [src/flsound/README](../../../src/flsound/README) · [06-apps](../../manual/06-apps.md) · [10-architecture](../../manual/10-architecture.md) · [17-hardware-drivers](../../manual/17-hardware-drivers.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
