#!/usr/bin/env python3
"""A Ventoy-style USB stick image from a live ISO, without root and without a mount:

    build/m21/mkstick.py ISO OUT.img [--free MB]

  partition 1  exFAT "Ventoy" holding the ISO as a file at the top (written by this script: mkfs.exfat makes the
               volume, the script adds the directory entries and the cluster run; fsck.exfat checks it)
  partition 2  FAT "VTOYEFI": the ISO's own EFI boot image (flboot, kernel, initrd, flboot.conf), so a UEFI
               firmware boots the stick; flinitrd then finds the ISO file on partition 1, as it does on a real
               Ventoy stick (where Ventoy's loader replaces partition 2's job).
MBR partition table, like Ventoy's default. --free: MB of free space left on partition 1 (default 200).
"""
import argparse, os, shutil, struct, subprocess, sys, tempfile

ap = argparse.ArgumentParser()
ap.add_argument("iso")
ap.add_argument("out")
ap.add_argument("--free", type=int, default=200)
ap.add_argument("--extra", default="", help="kernel options added to every boot entry of the stick (e.g. flive.toram=0)")
ap.add_argument("--name", default=None, help="file name on the stick (default freelfs-live-NAME.iso style: must match freelfs-live*.iso)")
args = ap.parse_args()
SEC = 512
iso_size = os.path.getsize(args.iso)


def iso_esp(path):
    """(start byte, size) of the ISO's GPT partition of type EFI System (the appended El Torito image)."""
    with open(path, "rb") as f:
        f.seek(SEC)
        h = f.read(92)
        if h[:8] != b"EFI PART":
            sys.exit("the ISO has no GPT: no appended EFI partition")
        ent_lba, n, size = struct.unpack_from("<QII", h, 72)
        f.seek(ent_lba * SEC)
        for i in range(n):
            e = f.read(size)
            if e[:16] == bytes(16):
                continue
            first, last = struct.unpack_from("<QQ", e, 32)
            if e[:16] == bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b"):
                return first * SEC, (last - first + 1) * SEC
    sys.exit("no EFI System partition in the ISO's GPT")


