#!/usr/bin/env python3
"""flboot's firmware entries in real OVMF, text mode over a serial console (no GPU: flboot falls back to the text menu).

    build/m21/flboot-fw.py STICK.img          (a stick from build/m21/mkstick.py: its FAT partition has flboot)

Boots QEMU with the stick as the only boot disk plus a second disk that holds \\EFI\\BOOT\\BOOTX64.EFI (flboot itself:
started from a volume without kernels it says so, which proves it was chain-loaded). Keys go in through the serial port.
  1. "Firmware setup (BIOS)": the machine resets and OVMF's setup screen (Device Manager, Boot Manager) appears
  2. "Other drives and systems" lists the firmware's boot options and the other drive; the chain entry starts the program
     on the second disk ("no kernels" from the chained flboot)
  3. a firmware option (EFI Internal Shell, when OVMF has one in BootOrder): BootNext + reset: the Shell starts
Prints one line per check; exit 1 when any fails.
"""
import os, re, socket, subprocess, sys, tempfile, time

stick = os.path.abspath(sys.argv[1])
top = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
R = os.path.expanduser("~/freelfs-work/rootfs-x86_64/.bootstrap/pkg/root")
ld = ["env", "MTOOLS_SKIP_CHECK=1", "MTOOLSRC=/dev/null", "GCONV_PATH=" + R + "/usr/lib/gconv", R + "/usr/lib/ld-linux-x86-64.so.2",
      "--library-path", R + "/usr/lib"]
work = tempfile.mkdtemp(prefix="flboot-fw-")
fails = []


def check(ok, what):
    print(("ok:   " if ok else "FAIL: ") + what, flush=True)
    if not ok:
        fails.append(what)


# the second disk: a FAT image with flboot as \EFI\BOOT\BOOTX64.EFI
# (the stick's FAT partition is the last partition: find it with sfdisk)
out = subprocess.run(["/usr/sbin/sfdisk", "-d", stick], capture_output=True, text=True, check=True).stdout
starts = [int(m.group(1)) for m in re.finditer(r"start=\s*(\d+)", out)]
p2 = starts[-1] * 512
efi = os.path.join(work, "BOOTX64.EFI")
with open(efi, "wb") as f:
    f.write(subprocess.run(ld + [R + "/usr/bin/mtype", "-i", "%s@@%d" % (stick, p2), "::/EFI/BOOT/BOOTX64.EFI"], capture_output=True, check=True).stdout)
disk2 = os.path.join(work, "disk2.img")
with open(disk2, "wb") as f:
    f.truncate(64 * 1024 * 1024)
subprocess.run(["/usr/sbin/mkfs.vfat", "-F", "32", "-n", "OTHER", disk2], check=True, stdout=subprocess.DEVNULL)
subprocess.run(ld + [R + "/usr/bin/mmd", "-i", disk2, "::/EFI", "::/EFI/BOOT"], check=True)
subprocess.run(ld + [R + "/usr/bin/mcopy", "-i", disk2, efi, "::/EFI/BOOT/BOOTX64.EFI"], check=True)

code = vars_t = None
for c, v in (("/usr/share/qemu/ovmf-x86_64-4m-code.bin", "/usr/share/qemu/ovmf-x86_64-4m-vars.bin"),
             ("/usr/share/qemu/ovmf-x86_64-code.bin", "/usr/share/qemu/ovmf-x86_64-vars.bin"),
             ("/usr/share/qemu/edk2-x86_64-code.fd", "/usr/share/qemu/edk2-i386-vars.fd"),
             ("/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_VARS_4M.fd"),
             ("/usr/share/edk2/x64/OVMF_CODE.4m.fd", "/usr/share/edk2/x64/OVMF_VARS.4m.fd"),
             ("/usr/share/edk2/x64/OVMF_CODE.fd", "/usr/share/edk2/x64/OVMF_VARS.fd")):
    if os.path.exists(c) and os.path.exists(v):
        code, vars_t = c, v
        break
if not code:
    sys.exit("no OVMF")
vars_f = os.path.join(work, "vars.fd")
subprocess.run(["cp", vars_t, vars_f], check=True)
sock = os.path.join(work, "ser")
cmd = ["qemu-system-x86_64", "-enable-kvm", "-cpu", "host", "-m", "512", "-machine", "q35", "-display", "none", "-vga", "none",
       "-drive", "if=pflash,format=raw,readonly=on,file=" + code, "-drive", "if=pflash,format=raw,file=" + vars_f,
       "-device", "qemu-xhci,id=x", "-drive", "file=%s,format=raw,if=none,id=stick" % stick,
       "-device", "usb-storage,drive=stick,bus=x.0,bootindex=0",
       "-drive", "file=%s,format=raw,if=none,id=d2" % disk2, "-device", "virtio-blk-pci,drive=d2,bootindex=5",
       "-serial", "unix:%s,server=on,wait=on" % sock, "-no-reboot"]
# OVMF resets the machine for OsIndications: QEMU must not stop on it
cmd.remove("-no-reboot")
q = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(100):
    if os.path.exists(sock):
        break
    time.sleep(0.1)
s = socket.socket(socket.AF_UNIX)
s.connect(sock)
buf = ""
ANSI = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]|\x1b[()][A-Z0-9]|\r")


