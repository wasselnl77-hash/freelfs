#!/usr/bin/env python3
"""Boot the live ISO (or a dd'd disk) headless, log in as root on the serial console, run
a file of commands, print the output.

    build/live-run.py [--iso FILE | --disk FILE] [--mem 1G] CMDFILE [-- QEMU-ARGS...]

CMDFILE: one shell command per line, '#' lines skipped. Lines starting with '@':
    @wait SECONDS         sleep
    @keys A B C           several keys in a row
    @type TEXT            type text (letters, digits, space . - /)
    @key NAME             QEMU sendkey (e.g. ret, ctrl-alt-t, meta_l)
    @mon TEXT             any QEMU monitor command (device_add, drive_add ...)
    @shot FILE            screendump to FILE (PPM)
    @mouse X Y            absolute pointer, pixels of a 1280x800 screen (needs -device usb-tablet in QEMU-ARGS)
    @click                left click
    @desktop              wait for the greeter, press Enter (tester has no password), wait for the panel
    @poweroffwait SECS    wait up to SECS for QEMU to exit; NOT exited, a slow power-off (over 20 s from
                          the call) or a missing "Powering off." line in the console FAIL the run
    @greeter              wait for the login screen (flgreet-ui running); no key is pressed; FAILS if it never comes
    @ok TEXT :: CMD       run CMD in the guest as root; FAILS unless it exits 0 (TEXT names the check)
    @waitok SECS TEXT :: CMD   like @ok, but CMD is tried every second for up to SECS seconds
    @shotok FILE          like @shot (kept for old files), or (almost) one colour
    @shotdiff BASE NEW MIN [X1 Y1 X2 Y2]  FAILS unless at least MIN pixels differ between two screendumps
    @shotpix FILE RRGGBB MIN  FAILS unless at least MIN pixels have exactly that colour
    @shotcolors FILE MIN  FAILS unless the screendump FILE has at least MIN distinct colours
    @shot FILE            screendump; also FAILS when the file is missing or blank
Console transcript: $FLFS_WORK/logs/live-run.log. Exit 1 when any check failed (see the RESULT line),
0 when the VM was driven to the end and all passed.
"""
import argparse, importlib.util, os, re, socket, subprocess, sys, tempfile, time

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("boottest", os.path.join(here, "boot-test.py"))
bt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bt)

argv = sys.argv[1:]
extra = []
if "--" in argv:
    i = argv.index("--")
    argv, extra = argv[:i], argv[i + 1:]
ap = argparse.ArgumentParser()
ap.add_argument("--iso")
ap.add_argument("--disk")
ap.add_argument("--mem", default="1G")
ap.add_argument("--keep", action="store_true", help="leave QEMU running at the end")
ap.add_argument("cmdfile")
args = ap.parse_args(argv)
LOG = os.path.join(bt.WORK, "logs", "live-run.log")
ANSI = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")

os.makedirs(os.path.dirname(LOG), exist_ok=True)
tmp = tempfile.mkdtemp(prefix="flfs-live-")
path, mon = os.path.join(tmp, "console"), os.path.join(tmp, "mon")
env = dict(os.environ, FLFS_VM_SERIAL=f"unix:{path},server=on,wait=on", FLFS_VM_MEM=args.mem,
           FLFS_VM_VARS=os.path.join(tmp, "ovmf-vars.fd"))
cmd = [os.path.join(bt.TOP, "build", "51-qemu.sh"), "--serial"]
if args.disk:
    env["FLFS_IMG"] = args.disk
if args.iso:
    cmd += ["--iso", args.iso]
qmp_path = os.path.join(tmp, "qmp")
cmd += ["--", "-monitor", f"unix:{mon},server=on,wait=off", "-qmp", f"unix:{qmp_path},server=on,wait=off", "-usb"] + extra


def monitor(text, wait=0.3):
    m = socket.socket(socket.AF_UNIX)
    m.connect(mon)
    m.sendall(text.encode() + b"\n")
    time.sleep(wait)
    m.close()


