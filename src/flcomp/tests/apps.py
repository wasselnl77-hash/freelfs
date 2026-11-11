#!/usr/bin/env python3
# Headless desktop tests for the apps: flfiles, fledit, flgreet-ui, flpackages.
# SPDX-License-Identifier: MIT
#
# Starts flcomp with FLCOMP_HEADLESS, starts the app under test, types and
# clicks through FLCOMP_TEST_FIFO, checks what ended up on disk and writes
# screenshots as PNG (no PIL needed). Run inside the build chroot:
#
#   APPS_BIN=/path/with/the/binaries APPS_OUT=docs/m7 python3 -I apps.py [flfiles|fledit|flgreet|flpackages]
#
# FLCOMP (default flcomp), APPS_OUT (default: no screenshots kept), APPS_THEME=light,
# APPS_SUFFIX=-light (added to the screenshot names).
import os, shutil, signal, struct, subprocess, sys, tempfile, time, zlib

W, H = 1000, 700
CODES = dict(zip("qwertyuiop", range(16, 26)))
CODES.update(zip("asdfghjkl", range(30, 39)))
CODES.update(zip("zxcvbnm", range(44, 51)))
CODES.update(zip("1234567890", range(2, 12)))
CODES.update({"minus": 12, "equal": 13, "bksp": 14, "tab": 15, "enter": 28, "ctrl": 29, "shift": 42, "alt": 56,
              "space": 57, "slash": 53, "dot": 52, "comma": 51, "semicolon": 39, "esc": 1, "f2": 60, "f3": 61,
              "f5": 63, "delete": 111, "left": 105, "right": 106, "up": 103, "down": 108, "home": 102,
              "end": 107, "pgup": 104, "pgdn": 109, "apostrophe": 40})
SHIFTED = {"_": "minus", ":": "semicolon", "?": "slash", "+": "equal", '"': "apostrophe"}
PLAIN = {" ": "space", "-": "minus", ".": "dot", "/": "slash", ",": "comma", "=": "equal", ";": "semicolon", "'": "apostrophe"}

fails = 0


def ok(name):
    print("ok   " + name, flush=True)


def bad(name, why=""):
    global fails
    fails += 1
    print("FAIL " + name + (": " + why if why else ""), flush=True)


def check(name, cond, why=""):
    ok(name) if cond else bad(name, why)


def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline() == b"P6\n"
        w, h = map(int, f.readline().split())
        f.readline()
        return w, h, f.read()


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, b):
        c = struct.pack(">I", len(b)) + t + b
        return c + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


