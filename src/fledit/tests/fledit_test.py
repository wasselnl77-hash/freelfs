#!/usr/bin/env python3
# Headless desktop tests for fledit: tabs, syntax colours, find and replace, go to line,
# auto-indent, bracket matching, the remembered session, a big file. Starts flcomp with
# FLCOMP_HEADLESS (like src/flcomp/tests/apps.py, whose Desktop class this uses), types and
# clicks through FLCOMP_TEST_FIFO, checks what ended up on disk, writes screenshots (PNG).
# SPDX-License-Identifier: MIT
#
#   FLEDIT=/path/to/fledit FLCOMP=/path/to/flcomp APPS_OUT=docs/m7 python3 -I fledit_test.py
#
# HOME is a scratch directory, so the session file goes to its .local/state, never to the real one.
import os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "flcomp", "tests"))
import apps  # noqa: E402

# the rest of a US keyboard, which apps.py does not need
apps.CODES.update({"lbracket": 26, "rbracket": 27, "backslash": 43, "grave": 41, "pgup": 104, "pgdn": 109})
SHIFT = {"!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8", "(": "9", ")": "0",
         "_": "minus", "+": "equal", "{": "lbracket", "}": "rbracket", ":": "semicolon", '"': "apostrophe",
         "<": "comma", ">": "dot", "?": "slash", "|": "backslash", "~": "grave"}
PLAIN = {" ": "space", "-": "minus", ".": "dot", "/": "slash", ",": "comma", "=": "equal", ";": "semicolon",
         "'": "apostrophe", "[": "lbracket", "]": "rbracket", "\\": "backslash", "`": "grave"}

check, fails = apps.check, 0


def type_text(dk, text):
    for ch in text:
        if ch == "\n":
            dk.key("enter")
        elif ch == "\t":
            dk.key("tab")
        elif ch.isupper():
            dk.key(ch.lower(), "shift")
        elif ch in SHIFT:
            dk.key(SHIFT[ch], "shift")
        else:
            dk.key(PLAIN.get(ch, ch))


