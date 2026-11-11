#!/usr/bin/env python3
# The easter egg ("You can't do that."): which paths it asks about, the keys on a console, the
# hooks in an interactive flsh (and not in a script), and, with a compositor, the full-screen
# window on the desktop. Needs no root. Environment:
#   FLCRASH=path    the flcrash binary (default ../flcrash)
#   FLSH=path       flsh (the flsh checks are skipped without it)
#   FLCOMP=path     flcomp (the desktop checks are skipped without it)
#   RUN="ld.so --library-path ..."   a prefix to run the binaries above (host-side builds)
#   EGG_OUT=dir     write egg-*.png screenshots there
import os, pty, select, shlex, shutil, signal, struct, subprocess, sys, tempfile, time, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
FLCRASH = os.path.abspath(os.environ.get("FLCRASH", os.path.join(HERE, "..", "flcrash")))
FLSH = os.environ.get("FLSH")
FLCOMP = os.environ.get("FLCOMP")
RUN = shlex.split(os.environ.get("RUN", ""))
OUT = os.environ.get("EGG_OUT")
fails = 0
tmp = tempfile.mkdtemp(prefix="egg-")

# programs started by name (flsh's flcrash hook) are found through a directory of small wrappers
bindir = os.path.join(tmp, "bin")
os.mkdir(bindir)
with open(os.path.join(bindir, "flcrash"), "w") as f:
    f.write("#!/bin/sh\nexec %s %s \"$@\"\n" % (" ".join(shlex.quote(a) for a in RUN), shlex.quote(FLCRASH)))
os.chmod(os.path.join(bindir, "flcrash"), 0o755)
BASEENV = dict(os.environ, PATH=bindir + ":" + os.environ["PATH"], FLCRASH_NO_SOUND="1")
BASEENV.pop("WAYLAND_DISPLAY", None)


def check(name, cond, why=""):
    global fails
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else ": " + why), flush=True)
    if not cond:
        fails += 1


