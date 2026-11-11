#!/usr/bin/env python3
# Headless desktop tests for flarchive: browse and extract zip, tar.gz, tar.zst, 7z, rar, a single
# .gz; create zip and tar.zst; progress, cancel, unsafe paths, encrypted archives. Starts flcomp
# headless (src/flcomp/tests/apps.py's Desktop), runs the real program, checks what ends up on disk
# and writes screenshots (PNG) when APPS_OUT is set. Samples are made with the Python standard
# library, 7z and zstd when they are installed (those tests are skipped otherwise).
# SPDX-License-Identifier: MIT
#
#   FLARCHIVE=/path/to/flarchive FLCOMP=/path/to/flcomp APPS_OUT=docs/m7 python3 -I flarchive_test.py
#
# HOME and the XDG directories are a scratch directory.
import io, lzma, os, shutil, struct, subprocess, sys, tarfile, time, zipfile, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "flcomp", "tests"))
import apps  # noqa: E402

apps.CODES.update({"lbracket": 26, "rbracket": 27, "backslash": 43, "grave": 41})
check = apps.check


def type_text(dk, text):
    plain = {" ": "space", "-": "minus", ".": "dot", "/": "slash", ",": "comma", "=": "equal", "'": "apostrophe"}
    shift = {"_": "minus", ":": "semicolon"}
    for ch in text:
        if ch == "\n":
            dk.key("enter")
        elif ch.isupper():
            dk.key(ch.lower(), "shift")
        elif ch in shift:
            dk.key(shift[ch], "shift")
        else:
            dk.key(plain.get(ch, ch))


