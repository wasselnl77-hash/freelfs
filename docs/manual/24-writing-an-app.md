# 24. Writing an app: a step-by-step tutorial

Summary: this chapter builds a small FreeLFS program from nothing: a source directory, a Makefile, a `.desktop` file,
a recipe, a window that shows its errors, desktop settings, launching other programs, and a headless test.

## Contents

1. The example and what it does
2. Step 1: the directory layout
3. Step 2: the Makefile
4. Step 3: the program (`flsample.c`)
5. Step 4: the `.desktop` file (name, category, glyph)
6. Step 5: the icon
7. Step 6: launching the program, and launching others
8. Step 7: settings in `desktop.conf`
9. Step 8: errors are shown, never silent
10. Step 9: the recipe
11. Step 10: building and installing into the image
12. Step 11: testing headless
13. Checklist before you send it

---

## 1. The example and what it does

The example is `flsample`: it opens a text file given on the command line, lists its lines, and has a Reload button.
It shows errors in its own window. It is an illustration, not a program in the tree. Every function it uses is declared in
`src/flui/flui.h` (chapter 22).

The complete code is in this chapter. It was written against the real API but **not compiled** in this pass, because builds
were not allowed for this documentation task. Compile it as described in step 3 and fix any warning before you rely on it.

## 2. Step 1: the directory layout

Every FreeLFS program lives in its own directory `src/NAME/`, next to the others (`src/flview`, `src/fltime`, ...):

```
src/flsample/
    flsample.c          the program
    flsample.desktop    how the menu shows it
    Makefile            build, check and install
    README              what it is, its keys and settings (installed with the program)
    tests/run.sh        the headless test (step 11)
```

There is no icon file (step 5). The name `flsample` is `fl` plus a word. Use the same prefix for the binary, the
directory, the recipe and the `app_id`.

## 3. Step 2: the Makefile

The pattern is the same in every FreeLFS program. This is `src/flview/Makefile` with the names changed:

```make
# flsample -- an example text viewer
# SPDX-License-Identifier: MIT

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter
PKGS = flui
PREFIX ?= /usr
DESTDIR ?=

flsample: flsample.c
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) $(LDFLAGS) -o $@ $< $$(pkg-config --libs $(PKGS)) -lm

# headless: draws the first frame to a PPM with no compositor (tests/run.sh)
check: flsample
	./tests/run.sh

install: flsample
	install -Dm755 flsample $(DESTDIR)$(PREFIX)/bin/flsample
	install -Dm644 flsample.desktop $(DESTDIR)$(PREFIX)/share/applications/flsample.desktop
	install -Dm644 README $(DESTDIR)$(PREFIX)/share/doc/flsample/README

clean:
	rm -f flsample

.PHONY: check install clean
```

Notes:

- `CFLAGS +=` keeps the project's warnings on. `-std=c17` and `-D_GNU_SOURCE` are what every program in `src/` uses.
- `pkg-config --cflags --libs flui` brings the header and the library. The library must be installed or pointed at
  with `PKG_CONFIG_PATH` (for a tree build, `src/flui/flui.pc` after `make` in that directory).
- `make install` honours `DESTDIR`, which the recipe uses (step 9).
- A `check` target is how the test is run (step 11). The package build does not run it (chapter 21, section 3).

## 4. Step 3: the program (`flsample.c`)

