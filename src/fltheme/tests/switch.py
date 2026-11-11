#!/usr/bin/env python3
# Dark -> light -> dark in a headless desktop: flcomp, the real flpanel, a libflui app (Themes itself)
# and an X11 window (the frame flcomp draws, needs Xwayland: skipped without). After each switch the
# panel, the app's window and the X frame must show the new scheme's colours and none of the old
# one's; then the panel goes to the top, the title bar gets compact and the window buttons move.
# Reuses the Desktop harness of flcomp/tests/apps.py. Everything happens in a scratch HOME.
#
#   FLCOMP=path  FLTHEME=path  APPS_BIN=dir with flpanel  XCLIENT=path to flcomp/tests/xclient
#   SWITCH_OUT=DIR keeps PNG screenshots (fltheme-*.png)
# SPDX-License-Identifier: MIT
import importlib.util, os, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("apps", os.path.join(HERE, "..", "..", "flcomp", "tests", "apps.py"))
apps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(apps)
W, H = apps.W, apps.H
FLTHEME = os.environ.get("FLTHEME", os.path.join(HERE, "..", "fltheme"))
XCLIENT = os.environ.get("XCLIENT", os.path.join(HERE, "..", "..", "flcomp", "tests", "xclient"))
OUT = os.environ.get("SWITCH_OUT")
XWAYLAND = shutil.which("Xwayland")
fails = 0

# the tokens of flpalette.h: bg, panel, raised, line
DARK = dict(bg=(0x1d, 0x21, 0x25), panel=(0x27, 0x2c, 0x33), raised=(0x34, 0x3a, 0x43), line=(0x3a, 0x40, 0x48))
LIGHT = dict(bg=(0xe1, 0xe5, 0xe9), panel=(0xf5, 0xf6, 0xf8), raised=(0xff, 0xff, 0xff), line=(0xc4, 0xca, 0xd2))


def check(name, cond, why=""):
    global fails
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else ": " + why), flush=True)
    if not cond:
        fails += 1


def count(img, rgb, x0, y0, x1, y1, step=2):
    w, h, d = img
    want = bytes(rgb)
    n = 0
    for y in range(max(0, y0), min(h, y1), step):
        row = d[y * w * 3:(y + 1) * w * 3]
        for x in range(max(0, x0), min(w, x1), step):
            if row[x * 3:x * 3 + 3] == want:
                n += 1
    return n


