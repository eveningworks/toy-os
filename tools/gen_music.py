#!/usr/bin/env python3
"""tools/gen_music.py -- the music files that ship with toy-os: MP3s, and
the same score as a MIDI file.

Written here rather than fetched, the same call tools/gen_audio.py made
for the WAVs and for the same reason: a build-time dependency on
somebody else's media is the failure that shipped images with no
keyboard layouts for months, silently. The output is TRACKED, so a
clean checkout has music. It is also ours, so the repository's own
licence covers it and LICENSE needs no third-party entry.

**THE ENCODE IS AS MUCH THE POINT AS THE TUNE.** gen_audio.py puts each
shipped WAV in a different format so the data alone exercises every
branch of usnd's conversion stage; this does the same one layer up, for
userland/lib/usnd_mp3.c:

    first-boot.mp3   44.1 kHz, JOINT STEREO, VBR, with an ID3v2 tag.
                     Joint stereo means mid/side frames, VBR means a
                     Xing header (which is a real frame carrying no
                     audio -- a decoder that plays it emits a click),
                     and the tag means the probe has to skip a few KiB
                     before it ever sees a sync word.
    sine1k.mp3       44.1 kHz, MONO, CBR 128k, no tag. The mono upmix,
                     a fixed frame size, and no Xing frame to skip.

and for userland/lib/usnd_flac.c, when `flac` is on PATH:

    first-boot.flac  the first 20 s at 24 BITS, 44.1 kHz, with Vorbis
                     comments and a front-cover PICTURE -- what a hi-res
                     file looks like, and what reaches the Player's tags.
    sine1k.flac      the same tone as sine1k.mp3, 16-bit mono.
    ramp24.flac      24-bit stereo whose every sample is a FORMULA of its
                     index, so /tests/usnd_test checks exactness with no
                     reference decoder in the guest.

sine1k.mp3 is a STEADY 1 kHz tone because tools/audio_test.py judges
playback by counting zero crossings in QEMU's own recording -- an oracle
that shares no code with the decoder. Music cannot be measured that way;
a sine can.

    python3 tools/gen_music.py [--out-music DIR] [--out-tests DIR] [--midi-only]

Needs `lame` on PATH to encode (not for --midi-only). Nothing in the
build runs this: the outputs are tracked, and this is how they are
regenerated and reviewed.
"""
import argparse
import array
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

RATE = 44100
BPM = 100
BEAT = 60.0 / BPM               # seconds
BAR = 4 * BEAT

# --- oscillators ------------------------------------------------------
#
# Band-limited by construction: each wavetable is a partial sum of
# harmonics, not a sampled ideal shape. A naive saw ramp aliases into
# audible inharmonic tones the moment it is played back at a pitch that
# does not divide the table length, and the encoder then spends bits on
# the aliases.
TABLE = 4096


def _table(harmonics):
    """One cycle, from (harmonic, amplitude, phase) triples."""
    t = array.array("d", [0.0]) * TABLE
    for i in range(TABLE):
        v = 0.0
        for h, amp, ph in harmonics:
            v += amp * math.sin(2 * math.pi * h * i / TABLE + ph)
        t[i] = v
    peak = max(abs(v) for v in t) or 1.0
    for i in range(TABLE):
        t[i] /= peak
    return t


