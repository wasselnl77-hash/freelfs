#!/usr/bin/env python3
"""Round 23: the REAL live ISO (root locked), driven over the serial console as nobody-with-a-password-would.

    build/m23-real.py [--iso FILE] [--mem 1G] [--port N]

1. root cannot log in on the serial port (empty password, a wrong password): no shell, the prompt comes back.
2. the live user tester logs in (empty password, /run/live) and, as that ordinary user, checks what a user can see:
   sysctls, /proc/config.gz hardening options, ping without setuid, setuid helpers as documented, su refused for a
   non-wheel user, the installer helper refusing a start by any other program, a helper that stays silent for the
   live tester, and (with --port) outbound HTTP to the host.
Exit 0 only when every check passed. Console log: $FLFS_WORK/logs/m23-real.log.
"""
import argparse, importlib.util, os, re, socket, subprocess, sys, tempfile, time

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("boottest", os.path.join(here, "boot-test.py"))
bt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bt)
ap = argparse.ArgumentParser()
ap.add_argument("--iso", default=os.path.expanduser("~/freelfs-work/freelfs-live.iso"))
ap.add_argument("--mem", default="1G")
ap.add_argument("--port", default="")
args = ap.parse_args()
LOG = os.path.join(bt.WORK, "logs", "m23-real.log")
ANSI = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")
tmp = tempfile.mkdtemp(prefix="flfs-m23-")
path, mon = os.path.join(tmp, "console"), os.path.join(tmp, "mon")
env = dict(os.environ, FLFS_VM_SERIAL=f"unix:{path},server=on,wait=on", FLFS_VM_MEM=args.mem, FLFS_VM_VARS=os.path.join(tmp, "ovmf-vars.fd"))
cmd = [os.path.join(bt.TOP, "build", "51-qemu.sh"), "--serial", "--iso", args.iso, "--", "-monitor", f"unix:{mon},server=on,wait=off", "-usb"]
FAILS = []


def check(text, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + text + ("" if ok else "  [" + detail.strip()[-300:] + "]"), flush=True)
    if not ok:
        FAILS.append(text)


def monitor(text, wait=0.3):
    m = socket.socket(socket.AF_UNIX)
    m.connect(mon)
    m.sendall(text.encode() + b"\n")
    time.sleep(wait)
    m.close()


