#!/usr/bin/env python3
"""Make the sample files the M10 desktop test opens (build/m10-features.sh), as a small FAT disk
image: sample.png, sample.pdf, sample.wav, sample.flac, sample.m4a, sample.mp3 (build/mk-mp3.py), sample.mp4 (mpeg4 + aac), a zip
with a folder in it, a text file and a shell script. Everything is generated here (python3 standard
library; the audio and video encoders are the ones of the FreeLFS ffmpeg run through its own loader),
nothing is downloaded.

    build/m10-samples.py [OUT.img]        default $FLFS_WORK/m10-samples.img

The VM gets it as a second virtio disk (read-only) and copies the files to the tester's home.
"""
import math, os, struct, subprocess, sys, tempfile, wave, zipfile, zlib

WORK = os.environ.get("FLFS_WORK", os.path.expanduser("~/freelfs-work"))
ROOT = os.path.join(WORK, "rootfs-x86_64", ".bootstrap", "pkg", "root")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(WORK, "m10-samples.img")
LOADER = [os.path.join(ROOT, "usr/lib/ld-linux-x86-64.so.2"), "--library-path", os.path.join(ROOT, "usr/lib")]


def tool(name, *args, **kw):
    path = os.path.join(ROOT, "usr/bin", name)
    if not os.path.exists(path):
        path = os.path.join(ROOT, "usr/sbin", name)
    env = dict(os.environ, GCONV_PATH=os.path.join(ROOT, "usr/lib/gconv"), MTOOLS_SKIP_CHECK="1", MTOOLSRC="/dev/null")
    return subprocess.run(LOADER + [path] + list(args), env=env, check=True, **kw)


