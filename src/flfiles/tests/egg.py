#!/usr/bin/env python3
# flfiles asks flcrash before deleting a vital folder: "You can't do that." (headless desktop)
# Reuses the Desktop harness of flcomp/tests/apps.py. Environment:
#   FLCOMP, FLFILES   programs (wrapper scripts that run host-built binaries are fine)
#   FLCRASH_BIN       the flcrash program, put on PATH as `flcrash` for flfiles
#   EGG_OUT           dir for a screenshot (egg-flfiles.png)
# FLCRASH_PROTECT names the folder that counts as vital here (the real ones need root anyway).
import importlib.util, os, shutil, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("apps", os.path.join(HERE, "..", "..", "flcomp", "tests", "apps.py"))
apps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(apps)
OUT = os.environ.get("EGG_OUT")
fails = 0


def check(name, cond, why=""):
    global fails
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else ": " + why), flush=True)
    if not cond:
        fails += 1


scratch = tempfile.mkdtemp(prefix="egg-flfiles-")
os.environ["XDG_CONFIG_HOME"] = os.path.join(scratch, "config")
os.environ["XDG_DATA_HOME"] = os.path.join(scratch, "data")
os.makedirs(os.environ["XDG_CONFIG_HOME"])
bindir = os.path.join(scratch, "bin")
os.makedirs(bindir)
os.symlink(os.path.abspath(os.environ["FLCRASH_BIN"]), os.path.join(bindir, "flcrash"))
dk = apps.Desktop()
try:
    home = dk.home
    vital = os.path.join(home, "vital")
    plain = os.path.join(home, "plain")
    for d in (vital, plain):
        os.makedirs(d)
        open(os.path.join(d, "f"), "w").write("x")
    trash = os.path.join(scratch, "data", "Trash", "files")
    p = dk.start([os.environ.get("FLFILES", "flfiles")], PATH=bindir + ":" + os.environ["PATH"],
                 FLCRASH_PROTECT=vital, FLCRASH_NO_SOUND="1")
    time.sleep(1.0)

    def jump(name):
        time.sleep(1.3)
        dk.type(name)

    jump("plain")
    dk.key("delete")
    time.sleep(0.8)
    check("an ordinary folder goes to the Trash without a question", not os.path.exists(plain) and os.path.exists(os.path.join(trash, "plain")))
    jump("vital")
    dk.key("delete")
    time.sleep(2.0)
    w, h, d = dk.shot()
    if OUT:
        apps.write_png(os.path.join(OUT, "egg-flfiles.png"), w, h, d)
    check("the egg covers the screen in the crash screen green", dk.pixel(d, 5, 5) == "1f3d2e" and dk.pixel(d, 990, 690) == "1f3d2e")
    dk.key("esc")
    time.sleep(1.0)
    check("Never mind: the folder is still where it was", os.path.exists(os.path.join(vital, "f")))
    w, h, d = dk.shot()
    check("and flfiles is back on screen", dk.pixel(d, 5, 5) != "1f3d2e")
    dk.key("delete")
    time.sleep(2.0)
    dk.key("tab")
    dk.key("enter")
    time.sleep(1.0)
    check("Continue: it goes to the Trash after all", not os.path.exists(vital) and os.path.exists(os.path.join(trash, "vital")))
finally:
    dk.stop()
    shutil.rmtree(scratch, ignore_errors=True)
sys.exit(1 if fails else 0)
