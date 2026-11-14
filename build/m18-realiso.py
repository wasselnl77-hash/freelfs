#!/usr/bin/env python3
"""Round 18: the REAL live ISO (normal kernel command line, no serial console, no root shell, nothing of the
guest is read) shut down from its login screen, as the person at the laptop does it.

    build/m18-realiso.py [--iso FILE] [--mem 1G]

Boot, answer the first-boot keyboard question with Enter, wait for the login screen (a screendump with many
colours and the Shut down button's corner lit), click "Shut down" twice (first click arms it), and require that
QEMU exits within 30 s of the second click. Exit 0 only then; anything else prints FAIL and exits 1.
Pictures: $OUT (default ~/freelfs-work/scratch/m18-real).
"""
import argparse, json, os, re, socket, subprocess, sys, tempfile, time

here = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser()
ap.add_argument("--iso", default=os.path.expanduser("~/freelfs-work/freelfs-live.iso"))
ap.add_argument("--mem", default="1G")
args = ap.parse_args()
out = os.environ.get("OUT", os.path.expanduser("~/freelfs-work/scratch/m18-real"))
os.makedirs(out, exist_ok=True)
tmp = tempfile.mkdtemp(prefix="flfs-real-")
mon, qmp_path = os.path.join(tmp, "mon"), os.path.join(tmp, "qmp")
env = dict(os.environ, FLFS_VM_SERIAL="null", FLFS_VM_MEM=args.mem, FLFS_VM_VARS=os.path.join(tmp, "ovmf-vars.fd"))
cmd = [os.path.join(here, "51-qemu.sh"), "--serial", "--iso", args.iso, "--", "-monitor", f"unix:{mon},server=on,wait=off",
       "-qmp", f"unix:{qmp_path},server=on,wait=off", "-usb", "-device", "usb-tablet"]


def monitor(text, wait=0.3):
    m = socket.socket(socket.AF_UNIX)
    m.connect(mon)
    m.sendall(text.encode() + b"\n")
    time.sleep(wait)
    m.close()


def qmp(events, wait=0.3):
    for _ in range(3):
        c = socket.socket(socket.AF_UNIX)
        c.settimeout(5)
        try:
            c.connect(qmp_path)
            f = c.makefile("rw")
            f.readline()
            for q in ({"execute": "qmp_capabilities"}, {"execute": "input-send-event", "arguments": {"events": events}}):
                f.write(json.dumps(q) + "\n")
                f.flush()
                f.readline()
            c.close()
            break
        except (socket.timeout, OSError):
            c.close()
            time.sleep(0.5)
    time.sleep(wait)


def move(x, y):  # the tablet is 0..32767 on a 1280x800 screen
    qmp([{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / 1279)}},
         {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / 799)}}], 0.3)


def click():
    qmp([{"type": "btn", "data": {"button": "left", "down": True}}], 0.08)
    qmp([{"type": "btn", "data": {"button": "left", "down": False}}], 0.3)


def colours(fn):
    try:
        d = open(fn, "rb").read()
        m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", d)
        px = d[m.end():]
    except (OSError, AttributeError):
        return -1
    return len({px[i:i + 3] for i in range(0, len(px) - 3, 3 * 7)})


def pixel(fn, x, y):
    try:
        d = open(fn, "rb").read()
        m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", d)
        w = int(m[1])
        o = m.end() + (y * w + x) * 3
        return tuple(d[o:o + 3])
    except (OSError, AttributeError, TypeError):
        return None


def is_greeter(fn):
    """the login card: its green top bar (3f9b63) at the top and the (dimmed, no password yet) Log in button below"""
    near = lambda p, q: p is not None and all(abs(a - b) < 14 for a, b in zip(p, q))
    return near(pixel(fn, 640, 210), (0x3f, 0x9b, 0x63)) and near(pixel(fn, 766, 535), (51, 123, 79))


def shot(name):
    fn = os.path.join(out, name + ".ppm")
    monitor(f"screendump {fn}", 1.5)
    return fn


fails = []


def fail(msg):
    fails.append(msg)
    print("FAIL:", msg, flush=True)


qemu = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(100):
        if os.path.exists(mon) and os.path.exists(qmp_path):
            break
        time.sleep(0.2)
    # the first-boot keyboard question: Enter takes the default; then the login screen
    t0 = time.monotonic()
    pressed = False
    greeter = False
    while time.monotonic() - t0 < 240 and qemu.poll() is None:
        time.sleep(5)
        n = colours(shot("poll"))
        waited = time.monotonic() - t0
        if not pressed and waited > 25 and n > 30:
            monitor("sendkey ret")
            pressed = True
            print("keyboard question answered at %.0f s (%d colours)" % (waited, n), flush=True)
            continue
        # the login screen: wallpaper plus the card, many colours; stable over two looks
        if pressed and is_greeter(os.path.join(out, "poll.ppm")):
            time.sleep(4)
            if is_greeter(shot("poll")):
                greeter = True
                break
    if not greeter:
        fail("the login screen never showed up (240 s)")
    else:
        print("login screen after %.0f s" % (time.monotonic() - t0), flush=True)
        shot_fn = shot("rs-1-greeter")
        move(534, 535)
        click()
        time.sleep(1)
        shot("rs-2-armed")
        if qemu.poll() is not None:
            fail("QEMU exited after ONE click (the shut-down must ask again)")
        else:
            click()
            t1 = time.monotonic()
            while time.monotonic() - t1 < 40 and qemu.poll() is None:
                time.sleep(0.5)
            took = time.monotonic() - t1
            if qemu.poll() is None:
                shot("rs-3-stuck")
                fail("the real ISO did not power off within 40 s after the second click on Shut down")
            elif took > 30:
                fail("power-off took %.0f s (budget 30 s)" % took)
            else:
                print("powered off %.0f s after the second click" % took, flush=True)
finally:
    if qemu.poll() is None:
        qemu.terminate()
        try:
            qemu.wait(10)
        except subprocess.TimeoutExpired:
            qemu.kill()
print("RESULT: PASS" if not fails else "RESULT: FAIL (%d): %s" % (len(fails), "; ".join(fails)))
print("pictures in", out)
sys.exit(1 if fails else 0)