```c
/* flsample -- shows a text file, one line per row, with a Reload button.
 * Example program for docs/manual/24-writing-an-app.md. SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <flui.h>

#define MAX_BYTES (256 * 1024)

enum { ID_LIST = 1, ID_RELOAD };

struct sample {
	fl_app *app;
	fl_win *win;
	fl_font *ui_font, *mono;
	fl_timer *msg_timer;
	fl_list_state list;
	char path[512];
	char *text;         /* the file, NUL terminated */
	int *starts;        /* offset in text of each line */
	int lines;
	char msg[160];      /* the status line: an error or a note */
	int msg_err;
};

/* Show a message in the status line for five seconds. */
static void say(struct sample *s, int err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s->msg, sizeof s->msg, fmt, ap);
	va_end(ap);
	s->msg_err = err;
	fl_timer_arm(s->msg_timer, 5000, 0);
}

static void msg_clear(void *ud)
{
	struct sample *s = ud;
	s->msg[0] = 0;
	fl_win_redraw(s->win);
}

static void forget(struct sample *s)
{
	free(s->text);
	free(s->starts);
	s->text = NULL;
	s->starts = NULL;
	s->lines = 0;
}

/* Read the file and index its lines. Returns 0, or -1 with a message in the status line. */
static int load(struct sample *s)
{
	FILE *f = fopen(s->path, "rb");
	if (!f) {
		say(s, 1, "Cannot open %s: %s", s->path, strerror(errno));
		return -1;
	}
	char *buf = malloc(MAX_BYTES + 1);
	if (!buf) {
		fclose(f);
		say(s, 1, "Out of memory");
		return -1;
	}
	size_t n = fread(buf, 1, MAX_BYTES, f);
	int more = fgetc(f) != EOF;
	int failed = ferror(f);
	fclose(f);
	if (failed) {
		free(buf);
		say(s, 1, "Cannot read %s", s->path);
		return -1;
	}
	buf[n] = 0;

	int count = 1;
	for (size_t i = 0; i < n; i++)
		if (buf[i] == '\n')
			count++;
	int *starts = malloc(sizeof *starts * (size_t)(count + 1));
	if (!starts) {
		free(buf);
		say(s, 1, "Out of memory");
		return -1;
	}
	int k = 0;
	starts[k++] = 0;
	for (size_t i = 0; i < n; i++)
		if (buf[i] == '\n' && i + 1 < n)
			starts[k++] = (int)(i + 1);

	forget(s);
	s->text = buf;
	s->starts = starts;
	s->lines = k;
	s->list = (fl_list_state){0};
	if (more)
		say(s, 0, "Showing the first 256 KB of the file");
	else
		say(s, 0, "%d lines", s->lines);
	return 0;
}

/* One row of the list: the text of line `index`, clipped to the row. */
static void draw_row(fl_ui *ui, fl_rect r, int index, int hover, int selected, void *ud)
{
	struct sample *s = ud;
	char line[512];
	int from = s->starts[index];
	int to = index + 1 < s->lines ? s->starts[index + 1] - 1 : (int)strlen(s->text);
	int len = to - from;
	if (len < 0)
		len = 0;
	if (len >= (int)sizeof line)
		len = (int)sizeof line - 1;
	memcpy(line, s->text + from, (size_t)len);
	line[len] = 0;

	if (selected)
		fl_fill_rect(&ui->cv, r, fl_th.select);
	else if (hover)
		fl_fill_rect(&ui->cv, r, fl_th.raised);
	fl_text_in(&ui->cv, s->mono, r, FL_LEFT, 8, selected ? fl_th.strong : fl_th.text, line);
}

static void draw(fl_ui *ui, void *ud)
{
	struct sample *s = ud;
	fl_rect c = fl_win_content(ui->win);
	fl_fill_rect(&ui->cv, c, fl_th.bg);

	fl_rect head = fl_cut_top(&c, 40);
	fl_label(ui, fl_inset(head, 12, 0), s->path[0] ? s->path : "No file", s->ui_font, fl_th.strong, FL_LEFT);
	if (fl_button(ui, ID_RELOAD, fl_r(head.x + head.w - 112, head.y + 6, 100, 28), "Reload", FL_BTN_OUTLINE) &&
	    s->path[0])
		load(s);

	fl_rect foot = fl_cut_bottom(&c, 28);
	if (s->msg[0])
		fl_label(ui, fl_inset(foot, 12, 0), s->msg, s->ui_font, s->msg_err ? fl_th.err : fl_th.dim, FL_LEFT);

	if (s->lines > 0)
		fl_list(ui, ID_LIST, c, s->lines, 20, &s->list, draw_row, s);
}

int main(int argc, char **argv)
{
	if (argc > 2) {
		fprintf(stderr, "usage: flsample [FILE]\n");
		return 2;
	}
	struct sample *s = calloc(1, sizeof *s);
	if (!s)
		return 1;
	if (argc == 2)
		snprintf(s->path, sizeof s->path, "%s", argv[1]);

	s->app = fl_app_new(NULL, NULL);
	if (!s->app) {
		fprintf(stderr, "flsample: cannot connect to a Wayland compositor\n");
		return 1;
	}
	s->ui_font = fl_font_ui(400, 13);
	s->mono = fl_font_mono(400, 13);
	s->msg_timer = fl_timer_new(s->app, msg_clear, s);

	fl_win_opts o = {
		.title = "Sample", .app_id = "flsample", .w = 640, .h = 480,
		.min_w = 320, .min_h = 240, .decorated = 1, .resizable = 1,
		.draw = draw, .ud = s,
	};
	s->win = fl_win_new(s->app, &o);
	if (!s->win) {
		fprintf(stderr, "flsample: cannot open the window\n");
		return 1;
	}

	if (s->path[0])
		load(s);
	else
		say(s, 0, "Start with a file: flsample FILE");

	fl_app_watch_conf(s->app, NULL, NULL);
	fl_app_run(s->app);

	forget(s);
	fl_timer_free(s->msg_timer);
	fl_font_close(s->ui_font);
	fl_font_close(s->mono);
	fl_app_free(s->app);
	free(s);
	return 0;
}
```