SINE = _table([(1, 1.0, 0.0)])
# 12 harmonics: rich enough to sound like a string pad, and still clean
# for a fundamental up to ~1.8 kHz at this rate.
SAW = _table([(h, 1.0 / h, 0.0) for h in range(1, 13)])
SQUARE = _table([(h, 1.0 / h, 0.0) for h in range(1, 16, 2)])
TRI = _table([(h, (1.0 / (h * h)) * (1 if (h // 2) % 2 == 0 else -1), 0.0)
              for h in range(1, 16, 2)])


def midi_hz(n):
    return 440.0 * (2.0 ** ((n - 69) / 12.0))


class Noise:
    """A 32-bit LCG rather than `random`, so a regenerated file is
    byte-identical -- a tracked binary that changes for no reason is a
    diff nobody can review. Same rule, same constants, as gen_audio.py."""

    def __init__(self, seed=0x12345678):
        self.s = seed

    def __call__(self):
        self.s = (self.s * 1103515245 + 12345) & 0xFFFFFFFF
        return ((self.s >> 16) & 0xFFFF) / 32768.0 - 1.0


def adsr(n, a, d, s, r):
    """Per-sample amplitude, as a list. Attack and release are never
    zero samples: a note that starts at full scale pops, and the pop is
    broadband -- it is what makes naive synthesis sound like a fault."""
    a = max(1, int(a * RATE))
    d = max(1, int(d * RATE))
    r = max(1, int(r * RATE))
    body = max(0, n - a - d - r)
    env = array.array("d", [0.0]) * n
    i = 0
    for k in range(min(a, n)):
        env[i] = k / a
        i += 1
    for k in range(min(d, n - i)):
        env[i] = 1.0 - (1.0 - s) * (k / d)
        i += 1
    for _ in range(min(body, n - i)):
        env[i] = s
        i += 1
    left = n - i
    for k in range(left):
        env[i] = s * max(0.0, 1.0 - k / r)
        i += 1
    return env


def osc(table, hz, n, env, detune=0.0, phase0=0.0):
    """One voice: `n` samples of `table` at `hz`, scaled by `env`.

    Linearly interpolated between table entries. Nearest-neighbour is
    cheaper and adds a noise floor that rises with pitch -- inaudible on
    its own and clearly there once a dozen voices carry it."""
    out = array.array("d", [0.0]) * n
    step = (hz * (1.0 + detune)) * TABLE / RATE
    ph = phase0
    for i in range(n):
        j = int(ph)
        frac = ph - j
        a = table[j & (TABLE - 1)]
        b = table[(j + 1) & (TABLE - 1)]
        out[i] = (a + (b - a) * frac) * env[i]
        ph += step
        if ph >= TABLE:
            ph -= TABLE
    return out


def lowpass(buf, cutoff_hz):
    """One-pole. Its job is taking the edge off the square and saw
    tables, not filter design."""
    a = 1.0 - math.exp(-2 * math.pi * cutoff_hz / RATE)
    y = 0.0
    for i in range(len(buf)):
        y += a * (buf[i] - y)
        buf[i] = y
    return buf


# --- instruments ------------------------------------------------------
#
# Each returns a mono buffer. The arrangement below places them; nothing
# here knows where it sits in the piece.

def pad(notes, beats, amp=0.16):
    """A held chord. Two detuned saws per note, which is what makes it
    move rather than sit still."""
    n = int(beats * BEAT * RATE)
    env = adsr(n, 0.35, 0.2, 0.75, 0.5)
    out = array.array("d", [0.0]) * n
    for m in notes:
        hz = midi_hz(m)
        for det in (-0.0025, 0.0025):
            v = osc(SAW, hz, n, env, detune=det)
            for i in range(n):
                out[i] += v[i] * amp
    return lowpass(out, 2200)


def bass(m, beats, amp=0.5):
    n = int(beats * BEAT * RATE)
    env = adsr(n, 0.005, 0.12, 0.6, 0.08)
    out = osc(SQUARE, midi_hz(m), n, env)
    for i in range(n):
        out[i] *= amp
    return lowpass(out, 420)


def lead(m, beats, amp=0.34):
    """Sine plus a quiet square an octave up, with vibrato. The square
    is what stops it disappearing into the pad."""
    n = int(beats * BEAT * RATE)
    env = adsr(n, 0.02, 0.1, 0.7, 0.25)
    hz = midi_hz(m)
    out = array.array("d", [0.0]) * n
    body = osc(SINE, hz, n, env)
    top = osc(SQUARE, hz * 2, n, env)
    for i in range(n):
        # ~5 Hz vibrato, applied as amplitude rather than pitch: a pitch
        # LFO needs the phase accumulator, and this reads the same at
        # this depth.
        vib = 1.0 + 0.06 * math.sin(2 * math.pi * 5.0 * i / RATE)
        out[i] = (body[i] + 0.18 * top[i]) * amp * vib
    return out


def pluck(m, beats, amp=0.22):
    n = int(beats * BEAT * RATE)
    env = adsr(n, 0.002, 0.18, 0.0, 0.02)
    out = osc(TRI, midi_hz(m), n, env)
    for i in range(n):
        out[i] *= amp
    return out


def kick(amp=0.85):
    """A pitch sweep, 130 Hz down to 45, which is a kick drum. Written
    as a phase accumulator because the frequency changes per sample and
    a table lookup at a fixed step cannot express that."""
    n = int(0.28 * RATE)
    out = array.array("d", [0.0]) * n
    ph = 0.0
    for i in range(n):
        t = i / n
        hz = 45.0 + 85.0 * math.exp(-9.0 * t)
        ph += 2 * math.pi * hz / RATE
        out[i] = math.sin(ph) * amp * math.exp(-5.0 * t)
    return out


def snare(rng, amp=0.4):
    n = int(0.20 * RATE)
    out = array.array("d", [0.0]) * n
    for i in range(n):
        t = i / n
        body = math.sin(2 * math.pi * 190.0 * i / RATE) * 0.35
        out[i] = (rng() * 0.8 + body) * amp * math.exp(-11.0 * t)
    return lowpass(out, 6500)


def hat(rng, amp=0.16, secs=0.05):
    n = int(secs * RATE)
    out = array.array("d", [0.0]) * n
    for i in range(n):
        out[i] = rng() * amp * math.exp(-40.0 * (i / n))
    # High-passed by subtracting a lowpassed copy: a hat is the part a
    # lowpass throws away.
    lp = lowpass(array.array("d", out), 3000)
    for i in range(n):
        out[i] -= lp[i]
    return out


# --- the piece --------------------------------------------------------
#
# A minor, 100 BPM, 32 bars. Four sections that each ADD a layer, which
# is the cheapest way to make a loop sound like an arrangement.
#
#   1-4    pad alone
#   5-12   + bass, kick, hats, arpeggio
#   13-20  + melody and snare
#   21-28  melody an octave up, busier kit
#   29-32  everything drops away but the pad
#
# MIDI numbers: A3 = 57, C4 = 60, A4 = 69.
CHORDS = [
    # (root for the bass, the pad's three notes)
    (45, (57, 60, 64)),   # Am
    (41, (57, 60, 65)),   # F
    (48, (60, 64, 67)),   # C
    (43, (59, 62, 67)),   # G
]

# (bar offset within the 8-bar phrase, beat, length in beats, midi)
MELODY = [
    (0, 0.0, 1.5, 69), (0, 1.5, 0.5, 72), (0, 2.0, 2.0, 71),
    (1, 0.0, 1.0, 69), (1, 1.0, 1.0, 67), (1, 2.0, 2.0, 65),
    (2, 0.0, 1.5, 64), (2, 1.5, 0.5, 67), (2, 2.0, 2.0, 69),
    (3, 0.0, 3.0, 67),
    (4, 0.0, 1.0, 72), (4, 1.0, 1.0, 71), (4, 2.0, 2.0, 69),
    (5, 0.0, 1.5, 65), (5, 1.5, 0.5, 64), (5, 2.0, 2.0, 65),
    (6, 0.0, 1.0, 67), (6, 1.0, 1.0, 69), (6, 2.0, 2.0, 72),
    (7, 0.0, 4.0, 69),
]

BARS = 32


def add(dst_l, dst_r, src, at, pan=0.0, gain=1.0):
    """Mix `src` in at sample `at`, panned -1..+1. Constant-power, so a
    voice does not get louder as it moves to the middle."""
    ang = (pan + 1.0) * math.pi / 4.0
    gl = math.cos(ang) * gain * math.sqrt(2.0)
    gr = math.sin(ang) * gain * math.sqrt(2.0)
    n = min(len(src), len(dst_l) - at)
    for i in range(n):
        v = src[i]
        dst_l[at + i] += v * gl
        dst_r[at + i] += v * gr


def render():
    total = int(BARS * BAR * RATE) + RATE      # a second of tail
    L = array.array("d", [0.0]) * total
    R = array.array("d", [0.0]) * total
    rng = Noise()

    # Rendered ONCE and spliced. Every voice here repeats, and rendering
    # each occurrence would be the same arithmetic a few hundred times.
    pads = [pad(ch[1], 4) for ch in CHORDS]
    kick_buf = kick()
    snare_buf = snare(rng)
    hat_buf = hat(rng)

    for bar in range(BARS):
        t0 = int(bar * BAR * RATE)
        ch = CHORDS[bar % 4]
        phase_bar = bar % 8

        add(L, R, pads[bar % 4], t0, gain=1.0)

        if 4 <= bar < 28:
            add(L, R, bass(ch[0], 4), t0, gain=1.0)
            for b in range(4):
                add(L, R, kick_buf, t0 + int(b * BEAT * RATE),
                    gain=1.0 if b % 2 == 0 else 0.55)
            for b in range(8):
                add(L, R, hat_buf, t0 + int(b * 0.5 * BEAT * RATE),
                    pan=0.35 if b % 2 else -0.35)
            # An arpeggio of the chord, alternating across the stereo
            # field. Eighth notes, so it sits under the melody.
            for b in range(8):
                m = ch[1][b % 3] + (12 if b >= 4 else 0)
                add(L, R, pluck(m, 0.5), t0 + int(b * 0.5 * BEAT * RATE),
                    pan=-0.5 if b % 2 else 0.5)

        if 12 <= bar < 28:
            for b in (1, 3):
                add(L, R, snare_buf, t0 + int(b * BEAT * RATE))
            for mb, beat, length, m in MELODY:
                if mb != phase_bar % 8:
                    continue
                note = m + (12 if bar >= 20 else 0)
                add(L, R, lead(note, length),
                    t0 + int(beat * BEAT * RATE), pan=0.12)

    # A delay on the whole mix, wide enough to hear and short enough not
    # to smear the drums. Feedback is applied IN PLACE and reads samples
    # it has already written, which is what makes it a tail rather than
    # a single echo.
    d = int(0.30 * RATE)
    fb, mix = 0.32, 0.22
    for i in range(d, total):
        L[i] += R[i - d] * fb * mix
        R[i] += L[i - d] * fb * mix

    peak = max(max(abs(v) for v in L), max(abs(v) for v in R)) or 1.0
    # -1 dBFS. Encoding a mix that touches full scale is how an MP3 comes
    # back clipped: the decoder's output is not bounded by the input's.
    g = 0.891 / peak
    return L, R, g


def write_wav(path, chans, gain, rate=RATE):
    n = len(chans[0])
    body = bytearray()
    for i in range(n):
        for c in chans:
            v = int(max(-32768, min(32767, c[i] * gain * 32767)))
            body += struct.pack("<h", v)
    block = len(chans) * 2
    hdr = b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, len(chans), rate,
                                 rate * block, block, 16)
    hdr += b"data" + struct.pack("<I", len(body))
    with open(path, "wb") as f:
        f.write(hdr + bytes(body))


# --- the same piece, as a Standard MIDI File --------------------------
#
# first-boot.mid is THIS SCORE, played by whatever SoundFont the machine
# has, so the two files can be compared by ear. It is format 1 with a
# conductor track, and it deliberately uses what a GM file uses: program
# changes, the drum channel, running status, a sustain pedal, pan and
# modulation controllers, a pitch-bend scoop, and a tempo map with a
# ritardando -- the data alone exercises userland/lib/usnd_mid.c.

PPQ = 480


def _vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(out))


def _track(events, running=True):
    """events: (tick, bytes). Stable-sorted, so same-tick order holds."""
    out, last, status = b"", 0, None
    for tick, data in sorted(events, key=lambda e: e[0]):
        body = data
        if running and data[0] < 0xF0 and data[0] == status:
            body = data[1:]             # running status
        # A meta event cancels running status (SMF 1.0), so a writer
        # must restate it -- even though usnd_mid.c would not need it.
        status = data[0] if data[0] < 0xF0 else None
        out += _vlq(tick - last) + body
        last = tick
    out += _vlq(0) + b"\xff\x2f\x00"
    return b"MTrk" + struct.pack(">I", len(out)) + out


def score_midi():
    T = lambda bars, beats=0.0: int(round((bars * 4 + beats) * PPQ))
    on = lambda ch, n, v: bytes([0x90 | ch, n, v])
    # Note-off as a zero-velocity note-on, which is what lets a track
    # run on one status byte.
    off = lambda ch, n: bytes([0x90 | ch, n, 0])
    cc = lambda ch, c, v: bytes([0xB0 | ch, c, v])

    tempo = []
    def set_bpm(tick, bpm):
        tempo.append((tick, b"\xff\x51\x03" + int(60e6 / bpm).to_bytes(3, "big")))
    title = b"First Boot"
    tempo.append((0, b"\xff\x03" + bytes([len(title)]) + title))
    tempo.append((0, b"\xff\x58\x04\x04\x02\x18\x08"))
    set_bpm(0, BPM)
    for i, bpm in enumerate((96, 92, 86, 78, 70)):  # the last two bars slow
        set_bpm(T(30, i * 1.5), bpm)

    PAD, BASS, ARP, LEAD, DRUMS = 0, 1, 2, 3, 9
    ev = {k: [] for k in (PAD, BASS, ARP, LEAD, DRUMS)}
    ev[PAD] += [(0, bytes([0xC0 | PAD, 89])), (0, cc(PAD, 7, 88)), (0, cc(PAD, 10, 64))]
    ev[BASS] += [(0, bytes([0xC0 | BASS, 38])), (0, cc(BASS, 7, 96))]
    ev[ARP] += [(0, bytes([0xC0 | ARP, 46])), (0, cc(ARP, 7, 80))]
    ev[LEAD] += [(0, bytes([0xC0 | LEAD, 73])), (0, cc(LEAD, 7, 104)),
                 (0, cc(LEAD, 10, 72))]
    ev[DRUMS] += [(0, cc(DRUMS, 7, 100))]

    for bar in range(BARS):
        root, notes = CHORDS[bar % 4]
        t0 = T(bar)
        # The pad is re-struck each bar under a held pedal, lifted just
        # before the next chord so the change is clean.
        ev[PAD].append((t0, cc(PAD, 64, 127)))
        for m in notes:
            ev[PAD].append((t0, on(PAD, m, 70)))
            ev[PAD].append((t0 + PPQ, off(PAD, m)))
        ev[PAD].append((t0 + T(1) - 12, cc(PAD, 64, 0)))

        if 4 <= bar < 28:
            ev[BASS].append((t0, on(BASS, root, 100)))
            ev[BASS].append((t0 + T(1) - 30, off(BASS, root)))
            for b in range(4):
                ev[DRUMS].append((t0 + T(0, b), on(DRUMS, 36, 112 if b % 2 == 0 else 80)))
                ev[DRUMS].append((t0 + T(0, b) + 60, off(DRUMS, 36)))
            for b in range(8):
                ev[DRUMS].append((t0 + T(0, b * 0.5), on(DRUMS, 42, 70 if b % 2 else 90)))
                ev[DRUMS].append((t0 + T(0, b * 0.5) + 40, off(DRUMS, 42)))
                m = notes[b % 3] + (12 if b >= 4 else 0)
                ev[ARP].append((t0 + T(0, b * 0.5), cc(ARP, 10, 24 if b % 2 else 104)))
                ev[ARP].append((t0 + T(0, b * 0.5), on(ARP, m, 84)))
                ev[ARP].append((t0 + T(0, b * 0.5 + 0.45), off(ARP, m)))
        if 12 <= bar < 28:
            for b in (1, 3):
                ev[DRUMS].append((t0 + T(0, b), on(DRUMS, 38, 100)))
                ev[DRUMS].append((t0 + T(0, b) + 60, off(DRUMS, 38)))
            if bar == 20:
                ev[DRUMS].append((t0, on(DRUMS, 49, 110)))
                ev[DRUMS].append((t0 + 60, off(DRUMS, 49)))
            for mb, beat, length, m in MELODY:
                if mb != bar % 8:
                    continue
                note = m + (12 if bar >= 20 else 0)
                at = t0 + T(0, beat)
                if mb == 0 and beat == 0.0:
                    # A scoop: start a whole tone flat and bend up.
                    ev[LEAD].append((at, bytes([0xE0 | LEAD, 0x00, 0x00])))
                    for k in range(1, 9):
                        v = k * 8192 // 8
                        v = min(v, 8192)
                        ev[LEAD].append((at + k * 12, bytes([0xE0 | LEAD, v & 0x7F, v >> 7])))
                ev[LEAD].append((at, on(LEAD, note, 96)))
                ev[LEAD].append((at + T(0, length) - 20, off(LEAD, note)))
                if length >= 2:
                    ev[LEAD].append((at + T(0, 1), cc(LEAD, 1, 64)))
                    ev[LEAD].append((at + T(0, length) - 20, cc(LEAD, 1, 0)))

    tracks = [_track(tempo)] + [_track(ev[k]) for k in (PAD, BASS, ARP, LEAD, DRUMS)]
    return b"MThd" + struct.pack(">IHHH", 6, 1, len(tracks), PPQ) + b"".join(tracks)


def encode(wav, mp3, args):
    cmd = ["lame", "--quiet"] + args + [wav, mp3]
    subprocess.run(cmd, check=True)
    size = os.path.getsize(mp3)
    print(f"  {os.path.relpath(mp3, REPO)}  {size} bytes  "
          f"({' '.join(args)})")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out-music",
                    default=os.path.join(REPO, "data/usr/share/music"))
    ap.add_argument("--out-tests", default=os.path.join(REPO, "data/tests"))
    ap.add_argument("--midi-only", action="store_true",
                    help="write first-boot.mid and skip the MP3 encodes")
    args = ap.parse_args()

    os.makedirs(args.out_music, exist_ok=True)
    os.makedirs(args.out_tests, exist_ok=True)

    mid = os.path.join(args.out_music, "first-boot.mid")
    with open(mid, "wb") as f:
        f.write(score_midi())
    print(f"  {os.path.relpath(mid, REPO)}  {os.path.getsize(mid)} bytes")
    if args.midi_only:
        return 0

    if not shutil.which("lame"):
        print("gen_music: no `lame` on PATH -- cannot encode", file=sys.stderr)
        return 1
    tmp = tempfile.mkdtemp(prefix="genmusic.")

    print("music:")
    L, R, g = render()
    src = os.path.join(tmp, "first-boot.wav")
    write_wav(src, (L, R), g)
    # -V4 is VBR, so the file gets a Xing header; -m j is joint stereo,
    # so most frames are mid/side; the tag arguments are what make lame
    # write ID3v2 at the front.
    encode(src, os.path.join(args.out_music, "first-boot.mp3"),
           ["-V", "4", "-m", "j",
            "--tt", "First Boot", "--ta", "toy-os", "--tl", "toy-os",
            "--tc", "Generated by tools/gen_music.py"])

    print("fixtures:")
    # The measured file: one steady frequency for its whole length, mono,
    # CBR. tools/audio_test.py counts its zero crossings in QEMU's
    # recording, which is an oracle sharing no code with the decoder.
    n = int(1.5 * RATE)
    mono = array.array("d", [0.0]) * n
    for i in range(n):
        mono[i] = 0.5 * math.sin(2 * math.pi * 1000.0 * i / RATE)
    src = os.path.join(tmp, "sine1k.wav")
    write_wav(src, (mono,), 1.0)
    encode(src, os.path.join(args.out_tests, "sine1k.mp3"),
           ["-b", "128", "-m", "m", "--cbr", "-t", "--noreplaygain"])

    if shutil.which("flac"):
        print("flac:")
        excerpt = int(20 * RATE)
        src = os.path.join(tmp, "first-boot-24.wav")
        write_wav24(src, (L[:excerpt], R[:excerpt]), g)
        cover = os.path.join(REPO, "data", "wallpapers", "dusk.jpg")
        flac_encode(src, os.path.join(args.out_music, "first-boot.flac"),
                    ["-8", "-T", "TITLE=First Boot (excerpt)", "-T", "ARTIST=toy-os",
                     "-T", "ALBUM=toy-os", "-T", "COMMENT=Generated by tools/gen_music.py",
                     "--picture=3|image/jpeg|||" + cover])
        src = os.path.join(tmp, "sine1k-16.wav")
        write_wav(src, (mono,), 1.0)
        flac_encode(src, os.path.join(args.out_tests, "sine1k.flac"), ["-5"])
        raw = os.path.join(tmp, "ramp24.raw")
        with open(raw, "wb") as f:
            f.write(bytes(b for i in range(RAMP24_FRAMES) for v in ramp24(i)
                          for b in (v & 0xFFFFFF).to_bytes(3, "little")))
        flac_encode(raw, os.path.join(args.out_tests, "ramp24.flac"),
                    ["-5", "--force-raw-format", "--endian=little", "--sign=signed",
                     "--channels=2", "--bps=24", "--sample-rate=48000"])
    else:
        print("gen_music: no `flac` on PATH -- the FLAC files are left as they are")

    shutil.rmtree(tmp, ignore_errors=True)
    return 0


# ramp24.flac's samples. userland/tests/usnd_test.c computes the same
# numbers -- change both together.
RAMP24_FRAMES = 4800


def ramp24(i):
    left = ((i * 7919) % 0x1000000) - 0x800000
    return left, -left - 1


def write_wav24(path, chans, gain, rate=RATE):
    n = len(chans[0])
    body = bytearray()
    for i in range(n):
        for c in chans:
            v = int(max(-8388608, min(8388607, c[i] * gain * 8388607)))
            body += (v & 0xFFFFFF).to_bytes(3, "little")
    block = len(chans) * 3
    hdr = b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, len(chans), rate,
                                 rate * block, block, 24)
    hdr += b"data" + struct.pack("<I", len(body))
    with open(path, "wb") as f:
        f.write(hdr + bytes(body))


def flac_encode(src, out, args):
    subprocess.run(["flac", "-s", "-f"] + args + ["-o", out, src], check=True)
    print(f"  {os.path.relpath(out, REPO)}  {os.path.getsize(out)} bytes")


if __name__ == "__main__":
    sys.exit(main())