def win_origin(d, W=1000):
    """top-left of the first window: the first pixel of the green line along the top of the title bar
    (apps.Desktop.origin compares with the corner pixel, which a window at 0,0 covers)"""
    accent = bytes([0x3f, 0x9b, 0x63])
    i = d.find(accent)
    while i >= 0 and i % 3:
        i = d.find(accent, i + 1)
    return (i // 3 % W, i // 3 // W) if i >= 0 else None


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def write(path, text):
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


SAMPLES = {
    "demo.c": '''#include <stdio.h>
#include "util.h"

#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* Count the words in a line. */
static int count_words(const char *s)
{
	int n = 0, in = 0;
	for (; *s; s++) {
		if (*s == ' ' || *s == '\\t') {
			in = 0;
		} else if (!in) {
			in = 1;
			n++;
		}
	}
	return n; // done
}

struct point { double x, y; };

int main(int argc, char **argv)
{
	const char *text = "the quick brown fox\\n";
	printf("%d words, max %d\\n", count_words(text), MAX(3, 0x1F));
	return argc > 1 ? 1 : 0;
}
''',
    "demo.sh": '''#!/bin/sh
# build and install
set -eu
PREFIX=${PREFIX:-/usr}
for f in src/*.c; do
	if [ -f "$f" ] && grep -q 'main' "$f"; then
		echo "building $f"
		cc -O2 -o "out/$(basename "$f" .c)" "$f" || exit 1
	fi
done
cat <<EOF > notes.txt
It's not code: $PREFIX stays text
EOF
install -Dm755 out/app "$DESTDIR$PREFIX/bin/app"
''',
    "demo.py": '''#!/usr/bin/env python3
"""A small module."""
import os
from dataclasses import dataclass


@dataclass
class Point:
    x: float = 0.0
    y: float = 0.0

    def dist(self, other: "Point") -> float:
        dx, dy = self.x - other.x, self.y - other.y
        return (dx * dx + dy * dy) ** 0.5  # euclid


def main(argv=None):
    pts = [Point(1, 2), Point(3.5, -4)]
    print(f"{len(pts)} points, first at {pts[0].x:.1f}")
    return 0 if os.path.exists("x") else 1
''',
    "demo.js": '''// A tiny module
const re = /ab+c/gi;
export function greet(name = "world") {
  const items = [1, 2.5, 0xff, true, null];
  /* multi
     line */
  return `Hello, ${name.toUpperCase()}! ${items.length} items`;
}
class Cache extends Map { static size = 10; }
const cfg = { "key": "value", count: 3 };
''',
    "demo.html": '''<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Demo &amp; test</title>
  <style>
    .card { color: #3f9b63; margin: 0 auto !important; }
  </style>
</head>
<body class="card">
  <!-- a comment -->
  <a href="https://example.org" id='main'>A link</a>
  <script>
    const n = 42; // answer
  </script>
</body>
</html>
''',
    "demo.css": '''/* the card */
:root { --accent: #3f9b63; }
.card:hover, #main > p {
  color: var(--accent);
  margin: 0 auto 1.5rem;
  background: url(card.png) no-repeat;
  font: 600 14px/1.4 "IBM Plex Sans", sans-serif;
}
@media (min-width: 600px) {
  .card { padding: 8px 12px !important; }
}
''',
    "demo.dssl": '''// FreeLFS card sheet
tokens {
  color {
    ink: #16161d
    accent: oklch(62% 0.19 35)
  }
  space { 1: 4, 2: 8, 4: 16 }
}

.card {
  background: color.ink/20
  padding: space.4 space.2
  radius: 6
  hover {
    shadow: 0 2 4 color.accent -> 0 4 12
  }
  use fire
  truncate
}

@media dark { .card { color: white } }

behavior fire {
  aura-glow: #ff7a00 36 pulse 900
}
raw {
  .x { color: red; }
}
''',
    "demo.md": '''# FreeLFS notes

Some *emphasis*, **strong text**, `inline code` and a [link](https://example.org).

- first item
- [ ] a todo
1. numbered

> a quote

| name | size |
|------|------|
| a    | 1    |

```c
int main(void) { return 0; }
```
''',
    "demo.conf": '''# a configuration file
[Desktop Entry]
Name=Text Editor
Exec=fledit %f
Terminal=false
Version = 1.5 # trailing comment
path = "/usr/share/${name}"
bare_key some value
''',
}


def start(dk, fledit, *files, **env):
    p = dk.start([fledit] + list(files), **env)
    for _ in range(50):  # a loaded machine can take seconds to show the window
        w, h, d = dk.shot()
        if win_origin(d):
            break
        time.sleep(0.2)
    return p


def content_origin(dk):
    w, h, d = dk.shot()
    o = win_origin(d)
    return o[0] + 1, o[1] + 36


def test(fledit):
    dk = apps.Desktop()
    home = dk.home
    # never the real XDG directories of whoever runs this
    dk.env.update(XDG_STATE_HOME=os.path.join(home, ".local", "state"), XDG_CONFIG_HOME=os.path.join(home, ".config"),
                  XDG_DATA_HOME=os.path.join(home, ".local", "share"), XDG_CACHE_HOME=os.path.join(home, ".cache"))
    work = os.path.join(home, "w")
    os.makedirs(work)
    for name, text in SAMPLES.items():
        write(os.path.join(work, name), text)
    P = lambda n: os.path.join(work, n)
    p = None
    try:
        # --- syntax colours, one shot per language
        p = start(dk, fledit, P("demo.c"))
        w, h, d = dk.shot("fledit-1-open")
        check("fledit: window appears", win_origin(d) is not None)
        # the cursor on a bracket: the partner is outlined
        dk.key("down", "ctrl") if False else None
        for _ in range(7):
            dk.key("down")
        dk.key("end")
        dk.shot("fledit-2-brackets")
        for name in ("demo.sh", "demo.py", "demo.js", "demo.html", "demo.css", "demo.dssl", "demo.md", "demo.conf"):
            dk.key("o", "ctrl")
            time.sleep(0.2)
            dk.key("a", "ctrl")
            type_text(dk, P(name))
            dk.key("enter")
            time.sleep(0.4)
            if name in ("demo.sh", "demo.py", "demo.html", "demo.dssl"):
                dk.shot("fledit-syntax-" + name.split(".")[1])
        dk.shot("fledit-3-tabs")
        check("fledit: nine files in tabs, still running", p.poll() is None)
        # tab keys: Ctrl+Tab, Alt+1
        dk.key("tab", "ctrl")
        dk.key("1", "alt")
        dk.shot("fledit-3b-first-tab")
        # a dirty tab shows the dot; the others stay clean
        dk.key("end", "ctrl")
        type_text(dk, "x")
        dk.shot("fledit-4-dirty-dot")
        # Ctrl+W on the dirty tab asks first; Don't save closes it
        dk.key("w", "ctrl")
        time.sleep(0.3)
        dk.shot("fledit-5-unsaved")
        dk.key("tab")
        dk.key("enter")
        time.sleep(0.4)
        check("fledit: Don't save left demo.c as it was", read(P("demo.c")) == SAMPLES["demo.c"])
        p.terminate()
        p.wait(5)

        # --- find, regular expressions, replace
        write(P("find.txt"), "foo123 bar45 Foo7\nnothing\nbaz9 foo\n")
        p = start(dk, fledit, P("find.txt"))
        dk.key("f", "ctrl")
        type_text(dk, "foo")
        dk.shot("fledit-6-find")
        dk.key("h", "ctrl")
        dk.shot("fledit-7-replace")
        # regex on: click the .* button; the pattern and the replacement use groups
        cx, cy = content_origin(dk)
        dk.key("a", "ctrl")
        type_text(dk, "([a-z]+)([0-9]+)")
        # the .* button: field (to 300 wide) + Previous + Next + Aa, then the button
        dk.click(cx + 12 + 300 + 8 + 90 + 74 + 38 + 17, cy + 36 + 22)
        dk.key("h", "ctrl")  # Ctrl+H again: into the replace field
        type_text(dk, "\\2-\\1")
        dk.key("enter", "ctrl")
        time.sleep(0.3)
        dk.shot("fledit-8-regex-replaced")
        dk.key("esc")
        dk.key("s", "ctrl")
        time.sleep(0.4)
        got = read(P("find.txt"))
        check("fledit: regex replace all with groups", got == "123-foo 45-bar 7-Foo\nnothing\n9-baz foo\n", repr(got))
        dk.key("z", "ctrl")
        dk.key("s", "ctrl")
        time.sleep(0.3)
        check("fledit: replace all is one undo step", read(P("find.txt")) == "foo123 bar45 Foo7\nnothing\nbaz9 foo\n", repr(read(P("find.txt"))))
        p.terminate()
        p.wait(5)

        # --- go to line
        write(P("lines.txt"), "".join("line %d\n" % i for i in range(1, 201)))
        p = start(dk, fledit, P("lines.txt"))
        dk.key("g", "ctrl")
        time.sleep(0.2)
        type_text(dk, "150:3")
        dk.shot("fledit-9-goto")
        dk.key("enter")
        time.sleep(0.3)
        type_text(dk, "X")
        dk.shot("fledit-9b-goto-done")
        dk.key("s", "ctrl")
        time.sleep(0.4)
        got = read(P("lines.txt")).split("\n")
        check("fledit: go to line 150 column 3", got[149] == "liXne 150", repr(got[149]))
        p.terminate()
        p.wait(5)

        # --- auto-indent
        p = start(dk, fledit, P("new.c"))
        type_text(dk, "int main(void) {\nreturn 0;\n}")
        dk.key("s", "ctrl")
        time.sleep(0.4)
        got = read(P("new.c"))
        check("fledit: C auto-indent and closing brace", got == "int main(void) {\n\treturn 0;\n}", repr(got))
        dk.key("a", "ctrl")
        type_text(dk, "f()\n{}")
        dk.key("left")
        dk.key("enter")
        type_text(dk, "x")
        dk.key("s", "ctrl")
        time.sleep(0.4)
        got = read(P("new.c"))
        check("fledit: Enter between { and } opens the block", got == "f()\n{\n\tx\n}", repr(got))
        p.terminate()
        p.wait(5)

        p = start(dk, fledit, P("new.py"))
        type_text(dk, "def f():\npass\nx = 1")
        dk.key("s", "ctrl")
        time.sleep(0.4)
        got = read(P("new.py"))
        check("fledit: Python indents after a colon (4 spaces)", got == "def f():\n    pass\n    x = 1", repr(got))
        # Tab on a selection indents the lines, Shift+Tab takes one level back
        dk.key("a", "ctrl")
        dk.key("tab")
        dk.key("s", "ctrl")
        time.sleep(0.4)
        got = read(P("new.py"))
        check("fledit: Tab indents a selection", got == "    def f():\n        pass\n        x = 1", repr(got))
        dk.key("tab", "shift")
        dk.key("s", "ctrl")
        time.sleep(0.4)
        got = read(P("new.py"))
        check("fledit: Shift+Tab dedents it", got == "def f():\n    pass\n    x = 1", repr(got))
        dk.key("end", "ctrl")
        for _ in range(6):
            dk.key("bksp")
        dk.key("s", "ctrl")
        time.sleep(0.4)
        got = read(P("new.py"))
        check("fledit: Backspace in the indentation steps a level", got == "def f():\n    pass\n", repr(got))
        p.terminate()
        p.wait(5)

        # --- the session: open files, tab order and cursor positions come back
        st = os.path.join(home, ".local", "state", "freelfs", "fledit", "session")
        p = start(dk, fledit)  # no file: this window owns the list of open files
        for name in ("demo.py", "demo.dssl"):
            dk.key("o", "ctrl")
            time.sleep(0.2)
            dk.key("a", "ctrl")
            type_text(dk, P(name))
            dk.key("enter")
            time.sleep(0.4)
        dk.key("down")
        dk.key("down")
        dk.key("q", "ctrl")
        time.sleep(0.6)
        check("fledit: session file written", os.path.exists(st))
        check("fledit: Ctrl+Q quits", p.poll() is not None)
        sess = read(st) if os.path.exists(st) else ""
        print("     session:", sess.replace("\n", " | ")[:300])
        p = start(dk, fledit)
        dk.shot("fledit-10-session-restored")
        check("fledit: session lists both files", sess.count("tab ") == 2, sess)
        p.terminate()
        p.wait(5)
        p = start(dk, fledit, P("lines.txt"))  # opened with a file: the saved list stays untouched
        dk.key("q", "ctrl")
        time.sleep(0.5)
        check("fledit: a window opened with a file keeps the list", read(st).count("tab ") == 2, read(st))
        # a fresh start with one file, a known cursor position
        os.unlink(st)
        p = start(dk, fledit)
        dk.key("o", "ctrl")
        time.sleep(0.2)
        dk.key("a", "ctrl")
        type_text(dk, P("lines.txt"))
        dk.key("enter")
        time.sleep(0.4)
        dk.key("g", "ctrl")
        time.sleep(0.2)
        type_text(dk, "77")
        dk.key("enter")
        time.sleep(0.3)
        time.sleep(2.6)  # the session is written two seconds after the cursor moved
        p.terminate()  # SIGTERM kills it without a clean exit: the debounced save must have written it
        p.wait(5)
        check("fledit: the debounced save wrote the session", os.path.exists(st))
        p = start(dk, fledit)
        type_text(dk, "Z")
        dk.key("s", "ctrl")
        time.sleep(0.4)
        got = read(P("lines.txt")).split("\n")
        check("fledit: reopened at the remembered line", len(got) > 77 and got[76].startswith("Zline 77"),
              repr(got[74:79]))
        p.terminate()
        p.wait(5)

        # --- a big file
        big = os.path.join(home, "big.c")
        unit = read(P("demo.c"))
        with open(big, "w") as f:
            n = 0
            while n < 5 * 1024 * 1024:
                f.write(unit)
                n += len(unit.encode())
        t0 = time.time()
        p = start(dk, fledit, big)
        rss0 = apps.rss_kb(p.pid)
        dk.shot("fledit-11-big")
        dk.key("end", "ctrl")
        t1 = time.time()
        w, h, d = dk.shot("fledit-12-big-end")
        t2 = time.time()
        dk.key("pgup")
        dk.key("f", "ctrl")
        type_text(dk, "count_words")
        time.sleep(0.5)
        t3 = time.time()
        w, h, d = dk.shot("fledit-13-big-find")
        rss = apps.rss_kb(p.pid)
        check("fledit: 5 MB file opens and stays up", p.poll() is None)
        check("fledit: 5 MB memory (<40 MB)", rss < 40000, "%d kB" % rss)
        print("     5 MB: %d kB RSS after open, %d kB after jump to the end and find; end-jump shot %.2f s, find shot %.2f s"
              % (rss0, rss, t2 - t1, time.time() - t3))
        p.terminate()
        p.wait(5)
        # idle memory of a small window
        p = start(dk, fledit, P("demo.py"))
        time.sleep(0.5)
        rss = apps.rss_kb(p.pid)
        check("fledit: idle memory under 25 MB", rss < 25000, "%d kB" % rss)
        p.terminate()
        p.wait(5)
    finally:
        dk.stop()
    return apps.fails


if __name__ == "__main__":
    fledit = os.environ.get("FLEDIT", "fledit")
    out = os.environ.get("APPS_OUT")
    if out:
        os.makedirs(out, exist_ok=True)
    if "FLCOMP" not in os.environ:
        os.environ["FLCOMP"] = os.path.join(HERE, "..", "..", "flcomp", "flcomp")
    rc = test(fledit)
    print("all tests passed" if not rc else "%d test(s) failed" % rc)
    sys.exit(1 if rc else 0)