What the code shows, point by point:

- **Order.** `fl_app_new` first, then fonts and timers (they need the app), then the window, then `fl_app_run`.
- **Failures are reported in two places**: a message in the window (`say`) and, for a program that cannot start at all, on
  stderr with exit status 1. A failed load never ends the program; the window stays, with the message.
- **The draw callback is pure**: it draws from `s` and reports the button press. It runs many times, so it does no file
  I/O itself, except that the Reload button calls `load` in the pass where it was pressed (exactly once per click).
- **Widgets are placed inside `fl_win_content`**, and `fl_cut_top` and `fl_cut_bottom` take slices off the rectangle.
- **The list** (`fl_list`) keeps its selection in `s->list`, handles the arrow keys, and calls `draw_row` for each visible row.
- **Strings from the file** are copied into a bounded buffer before drawing (`draw_row`). Never pass file contents to a
  `printf` format.
- **Clean-up** at the end: the timer, the fonts and the app are released with their `fl_*_free`/`fl_*_close` calls, then the
  program frees its own memory.

Build it the way the Makefile does, from `src/flsample`: `make` (with `flui` installed or found by `pkg-config`).

## 5. Step 4: the `.desktop` file

`flsample.desktop` tells the start menu how to show and start the program. This is the format the panel reads
(`src/flpanel/menu.c`):

```ini
[Desktop Entry]
Type=Application
Name=Sample
GenericName=Text viewer
Comment=Show a text file, one line per row
Exec=flsample %f
Categories=Office;Viewer;
MimeType=text/plain;
X-FreeLFS-Glyph=S
```

What the panel reads today:

| Key | Use |
|---|---|
| `Type` | must be `Application` |
| `Name` | the name in the menu and in search (required; an entry without it is skipped) |
| `Comment` | the small text under the name |
| `GenericName` | **planned**, not read by the panel yet. `docs/PLAN-installer.md` asks for it on every `.desktop` file: the real name big, the generic name ("Text viewer") small under it, and search matches both. None of the 16 `.desktop` files in the tree has it yet. Write it now so that nothing has to change later. |
| `Exec` | the command. `%f` is the file given to the program when it is opened from Files ("Open with"). |
| `TryExec` | a program that must exist for the entry to show (optional) |
| `Categories` | menu groups. The panel maps: `Network`, `WebBrowser`, `Email` to Internet; `Office`, `TextEditor` to Office; `System`, `Utility`, `TerminalEmulator`, `FileManager`, `PackageManager` to System; `Settings` to Settings. Every entry is also in "All programs". Anything else is in "All programs" only. |
| `Terminal` | `true` when the program must run in a terminal (the panel reads it; no program in the tree sets it) |
| `NoDisplay` or `Hidden` | `true` to hide the entry |
| `MimeType` | the types this program opens. Files uses it for "Open with" (`src/flpanel/fileinfo.c`). `src/flview/flview.desktop` has `MimeType=image/png;image/jpeg;image/webp;`. |
| `X-FreeLFS-Glyph` | one character drawn on the program's tile in the menu. Without it, the tile shows the first letter of `Name`. The tree uses `T`, `≡`, `◫` and others. |
| `X-FreeLFS-Desktop-Icon` | `true` to put the program on the desktop as an icon (`src/flpanel/deskicons.c`) |

The `.desktop` file is installed to `/usr/share/applications` by the Makefile (step 3).

## 6. Step 5: the icon

FreeLFS programs have no image icon files. Two things stand in for one:

- `X-FreeLFS-Glyph`: a single character on the menu tile (step 4).
- The built-in icons of libflui (`fl_icon_draw`, chapter 22, section 7). The toolkit maps a few Wayland app ids to them
  with `fl_icon_for_app`: an id containing `terminal` (or `flterm`) gets the terminal icon, `settings` the settings icon,
  `files` the folder, `edit` the document. Your app id `flsample` gets the generic window icon. A program that wants its own
  picture draws it inside its window with `fl_path_svg` and `fl_path_fill`.