def win_origin(d, W=1000):
    """top-left of the first window: the first pixel of the green line along the top of the title bar
    (apps.Desktop.origin compares with the corner pixel, which a window at 0,0 covers)"""
    accent = bytes([0x3f, 0x9b, 0x63])
    i = d.find(accent)
    while i >= 0 and i % 3:
        i = d.find(accent, i + 1)
    return (i // 3 % W, i // 3 // W) if i >= 0 else None


def start(dk, argv, wait=True):
    """start the program; unless wait is off, wait (up to 10 s) until its window is on screen"""
    p = dk.start(argv)
    for _ in range(50 if wait else 0):
        w, h, d = dk.shot()
        if win_origin(d):
            break
        time.sleep(0.2)
    return p


def wait_until(fn, secs=10):
    for _ in range(int(secs * 50)):
        if fn():
            return True
        time.sleep(0.02)
    return False


def vint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def rar5(name, data):
    """a RAR5 archive with one stored file, built by hand (no rar tool needed)"""
    def block(btype, flags, body, data_size=None):
        h = vint(btype) + vint(flags)
        if data_size is not None:
            h += vint(data_size)
        h += body
        sz = vint(len(h))
        crc = struct.pack("<I", zlib.crc32(sz + h) & 0xFFFFFFFF)
        return crc + sz + h
    main = block(1, 0, vint(0))
    nm = name.encode()
    fbody = vint(0x04) + vint(len(data)) + vint(0o100644) + struct.pack("<I", zlib.crc32(data) & 0xFFFFFFFF) + vint(0) + vint(1) + vint(len(nm)) + nm
    f = block(2, 0x02, fbody, data_size=len(data)) + data
    end = block(5, 0, vint(0))
    return b"\x52\x61\x72\x21\x1a\x07\x01\x00" + main + f + end


def make_samples(d):
    os.makedirs(d)
    tree = {
        "project/README.txt": b"Archives test project\n",
        "project/src/main.c": b"int main(void) { return 0; }\n",
        "project/src/util.h": b"#pragma once\n",
        "project/docs/guide.md": b"# Guide\n" + b"text\n" * 200,
        "project/data/blob.bin": os.urandom(150000),
        "project/photos/a.jpg": os.urandom(30000),
        "project/photos/b.jpg": os.urandom(40000),
    }
    with zipfile.ZipFile(os.path.join(d, "project.zip"), "w", zipfile.ZIP_DEFLATED) as z:
        for k, v in tree.items():
            z.writestr(k, v)
    with tarfile.open(os.path.join(d, "project.tar.gz"), "w:gz") as t:
        for k, v in tree.items():
            ti = tarfile.TarInfo(k)
            ti.size = len(v)
            ti.mtime = 1700000000
            t.addfile(ti, io.BytesIO(v))
    # several top-level items: extracted into a folder named after the archive
    with zipfile.ZipFile(os.path.join(d, "loose.zip"), "w") as z:
        z.writestr("one.txt", b"1\n")
        z.writestr("two.txt", b"2\n")
        z.writestr("sub/three.txt", b"3\n")
    # a single file in a tar.xz: goes next to the archive as it is
    with tarfile.open(os.path.join(d, "single.tar.xz"), "w:xz") as t:
        ti = tarfile.TarInfo("notes.txt")
        ti.size = 6
        t.addfile(ti, io.BytesIO(b"notes\n"))
    # entries that try to leave the folder
    with tarfile.open(os.path.join(d, "evil.tar"), "w") as t:
        for n in ("fine.txt", "../escaped.txt", "/abs-escaped.txt", "a/../../escaped2.txt"):
            ti = tarfile.TarInfo(n)
            ti.size = 4
            t.addfile(ti, io.BytesIO(b"evil"))
    # a .gz that is one compressed file
    import gzip
    with gzip.open(os.path.join(d, "readme.txt.gz"), "wb") as g:
        g.write(b"compressed readme\n" * 20)
    # a big one (stored random data, 480 MB) that takes a moment to unpack
    with zipfile.ZipFile(os.path.join(d, "big.zip"), "w", zipfile.ZIP_STORED) as z:
        for i in range(8):
            z.writestr("big/part%d.bin" % i, os.urandom(60 * 1024 * 1024))
    write(os.path.join(d, "fake.zip"), b"this is not an archive at all\n" * 10)
    write(os.path.join(d, "test.rar"), rar5("rar-hello.txt", b"hello from rar\n"))
    have = {"zstd": shutil.which("zstd"), "7z": shutil.which("7z") or shutil.which("7za")}
    src = os.path.join(d, "src")
    for k, v in tree.items():
        p = os.path.join(src, k)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        write(p, v)
    if have["zstd"] and shutil.which("tar"):
        subprocess.run(["tar", "-C", src, "--zstd", "-cf", os.path.join(d, "project.tar.zst"), "project"], check=True)
    if have["7z"]:
        subprocess.run([have["7z"], "a", "-bd", "-bso0", "-bsp0", os.path.join(d, "project.7z"), os.path.join(src, "project")],
                       check=True, cwd=d)
        subprocess.run([have["7z"], "a", "-tzip", "-bd", "-bso0", "-bsp0", "-psecret", os.path.join(d, "locked.zip"),
                        os.path.join(src, "project", "README.txt")], check=True, cwd=d)
    return have


def write(path, data):
    with open(path, "wb") as f:
        f.write(data)


def tree_of(root):
    out = {}
    for dp, dn, fn in os.walk(root):
        for f in fn:
            p = os.path.join(dp, f)
            out[os.path.relpath(p, root)] = open(p, "rb").read()
    return out


def run_to_exit(p, secs=20):
    for _ in range(int(secs * 10)):
        if p.poll() is not None:
            return True
        time.sleep(0.1)
    return False


def test(prog):
    dk = apps.Desktop()
    home = dk.home
    dk.env.update(XDG_STATE_HOME=os.path.join(home, ".local", "state"), XDG_CONFIG_HOME=os.path.join(home, ".config"),
                  XDG_DATA_HOME=os.path.join(home, ".local", "share"), XDG_CACHE_HOME=os.path.join(home, ".cache"))
    work = os.path.join(home, "w")
    have = make_samples(work)
    P = lambda n: os.path.join(work, n)
    ref = tree_of(os.path.join(work, "src"))
    p = None
    try:
        # ---- browse a zip
        p = start(dk, [prog, P("project.zip")])
        time.sleep(0.6)
        dk.shot("archive-1-browse")
        w, h, d = dk.shot()
        ox, oy = win_origin(d)
        cx, cy = ox + 1, oy + 36
        # into the folder "src": rows are folders first, A to Z: data, docs, photos, src ... (project/ is the only top row)
        dk.click(cx + 120, cy + 112 + 15, count=2)  # project/
        dk.shot("archive-2-folder")
        check("archive: still running after browsing", p.poll() is None)
        # ---- extract all with Ctrl+E (prompt, then Enter)
        dk.key("e", "ctrl")
        time.sleep(0.3)
        dk.shot("archive-3-extract-prompt")
        dk.key("a", "ctrl")
        type_text(dk, os.path.join(work, "out1") + "/")
        dk.key("enter")
        wait_until(lambda: os.path.isdir(os.path.join(work, "out1", "project")) and os.path.exists(os.path.join(work, "out1", "project", "README.txt")))
        time.sleep(0.5)
        dk.shot("archive-4-extracted")
        got = tree_of(os.path.join(work, "out1", "project"))
        want = {k[len("project/"):]: v for k, v in ref.items()}
        check("archive: zip extracted completely, single top folder kept", got == want, "%d files vs %d" % (len(got), len(want)))
        # again: nothing is overwritten, the new copy gets a free name
        dk.key("e", "ctrl")
        time.sleep(0.3)
        dk.key("a", "ctrl")
        type_text(dk, os.path.join(work, "out1") + "/")
        dk.key("enter")
        wait_until(lambda: os.path.isdir(os.path.join(work, "out1", "project 2")))
        check("archive: a second extract picks a free name", os.path.isdir(os.path.join(work, "out1", "project 2")),
              str(os.listdir(os.path.join(work, "out1"))))
        p.terminate()
        p.wait(5)

        # ---- extract here (the flfiles command): several formats in one go
        for name, expect in (("project.tar.gz", "project"), ("loose.zip", "loose"), ("single.tar.xz", "notes.txt"),
                             ("readme.txt.gz", "readme.txt")):
            sub = os.path.join(work, "here-" + name)
            os.makedirs(sub)
            shutil.copy(P(name), sub)
            p = start(dk, [prog, "--extract-here", os.path.join(sub, name)], wait=False)
            if name == "project.tar.gz":
                dk.shot("archive-5-extract-here")
            done = run_to_exit(p)
            names = sorted(os.listdir(sub))
            check("archive: --extract-here %s -> %s, window closed by itself" % (name, expect), done and expect in names, str(names))
        loose = tree_of(os.path.join(work, "here-loose.zip", "loose"))
        check("archive: several top-level items go into a folder named after the archive",
              set(loose) == {"one.txt", "two.txt", "sub/three.txt"}, str(set(loose)))
        check("archive: tar.gz content equals the original", tree_of(os.path.join(work, "here-project.tar.gz", "project")) == want)
        if os.path.exists(P("project.tar.zst")):
            sub = os.path.join(work, "here-zst")
            os.makedirs(sub)
            shutil.copy(P("project.tar.zst"), sub)
            p = start(dk, [prog, "--extract-here", os.path.join(sub, "project.tar.zst")], wait=False)
            run_to_exit(p)
            check("archive: tar.zst extracted", tree_of(os.path.join(sub, "project")) == want)
        else:
            print("     (skipped tar.zst: no zstd or tar)")
        if os.path.exists(P("project.7z")):
            sub = os.path.join(work, "here-7z")
            os.makedirs(sub)
            shutil.copy(P("project.7z"), sub)
            p = start(dk, [prog, "--extract-here", os.path.join(sub, "project.7z")], wait=False)
            run_to_exit(p)
            check("archive: 7z extracted", tree_of(os.path.join(sub, "project")) == want)
            p = start(dk, [prog, P("project.7z")])
            time.sleep(0.5)
            dk.shot("archive-6-7z")
            p.terminate()
            p.wait(5)
        else:
            print("     (skipped 7z: no 7z)")
        sub = os.path.join(work, "here-rar")
        os.makedirs(sub)
        shutil.copy(P("test.rar"), sub)
        p = start(dk, [prog, "--extract-here", os.path.join(sub, "test.rar")], wait=False)
        run_to_exit(p)
        rar_ok = os.path.exists(os.path.join(sub, "rar-hello.txt")) and open(os.path.join(sub, "rar-hello.txt"), "rb").read() == b"hello from rar\n"
        check("archive: rar (hand-made RAR5, stored) extracted", rar_ok, str(os.listdir(sub)))

        # ---- unsafe paths: nothing may leave the destination
        sub = os.path.join(work, "here-evil")
        os.makedirs(sub)
        shutil.copy(P("evil.tar"), sub)
        p = start(dk, [prog, "--extract-here", os.path.join(sub, "evil.tar")], wait=False)
        time.sleep(2.0)
        escaped = [n for n in (os.path.join(work, "escaped.txt"), os.path.join(work, "escaped2.txt"), "/abs-escaped.txt",
                               os.path.join(sub, "escaped.txt")) if os.path.exists(n)]
        check("archive: .. and absolute paths do not escape", not escaped, str(escaped))
        leftovers = [n for n in os.listdir(sub) if n.startswith(".flarchive")]
        check("archive: no temporary folder is left behind", not leftovers, str(leftovers))
        dk.shot("archive-7-unsafe-skipped")
        p.terminate()
        p.wait(5)

        # ---- not an archive, and a password protected one: a plain message
        p = start(dk, [prog, P("fake.zip")])
        time.sleep(0.8)
        dk.shot("archive-8-not-an-archive")
        check("archive: not an archive leaves the window up with a message", p.poll() is None)
        p.terminate()
        p.wait(5)
        if os.path.exists(P("locked.zip")):
            sub = os.path.join(work, "here-locked")
            os.makedirs(sub)
            shutil.copy(P("locked.zip"), sub)
            p = start(dk, [prog, os.path.join(sub, "locked.zip")])
            time.sleep(0.6)
            dk.key("e", "ctrl")
            time.sleep(0.3)
            dk.key("enter")
            time.sleep(1.0)
            dk.shot("archive-9-encrypted")
            check("archive: password protected: nothing extracted", [n for n in os.listdir(sub) if n != "locked.zip"] == [],
                  str(os.listdir(sub)))
            p.terminate()
            p.wait(5)

        # ---- progress and cancel on a big archive: the worker is stopped half way so the picture holds still
        sub = os.path.join(work, "here-big")
        os.makedirs(sub)
        shutil.copy(P("big.zip"), sub)
        p = subprocess.Popen([prog, "--extract-here", os.path.join(sub, "big.zip")], env=dk.env,
                             stderr=open(os.path.join(dk.dir, "app.log"), "a"))  # no pause after the start
        dk.procs.append(p)
        kids = []

        def find_kids():
            try:
                kids[:] = [int(x) for x in open("/proc/%d/task/%d/children" % (p.pid, p.pid)).read().split()]
            except OSError:
                pass
            return bool(kids)
        wait_until(find_kids, 5)
        time.sleep(0.12)
        for k in kids:
            os.kill(k, 19)  # SIGSTOP half way
        time.sleep(0.5)
        check("archive: the work runs in a child process", len(kids) == 1, str(kids))
        dk.shot("archive-10-progress")
        w, h, d = dk.shot()
        ox, oy = win_origin(d)
        dk.click(ox + 260, oy + 239)  # Cancel
        for k in kids:
            os.kill(k, 18)  # SIGCONT: the pending SIGTERM now cleans up
        wait_until(lambda: [n for n in os.listdir(sub) if n.startswith(".flarchive")] == [], 5)
        time.sleep(0.8)
        left = [n for n in os.listdir(sub) if n != "big.zip"]
        check("archive: cancel leaves nothing behind", left == [], str(left))
        dk.shot("archive-10b-cancelled")
        p.terminate()
        p.wait(5)

        # ---- create
        src = os.path.join(work, "src", "project")
        p = start(dk, [prog, "--compress", src, os.path.join(work, "src", "project", "README.txt")])
        time.sleep(0.6)
        dk.shot("archive-11-create")
        w, h, d = dk.shot()
        ox, oy = win_origin(d)
        cx, cy = ox + 1, oy + 36
        dk.click(ox + 760, oy + 505)  # Create (the page is centred; positions are relative to the window)
        time.sleep(1.5)
        z = [n for n in os.listdir(os.path.join(work, "src")) if n.endswith(".zip")]
        check("archive: create zip", z != [], str(os.listdir(os.path.join(work, "src"))))
        if z:
            zf = zipfile.ZipFile(os.path.join(work, "src", z[0]))
            check("archive: the zip tests clean and holds the files", zf.testzip() is None and "project/src/main.c" in zf.namelist(),
                  str(zf.namelist()[:5]))
        dk.shot("archive-12-created")
        p.terminate()
        p.wait(5)
        # tar.zst: pick the format first
        out2 = os.path.join(work, "out2")
        os.makedirs(out2)
        p = start(dk, [prog, "--compress", src])
        time.sleep(0.5)
        w, h, d = dk.shot()
        ox, oy = win_origin(d)
        dk.click(ox + 263, oy + 372)  # the "Tar + Zstd" button
        dk.shot("archive-13-create-zst")
        dk.click(ox + 760, oy + 505)  # Create
        wait_until(lambda: any(n.endswith(".tar.zst") for n in os.listdir(os.path.join(work, "src"))), 8)
        time.sleep(0.5)
        zs = [n for n in os.listdir(os.path.join(work, "src")) if n.endswith(".tar.zst")]
        check("archive: create tar.zst", zs != [], str(os.listdir(os.path.join(work, "src"))))
        if zs and have["zstd"]:
            r = subprocess.run(["tar", "--zstd", "-tf", os.path.join(work, "src", zs[0])], capture_output=True, text=True)
            check("archive: the tar.zst lists with the real tar and holds the files", r.returncode == 0 and "project/src/main.c" in r.stdout, r.stderr[:200])
        dk.shot("archive-14-created-zst")
        p.terminate()
        p.wait(5)
        # ---- the flfiles right-click menu: "Extract here" and "Compress" (FLFILES=path of the built flfiles)
        flfiles = os.environ.get("FLFILES")
        if flfiles:
            fl = os.path.join(work, "fl")
            os.makedirs(fl)
            shutil.copy(P("project.zip"), fl)
            bindir = os.path.dirname(os.path.abspath(prog))
            env = {"PATH": bindir + ":" + os.environ.get("PATH", "")}
            pf = dk.start([flfiles, fl], **env)
            w, h, d = dk.shot()
            ox, oy = win_origin(d)
            cx, cy = ox + 1, oy + 36
            mx, my = cx + 188 + 8 + 54, cy + 44 + 8 + 49
            dk.click(mx, my, button=273)
            dk.shot("archive-15-flfiles-menu")
            dk.click(mx + 60, my + 4 + 5 * 30 + 15)  # Extract here
            ok = wait_until(lambda: os.path.isdir(os.path.join(fl, "project")), 15)
            check("archive: flfiles right click, Extract here", ok, str(os.listdir(fl)))
            dk.key("f5")
            time.sleep(0.4)
            dk.shot("archive-16-flfiles-extracted")
            dk.click(mx, my, button=273)
            dk.click(mx + 60, my + 4 + 6 * 30 + 15)  # Compress
            time.sleep(1.2)
            dk.shot("archive-17-flfiles-compress")
            pf.terminate()
            pf.wait(5)
    finally:
        dk.stop()
    return apps.fails


if __name__ == "__main__":
    prog = os.environ.get("FLARCHIVE", "flarchive")
    out = os.environ.get("APPS_OUT")
    if out:
        os.makedirs(out, exist_ok=True)
    if "FLCOMP" not in os.environ:
        os.environ["FLCOMP"] = os.path.join(HERE, "..", "..", "flcomp", "flcomp")
    rc = test(prog)
    print("all tests passed" if not rc else "%d test(s) failed" % rc)
    sys.exit(1 if rc else 0)
