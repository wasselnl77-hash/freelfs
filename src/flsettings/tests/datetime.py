#!/usr/bin/env python3
# Headless desktop test of Settings > Date & time: flcomp headless (as src/flcomp/tests/apps.py),
# a click on the world map, the search field, the 12/24 h choice and the automatic time switch,
# with a test build of the fltime-set helper that writes into a scratch directory.
# SPDX-License-Identifier: MIT
#
#   FLCOMP=path/to/flcomp FLSETTINGS=path/to/flsettings FL_ZONEINFO=/usr/share/zoneinfo \
#   APPS_OUT=docs/m7 python3 -I datetime.py
#
# Needs src/fltime/fltime-set.c (compiled here with -DFLTIME_TEST -DNO_CLOCK) and a C compiler (CC).
import importlib.util, math, os, subprocess, sys, tempfile, time

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("apps", os.path.join(here, "..", "..", "flcomp", "tests", "apps.py"))
apps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(apps)


def ne(lon, lat):
    """Natural Earth projection, as libflui draws it (radians in)"""
    p2 = lat * lat
    p4 = p2 * p2
    x = lon * (0.8707 - 0.131979 * p2 + p4 * (-0.013791 + p4 * (0.003971 * p2 - 0.001529 * p4)))
    y = lat * (1.007226 + p2 * (0.015085 + p4 * (-0.044475 + 0.028874 * p2 - 0.005916 * p4)))
    return x, y


def map_pixel(lon_deg, lat_deg, win_w=780):
    """pixel inside the window (content at 1,36; side bar 168; margins 28; search row)"""
    w = win_w - 2 - 168 - 56
    xmax = ne(math.pi, 0)[0]
    yn, ys = ne(0, math.radians(84))[1], ne(0, math.radians(-90))[1]
    sc = min(w / (2 * xmax), int((yn - ys) / (2 * xmax) * w + 0.5) / (yn - ys))
    pw = int(2 * xmax * sc)
    x0 = 1 + 168 + 28 + (w - pw) // 2
    y0 = 36 + 24 + 28 + 36 + 10
    x, y = ne(math.radians(lon_deg), math.radians(lat_deg))
    return int(x0 + (x + xmax) * sc), int(y0 + (yn - y) * sc)