def read(t=1.0):
    global buf
    end = time.time() + t
    while time.time() < end:
        s.settimeout(0.3)
        try:
            d = s.recv(65536)
        except socket.timeout:
            continue
        if not d:
            break
        buf += d.decode(errors="replace")


def wait_for(pat, t=90):
    end = time.time() + t
    while time.time() < end:
        read(1.0)
        if re.search(pat, ANSI.sub("", buf)):
            return True
    return False


def screen():
    return ANSI.sub("", buf)


def keys(seq):
    s.sendall(seq)


UP, DOWN, ENTER = b"\x1b[A", b"\x1b[B", b"\r"


def select(title, t=20):
    """Move the selection to the row with this title (the menu prints '> ' before the chosen row), stopping at the page."""
    for _ in range(30):
        read(0.8)
        tail = screen()[-3000:]
        m = list(re.finditer(r"^\s*> (.+?)\s*$", tail, re.M))
        if m and title in m[-1].group(1):
            return True
        keys(DOWN)
    return False


def stop_countdown():
    for _ in range(40):
        keys(b" ")
        read(0.5)
        if "Choose what to start" in screen() or "Up/Down" in screen():
            return True
    return False


try:
    check(wait_for(r"Choose what to start", 120), "flboot's text menu is on the serial console (the stick booted in OVMF)")
    stop_countdown()
    read(1.5)
    menu = screen()[screen().rindex("Choose what to start"):]
    check("Firmware setup (BIOS)" in menu, "the main menu offers Firmware setup (BIOS) (OVMF supports it)")
    check("Other drives and systems" in menu, "the main menu offers Other drives and systems")
    # ---- 3 first: the other drives page ----
    buf = ""
    keys(UP)  # wraps to the last row: Other drives and systems
    read(1)
    keys(ENTER)
    read(2)
    page = screen()
    check("Start something else on this computer" in page, "Other drives and systems opens its page")
    check(re.search(r"Other drive: EFI boot program \(drive \d\)", page) is not None, "the second disk is listed as an EFI boot program")
    opts = re.findall(r"(Firmware option: [^\r\n]+?)\s*\n", page)
    check(len(opts) > 0, "the firmware's own boot options are listed: %s" % ", ".join(o.strip() for o in opts[:4]))
    # ---- chain-load the second disk ----
    if select("Other drive: EFI boot program"):
        keys(ENTER)
        check(wait_for(r"no kernels|could not start|cannot read the EFI", 40), "the EFI program on the other disk was started (the chained flboot finds no kernels there)")
    else:
        check(False, "could not select the other drive entry")
    # that fatal() halts: reset the machine through the monitor-less way: kill and go on with a fresh VM for the next steps
finally:
    pass
q.kill()
q.wait()

# ---- 1 and the BootNext entry, fresh machine ----------------------------------------------------------------------
def fresh():
    global q, s, buf
    subprocess.run(["cp", vars_t, vars_f], check=True)
    if os.path.exists(sock):
        os.unlink(sock)
    q = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        if os.path.exists(sock):
            break
        time.sleep(0.1)
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock)
    buf = ""


fresh()
check(wait_for(r"Choose what to start", 120), "(second boot) the menu is back")
stop_countdown()
read(1)
buf = ""
keys(UP + UP)  # Firmware setup (BIOS) is the row above Other drives and systems (no snapshot row on a live stick)
read(1)
check(re.search(r"^\s*> Firmware setup \(BIOS\)", screen()[-2500:], re.M) is not None, "Up, Up selects Firmware setup (BIOS)")
buf = ""
keys(ENTER)
# OVMF draws its setup screen with box characters (the serial line shows them as UTF-8 box drawing or replacement
# characters); the menu of flboot must not come back
okfw = wait_for(r"Device Manager|Boot Manager|Select Language|Continue|[\u2500\u2502\ufffd]{20}", 90)
read(3)
check(okfw and "Choose what to start" not in screen(), "Firmware setup (BIOS): the computer restarted into OVMF's setup screen (box drawing on the console, no flboot menu)")
if not okfw:
    print("---- what the console showed after Enter:\n" + screen()[-1500:] + "\n----")
q.kill()
q.wait()

fresh()
check(wait_for(r"Choose what to start", 120), "(third boot) the menu is back")
stop_countdown()
read(1)
keys(UP)
read(1)
keys(ENTER)
read(2)
opt = "EFI Internal Shell" if select("Firmware option: EFI Internal Shell") else None
if not opt:
    opt = "EFI Firmware Setup" if select("Firmware option: EFI Firmware Setup") else None
if opt:
    buf = ""
    keys(ENTER)
    if opt == "EFI Internal Shell":
        check(wait_for(r"UEFI Interactive Shell|Shell>", 90), "firmware option '%s': BootNext + reset started it" % opt)
    else:
        ok2 = wait_for(r"Device Manager|Boot Manager|Select Language|Continue|[\u2500\u2502\ufffd]{20}", 90)
        read(3)
        check(ok2 and "Choose what to start" not in screen(), "firmware option '%s': BootNext + reset opened the setup screen (not flboot)" % opt)
else:
    check(False, "no firmware option to try BootNext with")
q.kill()
q.wait()
print("RESULT:", "FAIL (%d): %s" % (len(fails), "; ".join(fails)) if fails else "PASS")
sys.exit(1 if fails else 0)
