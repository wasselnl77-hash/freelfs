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
    @poweroffwait SECS    wait up to SECS for QEMU to exit (prints exited/NOT exited)
Console transcript: $FLFS_WORK/logs/live-run.log. Exit 0 when the VM was driven to the end.
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
                elif w[0] == "shot":
                    monitor(f"screendump {w[1]}", 1.5)
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
                elif w[0] == "desktop":
                    con.send("for i in $(seq 60); do pgrep -x flgreet-ui >/dev/null && break; sleep 1; done; echo greeter-up")
                    con.expect("greeter-up", 90)
                    con.expect(":~#", 10)
                    time.sleep(3)
                    monitor("sendkey ret")
                    time.sleep(15)
                elif w[0] == "poweroffwait":
                    end = time.monotonic() + float(w[1])
                    while time.monotonic() < end and qemu.poll() is None:
                        time.sleep(0.5)
                    print("@poweroffwait:", "exited" if qemu.poll() is not None else "NOT exited")
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
