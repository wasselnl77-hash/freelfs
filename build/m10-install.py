#!/usr/bin/env python3
"""M10: the installer, for real, onto a blank VIRTUAL disk, then boot what it made.

    build/m10-install.py [--iso FILE] [--mem 2G] [--shots DIR]

1. Boots the live ISO in QEMU with a second, blank 16 GiB disk (a sparse FILE in $FLFS_WORK/scratch:
   no host block device is ever given to QEMU), answers the first-boot keyboard question with US,
   logs in as tester on the screen and starts "FreeLFS install" from the start menu.
2. Clicks through the window like a person (language, keyboard, Wi-Fi, disk: "Use the whole disk",
   account anna / Anna Berger, time zone Berlin, summary, Install) and waits for "FreeLFS is installed".
3. Powers off, boots the virtual disk alone: the login screen must list the new account, the password
   logs in, the serial console checks the layout (btrfs @ @home @snapshots, ESP with flboot and kernel,
   time zone, keyboard, no live user, snapshots on) and the account.
The window is the first one of the session, so it opens at x=40: all click positions are relative to that
(the pictures show it; look at them). The test password is only for this throwaway disk.
Exit status 0: everything passed. Log: $FLFS_WORK/logs/m10-install.log; pictures docs/m7/m10-install-*.png.
"""
import argparse, importlib.util, os, re, socket, subprocess, sys, tempfile, time

here = os.path.dirname(os.path.abspath(__file__))


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, os.path.join(here, file))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


bt = load("boottest", "boot-test.py")
vr = load("vmrun", "vm-run.py")
WORK = bt.WORK
ap = argparse.ArgumentParser()
ap.add_argument("--iso", default=os.path.join(WORK, "freelfs-live.iso"))
ap.add_argument("--mem", default="2G")
ap.add_argument("--shots", default=os.path.join(bt.TOP, "docs", "m7"))
args = ap.parse_args()
PW = "flfsinstall1"
scratch = os.path.join(WORK, "scratch", "m10-inst")
LOG = os.path.join(WORK, "logs", "m10-install.log")
fails = 0


def check(name, ok, detail=""):
    global fails
    print(f"  {name:62s} {'ok' if ok else 'FAIL'}", flush=True)
    if not ok:
        fails += 1
        if detail:
            print("    got: " + detail.replace("\n", " | ")[-300:], flush=True)


class VM:
    def __init__(self, iso, disk, tag):
        self.tag = tag
        self.tmp = tempfile.mkdtemp(prefix="flfs-m10i-")
        self.console_path = os.path.join(self.tmp, "c")
        self.mon = os.path.join(self.tmp, "m")
        env = dict(os.environ, FLFS_VM_SERIAL=f"unix:{self.console_path},server=on,wait=on", FLFS_VM_MEM=args.mem,
                   FLFS_VM_VARS=os.path.join(self.tmp, "vars.fd"))
        cmd = [os.path.join(bt.TOP, "build", "51-qemu.sh"), "--serial"]
        if iso:
            cmd += ["--iso", iso]
        else:
            env["FLFS_IMG"] = disk
        cmd += ["--", "-monitor", f"unix:{self.mon},server=on,wait=off", "-qmp", f"unix:{self.mon}q,server=on,wait=off",
                "-usb", "-device", "usb-tablet"]
        if iso:
            cmd += ["-drive", f"file={disk},format=raw,if=virtio"]
        self.log = open(LOG, "a")
        self.q = subprocess.Popen(cmd, env=env, stdout=self.log, stderr=subprocess.STDOUT)
        while not os.path.exists(self.console_path):
            time.sleep(0.1)
        s = socket.socket(socket.AF_UNIX)
        s.connect(self.console_path)
        self.c = bt.Console(s, self.log)

    def shot(self, name):
        ppm = os.path.join(self.tmp, name + ".ppm")
        vr.monitor(self.mon, f"screendump {ppm}")
        time.sleep(1.3)
        os.makedirs(args.shots, exist_ok=True)
        subprocess.run(["magick", ppm, os.path.join(args.shots, f"m10-install-{self.tag}-{name}.png")], check=True)
        os.unlink(ppm)

    def key(self, *k):
        for x in k:
            vr.monitor(self.mon, f"sendkey {x}")
            time.sleep(0.3)

    def type(self, text):
        for ch in text:
            vr.monitor(self.mon, f"sendkey {vr.key_name(ch)}")

    def click(self, x, y):
        vr.qmp_click(self.mon + "q", x, y)
        time.sleep(0.5)

    def run(self, cmd, timeout=120):
        self.c.buf = ""
        self.c.send(cmd + "; echo @@EN''D")
        out = self.c.expect("@@END\r\n", timeout)
        out = re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]", "", out).replace("\r", "")
        return out.split("\n", 1)[1].rsplit("@@END", 1)[0] if "\n" in out else out

    def stop(self):
        try:
            self.run("poweroff", 10)
        except Exception:
            pass
        try:
            self.q.wait(60)
        except Exception:
            self.q.kill()
        for f in (self.console_path, self.mon, self.mon + "q"):
            try:
                os.unlink(f)
            except OSError:
                pass
        for f in os.listdir(self.tmp):
            os.unlink(os.path.join(self.tmp, f))
        os.rmdir(self.tmp)


