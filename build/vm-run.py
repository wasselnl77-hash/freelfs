#!/usr/bin/env python3
"""Boot the FreeLFS image headless, log in as root, run commands, power off.

    build/vm-run.py CMDFILE [QEMU-ARGS...]

CMDFILE holds one shell command per line ('#' lines are skipped); each
command's output is printed after it. QEMU-ARGS go to 51-qemu.sh, e.g. an
extra -drive. With FLFS_VM_KEYS set to QEMU key names separated by spaces
(e.g. "down ret"), those keys are sent to the boot menu first; a word
"shot:FILE.ppm" takes a screenshot there instead. With FLFS_VM_TTY set to
a file of steps, those are done on the VGA screen (tty1) after the
commands, one per line:

    type TEXT     type TEXT (\\n is Enter, \\t is Tab)
    key NAME      press a key by its QEMU name (e.g. ctrl-c, up)
    wait SECONDS
    shot FILE     screenshot (PPM)
    click X Y     move the pointer to pixel X Y of a 1280x800 screen and click the left button
                  (needs QEMU's -device usb-tablet, an absolute pointer: build/m9-features.sh)
    run CMD       run CMD on the serial console and print its output

Since M7 tty1 shows the login screen (flgreet), not a login prompt: Enter
logs in the selected account when it has no password, otherwise type the
password first. See build/m7-desktop.cmds and build/m7-desktop.tty for a
whole login-to-desktop run.

The console transcript goes to $FLFS_WORK/logs/vm-run.log.
"""

import importlib.util
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("boottest", os.path.join(here, "boot-test.py"))
bt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bt)


def monitor(path, line):
    s = socket.socket(socket.AF_UNIX)
    s.connect(path)
    time.sleep(0.05)
    s.recv(4096)
    s.sendall(line.encode() + b"\n")
    time.sleep(0.08)
    s.close()


# characters to QEMU key names (US layout)
KEYS = {" ": "spc", "\n": "ret", "\t": "tab", "-": "minus", "=": "equal", ".": "dot",
        ",": "comma", "/": "slash", ";": "semicolon", "'": "apostrophe", "`": "grave_accent",
        "[": "bracket_left", "]": "bracket_right", "\\": "backslash"}
SHIFTED = {"_": "minus", "+": "equal", ">": "dot", "<": "comma", "?": "slash", ":": "semicolon",
           '"': "apostrophe", "~": "grave_accent", "{": "bracket_left", "}": "bracket_right",
           "|": "backslash", "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6",
           "&": "7", "*": "8", "(": "9", ")": "0"}


def key_name(ch):
    if ch.isalnum() and ch.isascii():
        return f"shift-{ch.lower()}" if ch.isupper() else ch
    if ch in KEYS:
        return KEYS[ch]
    if ch in SHIFTED:
        return f"shift-{SHIFTED[ch]}"
    raise ValueError(f"cannot type {ch!r}")


def qmp_click(path, x, y):
    """move QEMU's absolute pointer (usb-tablet) to pixel X Y of a 1280x800 screen and click.
    The HMP mouse_move is relative; input-send-event of the QMP socket has absolute axes."""
    def ev(events):
        return {"execute": "input-send-event", "arguments": {"events": events}}
    s = socket.socket(socket.AF_UNIX)
    s.connect(path)
    f = s.makefile("rw")
    f.readline()  # greeting
    for cmd in ({"execute": "qmp_capabilities"},
                ev([{"type": "abs", "data": {"axis": "x", "value": x * 32767 // 1279}},
                    {"type": "abs", "data": {"axis": "y", "value": y * 32767 // 799}}])):
        f.write(json.dumps(cmd) + "\n")
        f.flush()
        f.readline()
    time.sleep(0.4)
    for down in (True, False):
        f.write(json.dumps(ev([{"type": "btn", "data": {"button": "left", "down": down}}])) + "\n")
        f.flush()
        f.readline()
        time.sleep(0.15)
    s.close()


def tty_steps(mon, path, c):
    for line in open(path):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        what, _, arg = line.partition(" ")
        if what == "type":
            text = arg.replace("\\n", "\n").replace("\\t", "\t")
            for ch in text:
                monitor(mon, f"sendkey {key_name(ch)}")
        elif what == "key":
            monitor(mon, f"sendkey {arg}")
        elif what == "wait":
            time.sleep(float(arg))
        elif what == "run":  # a command on the serial console
            c.send(arg)
            print(c.expect("@@# ", 60).replace("\r", ""), flush=True)
        elif what == "click":
            x, y = (int(v) for v in arg.split())
            qmp_click(mon + "q", x, y)
            time.sleep(0.4)
        elif what == "shot":
            monitor(mon, f"screendump {arg}")
            time.sleep(1)


def main():
    cmds = [l.rstrip("\n") for l in open(sys.argv[1]) if l.strip() and not l.startswith("#")]
    tmp = tempfile.mkdtemp(prefix="flfs-", dir="/tmp")  # unix socket paths must be short
    console, mon = os.path.join(tmp, "c"), os.path.join(tmp, "m")
    env = dict(os.environ, FLFS_VM_SERIAL=f"unix:{console},server=on,wait=on")
    log = open(os.path.join(bt.WORK, "logs", "vm-run.log"), "w")
    qemu = subprocess.Popen([os.path.join(here, "51-qemu.sh"), "--serial", "--",
                             "-monitor", f"unix:{mon},server=on,wait=off",
                             "-qmp", f"unix:{mon}q,server=on,wait=off"] + sys.argv[2:],
                            env=env, stdout=log, stderr=subprocess.STDOUT)
    try:
        while not os.path.exists(console):
            time.sleep(0.1)
        s = socket.socket(socket.AF_UNIX)
        s.connect(console)
        c = bt.Console(s, log)
        keys = os.environ.get("FLFS_VM_KEYS", "").split()
        if keys:
            c.expect("BdsDxe: starting", 120)
            time.sleep(1.5)  # flboot draws its menu
            for k in keys:
                if k.startswith("shot:"):  # screenshot (PPM) instead of a key
                    monitor(mon, f"screendump {k[5:]}")
                    time.sleep(1.5)
                else:
                    monitor(mon, f"sendkey {k}")
                time.sleep(0.5)
        if c.expect_any(["First boot", "login:"], 300) == "First boot":
            time.sleep(8)  # flsetup is on the screen: Enter takes the default layout
            monitor(mon, "sendkey ret")
            c.expect("login:", 300)
        c.send("root")
        c.expect(":~#", 30)
        c.send("PS1='\\n@@\\$ '")
        c.expect("@@# ", 10)
        for cmd in cmds:
            c.send(cmd)
            out = c.expect("@@# ", 1800).replace("\r", "").strip()
            body = out.split("\n", 1)[1] if "\n" in out else ""
            print(f"$ {cmd}\n{body}\n", flush=True)
        if os.environ.get("FLFS_VM_TTY"):
            tty_steps(mon, os.environ["FLFS_VM_TTY"], c)
        c.send("poweroff")
        c.drain(90)
        qemu.wait(30)
    finally:
        if qemu.poll() is None:
            qemu.kill()
        for f in (console, mon, mon + "q"):
            try:
                os.unlink(f)
            except OSError:
                pass
        os.rmdir(tmp)


if __name__ == "__main__":
    main()