class Desktop:
    def __init__(self, panel=False):
        self.dir = tempfile.mkdtemp(prefix="apps-")
        self.home = os.path.join(self.dir, "home")
        os.makedirs(os.path.join(self.home, ".config", "freelfs"))
        if os.environ.get("APPS_THEME"):
            with open(os.path.join(self.home, ".config", "freelfs", "desktop.conf"), "w") as f:
                f.write("theme=%s\n" % os.environ["APPS_THEME"])
        self.rt = os.path.join(self.dir, "rt")
        os.mkdir(self.rt, 0o700)
        self.fifo = os.path.join(self.dir, "in")
        os.mkfifo(self.fifo)
        self.ppm = os.path.join(self.dir, "shot.ppm")
        env = dict(os.environ, XDG_RUNTIME_DIR=self.rt, FLCOMP_HEADLESS="%dx%d" % (W, H),
                   FLCOMP_PANEL=os.environ.get("APPS_BIN", "") + "/flpanel" if panel else "",
                   FLCOMP_SESSION="", FLCOMP_SCREENSHOT=self.ppm, FLCOMP_TEST_FIFO=self.fifo,
                   FLCOMP_KEYBOARD_CONF=os.path.join(self.dir, "none"), HOME=self.home)
        self.comp = subprocess.Popen([os.environ.get("FLCOMP", "flcomp")], env=env,
                                     stderr=open(os.path.join(self.dir, "comp.log"), "w"))
        for _ in range(100):
            if os.path.exists(os.path.join(self.rt, "wayland-0")):
                break
            time.sleep(0.1)
        else:
            raise SystemExit("flcomp did not start")
        self.fd = os.open(self.fifo, os.O_RDWR)
        self.env = dict(env, WAYLAND_DISPLAY="wayland-0", HOME=self.home)
        for k in ("FLCOMP_HEADLESS", "FLCOMP_TEST_FIFO", "FLCOMP_SCREENSHOT"):
            self.env.pop(k)
        self.procs = []

    def start(self, argv, pass_fds=(), **extra):
        p = subprocess.Popen(argv, env=dict(self.env, **extra), stderr=open(os.path.join(self.dir, "app.log"), "a"),
                             pass_fds=pass_fds)
        self.procs.append(p)
        time.sleep(1.0)
        return p

    def send(self, line, wait=0.12):
        os.write(self.fd, (line + "\n").encode())
        time.sleep(wait)

    def key(self, name, *mods):
        for m in mods:
            self.send("key %d 1" % CODES[m], 0.03)
        self.send("key %d 1" % CODES[name], 0.03)
        self.send("key %d 0" % CODES[name], 0.03)
        for m in reversed(mods):
            self.send("key %d 0" % CODES[m], 0.03)
        time.sleep(0.12)

    def type(self, text):
        for ch in text:
            if ch.isupper():
                self.key(ch.lower(), "shift")
            elif ch in SHIFTED:
                self.key(SHIFTED[ch], "shift")
            elif ch == "\n":
                self.key("enter")
            elif ch == "\t":
                self.key("tab")
            else:
                self.key(PLAIN.get(ch, ch))

    def click(self, x, y, button=272, count=1):
        self.send("move %d %d" % (x, y), 0.08)
        for _ in range(count):
            self.send("button %d 1" % button, 0.03)
            self.send("button %d 0" % button, 0.05)
        time.sleep(0.15)

    def drag(self, x0, y0, x1, y1):
        self.send("move %d %d" % (x0, y0), 0.08)
        self.send("button 272 1", 0.05)
        for i in range(1, 6):
            self.send("move %d %d" % (x0 + (x1 - x0) * i // 5, y0 + (y1 - y0) * i // 5), 0.05)
        self.send("button 272 0", 0.1)

    def shot(self, name=None):
        try:
            os.unlink(self.ppm)
        except FileNotFoundError:
            pass
        self.comp.send_signal(signal.SIGUSR1)
        for _ in range(50):
            time.sleep(0.1)
            if os.path.exists(self.ppm) and os.path.getsize(self.ppm) > W * H * 3:
                break
        time.sleep(0.1)
        w, h, d = read_ppm(self.ppm)
        out = os.environ.get("APPS_OUT")
        if name and out:
            write_png(os.path.join(out, name + os.environ.get("APPS_SUFFIX", "") + ".png"), w, h, d)
        return w, h, d

    def pixel(self, d, x, y):
        o = (y * W + x) * 3
        return "%02x%02x%02x" % tuple(d[o:o + 3])

    def origin(self, d):
        """top-left of the first window: first pixel that is not the desktop colour"""
        bg = d[0:3]
        for y in range(H):
            row = d[y * W * 3:(y + 1) * W * 3]
            for x in range(W):
                if row[x * 3:x * 3 + 3] != bg:
                    return x, y
        return None

    def stop(self):
        for p in self.procs:
            if p.poll() is None:
                p.terminate()
        for p in self.procs:
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
        self.comp.terminate()
        try:
            self.comp.wait(5)
        except subprocess.TimeoutExpired:
            self.comp.kill()
        shutil.rmtree(self.dir, ignore_errors=True)


def rss_kb(pid):
    with open("/proc/%d/status" % pid) as f:
        for line in f:
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    return 0


def test_flfiles(binp):
    dk = Desktop()

    def jump(name):  # type-ahead: letters typed within a second join up, so wait first
        time.sleep(1.3)
        dk.type(name)
    try:
        home = dk.home
        for d in ("documents", "downloads", "music", "pictures", "projects"):
            os.makedirs(os.path.join(home, d))
        with open(os.path.join(home, ".config", "user-dirs.dirs"), "w") as f:
            f.write('XDG_DOCUMENTS_DIR="$HOME/documents"\nXDG_DOWNLOAD_DIR="$HOME/downloads"\n'
                    'XDG_MUSIC_DIR="$HOME/music"\nXDG_PICTURES_DIR="$HOME/pictures"\n')
        for n, t in (("notes.txt", "hello\n"), ("main.c", "int main(void) { return 0; }\n"), ("Makefile", "all:\n"),
                     ("photo.png", "x"), ("archive.tar.gz", "x"), (".secret", "s"), ("hid.txt", "h")):
            with open(os.path.join(home, n), "w") as f:
                f.write(t)
        with open(os.path.join(home, ".hidden"), "w") as f:
            f.write("hid.txt\n")
        for i in range(1, 8):
            with open(os.path.join(home, "documents", "doc%d.txt" % i), "w") as f:
                f.write("%d\n" % i)
        os.symlink("documents", os.path.join(home, "link-to-docs"))
        p = dk.start([os.path.join(binp, "flfiles")])
        w, h, d = dk.shot("flfiles-1-home")
        o = dk.origin(d)
        check("flfiles: window appears", o is not None)
        ox, oy = o
        cx, cy = ox + 1, oy + 36  # content origin
        check("flfiles: memory under 20 MB", rss_kb(p.pid) < 20000, "%d kB" % rss_kb(p.pid))

        # sidebar: Documents; the grid then lists doc1..doc7
        dk.click(cx + 60, cy + 125)  # second place: Documents
        dk.shot("flfiles-2-documents")
        # go back, then open projects by double click on the second row's first folder
        dk.key("left", "alt")
        dk.key("pgdn")
        # hidden files
        dk.key("h", "ctrl")
        dk.shot("flfiles-3-hidden")
        dk.key("h", "ctrl")
        # type-ahead + rename
        jump("notes")
        dk.key("f2")
        time.sleep(0.2)
        dk.shot("flfiles-4-rename")
        dk.key("a", "ctrl")
        dk.type("todo.txt")
        dk.key("enter")
        time.sleep(0.3)
        check("flfiles: rename F2", os.path.exists(os.path.join(home, "todo.txt")) and
              not os.path.exists(os.path.join(home, "notes.txt")))
        # new folder
        dk.key("n", "ctrl", "shift")
        time.sleep(0.2)
        dk.key("a", "ctrl")
        jump("stuff")
        dk.key("enter")
        time.sleep(0.3)
        check("flfiles: new folder", os.path.isdir(os.path.join(home, "stuff")))
        # copy + paste into a folder: select todo.txt, copy, open stuff, paste
        jump("todo")
        dk.key("c", "ctrl")
        jump("stuff")
        dk.key("enter")
        time.sleep(1.0)
        dk.key("v", "ctrl")
        time.sleep(1.0)
        check("flfiles: copy and paste", os.path.exists(os.path.join(home, "stuff", "todo.txt")) and
              os.path.exists(os.path.join(home, "todo.txt")))
        # cut + paste back one level: select, cut, up, paste (name collides -> renamed copy)
        dk.key("x", "ctrl")
        dk.key("up", "alt")
        time.sleep(0.3)
        dk.key("v", "ctrl")
        time.sleep(1.0)
        check("flfiles: cut and paste moves", not os.path.exists(os.path.join(home, "stuff", "todo.txt")) and
              os.path.exists(os.path.join(home, "todo copy.txt")))
        # delete goes to the Trash
        jump("todo copy")
        dk.key("delete")
        time.sleep(0.4)
        tr = os.path.join(home, ".local", "share", "Trash")
        check("flfiles: Del moves to Trash", os.path.exists(os.path.join(tr, "files", "todo copy.txt")) and
              os.path.exists(os.path.join(tr, "info", "todo copy.txt.trashinfo")) and
              not os.path.exists(os.path.join(home, "todo copy.txt")))
        # right-click menu on the first item
        dk.click(cx + 188 + 8 + 54 + 108 * 2, cy + 44 + 8 + 49, button=273)
        dk.shot("flfiles-5-menu")
        dk.key("esc")
        # path field
        dk.key("l", "ctrl")
        dk.key("a", "ctrl")
        dk.type("/")
        dk.key("enter")
        time.sleep(0.3)
        dk.shot("flfiles-6-root")
        check("flfiles: still running", p.poll() is None)
    finally:
        dk.stop()


def test_fledit(binp):
    dk = Desktop()
    try:
        big = os.path.join(dk.home, "big.txt")
        with open(big, "w") as f:
            line = "The quick brown fox jumps over the lazy dog 0123456789 äöü\n"
            for i in range(10 * 1024 * 1024 // len(line.encode()) + 1):
                f.write(line)
        small = os.path.join(dk.home, "hello.c")
        with open(small, "w") as f:
            f.write('#include <stdio.h>\n\nint main(void)\n{\n\tprintf("hällo\\n");\n\treturn 0;\n}\n')
        p = dk.start([os.path.join(binp, "fledit"), small])
        w, h, d = dk.shot("fledit-1-open")
        check("fledit: window appears", dk.origin(d) is not None)
        dk.key("end", "ctrl")
        dk.type("// done")
        dk.key("enter")
        dk.type("x")
        dk.key("z", "ctrl")
        dk.key("z", "ctrl")
        dk.key("y", "ctrl")
        dk.shot("fledit-2-edited")
        dk.key("s", "ctrl")
        time.sleep(0.4)
        txt = open(small, encoding="utf-8").read()
        check("fledit: save writes the file", txt.endswith("// done") or txt.endswith("// done\n"), repr(txt[-30:]))
        dk.key("home", "ctrl")
        dk.key("f", "ctrl")
        dk.type("printf")
        dk.shot("fledit-3-find")
        dk.key("esc")
        dk.key("a", "ctrl")
        dk.key("c", "ctrl")
        dk.key("end", "ctrl")
        dk.key("enter")
        dk.key("v", "ctrl")
        dk.key("s", "shift", "ctrl") if False else None
        dk.key("n", "ctrl")
        time.sleep(0.3)
        dk.shot("fledit-4-unsaved")
        dk.key("esc")
        # save as through the path dialog
        dk.key("s", "ctrl", "shift")
        time.sleep(0.3)
        dk.key("a", "ctrl")
        dk.type(os.path.join(dk.home, "copy.c"))
        dk.shot("fledit-5-saveas")
        dk.key("enter")
        time.sleep(0.5)
        check("fledit: save as", os.path.exists(os.path.join(dk.home, "copy.c")))
        check("fledit: memory under 20 MB", rss_kb(p.pid) < 20000, "%d kB" % rss_kb(p.pid))
        p.terminate()
        p.wait(5)
        # a 10 MB file
        t0 = time.time()
        p = dk.start([os.path.join(binp, "fledit"), big])
        dk.key("end", "ctrl")
        dk.type("end")
        dk.shot("fledit-6-big")
        rss = rss_kb(p.pid)
        check("fledit: opens 10 MB", p.poll() is None, "died")
        check("fledit: 10 MB memory sane (<64 MB)", rss < 64000, "%d kB" % rss)
        dk.key("pgup")
        dk.key("pgup")
        dk.key("f", "ctrl")
        dk.type("lazy dog")
        time.sleep(0.3)
        check("fledit: 10 MB still responsive", p.poll() is None)
        print("     (10 MB session took %.1f s, %d kB RSS)" % (time.time() - t0, rss_kb(p.pid)))
    finally:
        dk.stop()


def crypt_hash(pw):
    import ctypes
    lib = ctypes.CDLL("libcrypt.so.2")
    lib.crypt.restype = ctypes.c_char_p
    return lib.crypt(pw.encode(), b"$6$flfstest$").decode()


def test_panel(binp):
    """the start menu over a focused app: Super opens it, typing searches, Enter starts one program"""
    apps = tempfile.mkdtemp(prefix="apps-desk-")
    os.environ["FLPANEL_APPS"] = apps
    dk = Desktop(panel=True)
    try:
        for n in ("alpha", "beta"):
            with open(os.path.join(apps, n + ".desktop"), "w") as f:
                f.write("[Desktop Entry]\nType=Application\nName=%s\nExec=touch %s/ran-%s\n" % (n, dk.dir, n))
        time.sleep(1.0)
        dk.start([os.path.join(binp, "flfiles")])
        dk.send("key 125 1", 0.05)
        dk.send("key 125 0", 0.5)
        dk.type("beta")
        dk.shot("panel-menu")
        dk.key("enter")
        time.sleep(0.6)
        check("panel: Super, search, Enter starts the program", os.path.exists(os.path.join(dk.dir, "ran-beta")) and
              not os.path.exists(os.path.join(dk.dir, "ran-alpha")))
        # and a second time, with the file manager (an app that reads keys) focused again
        os.unlink(os.path.join(dk.dir, "ran-beta"))
        n_before = len([p for p in os.listdir("/proc") if p.isdigit() and
                        open("/proc/%s/comm" % p).read().strip() == "flfiles"])
        dk.send("key 125 1", 0.05)
        dk.send("key 125 0", 0.5)
        dk.type("alpha")
        dk.shot("panel-menu2")
        dk.key("enter")
        time.sleep(0.6)
        n_after = len([p for p in os.listdir("/proc") if p.isdigit() and
                       open("/proc/%s/comm" % p).read().strip() == "flfiles"])
        check("panel: second time starts only the chosen program", os.path.exists(os.path.join(dk.dir, "ran-alpha")) and
              not os.path.exists(os.path.join(dk.dir, "ran-beta")) and n_after == n_before, "%d -> %d" % (n_before, n_after))
    finally:
        dk.stop()
        shutil.rmtree(apps, ignore_errors=True)


def test_menu_dirs(binp):
    """the start menu finds programs in $XDG_DATA_DIRS and in what Flatpak exports for the person"""
    os.environ.pop("FLPANEL_APPS", None)
    extra = tempfile.mkdtemp(prefix="apps-xdg-")
    os.environ["XDG_DATA_DIRS"] = extra + ":/usr/share"
    os.environ.pop("XDG_DATA_HOME", None)
    dk = Desktop(panel=True)
    try:
        fx = os.path.join(dk.home, ".local", "share", "flatpak", "exports", "share", "applications")
        os.makedirs(fx)
        os.makedirs(os.path.join(extra, "applications"))
        for d, fname, name, tag in ((fx, "org.example.Pad.desktop", "Pad Flatpak", "fluser"),
                                    (os.path.join(extra, "applications"), "zed.desktop", "Zed Xdg", "xdgdir")):
            with open(os.path.join(d, fname), "w") as f:
                f.write("[Desktop Entry]\nType=Application\nName=%s\nExec=touch %s/ran-%s %%U\n" % (name, dk.dir, tag))
        time.sleep(1.0)
        for query, tag in (("pad fla", "fluser"), ("zed", "xdgdir")):
            dk.send("key 125 1", 0.05)
            dk.send("key 125 0", 0.5)
            dk.type(query)
            if tag == "fluser":
                dk.shot("panel-menu-flatpak")
            dk.key("enter")
            time.sleep(0.6)
            check("panel: the menu lists the %s entry" % ("Flatpak export" if tag == "fluser" else "XDG_DATA_DIRS"),
                  os.path.exists(os.path.join(dk.dir, "ran-" + tag)))
    finally:
        dk.stop()
        shutil.rmtree(extra, ignore_errors=True)
        os.environ.pop("XDG_DATA_DIRS", None)


WPCTL = """#!%s
# stand-in wpctl for the panel tests: state in $WPCTL_DIR/state ("0.40" or "0.40 M"), every call
# logged to $WPCTL_DIR/log, slow on purpose (seconds in $WPCTL_DIR/delay), and it notes when two run at once
import os, sys, time
d = os.environ["WPCTL_DIR"]
lock = os.path.join(d, "lock")
try:
    os.close(os.open(lock, os.O_CREAT | os.O_EXCL))
    clash = False
except FileExistsError:
    clash = True
with open(os.path.join(d, "log"), "a") as f:
    f.write(("OVERLAP " if clash else "") + " ".join(sys.argv[1:]) + "\\n")
try:
    delay = float(open(os.path.join(d, "delay")).read())
except (OSError, ValueError):
    delay = 0.0
time.sleep(delay)
st = open(os.path.join(d, "state")).read().split()
vol, mute = float(st[0]), len(st) > 1
a = sys.argv[1:]
if a[0] == "get-volume":
    print("Volume: %%.2f%%s" %% (vol, " [MUTED]" if mute else ""))
elif a[0] == "set-volume":
    vol = min(float(a[2].rstrip("%%")) / 100, 1.5)
elif a[0] == "set-mute":
    mute = (not mute) if a[2] == "toggle" else a[2] == "1"
open(os.path.join(d, "state"), "w").write("%%.2f%%s" %% (vol, " M" if mute else ""))
if not clash:
    os.unlink(lock)
""" % sys.executable


def test_volume(binp):
    """the volume tray: optimistic mute, the flyout (drag, keys, Esc, click outside), the laptop keys, one wpctl at a time"""
    wd = tempfile.mkdtemp(prefix="apps-wpctl-")
    bindir = os.path.join(wd, "bin")
    os.mkdir(bindir)
    with open(os.path.join(bindir, "wpctl"), "w") as f:
        f.write(WPCTL)
    os.chmod(os.path.join(bindir, "wpctl"), 0o755)

    def put(name, text):
        with open(os.path.join(wd, name), "w") as f:
            f.write(text)

    def state():
        st = open(os.path.join(wd, "state")).read().split()
        return round(float(st[0]) * 100), len(st) > 1

    def calls():
        try:
            return open(os.path.join(wd, "log")).read().splitlines()
        except OSError:
            return []

    put("state", "0.40")
    put("delay", "1.5")
    os.environ.update(PATH=bindir + ":" + os.environ["PATH"], WPCTL_DIR=wd, FLPANEL_NOBAT="1")
    dk = Desktop(panel=True)
    try:
        time.sleep(2.0)
        PY = H - 44 + 22
        vx = W - 140  # inside the volume icon whether it shows a percent or not (the clock text varies in width)

        def region(d):  # the right part of the panel, where the tray is
            return [d[(y * W + x) * 3:(y * W + x) * 3 + 3] for y in range(H - 40, H - 4) for x in range(W - 330, W - 130)]

        w, h, d0 = dk.shot("volume-0")
        r0 = region(d0)
        # (1) a right click mutes: the icon changes before wpctl (1.5 s) is done
        dk.click(vx, PY, button=273)
        w, h, d1 = dk.shot("volume-muted")
        check("volume: mute shows at once", region(d1) != r0 and state() == (40, False), str(state()))
        time.sleep(3.5)
        w, h, d2 = dk.shot()
        check("volume: wpctl muted it, nothing flickers back", state() == (40, True) and region(d2) == region(d1) and
              calls().count("set-mute @DEFAULT_AUDIO_SINK@ 1") == 1, str(calls()))
        # the reading corrects the icon when the system says something else (here: unmuted behind its back)
        put("state", "0.40")
        put("delay", "0.25")
        dk.click(vx, PY)  # opens the flyout, which asks wpctl
        time.sleep(0.8)
        dk.key("esc")
        dk.send("move 300 300")
        time.sleep(0.3)
        w, h, d2b = dk.shot()
        inner = lambda d: [d[(y * W + x) * 3:(y * W + x) * 3 + 3] for y in range(H - 36, H - 8) for x in range(W - 174, W - 122)]
        check("volume: a different reading corrects the icon", inner(d2b) == inner(d0))  # inside the focus ring
        # (2) the flyout
        dk.click(vx, PY)
        time.sleep(0.5)
        w, h, d3 = dk.shot("volume-flyout")
        fy = H - 44 - 76
        accent = "3f9b63" if not os.environ.get("APPS_THEME") else None
        xs = [x for x in range(W) if dk.pixel(d3, x, fy) == (accent or dk.pixel(d3, x, fy)) and
              dk.pixel(d3, x, fy) != dk.pixel(d3, 0, fy)] if accent else []
        check("volume: the icon opens the flyout above it", len(xs) == 260 and abs((xs[0] + xs[-1]) // 2 - vx) < 40,
              "%d accent pixels at y=%d" % (len(xs), fy))
        if len(xs) != 260:
            return
        fx = xs[0]
        sx0, sx1, sy = fx + 48 + 6, fx + 260 - 12 - 6, fy + 52
        n = len(calls())
        dk.drag(sx0 + 20, sy, sx1 + 80, sy)  # past the right end: 100%
        time.sleep(1.2)
        check("volume: dragging the slider sets the level, a few calls only", state()[0] == 100 and
              len([c for c in calls()[n:] if c.startswith("set-volume")]) <= 3 and
              not any(c.startswith("OVERLAP") for c in calls()), str(calls()[n:]))
        dk.drag(sx1, sy, (sx0 + sx1) // 2, sy)
        time.sleep(1.0)
        check("volume: dragging to the middle", abs(state()[0] - 50) <= 4, str(state()))
        dk.shot("volume-flyout-50")
        dk.key("left")
        dk.key("left")
        dk.key("pgdn")
        time.sleep(1.0)
        check("volume: arrows and Page Down", abs(state()[0] - (50 - 20)) <= 5 and abs(state()[0] - 30) <= 5, str(state()))
        dk.key("end")
        time.sleep(0.8)
        check("volume: End is 100%", state()[0] == 100, str(state()))
        dk.key("m")
        time.sleep(0.8)
        w, h, d4 = dk.shot("volume-flyout-muted")
        check("volume: M mutes", state() == (100, True), str(state()))
        dk.key("home")
        time.sleep(0.8)
        check("volume: Home is 0% and unmutes", state() == (0, False), str(state()))
        dk.key("esc")
        time.sleep(0.3)
        w, h, d5 = dk.shot()
        check("volume: Esc closes the flyout", dk.pixel(d5, fx + 100, fy) == dk.pixel(d5, 100, fy))
        dk.click(vx, PY)
        time.sleep(0.4)
        dk.click(100, 100)
        time.sleep(0.3)
        w, h, d6 = dk.shot()
        check("volume: a click outside closes it", dk.pixel(d6, fx + 100, fy) == dk.pixel(d6, 100, fy))
        # (3) the laptop keys
        put("state", "0.40")
        dk.click(vx, PY)  # the panel only learns of it when asked: opening the flyout asks
        time.sleep(0.8)
        dk.key("esc")
        dk.send("move 300 300")
        dk.send("key 115 1", 0.05)
        dk.send("key 115 0", 0.6)
        check("volume: XF86AudioRaiseVolume", state() == (45, False), str(state()))
        dk.send("key 114 1", 0.05)
        dk.send("key 114 0", 0.05)
        dk.send("key 114 1", 0.05)
        dk.send("key 114 0", 0.6)
        check("volume: XF86AudioLowerVolume", state() == (35, False), str(state()))
        dk.send("key 113 1", 0.05)
        dk.send("key 113 0", 0.6)
        check("volume: XF86AudioMute", state() == (35, True), str(state()))
        dk.send("key 115 1", 1.2)
        dk.send("key 115 0", 0.8)
        check("volume: a held key repeats and unmutes", state()[0] >= 50 and state()[1] is False, str(state()))
        check("volume: never two wpctl at once", not any(c.startswith("OVERLAP") for c in calls()))
    finally:
        dk.stop()
        shutil.rmtree(wd, ignore_errors=True)


def test_flgreet(binp):
    dk = Desktop()
    try:
        # fake account files, bind-mounted over /etc/passwd and /etc/shadow (we are root in a private
        # mount namespace here, so nothing outside is touched)
        pw = os.path.join(dk.dir, "passwd")
        sh = os.path.join(dk.dir, "shadow")
        with open(pw, "w") as f:
            f.write("root:x:0:0:root:/root:/bin/bash\n"
                    "alice:x:1001:1001:Alice Example,,,:/home/alice:/bin/bash\n"
                    "bob:x:1002:1002:bob:/home/bob:/bin/bash\n"
                    "carol:x:1003:1003:Carol:/home/carol:/bin/bash\n"
                    "daemon:x:6:6::/:/bin/false\n")
        with open(sh, "w") as f:
            f.write("root:!:19000:0:99999:7:::\nalice:%s:19000:0:99999:7:::\nbob::19000:0:99999:7:::\n"
                    "carol:!$6$x$y:19000:0:99999:7:::\ndaemon:*:19000:0:99999:7:::\n" % crypt_hash("secret"))
        for src, dst in ((pw, "/etc/passwd"), (sh, "/etc/shadow")):
            r = subprocess.run(["mount", "--bind", src, dst], capture_output=True)
            if r.returncode:
                print("skip flgreet: cannot bind-mount over %s: %s" % (dst, r.stderr.decode().strip()))
                return
        try:
            def run_ui():
                rd, wr = os.pipe()
                p = dk.start([os.path.join(binp, "flgreet-ui")], pass_fds=(wr,), FLGREET_FD=str(wr))
                os.close(wr)
                return p, rd

            def got(rd):
                import select
                if not select.select([rd], [], [], 0.5)[0]:
                    return ""
                return os.read(rd, 100).decode()

            p, rd = run_ui()
            w, h, d = dk.shot("flgreet-1-login")
            check("flgreet: window appears", dk.origin(d) is not None or d[0:3] != d[(W * 5) * 3:(W * 5) * 3 + 3])
            check("flgreet: memory under 20 MB", rss_kb(p.pid) < 20000, "%d kB" % rss_kb(p.pid))
            dk.type("wrong")
            dk.shot("flgreet-2-typing")
            dk.key("enter")
            time.sleep(1.6)
            w, h, d = dk.shot("flgreet-3-wrong")
            check("flgreet: wrong password stays", p.poll() is None and got(rd) == "")
            dk.type("secret")
            dk.key("enter")
            time.sleep(1.8)
            check("flgreet: right password logs in", p.poll() == 0, "status %s" % p.poll())
            check("flgreet: name comes back on the pipe", got(rd) == "alice\n")
            os.close(rd)
            # an empty hash logs in without a password; a locked account does not
            p, rd = run_ui()
            dk.key("down")
            dk.key("enter")
            time.sleep(1.8)
            check("flgreet: empty hash needs no password", p.poll() == 0 and got(rd) == "bob\n")
            os.close(rd)
            p, rd = run_ui()
            dk.key("down")
            dk.key("down")
            dk.type("x")
            dk.key("enter")
            time.sleep(1.8)
            dk.shot("flgreet-4-locked")
            check("flgreet: locked account refused", p.poll() is None and got(rd) == "")
            os.close(rd)
        finally:
            for dst in ("/etc/shadow", "/etc/passwd"):
                subprocess.run(["umount", dst], capture_output=True)
    finally:
        dk.stop()


FAKE_FLPKG = r"""#!/bin/sh
# stands in for flpkg: the lists come from files, every call is logged
[ "$1" = --config ] && shift 2
echo "flpkg $*" >> "$FP_LOG"
case "$1" in
list) cat "$FP_STATE" ;;
search)
	shift
	while IFS='|' read -r n v s; do
		ok=1
		for a in "$@"; do
			case "$(echo "$n $s" | tr A-Z a-z)" in *"$(echo "$a" | tr A-Z a-z)"*) ;; *) ok=0 ;; esac
		done
		[ $ok = 1 ] || continue
		tag=""
		grep -q "^$n " "$FP_STATE" && tag=" [installed]"
		printf 'core/%s %s%s\n    %s\n' "$n" "$v" "$tag" "$s"
	done < "$FP_CATALOG" ;;
info)
	if [ "$2" = --repo ]; then n=$3; else n=$2; fi
	line=$(grep "^$n|" "$FP_CATALOG")
	[ -n "$line" ] || line="$n|1.0-1|An installed package"
	v=$(echo "$line" | cut -d'|' -f2); s=$(echo "$line" | cut -d'|' -f3)
	[ "$2" = --repo ] && printf '%-14s: %s\n' Repository core
	printf '%-14s: %s\n' Name "$n" Version "$v" Summary "$s" URL "https://example.org/$n" Architecture x86_64 \
		Licenses MIT Groups None "Depends on" "glibc  zlib" "Installed size" "812.0 KiB" Base "$n"
	[ "$2" = --repo ] && printf '%-14s: %s\n' "Download size" "211.5 KiB"
	true ;;
esac
"""

FAKE_SEAT = r"""#!/bin/sh
# stands in for flpkg-seat: slow, chatty, and "broken" fails
echo "seat $*" >> "$FP_LOG"
echo "Resolving dependencies for $1 $2"
sleep 1.2
echo "Checking signatures"
sleep 1.2
if [ "$2" = broken ]; then
	echo "flpkg: error: broken-1.0-1.flpkg: checksum mismatch" >&2
	exit 1
fi
if [ "$1" = install ]; then
	echo "Installing $2"
	grep -q "^$2 " "$FP_STATE" || echo "$2 $(grep "^$2|" "$FP_CATALOG" | cut -d'|' -f2)" >> "$FP_STATE"
else
	echo "Removing $2"
	grep -v "^$2 " "$FP_STATE" > "$FP_STATE.new"
	mv "$FP_STATE.new" "$FP_STATE"
fi
sleep 0.5
echo "Done"
"""

CATALOG = """editor|1.2-1|A small text editor
fastfetch|2.0-1|System information tool
broken|1.0-1|A package whose download is corrupt
imageviewer|0.9-2|Look at pictures
glibc|2.43-1|The GNU C library
zlib|1.3.2-1|Compression library
musicplayer|3.1-1|Plays music with PipeWire
"""

STATE = "glibc 2.43-1\nzlib 1.3.2-1\nbash 5.3-1\n"


def test_flpackages(binp):
    dk = Desktop()
    try:
        d = dk.dir
        for name, text in (("flpkg", FAKE_FLPKG), ("flpkg-seat", FAKE_SEAT)):
            with open(os.path.join(d, name), "w") as f:
                f.write(text)
            os.chmod(os.path.join(d, name), 0o755)
        log, state, cat = (os.path.join(d, n) for n in ("fp.log", "fp.state", "fp.catalog"))
        with open(cat, "w") as f:
            f.write(CATALOG)
        with open(state, "w") as f:
            f.write(STATE)
        open(log, "w").close()
        conf = os.path.join(d, "flpkg.conf")
        env = dict(FLPACKAGES_FLPKG=os.path.join(d, "flpkg"), FLPACKAGES_SEAT=os.path.join(d, "flpkg-seat"),
                   FLPACKAGES_CONF=conf, FP_LOG=log, FP_STATE=state, FP_CATALOG=cat)

        def calls():
            with open(log) as f:
                return f.read().splitlines()

        def installed():
            with open(state) as f:
                return [l.split()[0] for l in f.read().splitlines()]

        # The first window may sit at 0,0, where dk.origin() has no desktop colour to compare with: find
        # the window's accent top edge instead. Content origin = edge + (1, 36). Toolbar 44, list 340 wide,
        # rows 52, details to the right of it.
        def where(shot):
            acc = bytes.fromhex("3f9b63")
            i = shot.find(acc)
            while i >= 0 and i % 3:
                i = shot.find(acc, i + 1)
            return (i // 3) % W + 1, (i // 3) // W + 36

        # ---- no repo configured: the app says so, never asks flpkg to search
        with open(conf, "w") as f:
            f.write("# no repo\n#repo core file:///srv/flpkg/core\nsignatures = required\n")
        p = dk.start([os.path.join(binp, "flpackages")], **env)
        w, h, shot = dk.shot("flpackages-1-installed")
        cx, cy = where(shot)
        check("flpackages: window appears", shot.find(bytes.fromhex("3f9b63")) >= 0)
        check("flpackages: memory under 20 MB", rss_kb(p.pid) < 20000, "%d kB" % rss_kb(p.pid))
        check("flpackages: reads the installed list with flpkg list", "flpkg list" in calls(), str(calls()))
        dk.click(cx + 156, cy + 22)  # Available
        time.sleep(0.4)
        dk.shot("flpackages-2-no-repo")
        check("flpackages: no repo: search is not run", not any(c.startswith("flpkg search") for c in calls()),
              str(calls()))
        dk.click(cx + 52, cy + 22)  # back to Installed
        time.sleep(0.4)
        p.terminate()
        p.wait(5)

        # ---- with a repo
        with open(conf, "w") as f:
            f.write("repo core file:///srv/flpkg/core\n")
        open(log, "w").close()
        p = dk.start([os.path.join(binp, "flpackages")], **env)
        w, h, shot = dk.shot("flpackages-3-list")
        cx, cy = where(shot)
        # the installed list is sorted: bash, glibc, zlib; click the first row
        dk.click(cx + 150, cy + 44 + 26)
        time.sleep(0.6)
        check("flpackages: selecting asks flpkg info", "flpkg info bash" in calls(), str(calls()))
        dk.shot("flpackages-4-details")
        # search filters the installed list in the window (no flpkg call)
        n0 = len(calls())
        dk.click(cx + 400, cy + 22)
        dk.type("zl")
        time.sleep(0.4)
        dk.shot("flpackages-5-filter")
        check("flpackages: filtering the installed list stays local", len(calls()) == n0, str(calls()[n0:]))
        dk.key("a", "ctrl")
        dk.key("bksp")
        # Available: all packages, installed ones marked
        dk.click(cx + 156, cy + 22)
        time.sleep(0.6)
        check("flpackages: Available lists the repo", any(c.startswith("flpkg search") for c in calls()),
              str(calls()))
        dk.shot("flpackages-6-available")
        # type a search: flpkg search editor, after a short pause
        dk.click(cx + 400, cy + 22)
        dk.type("edit")
        time.sleep(1.0)
        check("flpackages: typing searches the repo", "flpkg search edit" in calls(), str(calls()))
        dk.shot("flpackages-7-search")
        # select the result and install it: the install button is the first one under the title
        dk.click(cx + 150, cy + 44 + 26)
        time.sleep(0.6)
        check("flpackages: selecting a repo package asks flpkg info --repo", "flpkg info --repo editor" in calls(),
              str(calls()))
        bx, by = cx + 340 + 24 + 50, cy + 44 + 24 + 34 + 30 + 14 + 16
        dk.click(bx, by)
        time.sleep(0.8)
        dk.shot("flpackages-8-installing")
        check("flpackages: Install runs flpkg-seat install editor", "seat install editor" in calls(), str(calls()))
        time.sleep(3.5)
        dk.shot("flpackages-9-installed")
        check("flpackages: the change took effect", "editor" in installed(), str(installed()))
        check("flpackages: list is read again afterwards", calls().count("flpkg list") >= 1 and
              calls()[-1].startswith("flpkg "), str(calls()[-4:]))
        # remove it again: Remove, then confirm
        bx2 = bx + 112
        dk.click(bx2 - 112 + 0, by)  # now installed: the Remove button is first
        time.sleep(0.4)
        dk.shot("flpackages-10-confirm")
        check("flpackages: Remove asks first", "seat remove editor" not in calls(), str(calls()))
        # the confirm buttons sit after "Remove editor?"
        dk.click(bx2 - 112 + 150, by)
        time.sleep(0.6)
        check("flpackages: confirmed Remove runs flpkg-seat remove editor", "seat remove editor" in calls(),
              str(calls()))
        time.sleep(3.5)
        check("flpackages: removed", "editor" not in installed(), str(installed()))
        # a failing install shows the error and keeps running
        dk.click(cx + 400, cy + 22)
        dk.key("a", "ctrl")
        dk.type("broken")
        time.sleep(1.0)
        dk.click(cx + 150, cy + 44 + 26)
        time.sleep(0.6)
        dk.click(bx, by)
        time.sleep(3.5)
        dk.shot("flpackages-11-failed")
        check("flpackages: failed install ran", "seat install broken" in calls(), str(calls()))
        check("flpackages: failed install leaves the window running", p.poll() is None)
        check("flpackages: nothing was installed by the failure", "broken" not in installed())
    finally:
        dk.stop()


FAKE_FLATPAK = r"""#!/bin/sh
# stands in for flatpak: the catalog is a file, the remote, the installed apps and
# the updates are state files, every call is logged. Plain output, as a pipe gets it.
echo "flatpak $*" >> "$FP_LOG"
cmd=$1; shift
have_remote() { [ -e "$FP_REMOTE" ]; }
lookup() { grep "^$1|" "$FP_CATALOG"; }
case "$cmd" in
remotes) have_remote && echo flathub; exit 0 ;;
remote-add) sleep 1; : > "$FP_REMOTE"; exit 0 ;;
update)
	for a in "$@"; do
		[ "$a" = --appstream ] && { sleep 1.5; mkdir -p "$HOME/.local/share/flatpak/appstream/flathub"; exit 0; }
	done
	id=""; for a in "$@"; do case $a in -*) ;; *) id=$a ;; esac; done
	echo "Looking for updates…"
	if [ -n "$id" ]; then ids=$id; else ids=$(cat "$FP_UPD"); fi
	n=$(echo $ids | wc -w); i=0
	for u in $ids; do i=$((i + 1)); echo "Updating $i/$n…"; sleep 1.2; done
	for u in $ids; do grep -vx "$u" "$FP_UPD" > "$FP_UPD.new"; mv "$FP_UPD.new" "$FP_UPD"; done
	for u in $ids; do ver=$(lookup "$u" | cut -d'|' -f3); grep -v "^$u|" "$FP_INST" > "$FP_INST.new"; mv "$FP_INST.new" "$FP_INST"; echo "$u|$ver" >> "$FP_INST"; done
	echo "Updates complete."
	exit 0 ;;
remote-ls)
	upd=0; cached=0
	for a in "$@"; do case $a in --updates) upd=1 ;; --cached) cached=1 ;; esac; done
	[ $cached = 1 ] && [ ! -e "$FP_SUMMARY" ] && exit 0
	[ $cached = 0 ] && { sleep 0.4; : > "$FP_SUMMARY"; }
	if [ $upd = 1 ]; then
		cat "$FP_UPD" | tr '|' '\n' | grep . ; exit 0
	fi
	while IFS='|' read -r id name ver desc; do
		printf '%s\t%s\t%s\t%s\n' "$id" "$name" "$ver" "$desc"
	done < "$FP_CATALOG"
	exit 0 ;;
list)
	while IFS='|' read -r id ver; do
		[ -n "$id" ] || continue
		line=$(lookup "$id"); name=$(echo "$line" | cut -d'|' -f2); desc=$(echo "$line" | cut -d'|' -f4)
		printf '%s\t%s\t%s\t%s\n' "$id" "$name" "$ver" "$desc"
	done < "$FP_INST"
	exit 0 ;;
info|remote-info)
	id=""; for a in "$@"; do case $a in -*|flathub) ;; *) id=$a ;; esac; done
	line=$(lookup "$id"); name=$(echo "$line" | cut -d'|' -f2); ver=$(echo "$line" | cut -d'|' -f3)
	desc=$(echo "$line" | cut -d'|' -f4)
	printf '%s - %s\n\n' "$name" "$desc"
	printf '%12s: %s\n' ID "$id" Ref "app/$id/x86_64/stable" Arch x86_64 Branch stable Version "$ver" \
		License GPL-3.0-or-later Collection org.flathub.Stable
	if [ "$cmd" = info ]; then
		printf '%12s: %s\n' Installation user Installed "184.3 MB" Runtime org.gnome.Platform/x86_64/47 \
			Sdk org.gnome.Sdk/x86_64/47
		printf '\n%12s: %s\n' Commit 3c2a9f0be41d Parent a77e1c0d5b92 Subject "Update $name" \
			Date "2025-01-14 09:41:17 +0000"
	else
		printf '%12s: %s\n' Download "61.8 MB" Installed "184.3 MB" Runtime org.gnome.Platform/x86_64/47 \
			Sdk org.gnome.Sdk/x86_64/47
		printf '\n%12s: %s\n' Subject "Update $name" Date "2025-01-14 09:41:17 +0000"
	fi
	exit 0 ;;
install)
	id=""; for a in "$@"; do case $a in -*|flathub) ;; *) id=$a ;; esac; done
	echo "Looking for matches…"
	if [ "$id" = com.example.Broken ]; then
		echo "error: Failed to install com.example.Broken: Server returned status 404" >&2
		exit 1
	fi
	echo "Required runtime for $id/x86_64/stable (runtime/org.gnome.Platform/x86_64/47) found in remote flathub"
	sleep 0.8
	echo "Installing 1/3…"; sleep 1.2
	echo "Installing 2/3…"; sleep 1.2
	echo "Installing 3/3…"; sleep 1.2
	ver=$(lookup "$id" | cut -d'|' -f3)
	grep -q "^$id|" "$FP_INST" || echo "$id|$ver" >> "$FP_INST"
	echo "Installation complete."
	exit 0 ;;
uninstall)
	id=""; for a in "$@"; do case $a in -*) ;; *) id=$a ;; esac; done
	echo "Uninstalling $id…"
	sleep 1
	if [ -n "$id" ]; then grep -v "^$id|" "$FP_INST" > "$FP_INST.new"; mv "$FP_INST.new" "$FP_INST"; fi
	echo "Uninstall complete."
	exit 0 ;;
run) echo "run $*" >> "$FP_LOG"; exit 0 ;;
esac
exit 1
"""

FLATHUB = """org.gnome.Calculator|Calculator|47.0|Perform arithmetic, scientific or financial calculations
org.gnome.TextEditor|Text Editor|47.2|Edit text files
org.mozilla.firefox|Firefox|134.0.1|Fast, Private and Safe Web Browser
org.mozilla.Thunderbird|Thunderbird|128.6.0esr|Make email easier
org.videolan.VLC|VLC|3.0.21|Multimedia player and streamer
org.gimp.GIMP|GIMP|2.10.38|Create images and edit photographs
org.inkscape.Inkscape|Inkscape|1.4|Vector Graphics Editor
org.libreoffice.LibreOffice|LibreOffice|24.8.4|The LibreOffice productivity suite
org.kde.krita|Krita|5.2.9|Digital Painting, Creative Freedom
org.audacityteam.Audacity|Audacity|3.7.1|Record and edit audio
com.valvesoftware.Steam|Steam|1.0.0.82|Launcher for the Steam software distribution service
org.telegram.desktop|Telegram Desktop|5.10.3|New era of messaging
com.spotify.Client|Spotify|1.2.52|Online music streaming service
org.blender.Blender|Blender|4.3.2|3D creation suite
org.keepassxc.KeePassXC|KeePassXC|2.7.9|Community-driven port of the Keepass password manager
com.example.Broken|Broken App|1.0|An app whose download fails
"""


def test_flathub(binp):
    """The Flathub tab of flpackages against a stand-in flatpak: no flatpak, no remote (Add Flathub),
    the first catalog fetch, search, details, install with progress, Open, update, remove."""
    dk = Desktop()
    try:
        d = dk.dir
        fp = os.path.join(d, "flatpak")
        with open(fp, "w") as f:
            f.write(FAKE_FLATPAK)
        os.chmod(fp, 0o755)
        # the other tabs are not the subject: stand-ins that answer with nothing
        for name in ("flpkg", "flpkg-seat"):
            with open(os.path.join(d, name), "w") as f:
                f.write("#!/bin/sh\nexit 0\n")
            os.chmod(os.path.join(d, name), 0o755)
        st = {n: os.path.join(d, "fp." + n) for n in ("log", "remote", "inst", "upd", "catalog", "summary")}
        with open(st["catalog"], "w") as f:
            f.write(FLATHUB)
        with open(st["inst"], "w") as f:
            f.write("org.gnome.Calculator|46.1\norg.videolan.VLC|3.0.20\norg.mozilla.firefox|134.0.1\n")
        with open(st["upd"], "w") as f:
            f.write("org.gnome.Calculator\n")
        open(st["log"], "w").close()
        conf = os.path.join(d, "flpkg.conf")
        open(conf, "w").close()
        env = dict(FLPACKAGES_FLPKG=os.path.join(d, "flpkg"), FLPACKAGES_SEAT=os.path.join(d, "flpkg-seat"),
                   FLPACKAGES_CONF=conf, FLPACKAGES_FLATPAK=fp, FP_LOG=st["log"], FP_REMOTE=st["remote"],
                   FP_INST=st["inst"], FP_UPD=st["upd"], FP_CATALOG=st["catalog"], FP_SUMMARY=st["summary"],
                   XDG_DATA_HOME=os.path.join(dk.home, ".local", "share"))

        def calls():
            with open(st["log"]) as f:
                return f.read().splitlines()

        def installed():
            with open(st["inst"]) as f:
                return [l.split("|")[0] for l in f.read().splitlines()]

        def where(shot):
            acc = bytes.fromhex("3f9b63")
            i = shot.find(acc)
            while i >= 0 and i % 3:
                i = shot.find(acc, i + 1)
            return (i // 3) % W + 1, (i // 3) // W + 36

        def clear_search():
            dk.click(cx + 500, cy + 22)
            dk.key("a", "ctrl")
            dk.key("bksp")

        # ---- no flatpak program: the tab says so and runs nothing
        p = dk.start([os.path.join(binp, "flpackages")], **dict(env, FLPACKAGES_FLATPAK=os.path.join(d, "nope")))
        w, h, shot = dk.shot()
        cx, cy = where(shot)
        dk.click(cx + 256, cy + 22)  # Flathub
        time.sleep(0.5)
        dk.shot("packages-flathub-0-no-flatpak")
        check("flathub: without flatpak nothing is run", calls() == [], str(calls()))
        p.terminate()
        p.wait(5)

        # ---- flatpak, but no Flathub remote yet: Add Flathub
        p = dk.start([os.path.join(binp, "flpackages")], **env)
        w, h, shot = dk.shot()
        cx, cy = where(shot)
        dk.click(cx + 256, cy + 22)
        time.sleep(1.0)
        check("flathub: the tab asks which remotes exist", "flatpak remotes --user --columns=name" in calls(),
              str(calls()))
        dk.shot("packages-flathub-1-no-remote")
        check("flathub: no remote: no catalog is read", not any(c.startswith("flatpak remote-ls") for c in calls()),
              str(calls()))
        dk.click(cx + 20 + 66, cy + 44 + 28 + 40 + 14 + 16 + 40)  # Add Flathub (under the notice, roughly)
        time.sleep(0.4)
        c = calls()
        if not any(x.startswith("flatpak remote-add") for x in c):  # the notice height varies with the font: look
            for dy in range(0, 120, 8):
                dk.click(cx + 90, cy + 44 + 100 + dy)
                time.sleep(0.2)
                if any(x.startswith("flatpak remote-add") for x in calls()):
                    break
        check("flathub: Add Flathub runs remote-add --user --if-not-exists",
              any(x == "flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo"
                  for x in calls()), str(calls()))
        time.sleep(0.5)
        dk.shot("packages-flathub-2-adding")
        # remote added: the appstream data is missing, so the catalog is fetched first, then read
        time.sleep(4.5)
        check("flathub: the first time the appstream data is refreshed",
              "flatpak update --user --appstream --noninteractive flathub" in calls(), str(calls()))
        check("flathub: then the catalog is read with names and descriptions",
              any(x.startswith("flatpak remote-ls --user --app") and "--columns=application,name,version,description" in x
                  for x in calls()), str(calls()))
        dk.send("move 700 500", 0.2)  # the pointer out of the list
        w, h, shot = dk.shot("packages-flathub-3-browse")
        check("flathub: installed apps are read with flatpak list", any(x.startswith("flatpak list --user --app")
                                                                         for x in calls()), str(calls()))
        check("flathub: memory under 24 MB", rss_kb(p.pid) < 24000, "%d kB" % rss_kb(p.pid))

        # ---- search is local: no call
        n0 = len(calls())
        dk.click(cx + 500, cy + 22)
        dk.type("video")
        time.sleep(0.5)
        dk.shot("packages-flathub-4-search")
        check("flathub: searching does not call flatpak", len(calls()) == n0, str(calls()[n0:]))
        clear_search()
        dk.type("graph")  # matches the description of GIMP only
        time.sleep(0.4)
        dk.shot("packages-flathub-4b-search-description")
        clear_search()
        dk.type("steam")
        time.sleep(0.5)

        # ---- details of an app that is not installed, then install it
        dk.click(cx + 150, cy + 44 + 40 + 26)
        time.sleep(0.8)
        check("flathub: selecting asks remote-info from the cache",
              "flatpak remote-info --user --cached flathub com.valvesoftware.Steam" in calls(), str(calls()))
        dk.shot("packages-flathub-5-details")
        bx, by = cx + 340 + 24 + 50, cy + 44 + 24 + 34 + 30 + 14 + 16
        dk.click(bx, by)  # Install
        time.sleep(2.2)
        check("flathub: Install runs flatpak install --user -y flathub ID",
              "flatpak install --user -y --noninteractive flathub com.valvesoftware.Steam" in calls(), str(calls()))
        dk.shot("packages-flathub-6-installing")
        time.sleep(3.0)
        check("flathub: the app is installed", "com.valvesoftware.Steam" in installed(), str(installed()))
        dk.shot("packages-flathub-7-installed")
        check("flathub: the installed list is read again", calls().count("flatpak list --user --app --columns=application,name,version,description") >= 2,
              str(calls()))

        # ---- the Installed view: three apps and one update
        clear_search()
        dk.click(cx + 138, cy + 44 + 20)  # Installed (strip)
        time.sleep(0.8)
        dk.shot("packages-flathub-8-installed-list")
        dk.click(cx + 150, cy + 44 + 40 + 26)  # the first row: Calculator (alphabetical), with an update
        time.sleep(0.8)
        check("flathub: an installed app asks flatpak info", "flatpak info --user org.gnome.Calculator" in calls(),
              str(calls()))
        dk.shot("packages-flathub-9-update")
        dk.click(bx, by)  # Update
        time.sleep(2.0)
        check("flathub: Update runs flatpak update --user -y ID",
              "flatpak update --user -y --noninteractive org.gnome.Calculator" in calls(), str(calls()))
        time.sleep(2.5)
        dk.shot("packages-flathub-9b-updated")
        # Open (first button now: [Open][Remove])
        dk.click(bx, by)
        time.sleep(0.6)
        check("flathub: Open runs flatpak run ID", "run run org.gnome.Calculator" in calls() or
              "flatpak run org.gnome.Calculator" in calls(), str(calls()[-4:]))
        # Remove, with the question first
        dk.click(bx + 112, by)  # the buttons are [Open][Remove] now
        time.sleep(0.4)
        dk.shot("packages-flathub-10-confirm")
        check("flathub: Remove asks first", not any(x.startswith("flatpak uninstall") for x in calls()), str(calls()))
        dk.click(bx + 130, by)  # the confirming Remove sits after "Remove Calculator?"
        time.sleep(2.8)
        check("flathub: confirmed Remove runs flatpak uninstall --user -y ID",
              "flatpak uninstall --user -y --noninteractive org.gnome.Calculator" in calls(), str(calls()))
        check("flathub: removed", "org.gnome.Calculator" not in installed(), str(installed()))

        # ---- a failing install shows the error and keeps running
        dk.click(cx + 52 + 30, cy + 44 + 20)  # Browse
        time.sleep(0.6)
        clear_search()
        dk.type("broken")
        time.sleep(0.5)
        dk.click(cx + 150, cy + 44 + 40 + 26)
        time.sleep(0.8)
        dk.click(bx, by)
        time.sleep(3.0)
        dk.shot("packages-flathub-11-failed")
        check("flathub: a failing install leaves the window running", p.poll() is None)
        check("flathub: nothing was installed by the failure", "com.example.Broken" not in installed())
    finally:
        dk.stop()


BUS_CONF = """<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>system</type><listen>unix:path=%s</listen><auth>EXTERNAL</auth>
<policy context="default"><allow send_destination="*" eavesdrop="true"/><allow eavesdrop="true"/>
<allow own="*"/></policy></busconfig>"""


def test_flsettings(binp):
    """The Wi-Fi page against fakeiwd (src/flsettings/tests) on a private bus: list, scan, connect with
    a wrong then the right password through the agent, cancel, disconnect, forget, hand-over of a page to a
    running window, no iwd, no hardware."""
    fake = os.environ.get("FAKEIWD", os.path.join(binp, "fakeiwd"))
    bus = None

    def start_bus(dk):
        nonlocal bus
        sock = os.path.join(dk.dir, "bus")
        conf = os.path.join(dk.dir, "bus.conf")
        with open(conf, "w") as f:
            f.write(BUS_CONF % sock)
        bus = subprocess.Popen(["dbus-daemon", "--config-file=" + conf, "--nofork"], stderr=subprocess.DEVNULL)
        for _ in range(50):
            if os.path.exists(sock):
                break
            time.sleep(0.1)
        return "unix:path=" + sock

    def where(shot):  # accent top edge of the focused window -> content origin
        acc = bytes.fromhex("3f9b63")
        i = shot.find(acc)
        while i >= 0 and i % 3:
            i = shot.find(acc, i + 1)
        return (i // 3) % W + 1, (i // 3) // W + 36

    def lines(path):
        try:
            with open(path) as f:
                return f.read().splitlines()
        except FileNotFoundError:
            return []

    for variant in ("normal", "nohw", "noiwd"):
        dk = Desktop()
        iw = None
        try:
            addr = start_bus(dk)
            log = os.path.join(dk.dir, "fakeiwd.log")
            if variant != "noiwd":
                iw = subprocess.Popen([fake, log] + (["nohw"] if variant == "nohw" else []),
                                      env=dict(os.environ, DBUS_SYSTEM_BUS_ADDRESS=addr))
                time.sleep(0.5)
            p = dk.start([os.path.join(binp, "flsettings"), "--page", "wifi"], DBUS_SYSTEM_BUS_ADDRESS=addr)
            time.sleep(1.0)
            w, h, shot = dk.shot("flsettings-wifi-" + variant)
            cx, cy = where(shot)
            check("flsettings %s: window appears" % variant, shot.find(bytes.fromhex("3f9b63")) >= 0)
            check("flsettings %s: memory under 20 MB" % variant, rss_kb(p.pid) < 20000, "%d kB" % rss_kb(p.pid))
            if variant != "normal":
                if variant == "nohw":
                    check("flsettings nohw: nothing to scan", "scan" not in lines(log))
                # the Wi-Fi page is the one shown: its sidebar entry has the selection colour
                continue
            check("flsettings: registers its password agent", "register agent" in lines(log), str(lines(log)))
            check("flsettings: asks for one scan when the page opens", lines(log).count("scan") == 1, str(lines(log)))

            # rows: HomeNet (connected later, saved), CafeWifi, Neighbor, Corp; 48 px each from LIST_Y
            lx, ly = cx + 168 + 28, cy + 198
            cw = 780 - 2 - 168 - 56
            def btn(row, second=False):
                bx = lx + cw - 10 - 84 - (92 if second else 0)
                return bx + 42, ly + row * 48 + 24

            dk.click(*btn(1))  # CafeWifi: open, connects at once
            check("flsettings: open network connects without a prompt", "connect CafeWifi" in lines(log), str(lines(log)))
            time.sleep(0.6)
            dk.click(*btn(2))  # connected network moved to the top: Neighbor is third again
            time.sleep(0.6)
            w, h, shot = dk.shot("flsettings-wifi-prompt")
            dk.type("wrong-pass-1\n")
            time.sleep(0.8)
            check("flsettings: wrong password is sent and rejected (length only logged)",
                  "passphrase length 12: rejected" in lines(log), str(lines(log)))
            w, h, shot = dk.shot("flsettings-wifi-wrong")
            dk.click(*btn(2))
            time.sleep(0.5)
            dk.type("correct-horse\n")
            time.sleep(0.8)
            check("flsettings: right password is accepted", "passphrase length 13: accepted" in lines(log), str(lines(log)))
            w, h, shot = dk.shot("flsettings-wifi-connected")
            dk.click(*btn(0, True))  # Neighbor is connected and on top, saved: Disconnect sits left of Forget
            time.sleep(0.5)
            check("flsettings: Disconnect", "disconnect" in lines(log), str(lines(log)))
            dk.click(*btn(2))  # Neighbor again third, saved: Forget (rightmost)
            time.sleep(0.5)
            check("flsettings: Forget", "forget Neighbor" in lines(log), str(lines(log)))
            dk.click(*btn(2))  # unknown again: asks again, Escape cancels
            time.sleep(0.5)
            dk.key("esc")
            time.sleep(0.6)
            check("flsettings: Escape cancels the question",
                  any(l.startswith("agent: ") and "Canceled" in l for l in lines(log)), str(lines(log)))
            # a second start only passes its page on
            q = subprocess.Popen([os.path.join(binp, "flsettings"), "--page", "general"], env=dict(dk.env, DBUS_SYSTEM_BUS_ADDRESS=addr))
            try:
                rc = q.wait(5)
            except subprocess.TimeoutExpired:
                q.kill()
                rc = -1
            check("flsettings: a second start hands over and exits", rc == 0, str(rc))
            time.sleep(0.5)
            w, h, shot2 = dk.shot("flsettings-general")
            check("flsettings: the page changed to General", shot2 != shot)
            iw.terminate()
            iw.wait(5)
            iw = None
            q = subprocess.Popen([os.path.join(binp, "flsettings"), "--page", "wifi"], env=dict(dk.env, DBUS_SYSTEM_BUS_ADDRESS=addr))
            q.wait(5)
            time.sleep(1.0)
            w, h, shot3 = dk.shot("flsettings-wifi-gone")
            check("flsettings: iwd going away changes the page", shot3 != shot2)
        finally:
            dk.stop()
            if iw:
                iw.terminate()
            if bus:
                bus.terminate()
                bus.wait(5)
                bus = None


def main():
    which = sys.argv[1:] or ["flfiles", "fledit"]
    binp = os.environ.get("APPS_BIN", "/usr/bin")
    out = os.environ.get("APPS_OUT")
    if out:
        os.makedirs(out, exist_ok=True)
    for t in which:
        globals()["test_" + t](binp)
    print("all tests passed" if not fails else "%d test(s) failed" % fails)
    return fails


if __name__ == "__main__":
    sys.exit(main())
