#!/usr/bin/env python3
# Frames in the middle of every animation, from a headless desktop with the real panel and the real
# Themes window, slowed down ten times (animation_speed=0.1) so a screenshot can catch them:
# open, maximize, minimize, workspace slide, overview, close. PNGs go to FRAMES_OUT.
#   FLCOMP=path FLTHEME=path APPS_BIN=dir with flpanel FRAMES_OUT=docs/m7
# SPDX-License-Identifier: MIT
import importlib.util, os, signal, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("apps", os.path.join(HERE, "..", "..", "flcomp", "tests", "apps.py"))
apps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(apps)
FLTHEME = os.environ.get("FLTHEME", os.path.join(HERE, "..", "fltheme"))
OUT = os.environ.get("FRAMES_OUT", ".")
os.environ["FLCOMP_WALLPAPER"] = ""
dk = apps.Desktop(panel=True)
conf = os.path.join(dk.home, ".config", "freelfs", "desktop.conf")


def shot(name, delay=0.0):
    time.sleep(delay)
    os.kill(dk.comp.pid, signal.SIGUSR1)
    time.sleep(0.35)
    w, h, d = apps.read_ppm(dk.ppm)
    apps.write_png(os.path.join(OUT, "fltheme-anim-%s.png" % name), w, h, d)
    print("wrote", name, flush=True)


try:
    with open(conf, "a") as f:
        f.write("animation_speed=0.1\n")
    time.sleep(1.5)
    app = dk.start([FLTHEME], )   # waits 1 s after the start: the window is up, its open motion running
    shot("open-mid")
    time.sleep(3)
    dk.send("key 125 1", 0.05); dk.send("key 103 1", 0.05); dk.send("key 103 0", 0.05); dk.send("key 125 0", 0.05)
    shot("maximize-mid", 0.3)
    time.sleep(3)
    dk.send("key 125 1", 0.05); dk.send("key 29 1", 0.05); dk.send("key 106 1", 0.05); dk.send("key 106 0", 0.05)
    dk.send("key 29 0", 0.05); dk.send("key 125 0", 0.05)
    shot("workspace-slide-mid", 0.5)
    time.sleep(3)
    dk.send("key 125 1", 0.05); dk.send("key 29 1", 0.05); dk.send("key 105 1", 0.05); dk.send("key 105 0", 0.05)
    dk.send("key 29 0", 0.05); dk.send("key 125 0", 0.05)
    time.sleep(3)
    dk.send("key 125 1", 0.05); dk.send("key 15 1", 0.05); dk.send("key 15 0", 0.05); dk.send("key 125 0", 0.05)
    shot("overview-mid", 0.2)
    time.sleep(3)
    dk.send("key 1 1", 0.05); dk.send("key 1 0", 0.05)
    time.sleep(3)
    # minimize: Super+Down twice (restore from maximized, then minimize)
    dk.send("key 125 1", 0.05); dk.send("key 108 1", 0.05); dk.send("key 108 0", 0.05); dk.send("key 125 0", 0.05)
    time.sleep(3)
    dk.send("key 125 1", 0.05); dk.send("key 108 1", 0.05); dk.send("key 108 0", 0.05); dk.send("key 125 0", 0.05)
    shot("minimize-mid", 0.6)
    time.sleep(3)
    app.terminate()
    shot("close-mid", 0.3)
finally:
    for p in getattr(dk, "procs", []):
        try:
            p.terminate()
        except Exception:
            pass
    dk.comp.terminate()