def main():
    cc = os.environ.get("CC", "cc")
    src = os.path.join(here, "..", "..", "fltime", "fltime-set.c")
    tmp = tempfile.mkdtemp(prefix="dt-")
    zi = os.environ.get("FL_ZONEINFO", "/usr/share/zoneinfo")
    etc, initd = os.path.join(tmp, "etc"), os.path.join(tmp, "init.d")
    os.makedirs(etc)
    os.makedirs(initd)
    with open(os.path.join(initd, "ntp"), "w") as f:
        f.write("#!/bin/sh\necho \"$1\" >> %s/service.log\n" % tmp)
    os.chmod(os.path.join(initd, "ntp"), 0o755)
    helper = os.path.join(tmp, "fltime-set")
    subprocess.check_call(cc.split() + ["-std=c17", "-D_GNU_SOURCE", "-DFLTIME_TEST", "-DNO_CLOCK", '-DZONEINFO="%s"' % zi,
                                        '-DETCDIR="%s"' % etc, '-DINITD="%s"' % initd, '-DHWCLOCK="/bin/true"', "-o", helper, src])
    with open(os.path.join(etc, "timezone"), "w") as f:
        f.write("Europe/Berlin\n")
    check = apps.check
    dk = apps.Desktop()
    conf = os.path.join(dk.home, ".config", "freelfs", "desktop.conf")  # never the real ~/.config
    try:
        os.environ["FLCOMP"] = os.environ.get("FLCOMP", "flcomp")
        p = dk.start([os.environ.get("FLSETTINGS", "flsettings"), "--page", "datetime"], FLTIME_SET=helper,
                     FLTIME_CONF=os.path.join(etc, "fltime.conf"), FLTIME_STATUS=os.path.join(tmp, "none"),
                     FL_TZ_ETC=etc, FL_ZONEINFO=zi, FL_NOW="1794234720",
                     XDG_CONFIG_HOME=os.path.join(dk.home, ".config"), FLUI_CONF=conf)
        w, h, shot = dk.shot("settings-datetime")
        ox, oy = dk.origin(shot) or (0, 0)
        check("datetime: the pin is drawn (accent colour)", shot.find(bytes.fromhex("3f9b63")) >= 0)
        # Tokyo: 35.65 N, 139.74 E
        tx, ty = map_pixel(139.74, 35.65)
        dk.click(ox + tx, oy + ty)
        time.sleep(0.4)
        tz = open(os.path.join(etc, "timezone")).read().strip()
        check("datetime: a click on Tokyo chooses Asia/Tokyo", tz == "Asia/Tokyo", tz)
        check("datetime: /etc/localtime points into zoneinfo", os.readlink(os.path.join(etc, "localtime")) == zi + "/Asia/Tokyo")
        check("datetime: tz_changed written for the panel", os.path.exists(conf) and "tz_changed=" in open(conf).read())
        # Sao Paulo, -23.55 -46.63
        sx, sy = map_pixel(-46.63, -23.55)
        dk.send("move %d %d" % (ox + sx, oy + sy), 0.3)
        w, h, shot = dk.shot("settings-datetime-hover")
        dk.click(ox + sx, oy + sy)
        time.sleep(0.4)
        tz = open(os.path.join(etc, "timezone")).read().strip()
        check("datetime: a click on Sao Paulo chooses America/Sao_Paulo", tz == "America/Sao_Paulo", tz)
        # search: click in the field, type, Enter
        dk.click(ox + 300, oy + 106)
        dk.type("berl")
        time.sleep(0.3)
        w, h, shot = dk.shot("settings-datetime-search")
        dk.key("enter")
        time.sleep(0.4)
        tz = open(os.path.join(etc, "timezone")).read().strip()
        check("datetime: searching 'berl' and Enter chooses Europe/Berlin", tz == "Europe/Berlin", tz)
        dk.click(ox + 300, oy + 106)
        dk.type("new york")
        time.sleep(0.2)
        dk.key("down")
        dk.key("up")
        dk.key("enter")
        time.sleep(0.4)
        tz = open(os.path.join(etc, "timezone")).read().strip()
        check("datetime: 'new york' chooses America/New_York", tz == "America/New_York", tz)
        # a click on a suggestion
        dk.click(ox + 300, oy + 106)
        dk.type("tokyo")
        time.sleep(0.3)
        dk.click(ox + 300, oy + 106 + 36 + 18)
        time.sleep(0.4)
        tz = open(os.path.join(etc, "timezone")).read().strip()
        check("datetime: clicking the first suggestion chooses Asia/Tokyo", tz == "Asia/Tokyo", tz)
        # 12-hour clock: the row is below the map (y = 36 + 24 + 28 + 46 + map + ...); use the keyboard instead
        dk.click(ox + 300, oy + 106)
        dk.key("tab")  # leaves the field: next is 24-hour
        dk.key("tab")  # 12-hour
        dk.key("space")
        time.sleep(0.4)
        check("datetime: 12-hour choice written", os.path.exists(conf) and "clock_24h=0" in open(conf).read())
        dk.key("tab")  # Sync now (ntp is on by default)
        dk.key("tab")  # the switch
        dk.key("space")
        time.sleep(0.5)
        c = open(os.path.join(etc, "fltime.conf")).read() if os.path.exists(os.path.join(etc, "fltime.conf")) else ""
        check("datetime: the switch turns automatic time off", "NTP=0" in c, c)
        log = open(os.path.join(tmp, "service.log")).read() if os.path.exists(os.path.join(tmp, "service.log")) else ""
        check("datetime: ... and stops the service", log.split()[-1:] == ["stop"], log)
        check("datetime: memory under 30 MB", apps.rss_kb(p.pid) < 30000, "%d kB" % apps.rss_kb(p.pid))
    finally:
        dk.stop()
    print("all tests passed" if not apps.fails else "%d test(s) failed" % apps.fails)
    return apps.fails


if __name__ == "__main__":
    sys.exit(main())
