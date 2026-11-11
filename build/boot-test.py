#!/usr/bin/env python3
"""Boot the FreeLFS image headless and check it end to end.

Starts ./build/51-qemu.sh with the serial console on a unix socket, waits
for the login prompt, logs in as root (development images have no root
password), runs a few checks and powers off. The whole console transcript
goes to $FLFS_WORK/logs/boot-test.log. Exit status 0 means every step passed.

usage: build/boot-test.py
"""

import codecs
import os
import socket
import subprocess
import sys
import tempfile
import time

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.environ.get("FLFS_WORK", os.path.expanduser("~/freelfs-work"))
LOG = os.path.join(WORK, "logs", "boot-test.log")

# Commands run after login; each must print its marker for the test to pass.
CHECKS = [
    ("uname -r", "-freelfs"),
    ("cat /etc/os-release", 'NAME="FreeLFS"'),
    ("grep ' / ' /proc/mounts", " / btrfs rw"),
    # the M4 disk layout: ESP on /boot, btrfs subvolumes
    ("grep ' /boot ' /proc/mounts", " /boot vfat"),
    ("grep ' /usr ' /proc/mounts", "subvol=/@home"),
    # the FreeLFS layout: the system in /sysflfs, homes in /usr
    ("readlink /etc /var /bin /usr/bin | tr '\\n' ' '",
     "sysflfs/settings sysflfs/data sysflfs/bin ../sysflfs/bin"),
    ("ls /sysflfs | tr '\\n' ' '",
     "bin data include lib libexec local opt root sbin settings share src srv"),
    ("command -v ls; /usr/bin/env true && echo env-ok", "/sysflfs/bin/ls\r\nenv-ok"),
    ("btrfs subvolume get-default /", "path @"),
    ("ls /boot/EFI/BOOT", "BOOTX64.EFI"),
    ("/etc/rc.d/init.d/syslog status", "is running"),
    ("stat -c %G /usr/bin/wall", "tty"),
    # DHCP lease from QEMU's user network; dhcpcd probes the address first,
    # so the lease can take a few seconds after login.
    ("for i in $(seq 20); do ip -4 -br addr | grep -q 10.0.2.15 && break;"
     " sleep 1; done; ip -4 -brief addr", "10.0.2.15/24"),
    # M6: the desktop's system services
    ("/etc/rc.d/init.d/dbus status", "is running"),
    ("/etc/rc.d/init.d/seatd status", "is running"),
    ("/etc/rc.d/init.d/iwd status", "is running"),
    ("ls /dev/dri", "card"),
    # M7: the first boot asked for the keyboard
    ("cat /etc/keyboard.conf", "LAYOUT=de"),
    # M7: tty1 runs the login screen (flgreet starts flcomp, which starts flgreet-ui)
    ("grep -c '^1:.*flgreet' /etc/inittab", "1"),
    ("for i in $(seq 30); do pgrep -x flgreet-ui >/dev/null && break; sleep 1; done;"
     " pgrep -x flgreet-ui >/dev/null && echo greeter-up", "greeter-up"),
    # the login screen holds the seat, and the M6 drawing test below needs it:
    # take tty1 out of inittab (this VM only) and stop the greeter
    ("{ [ -f /etc/inittab.orig ] && mv /etc/inittab.orig /etc/inittab; }; cp /etc/inittab /etc/inittab.orig;"
     " sed -i '/^1:2345/d' /etc/inittab; telinit q; pkill -x flgreet; pkill -x flcomp; sleep 2;"
     " echo greeter-stopped", "greeter-stopped"),
    # M5: flsh is the shell of new users
    ("flsh -c 'echo $((6 * 7)) $FLSH_VERSION'", "42 0.1"),
    ("{ id tester >/dev/null 2>&1 || useradd -m tester; } && passwd -d tester >/dev/null"
     " && getent passwd tester",
     ":/usr/tester:/usr/bin/flsh"),
    ("ls /usr/tester | tr '\\n' ' '", "desktop documents downloads music pictures videos"),
    # M7: the Packages app's setuid helper. As tester (group seat) it must reach flpkg as root: `remove`
    # takes the database lock before it says the name is not installed. Anything else is refused.
    ("stat -c %U:%G:%a /usr/bin/flpkg-seat", "root:seat:4750"),
    ("su -s /bin/sh tester -c 'flpkg-seat remove nosuchpkg' 2>&1", "nosuchpkg is not installed"),
    ("su -s /bin/sh tester -c 'flpkg-seat install ../x; flpkg-seat sync' 2>&1", "not a package name\r\nusage: flpkg-seat"),
    # Wi-Fi (the Settings page talks to iwd over D-Bus). The laptop's RTL8852BE needs these files and
    # modules; the group netdev is named in iwd's D-Bus policy; dhcpcd must leave wl* to iwd's own DHCP.
    ("echo fw-$(ls /lib/firmware/rtw89/rtw8852b_fw*.bin | wc -l)-$(test -s /lib/firmware/regulatory.db && echo reg)",
     "fw-3-reg"),
    ("echo deps-$(modprobe -n -v rtw89_8852be | grep -c 'insmod.*rtw89_core')", "deps-1"),
    ("getent group netdev", "netdev:x:86"),
    ("echo deny-$(grep -c '^denyinterfaces wl' /etc/dhcpcd.conf) iwd-$(grep -c '^EnableNetworkConfiguration=true' /etc/iwd/main.conf)",
     "deny-1 iwd-1"),
    # tester (group seat) may call iwd; before the seat policy this was AccessDenied
    ("su -s /bin/sh tester -c 'dbus-send --system --print-reply --dest=net.connman.iwd /"
     " org.freedesktop.DBus.ObjectManager.GetManagedObjects' 2>&1 | head -n 2", "method return"),
    # mac80211_hwsim: two fake radios. iwd runs an access point on one, the other joins it with a
    # passphrase and gets an address from the AP's DHCP server through iwd's DHCP client. Both radios
    # would race for the name wlan0 when iwd recreates their interfaces, so iwd uses the default ones.
    # (the test runs on the real image: the sed must not add the line again on every run, and iwd needs more
    # than 5 s to take both radios on a loaded or small machine: wait until it lists both, restart it once if not)
    ("/etc/rc.d/init.d/iwd stop >/dev/null 2>&1; sed -i '/^UseDefaultInterface/d' /etc/iwd/main.conf;"
     " sed -i 's/^\\[General\\]/[General]\\nUseDefaultInterface=true/' /etc/iwd/main.conf;"
     " /etc/rc.d/init.d/iwd start >/dev/null 2>&1; sleep 1; modprobe mac80211_hwsim radios=2;"
     " for i in $(seq 12); do [ \"$(iwctl device list 2>/dev/null | grep -c wlan)\" = 2 ] && break; sleep 1; done;"
     # iwd sometimes leaves the first radio alone (a race with the interface it finds): a restart takes both
     " if [ \"$(iwctl device list 2>/dev/null | grep -c wlan)\" != 2 ]; then /etc/rc.d/init.d/iwd stop >/dev/null 2>&1;"
     " /etc/rc.d/init.d/iwd start >/dev/null 2>&1; for i in $(seq 12); do [ \"$(iwctl device list 2>/dev/null | grep -c wlan)\" = 2 ] && break; sleep 1; done; fi;"
     " iwctl device wlan1 set-property Mode ap && sleep 1 && iwctl ap wlan1 start fltest secretpass1;"
     " sleep 1; iwctl station wlan0 scan; sleep 4; iwctl --passphrase secretpass1 station wlan0 connect fltest;"
     " for i in $(seq 15); do ip -4 -o addr show wlan0 | grep -q 192.168 && break; sleep 1; done;"
     " echo hwsim-$(ip -4 -o addr show wlan0 | grep -c 192.168)", "hwsim-1"),
    ("echo dhcpcd-wl-$(pgrep -a dhcpcd | grep -c wlan)", "dhcpcd-wl-0"),
    # the resolvconf stand-in iwd calls: -a writes /etc/resolv.conf, -d takes its file away again
    ("cp /etc/resolv.conf /tmp/resolv.bak; printf 'nameserver 192.0.2.53\\n' | /usr/lib/iwd/bin/resolvconf -a t.dns &&"
     " echo rc-$(grep -c 192.0.2.53 /etc/resolv.conf); /usr/lib/iwd/bin/resolvconf -d t.dns;"
     " echo gone-$(test -e /etc/resolv.conf && echo no || echo yes); cp /tmp/resolv.bak /etc/resolv.conf", "rc-1\r\ngone-yes"),
]
# A USB image (build/52-usb.sh, FLFS_BOOT_USB=1) has tester already, and its home belongs to them
if os.environ.get("FLFS_BOOT_USB"):
    CHECKS.insert(1, ("stat -c %U:%G:%a /usr/tester; id -nG tester", "tester:users:700"))
    CHECKS.insert(2, ("id -nG tester", "seat"))

