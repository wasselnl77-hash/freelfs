#!/usr/bin/env python3
"""Synthesise a small music library under DIR/ARTIST/ALBUM/NN Title.ogg with tags.

    python3 -I mkaudio.py DIR

Each track is a short melody (a random walk on a pentatonic scale, a few harmonics,
a plucked-string envelope) written as 22 kHz WAV with the standard library, then
encoded and tagged with tests/tagenc (GStreamer's vorbisenc and oggmux; $TAGENC names it). One WAV is
left untagged on purpose (the title then comes from the file name). Everything is
made up here; nothing is copied from anywhere."""
import math, os, random, struct, subprocess, sys, wave

RATE = 22050
LIB = [
    ("Northern Lights Ensemble", "Harbour Lights", 220.0, 96,
     ["Morning Ferry", "Copper Roofs", "Salt and Rope", "Low Tide Walk"]),
    ("Northern Lights Ensemble", "Slow Tide", 196.0, 66,
     ["First Water", "Drift", "Evening Mooring"]),
    ("Copper Kettle", "Orchard Sessions", 261.6, 120,
     ["Windfall", "Ladder Song", "Cider Press", "Late Harvest"]),
    ("Marta Veil", "Glass Meadow", 246.9, 84,
     ["Clear Morning", "Dew", "Long Grass", "Paper Kite", "Meadow Hymn"]),
    ("Marta Veil", "Night Ferry", 174.6, 72,
     ["Lamp Room", "Crossing", "Far Shore"]),
    ("The Quarry Choir", "Granite", 207.7, 100,
     ["Cut Stone", "Echo Floor", "Gravel Road"]),
]
SCALE = [0, 2, 4, 7, 9, 12, 14, 16]


def tone(freq, dur, vol, bright):
    n = int(dur * RATE)
    out = []
    for i in range(n):
        t = i / RATE
        env = math.exp(-t * (3.0 + 2 * bright)) * min(1.0, i / 120.0)
        s = (math.sin(2 * math.pi * freq * t) + 0.5 * bright * math.sin(4 * math.pi * freq * t)
             + 0.25 * bright * math.sin(6 * math.pi * freq * t))
        out.append(s * env * vol)
    return out


def melody(seed, root, bpm, bars=6):
    rnd = random.Random(seed)
    beat = 60.0 / bpm
    mix = [0.0] * int((bars * 4 * beat + 1.2) * RATE)
    deg = 2
    t = 0.0
    while t < bars * 4 * beat:
        deg = max(0, min(len(SCALE) - 1, deg + rnd.choice([-2, -1, -1, 0, 1, 1, 2])))
        f = root * 2 ** (SCALE[deg] / 12.0)
        dur = beat * rnd.choice([0.5, 1, 1, 1.5, 2])
        for k, v in enumerate(tone(f, dur + 0.6, 0.30, 0.6)):
            j = int(t * RATE) + k
            if j < len(mix):
                mix[j] += v
        if int(t / beat) % 4 == 0:  # a bass note on the bar
            for k, v in enumerate(tone(root / 2, beat * 2, 0.22, 0.2)):
                j = int(t * RATE) + k
                if j < len(mix):
                    mix[j] += v
        t += dur
    peak = max(abs(x) for x in mix) or 1.0
    return [x / peak * 0.8 for x in mix]


def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(x * 32767)) for x in samples))


def main():
    d = sys.argv[1]
    seed = 1
    for artist, album, root, bpm, titles in LIB:
        folder = os.path.join(d, artist, album)
        os.makedirs(folder, exist_ok=True)
        for no, title in enumerate(titles, 1):
            seed += 1
            wav = os.path.join(folder, "tmp.wav")
            write_wav(wav, melody(seed, root, bpm + 4 * no))
            out = os.path.join(folder, "%02d %s.ogg" % (no, title))
            subprocess.check_call([os.environ.get("TAGENC", "tagenc"), wav, out, title, artist, album, str(no)])
            os.unlink(wav)
    # an untagged file, for the file-name fallback
    loose = os.path.join(d, "Loose")
    os.makedirs(loose, exist_ok=True)
    write_wav(os.path.join(loose, "Untitled sketch.wav"), melody(99, 233.1, 90, 3))


main()
