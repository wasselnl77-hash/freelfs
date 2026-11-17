#!/usr/bin/env python3
"""g.py [-t SECS] CMD...   run a root shell command in the persistent VM (serial socket ./ser), print output.
   g.py --login          log in as root first (after boot)
   g.py --mon 'cmd'      QEMU monitor
   g.py --key NAME / --shot FILE"""
import socket, sys, time, os, re
d = os.path.dirname(os.path.abspath(__file__))
ANSI = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")
def mon(t, w=1.0):
    m = socket.socket(socket.AF_UNIX); m.connect(d + "/mon"); time.sleep(0.2)
    m.settimeout(2)
    try: m.recv(4096)
    except Exception: pass
    m.sendall(t.encode() + b"\n"); time.sleep(w)
    try: r = m.recv(65536).decode(errors="replace")
    except Exception: r = ""
    m.close(); return r
a = sys.argv[1:]
if a and a[0] == "--mon": print(mon(" ".join(a[1:]))); sys.exit()
W, Hh = int(os.environ.get("XRES", 1920)), int(os.environ.get("YRES", 1080))
def qmp(events):
    import json
    c = socket.socket(socket.AF_UNIX); c.settimeout(5); c.connect(d + "/qmp"); f = c.makefile("rw"); f.readline()
    for q in ({"execute": "qmp_capabilities"}, {"execute": "input-send-event", "arguments": {"events": events}}):
        f.write(json.dumps(q) + "\n"); f.flush(); f.readline()
    c.close()
if a and a[0] == "--click":   # --click X Y [left|right]
    x, y = int(a[1]), int(a[2]); b = a[3] if len(a) > 3 else "left"
    qmp([{"type": "abs", "data": {"axis": "x", "value": x * 32767 // (W - 1)}}, {"type": "abs", "data": {"axis": "y", "value": y * 32767 // (Hh - 1)}}]); time.sleep(0.3)
    qmp([{"type": "btn", "data": {"button": b, "down": True}}]); time.sleep(0.1)
    qmp([{"type": "btn", "data": {"button": b, "down": False}}]); sys.exit()
if a and a[0] == "--key": print(mon("sendkey " + " ".join(a[1:]), 0.5)); sys.exit()
if a and a[0] == "--shot": print(mon("screendump " + a[1], 2.0)); sys.exit()
if a and a[0] == "--put":   # --put LOCAL REMOTE (mode 755)
    import base64, gzip
    sk = socket.socket(socket.AF_UNIX); sk.connect(d + "/ser")
    def cmd(c, t=30):
        sk.sendall((c + "; echo PUT$((40+2))\r").encode()); buf = ""; end = time.time() + t
        while "PUT42" not in buf.split("PUT$", 1)[-1] and time.time() < end:
            sk.settimeout(1)
            try: buf += sk.recv(65536).decode(errors="replace")
            except socket.timeout: pass
    sk.sendall(b"\r"); time.sleep(0.5)
    data = base64.b64encode(gzip.compress(open(a[1], "rb").read(), 6)).decode()
    cmd(": > %s.b64" % a[2])
    for i in range(0, len(data), 3000):
        cmd("printf %%s '%s' >> %s.b64" % (data[i:i + 3000], a[2]))
    cmd("base64 -d %s.b64 | gzip -dc > %s.new; chmod 755 %s.new; mv -f %s.new %s; rm %s.b64" % (a[2], a[2], a[2], a[2], a[2], a[2]))
    print("put", a[2]); sys.exit()
if a and a[0] == "--get":   # --get REMOTE LOCAL
    import base64, gzip
    s = socket.socket(socket.AF_UNIX); s.connect(d + "/ser")
    s.sendall(b"\r"); time.sleep(0.5)
    s.sendall(("gzip -c %s | base64 -w0; echo; echo GETEND$((40+2))\r" % a[1]).encode())
    buf = ""; end = time.time() + 120
    while "GETEND42" not in buf.split("GETEND$", 1)[-1] and time.time() < end:
        s.settimeout(1)
        try: buf += s.recv(1 << 20).decode(errors="replace")
        except socket.timeout: pass
    body = [l for l in ANSI.sub("", buf).replace("\r", "").split("\n") if re.fullmatch(r"[A-Za-z0-9+/=]{20,}", l)]
    open(a[2], "wb").write(gzip.decompress(base64.b64decode(body[-1]))); print("saved", a[2]); sys.exit()
tmo = 60
if a and a[0] == "-t": tmo = int(a[1]); a = a[2:]
s = socket.socket(socket.AF_UNIX); s.connect(d + "/ser")
def rd(until, t):
    buf = ""; end = time.time() + t
    while until not in buf and time.time() < end:
        s.settimeout(0.5)
        try:
            x = s.recv(65536)
            if not x: break
            buf += x.decode(errors="replace")
        except socket.timeout: pass
    return buf
if a and a[0] == "--login":
    s.sendall(b"\r"); b = rd("login:", 5)
    if "login:" in b: s.sendall(b"root\r")
    print(ANSI.sub("", rd(":~#", 20))); sys.exit()
s.sendall(b"\r"); rd(":~#", 3)
tag = "END%d" % int(time.time())
s.sendall((" ".join(a) + "; echo " + tag + "$((40+2))\r").encode())
out = rd(tag + "42", tmo)
out = ANSI.sub("", out).replace("\r", "")
lines = out.split("\n")
print("\n".join(l for l in lines[1:] if tag not in l))
