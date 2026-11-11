#!/usr/bin/env python3
"""Boot the live ISO (or a test disk from iso-test-disk.py) in QEMU/OVMF and check it.

    build/iso-test.py --iso FILE [--mem 1G] [--shots DIR]     as a CD
    build/iso-test.py --disk FILE [--mem 1G] [--shots DIR]    as a disk (a dd'd
                                      stick, or a test disk with the ISO as a file)

Boots with the serial console on a unix socket, answers the first-boot keyboard
question, checks that rc.sysinit printed no [FAIL], logs in as root on the
serial console (the live system has no root password), checks that no disk is
mounted, takes screenshots of the login screen, logs in as tester on the
screen and screenshots the desktop. Everything the console said goes to
$FLFS_WORK/logs/iso-test.log. Exit status 0: all steps passed.
"""
import argparse, importlib.util, os, re, socket, struct, subprocess, sys, tempfile, time, zlib

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("boottest", os.path.join(here, "boot-test.py"))
bt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bt)

ap = argparse.ArgumentParser()
g = ap.add_mutually_exclusive_group(required=True)
g.add_argument("--iso")
g.add_argument("--disk")
ap.add_argument("--mem", default="1G")
ap.add_argument("--shots", default="")
ap.add_argument("--no-desktop", action="store_true")
ap.add_argument("--gui-shots", action="store_true", help="after the desktop and menu shots: start Store, Videos (Ctrl+O, the "
                "portal's file dialog) and the egg as tester, and take a screenshot of each (needs --shots)")
ap.add_argument("--profile", default="", help="after 60 s of idle desktop write the memory profile (meminfo, mounts, "
                "per-process PSS) to this file (not for the memory-poor first minute: it waits)")
args = ap.parse_args()
LOG = os.path.join(bt.WORK, "logs", "iso-test.log")


def ppm_to_png(src, dst):
    d = open(src, "rb").read()
    parts = d.split(None, 4)  # P6 w h max data
    w, h = int(parts[1]), int(parts[2])
    pix = d[len(d) - w * h * 3:]
    raw = b"".join(b"\0" + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, b):
        c = struct.pack(">I", len(b)) + t + b
        return c + struct.pack(">I", zlib.crc32(t + b))
    open(dst, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                          chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


PROFILE = [  # shell lines run as root on the serial console, output kept verbatim
    ("meminfo", "grep -E '^(MemTotal|MemFree|MemAvailable|Buffers|Cached|Active\\(anon\\)|Inactive\\(anon\\)|Active\\(file\\)|Inactive\\(file\\)|AnonPages|Mapped|Shmem|Slab|SReclaimable|SUnreclaim|KernelStack|PageTables|Percpu|Dirty)' /proc/meminfo"),
    ("used = MemTotal - MemAvailable (kB)", "awk '/^MemTotal:/{t=$2} /^MemAvailable:/{a=$2} END{print t-a}' /proc/meminfo"),
    ("MemTotal - MemFree (kB, counts the page cache as used)", "awk '/^MemTotal:/{t=$2} /^MemFree:/{a=$2} END{print t-a}' /proc/meminfo"),
    ("free -m", "free -m"),
    ("mounts of tmpfs, overlay, squashfs", "grep -E ' (tmpfs|overlay|squashfs) ' /proc/mounts"),
    ("df -k (tmpfs, overlay, squashfs)", "df -k | grep -E 'tmpfs|overlay|squashfs|Filesystem'"),
    ("what the overlay holds in RAM (the upper layer, du -k, biggest first)", "du -k -d 3 /run/live/rw/up 2>/dev/null | sort -rn | head -n 12; du -sk /tmp /dev/shm; du -k -d 4 /run/live/rw/up/sysflfs/data/lib /run/live/rw/up/usr/tester/.local 2>/dev/null | sort -rn | head -n 12"),
    ("per-process PSS, top 15 (Pss Pss_Anon Pss_File Pss_Shmem in kB, name, pid)",
     "for p in /proc/[0-9]*; do [ -r $p/smaps_rollup ] || continue; echo \"$(awk '/^Pss:/{a=$2} /^Pss_Anon:/{b=$2} /^Pss_File:/{c=$2} /^Pss_Shmem:/{d=$2} END{print a+0, b+0, c+0, d+0}' $p/smaps_rollup 2>/dev/null) $(cat $p/comm) ${p#/proc/}\"; done | sort -rn | head -n 15"),
    ("sum of all PSS (kB) and number of processes", "for p in /proc/[0-9]*; do [ -r $p/smaps_rollup ] && awk '/^Pss:/{print $2}' $p/smaps_rollup 2>/dev/null; done | awk '{s+=$1; n++} END{print s, n}'"),
    ("loaded kernel modules", "lsmod | wc -l"),
    ("processes by name (count)", "ps -eo comm | sort | uniq -c | sort -rn | head -n 12"),
]


def memory_profile(con, run, path):
    with open(path, "w") as f:
        for title, cmd in PROFILE:
            ok, out = run(cmd, "")
            body = re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]", "", out).replace("\r", "")
            lines = body.splitlines()
            f.write(f"== {title}\n" + "\n".join(lines[1:-1] if len(lines) > 2 else lines) + "\n\n")


