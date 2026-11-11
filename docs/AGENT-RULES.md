# Rules for every agent working on FreeLFS

Read this fully before starting. Plan: docs/ROADMAP-NEXT.md. Look: docs/PLAN.md / existing apps.

## Hard rules (never break)
- Never use sudo, never enter or ask for passwords. Everything is rootless.
- Never read or search ~/Dokumente/tokens.
- Never write into the host's real ~/.config, ~/.local or $HOME dotfiles. Point
  XDG_CONFIG_HOME / XDG_DATA_HOME / HOME at a scratch dir for any test run.
- Never write to a block device, never touch /run/media.
- Do not kill processes you did not start (esp. ~/.local/bin/sleep-mode).
- Several agents work in parallel. Only edit the files/dirs your brief gives you.
  If you truly need a change elsewhere, keep it minimal and additive and list it
  in your report.
- **Only the agent whose brief says "rootfs owner" may run build/60-packages.sh,
  50-image.sh, 53-iso.sh or QEMU tests.** Everyone else compiles host-side:
  `gcc --sysroot=$HOME/freelfs-work/rootfs-x86_64 ...` and runs binaries via the
  rootfs ld.so (see how src/*/Makefile `check` targets do it), screenshots via
  FLUI_PPM / FLCOMP_HEADLESS + FLCOMP_SCREENSHOT.
- Long commands go through build/gentle.sh or `nice -n 19 ionice -c3`. Host has
  limited RAM: never more than 4 compile jobs.

## Look
- Palette: bg 1d2125, panel 272c33, raised 343a43, line 3a4048, text d8dce0,
  dim 8a9199, accent 3f9b63 (or the user accent once A4 exists). IBM Plex Sans /
  Mono. Sharp corners, flat, no gradients, no shadows-everywhere, no emoji, no
  "AI slop" (no purple, no glassmorphism, no rounded pill cards).
- Low end: 1 GB RAM target, apps idle under ~25 MB RSS where possible.

## Delivery
- Each app: src/<name>/ with Makefile (`all`, `check`, `install DESTDIR=`),
  recipes/<name> following existing recipes, a .desktop file, headless
  screenshots in docs/m7/<name>-*.png that you have LOOKED at.
- Update docs/ROADMAP-NEXT.md status lines for your items (append, don't rewrite
  others' lines).
- Final report: short. What works, what's verified how, files touched outside
  your area, what is untested. No code dumps.

## Git
- The tree is a git repo now (branch main). Do NOT commit, push, branch or
  rewrite history; the coordinator reviews and commits. `git diff` / `git status`
  are fine and useful to check your own changes.
