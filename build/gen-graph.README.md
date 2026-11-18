# gen-graph: the FreeLFS graph (Obsidian vault section and agent map)

`build/gen-graph.py` reads `src/`, `recipes/`, `docs/` and the git history and writes:

- `docs/graph/`: Obsidian notes with `[[wikilinks]]` and tags, plus plain `.md` "See also" links that also render on GitHub.
- `docs/graph/AGENT-MAP.md`: one compact file (about 350 lines) for AI agents: per component its purpose, key files, uses, used-by, talks-to, recipe and manual chapters, then a change-impact section.

It uses Python 3 stdlib only, builds nothing, runs no tests, and makes no commits.

## Run

From the repo root (any working directory also works):

    python3 -I build/gen-graph.py

Re-running is safe. The script reads `docs/graph/.generated`, deletes exactly the files listed there, removes directories it emptied, and writes the output again. If a file it is about to write already exists and is not in the manifest, it stops and changes nothing, so hand-written notes you add under `docs/graph/` are never overwritten. Output is deterministic (sorted, no timestamps). Only `INDEX.md` names a commit: the short hash of HEAD.

Output layout:

    docs/graph/INDEX.md          entry point: main docs, components, package groups, rounds
    docs/graph/AGENT-MAP.md      compact map for agents
    docs/graph/components/       one note per src/<comp> (tags #component + #app|#system|#library|#daemon)
    docs/graph/files/<comp>/     one note per .c/.h in src/<comp> (tag #source; #generated for protocol stubs)
    docs/graph/packages/         one note per recipes/<name>/recipe (tag #package)
    docs/graph/groups/           one note per package group (tag #package-group)
    docs/graph/rounds/           one note per "Round NN:" commit (tag #round)
    docs/graph/.generated        manifest of the files above (paths relative to docs/graph/)

## Open it in Obsidian

1. In Obsidian choose "Open folder as vault" and pick the repo folder (`FreeLFS/`), not `docs/graph/`. The wikilinks use full repo paths such as `[[docs/graph/components/flcomp|flcomp]]`, so they resolve from the repo root.
2. Press Ctrl+G for the graph view.
3. Colour groups by tag (Graph view settings, Groups, then add each query):
   - `tag:#component`: blue
   - `tag:#package`: green
   - `tag:#round`: orange
   - `tag:#app`: accent colour (the palette accent is 3f9b63)
4. Hide the roughly 280 source-file notes with the filter `-tag:#source` if the graph is too busy. Packages and components stay visible.

## What the heuristics do (and where they are wrong)

Everything is best effort. Known inaccuracies, all visible in the output:

- **Component descriptions** are the first paragraph of `src/<comp>/README` after skipping a title line with an underline. Where a README opens with usage text (flarchive) or the paragraph is cut at 240 characters, the description is poor. Fallback: the first `# name -- text` line of the Makefile.
- **Tags** (`#app`, `#system`, `#library`, `#daemon`) are rules, applied in this order: EFI loader (`efi_main` or `efi.h`) = system; no `main()` and no installed binary, or a `lib*` name, or `-shared` = library; `daemon(` or a `*.service` file = daemon; compositor (`wayland-server`), `mount`/`reboot`/`setuid`-style calls, or an `/sbin` install = system; a `.desktop` file or `flui`/`wayland-client` in PKGS = app; otherwise system. CLI tools such as flpkg, flsh and flsnap therefore show as `#system`.
- **Uses, pkg-config**: taken from `PKGS`, `GLPKGS`, `LIBS`, `LDLIBS`, `UI_LIBS`, `PKG_LIBS` and `pkg-config --libs/--cflags` names in the Makefile. Makefile variable indirection beyond those names is not expanded, so a few libraries may be missing.
- **Uses, components**: a component is used when one of its files `#include`s a header that lives in another component. Lookup tries the path relative to the file first, then the header's basename across `src/`. Headers vendored as copies (`flauth-check.h` in four components, `fl-shell-*-protocol.h` in several) resolve locally, so their origin is not reported. Where a basename exists in several components, every owner is linked. `flinstall` includes `wifi.h`, which it does not carry, so it links to `flsettings`.
- **Talks to, CLI**: string literals in the first 800 characters after an `exec*`, `system`, `popen` or `posix_spawn` call, if the first word names a binary installed by some component's Makefile. Binary-to-component comes from the `install ... bin/` or `sbin/` lines. The script does not follow variables, shell scripts (`recipe.sh`, `session`) or multi-hop helpers.
- **Talks to, D-Bus**: only a fixed set of bus-name prefixes (`org.freedesktop.portal`, `org.freedesktop.impl.portal`, `org.freedesktop.Notifications`, `org.freelfs`, `net.connman.iwd`, `org.bluez`, `org.mpris.MediaPlayer2`). It also lists interface names that appear as strings (for example `org.freedesktop.portal.Request`), which are not bus names. Names built at runtime from pieces are missed.
- **Talks to, Wayland**: a fixed interface pattern (`*_v<N>` for zxdg, zwp, zwlr, ext, wp and xwayland names, plus xdg core, `fl_shell`, `fl_screencopy_*`, and a few wl_ core names). Core interfaces are included, so many components show `wl_seat` and `wl_shm`.
- **Functions** come from a regex for column-0 C definitions with a `{` before any `;`. It misses definitions whose return type is split over lines and functions defined by macros, and a multi-line prototype can be mistaken for a definition. Treat the list as a hint, not a symbol table. Generated protocol stubs and `tests/` are excluded.
- **Installed by**: a package is primary when its name equals the component. Packages whose recipe copies `src/<comp>` (for example pkg flcomp copies flui) are listed as "also copied from". `fl-drawtest` is found this way for `drawtest`.
- **Docs**: a manual chapter is listed when the component name appears as a whole word in it. Common words could match falsely, and very short names may over-match.
- **Change impact**: a header's rebuild set follows reverse includes transitively, plus the components that carry generated copies of a protocol. Link-only consumers that do not `#include` the header are not added, except through `PKGS`. `flconf` is named in the brief but is not a file in this repo; the map says so and points at `flui` (`flpalette.h`, `conf.c`).
- **Round notes** list every file from `git show --numstat --no-renames`, grouped by component or package. A file inside a component's `tests/` directory is listed without a note.

## Checks

- Re-running produces identical output (`diff -r` after two runs: no difference).
- All relative `.md` links and `[[wikilinks]]` in `docs/graph/` resolve.
