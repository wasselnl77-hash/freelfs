#!/usr/bin/env python3
# Headless desktop test of the installer window: flcomp headless (as src/flcomp/tests/apps.py), real
# clicks and typing through every step, with the helper in --dry-run on a fixture of fake disks.
# The helper's input is tee'd to a file, so the test can check what the window asked for. Nothing is
# written to any disk: the helper never gets past --dry-run, and the fixture has no real device.
# SPDX-License-Identifier: MIT
#
#   FLCOMP=path/to/flcomp FLINSTALL=path/to/flinstall HELPER=path/to/flinstall-helper \
#   SOURCE=path/to/a/live/root-tree FIXTURE=tests/fixture APPS_OUT=docs/m7 python3 -I install.py
import importlib.util, os, subprocess, sys, tempfile, time

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("apps", os.path.join(here, "..", "..", "flcomp", "tests", "apps.py"))
apps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(apps)
apps.W, apps.H = 1280, 800

flinstall = os.environ["FLINSTALL"]
helper = os.environ["HELPER"]
source = os.environ["SOURCE"]
fixture = os.environ.get("FIXTURE", os.path.join(here, "fixture"))
work = tempfile.mkdtemp(prefix="flinstall-test-")
log_in = os.path.join(work, "helper-in.txt")
wrapper = os.path.join(work, "helper.sh")
with open(wrapper, "w") as f:
    f.write('#!/bin/sh\ntee "%s" | exec "%s" --dry-run --source "%s"\n' % (log_in, helper, source))
os.chmod(wrapper, 0o755)

dk = apps.Desktop()
dk.start([flinstall], FLINSTALL_HELPER=wrapper, FLINSTALL_FAKE=fixture, FL_ZONEINFO=os.environ.get("FL_ZONEINFO", "/usr/share/zoneinfo"),
         FL_KEYBOARD_CONF=os.path.join(work, "none"), FLUI_CONF=os.path.join(work, "none.conf"),
         DBUS_SYSTEM_BUS_ADDRESS="unix:path=" + os.path.join(work, "nobus"), FLINSTALL_RESTART="true")
time.sleep(1.5)
d = dk.shot("install-test-0")
ox, oy = dk.origin(d)
print("window at", ox, oy)


def click(x, y):
    dk.click(ox + x, oy + y)
    time.sleep(0.3)


def sent():
    try:
        return open(log_in).read()
    except FileNotFoundError:
        return ""


def cont():
    click(973, 682)


# language: Deutsch
click(910, 152)
cont()
# keyboard: German was suggested by the language; try typing in the field
click(640, 416)
dk.type("hallo")
dk.shot("install-test-keyboard")
cont()
# Wi-Fi: nothing to do
cont()
# disk: the Samsung disk (second row), whole disk
dk.shot("install-test-disk-0")
click(640, 230)
dk.shot("install-test-disk-1")
cont()
# account
click(500, 170)
dk.type("Anna Berger")
click(500, 341)
dk.type("secret1")
click(500, 417)
dk.type("secret1")
dk.shot("install-test-account")
cont()
# time zone: type in the search field, pick the first suggestion
click(640, 148)
dk.type("berlin")
time.sleep(0.4)
dk.shot("install-test-tz-search")
dk.key("down")
dk.key("enter")
time.sleep(0.4)
dk.shot("install-test-tz")
cont()
time.sleep(1.0)
dk.shot("install-test-summary")
s = sent()
apps.check("helper got the language", "set lang de_DE.UTF-8" in s, s)
apps.check("helper got the keyboard", "set keyboard de" in s)
apps.check("helper got the second disk (sda), not the first", "set disk /dev/sda\n" in s)
apps.check("helper got the whole-disk mode", "set mode whole" in s)
apps.check("helper got the user name anna", "set user anna" in s)
apps.check("helper got the full name", "set fullname Anna Berger" in s)
apps.check("helper got the password once", s.count("set password secret1") >= 1)
apps.check("helper got Europe/Berlin", "set tz Europe/Berlin" in s)
apps.check("helper was asked to check", "\ncheck\n" in s)
apps.check("no install asked yet", "install" not in s.replace("installer", ""))
# install
click(944, 682)
time.sleep(2.5)
dk.shot("install-test-done")
s = sent()
apps.check("install was asked", "\ninstall\n" in s or s.rstrip().endswith("install"))
dk.stop()
print("%d failure(s)" % apps.fails)
sys.exit(1 if apps.fails else 0)
