#!/usr/bin/env python3
# Accent colour and wallpaper in a headless desktop: click a swatch in Settings, see Settings and
# flcomp's wallpaper change colour at once, import a picture, see it become the wallpaper.
# Reuses the Desktop harness of flcomp/tests/apps.py. Environment:
#   FLCOMP=program   flcomp (a wrapper script that runs the host-built binary is fine)
#   FLSETTINGS=program, FLCOMP_SHARE=dir with wallpaper.flw and wallpaper-NAME.flw
#   ACCENT_OUT=dir   write PNG screenshots there (accent-*.png, wallpaper-*.png)
#   ACCENT_IMG=dir   a scratch dir for the test picture
import importlib.util, os, signal, struct, subprocess, sys, time, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("apps", os.path.join(HERE, "..", "..", "flcomp", "tests", "apps.py"))
apps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(apps)
W, H = apps.W, apps.H
SETTINGS = os.environ.get("FLSETTINGS", "flsettings")
OUT = os.environ.get("ACCENT_OUT")
IMG = os.environ.get("ACCENT_IMG", "/tmp")
fails = 0


def check(name, cond, why=""):
    global fails
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else ": " + why), flush=True)
    if not cond:
        fails += 1


def png_test_image(path):
    # 600 x 300: top third 802020, middle 208020, bottom 2040a0 (solid bands)
    w, h = 600, 300
    rows = b""
    for y in range(h):
        c = (0x80, 0x20, 0x20) if y < 100 else (0x20, 0x80, 0x20) if y < 200 else (0x20, 0x40, 0xa0)
        rows += b"\x00" + bytes(c) * w
    def chunk(t, b):
        x = struct.pack(">I", len(b)) + t + b
        return x + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def px(d, x, y):
    o = (y * W + x) * 3
    return d[o], d[o + 1], d[o + 2]