def main():
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="flfs-iso-")
    path, mon = os.path.join(tmp, "console"), os.path.join(tmp, "mon")
    env = dict(os.environ, FLFS_VM_SERIAL=f"unix:{path},server=on,wait=on", FLFS_VM_MEM=args.mem,
               FLFS_VM_VARS=os.path.join(tmp, "ovmf-vars.fd"))  # fresh firmware settings: a stale boot order skips the CD
    if args.disk:
        env["FLFS_IMG"] = args.disk
    cmd = [os.path.join(bt.TOP, "build", "51-qemu.sh"), "--serial"]
    if args.iso:
        cmd += ["--iso", args.iso]
    cmd += ["--", "-monitor", f"unix:{mon},server=on,wait=off"]
    fails = 0

    def monitor(text, wait=0.5):
        m = socket.socket(socket.AF_UNIX)
        m.connect(mon)
        m.sendall(text.encode() + b"\n")
        time.sleep(wait)
        m.close()

    def shot(name):
        ppm = os.path.join(tmp, name + ".ppm")
        monitor(f"screendump {ppm}", 1.5)
        if args.shots and os.path.exists(ppm):
            os.makedirs(args.shots, exist_ok=True)
            ppm_to_png(ppm, os.path.join(args.shots, name + ".png"))

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
            t0 = time.monotonic()
            seen = con.expect_any(["First boot", "login:", "could not find the FreeLFS"], 240)
            if seen.startswith("could not"):
                raise RuntimeError("flinitrd did not find the live system")
            if seen == "First boot":
                time.sleep(8)
                shot("live-first-boot")
                monitor("sendkey ret")
            boot = bt.step("boot to login prompt", lambda: con.expect("login:", 240))
            print(f"    ({time.monotonic() - t0:.1f}s)")
            import re
            for line in re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]", "", boot).splitlines():
                if "flinitrd:" in line and "live system" in line:
                    print("    " + line.strip())
                if "[FAIL]" in line or "FAIL]" in line:
                    print(f"    boot step failed: {line.strip()}")
                    fails += 1
            con.send("root")
            bt.step("log in as root", lambda: con.expect(":~#", 30))

            def run(cmd, marker):
                con.send(cmd)
                out = con.expect(":~#", 60)
                ok = marker in out
                print(f"  {cmd[:70]} ... {'ok' if ok else 'FAIL'}")
                if not ok:
                    print("    got: " + out.replace("\r", "")[-300:])
                return ok, out

            checks = [
                ("grep ' / ' /proc/mounts", " / overlay "),
                ("grep -c squashfs /proc/mounts", "1"),
                ("id tester", "uid=1000(tester)"),
                ("ls -ld /usr/tester", "tester"),
                ("test ! -e /boot/vmlinuz* && echo noboot-ok", "noboot-ok"),
                ("cat /etc/fstab | grep -c btrfs; true", "0"),
                ("free -m | sed -n 2p", "Mem:"),
            ]
            for c, m in checks:
                ok, _ = run(c, m)
                fails += not ok
            # the host's disks: the ISO's own device (or the file system holding the ISO
            # file) is mounted under /run/live, read-only, and nothing else of a disk is mounted
            ok, out = run("awk '$1 ~ /^\\/dev\\/(sd|nvme|vd|sr|dm)/ {print \"HOSTDISK\", $1, $2, $4}' /proc/mounts; echo end", "end")
            for line in out.rsplit("end", 1)[0].splitlines():
                if "HOSTDISK" in line and "awk" not in line:
                    f = line.split()
                    if not (f[2].startswith("/run/live/") and f[3].split(",")[0] == "ro"):
                        print("  a block device is mounted other than read-only under /run/live: " + line)
                        fails += 1
                    else:
                        print("    " + " ".join(f[1:]))
            if args.no_desktop:
                pass
            else:
                bt.step("login screen up", lambda: (con.send("for i in $(seq 40); do pgrep -x flgreet-ui >/dev/null && break; sleep 1; done; pgrep -x flgreet-ui >/dev/null && echo greeter-up"), con.expect("greeter-up", 60)))
                con.expect(":~#", 10)
                time.sleep(3)
                shot("live-login")
                monitor("sendkey ret")  # tester has no password
                time.sleep(15)
                shot("live-desktop")
                run("ps -eo rss,comm | grep -E 'flcomp|flpanel' | head -4; free -m | sed -n 2p", "Mem:")
                if args.profile:
                    time.sleep(60)
                    memory_profile(con, run, args.profile)
                monitor("sendkey meta_l")
                time.sleep(2)
                shot("live-menu")
                if args.gui_shots:
                    monitor("sendkey meta_l")
                    time.sleep(1)

                    def gui(cmd, wait):
                        env = ("export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 "
                               "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus XDG_CURRENT_DESKTOP=FreeLFS; ")
                        run(f"su -s /bin/sh tester -c '{env}setsid sh -c \"{cmd}\" >/tmp/gui.log 2>&1 </dev/null &'; echo started", "started")
                        time.sleep(wait)

                    gui("flstore", 8)
                    shot("live-store")
                    run("pkill -x flstore; echo gone", "gone")
                    run("ls -d /run/live/rw/up/usr/tester/.local/share/flatpak 2>&1 | tail -1; du -sk /run/live/rw/up | cut -f1", "")
                    gui("flvideo", 4)
                    monitor("sendkey ctrl-o")
                    time.sleep(8)
                    shot("live-video-open")
                    run("pgrep -a flportal; pkill -x flvideo; pkill -f 'flvideo --pick-file'; pkill -x flportal; echo gone", "gone")
                    gui("flcrash --cant /sysflfs/bin", 4)
                    shot("live-egg")
                    run("pkill -x flcrash; echo gone", "gone")
            con.send("poweroff")
            con.drain(40)
        except Exception as e:
            print(f"FAIL: {e}")
            fails += 1
            try:
                shot("live-failure")
            except Exception:
                pass
        finally:
            qemu.terminate()
            qemu.wait()
    print("PASS" if not fails else f"{fails} problem(s); console log: {LOG}")
    return 1 if fails else 0


sys.exit(main())
