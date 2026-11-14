#!/usr/bin/env python3
"""Rasterize a subset of the Papirus icon theme (SVG, upstream) to PNG, as ONE reproducible tarball
for the recipe `papirus-icon-theme`.

Build-host step: flpkg builds inside the chroot, which has no SVG renderer, and FreeLFS ships none at run time
(flpanel looks up PNG only). This script is NOT part of the system; it only produces papirus-png-VERSION.tar.xz,
whose sha256 the recipe pins.

  build/papirus-raster.py UPSTREAM.tar.gz OUT.tar.xz [--rsvg PATH]

The upstream tarball is the official release (checksum in the recipe). Subset: apps and categories (a curated list
plus the generic names), places (the folders, user, network, start-here), mimetypes and devices at 16, 22, 24, 32 and
48 px, same Papirus/NxN/context/name.png layout; aliases stay symlinks. Needs python3 and an rsvg-convert (any
2.5x; the GNOME flatpak runtime has one: pass --rsvg, LD_LIBRARY_PATH is set from its path).
SPDX-License-Identifier: MIT (this script); the icons are GPL-3.0 (Papirus Development Team).
"""
import os, re, subprocess, sys, tarfile, tempfile, shutil, io
from concurrent.futures import ThreadPoolExecutor

SIZES = [16, 22, 24, 32, 48]
ROOT = None
APPS = """
accessories-calculator accessories-text-editor accessories-screenshot accessories-archiver applets-screenshooter
application-pdf applications-graphics applications-multimedia applications-system applications-utilities applications-internet
applications-office applications-development applications-games applications-accessories applications-other
archive-manager file-roller org.gnome.FileRoller system-file-manager org.gnome.Nautilus org.kde.dolphin thunar pcmanfm
utilities-terminal utilities-system-monitor utilities-log-viewer org.gnome.SystemMonitor org.gnome.Terminal terminal foot
preferences-system preferences-desktop preferences-desktop-theme preferences-desktop-wallpaper preferences-desktop-display
preferences-desktop-sound preferences-desktop-keyboard preferences-desktop-locale preferences-system-network
preferences-system-bluetooth preferences-system-time preferences-system-power preferences-system-printers
preferences-desktop-accessibility preferences-desktop-default-applications preferences-other preferences-color
system-software-install system-software-update system-run system-search system-users system-lock-screen system-shutdown
system-reboot system-log-out system-help gnome-software org.gnome.Software
web-browser internet-web-browser firefox firefox-esr org.mozilla.firefox chromium google-chrome brave-browser
multimedia-video-player multimedia-audio-player multimedia-volume-control multimedia-photo-viewer image-viewer
org.gnome.Loupe eog vlc mpv org.videolan.VLC org.gnome.Totem rhythmbox spotify
evince org.gnome.Evince okular gimp org.gimp.GIMP inkscape krita libreoffice-writer libreoffice-calc libreoffice-impress
libreoffice-startcenter thunderbird org.mozilla.Thunderbird geany code visual-studio-code vscode org.gnome.gedit gedit
org.gnome.TextEditor kate mousepad
gvim vim package-x-generic package preferences-desktop-font help-browser help-about dialog-information
bluetooth blueman org.gnome.Settings gnome-settings gnome-control-center
steam discord telegram signal-desktop org.telegram.desktop com.valvesoftware.Steam
libreoffice-main obs obs-studio com.obsproject.Studio blender audacity kdenlive org.kde.kdenlive
org.gnome.Calculator org.gnome.Calendar org.gnome.Contacts org.gnome.Maps org.gnome.clocks org.gnome.Weather
org.gnome.Characters org.gnome.Logs org.gnome.baobab baobab org.gnome.Cheese org.gnome.Console
bitwarden keepassxc org.keepassxc.KeePassXC transmission transmission-gtk qbittorrent
com.github.tchx84.Flatseal flatseal io.missioncenter.MissionCenter missioncenter it.mijorus.smile smile
org.mozilla.firefox com.brave.Browser org.chromium.Chromium com.google.Chrome io.gitlab.librewolf-community librewolf
org.libreoffice.LibreOffice org.videolan.VLC org.gimp.GIMP org.inkscape.Inkscape org.kde.krita org.audacityteam.Audacity
""".split()
APP_PREFIX = ("applications-", "preferences-", "utilities-", "accessories-", "system-", "multimedia-")
PLACES_RE = re.compile(r"^(folder(-open|-documents|-download|-downloads|-music|-pictures|-videos|-publicshare|-templates|-remote|-recent|-saved-search|-desktop|-development)?|"
                       r"user-(home|desktop|trash|trash-full|bookmarks|info)|network-.*|start-here.*|distributor-logo.*|desktop|bookmark-missing|"
                       r"inode-directory|bookmarks|computer|gtk-directory|folder-activities)$")


def selected(ctx, name):
    if ctx in ("apps", "categories"):
        return name in APPS or name.startswith(APP_PREFIX)
    if ctx == "places":
        return bool(PLACES_RE.match(name))
    return True  # mimetypes, devices