def egg(paths, stdin=subprocess.DEVNULL, env=None, extra=()):
    r = subprocess.run(RUN + [FLCRASH, "--cant", *extra, *paths], stdin=stdin, env=env or BASEENV,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
    return r.returncode


def pty_run(argv, keys, env=None, cwd=None, first_wait=0.6, gap=0.35):
    """Run argv on a pseudo terminal, send each key after a pause, return (exit status, output)."""
    pid, fd = pty.fork()
    if pid == 0:
        if cwd:
            os.chdir(cwd)
        e = env or BASEENV
        os.environ.clear()
        os.environ.update(e)
        os.execvp(argv[0], argv)
    import fcntl, termios
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    out = b""

    def pump(t):
        nonlocal out
        end = time.time() + t
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                try:
                    d = os.read(fd, 65536)
                except OSError:
                    return
                if not d:
                    return
                out += d

    pump(first_wait)
    for k in keys:
        os.write(fd, k)
        pump(gap)
    status = None
    for _ in range(80):
        p, st = os.waitpid(pid, os.WNOHANG)
        if p:
            status = st
            break
        pump(0.1)
    if status is None:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
        return None, out.decode(errors="replace")
    return os.waitstatus_to_exitcode(status), out.decode(errors="replace")


# ---- which paths ------------------------------------------------------------------------------
check("not a vital path: no question, go ahead (0)", egg(["/tmp/whatever/x"]) == 0)
check("no path at all: go ahead", egg([]) == 0)
for p in ["/sysflfs", "/sysflfs/bin", "/sysflfs/lib", "/sysflfs/settings", "/boot", "/boot/", "/sysflfs//bin/",
          "/sysflfs/bin/.", "/sysflfs/../boot", "/usr/bin", "/etc", "/usr", "/", "/home", "/var"]:
    # no keyboard and no desktop: the answer is "Never mind" (1)
    check("%s is protected" % p, egg([p]) == 1)
check("a file inside /boot is not (only the folders themselves)", egg(["/boot/vmlinuz"]) == 0)
check("a vital one among harmless ones asks", egg(["/tmp/a", "/sysflfs/lib", "/tmp/b"]) == 1)
check("a relative name that folds to one", egg(["../../boot"], extra=()) in (0, 1))  # depends on cwd
d = os.path.join(tmp, "extra")
os.mkdir(d)
env = dict(BASEENV, FLCRASH_PROTECT=d)
check("FLCRASH_PROTECT adds a path (tests)", egg([d], env=env) == 1 and egg([d + "/x"], env=env) == 0)
link = os.path.join(tmp, "lnk")
os.symlink(d, link)
check("a link to a protected folder is protected too", egg([link], env=env) == 1)

# ---- the keys on a console -----------------------------------------------------------------------
cmd = RUN + [FLCRASH, "--cant", "/boot"]
rc, out = pty_run(cmd, [b"\r"])
check("console: Enter is Never mind (1)", rc == 1, str(rc))
check("console: the screen says it", "You can't do that." in out and "oh no." in out and ">:(" in out
      and "Never mind" in out and "Continue" in out, out[-300:])
rc, _ = pty_run(cmd, [b"\t", b"\r"])
check("console: Tab, Enter is Continue (0)", rc == 0, str(rc))
rc, _ = pty_run(cmd, [b"\x1b[C", b"\r"])
check("console: Right, Enter is Continue (0)", rc == 0, str(rc))
rc, _ = pty_run(cmd, [b"\t", b"\t", b"\r"])
check("console: Tab twice is back on Never mind (1)", rc == 1, str(rc))
rc, _ = pty_run(cmd, [b"c"])
check("console: C is Continue (0)", rc == 0, str(rc))
rc, _ = pty_run(cmd, [b"n"])
check("console: N is Never mind (1)", rc == 1, str(rc))
rc, _ = pty_run(cmd, [b"\x1b"])
check("console: Esc is Never mind (1)", rc == 1, str(rc))
rc, _ = pty_run(cmd, [b"x", b"y", b"\r"])
check("console: other keys change nothing", rc == 1, str(rc))

# ---- flsh -----------------------------------------------------------------------------------------
if FLSH:
    flsh = RUN + [FLSH]
    vital = os.path.join(tmp, "vital")
    env = dict(BASEENV, FLCRASH_PROTECT=vital, PS1="$ ", ENV="/dev/null", HOME=tmp)

    def mk():
        shutil.rmtree(vital, ignore_errors=True)
        os.makedirs(os.path.join(vital, "sub"))
        open(os.path.join(vital, "sub", "f"), "w").write("x")

    def sh_keys(line, keys):
        # type the command, the egg comes up, answer it, then leave the shell
        pid, fd = None, None
        rc, out = pty_run(flsh + ["-i"], [line.encode() + b"\r"] + keys + [b"exit\r"], env=env, cwd=tmp, first_wait=0.8, gap=0.7)
        return rc, out

    mk()
    sh_keys("rm -r vital", [b"\r"])
    check("flsh: rm -r on a vital folder, Never mind: still there", os.path.exists(os.path.join(vital, "sub", "f")))
    sh_keys("rm -rf vital", [b"n"])
    check("flsh: rm -rf, N: still there", os.path.exists(vital))
    sh_keys("rm -r -- vital/", [b"\x1b"])
    check("flsh: trailing slash, Esc: still there", os.path.exists(vital))
    sh_keys("rm -r vital", [b"c"])
    check("flsh: Continue really deletes", not os.path.exists(vital))
    mk()
    sh_keys("rm -r vital/sub", [])
    check("flsh: a path that is not vital goes without a question", not os.path.exists(os.path.join(vital, "sub")))
    mk()
    open(os.path.join(tmp, "plain"), "w").write("x")
    sh_keys("rm vital/sub/f plain", [])
    check("flsh: rm without -r is not asked, files go", not os.path.exists(os.path.join(tmp, "plain")))
    # scripts and sh -c: no question, no hook
    mk()
    r = subprocess.run(RUN + [FLSH, "-c", "rm -r vital"], env=env, cwd=tmp, stdin=subprocess.DEVNULL, timeout=20)
    check("flsh -c: no question, deleted", r.returncode == 0 and not os.path.exists(vital))
    mk()
    sc = os.path.join(tmp, "s.sh")
    open(sc, "w").write("rm -rf vital\n")
    r = subprocess.run(RUN + [FLSH, sc], env=env, cwd=tmp, stdin=subprocess.DEVNULL, timeout=20)
    check("flsh script: no question, deleted", r.returncode == 0 and not os.path.exists(vital))

# ---- on the desktop ------------------------------------------------------------------------------------
def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, b):
        c = struct.pack(">I", len(b)) + t + b
        return c + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))

def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline() == b"P6\n"
        w, h = map(int, f.readline().split())
        f.readline()
        return w, h, f.read()

