#!/usr/bin/env python3
"""Drive an interactive flsh in a pseudo terminal and check the screen.

    tests/interactive.py [-v]

A small terminal emulator keeps the screen, so the checks see what a person
would: the prompt, colours stripped, completion menus, wrapped lines.
"""

import fcntl
import os
import pty
import select
import shutil
import signal
import struct
import sys
import tempfile
import termios
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FLSH = os.path.join(HERE, "..", "flsh")
VERBOSE = "-v" in sys.argv
ROWS, COLS = 24, 60


class Screen:
    """Enough of a VT100 for a line editor: text, CR/LF, cursor moves, clears."""

    def __init__(self, rows, cols):
        self.rows, self.cols = rows, cols
        self.grid = [[" "] * cols for _ in range(rows)]
        self.r = self.c = 0
        self.wrap = False
        self.buf = b""

    def scroll(self):
        self.grid.pop(0)
        self.grid.append([" "] * self.cols)

    def newline(self):
        if self.r == self.rows - 1:
            self.scroll()
        else:
            self.r += 1

    def put(self, ch):
        if self.wrap:
            self.c = 0
            self.newline()
            self.wrap = False
        self.grid[self.r][self.c] = ch
        if self.c == self.cols - 1:
            self.wrap = True
        else:
            self.c += 1

    def feed(self, data):
        self.buf += data
        text = self.buf.decode("utf-8", "ignore")
        # keep an incomplete trailing sequence for next time
        self.buf = b""
        i = 0
        while i < len(text):
            ch = text[i]
            if ch == "\x1b":
                if i + 1 >= len(text):
                    self.buf = text[i:].encode()
                    return
                if text[i + 1] == "[":
                    j = i + 2
                    while j < len(text) and not ("\x40" <= text[j] <= "\x7e"):
                        j += 1
                    if j >= len(text):
                        self.buf = text[i:].encode()
                        return
                    self.csi(text[i + 2 : j], text[j])
                    i = j + 1
                    continue
                i += 2
                continue
            if ch == "\r":
                self.c = 0
                self.wrap = False
            elif ch == "\n":
                self.wrap = False
                self.newline()
            elif ch == "\b":
                self.c = max(0, self.c - 1)
                self.wrap = False
            elif ch == "\a":
                pass
            elif ord(ch) >= 32:
                self.put(ch)
            i += 1

    def csi(self, params, final):
        if params.startswith("?"):
            return
        nums = [int(p) if p.isdigit() else 0 for p in params.split(";")] if params else []
        n = nums[0] if nums and nums[0] else 1
        if final == "A":
            self.r = max(0, self.r - n)
        elif final == "B":
            self.r = min(self.rows - 1, self.r + n)
        elif final == "C":
            self.c = min(self.cols - 1, self.c + n)
        elif final == "D":
            self.c = max(0, self.c - n)
        elif final == "H":
            self.r = (nums[0] - 1) if nums and nums[0] else 0
            self.c = (nums[1] - 1) if len(nums) > 1 and nums[1] else 0
        elif final == "J":
            mode = nums[0] if nums else 0
            if mode == 0:
                self.grid[self.r][self.c :] = [" "] * (self.cols - self.c)
                for r in range(self.r + 1, self.rows):
                    self.grid[r] = [" "] * self.cols
            elif mode == 2:
                self.grid = [[" "] * self.cols for _ in range(self.rows)]
        elif final == "K":
            self.grid[self.r][self.c :] = [" "] * (self.cols - self.c)
        self.wrap = False

    def lines(self):
        return ["".join(row).rstrip() for row in self.grid]

    def text(self):
        return "\n".join(self.lines()).rstrip()

    def cursor_line(self):
        return "".join(self.grid[self.r]).rstrip()