def main():
    args = [a for a in sys.argv[1:]]
    rsvg = "rsvg-convert"
    if "--rsvg" in args:
        i = args.index("--rsvg")
        rsvg = args[i + 1]
        del args[i:i + 2]
    tarball, out = args
    env = dict(os.environ)
    base = os.path.dirname(os.path.dirname(os.path.realpath(rsvg)))
    env["LD_LIBRARY_PATH"] = f"{base}/lib:{base}/lib/x86_64-linux-gnu"
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.check_call(["tar", "xzf", tarball, "-C", tmp, "--wildcards", "*/Papirus/*", "*/LICENSE", "*/AUTHORS"], stderr=subprocess.DEVNULL)
        top = os.path.join(tmp, os.listdir(tmp)[0])
        src = os.path.join(top, "Papirus")
        dst = os.path.join(tmp, "stage")
        outroot = os.path.join(dst, "Papirus")
        jobs, links = [], []
        for s in SIZES:
            for ctx in ("apps", "places", "mimetypes", "devices"):
                d = os.path.join(src, f"{s}x{s}", ctx)
                if not os.path.isdir(d):
                    continue
                od = os.path.join(outroot, f"{s}x{s}", ctx)
                os.makedirs(od, exist_ok=True)
                # first the aliases, so that their real targets are rendered too
                for fn in sorted(os.listdir(d)):
                    if not fn.endswith(".svg"):
                        continue
                    nm = fn[:-4]
                    if not selected(ctx, nm):
                        continue
                    p = os.path.join(d, fn)
                    if os.path.islink(p):
                        tgt = os.readlink(p)
                        real = os.path.realpath(p)
                        if "/" not in tgt and os.path.isfile(real):
                            links.append((od, nm, tgt[:-4]))
                            tn = tgt[:-4]
                            jobs.append((real, os.path.join(od, tn + ".png"), s))
                        elif os.path.isfile(real):
                            jobs.append((real, os.path.join(od, nm + ".png"), s))
                    elif os.path.isfile(p):
                        jobs.append((p, os.path.join(od, nm + ".png"), s))
        # render each output once
        seen = {}
        for real, o, s in jobs:
            seen[o] = (real, s)

        def render(item):
            o, (real, s) = item
            subprocess.check_call([rsvg, "-w", str(s), "-h", str(s), "-o", o, real], env=env)
        with ThreadPoolExecutor(max_workers=4) as ex:
            list(ex.map(render, sorted(seen.items())))
        for od, nm, tn in links:
            lp = os.path.join(od, nm + ".png")
            if not os.path.lexists(lp) and os.path.exists(os.path.join(od, tn + ".png")):
                os.symlink(tn + ".png", lp)
        # categories: the same directory as apps, as upstream does
        for s in SIZES:
            c = os.path.join(outroot, f"{s}x{s}", "categories")
            if os.path.isdir(os.path.join(outroot, f"{s}x{s}", "apps")):
                os.symlink("apps", c)
        # index.theme: only the directories we ship
        dirs = []
        for s in SIZES:
            for ctx in ("apps", "categories", "devices", "mimetypes", "places"):
                if os.path.lexists(os.path.join(outroot, f"{s}x{s}", ctx)):
                    dirs.append(f"{s}x{s}/{ctx}")
        ctxname = {"apps": "Applications", "categories": "Categories", "devices": "Devices", "mimetypes": "MimeTypes", "places": "Places"}
        idx = ["[Icon Theme]", "Name=Papirus", "Comment=Papirus icon theme (a PNG subset made for FreeLFS)", "Inherits=hicolor", "",
               "Directories=" + ",".join(dirs), ""]
        for d in dirs:
            s, ctx = d.split("/")
            idx += [f"[{d}]", f"Context={ctxname[ctx]}", f"Size={s.split('x')[0]}", "Type=Fixed", ""]
        with open(os.path.join(outroot, "index.theme"), "w") as f:
            f.write("\n".join(idx))
        for fn in ("LICENSE", "AUTHORS"):
            shutil.copy(os.path.join(top, fn), os.path.join(outroot, fn))
        import lzma
        names = []
        for dp, dn, fn in os.walk(os.path.join(dst, "Papirus")):
            for x in dn + fn:
                names.append(os.path.relpath(os.path.join(dp, x), dst))
        names.append("Papirus")
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w", format=tarfile.GNU_FORMAT) as tf:
            for nm in sorted(names):
                ti = tf.gettarinfo(os.path.join(dst, nm), arcname=nm)
                ti.uid = ti.gid = 0
                ti.uname = ti.gname = ""
                ti.mtime = 1785592973
                ti.mode = 0o755 if ti.isdir() else 0o777 if ti.issym() else 0o644
                if ti.isreg():
                    with open(os.path.join(dst, nm), "rb") as fh:
                        tf.addfile(ti, fh)
                else:
                    tf.addfile(ti)
        with open(out, "wb") as fo:
            fo.write(lzma.compress(buf.getvalue(), preset=9))
        n = sum(1 for _ in jobs)
        print(f"{len(seen)} PNG files, {len(links)} links -> {out}")


main()
