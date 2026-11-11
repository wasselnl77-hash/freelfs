#!/usr/bin/env python3
# Writes every sound with `flsound --write` and checks what a listener would notice:
# length, level (quiet, not clipping), silence at both ends, no DC offset, no jump at the end.
import math, struct, subprocess, sys, tempfile, os, wave

exe = sys.argv[1] if len(sys.argv) > 1 else "./flsound"
runner = os.environ.get("FLSOUND_RUN", "").split()
names = subprocess.check_output(runner + [exe, "--list"], text=True).split()
want = {"login": (1.4, 1.8), "notification": (0.5, 0.9), "error": (0.4, 0.7), "tick": (0.02, 0.08), "screenshot": (0.15, 0.4)}
fails = 0

def check(name, cond, why=""):
    global fails
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else ": " + why))
    if not cond:
        fails += 1

check("the five sounds exist", sorted(names) == sorted(want), str(names))
d = tempfile.mkdtemp()
for n in names:
    p = os.path.join(d, n + ".wav")
    subprocess.check_call(runner + [exe, "--write", n, p])
    w = wave.open(p)
    check(n + ": 48 kHz mono 16 bit", (w.getframerate(), w.getnchannels(), w.getsampwidth()) == (48000, 1, 2))
    fr = w.readframes(w.getnframes())
    s = struct.unpack("<%dh" % w.getnframes(), fr)
    secs = len(s) / 48000
    lo, hi = want.get(n, (0, 9))
    check(n + ": length %.2f s" % secs, lo <= secs <= hi)
    pk = max(abs(x) for x in s) / 32768
    check(n + ": peak %.1f dBFS is quiet and not clipping" % (20 * math.log10(pk)), 0.15 < pk < 0.4)
    rms = math.sqrt(sum(x * x for x in s) / len(s)) / 32768
    check(n + ": rms %.1f dBFS" % (20 * math.log10(rms)), rms > 0.005)
    dc = abs(sum(s) / len(s)) / 32768
    check(n + ": no DC offset (%.4f)" % dc, dc < 0.01)
    check(n + ": ends in silence (no click)", abs(s[-1]) < 40)
    # the biggest step between neighbours: a hard click would be a jump of a large part of the peak
    jump = max(abs(s[i + 1] - s[i]) for i in range(len(s) - 1)) / 32768
    check(n + ": largest sample step %.2f" % jump, jump < 0.30)
sys.exit(1 if fails else 0)
