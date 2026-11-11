#!/usr/bin/env python3
"""A test disk for the live ISO's "ISO file on a Ventoy-like stick" path.

    build/iso-test-disk.py ISO OUT.img exfat|fat32|ventoy [SUBDIR]

OUT.img is a GPT disk: partition 1 is the ISO's own ESP (flboot, kernel,
initrd.img, flboot.conf: GPT partition 2 of the ISO), partition 2 is a data
partition (exFAT or FAT32) holding the ISO as SUBDIR/freelfs-live.iso (SUBDIR
default: none, the top level). QEMU boots it through OVMF -> flboot ->
kernel -> flinitrd, which has to find the ISO in the data partition. Rootless:
FAT32 comes from FreeLFS's own mkfs.fat and mtools, exFAT from the writer
below (a single contiguous file, which is also how Ventoy lays ISOs out).
With "ventoy" the disk is laid out like a Ventoy stick instead: exFAT data
partition 1 with the ISO, then Ventoy's own 32 MB EFI partition (VTOYEFI,
from the release tarball in $FLFS_WORK/cache/ventoy), no ESP of ours at all.
"""
import os, struct, subprocess, sys, tempfile

iso, out, fs = sys.argv[1:4]
subdir = sys.argv[4] if len(sys.argv) > 4 else ""
WORK = os.environ.get("FLFS_WORK", os.path.expanduser("~/freelfs-work"))
ROOT = os.environ.get("FLFS_IMAGE_ROOT", WORK + "/rootfs-x86_64/.bootstrap/pkg/root")
LOADER = ROOT + "/usr/lib/ld-linux-x86-64.so.2"


def tool(name, *args, **kw):
    path = ROOT + "/usr/bin/" + name
    if not os.path.exists(path):
        path = ROOT + "/usr/sbin/" + name
    env = dict(os.environ, GCONV_PATH=ROOT + "/usr/lib/gconv", MTOOLS_SKIP_CHECK="1", MTOOLSRC="/dev/null")
    return subprocess.run([LOADER, "--library-path", ROOT + "/usr/lib", path, *args], env=env, check=True, **kw)


def copy_range(src, soff, dst, doff, n):
    with open(src, "rb") as f, open(dst, "r+b") as g:
        f.seek(soff)
        g.seek(doff)
        left = n
        while left:
            b = f.read(min(left, 1 << 22))
            if not b:
                break
            g.write(b)
            left -= len(b)


# ---- the ESP: GPT partition 2 of the ISO ---------------------------------------------
with open(iso, "rb") as f:
    f.seek(512)
    hdr = f.read(92)
    assert hdr[:8] == b"EFI PART", "the ISO has no GPT"
    entries_lba, n, size = struct.unpack("<QII", hdr[72:88])
    f.seek(entries_lba * 512)
    table = f.read(n * size)
ESP = bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b")
esp = None
for i in range(n):
    e = table[i * size:(i + 1) * size]
    if e[:16] == ESP:
        esp = struct.unpack("<QQ", e[32:48])
assert esp, "the ISO has no ESP partition"
esp_first, esp_last = esp
esp_sectors = esp_last - esp_first + 1

