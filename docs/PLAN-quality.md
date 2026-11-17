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

## Software availability (plan, 2026-11-16)
1. Host the signed flpkg repo online (GitHub Releases/Pages), with updates via `flpkg upgrade --sync` and the Store.
2. `flpkg install NAME`: search the FreeLFS repo first, then Flathub (install through Flatpak), with one password prompt (flauth).
3. Optional Nix: a package `nix` that sets up /nix with a nix-daemon started on demand (no idle RAM). The Store can search nixpkgs. GUI apps may need nixGL.
4. Optional Distrobox: podman (daemonless, rootless, needs user namespaces and cgroups in the kernel) plus distrobox. Ubuntu/Arch/Fedora boxes with `distrobox-export` to put apps in the start menu.
5. Haiku agents write recipes for the 50 most-wanted tools.
6. **One command for everything** (the user's requirement: "we can't have 4 different commands"). flpkg is the single front end:
   - `flpkg install NAME` searches in order: FreeLFS repo → Flathub → Nix → Distrobox (apt/pacman/dnf).
   - If several sources have it, it shows the choices ("1) FreeLFS 2) Flathub 3) Nix 4) Ubuntu (apt)") with the FreeLFS one preselected. `--from=flatpak|nix|apt|pacman|dnf` skips the question.
   - `flpkg search`, `flpkg remove`, `flpkg upgrade` and `flpkg list` work across all sources (upgrade runs each backend's update).
   - Distrobox containers are created on first use (asking first), and installed apps are exported to the start menu automatically.
   - The Store uses the same backends, so GUI and CLI show the same thing.
   - flpkg records which source installed what, so `remove` uses the right backend.