# Typed into flsh after logging in as tester: (keys, expected output). The
# expected text must not appear in the keys, which flsh echoes as they are
# typed.
FLSH_CHECKS = [
    ("grpe x", "Did you mean grep?"),
    ("ech\tresult=$((40 + 2))", "result=42"),        # Tab completes "echo "
    ("for i in 1 2; do echo n$i; done", "n1\r\nn2"),
    ("cd /usr/sh\t && echo at:$PWD", "at:/usr/share"),  # a directory gets its /
    # M6: a normal user draws with EGL on KMS, the screen handed out by seatd
    ("fl-drawtest -t 3", "frames in"),
]
EDITOR_READY = "\x1b[?2004h"  # flsh's line editor turns on bracketed paste


class Console:
    def __init__(self, sock, log):
        self.sock = sock
        self.log = log
        self.buf = ""
        # the serial line delivers a few bytes at a time: keep UTF-8
        # characters that arrive split across reads whole
        self.dec = codecs.getincrementaldecoder("utf-8")("replace")

    def expect(self, text, timeout):
        end = time.monotonic() + timeout
        while text not in self.buf:
            left = end - time.monotonic()
            if left <= 0:
                raise TimeoutError(f"no {text!r} within {timeout}s")
            self.sock.settimeout(left)
            try:
                data = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not data:
                raise EOFError(f"console closed while waiting for {text!r}")
            chunk = self.dec.decode(data)
            self.log.write(chunk)
            self.log.flush()
            self.buf += chunk
        before, _, self.buf = self.buf.partition(text)
        return before

    def expect_any(self, texts, timeout):
        """Wait for the first of texts; returns it, leaving the rest unread."""
        end = time.monotonic() + timeout
        while True:
            hits = [(self.buf.find(t), t) for t in texts if t in self.buf]
            if hits:
                return min(hits)[1]
            left = end - time.monotonic()
            if left <= 0:
                raise TimeoutError(f"none of {texts!r} within {timeout}s")
            self.sock.settimeout(left)
            try:
                data = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not data:
                raise EOFError("console closed")
            chunk = self.dec.decode(data)
            self.log.write(chunk)
            self.log.flush()
            self.buf += chunk

    def drain(self, timeout):
        """Read until the VM closes the console; return everything read."""
        end = time.monotonic() + timeout
        start = len(self.buf)
        while time.monotonic() < end:
            self.sock.settimeout(max(end - time.monotonic(), 0.1))
            try:
                data = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not data:
                break
            chunk = self.dec.decode(data)
            self.log.write(chunk)
            self.log.flush()
            self.buf += chunk
        return self.buf[start:]

    def send(self, line):
        self.sock.sendall(line.encode() + b"\r")