class Shell:
    def __init__(self, home, cwd):
        env = {
            "HOME": home,
            "PATH": "/usr/bin:/bin",
            "TERM": "xterm",
            "USER": "tester",
            "LANG": "C.UTF-8",
            "XDG_DATA_HOME": os.path.join(home, ".local/share"),
            "XDG_CONFIG_HOME": os.path.join(home, ".config"),
        }
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.chdir(cwd)
            os.execve(FLSH, [FLSH], env)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
        self.screen = Screen(ROWS, COLS)
        self.read(1.0)

    def read(self, quiet=0.25, limit=5.0):
        end = time.time() + limit
        last = time.time()
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if r:
                try:
                    data = os.read(self.fd, 65536)
                except OSError:
                    break
                if not data:
                    break
                self.screen.feed(data)
                last = time.time()
            elif time.time() - last > quiet:
                break

    def send(self, keys, quiet=0.25):
        for k in keys if isinstance(keys, list) else [keys]:
            os.write(self.fd, k.encode() if isinstance(k, str) else k)
            self.read(0.08)
        self.read(quiet)

    def close(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        os.waitpid(self.pid, 0)


UP, DOWN, RIGHT, LEFT = "\x1b[A", "\x1b[B", "\x1b[C", "\x1b[D"
TAB, ENTER, ESC = "\t", "\r", "\x1b"


def ctrl(c):
    return chr(ord(c) & 0x1F)


failures = 0


def check(name, cond, sh):
    global failures
    if cond:
        print(f"ok   {name}")
    else:
        failures += 1
        print(f"FAIL {name}\n----- screen -----\n{sh.screen.text()}\n------------------")


def main():
    tmp = tempfile.mkdtemp(prefix="flsh-it-")
    home = os.path.join(tmp, "home")
    work = os.path.join(home, "work")
    os.makedirs(os.path.join(work, "alpine"))
    open(os.path.join(work, "alpha.txt"), "w").close()
    open(os.path.join(work, "beta file.txt"), "w").close()
    sh = Shell(home, work)
    try:
        check("prompt shows user and folder", "tester ~/work" in sh.screen.text() and "›" in sh.screen.text(), sh)

        sh.send(["echo hello", ENTER])
        check("runs a command", "\nhello" in sh.screen.text(), sh)

        sh.send(["false", ENTER])
        check("failure mark in the prompt", "✗1" in sh.screen.cursor_line(), sh)

        sh.send(["grpe foo", ENTER])
        check("did you mean", "Did you mean grep?" in sh.screen.text(), sh)

        sh.send("ec")
        check("suggestion from history", sh.screen.cursor_line().endswith("echo hello"), sh)
        sh.send([RIGHT, ENTER])
        check("Right takes the suggestion", sh.screen.text().count("\nhello") == 2, sh)

        sh.send(["ls al", TAB])
        check("Tab adds the common prefix", sh.screen.cursor_line().endswith("ls alp"), sh)
        sh.send(TAB)
        txt = sh.screen.text()
        check("second Tab opens the menu", "alpha.txt" in txt and "alpine/" in txt and "Tab: next" in txt, sh)
        sh.send(TAB)
        check("Tab selects the first entry", "ls alpha.txt" in sh.screen.text(), sh)
        sh.send(ENTER)
        check("Enter picks it and closes the menu", "Tab: next" not in sh.screen.text(), sh)
        sh.send(ENTER)
        check("the picked file is used", "\nalpha.txt" in sh.screen.text(), sh)

        sh.send(["ls be", TAB])
        check("file names get quoted", sh.screen.cursor_line().endswith("ls beta\\ file.txt"), sh)
        sh.send(ctrl("u"))
        sh.send([ctrl("c")])

        sh.send(["cd alpi", TAB])
        check("a directory completes with a slash", sh.screen.cursor_line().endswith("cd alpine/"), sh)
        sh.send([ENTER])
        check("cd changes the prompt folder", "~/work/alpine" in sh.screen.cursor_line() or "~/w/alpine" in sh.screen.cursor_line(), sh)
        sh.send(["cd ..", ENTER])

        sh.send(["for i in 1 2", ENTER])
        check("continuation prompt", sh.screen.cursor_line().startswith("…"), sh)
        sh.send(["do echo n$i; done", ENTER])
        txt = sh.screen.text()
        check("multi-line command runs", "\nn1\nn2" in txt, sh)

        sh.send(["sleepy", ctrl("c")])
        check("Ctrl-C cancels the line", "sleepy^C" in sh.screen.text() and "✗INT" in sh.screen.cursor_line(), sh)

        sh.send([ctrl("r"), "hel"])
        check("Ctrl-R finds a command", "echo hello" in sh.screen.text() and "search history: hel" in sh.screen.text(), sh)
        sh.send([ESC])
        check("Esc leaves the search", "search history" not in sh.screen.text(), sh)
        sh.send(ctrl("u"))

        sh.send(["e", UP])
        check("Up searches by prefix", sh.screen.cursor_line().endswith("echo hello"), sh)
        sh.send([ctrl("u"), UP])
        check("Up with nothing typed: the last command", sh.screen.cursor_line().endswith("› for i in 1 2↵do echo n$i; done"), sh)
        sh.send([UP])
        check("Up again goes further back", sh.screen.cursor_line().endswith("› cd .."), sh)
        sh.send([ctrl("u"), "e", UP, DOWN, "x"])
        check("Down comes back to what was typed", sh.screen.cursor_line().endswith("› ex"), sh)
        sh.send(ctrl("u"))

        long = "echo " + "x" * 70
        sh.send(long)
        lines = sh.screen.lines()
        check("long lines wrap", any(l.startswith("x" * 10) for l in lines), sh)
        sh.send([ctrl("a"), "#"])
        check("Ctrl-A goes to the start", "#echo xxx" in sh.screen.text(), sh)
        sh.send([ctrl("e"), ctrl("u")])
        check("Ctrl-U clears the wrapped line", "xxxxxxxxxx" not in sh.screen.text().split("\n")[-1], sh)

        sh.send(["sleep 30", ENTER], quiet=0.5)
        sh.send(ctrl("z"), quiet=0.5)
        check("Ctrl-Z stops a job", "Stopped" in sh.screen.text(), sh)
        sh.send(["jobs", ENTER])
        check("jobs lists it", "[1]+" in sh.screen.text() and "sleep 30" in sh.screen.text(), sh)
        sh.send(["fg", ENTER], quiet=0.5)
        sh.send(ctrl("c"), quiet=0.5)
        sh.send(["echo after $?", ENTER])
        check("fg and Ctrl-C end it", "after 130" in sh.screen.text(), sh)

        sh.send(["exit", ENTER], quiet=0.5)
        hist = open(os.path.join(home, ".local/share/flsh/history")).read()
        check("history was saved", "echo hello\n" in hist and "for i in 1 2\\ndo echo n$i; done" in hist, sh)
    finally:
        sh.close()
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"{'all passed' if not failures else f'{failures} failed'}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
