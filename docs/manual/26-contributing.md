# 26. Contributing to FreeLFS

Summary: how to report a problem, apply to contribute, get and build the code, follow the C and shell style of the tree,
write commit messages in the project's style, and what licence applies. FreeBWS is a community project with help wanted.

## Contents

1. Ways to take part
2. Reporting a problem (issues and `flreport`)
3. Applying as a contributor
4. Getting the code
5. Before you send a change
6. Code style: C
7. Code style: shell, Python and recipes
8. Commit messages
9. Licence and third-party code
10. Security reports
11. FreeBWS: a community project (help wanted)
12. Translations (planned)

---

## 1. Ways to take part

FreeLFS is pre-alpha (`README.md`): things break, and the tree changes without notice. Useful help, roughly from the easiest:

| What | How |
|---|---|
| Test on a real computer | Run the live ISO on a spare machine (never a disk you need) and report what happens. Hardware that has never been tried is the most valuable (`docs/HARDWARE.md`, "Wanted testers"). Intel laptops and AMD laptops with other chips are the gaps. |
| Report a bug | Open an issue with `flreport` attached (section 2). |
| Apply as a contributor | Open an issue titled "Contributor application" (section 3). |
| Fix a bug | Pick an issue, build (chapter 20), change the code, test (chapter 25), and send the change (section 8). |
| Write documentation | The manual is in `docs/manual/` (this chapter is one of its files). |
| Translate | Planned, not started: 12 languages (section 12, chapter 28). |
| Help with FreeBWS | A community project that stays in the main repository. See section 11. |

The public repository and issue tracker are at `github.com/wasselnl77-hash/freelfs` (the links in the project's web pages in
`site/`). The web pages are `site/index.html`, `site/download.html`, `site/testers.html` and `site/docs.html`.

## 2. Reporting a problem (issues and `flreport`)

The tester page (`site/testers.html`) gives the steps:

1. Open the Terminal and run `flreport`. It collects a report of the machine for bug hunting (`src/flreport`) and writes a
   tarball. Attach that file to the issue.
2. Open an issue at `github.com/wasselnl77-hash/freelfs/issues/new`. A GitHub account is needed.
3. Include: the hardware (model, graphics chip, Wi-Fi chip, memory), what you did, what happened, what you expected, and
   whether the same happens in QEMU (if you can tell). The page has a report template with the fields "flreport file
   attached: yes / no".

Useful facts for a report:

- A problem in a program that you started from the menu is easier to find with the program's own message. Programs show
  their errors in their window (chapter 24, section 9). Quote the text.
- The desktop's logs: `$XDG_RUNTIME_DIR/flcomp.log` (the compositor), `session.log` (the session services) and
  `xwayland.log` (X11 programs). Quote them if they show an error (whether `flreport` collects them is not verified).
- Say which image or ISO you used and when it was built. Some round notes in `docs/INTEGRATION-TODO.md` record the ISO's SHA256.

A report that says "it does not work" cannot be acted on. One that says "clicking Shut down on the login screen once, then again,
does nothing; flreport attached" can.

## 3. Applying as a contributor

The download page and the testers page link to a prefilled issue: `github.com/wasselnl77-hash/freelfs/issues/new?title=Contributor%20application`.
Use it to say what you would like to work on: code, testing, translations or documentation. Tell the project which of these you
can do, and your experience (languages, tools). The issue is the application; there is no separate form in the repository.

The repository does not describe the review of applications or the steps after one (not verified).

## 4. Getting the code

```sh
git clone https://github.com/wasselnl77-hash/freelfs.git
cd freelfs
git switch main            # the development branch is main
```

The tree is a git repository. Build it as chapter 20 describes, without root. The first build takes hours (chapter 20, section 9).
Build one program at a time while you work on it, with its own Makefile (`cd src/flview && make && make check`). Read the
program's `README` first.

Do not commit build output. `.gitignore` lists the object files, binaries, Python caches and PPM pictures. Check `git status`
before you commit.

## 5. Before you send a change