def area(x0, y0, x1, y1, step=2):
    return ((x1 - x0) // step) * ((y1 - y0) // step)


def px(img, x, y):
    w, h, d = img
    o = (y * w + x) * 3
    return tuple(d[o:o + 3])


def shot(dk, name):
    # the scratch screenshot of the desktop, as a PPM
    os.kill(dk.comp.pid, 10)  # SIGUSR1
    time.sleep(0.6)
    img = apps.read_ppm(dk.ppm)
    if OUT:
        apps.write_png(os.path.join(OUT, "fltheme-%s.png" % name), *img)
    return img


def apply(dk, preset):
    p = subprocess.run([FLTHEME, "--apply", preset], env=dk.env, capture_output=True, text=True)
    check("fltheme --apply %s" % preset, p.returncode == 0, p.stderr)
    time.sleep(1.2)


def scheme(img, S, O, name, panel_rows, wx0, wx1, xframe):
    """the panel strip, the Themes window and (when asked) the X frame show scheme S and nothing of O"""
    y0, y1 = panel_rows
    n = area(0, y0, W, y1)
    check("%s: the panel is %s" % (name, "light" if S is LIGHT else "dark"),
          count(img, S["panel"], 0, y0, W, y1) > n * 0.6, "panel colour %d of %d" % (count(img, S["panel"], 0, y0, W, y1), n))
    check("%s: nothing of the old scheme is left in the panel" % name, count(img, O["panel"], 0, y0, W, y1) == 0)
    win = area(wx0, 40, wx1, 600)
    # the old scheme's colours appear only in the preset thumbnails (they draw both schemes); look above them
    top_rows = (40, 270)
    check("%s: the Themes window body is repainted" % name, count(img, S["panel"], wx0, 40, wx1, 600) > win * 0.2)
    check("%s: ... and its sidebar" % name, count(img, S["bg"], wx0, 40, wx0 + 168, 600) > area(wx0, 40, wx0 + 168, 600) * 0.5)
    check("%s: no pixel of the old scheme's body colour in the window" % name, count(img, O["panel"], wx0, top_rows[0], wx1, top_rows[1]) == 0,
          "%d old" % count(img, O["panel"], wx0, top_rows[0], wx1, top_rows[1]))
    check("%s: the desktop behind is %s" % (name, "light" if S is LIGHT else "dark"), count(img, S["bg"], W - 150, 100, W - 10, 300) > 100)
    if xframe:
        x0, y0, x1, y1 = xframe
        check("%s: the frame of the X window follows (title bar)" % name, count(img, S["raised"], x0, y0, x1, y0 + 30) > 100,
              "raised %d, panel %d" % (count(img, S["raised"], x0, y0, x1, y0 + 30), count(img, S["panel"], x0, y0, x1, y0 + 30)))
        check("%s: ... with nothing of the old scheme" % name, count(img, O["raised"], x0, y0, x1, y0 + 30) == 0
              and count(img, O["line"], x0 + 2, y0 + 8, x1 - 2, y0 + 26) == 0)


def first_line_y(img, x, line):
    for y in range(3, 80):
        if px(img, x, y) == tuple(line):
            return y
    return -1


home_cfg = None
if XWAYLAND:
    os.environ["FLCOMP_XWAYLAND"] = XWAYLAND  # headless runs have no X unless asked
os.environ["FLCOMP_WALLPAPER"] = ""  # the desktop is the bg token, not a picture
dk = apps.Desktop(panel=True)
try:
    # the compositor's own colours come from the same file; wallpaper off so the desktop is the bg token
    dk.env["XDG_CONFIG_HOME"] = os.path.join(dk.home, ".config")
    time.sleep(0.5)
    app = dk.start([FLTHEME, "--page", "presets"])
    time.sleep(1.0)
    xframe = None
    xp = None
    if XWAYLAND and os.path.exists(XCLIENT):
        log = open(os.path.join(dk.dir, "comp.log")).read()
        import re
        m = re.search(r"DISPLAY=(:\d+),", log)
        if m:
            xp = subprocess.Popen([XCLIENT, "-t", "xwin", "-w", "220", "-h", "120", "-s", "100"],
                                  env=dict(dk.env, DISPLAY=m.group(1)),
                                  stdout=open(os.path.join(dk.dir, "x.out"), "w"), stderr=subprocess.STDOUT)
            dk.procs.append(xp)
            time.sleep(3.0)
        else:
            check("flcomp announced DISPLAY=", False, log[-200:])
    else:
        print("skip: no Xwayland, the X frame is not checked")
    first = shot(dk, "dark")
    # where is the X frame? find its 'raised' title bar: the first raised-coloured row run outside Themes' window
    if xp:
        for y in range(40, H - 60):
            for x in range(40, W - 20, 2):
                if px(first, x, y) == DARK["raised"] and px(first, x + 20, y) == DARK["raised"]:
                    xframe = (x, y, x + 150, y + 30)
                    break
            if xframe:
                break
        check("an X window with a frame is on screen", xframe is not None)
    # Themes' window: the first window opens at 40, 0 (800 wide); its content colours are what we count
    wx0, wx1 = 44, 836
    pr = (H - 40, H - 2)
    scheme(first, DARK, LIGHT, "dark", pr, wx0, wx1, xframe)

    apply(dk, "FreeLFS Light")
    light = shot(dk, "light")
    scheme(light, LIGHT, DARK, "light", pr, wx0, wx1, xframe)

    apply(dk, "FreeLFS Dark")
    again = shot(dk, "dark-again")
    scheme(again, DARK, LIGHT, "dark again", pr, wx0, wx1, xframe)
    a1, a2 = count(first, DARK["panel"], 0, pr[0], W, pr[1], 1), count(again, DARK["panel"], 0, pr[0], W, pr[1], 1)
    check("dark again is the first dark (panel colour %d px, then %d px, the clock may differ)" % (a1, a2), abs(a1 - a2) < a1 * 0.01)

    # panel at the top, compact title bar, buttons at the left (the three settings together)
    dk_conf = os.path.join(dk.home, ".config", "freelfs", "desktop.conf")
    before = first_line_y(again, 440, DARK["line"])
    with open(dk_conf, "a") as f:
        f.write("panel_position=top\ntitlebar=compact\nbuttons=left\n")
    time.sleep(1.5)
    top = shot(dk, "top-compact")
    check("panel at the top: the top rows are the panel", count(top, DARK["panel"], 0, 4, W, 40) > area(0, 4, W, 40) * 0.6)
    check("... and the bottom rows are not (beside the Themes window)", count(top, DARK["panel"], 860, H - 40, W, H - 2) == 0)
    # the window moved out from under the panel (44 px) and its title bar is 6 px lower than compact: 44 + 29
    after = -1
    for y in range(46, 120):
        if px(top, 440, y) == tuple(DARK["line"]):
            after = y
            break
    check("compact title bar: the line under it is at 44 + 29 (was %d, now %d)" % (before, after), after == 73, "")
    # the window buttons at the left: the close icon is near the left edge of the title bar, not the right
    def inked(x0, x1, y0, y1):
        return sum(1 for y in range(y0, y1) for x in range(x0, x1) if px(top, x, y) not in (DARK["panel"], DARK["line"]) and
                   sum(px(top, x, y)) > 3 * 0x50)
    tb_y0 = 44 + 4
    left_ink = inked(44, 140, tb_y0, tb_y0 + 20)
    right_ink = inked(740, 836, tb_y0, tb_y0 + 20)
    check("window buttons at the left (icons drawn there, none at the right)", left_ink > 10 and right_ink < left_ink, "left %d right %d" % (left_ink, right_ink))

    # back to the default, in one go
    p = subprocess.run([FLTHEME, "--reset"], env=dk.env, capture_output=True)
    time.sleep(1.5)
    final = shot(dk, "reset")
    check("reset: the panel is at the bottom again", count(final, DARK["panel"], 0, pr[0], W, pr[1]) > area(0, pr[0], W, pr[1]) * 0.6)
    check("compositor alive", dk.comp.poll() is None)
    log = open(os.path.join(dk.dir, "comp.log")).read()
    check("no crash message in the compositor's log", "Segmentation" not in log and "assert" not in log.lower(), log[-300:])
finally:
    dk.stop() if hasattr(dk, "stop") else None
    for p_ in getattr(dk, "procs", []):
        try:
            p_.terminate()
        except Exception:
            pass
    try:
        dk.comp.terminate()
    except Exception:
        pass
print("all tests passed" if not fails else "%d test(s) failed" % fails)
sys.exit(1 if fails else 0)
