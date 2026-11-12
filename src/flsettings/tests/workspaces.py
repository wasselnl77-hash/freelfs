#!/usr/bin/env python3
# Settings > Workspaces in a headless desktop: flcomp headless (as src/flcomp/tests/apps.py), clicks on
# "+" and "-" of Across and Down and on the quick choices, then what desktop.conf says and, when flcomp's
# tests/shmclient is there, what flcomp announces (the grid really changes). Never touches the real
# ~/.config: HOME, XDG_CONFIG_HOME and FLUI_CONF point into a scratch directory.
# SPDX-License-Identifier: MIT
#
#   FLCOMP=path/to/flcomp FLSETTINGS=path/to/flsettings [SHMCLIENT=path/to/shmclient] \
#   [APPS_OUT=docs/m7] [APPS_THEME=light APPS_SUFFIX=-light] python3 -I workspaces.py
import importlib.util, os, shutil, subprocess, sys, tempfile, time

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("apps", os.path.join(here, "..", "..", "flcomp", "tests", "apps.py"))
apps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(apps)
check = apps.check

scratch = tempfile.mkdtemp(prefix="wssettings-")
# Desktop() makes its own home below the temp dir and writes theme= there; the config goes by FLUI_CONF
# into that same file, so set the variables to what Desktop will use once it exists (see main)


def grid(conf):
    """workspaces= from the file, '' when there is none"""
    try:
        for line in open(conf).read().split("\n"):
            if line.startswith("workspaces="):
                return line[len("workspaces="):]
    except FileNotFoundError:
        pass
    return ""


def main():
    shm = os.environ.get("SHMCLIENT") or os.path.join(here, "..", "..", "flcomp", "tests", "shmclient")
    os.environ["FLCOMP"] = os.environ.get("FLCOMP", "flcomp")
    # a private HOME and config for flcomp and Settings, before Desktop() copies the environment
    home = os.path.join(scratch, "h")
    os.makedirs(os.path.join(home, ".config", "freelfs"))
    conf = os.path.join(home, ".config", "freelfs", "desktop.conf")
    light = os.environ.get("APPS_THEME") == "light"
    if os.environ.get("APPS_THEME"):
        with open(conf, "w") as f:
            f.write("theme=%s\n" % os.environ["APPS_THEME"])
    os.environ.update(HOME=home, XDG_CONFIG_HOME=os.path.join(home, ".config"),
                      XDG_DATA_HOME=os.path.join(home, ".local", "share"), FLUI_CONF=conf)
    os.environ.pop("APPS_THEME", None)  # Desktop() would write it into its own home
    dk = apps.Desktop()
    dk.env.update(HOME=home, XDG_CONFIG_HOME=os.environ["XDG_CONFIG_HOME"], FLUI_CONF=conf)
    listen = None
    lout = os.path.join(scratch, "listen.out")
    try:
        if os.path.exists(shm):
            listen = subprocess.Popen([shm, "-l", "-W", "-s", "600"], env=dk.env, stdout=open(lout, "w"),
                                      stderr=subprocess.DEVNULL)
            dk.procs.append(listen)
            time.sleep(0.8)

        def announced():
            lines = [l for l in open(lout).read().split("\n") if l.startswith("workspaces ")]
            return lines[-1].split()[1] if lines else ""

        dk.start([os.environ.get("FLSETTINGS", "flsettings"), "--page", "workspaces"])
        w, h, shot = dk.shot("ws-settings-page")
        ox, oy = dk.origin(shot) or (0, 0)
        # geometry: content at (ox+1, oy+36), sidebar 168, margin 28; the rows start 88 px below the
        # window's top: Across, Down, the total, the quick choices (56 px each)
        right = ox + 1 + 168 + 28 + (780 - 2 - 168 - 56)
        row = lambda i: oy + 88 + 56 * i + 28
        plus = lambda i: (right - 16, row(i))
        minus = lambda i: (right - 32 - 44 - 16, row(i))
        quick = lambda i: (right - 5 * 44 - 4 * 4 + i * 48 + 22, row(3))

        check("workspaces: nothing is written before anything is clicked", grid(conf) == "", grid(conf))
        check("workspaces: the page draws the accent colour", shot.find(bytes.fromhex("2c7a4b" if light else "3f9b63")) >= 0)
        if listen:
            check("workspaces: flcomp starts with the default grid 2x2", announced() == "2x2", announced())

        dk.click(*plus(0))
        check("Across +: 2x2 becomes 3x2", grid(conf) == "3x2", grid(conf))
        time.sleep(0.5)
        if listen:
            check("flcomp announces 3x2", announced() == "3x2", announced())
        dk.click(*minus(1))
        check("Down -: 3x2 becomes 3x1", grid(conf) == "3x1", grid(conf))
        dk.shot("ws-settings-3x1")
        dk.click(*minus(1))
        check("Down - at 1: nothing happens", grid(conf) == "3x1", grid(conf))
        dk.click(*quick(3))
        check("quick choice 6: 3x2", grid(conf) == "3x2", grid(conf))
        dk.click(*quick(4))
        check("quick choice 8: 4x2", grid(conf) == "4x2", grid(conf))
        time.sleep(0.5)
        if listen:
            check("flcomp announces 4x2", announced() == "4x2", announced())
        dk.shot("ws-settings-4x2")
        dk.click(*quick(0))
        check("quick choice 1: 1x1", grid(conf) == "1x1", grid(conf))
        dk.click(*minus(0))
        check("Across - at 1: stays 1x1", grid(conf) == "1x1", grid(conf))
        dk.click(*plus(1))
        check("Down +: 1x2", grid(conf) == "1x2", grid(conf))
        for _ in range(8):
            dk.click(*plus(0))
            dk.click(*plus(1))
        check("Across and Down stop at 8: 8x8", grid(conf) == "8x8", grid(conf))
        dk.shot("ws-settings-8x8")
        # a hand-written bare number is N x 1 (as flcomp reads it)
        with open(conf, "w") as f:
            f.write("workspaces=3\n")
        time.sleep(0.8)
        dk.click(*plus(0))
        check("a bare 3 in the file is 3x1: Across + writes 4x1", grid(conf) == "4x1", grid(conf))
        # other keys in the file are kept
        with open(conf, "w") as f:
            f.write("accent=blue\nworkspaces=2x2\n")
        time.sleep(0.8)
        dk.click(*plus(0))
        cf = open(conf).read().split("\n")
        check("other keys stay when the grid changes", "accent=blue" in cf and "workspaces=3x2" in cf, str(cf))
    finally:
        dk.stop()
        shutil.rmtree(scratch, ignore_errors=True)


main()
sys.exit(1 if apps.fails else 0)