- It builds without warnings with the project's flags. C is built with `-std=c17 -Wall -Wextra` (`-Wshadow` in flcomp).
- The program's headless test passes: `make check` in its `src/` directory (chapter 25, section 3).
- If the change touches the session, the image or the installer, run the QEMU suite that covers it (chapter 25, sections 5 and 7).
  A pass in QEMU is not a pass on hardware (chapter 25, section 10); say so in the description.
- A visual change comes with a picture you have looked at (the project keeps screenshots under `docs/m7/`).
- A recipe change bumps `release` (chapter 21, section 10) and the recipe still builds on its own (`flpkg build --nocheck`).
- The documentation of the change is updated: a new key in `desktop.conf` goes into the program's README and chapter 23.
- Nothing from `~/.config` or a token file of your own is in the change.

## 6. Code style: C

The style is the one the tree already uses. Read a file of the program you change and copy it. The rules that show in the code:

- **Indentation with tabs.** The code uses tabs for indentation, 8 columns wide. Continuation lines are indented with tabs too.
- **Braces.** A function's opening brace is on its own line. Control statements have the brace on the same line:

  ```c
  static void img_free(struct img *im)   /* shortened from src/flview/flview.c */
  {
  	free(im->px);
  	im->px = NULL;
  }

  if (n == 0) {
  	...
  }
  ```

  A single statement under `if` or `for` often has no braces, as in `if (fd < 0)` followed by one line.
- **File header.** Every source file starts with a comment that names the file and says what it is for, then the licence:

  ```c
  /* flview -- the FreeLFS image viewer ("Images")
   * SPDX-License-Identifier: MIT
   *
   * What it does, in a few lines.
   */
  ```

- **Names.** Lower case with underscores. The public API of libflui is `fl_` prefixed (`fl_button`, `fl_win_new`). Programs use
  their own short prefix for their internal names. Enumerators are upper case (`FL_BTN_ACCENT`, `ID_OK`). `static` for everything
  that is not public. Helper functions are small and named for what they do.
- **Public functions of libflui** carry `FL_API`, and are declared in `flui.h` with a comment about what they return.
- **Errors.** Return `0` for success and `-1` (or a `NULL` pointer) for failure, and set `errno`. A program prints its failures with
  its own name first: `flview: cannot connect to a Wayland compositor`. Show errors the person can read where the program runs
  (chapter 24, section 9).
- **Bounded strings.** `snprintf` with `sizeof` for buffers. Never `sprintf` or `strcpy` with a size that is not checked. File
  contents go through a copy with a length check before they are drawn or printed.
- **Warnings.** The code compiles clean with `-Wall -Wextra`. `-Wno-unused-parameter` is accepted for callbacks that take `ud`.
- **Comments.** Explain why something is done, not what the next line does. Long rationale goes in the header or in the program's README.
- **Portability.** The code is C17 on Linux with glibc, with `_GNU_SOURCE` where a feature needs it. The project aims at other
  architectures later (chapter 28); keep the C free of x86 assumptions.
- **Memory.** The 1 GB RAM target is part of the design (chapter 18, and `docs/RAM-PROFILE.md`). Do not add an idle cost (a timer that
  wakes every second, a copy of a large file in memory) without a reason written in the code.

## 7. Code style: shell, Python and recipes

- **Shell (`build/`).** `#!/bin/bash` and `set -euo pipefail` at the top of a stage script. Shared functions live in
  `build/lib.sh`, settings in `build/config.sh`. A stage is resumable: it writes a stamp when a step is done (chapter 20, section 7).
  Quote every variable. Use `die` (from `lib.sh`) for errors, and `msg` for progress. The FreeLFS shell (`flsh`) is a separate
  program in C.
- **Python (`build/`, tests).** `#!/usr/bin/env python3`. The tree's generator tools run as `python3 -I` (isolated mode), so
  a file in the working directory cannot replace a module. The world map generator uses the standard library only (`src/flui/README`).
- **Recipes (`recipes/`).** Follow an existing recipe (chapter 21). Keep the field order of the others. A recipe has one `summary`
  line, one `license`, and checksums for every source.
