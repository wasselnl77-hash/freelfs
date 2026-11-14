# Code quality plan

From an outside review (DeepSeek) on 2026-11-14. The wl_shm stride bug it found was confirmed and is being fixed in round 19.

- **Fuzzing:** add libFuzzer targets for the parsers:
  - the flsh grammar
  - libdssl CSS
  - flpkg's tar and manifest reader
  - Wayland wire input to flcomp (shm and dmabuf buffers)
  - flview image decoders
  - the .desktop and desktop.conf parsers
- **CI:** GitHub Actions to build host-side, run the unit and differential tests, ASan/UBSan, and short fuzz runs.
- **Allocations:** 121 of 362 raw allocations are unchecked. Use one xmalloc-style wrapper, starting in flcomp.
- **snprintf truncation:** check return values on path building, where PATH_MAX is the real risk.
- **Large files:** split flfiles.c (about 3.9k lines), fledit.c (about 3.3k) and freebws.c (about 3.2k) into modules before inviting contributors.
- **Comments:** comment density is only 4%. Add "why" comments in the core parts: flcomp, flpkg, flsh.
