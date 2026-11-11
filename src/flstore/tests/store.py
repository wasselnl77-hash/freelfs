#!/usr/bin/env python3
# Headless test of the Software Store: flcomp with FLCOMP_HEADLESS, the store against real Flathub data
# (appstream, icons, screenshots over the network) and stand-ins for flatpak, flpkg and flpkg-seat.
# SPDX-License-Identifier: MIT
#
#   STORE_BIN=/path/to/flstore STORE_RUN=wrapper-that-runs-sysroot-binaries FLCOMP=... \
#   STORE_APPSTREAM=appstream.xml.gz (optional: skips the 11 MB download) \
#   APPS_OUT=docs/m7 python3 -I store.py
#
# Everything lives in a scratch directory: HOME, XDG_* and the cache point there.
import os, sys, time, subprocess, shutil

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, "..", "..", "flcomp", "tests"))
import apps  # noqa: E402
from apps import Desktop, check, rss_kb, W, H  # noqa: E402

FAKE_FLATPAK = r"""#!/bin/sh
# stands in for flatpak: installed apps and updates are state files, every call is logged
echo "flatpak $*" >> "$FP_LOG"
cmd=$1; shift
nm() { grep "^$1|" "$FP_NAMES" | cut -d'|' -f2; }
case "$cmd" in
remotes) echo flathub ;;
remote-ls)
	for a in "$@"; do [ "$a" = --updates ] && { cat "$FP_UPD"; exit 0; }; done
	exit 0 ;;
list)
	while IFS='|' read -r id ver; do [ -n "$id" ] && printf '%s\t%s\t%s\n' "$id" "$(nm $id)" "$ver"; done < "$FP_INST" ;;
remote-info|info)
	printf '        ID: x\n'
	if [ "$cmd" = remote-info ]; then
		printf ' Download Size: 130.9 MB\nInstalled Size: 350.4 MB\n   Runtime: org.freedesktop.Platform/x86_64/25.08\n'
	else
		printf 'Installed Size: 350.4 MB\n   Runtime: org.freedesktop.Platform/x86_64/25.08\n'
	fi ;;
install)
	id=""; for a in "$@"; do case $a in -*|flathub) ;; *) id=$a ;; esac; done
	echo "Looking for matches…"
	if [ "$id" = com.example.Broken ]; then echo "error: Failed to install $id: Server returned status 404" >&2; exit 1; fi
	echo "Installing 1/3…"; sleep 1.2
	echo "Installing 2/3…"; sleep 1.2
	echo "Installing 3/3…"; sleep 1.2
	grep -q "^$id|" "$FP_INST" || echo "$id|1.0" >> "$FP_INST"
	echo "Installation complete." ;;
uninstall)
	id=""; for a in "$@"; do case $a in -*) ;; *) id=$a ;; esac; done
	echo "Uninstalling…"; sleep 1
	[ -n "$id" ] && { grep -v "^$id|" "$FP_INST" > "$FP_INST.new"; mv "$FP_INST.new" "$FP_INST"; }
	echo "Uninstall complete." ;;
update)
	echo "Updating 1/1…"; sleep 1.2
	: > "$FP_UPD"
	echo "Updates complete." ;;
run) ;;
esac
exit 0
"""

FAKE_FLPKG = r"""#!/bin/sh
# stands in for flpkg: a repo with one package that is outdated
[ "$1" = --config ] && shift 2
echo "flpkg $*" >> "$FP_LOG"
case "$1" in
list) cat "$FP_PKSTATE" ;;
search)
	printf 'core/editor 1.2-1\n    A small text editor\n'
	printf 'core/imageviewer 0.9-2\n    Look at pictures\n'
	printf 'core/musicplayer 3.1-1\n    Plays music with PipeWire\n'
	printf 'core/bash 5.3-2 [installed: older]\n    The GNU Bourne Again shell\n'
	printf 'core/zlib 1.3.2-1 [installed]\n    Compression library\n' ;;
info)
	n=$2; [ "$2" = --repo ] && n=$3
	printf '%-14s: %s\n' Name "$n" Version 1.2-1 Summary "A package" URL "https://example.org/$n" Licenses MIT \
		"Depends on" "glibc  zlib" "Installed size" "812.0 KiB" ;;
esac
exit 0
"""

FAKE_SEAT = r"""#!/bin/sh
echo "seat $*" >> "$FP_LOG"
echo "Resolving dependencies for $2"; sleep 1.2
echo "Installing $2"; sleep 0.8
echo "Done"
"""