def png(path, w=640, h=360):
    rows = []
    for y in range(h):
        row = bytearray([0])
        for x in range(w):
            # four coloured quarters with a diagonal ramp, the FreeLFS green in the middle
            r = (x * 255) // w
            g = (y * 255) // h
            b = ((x + y) * 255) // (w + h)
            if abs(x - w // 2) < 60 and abs(y - h // 2) < 60:
                r, g, b = 0x3f, 0x9b, 0x63
            row += bytes((r, g, b))
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(t, b):
        c = struct.pack(">I", len(b)) + t + b
        return c + struct.pack(">I", zlib.crc32(t + b))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def pdf(path):
    pages = ["FreeLFS sample page one", "Second page of the sample document"]
    objs = []
    objs.append(b"<< /Type /Catalog /Pages 2 0 R >>")
    kids = " ".join(f"{3 + 2 * i} 0 R" for i in range(len(pages)))
    objs.append(f"<< /Type /Pages /Kids [{kids}] /Count {len(pages)} >>".encode())
    for i, text in enumerate(pages):
        n = 3 + 2 * i
        objs.append(f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents {n + 1} 0 R "
                    f"/Resources << /Font << /F1 {3 + 2 * len(pages)} 0 R >> >> >>".encode())
        body = f"BT /F1 28 Tf 72 740 Td ({text}) Tj ET\nBT /F1 12 Tf 72 700 Td (Made by build/m10-samples.py for the desktop test.) Tj ET"
        objs.append(f"<< /Length {len(body)} >>\nstream\n{body}\nendstream".encode())
    objs.append(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    out = bytearray(b"%PDF-1.4\n")
    offs = []
    for i, o in enumerate(objs, 1):
        offs.append(len(out))
        out += f"{i} 0 obj\n".encode() + o + b"\nendobj\n"
    x = len(out)
    out += f"xref\n0 {len(objs) + 1}\n0000000000 65535 f \n".encode()
    for o in offs:
        out += f"{o:010d} 00000 n \n".encode()
    out += f"trailer\n<< /Size {len(objs) + 1} /Root 1 0 R >>\nstartxref\n{x}\n%%EOF\n".encode()
    open(path, "wb").write(out)


def wav(path, secs=4, rate=44100):
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(secs * rate):
            v = int(9000 * math.sin(2 * math.pi * 440 * i / rate) * (0.5 + 0.5 * math.sin(2 * math.pi * 2 * i / rate)))
            frames += struct.pack("<hh", v, v)
        w.writeframes(bytes(frames))


def video(path, wavfile, w=320, h=240, fps=25, secs=3):
    frames = bytearray()
    for n in range(fps * secs):
        bx = (n * 8) % (w - 40)
        for y in range(h):
            row = bytearray()
            for x in range(w):
                if 100 <= y < 140 and bx <= x < bx + 40:
                    row += b"\xff\xff\xff"
                else:
                    row += bytes(((x + n * 6) % 256, 90, (255 - x) % 256))
            frames += row
    p = subprocess.Popen(LOADER + [os.path.join(ROOT, "usr/bin/ffmpeg"), "-hide_banner", "-loglevel", "error", "-y",
                                   "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{w}x{h}", "-r", str(fps), "-i", "-",
                                   "-i", wavfile, "-c:v", "mpeg4", "-q:v", "4", "-c:a", "aac", "-shortest", path],
                         stdin=subprocess.PIPE)
    p.communicate(bytes(frames))
    if p.returncode:
        raise SystemExit("ffmpeg failed making the video")


def ff(*args):
    subprocess.run(LOADER + [os.path.join(ROOT, "usr/bin/ffmpeg"), "-hide_banner", "-loglevel", "error", "-y"] + list(args), check=True)


def main():
    d = tempfile.mkdtemp(prefix="m10-samples-", dir=os.path.join(WORK, "scratch"))
    png(f"{d}/sample.png")
    pdf(f"{d}/sample.pdf")
    wav(f"{d}/sample.wav")
    ff("-i", f"{d}/sample.wav", f"{d}/sample.flac")
    ff("-i", f"{d}/sample.wav", "-c:a", "aac", f"{d}/sample.m4a")
    # mp3: the ffmpeg here has a decoder but no encoder; build/mk-mp3.py writes the frames by hand
    subprocess.run([sys.executable, "-I", os.path.join(os.path.dirname(os.path.abspath(__file__)), "mk-mp3.py"),
                    f"{d}/sample.mp3", "12"], check=True, stdout=subprocess.DEVNULL)
    video(f"{d}/sample.mp4", f"{d}/sample.wav")
    with zipfile.ZipFile(f"{d}/project.zip", "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("project/readme.txt", "FreeLFS zip test: this file came out of an archive.\n")
        z.writestr("project/src/main.c", "int main(void)\n{\n\treturn 0;\n}\n")
        z.writestr("project/data.bin", bytes(range(256)) * 64)
    open(f"{d}/notes.txt", "w").write("Plain text file for the editor and the file manager.\nSecond line.\n")
    open(f"{d}/script.sh", "w").write("#!/bin/sh\n# a shell script for syntax colours\nfor i in 1 2 3; do\n\techo \"line $i\" # comment\ndone\n")
    # helpers for the test: run a command as tester in the desktop session (the VM copies them to /usr/bin)
    env = ('u=$(id -u tester)\nE="export XDG_RUNTIME_DIR=/run/user/$u WAYLAND_DISPLAY=wayland-0 '
           'DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$u/bus XDG_CURRENT_DESKTOP=FreeLFS"\n')
    open(f"{d}/m10sh.sh", "w").write("#!/bin/sh\n# m10sh COMMAND...: run a shell command as tester in the session, wait for it\n" + env +
                                     'exec su -s /bin/sh tester -c "$E; $*"\n')
    open(f"{d}/m10app.sh", "w").write("#!/bin/sh\n# m10app COMMAND...: start a program as tester in the session, in the background\n" + env +
                                      'su -s /bin/sh tester -c "$E; setsid sh -c \\"$*\\" >>/tmp/m10app.log 2>&1 </dev/null &"\n')
    names = sorted(os.listdir(d))
    img = os.path.join(d, "samples.img")
    with open(img, "wb") as f:
        f.truncate(48 * 1024 * 1024)
    tool("mkfs.fat", "-F", "16", "-n", "M10SAMPLES", img, stdout=subprocess.DEVNULL)
    for n in names:
        tool("mcopy", "-i", img, os.path.join(d, n), "::/" + n)
    os.replace(img, OUT)
    for n in names:
        os.unlink(os.path.join(d, n))
    os.rmdir(d)
    print("samples:", OUT, " ".join(names))


main()