def glow_hue(d):
    """average colour of the bright pixels around the F (the wallpaper's logo is at the centre)"""
    r = g = b = n = 0
    for y in range(H // 2 - 110, H // 2 + 60):
        for x in range(W // 2 - 90, W // 2 + 90):
            p = px(d, x, y)
            if max(p) > 110:
                r += p[0]; g += p[1]; b += p[2]; n += 1
    return (r // max(n, 1), g // max(n, 1), b // max(n, 1), n)


def find(d, rgb, x0=0, y0=0, x1=None, y1=None):
    want = bytes(rgb)
    for y in range(y0, y1 or H):
        row = d[y * W * 3:(y + 1) * W * 3]
        i = row.find(want, (x0) * 3)
        while i >= 0 and i % 3:
            i = row.find(want, i + 1)
        if i >= 0 and (x1 is None or i // 3 < x1):
            return i // 3, y
    return None


os.makedirs(IMG, exist_ok=True)
# Settings and flcomp go by XDG_CONFIG_HOME / XDG_DATA_HOME when they are set: point them at scratch
# directories, never at the real ones
import tempfile
scratch = tempfile.mkdtemp(prefix="accent-")
os.environ["XDG_CONFIG_HOME"] = os.path.join(scratch, "config")
os.environ["XDG_DATA_HOME"] = os.path.join(scratch, "data")
os.makedirs(os.environ["XDG_CONFIG_HOME"] + "/freelfs")
dk = apps.Desktop()
try:
    conf = os.path.join(scratch, "config", "freelfs", "desktop.conf")
    time.sleep(0.5)
    w, h, d = dk.shot()
    if OUT:
        apps.write_png(os.path.join(OUT, "wallpaper-1-green.png"), w, h, d)
    r, g, b, n = glow_hue(d)
    check("the wallpaper starts green (%d,%d,%d)" % (r, g, b), n > 200 and g > r + 20 and g > b + 10)

    dk.start([SETTINGS], FLCOMP_SHARE=os.environ["FLCOMP_SHARE"])
    time.sleep(0.5)
    w, h, d = dk.shot("accent-1-green" if False else None)
    ox, oy = dk.origin(d)
    # the swatches sit in a row on the right of "Accent colour"; the blue one has the blue accent colour
    sw = find(d, (0x4d, 0x8f, 0xd0), ox, oy)
    check("the blue swatch is on the General page", sw is not None)
    if OUT:
        apps.write_png(os.path.join(OUT, "accent-1-green.png"), w, h, d)
    # the selected sidebar item is filled with the green selection colour
    check("Settings is green to begin with (selection fill 2f4a3b)", find(d, (0x2f, 0x4a, 0x3b), ox, oy) is not None)
    dk.click(sw[0] + 8, sw[1] + 8)
    time.sleep(0.8)
    w, h, d = dk.shot()
    if OUT:
        apps.write_png(os.path.join(OUT, "accent-2-blue.png"), w, h, d)
    cf = open(conf).read() if os.path.exists(conf) else ""
    check("desktop.conf has accent=blue", "accent=blue" in cf.split("\n"), cf)
    # theme: panel 272c33 mixed 70/255 with 4d8fd0
    sel = tuple(round(a + (b2 - a) * 70 / 255) for a, b2 in zip((0x27, 0x2c, 0x33), (0x4d, 0x8f, 0xd0)))
    check("Settings itself turned blue at once (selection fill %02x%02x%02x)" % sel, find(d, sel, ox, oy) is not None)
    check("the green selection fill is gone", find(d, (0x2f, 0x4a, 0x3b), ox, oy) is None)

    # go back to green and blue again through the file (what the panel and other apps would see)
    dk.procs[-1].terminate()
    dk.procs[-1].wait(5)
    time.sleep(0.8)
    w, h, d = dk.shot()
    if OUT:
        apps.write_png(os.path.join(OUT, "wallpaper-2-blue-follows-accent.png"), w, h, d)
    r, g, b, n = glow_hue(d)
    check("the wallpaper turned blue with the accent, no restart (%d,%d,%d)" % (r, g, b), n > 200 and b > r + 30 and b > g)

    for acc, test in (("orange", lambda r, g, b: r > b + 30 and r > g), ("rose", lambda r, g, b: r > g + 30 and b > g)):
        with open(conf, "w") as f:
            f.write("accent=%s\n" % acc)
        time.sleep(0.8)
        w, h, d = dk.shot()
        if OUT:
            apps.write_png(os.path.join(OUT, "wallpaper-3-%s.png" % acc), w, h, d)
        r, g, b, n = glow_hue(d)
        check("accent=%s: the wallpaper follows (%d,%d,%d)" % (acc, r, g, b), n > 200 and test(r, g, b))
    with open(conf, "w") as f:
        f.write("accent=blue\nwallpaper=green\n")
    time.sleep(0.8)
    w, h, d = dk.shot()
    r, g, b, n = glow_hue(d)
    check("wallpaper=green pins the green one while the accent is blue (%d,%d,%d)" % (r, g, b), n > 200 and g > r + 20)

    # an imported picture
    img = os.path.join(IMG, "bands.png")
    png_test_image(img)
    env = dict(dk.env, FLCOMP_SHARE=os.environ["FLCOMP_SHARE"])
    r_ = subprocess.run([SETTINGS, "--set-wallpaper", img], env=env, capture_output=True, text=True, timeout=30)
    check("flsettings --set-wallpaper converts the PNG", r_.returncode == 0, r_.stderr)
    cf = open(conf).read()
    check("desktop.conf says wallpaper=user:bands", "wallpaper=user:bands" in cf.split("\n"), cf)
    check("the .flw is in the user's wallpaper folder", os.path.exists(os.path.join(scratch, "data/freelfs/wallpapers/bands.flw")))
    time.sleep(1.0)
    w, h, d = dk.shot()
    if OUT:
        apps.write_png(os.path.join(OUT, "wallpaper-4-imported.png"), w, h, d)
    top, mid, bot = px(d, W // 2, 60), px(d, W // 2, H // 2), px(d, W // 2, H - 60)
    near = lambda p, q: all(abs(a - b2) <= 6 for a, b2 in zip(p, q))
    check("the imported picture is the wallpaper (top, middle, bottom bands) %s %s %s" % (top, mid, bot),
          near(top, (0x80, 0x20, 0x20)) and near(mid, (0x20, 0x80, 0x20)) and near(bot, (0x20, 0x40, 0xa0)))
    # a broken request leaves everything as it was
    r_ = subprocess.run([SETTINGS, "--set-wallpaper", "/etc/passwd"], env=env, capture_output=True, text=True, timeout=30)
    check("a text file is refused with a message", r_.returncode == 1 and "PNG and JPEG" in r_.stderr, r_.stderr)
    check("and the wallpaper stays", "wallpaper=user:bands" in open(conf).read().split("\n"))
    # accent again: back to following it
    with open(conf, "w") as f:
        f.write("wallpaper=accent\naccent=teal\n")
    time.sleep(1.0)
    w, h, d = dk.shot()
    r, g, b, n = glow_hue(d)
    check("wallpaper=accent with teal: green and blue both strong (%d,%d,%d)" % (r, g, b), n > 200 and g > r + 30 and b > r + 30)
finally:
    dk.stop()
    import shutil
    shutil.rmtree(scratch, ignore_errors=True)
sys.exit(1 if fails else 0)