def main():
    os.makedirs(scratch, exist_ok=True)
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    open(LOG, "w").close()
    disk = os.path.join(scratch, "target.img")
    if os.path.exists(disk):
        os.unlink(disk)
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 ** 3)  # a sparse file: the installer's only disk, nothing of the host
    # ---- 1: the live system
    vm = VM(args.iso, disk, "live")
    try:
        seen = vm.c.expect_any(["First boot", "login:"], 240)
        if seen == "First boot":
            time.sleep(8)
            vm.click(553, 290)   # QWERTY US
            vm.click(925, 612)   # Continue
            vm.c.expect("login:", 240)
        vm.c.send("root")
        vm.c.expect(":~#", 30)
        vm.c.send("PS1='\\n@@\\$ '")
        vm.c.expect("@@# ", 10)
        out = vm.run("for i in $(seq 40); do pgrep -x flgreet-ui >/dev/null && break; sleep 1; done; pgrep -x flgreet-ui; ls /dev/vd*")
        check("live system up, the blank disk is /dev/vda", "/dev/vda" in out and "/dev/vda1" not in out, out)
        time.sleep(3)
        vm.shot("1-login")
        vm.key("ret")
        time.sleep(18)
        vm.key("meta_l")
        time.sleep(2)
        vm.type("install")
        time.sleep(1.5)
        vm.shot("2-menu")
        vm.key("ret")
        time.sleep(6)
        vm.shot("3-language")
        step = [("4-keyboard", None), ("5-wifi", None), ("6-disk", None)]
        for name, _ in step:
            vm.click(1013, 682)  # Continue
            time.sleep(2)
            vm.shot(name)
        vm.click(1013, 682)
        time.sleep(1.5)
        vm.click(540, 170)
        vm.type("Anna Berger")
        vm.click(540, 341)
        vm.type(PW)
        vm.click(540, 417)
        vm.type(PW)
        time.sleep(1)
        vm.shot("7-account")
        vm.click(1013, 682)
        time.sleep(2)
        vm.click(640, 148)
        vm.type("berlin")
        time.sleep(1)
        vm.key("down", "ret")
        time.sleep(1)
        vm.shot("8-timezone")
        vm.click(1013, 682)
        time.sleep(3)
        vm.shot("9-summary")
        vm.click(983, 682)  # Install (a blank disk needs no "I understand" box)
        time.sleep(30)
        vm.shot("10-installing")
        # wait for the helper to finish: it is the window's child
        out = vm.run("for i in $(seq 600); do pgrep -f flinstall-helper >/dev/null || break; sleep 2; done; pgrep -fc flinstall-helper; sfdisk -l /dev/vda | grep -E '^/dev/vda'", 1500)
        time.sleep(3)
        vm.shot("11-done")
        check("the installer finished (helper gone) and made two partitions", "vda1" in out and "vda2" in out, out)
        check("ESP + Linux root types", "EFI System" in out and "Linux root" in out, out)
    finally:
        vm.stop()
    # ---- 2: the installed disk, alone
    vm = VM(None, disk, "installed")
    try:
        vm.c.expect("login:", 300)
        vm.c.send("root")
        vm.c.expect("assword", 10)
        vm.c.send(PW)
        vm.c.expect(":~#", 30)
        vm.c.send("PS1='\\n@@\\$ '")
        vm.c.expect("@@# ", 10)
        out = vm.run("for i in $(seq 40); do pgrep -x flgreet-ui >/dev/null && break; sleep 1; done; pgrep -x flgreet-ui")
        check("login on the serial console as root with the account's password", True)
        time.sleep(3)
        vm.shot("1-login")
        vm.type(PW)
        vm.key("ret")
        time.sleep(22)
        vm.shot("2-desktop")
        checks = [
            ("id anna", "uid=1000(anna) gid=999(users)"),
            ("grep -c '^tester:' /etc/passwd; true", "0"),
            ("grep ' / ' /proc/mounts", " / btrfs rw"),
            ("btrfs subvolume get-default /", "path @"),
            ("grep ' /home ' /proc/mounts", "subvol=/@home"),
            ("grep ' /.snapshots ' /proc/mounts", "subvol=/@snapshots"),
            ("grep ' /boot ' /proc/mounts", " /boot vfat"),
            ("ls /boot /boot/EFI/BOOT /boot/flboot | tr '\\n' ' '", "BOOTX64.EFI"),
            ("ls /boot | grep -c vmlinuz", "1"),
            ("cat /boot/flboot/flboot.conf | grep -c PARTUUID", "1"),
            ("readlink /etc/localtime", "Europe/Berlin"),
            ("cat /etc/keyboard.conf", "LAYOUT=us"),
            ("flsnap status | grep supported", "supported=1"),
            ("grep -c '^snapshots = 5' /etc/flpkg.conf", "1"),
            ("grep ^root: /etc/shadow | cut -d: -f2 | cut -c1-3", "$6$"),
            ("stat -c %U:%G:%a /home/anna", "anna:users:700"),
            ("echo $(getent passwd anna | cut -d: -f6) $(readlink /sysflfs)", "/home/anna usr"),
            ("ls -d /usr/anna 2>&1 | grep -c 'No such'", "1"),
            ("useradd -m bob && echo $(getent passwd bob | cut -d: -f6) $(stat -c %U /home/bob) && userdel -r bob 2>/dev/null; echo gone-$(ls -d /home/bob 2>/dev/null | wc -l)",
             "/home/bob bob"),
            ("echo ucode-$(grep -c 'initrd=.ucode.img' /boot/flboot/flboot.conf)-$(test -s /boot/ucode.img && echo file)", "ucode-1-file"),
            ("stat -c %U:%G:%a /usr/libexec/flinstall-helper", "root:seat:4750"),
            ("ls /usr/share/applications | grep -c flinstall", "1"),
            ("ls /home/anna | tr '\\n' ' '", "desktop"),
            ("pgrep -c flpanel", "1"),
            ("btrfs subvolume list / | grep -c '@'", "3"),
            ("flsnap create --reason 'after install' && flsnap list | head -2", "after install"),
            ("free -m | sed -n 2p", "Mem:"),
        ]
        for cmd, marker in checks:
            out = vm.run(cmd)
            check(cmd[:60], marker in out, out)
        vm.shot("3-end")
    except Exception as e:
        check(f"installed system: {e}", False)
        try:
            vm.shot("failure")
        except Exception:
            pass
    finally:
        vm.stop()
    os.unlink(disk)
    print("PASS" if not fails else f"{fails} problem(s); log {LOG}")
    return 1 if fails else 0


sys.exit(main())
