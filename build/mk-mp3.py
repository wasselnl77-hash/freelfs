#!/usr/bin/env python3
"""Write a test MP3 without an MP3 encoder (the FreeLFS ffmpeg is an LGPL build: it decodes MP3 but has no
encoder). The frames are built by hand: MPEG-1 Layer III, 44.1 kHz, mono, 128 kbit/s, and in every granule
one non-zero spectral line (Huffman table 1) with the sign alternating, so a decoder gets a steady buzz of
about 765 Hz instead of silence. It is a decoder test file, not music.

    build/mk-mp3.py OUT.mp3 [SECONDS]
"""
import sys


class Bits:
    def __init__(self):
        self.v, self.n = 0, 0

    def put(self, val, bits):
        self.v = (self.v << bits) | (val & ((1 << bits) - 1))
        self.n += bits

    def bytes(self, size):
        pad = size * 8 - self.n
        assert pad >= 0, "frame too small"
        return (self.v << pad).to_bytes(size, "big")


def frame(sign):
    # main data: ten (0,0) pairs ('1' each), then the pair (1,0): '01' + the sign bit; the line is no. 20
    huff = Bits()
    for _ in range(10):
        huff.put(1, 1)
    huff.put(0b01, 2)
    huff.put(sign, 1)
    nbits = huff.n  # 13
    side = Bits()
    side.put(0, 9)   # main_data_begin: this frame's own data
    side.put(0, 5)   # private bits (mono)
    side.put(0, 4)   # scfsi
    for gr in range(2):
        side.put(nbits, 12)  # part2_3_length (scalefactors take 0 bits: slen = 0, 0)
        side.put(11, 9)     # big_values: 11 pairs = lines 0..21
        side.put(200, 8)    # global_gain
        side.put(0, 4)      # scalefac_compress
        side.put(0, 1)      # no window switching
        for _ in range(3):
            side.put(1, 5)  # table_select: table 1 for all three regions
        side.put(0, 4)      # region0_count
        side.put(0, 3)      # region1_count
        side.put(0, 1)      # preflag
        side.put(0, 1)      # scalefac_scale
        side.put(0, 1)      # count1table_select
    # both granules share one main data block: granule 1 follows granule 0 directly (the same bits again)
    main = Bits()
    main.put(huff.v, huff.n)
    main.put(huff.v, huff.n)
    hdr = bytes([0xFF, 0xFB, 0x90, 0xC0])  # MPEG-1, Layer III, no CRC, 128 kbit/s, 44.1 kHz, mono
    size = 417
    body = side.bytes(17) + main.bytes(size - 4 - 17)
    return hdr + body


def main():
    out = sys.argv[1]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 4
    n = int(secs * 44100 / 1152)
    with open(out, "wb") as f:
        for i in range(n):
            f.write(frame(i & 1))
    print("wrote", out, n, "frames")


main()