Do not add a PNG icon file just for the menu: the menu does not read one.

## 7. Step 6: launching the program, and launching others

A program starts from the menu with `Exec=`, with the file name in place of `%f`. It may also start another program.
`src/flfiles/flfiles.c` (function `spawn_desktop`, around line 1272) shows the full pattern, which is the safe one:

- Make a pipe with `O_CLOEXEC`, so that the parent learns whether `exec` worked.
- `fork`. In the child: `setsid()`, reset `SIGCHLD` to its default, close every descriptor from 3 up except the pipe's write
  end (so the new program does not inherit the file manager's sockets), then `execvp`.
- If `execvp` fails, the child writes `errno` to the pipe and exits with 127. The parent reads the pipe and shows the reason.

A short version:

```c
static int run_other(struct sample *s, char *const argv[])
{
	int ep[2];
	if (pipe2(ep, O_CLOEXEC) < 0) {
		say(s, 1, "Cannot start %s: %s", argv[0], strerror(errno));
		return -1;
	}
	pid_t pid = fork();
	if (pid == 0) {
		close(ep[0]);
		setsid();
		signal(SIGCHLD, SIG_DFL);
		for (int fd = 3; fd < 1024; fd++)
			if (fd != ep[1])
				close(fd);
		execvp(argv[0], argv);
		int e = errno;
		ssize_t wr = write(ep[1], &e, sizeof e);
		(void)wr;
		_exit(127);
	}
	close(ep[1]);
	if (pid < 0) {
		close(ep[0]);
		say(s, 1, "Cannot start %s: %s", argv[0], strerror(errno));
		return -1;
	}
	int e = 0;
	if (read(ep[0], &e, sizeof e) == sizeof e) {   /* exec failed in the child */
		close(ep[0]);
		say(s, 1, "Cannot start %s: %s", argv[0], strerror(e));
		return -1;
	}
	close(ep[0]);
	return 0;   /* started; the child is not waited for */
}
```

Notes: `pipe2`, `fork`, `setsid`, `signal` and `_exit` need `<unistd.h>`, `<signal.h>`, `<fcntl.h>` and `<sys/wait.h>` as the
real program includes. `flfiles` starts `flarchive` the same way (`execvp` after `fork`, near line 1024). A program that starts
another and must know when it ends has to `waitpid` for it; `flsample` does not.

Programs in the tree start one another by name (`execvp("flarchive", argv)`, `run_program("flterm")`). The name must be on the
`PATH` of the session (`/usr/bin`). Do not hard-code `/home`.

## 8. Step 7: settings in `desktop.conf`

`~/.config/freelfs/desktop.conf` (or `$FLUI_CONF`) is the shared settings file. It holds the look (colours, fonts, the panel,
animations) and values that the whole desktop shares. The reading functions are in `libflui` (chapter 22, section 9):

```c
const char *accent = fl_conf_get("accent", NULL);     /* a name such as "blue", or NULL */
int big = fl_conf_int("font_size", 13);               /* a number */
fl_app_watch_conf(s->app, on_settings_changed, s);    /* call on_settings_changed when the file changes */
```

Rules for an app:

- Take the **look** from libflui. Do not read `font` or `accent` yourself to draw: `fl_th`, `fl_lk` and the widget fonts already
  follow the file, and `fl_app_watch_conf` redraws the window when it changes.
- Read your **own** keys with a default, so the program works when the key is missing: `fl_conf_int("my_key", 0)`.
- Write a key with `fl_conf_set(key, value)` only when the whole desktop should know it (for example `power_saver`, which the
  panel writes and the compositor reads). `fl_conf_set_many` writes several at once.
- Settings that only one program needs go in a file of its own. The tree does this: FreeBWS keeps `~/.config/freebws/config`,
  and the desktop keeps icon positions in `~/.config/freelfs/desktop-icons.conf`. Give the file a name and a
  README entry, so the next person can find it.
- Document every key you read or write in the program's README (the `desktop.conf` keys are listed in chapter 23, section 16).

## 9. Step 8: errors are shown, never silent

The rule of the desktop: a program that fails must say so where the person can see it. A program started from the menu
has no terminal, so stderr is invisible. The image viewer's source says so in a comment: "errors are shown in the window: the
app is often started from Files, where nobody reads stderr". The pattern in `flsample` is:

- Every failure in a normal path goes to `say(...)`, which writes the status line with the reason (`strerror(errno)` for
  system calls, and the file name). Errors are red (`fl_th.err`), notes are dim.
- The message clears after a timer (`fl_timer_arm`), so the status line does not stay forever.
- A failed load does not exit. The window stays and shows the reason.
- The only exits are before the window exists: a usage error (status 2), or no compositor and no window (status 1), each with
  a message on stderr.
- A function that can fail returns an error code, and its caller checks it (the code above does).

Do not print a message and carry on as if it worked. Do not return 0 from `main` after an error.

## 10. Step 9: the recipe

`recipes/flsample/recipe` (copy of the flview recipe, step 3 of chapter 21, with the names changed):

```sh
pkgname=flsample
version=0.1
release=1
summary="Example text viewer: lists the lines of a file, shows its errors"
license=(MIT)
arch=(x86_64)
groups=(desktop)
depends=(glibc flui wayland)
makedepends=(flui)
source=()
sha256=()

build() {
	cp -r "$startdir/../../src/flsample" "$srcdir/flsample"
	cd "$srcdir/flsample"
	make clean
	make
}

package() {
	cd "$srcdir/flsample"
	make DESTDIR="$pkgdir" install
}
```

- `groups=(desktop)` puts it in the desktop set, so the image build installs it (chapter 20, section 5).
- `depends` lists what the program needs when it runs; `makedepends` what it needs to build (chapter 21).
- `release` is 1 for a new package and goes up for each change to the recipe later (chapter 21, section 10).

## 11. Step 10: building and installing into the image

From the top of the tree:

```sh
flpkg build -o ~/freelfs-work/out recipes/flsample     # build the package alone (no checks)
./build/60-packages.sh flsample                          # build it in the chroot and install it into the image root
```

`60-packages.sh flsample` rebuilds the package in the build environment, updates the repository and the root, and leaves the
rest of the build alone. Then make the image as described in chapter 20 (section 6). In the image the menu shows the program
after login.

Check the menu entry by booting the image (chapter 25) or by running the program in a session.

## 12. Step 11: testing headless

The headless test draws the first frame of the window to a picture, with no compositor. `FLUI_PPM` does that: `fl_app_new` does
not connect, `fl_app_run` renders each window to the file and returns (chapter 22, section 14). The test runs the program
on a known file and checks the picture. A minimal `tests/run.sh`:

```sh
#!/bin/sh
# Headless test of flsample: draws the first frame of a known file to a PPM and checks that the picture is a real one.
cd "$(dirname "$0")/.." || exit 1
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
printf 'first line\nsecond line\n' > "$dir/sample.txt"
FLUI_PPM="$dir/shot.ppm" ./flsample "$dir/sample.txt" || { echo "FAIL flsample exited with an error"; exit 1; }
head -c 2 "$dir/shot.ppm" | grep -q P6 || { echo "FAIL no picture"; exit 1; }
# a missing file must show an error in the window and still draw a picture
FLUI_PPM="$dir/missing.ppm" ./flsample "$dir/none.txt" || { echo "FAIL the window did not open for a missing file"; exit 1; }
[ -s "$dir/missing.ppm" ] || { echo "FAIL no picture for the error case"; exit 1; }
echo "ok   flsample"
```

Notes:

- Use `mktemp -d` (a scratch directory), never a fixed path in `/tmp` that another run could also use.
- The test checks the file format, not the pixels. A real test checks pixel colours (see chapter 25, section 6, on why a
  screendump must be checked and not just stored).
- `FLUI_PPM_DELAY=MS` lets timers and file descriptors run before the picture is taken, when the test needs a message that
  appears after a timer.
- Run it with `make check` in `src/flsample`.

## 13. Checklist before you send it

- The directory is `src/NAME/` with a Makefile (`all` or the default, `check`, `install DESTDIR=`, `clean`), a `.desktop` file,
  a README, and a test.
- The `.desktop` file has `Type`, `Name`, `Comment`, `Exec` with `%f`, `Categories`, `X-FreeLFS-Glyph`, and (planned)
  `GenericName`.
- The recipe is in `recipes/NAME/` with `groups=(desktop)`, `makedepends` and `depends`, and `source=()` for a tree program.
- Errors show in the window, with the reason; nothing fails silently; no exit status 0 after an error.
- The look comes from `fl_th` and `fl_lk`, not from literal colours; the program follows `fl_app_watch_conf`.
- Settings keys are read with defaults and documented in the README.
- Compiles with the project's flags (`-std=c17 -Wall -Wextra`) without warnings.
- The headless test passes, and the screenshot was looked at (AGENT-RULES asks for screenshots in `docs/m7/` for each app).
- The commit follows chapter 26.
