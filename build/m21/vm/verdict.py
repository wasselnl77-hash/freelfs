#!/usr/bin/env python3
"""verdict.py RUN : did the login flow end with a live, painting page? exit 0 yes, 1 no (prints why)"""
import json, sys, time, urllib.request
run = sys.argv[1]
d = json.load(urllib.request.urlopen("http://localhost:8080/status?run=" + run))
st = {s for _, s, _ in d}
need = ["click-signin", "popup-opened", "chooser-shown", "popup-done", "welcome-loaded", "api-me"]
missing = [n for n in need if n not in st]
hb = [(t, x) for t, s, x in d if s == "heartbeat" and x.startswith("client.test")]
frames = []
for t, x in hb[-6:]:
    try: frames.append(int(x.split("frames=")[1].split()[0]))
    except Exception: pass
age = time.time() - hb[-1][0] if hb else 999
painting = len(frames) >= 3 and frames[-1] > frames[-3] + 10
vis = hb[-1][1].split("vis=")[1] if hb and "vis=" in hb[-1][1] else "?"
print("visibility", vis, end="; ")
print("stages missing: %s; last heartbeat %.1f s ago; frames %s; painting=%s" % (missing or "none", age, frames, painting))
sys.exit(0 if not missing and painting and age < 5 else 1)