FAKE_CURL = r"""#!/bin/sh
# stands in for curl on the first start: the catalog download is slow and comes from a local file
# (STORE_APPSTREAM); everything else (icons, screenshots) goes to the real curl
out=""; url=""
while [ $# -gt 0 ]; do
	case $1 in -o) out=$2; shift ;; -*) ;; *) url=$1 ;; esac
	shift
done
case "$url" in
*appstream.xml.gz) sleep 9; cp "$FP_APPSTREAM" "$out" ;;
esac
""" 

NAMES = """org.videolan.VLC|VLC
org.gimp.GIMP|GNU Image Manipulation Program
org.mozilla.firefox|Firefox
com.obsproject.Studio|OBS Studio
"""


def main():
    binp = os.environ["STORE_BIN"]
    run = os.environ.get("STORE_RUN", "")
    wrap = [run] if run else []
    dk = Desktop()
    p = None
    try:
        d = dk.dir
        for name, text in (("flatpak", FAKE_FLATPAK), ("flpkg", FAKE_FLPKG), ("flpkg-seat", FAKE_SEAT)):
            with open(os.path.join(d, name), "w") as f:
                f.write(text)
            os.chmod(os.path.join(d, name), 0o755)
        st = {n: os.path.join(d, "fp." + n) for n in ("log", "inst", "upd", "names", "pkstate")}
        open(st["log"], "w").close()
        with open(st["inst"], "w") as f:
            f.write("org.videolan.VLC|3.0.20\norg.gimp.GIMP|3.0.0\n")
        with open(st["upd"], "w") as f:
            f.write("org.videolan.VLC\n")
        with open(st["names"], "w") as f:
            f.write(NAMES)
        with open(st["pkstate"], "w") as f:
            f.write("bash 5.3-1\nzlib 1.3.2-1\nglibc 2.43-1\n")
        cache = os.path.join(d, "cache")
        os.makedirs(os.path.join(cache, "flstore"))
        conf = os.path.join(d, "flpkg.conf")
        with open(conf, "w") as f:
            f.write("repo core file:///srv/flpkg/core\n")
        selfsh = os.path.join(d, "self.sh")
        with open(selfsh, "w") as f:
            f.write("#!/bin/sh\nexec %s\n" % " ".join(wrap + [os.path.join(binp, "flstore"), '"$@"']))
        os.chmod(selfsh, 0o755)
        env = dict(FLSTORE_FLATPAK=os.path.join(d, "flatpak"), FLSTORE_FLPKG=os.path.join(d, "flpkg"),
                   FLSTORE_SEAT=os.path.join(d, "flpkg-seat"), FLSTORE_CONF=conf, FLSTORE_SELF=selfsh,
                   XDG_CACHE_HOME=cache, XDG_DATA_HOME=os.path.join(dk.home, ".local", "share"),
                   FP_LOG=st["log"], FP_INST=st["inst"], FP_UPD=st["upd"], FP_NAMES=st["names"],
                   FP_PKSTATE=st["pkstate"])

        def calls():
            with open(st["log"]) as f:
                return f.read().splitlines()

        def installed():
            with open(st["inst"]) as f:
                return [l.split("|")[0] for l in f.read().splitlines()]

        def where(shot):
            acc = bytes.fromhex("3f9b63")
            i = shot.find(acc)
            while i >= 0 and i % 3:
                i = shot.find(acc, i + 1)
            return (i // 3) % W + 1, (i // 3) // W + 36

        # ---- offline with nothing cached: the error page, a way to try again
        d2 = os.path.join(d, "cache-offline")
        os.makedirs(d2)
        p = dk.start(wrap + [os.path.join(binp, "flstore")], **dict(env, XDG_CACHE_HOME=d2, FLSTORE_CURL="/bin/false"))
        time.sleep(4)
        w, h, shot = dk.shot("store-0-offline")
        check("store: offline with an empty cache the app says so and stays up", p.poll() is None)
        p.terminate()
        p.wait(5)
        dk.procs.remove(p)

        # ---- the first start: no index yet, so the catalog is fetched (slowly here) and built
        argv = wrap + [os.path.join(binp, "flstore")]
        fakecurl = os.path.join(d, "curl")
        with open(fakecurl, "w") as f:
            f.write(FAKE_CURL.replace('"$FP_APPSTREAM"', '"%s"' % os.environ["STORE_APPSTREAM"]) if os.environ.get("STORE_APPSTREAM") else "#!/bin/sh\nexec /usr/bin/curl \"$@\"\n")
        os.chmod(fakecurl, 0o755)
        env1 = dict(env, FLSTORE_CURL=fakecurl)
        p = dk.start(argv, **env1)
        time.sleep(3.5)
        w, h, shot = dk.shot("store-0-first-start")
        for _ in range(120):  # up to 2 minutes for the download and the index
            if os.path.exists(os.path.join(cache, "flstore", "index.tsv")):
                break
            time.sleep(1)
        check("store: the catalog index was built", os.path.exists(os.path.join(cache, "flstore", "index.tsv")))
        time.sleep(2.5)
        w, h, shot = dk.shot("store-0-first-start-done")
        p.terminate()
        p.wait(5)
        dk.procs.remove(p)
        # ---- the next start loads the index at once; icons arrive as the cards are shown
        p = dk.start(argv, **env)
        time.sleep(7)
        w, h, shot = dk.shot("store-1-home")
        cx, cy = where(shot)
        check("store: memory under 40 MB with the catalog loaded", rss_kb(p.pid) < 40000, "%d kB" % rss_kb(p.pid))
        n = len(os.listdir(os.path.join(cache, "flstore", "icons64")))
        check("store: icons were downloaded for the visible cards only", 0 < n < 40, str(n))

        # ---- a category
        dk.click(cx + 60, cy + 52 + 95)  # Games
        time.sleep(4)
        dk.shot("store-2-category")
        # ---- search
        dk.click(cx + 800, cy + 26)
        dk.type("video")
        time.sleep(4)
        dk.shot("store-3-search")
        dk.key("a", "ctrl")
        dk.key("bksp")
        time.sleep(0.8)
        # ---- Home, then an app page
        dk.click(cx + 60, cy + 52 + 28)
        time.sleep(1)
        dk.click(cx + 700, cy + 52 + 80)  # Firefox
        time.sleep(7)
        dk.shot("store-4-app")
        shots = os.listdir(os.path.join(cache, "flstore", "shots"))
        check("store: only the first screenshot was downloaded", len(shots) == 1, str(shots))
        dk.key("right")
        time.sleep(4)
        dk.shot("store-5-app-screenshot2")
        shots = os.listdir(os.path.join(cache, "flstore", "shots"))
        check("store: the next screenshot is fetched on demand", len(shots) == 2, str(shots))
        # ---- the text part of the page
        dk.send("move %d %d" % (cx + 600, cy + 400))
        dk.send("axis 7", 0.5)
        time.sleep(0.5)
        dk.shot("store-5b-app-about")
        dk.send("axis -20", 0.5)
        time.sleep(0.5)
        # ---- install
        dk.click(cx + 214 + 28 + 96 + 22 + 60, cy + 52 + 22 + 84 + 17)
        time.sleep(2)
        dk.shot("store-6-installing")
        check("store: Install runs flatpak install --user for the app id",
              any(c.startswith("flatpak install --user -y --noninteractive flathub org.mozilla.firefox") for c in calls()),
              str(calls()))
        time.sleep(5)
        dk.shot("store-7-installed")
        check("store: the app is installed afterwards", "org.mozilla.firefox" in installed(), str(installed()))
        # ---- Installed and Updates
        dk.click(cx + 172, cy + 26)
        time.sleep(1.5)
        dk.shot("store-8-installed")
        dk.click(cx + 250, cy + 26)
        time.sleep(2)
        dk.shot("store-9-updates")
        check("store: updates come from flatpak and flpkg",
              any("--updates" in c for c in calls()) and any(c.startswith("flpkg search") for c in calls()), str(calls()))
        dk.click(cx + 960 - 28 - 110 - 124 + 50, cy + 52 + 33)  # Update all
        time.sleep(3.5)
        dk.shot("store-10-updating")
        check("store: Update all updates the flatpak apps and the outdated package",
              "flatpak update --user -y --noninteractive" in calls() and
              any(c == "seat install bash" for c in calls()), str(calls()))
        time.sleep(6)
        # ---- FreeLFS packages
        dk.click(cx + 92, cy + 26)  # Explore
        time.sleep(0.5)
        dk.click(cx + 60, cy + 52 + 10 + 34 * 9 + 34 + 14 + 17)  # FreeLFS packages
        time.sleep(2)
        dk.shot("store-11-packages")
        check("store: still running", p.poll() is None)
    finally:
        if p and p.poll() is None:
            p.terminate()
        dk.stop()
    print("all tests passed" if not apps.fails else "%d test(s) failed" % apps.fails)
    return apps.fails


if __name__ == "__main__":
    sys.exit(main())
