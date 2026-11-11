#!/usr/bin/env python3
"""Boot the image headless and take screendumps of the boot screen (flsplash).

    build/splash-shots.py [OUTDIR]       (default docs/m7)

The animation starts a moment before rc.sysinit prints its first status
line, so the serial console's "Mounting kernel file systems" is the clock:
dumps at 0.7, 1.8, 2.7 and 5 s after it go to OUTDIR/splash-1..4.png. Then the
test waits for the first-boot or login screen, takes one more dump, logs in
on the serial console and prints /run/flsplash.log (what flsplash saw, in ms)
and the daemon's CPU time and memory when it is still around.
"""

import importlib.util
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("boottest", os.path.join(here, "boot-test.py"))
bt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bt)
vr = importlib.util.spec_from_file_location("vmrun", os.path.join(here, "vm-run.py"))
vm = importlib.util.module_from_spec(vr)
vr.loader.exec_module(vm)


def ppm_to_png(src, dst):
    with open(src, "rb") as f:
        data = f.read()
    parts = data.split(None, 4)  # P6 w h maxval, then the pixels
    w, h = int(parts[1]), int(parts[2])
    pix = data[len(data) - w * h * 3:]
    raw = b"".join(b"\0" + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d))

    with open(dst, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def main():
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "docs", "m7"))
    os.makedirs(out, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="flfs-", dir="/tmp")
    console, mon = os.path.join(tmp, "c"), os.path.join(tmp, "m")
    env = dict(os.environ, FLFS_VM_SERIAL=f"unix:{console},server=on,wait=on")
    log = open(os.path.join(bt.WORK, "logs", "splash-shots.log"), "w")
    qemu = subprocess.Popen([os.path.join(here, "51-qemu.sh"), "--serial", "--",
                             "-monitor", f"unix:{mon},server=on,wait=off"] + sys.argv[2:],
                            env=env, stdout=log, stderr=subprocess.STDOUT)
    try:
        while not os.path.exists(console):
            time.sleep(0.1)
        s = socket.socket(socket.AF_UNIX)
        s.connect(console)
        c = bt.Console(s, log)
        c.expect("Mounting kernel file systems", 240)
        t0 = time.monotonic()
        for i, at in enumerate((0.7, 1.8, 2.7, 5.0), 1):
            time.sleep(max(0, t0 + at - time.monotonic()))
            ppm = os.path.join(tmp, f"s{i}.ppm")
            vm.monitor(mon, f"screendump {ppm}")
            time.sleep(0.3)
            ppm_to_png(ppm, os.path.join(out, f"splash-{i}.png"))
            print(f"splash-{i}.png at about {at} s")
        if c.expect_any(["First boot", "login:"], 300) == "First boot":
            time.sleep(8)
            vm.monitor(mon, "screendump " + os.path.join(tmp, "f.ppm"))
            time.sleep(1)
            ppm_to_png(os.path.join(tmp, "f.ppm"), os.path.join(out, "splash-5-firstboot.png"))
            vm.monitor(mon, "sendkey ret")
            c.expect("login:", 300)
        time.sleep(4)
        vm.monitor(mon, "screendump " + os.path.join(tmp, "g.ppm"))
        time.sleep(1)
        ppm_to_png(os.path.join(tmp, "g.ppm"), os.path.join(out, "splash-6-login.png"))
        c.send("root")
        c.expect(":~#", 30)
        c.send("PS1='\\n@@\\$ '")
        c.expect("@@# ", 10)
        for cmd in ("cat /run/flsplash.log",
                    "ls /run/flsplash.* ; pgrep -x flsplash || echo flsplash-gone",
                    "dmesg | grep -ci 'simpledrm\\|bochs'"):
            c.send(cmd)
            print("$ " + cmd + "\n" + c.expect("@@# ", 30).replace("\r", "").split("\n", 1)[-1])
        c.send("poweroff")
        c.drain(90)
        qemu.wait(30)
    finally:
        if qemu.poll() is None:
            qemu.kill()
        for f in (console, mon):
            try:
                os.unlink(f)
            except OSError:
                pass


if __name__ == "__main__":
    main()