def qmp(events, wait=0.3):
    import json
    for attempt in range(3):	# the QMP socket serves one client at a time: a busy one is tried again
        c = socket.socket(socket.AF_UNIX)
        c.settimeout(5)
        try:
            c.connect(qmp_path)
            f = c.makefile("rw")
            f.readline()
            for cmd in ({"execute": "qmp_capabilities"}, {"execute": "input-send-event", "arguments": {"events": events}}):
                f.write(json.dumps(cmd) + "\n")
                f.flush()
                f.readline()
            c.close()
            break
        except (socket.timeout, OSError):
            c.close()
            time.sleep(0.5)
    time.sleep(wait)


def button(name, n=1):
    for _ in range(n):
        qmp([{"type": "btn", "data": {"button": name, "down": True}}], 0.08)
        qmp([{"type": "btn", "data": {"button": name, "down": False}}], 0.12)


FAILS = []


def fail(msg):
    FAILS.append(msg)
    print("FAIL:", msg, flush=True)


def shot_blank(fn):
    """None when the PPM at fn is a real picture, else the reason."""
    try:
        d = open(fn, "rb").read()
    except OSError:
        return "missing"
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", d)
    if not m:
        return "not a PPM"
    px = d[m.end():]
    n = len(px) // 3
    if n < 1000:
        return "empty"
    step = max(1, n // 20000)
    from collections import Counter
    c = Counter(px[i * 3:i * 3 + 3] for i in range(0, n, step))
    top = c.most_common(1)[0][1] / sum(c.values())
    return "one colour (%d%%)" % (top * 100) if top > 0.985 else None


def shot_check(fn, what):
    r = shot_blank(fn)
    if r:
        fail(f"{what}: screendump {os.path.basename(fn)} is {r}")


def shot_colours(fn):
    """Number of distinct colours in a sample of the PPM (a gradient picture or a video frame has hundreds,
    a blank or flat window a handful), or -1 when it cannot be read."""
    try:
        d = open(fn, "rb").read()
        m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", d)
        px = d[m.end():]
    except (OSError, AttributeError):
        return -1
    return len({px[i:i + 3] for i in range(0, len(px) - 3, 3 * 7)})


def shot_diff(a, b, box=None):
    """Number of pixels that differ between two PPMs (optionally inside box = x1 y1 x2 y2), -1 when unreadable."""
    try:
        da, db = open(a, "rb").read(), open(b, "rb").read()
        ma = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", da)
        mb = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", db)
        w, h = int(ma.group(1)), int(ma.group(2))
        if (w, h) != (int(mb.group(1)), int(mb.group(2))):
            return -1
        pa, pb = da[ma.end():], db[mb.end():]
    except (OSError, AttributeError):
        return -1
    x1, y1, x2, y2 = box or (0, 0, w, h)
    n = 0
    for y in range(y1, min(y2, h)):
        ra = pa[(y * w + x1) * 3:(y * w + min(x2, w)) * 3]
        rb = pb[(y * w + x1) * 3:(y * w + min(x2, w)) * 3]
        if ra != rb:
            n += sum(1 for i in range(0, len(ra), 3) if ra[i:i + 3] != rb[i:i + 3])
    return n


def guest_ok(con, text, cmdline):
    """Run cmdline in the guest; True when it exits 0. The verdict token is computed, so the console's
    echo of the command cannot contain it."""
    con.send(f"( {cmdline} ) >/tmp/ok.out 2>&1; echo OK$?T$((6*7))")
    out = ANSI.sub("", con.expect(":~#", 120)).replace("\r", "")
    return "OK0T42" in out


with open(LOG, "w") as log:
    qemu = subprocess.Popen(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
    try:
        for _ in range(100):
            if os.path.exists(path):
                break
            time.sleep(0.1)
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(path)
        con = bt.Console(sock, log)
        seen = con.expect_any(["First boot", "login:", "could not find the FreeLFS"], 300)
        if seen == "First boot":
            time.sleep(8)
            monitor("sendkey ret")
        con.expect("login:", 300)
        con.send("root")
        con.expect(":~#", 30)
        for line in open(args.cmdfile).read().splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("@put "):	# @put LOCAL REMOTE: a file through the serial console (base64), mode 755
                import base64, gzip
                _, local, remote = line.split()
                data = base64.b64encode(gzip.compress(open(local, "rb").read(), 6)).decode()
                con.send(f": > {remote}.b64")
                con.expect(":~#", 30)
                for i in range(0, len(data), 900):
                    con.send(f"printf %s '{data[i:i + 900]}' >> {remote}.b64")
                    con.expect(":~#", 30)
                con.send(f"base64 -d {remote}.b64 | gzip -dc > {remote}; chmod 755 {remote}; rm {remote}.b64; ls -l {remote}")
                print(ANSI.sub("", con.expect(":~#", 60)).replace("\r", "").strip().splitlines()[-1])
                continue
            if line.startswith("@"):
                w = line[1:].split()
                if w[0] == "wait":
                    time.sleep(float(w[1]))
                elif w[0] == "key":	# @key NAME [HOLD_MS]
                    monitor("sendkey " + " ".join(w[1:]), 0.3 + (int(w[2]) / 1000.0 if len(w) > 2 else 0))
                elif w[0] == "keys":	# several keys one after the other
                    for k in w[1:]:
                        monitor("sendkey " + k, 0.4)
                elif w[0] == "type":	# plain text through sendkey (letters, digits, space)
                    for ch in line[1:].split(None, 1)[1]:
                        # the live session's default keyboard is German (QWERTZ): the QEMU key names are US positions
                        de = {" ": "spc", ".": "dot", ",": "comma", "-": "slash", "_": "shift-slash", "/": "shift-7", ":": "shift-dot",
                              "=": "shift-0", "?": "shift-minus", "\n": "ret", "y": "z", "z": "y", "Y": "shift-z", "Z": "shift-y"}
                        monitor("sendkey " + de.get(ch, ("shift-" + ch.lower()) if ch.isupper() else ch), 0.15)
                elif w[0] == "mon":
                    monitor(line[1:].split(None, 1)[1], 1.0)
                elif w[0] in ("shot", "shotok"):
                    monitor(f"screendump {w[1]}", 1.5)
                    shot_check(w[1], "@" + w[0])
                elif w[0] == "shotcolors":	# @shotcolors FILE MIN: FAILS unless the picture has at least MIN colours
                    n = shot_colours(w[1])
                    print("colours in %s: %d" % (os.path.basename(w[1]), n))
                    if n < int(w[2]):
                        fail("%s has %d colours, expected at least %s (blank or flat window?)" % (os.path.basename(w[1]), n, w[2]))
                elif w[0] == "shotdiff":	# @shotdiff BASE NEW MIN [X1 Y1 X2 Y2]: FAILS unless at least MIN pixels differ
                    n = shot_diff(w[1], w[2], tuple(int(v) for v in w[4:8]) if len(w) >= 8 else None)
                    print("pixels that differ, %s vs %s: %d" % (os.path.basename(w[1]), os.path.basename(w[2]), n))
                    if n < int(w[3]):
                        fail("%s differs from %s in %d pixels, expected at least %s (no window appeared?)" % (os.path.basename(w[2]), os.path.basename(w[1]), n, w[3]))
                elif w[0] == "shotpix":	# @shotpix FILE RRGGBB MIN: FAILS unless at least MIN pixels have exactly that colour
                    d = open(w[1], "rb").read()
                    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", d)
                    want = bytes.fromhex(w[2])
                    px = d[m.end():]
                    n = sum(1 for i in range(0, len(px) - 2, 3) if px[i:i + 3] == want)
                    print("pixels of %s in %s: %d" % (w[2], os.path.basename(w[1]), n))
                    if n < int(w[3]):
                        fail("%s has %d pixels of colour %s, expected at least %s" % (os.path.basename(w[1]), n, w[2], w[3]))
                elif w[0] in ("ok", "waitok"):
                    secs = int(w[1]) if w[0] == "waitok" else 0
                    body = line.split(None, 2)[2] if w[0] == "waitok" else line.split(None, 1)[1]
                    what, _, c = body.partition("::")
                    t0 = time.monotonic()
                    while True:
                        good = guest_ok(con, what, c.strip())
                        if good or time.monotonic() - t0 >= secs:
                            break
                        time.sleep(1)
                    print(("ok: " if good else "NOT ok: ") + what.strip() + (" (%d s)" % (time.monotonic() - t0) if secs else ""))
                    if not good:
                        fail(what.strip() + "  [" + c.strip()[:120] + "]")
                elif w[0] == "mouse":
                    qmp([{"type": "abs", "data": {"axis": "x", "value": int(w[1]) * 32767 // 1279}},
                         {"type": "abs", "data": {"axis": "y", "value": int(w[2]) * 32767 // 799}}], 0.4)
                elif w[0] == "click":	# @click [left|right|middle]
                    button(w[1] if len(w) > 1 else "left")
                elif w[0] == "drag":	# @drag X1 Y1 X2 Y2 [SHOTFILE taken at the end, button still down]
                    x1, y1, x2, y2 = (int(v) for v in w[1:5])
                    qmp([{"type": "abs", "data": {"axis": "x", "value": x1 * 32767 // 1279}},
                         {"type": "abs", "data": {"axis": "y", "value": y1 * 32767 // 799}}], 0.3)
                    qmp([{"type": "btn", "data": {"button": "left", "down": True}}], 0.3)
                    for i in range(1, 9):
                        qmp([{"type": "abs", "data": {"axis": "x", "value": (x1 + (x2 - x1) * i // 8) * 32767 // 1279}},
                             {"type": "abs", "data": {"axis": "y", "value": (y1 + (y2 - y1) * i // 8) * 32767 // 799}}], 0.1)
                    time.sleep(0.5)
                    if len(w) > 5:
                        monitor(f"screendump {w[5]}", 1.5)
                    qmp([{"type": "btn", "data": {"button": "left", "down": False}}], 0.6)
                elif w[0] == "dclick":	# two clicks 0.2 s apart (flui counts a double click within 400 ms)
                    for _ in range(2):
                        qmp([{"type": "btn", "data": {"button": "left", "down": True}}], 0.03)
                        qmp([{"type": "btn", "data": {"button": "left", "down": False}}], 0.03)
                    time.sleep(0.5)
                elif w[0] in ("greeter", "desktop"):
                    if not guest_ok(con, "greeter", "for i in $(seq 90); do pgrep -x flgreet-ui && exit 0; sleep 1; done; exit 1"):
                        fail("the login screen (flgreet-ui) did not come up")
                    if w[0] == "greeter":
                        time.sleep(3)
                        continue
                    time.sleep(3)
                    monitor("sendkey ret")
                    if not guest_ok(con, "panel", "for i in $(seq 60); do pgrep -x flpanel && exit 0; sleep 1; done; exit 1"):
                        fail("no desktop (flpanel) 60 s after Enter on the login screen")
                    time.sleep(3)
                elif w[0] == "poweroffwait":
                    t0 = time.monotonic()
                    end = t0 + float(w[1])
                    seen = ""
                    while time.monotonic() < end and qemu.poll() is None:
                        con.sock.settimeout(0.5)
                        try:
                            data = con.sock.recv(4096)
                        except socket.timeout:
                            continue
                        except OSError:
                            break
                        if not data:
                            break
                        chunk = con.dec.decode(data)
                        log.write(chunk)
                        log.flush()
                        seen += chunk
                    for _ in range(20):
                        if qemu.poll() is not None:
                            break
                        time.sleep(0.1)
                    took = time.monotonic() - t0
                    gone = qemu.poll() is not None
                    print("@poweroffwait:", "exited after %.0f s" % took if gone else "NOT exited after %.0f s" % took)
                    if not gone:
                        fail("the machine did not power off within %s s" % w[1])
                    else:
                        if took > 20:
                            fail("power-off took %.0f s (budget 20 s)" % took)
                        if "Powering off" not in seen and "Rebooting" not in seen:
                            fail("the console never showed 'Powering off.' (rc.shutdown did not run to its end)")
                    break
                continue
            con.send(line)
            out = con.expect(":~#", 120)
            body = ANSI.sub("", out).replace("\r", "").splitlines()
            print("$ " + line)
            print("\n".join(body[1:]))
    finally:
        if not args.keep and qemu.poll() is None:
            qemu.terminate()
            try:
                qemu.wait(10)
            except Exception:
                qemu.kill()

print("RESULT:", "FAIL (%d): %s" % (len(FAILS), "; ".join(FAILS)) if FAILS else "PASS")
sys.exit(1 if FAILS else 0)