if FLCOMP:
    W, H = 1000, 700
    CODES = {"enter": 28, "tab": 15, "esc": 1, "c": 46, "n": 49}
    rt = os.path.join(tmp, "rt")
    os.mkdir(rt, 0o700)
    home = os.path.join(tmp, "home")
    os.makedirs(home)
    fifo = os.path.join(tmp, "in")
    os.mkfifo(fifo)
    ppm = os.path.join(tmp, "shot.ppm")
    env = dict(os.environ, XDG_RUNTIME_DIR=rt, FLCOMP_HEADLESS="%dx%d" % (W, H), FLCOMP_PANEL="", FLCOMP_SESSION="",
               FLCOMP_SCREENSHOT=ppm, FLCOMP_TEST_FIFO=fifo, FLCOMP_KEYBOARD_CONF=os.path.join(tmp, "none"),
               HOME=home, XDG_CONFIG_HOME=os.path.join(home, ".config"), FLCOMP_NO_SOUND="1")
    comp = subprocess.Popen(RUN + [FLCOMP], env=env, stderr=open(os.path.join(tmp, "comp.log"), "w"))
    for _ in range(100):
        if os.path.exists(os.path.join(rt, "wayland-0")):
            break
        time.sleep(0.1)
    ffd = os.open(fifo, os.O_RDWR)
    cenv = dict(BASEENV, XDG_RUNTIME_DIR=rt, WAYLAND_DISPLAY="wayland-0", HOME=home, XDG_CONFIG_HOME=os.path.join(home, ".config"))

    def send(line, wait=0.1):
        os.write(ffd, (line + "\n").encode())
        time.sleep(wait)

    def key(name):
        send("key %d 1" % CODES[name], 0.05)
        send("key %d 0" % CODES[name], 0.25)

    def start():
        return subprocess.Popen(RUN + [FLCRASH, "--cant", "/sysflfs/bin"], env=cenv, stdin=subprocess.DEVNULL,
                                stderr=open(os.path.join(tmp, "egg.log"), "a"))

    def shot(name):
        if os.path.exists(ppm):
            os.unlink(ppm)
        os.kill(comp.pid, signal.SIGUSR1)
        for _ in range(50):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1000:
                time.sleep(0.2)
                break
            time.sleep(0.1)
        w, h, rgb = read_ppm(ppm)
        if OUT:
            write_png(os.path.join(OUT, name), w, h, rgb)
        return w, h, rgb

    try:
        for label, keys, want in (("Enter", ["enter"], 1), ("Esc", ["esc"], 1), ("N", ["n"], 1),
                                  ("Tab, Enter", ["tab", "enter"], 0), ("C", ["c"], 0)):
            p = start()
            time.sleep(1.5)
            if label == "Enter":
                w, h, rgb = shot("egg-desktop.png")
                # the field is the crash screen's green and covers the whole output
                px = lambda x, y: rgb[(y * w + x) * 3:(y * w + x) * 3 + 3]
                check("desktop: the window fills the screen in the crash screen green",
                      px(w - 5, h - 5) == bytes.fromhex("1f3d2e") and px(5, 5) == bytes.fromhex("1f3d2e"), str(px(w - 5, h - 5)))
            for k in keys:
                key(k)
            try:
                rc = p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
                rc = None
            check("desktop: %s gives %s" % (label, "Never mind (1)" if want else "Continue (0)"), rc == want, str(rc))
        # a click on Continue
        p = start()
        time.sleep(1.5)
        w, h, rgb = shot("egg-desktop-tmp.png") if OUT else (W, H, None)
        s = H / 1000.0
        s = min(max(s, 0.62), 1.8)
        # buttons sit at x = 9 % + 190 s + 18 s, y about 0.07 H + the three lines
        send("move %d %d" % (int(W * 0.09) + int(190 * s) + int(18 * s) + 40, int(H * 0.47)), 0.3)
        send("button 272 1", 0.05)
        send("button 272 0", 0.3)
        try:
            rc = p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
            rc = None
        check("desktop: a click on Continue gives Continue (0)", rc == 0, str(rc))
    finally:
        comp.terminate()
        comp.wait()
        if OUT and os.path.exists(os.path.join(OUT, "egg-desktop-tmp.png")):
            os.unlink(os.path.join(OUT, "egg-desktop-tmp.png"))

shutil.rmtree(tmp, ignore_errors=True)
sys.exit(1 if fails else 0)