def step(name, fn):
    print(f"  {name} ... ", end="", flush=True)
    try:
        result = fn()
    except Exception as e:
        print(f"FAIL ({e})")
        raise
    print("ok")
    return result


def main():
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="flfs-boot-")
    path = os.path.join(tmp, "console")
    env = dict(os.environ, FLFS_VM_SERIAL=f"unix:{path},server=on,wait=on")

    with open(LOG, "w") as log:
        mon = os.path.join(tmp, "mon")
        qemu = subprocess.Popen([os.path.join(TOP, "build", "51-qemu.sh"), "--serial", "--",
                                 "-monitor", f"unix:{mon},server=on,wait=off"],
                                env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            for _ in range(100):
                if os.path.exists(path):
                    break
                time.sleep(0.1)
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(path)
            con = Console(sock, log)
            t0 = time.monotonic()

            if con.expect_any(["First boot", "login:"], 180) == "First boot":
                # flsetup is on the screen: Enter takes the default, QWERTZ
                time.sleep(8)
                m = socket.socket(socket.AF_UNIX)
                m.connect(mon)
                m.sendall(b"sendkey ret\n")
                time.sleep(0.5)
                m.close()
            boot = step("boot to login prompt", lambda: con.expect("login:", 180))
            print(f"    ({time.monotonic() - t0:.1f}s)")
            boot_failures = 0
            for line in boot.splitlines():
                if "[FAIL]" in line:
                    print(f"    boot step failed: {line.strip()}")
                    boot_failures += 1
            con.send("root")
            step("log in as root", lambda: con.expect(":~#", 30))
            # The prompt marker must not appear in the echoed command that
            # sets it: \$ only becomes # once bash prints the prompt.
            con.send("PS1='\\n@@\\$ '")
            con.expect("@@# ", 10)

            failed = boot_failures  # a failed boot step fails the test
            for cmd, want in CHECKS:
                con.send(cmd)
                out = con.expect("@@# ", 90)
                ok = want in out
                label = cmd if len(cmd) <= 34 else cmd[:31] + "..."
                print(f"  {label:<34} {'ok' if ok else 'FAIL'}")
                failed += not ok

            # log in as the new user: their shell is flsh
            con.send("exit")
            con.expect("login:", 30)
            con.send("tester")
            step("log in as tester (flsh)", lambda: con.expect("\u203a", 30))
            for keys, want in FLSH_CHECKS:
                con.sock.sendall(keys.encode() + b"\r")
                try:
                    con.expect(want, 20)
                    con.expect(EDITOR_READY, 10)  # back at the prompt
                    ok = True
                except TimeoutError:
                    ok = False
                label = "flsh: " + keys.replace("\t", "<Tab>")
                label = label if len(label) <= 34 else label[:31] + "..."
                print(f"  {label:<34} {'ok' if ok else 'FAIL'}")
                failed += not ok
            con.send("exit")
            con.expect("login:", 30)
            con.send("root")
            con.expect(":~#", 30)
            con.send("mv /etc/inittab.orig /etc/inittab && telinit q")  # tty1 gets its login screen back
            con.expect(":~#", 10)
            con.send("poweroff")

            def shutdown():
                out = con.drain(90)
                qemu.wait(timeout=10)
                if "Powering off." not in out:
                    raise RuntimeError("rc.shutdown did not finish")
                for line in out.splitlines():
                    if "Remounting root read-only" in line and "FAIL" in line:
                        raise RuntimeError("root was not remounted read-only")

            step("clean power off", shutdown)
        finally:
            if qemu.poll() is None:
                qemu.kill()
            try:
                os.unlink(path)
                os.rmdir(tmp)
            except OSError:
                pass

    print(f"transcript: {LOG}")
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        print(f"boot test failed; transcript: {LOG}")
        sys.exit(1)