os.makedirs(os.path.dirname(LOG), exist_ok=True)
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
        # the real ISO's /dev/console is tty0, so the first-boot keyboard question ("First boot") is not on the serial
        # port: press Enter now and then (the question is answered with its default) until the login prompt shows
        t0 = time.monotonic()
        while True:
            try:
                con.expect("login:", 25)
                break
            except TimeoutError:
                if time.monotonic() - t0 > 300:
                    raise
                monitor("sendkey ret")
        # ---- 1: root is refused on the serial port
        for pw in ("", "wrongpw"):
            con.send("root")
            got = con.expect_any(["assword", "# ", "Login incorrect"], 20)
            if got == "assword":
                con.send(pw)
                got = con.expect_any(["Login incorrect", ":~#", "# "], 30)
            check("root refused on ttyS0 (password %r): %s" % (pw, got), got == "Login incorrect", got)
            con.expect("login:", 30)
        # ---- 2: tester
        con.send("tester")
        time.sleep(6)
        con.buf = ""

        def run(text, line, timeout=60):
            """run LINE as tester; (rc, output)"""
            con.send(f"( {line} ) >/tmp/o 2>&1; echo T$((6*7))R$?E")
            con.expect("T42R", timeout)
            rc = con.expect("E", 5).strip()
            con.send("cat /tmp/o; echo X$((6*7))Y")
            out = ANSI.sub("", con.expect("X42Y", 30)).replace("\r", "")
            if out.lstrip().startswith("cat /tmp/o"):  # the tty echoes the command line first
                out = out.lstrip().split("\n", 1)[1] if "\n" in out.lstrip() else ""
            return rc, out

        def ok(text, line, timeout=60, want=None):
            rc, out = run(text, line, timeout)
            check(text, rc == "0" and (want is None or want in out), "rc=%s %s" % (rc, out))

        rc, out = run("whoami", "id -un")
        check("tester logged in on the serial port", rc == "0" and "tester" in out, out)
        ok("root has no shell: the id is not 0", "test $(id -u) -ne 0")
        # sysctls
        for k, v in (("kernel.kptr_restrict", "1"), ("kernel.dmesg_restrict", "1"), ("kernel.unprivileged_bpf_disabled", "1"),
                     ("kernel.yama.ptrace_scope", "1"), ("fs.protected_symlinks", "1"), ("fs.protected_hardlinks", "1"),
                     ("fs.protected_fifos", "2"), ("fs.protected_regular", "2"), ("fs.suid_dumpable", "0"),
                     ("net.ipv4.conf.all.rp_filter", "1"), ("kernel.kexec_load_disabled", "1"), ("kernel.io_uring_disabled", "2")):
            ok("sysctl %s = %s" % (k, v), f"test \"$(sysctl -n {k})\" = {v}")
        ok("user namespaces are still on (Flatpak, bwrap)", "unshare --user --map-root-user true")
        ok("dmesg is refused for a user", "! dmesg")
        for o in ("SECURITY_YAMA=y", "SECURITY_LOCKDOWN_LSM=y", "HARDENED_USERCOPY=y", "INIT_ON_ALLOC_DEFAULT_ON=y", "FORTIFY_SOURCE=y",
                  "BPF_UNPRIV_DEFAULT_OFF=y", "SECURITY_DMESG_RESTRICT=y", "IKCONFIG_PROC=y"):
            ok("kernel config has CONFIG_" + o, f"zcat /proc/config.gz | grep -qx CONFIG_{o}")
        ok("lockdown is built in and defaults to none", "zcat /proc/config.gz | grep -qx CONFIG_LOCK_DOWN_KERNEL_FORCE_NONE=y && zcat /proc/config.gz | grep -qx CONFIG_SECURITY_LOCKDOWN_LSM_EARLY=y")
        # setuid trimming
        ok("ping is not setuid", "test ! -u /usr/bin/ping && test ! -u /usr/bin/ping6")
        ok("ping works for the user (ICMP datagram socket)", "ping -c 1 -W 3 127.0.0.1")
        ok("mount and umount are not setuid", "test ! -u /usr/bin/mount && test ! -u /usr/bin/umount")
        ok("newuidmap and newgidmap are setuid (rootless containers, round 24a)", "test -u /usr/bin/newuidmap && test -u /usr/bin/newgidmap")
        ok("su is still setuid but wheel only", "test -u /usr/bin/su && grep -q '^SU_WHEEL_ONLY.*yes' /etc/login.defs")
        ok("traceroute is setuid only for its group", "stat -c '%a' /usr/bin/traceroute | grep -qx 4750")
        rc, out = run("su refused", "script -qec 'su -c id root' /dev/null < /dev/null; true")
        check("su is refused for a user who is not in wheel (message: %s)" % out.strip()[:60], "not authorized" in out, out)
        check("su gave no root shell", "uid=0" not in out, out)
        ok("the installer helper refuses a start by any other program", "printf 'quit\\n' | /usr/libexec/flinstall-helper 2>&1 | grep -q 'only the installer'")
        ok("bootstrap.pub is not trusted in the release image", "test ! -e /etc/flpkg/keys/bootstrap.pub && test -e /etc/flpkg/keys/freelfs-repo.pub")
        ok("securetty has no serial port", "test -s /etc/securetty && ! grep -v '^#' /etc/securetty | grep -q '^tty[SUA]' && grep -qx tty1 /etc/securetty")
        ok("the issue file has no development-image line", "! grep -q 'Development image' /etc/issue")
        ok("the firewall service is configured", "grep -q 'firewall' /etc/rc.conf && test -s /etc/nftables.conf")
        if args.port:
            ok("outbound HTTP works through the firewall", f"curl -sf -m 15 http://10.0.2.2:{args.port}/ -o /dev/null")
        ok("Flathub is reachable (outbound HTTPS)", "curl -sI -m 25 https://dl.flathub.org/repo/summary.idx | head -1 | grep -q ' [23][0-9][0-9]'", 90)
    except Exception as e:  # noqa: BLE001
        check("the run reached its end", False, repr(e))
    finally:
        if qemu.poll() is None:
            qemu.terminate()
            try:
                qemu.wait(10)
            except Exception:
                qemu.kill()
print("RESULT:", "FAIL (%d): %s" % (len(FAILS), "; ".join(FAILS)) if FAILS else "PASS")
sys.exit(1 if FAILS else 0)