iso_size = os.path.getsize(iso)
MB = 1 << 20
data_mb = (iso_size + 96 * MB + MB - 1) // MB  # room for the file system's own structures
ventoy = fs == "ventoy"
VT = WORK + "/cache/ventoy/x/ventoy-1.1.18/ventoy/ventoy.disk.img"
hdr = "label: gpt\nlabel-id: f1ee1f50-0000-4000-8000-0000000000e1\nunit: sectors\nfirst-lba: 2048\n\n"
BASIC = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
if ventoy:
    data_start = 2048
    vt_start = data_start + data_mb * 2048
    disk_bytes = (vt_start + os.path.getsize(VT) // 512 + 2048) * 512
    layout = (f"start={data_start}, size={data_mb * 2048}, type={BASIC}, name=\"Ventoy\"\n"
              f"start={vt_start}, size={os.path.getsize(VT) // 512}, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name=\"VTOYEFI\"\n")
else:
    data_start = 2048 + (esp_sectors + 2047) // 2048 * 2048
    disk_bytes = (data_start + data_mb * 2048 + 2048) * 512
    layout = (f"start=2048, size={esp_sectors}, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, uuid=f1ee1f50-0000-4000-8000-0000000000e2, name=\"ESP\"\n"
              f"start={data_start}, size={data_mb * 2048}, type={BASIC}, uuid=f1ee1f50-0000-4000-8000-0000000000e3, name=\"Ventoy-like data\"\n")
with open(out, "wb") as f:
    f.truncate(disk_bytes)
if ventoy:
    # Ventoy wants its own MBR (boot.img, which carries its signature) with an
    # exFAT partition from 1 MiB and its 32 MB EFI partition (type 0xEF) after it
    mbr = bytearray(open(os.path.dirname(VT) + "/../boot/boot.img", "rb").read())
    assert len(mbr) == 512
    nvt = os.path.getsize(VT) // 512
    mbr[440:444] = b"\x1e\xe1\xf1\x50"
    mbr[446:462] = struct.pack("<B3sB3sII", 0x80, b"\xfe\xff\xff", 0x07, b"\xfe\xff\xff", data_start, data_mb * 2048)
    mbr[462:478] = struct.pack("<B3sB3sII", 0x00, b"\xfe\xff\xff", 0xEF, b"\xfe\xff\xff", vt_start, nvt)
    mbr[510:512] = b"\x55\xAA"
    with open(out, "r+b") as f:
        f.write(mbr)
    copy_range(VT, 0, out, vt_start * 512, os.path.getsize(VT))
else:
    tool("sfdisk", "--quiet", out, input=(hdr + layout).encode())
if ventoy:
    pass
else:
    copy_range(iso, esp_first * 512, out, 2048 * 512, esp_sectors * 512)

part = tempfile.mkdtemp(prefix="isotest", dir=WORK) + "/data.img"
with open(part, "wb") as f:
    f.truncate(data_mb * MB)
NAME = "freelfs-live.iso"


# ---- exFAT -----------------------------------------------------------------------------------------
def ror(v, bits, w):
    return ((v >> 1) | ((v & 1) << (w - 1))) & ((1 << w) - 1)


def exfat_make(path, iso, subdir):
    BPS, SPC = 512, 8
    CL = BPS * SPC
    vol_sectors = os.path.getsize(path) // BPS
    fat_off = 128
    est = vol_sectors // SPC
    fat_len = (est * 4 + BPS - 1) // BPS + 1
    heap_off = (fat_off + fat_len + SPC - 1) // SPC * SPC
    ccount = (vol_sectors - heap_off) // SPC
    upcase = bytearray()
    for c in range(65536):
        upcase += struct.pack("<H", c - 32 if 97 <= c <= 122 else c)
    cs = 0
    for b in upcase:
        cs = (ror(cs, 32, 32) + b) & 0xFFFFFFFF
    nbm = (ccount + 7) // 8
    bm_cl = (nbm + CL - 1) // CL
    up_cl = (len(upcase) + CL - 1) // CL
    iso_cl = (os.path.getsize(iso) + CL - 1) // CL
    nxt = 2
    bm_first = nxt; nxt += bm_cl
    up_first = nxt; nxt += up_cl
    root_first = nxt; nxt += 1
    dir_first = None
    if subdir:
        dir_first = nxt; nxt += 1
    file_first = nxt; nxt += iso_cl
    assert nxt - 2 <= ccount, "the data partition is too small"

    def upper(s):
        return "".join(chr(ord(c) - 32) if "a" <= c <= "z" else c for c in s)

    def fentry(name, attr, first, length, contiguous):
        nb = name.encode("utf-16-le")
        nchars = len(nb) // 2
        h = 0
        for b in upper(name).encode("utf-16-le"):
            h = (ror(h, 16, 16) + b) & 0xFFFF
        names = []
        for i in range(0, len(nb), 30):
            names.append(b"\xC1\x00" + nb[i:i + 30].ljust(30, b"\0"))
        ts = 0x00210000  # 1980-01-01
        prim = bytearray(32)
        prim[0] = 0x85
        prim[1] = 1 + len(names)
        struct.pack_into("<H", prim, 4, attr)
        struct.pack_into("<III", prim, 8, ts, ts, ts)
        stream = bytearray(32)
        stream[0] = 0xC0
        stream[1] = 3 if contiguous else 1
        stream[3] = nchars
        struct.pack_into("<H", stream, 4, h)
        struct.pack_into("<Q", stream, 8, length)
        struct.pack_into("<I", stream, 20, first)
        struct.pack_into("<Q", stream, 24, length)
        raw = bytearray(prim + stream + b"".join(names))
        c = 0
        for i, b in enumerate(raw):
            if i in (2, 3):
                continue
            c = (ror(c, 16, 16) + b) & 0xFFFF
        struct.pack_into("<H", raw, 2, c)
        return bytes(raw)

    root = bytearray(CL)
    bm = bytearray(32); bm[0] = 0x81
    struct.pack_into("<IQ", bm, 20, bm_first, nbm)
    up = bytearray(32); up[0] = 0x82
    struct.pack_into("<I", up, 4, cs)
    struct.pack_into("<IQ", up, 20, up_first, len(upcase))
    root[0:64] = bm + up
    fdata = fentry(NAME, 0x20, file_first, os.path.getsize(iso), True)
    if subdir:
        root[64:64 + 0] = b""
        ent = fentry(subdir, 0x10, dir_first, CL, True)
        root[64:64 + len(ent)] = ent
        d = bytearray(CL)
        d[0:len(fdata)] = fdata
    else:
        root[64:64 + len(fdata)] = fdata
        d = None

    bitmap = bytearray(nbm)
    for c in range(2, nxt):
        bitmap[(c - 2) // 8] |= 1 << ((c - 2) % 8)
    fat = bytearray(fat_len * BPS)
    struct.pack_into("<II", fat, 0, 0xFFFFFFF8, 0xFFFFFFFF)

    def chain(first, n):
        for i in range(n):
            struct.pack_into("<I", fat, (first + i) * 4, first + i + 1 if i < n - 1 else 0xFFFFFFFF)

    chain(bm_first, bm_cl); chain(up_first, up_cl); chain(root_first, 1)

    boot = bytearray(512)
    boot[0:3] = b"\xEB\x76\x90"
    boot[3:11] = b"EXFAT   "
    struct.pack_into("<QQIIIIII", boot, 64, 0, vol_sectors, fat_off, fat_len, heap_off, ccount, root_first, 0xF1EE0E01)
    struct.pack_into("<HH", boot, 104, 0x0100, 0)
    boot[108] = 9; boot[109] = 3; boot[110] = 1; boot[111] = 0x80; boot[112] = 0xFF
    boot[120:510] = b"\xF4" * 390
    boot[510:512] = b"\x55\xAA"
    region = bytearray(boot)
    for _ in range(8):
        s = bytearray(512); s[508:512] = b"\x00\x00\x55\xAA"
        region += s
    region += bytes(1024)
    c = 0
    for i, b in enumerate(region):
        if i in (106, 107, 112):
            continue
        c = (ror(c, 32, 32) + b) & 0xFFFFFFFF
    region += struct.pack("<I", c) * 128

    def off(cl):
        return (heap_off + (cl - 2) * SPC) * BPS

    with open(path, "r+b") as f:
        f.seek(0); f.write(region)
        f.seek(12 * BPS); f.write(region)
        f.seek(fat_off * BPS); f.write(fat)
        f.seek(off(bm_first)); f.write(bitmap)
        f.seek(off(up_first)); f.write(upcase)
        f.seek(off(root_first)); f.write(root)
        if d is not None:
            f.seek(off(dir_first)); f.write(d)
    copy_range(iso, 0, path, off(file_first), os.path.getsize(iso))


if fs in ("exfat", "ventoy"):
    exfat_make(part, iso, subdir)
elif fs == "fat32":
    tool("mkfs.fat", "-F", "32", "-n", "VENTOYLIKE", "-i", "F1EE0F32", part, stdout=subprocess.DEVNULL)
    if subdir:
        tool("mmd", "-i", part, "::/" + subdir)
    tool("mcopy", "-i", part, iso, "::/" + (subdir + "/" if subdir else "") + NAME)
else:
    sys.exit("fs is exfat, fat32 or ventoy")
copy_range(part, 0, out, data_start * 512, data_mb * MB)
os.unlink(part)
os.rmdir(os.path.dirname(part))
print(f"{out}: {fs} data partition with {NAME}" + (f" in {subdir}/" if subdir else ""))