esp_off, esp_size = iso_esp(args.iso)
MB = 1 << 20
p1_size = ((iso_size + args.free * MB + MB - 1) // MB) * MB
p1_start = 2048 * SEC
p2_start = p1_start + p1_size
total = p2_start + ((esp_size + MB - 1) // MB) * MB + MB
work = tempfile.mkdtemp(prefix="mkstick-")
try:
    p1 = os.path.join(work, "p1")
    with open(p1, "wb") as f:
        f.truncate(p1_size)
    subprocess.run(["/usr/sbin/mkfs.exfat", "-L", "Ventoy", p1], check=True, stdout=subprocess.DEVNULL)

    # ---- put the ISO into the exFAT volume -------------------------------------------------------------
    with open(p1, "r+b") as v:
        b = bytearray(v.read(SEC * 12))
        bps = 1 << b[108]
        spc = 1 << b[109]
        heap = struct.unpack_from("<I", b, 88)[0]
        nclu = struct.unpack_from("<I", b, 92)[0]
        root = struct.unpack_from("<I", b, 96)[0]
        csize = bps * spc

        def clu_off(c):
            return (heap + (c - 2) * spc) * bps

        v.seek(clu_off(root))
        rd = bytearray(v.read(csize))
        bm_first = bm_len = None
        end = None
        for i in range(0, csize, 32):
            t = rd[i]
            if t == 0x81:
                bm_first, bm_len = struct.unpack_from("<IQ", rd, i + 20)[0], struct.unpack_from("<Q", rd, i + 24)[0]
            if t == 0 and end is None:
                end = i
        if bm_first is None or end is None:
            sys.exit("unexpected exFAT root directory")
        v.seek(clu_off(bm_first))
        bm = bytearray(v.read(bm_len))
        need = (iso_size + csize - 1) // csize
        run = start = 0
        for c in range(nclu):
            if bm[c >> 3] >> (c & 7) & 1:
                run = 0
                start = c + 1
            else:
                run += 1
                if run == need:
                    break
        else:
            sys.exit("no room for the ISO")
        first = start + 2
        for c in range(start, start + need):
            bm[c >> 3] |= 1 << (c & 7)
        v.seek(clu_off(bm_first))
        v.write(bm)
        v.seek(clu_off(first))
        with open(args.iso, "rb") as src:
            shutil.copyfileobj(src, v, 4 * MB)

        name = args.name or os.path.basename(args.iso)
        up = name.upper()
        utf = name.encode("utf-16-le")
        nn = (len(name) + 14) // 15
        sec = 1 + nn
        h = 0
        for ch in up.encode("utf-16-le"):
            h = (((h << 15) | (h >> 1)) + ch) & 0xFFFF
        ts = ((2026 - 1980) << 25) | (11 << 21) | (17 << 16) | (12 << 11)
        ent = bytearray(32 * (1 + sec))
        ent[0] = 0x85
        ent[1] = sec
        struct.pack_into("<H", ent, 4, 0x20)
        struct.pack_into("<III", ent, 8, ts, ts, ts)
        s = 32
        ent[s] = 0xC0
        ent[s + 1] = 3  # allocation possible, no FAT chain: the run is contiguous
        ent[s + 3] = len(name)
        struct.pack_into("<H", ent, s + 4, h)
        struct.pack_into("<QIIQ", ent, s + 8, iso_size, 0, first, iso_size)
        for k in range(nn):
            o = 32 * (2 + k)
            ent[o] = 0xC1
            part = utf[k * 30:(k + 1) * 30]
            ent[o + 2:o + 2 + len(part)] = part
        cs = 0
        for i, byte in enumerate(ent):
            if i in (2, 3):
                continue
            cs = (((cs << 15) | (cs >> 1)) + byte) & 0xFFFF
        struct.pack_into("<H", ent, 2, cs)
        if end + len(ent) > csize:
            sys.exit("root directory full")
        rd[end:end + len(ent)] = ent
        v.seek(clu_off(root))
        v.write(rd)
    r = subprocess.run(["/usr/sbin/fsck.exfat", "-n", p1], capture_output=True, text=True)
    print(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr.strip())
    if r.returncode:
        sys.exit("fsck.exfat found problems:\n" + r.stdout + r.stderr)

    # ---- the stick ------------------------------------------------------------------------------------------
    with open(args.out, "wb") as o:
        o.truncate(total)
    spec = "label: dos\nstart=2048, size=%d, type=7\nstart=%d, size=%d, type=ef\n" % (
        p1_size // SEC, p2_start // SEC, (total - p2_start - MB) // SEC)
    subprocess.run(["/usr/sbin/sfdisk", "--quiet", args.out], input=spec, text=True, check=True)
    with open(args.out, "r+b") as o, open(p1, "rb") as src:
        o.seek(p1_start)
        shutil.copyfileobj(src, o, 4 * MB)
        o.seek(p2_start)
        with open(args.iso, "rb") as iso:
            iso.seek(esp_off)
            o.write(iso.read(esp_size))
    if args.extra:   # rewrite flboot.conf inside the stick's FAT partition with the rootfs' mtools
        R = os.path.expanduser("~/freelfs-work/rootfs-x86_64/.bootstrap/pkg/root")   # the image root: it has mtools
        ld = ["env", "MTOOLS_SKIP_CHECK=1", "MTOOLSRC=/dev/null", "GCONV_PATH=" + R + "/usr/lib/gconv", R + "/usr/lib/ld-linux-x86-64.so.2", "--library-path", R + "/usr/lib"]
        img = "%s@@%d" % (args.out, p2_start)
        conf = subprocess.run(ld + [R + "/usr/bin/mtype", "-i", img, "::/flboot/flboot.conf"], capture_output=True, text=True, check=True).stdout
        out = []
        for line in conf.splitlines():
            if line.startswith("cmdline "):
                line += " " + args.extra
            elif line.startswith("entry "):
                line += " " + args.extra
            out.append(line)
        tmp = os.path.join(work, "flboot.conf")
        open(tmp, "w").write("\n".join(out) + "\n")
        subprocess.run(ld + [R + "/usr/bin/mcopy", "-o", "-i", img, tmp, "::/flboot/flboot.conf"], check=True)
    print("stick: %s (%d MiB): Ventoy exFAT with %s, VTOYEFI %d KiB" % (args.out, total // MB, name, esp_size // 1024))
finally:
    shutil.rmtree(work, ignore_errors=True)
