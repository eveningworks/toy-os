#!/usr/bin/env python3
"""tools/gen_audio.py -- the WAV files that ship with toy-os.

Written here rather than fetched, for the reason data/wallpapers and
data/icons are generated: a build-time dependency on a host's media
files is the failure that shipped images with no keyboard layouts for
months, silently. These are tracked, so a clean checkout has sound.

**THE FORMATS ARE THE POINT.** Every file is deliberately in a
DIFFERENT one, so the shipped data alone exercises each branch of
userland/lib/usnd.c's conversion stage -- 8-bit unsigned, 16-bit,
mono upmix, and two source rates that are not the device's 48 kHz.
A set of files that were all 48 kHz stereo 16-bit would test the
copy-only fast path and nothing else, on every boot, forever.

    python3 tools/gen_audio.py [--out-sounds DIR] [--out-tests DIR]

No third-party module: WAV is a header and some integers, and pulling
in a dependency to write 44 bytes would be worse than the arithmetic.
"""
import argparse
import math
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# The one file the host-side oracle measures (tools/audio_test.py). A
# SINGLE steady frequency, because that is what a zero-crossing count on
# the host recording can check; the musical files below cannot be
# measured that way and are not asked to be.
TEST_TONE_HZ = 1000
TEST_TONE_MS = 1500
TEST_TONE_RATE = 44100          # NOT 48000: the resampler is in the path

# The second fixture exists so TWO of them can play at once, which is
# the only way to judge a mixer: one recording must contain both
# frequencies. Well separated from TEST_TONE_HZ so neither lands on the
# other's harmonics -- 1000 and 440 share no low multiple.
TEST_TONE2_HZ = 440


def write_wav(path, frames, rate, channels, bits):
    """frames: a list of per-channel int lists, already at `bits` scale."""
    block = channels * (bits // 8)
    body = bytearray()
    for fr in frames:
        for c in range(channels):
            v = fr[c if c < len(fr) else 0]
            if bits == 8:
                body.append(max(0, min(255, v + 128)))
            else:
                body += struct.pack("<h", max(-32768, min(32767, v)))
    hdr = b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate,
                                 rate * block, block, bits)
    hdr += b"data" + struct.pack("<I", len(body))
    with open(path, "wb") as f:
        f.write(hdr + bytes(body))
    print(f"  {os.path.relpath(path, REPO)}  "
          f"{rate} Hz, {channels} ch, {bits}-bit, {len(frames)} frames")


def envelope(i, n, attack=0.01, release=0.3):
    """A click-free amplitude ramp. A tone that starts at full scale
    pops -- the discontinuity is a broadband transient, and it is what
    makes a naively generated effect sound like a fault."""
    a = int(n * attack) or 1
    r = int(n * release) or 1
    if i < a:
        return i / a
    if i > n - r:
        return max(0.0, (n - i) / r)
    return 1.0


def tone(rate, ms, hz, amp=0.5, channels=1, bits=16, detune=0.0):
    n = int(rate * ms / 1000)
    peak = 127 if bits == 8 else 32767
    out = []
    for i in range(n):
        e = envelope(i, n) * amp * peak
        fr = []
        for c in range(channels):
            f = hz * (1.0 + (detune if c else 0.0))
            fr.append(int(e * math.sin(2 * math.pi * f * i / rate)))
        out.append(fr)
    return out


def steady(rate, ms, hz, amp=0.5, channels=2, bits=16):
    """No envelope: the measured file must hold ONE frequency for its
    whole length, or the host's zero-crossing count measures the ramp."""
    n = int(rate * ms / 1000)
    peak = 32767 if bits == 16 else 127
    return [[int(amp * peak * math.sin(2 * math.pi * hz * i / rate))] * channels
            for i in range(n)]


def arpeggio(rate, notes, ms_each, amp=0.4, channels=2, bits=16):
    out = []
    for hz in notes:
        out += tone(rate, ms_each, hz, amp, channels, bits, detune=0.002)
    return out


def noise_burst(rate, ms, amp=0.6, bits=8):
    """A mine going off. A 32-bit LCG rather than `random`, so the file
    is byte-identical on every machine that regenerates it -- a tracked
    binary that changes for no reason is a diff nobody can review."""
    n = int(rate * ms / 1000)
    peak = 127 if bits == 8 else 32767
    seed = 0x12345678
    out = []
    for i in range(n):
        seed = (seed * 1103515245 + 12345) & 0xFFFFFFFF
        v = ((seed >> 16) & 0xFFFF) / 32768.0 - 1.0
        # A low-passed decay reads as a thud rather than as static.
        decay = (1.0 - i / n) ** 2
        out.append([int(v * amp * decay * peak)])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-sounds", default=os.path.join(REPO, "data/usr/share/sounds"))
    ap.add_argument("--out-tests", default=os.path.join(REPO, "data/tests"))
    args = ap.parse_args()

    os.makedirs(args.out_sounds, exist_ok=True)
    os.makedirs(args.out_tests, exist_ok=True)
    print("sounds:")

    # 44.1 kHz stereo 16-bit -- the commonest shape in the world, and the
    # one that proves the resampler runs on the ordinary path.
    write_wav(os.path.join(args.out_sounds, "chime.wav"),
              arpeggio(44100, [523, 659, 784], 140), 44100, 2, 16)

    # 22.05 kHz mono -- the mono upmix AND a 2:1 ratio.
    write_wav(os.path.join(args.out_sounds, "click.wav"),
              tone(22050, 35, 900, amp=0.35, channels=1), 22050, 1, 16)

    # 48 kHz mono -- the rate fast path with the upmix still in the way.
    write_wav(os.path.join(args.out_sounds, "flag.wav"),
              tone(48000, 45, 1400, amp=0.3, channels=1), 48000, 1, 16)

    # 8-BIT UNSIGNED, the encoding nothing else here produces.
    write_wav(os.path.join(args.out_sounds, "boom.wav"),
              noise_burst(44100, 420), 44100, 1, 8)

    write_wav(os.path.join(args.out_sounds, "win.wav"),
              arpeggio(44100, [523, 659, 784, 1046], 120), 44100, 2, 16)

    print("fixtures:")
    write_wav(os.path.join(args.out_tests, "sine1k.wav"),
              steady(TEST_TONE_RATE, TEST_TONE_MS, TEST_TONE_HZ),
              TEST_TONE_RATE, 2, 16)
    write_wav(os.path.join(args.out_tests, "sine440.wav"),
              steady(TEST_TONE_RATE, TEST_TONE_MS, TEST_TONE2_HZ),
              TEST_TONE_RATE, 2, 16)


if __name__ == "__main__":
    main()