- **Documentation (`docs/`).** Plain English, short sentences. Say what is verified and what is not ("not yet verified"). Mark a
  planned thing as planned.
- **Look and feel.** The desktop is flat: the palette from `docs/AGENT-RULES.md` (background `1d2125`, panel `272c33`, accent `3f9b63`
  by default), square corners, no gradients, no shadows, no emoji in the interface. Use the colour tokens of libflui, not literal
  colours (chapter 22, section 9).

## 8. Commit messages

Look at `git log`. The style in the tree:

- A subject line that names the change. A round of work gets a `Round N:` prefix and a list of what it contains, for example
  `Round 17: Bluetooth, printing, display settings, night light, desktop QoL`. A small change gets `Area: what it does`, for example
  `Workspaces UI: Settings > Workspaces page, pager right-click menu`, or `README: pre-alpha warning`.
- A blank line, then a body of short bullet points that say what changed in each part, and what is known to be missing
  (`Known gaps: ...`). Mention the tests that were run, and any hardware that was not tested.
- An optional `Co-Authored-By:` trailer. The history has one on agent-made commits.

Write the body for the reviewer: why, what, what you did not test. Keep the subject short (no length rule is written down).

## 9. Licence and third-party code

- FreeLFS's own code is under the **MIT licence** (`LICENSE`: "Copyright (c) 2026 The FreeLFS contributors"). Each source file
  carries `SPDX-License-Identifier: MIT`. Put that line in any new file.
- A contribution is made under the same licence. The repository has no separate contributor licence agreement (none is in the tree).
- Third-party software is packaged by its recipe with its own licence in the `license=` field (for example `Zlib`, `GPL-2.0-only`,
  `BSD-3-Clause`). Keep those licences correct when you change a recipe.
- Vendored or derived data says where it comes from and under what licence. The world map (`src/flui/worldmap_data.c`) is
  generated from Natural Earth data, which is in the public domain. The DSSL support (`src/libdssl`) follows the MIT-licensed
  DSSL project of EHS-Studio (`src/libdssl/README`).
- Do not copy code from a project under a licence you cannot ship. Ask in the issue tracker first.

## 10. Security reports

`docs/SECURITY.md` describes what is protected and how known vulnerabilities are tracked (`build/cve-check.py`,
`docs/CVE-REPORT.md`, `docs/CVE-KERNEL.md`). It does not name a private channel for reporting a vulnerability (not verified
elsewhere either). Until one is named, do not post exploit details in a public issue. Open an issue that says a security problem
needs a private contact, and wait for an answer.

## 11. FreeBWS: a community project (help wanted)

FreeBWS (`src/freebws`, the browser on WPE WebKit) is a community project inside the main repository. The plan
(`docs/PLAN-installer.md`, "FreeBWS as a community project") is:

- Issues on FreeBWS get the labels "help wanted" and "good first issue" (planned: the labels are not verified to exist yet).
- A file `docs/CONTRIBUTING-freebws.md` (planned, does not exist yet) will describe the build, the architecture and the known bugs
  (the Cloudflare Turnstile check and Google sign-in). Chapter 27 has the architecture and the known bugs now.
- Firefox (Flatpak) is the recommended browser. FreeBWS is the light one for 1 GB computers.
- The public site says: "Bug fixes are very welcome" for FreeBWS (`site/docs.html`).

Features that the FreeBWS README lists as "not there yet" (chapter 27): find in page, history, zoom and a private window. Checking
a known bug against a real site and recording exactly what happens is also useful.

## 12. Translations (planned)

The plan (`docs/PLAN-installer.md`) is to translate the desktop and its apps into the 12 languages the installer lists:
en_US, en_GB, de, fr, es, it, pt_BR, nl, pl, cs, sv and tr. The plan says a small catalogue in libflui, with the English
text as the default when a string is missing. No translation system exists in the tree yet (not verified in the source), so
there is nothing to translate into yet. Contributors who speak one of the languages can prepare the strings and wait for the
catalogue (chapter 28).
