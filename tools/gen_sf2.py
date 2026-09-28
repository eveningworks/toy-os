#!/usr/bin/env python3
"""tools/gen_sf2.py -- the built-in General MIDI SoundFont, toy-gm.sf2.

The MIDI codec (userland/lib/usnd_mid.c) needs a bank to play anything,
and every bank worth hearing is somebody else's and far too large to
track: GeneralUser GS is ~30 MB, FluidR3_GM 141 MB. So the repository
carries a SMALL bank it generates itself -- ours, under the repo's own
licence, a clean checkout plays MIDI -- and `make iso EXTRAS=1` fetches a
real one (tools/fetch_soundfont.py) that wins whenever it is installed.
The same arrangement as Windows' GS Wavetable Synth: a modest bank that
is always there.

**HOW THE SAMPLES ARE MADE: AN ATTACK, THEN ONE EXACT CYCLE.** Each
melodic sample is additive synthesis -- harmonic partials, each with its
own decay -- for an ATTACK of T seconds, after which the partial
amplitudes freeze and one period is the loop. That loop is SEAMLESS
because the sample's rate is chosen as `L * f0` for an integer cycle
length L, so every partial is periodic in exactly L samples. The bank's
envelopes then do what the attack cannot (the long decay of a piano,
the slow swell of a pad), which is how a 1990s ROM synth fitted GM into
a few megabytes. Drums are one-shot samples, no loop.

**ONE ROOT PER OCTAVE, AND EACH ROOT IS BAND-LIMITED FOR ITS ZONE.** A
sample played up to six semitones above its root must not carry a
partial past 18 kHz there, so the partial count is set per root; a
single sample stretched across the keyboard would alias in the treble.

    python3 tools/gen_sf2.py [--out PATH]

Deterministic (a fixed seed), so re-running it with no change produces
the same bytes. Nothing in the build runs it; the output is tracked.
"""
import argparse
import math
import os
import random
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEFAULT_OUT = os.path.join(REPO, "data", "usr", "share", "soundfonts", "toy-gm.sf2")

TARGET_RATE = 32000     # melodic samples land near this
DRUM_RATE = 22050
TOP_HZ = 18000          # no partial above this, at the top of a zone
ROOTS = [30, 42, 54, 66, 78, 90, 102]   # F# in each octave; zones are +-6

rng = random.Random(0x70F)


def hz(key):
    return 440.0 * 2 ** ((key - 69) / 12.0)


def tc(seconds):
    """SoundFont timecents."""
    return int(round(1200 * math.log2(max(seconds, 0.001))))


def abs_cents(freq):
    """SoundFont absolute cents (filter cutoff, LFO rates)."""
    return int(round(1200 * math.log2(freq / 8.176)))


# --- melodic timbres ----------------------------------------------------
#
# A timbre is a function (n, f0) -> (amplitude, decay per second) for
# harmonic n, plus an attack length, an optional noise transient, and
# the envelope the bank applies over the result.

def formant(freq, peaks):
    return 1.0 + sum(g / (1.0 + ((freq - f) / bw) ** 2) for f, bw, g in peaks)


def pluck_amp(n, pos, tilt):
    return abs(math.sin(math.pi * n * pos)) / n ** tilt


TIMBRES = {
    "piano": dict(
        partials=lambda n, f: (math.exp(-n * f / 4000) / n ** 1.0,
                               0.8 + 1.2 * n * f / 1000),
        attack=0.30, noise=(0.004, 0.10),
        env=dict(decay=16.0, sustain_cb=960, release=0.35, key_decay=25),
        filt=dict(fc=2600, env_cents=2400, env_decay=2.5),
    ),
    "epiano": dict(
        partials=lambda n, f: {1: (1.0, 0.6), 2: (0.28, 1.5), 3: (0.06, 3.0),
                               7: (0.22, 22.0), 8: (0.14, 25.0),
                               9: (0.08, 28.0)}.get(n, (0.0, 0)),
        attack=0.25,
        env=dict(decay=9.0, sustain_cb=960, release=0.30, key_decay=20),
    ),
    "harpsi": dict(
        partials=lambda n, f: (pluck_amp(n, 0.13, 0.75), 1.0 + 0.35 * n),
        attack=0.20,
        env=dict(decay=6.0, sustain_cb=960, release=0.15, key_decay=20),
    ),
    "mallet": dict(
        partials=lambda n, f: {1: (1.0, 2.5), 4: (0.4, 7.0),
                               10: (0.12, 25.0)}.get(n, (0.0, 0)),
        attack=0.25, noise=(0.003, 0.06),
        env=dict(decay=2.2, sustain_cb=960, release=0.20, key_decay=25),
    ),
    "bell": dict(
        partials=lambda n, f: {1: (1.0, 1.0), 2: (0.45, 1.6), 3: (0.35, 2.2),
                               5: (0.25, 3.5), 8: (0.15, 5.0),
                               11: (0.08, 8.0)}.get(n, (0.0, 0)),
        attack=0.20,
        env=dict(decay=5.0, sustain_cb=960, release=0.40, key_decay=15),
    ),
    "organ": dict(
        partials=lambda n, f: ({1: 1.0, 2: 0.75, 3: 0.55, 4: 0.5, 6: 0.3,
                                8: 0.28}.get(n, 0.0), 0.0),
        attack=0.02, noise=(0.003, 0.05),
        env=dict(attack=0.006, release=0.06),
    ),
    "church": dict(
        partials=lambda n, f: (1.0 / n ** 0.9 * (1.4 if n in (1, 2, 4, 8) else 1.0), 0.0),
        attack=0.02,
        env=dict(attack=0.08, release=0.45),
    ),
    "reed": dict(
        partials=lambda n, f: (formant(n * f, [(1400, 500, 2.0), (2800, 700, 0.8)])
                               / n ** 0.8, 0.0),
        attack=0.02,
        env=dict(attack=0.03, release=0.10),
        vib=(5.0, 0.35, 8),
    ),
    "clarinet": dict(
        partials=lambda n, f: ((1.0 / n) if n % 2 else (0.04 / n), 0.0),
        attack=0.02, noise=(0.03, 0.03),
        env=dict(attack=0.03, release=0.10),
        vib=(5.0, 0.4, 6),
    ),
    "flute": dict(
        partials=lambda n, f: ({1: 1.0, 2: 0.22, 3: 0.08, 4: 0.03}.get(n, 0.0), 0.0),
        attack=0.06, noise=(0.05, 0.10),
        env=dict(attack=0.04, release=0.10),
        vib=(5.2, 0.3, 10),
    ),
    "strings": dict(
        partials=lambda n, f: (formant(n * f, [(500, 250, 1.2), (1600, 600, 0.9),
                                               (3200, 900, 0.5)]) / n ** 1.05, 0.0),
        attack=0.02,
        env=dict(attack=0.09, release=0.30),
        vib=(5.5, 0.25, 12),
    ),
    "choir": dict(
        partials=lambda n, f: (formant(n * f, [(800, 90, 6.0), (1150, 100, 3.5),
                                               (2900, 150, 1.5)]) / n ** 1.4, 0.0),
        attack=0.02,
        env=dict(attack=0.18, release=0.40),
        vib=(5.0, 0.3, 14),
    ),
    "brass": dict(
        partials=lambda n, f: (1.0 / n ** 0.85, 0.0),
        attack=0.02,
        env=dict(attack=0.025, release=0.12),
        filt=dict(fc=900, env_cents=3600, env_attack=0.05, env_decay=0.5,
                  env_sustain=0.45),
        vib=(5.2, 0.45, 10),
    ),
    "pluck": dict(
        partials=lambda n, f: (pluck_amp(n, 0.2, 1.0),
                               1.2 + 0.3 * n * f / 100),
        attack=0.25, noise=(0.003, 0.04),
        env=dict(decay=5.5, sustain_cb=960, release=0.20, key_decay=20),
    ),
    "dist": dict(
        partials=lambda n, f: ((1.3 if n % 2 else 0.8) / n ** 0.55, 0.0),
        attack=0.02,
        env=dict(attack=0.005, decay=4.0, sustain_cb=160, release=0.12),
        filt=dict(fc=3200),
    ),
    "bass": dict(
        partials=lambda n, f: (pluck_amp(n, 0.25, 1.35), 1.0 + 0.5 * n * f / 100),
        attack=0.25, noise=(0.003, 0.04),
        env=dict(decay=4.5, sustain_cb=960, release=0.10, key_decay=10),
        roots=ROOTS[:4],
    ),
    "saw": dict(
        partials=lambda n, f: (1.0 / n, 0.0),
        attack=0.02,
        env=dict(attack=0.005, release=0.12),
        filt=dict(fc=4500),
    ),
    "square": dict(
        partials=lambda n, f: ((1.0 / n) if n % 2 else 0.0, 0.0),
        attack=0.02,
        env=dict(attack=0.005, release=0.10),
    ),
    "synthbass": dict(
        partials=lambda n, f: (1.0 / n, 0.0),
        attack=0.02,
        env=dict(attack=0.004, release=0.08),
        filt=dict(fc=500, q=80, env_cents=3600, env_decay=0.35, env_sustain=0.1),
        roots=ROOTS[:5],
    ),
    "pad": dict(
        partials=lambda n, f: (1.0 / n ** 1.5, 0.0),
        attack=0.02,
        env=dict(attack=0.50, release=1.2),
        filt=dict(fc=2500),
        vib=(4.0, 0.6, 8),
    ),
}


def render_melodic(timbre, root):
    """One looped sample: (pcm floats, rate, loop_start, loop_end)."""
    f0 = hz(root)
    L = max(16, int(round(TARGET_RATE / f0)))
    rate = int(round(L * f0))
    top = min(TOP_HZ / (f0 * 2 ** (6 / 12.0)), L / 2 - 1)
    parts = []
    for n in range(1, int(top) + 1):
        a, d = timbre["partials"](n, f0)
        if a > 1e-4:
            parts.append((n, a, d, rng.random() * 2 * math.pi))
    attack_s = timbre["attack"]
    na = int(round(attack_s * rate / L)) * L        # a whole number of cycles
    total = na + L
    sin_tab = [math.sin(2 * math.pi * i / L) for i in range(L)]
    out = [0.0] * total
    for n, a, d, ph in parts:
        off = int(ph / (2 * math.pi) * L)
        frozen = a * math.exp(-d * na / rate)
        k = (off) % L
        step = n % L
        if d == 0:
            for i in range(total):
                out[i] += a * sin_tab[k]
                k += step
                if k >= L:
                    k -= L
        else:
            decay = math.exp(-d / rate)
            g = a
            for i in range(na):
                out[i] += g * sin_tab[k]
                g *= decay
                k += step
                if k >= L:
                    k -= L
            for i in range(na, total):
                out[i] += frozen * sin_tab[k]
                k += step
                if k >= L:
                    k -= L
    if "noise" in timbre:
        dur, amp = timbre["noise"]
        nn = min(int(dur * rate), na)
        lp = 0.0
        peak = max(abs(v) for v in out) or 1.0
        for i in range(nn):
            lp += 0.35 * (rng.uniform(-1, 1) - lp)
            out[i] += lp * amp * peak * (1 - i / nn) ** 2
    return out, rate, na, na + L


# --- drums --------------------------------------------------------------

def env_exp(i, rate, decay):
    return math.exp(-i / (rate * decay))


class Noise:
    def __init__(self):
        self.r = random.Random(0xD2)

    def white(self):
        return self.r.uniform(-1, 1)


def hp(buf, a=0.85):
    """One-pole high pass."""
    out, prev_x, prev_y = [], 0.0, 0.0
    for x in buf:
        y = a * (prev_y + x - prev_x)
        out.append(y)
        prev_x, prev_y = x, y
    return out


def lp(buf, a):
    out, y = [], 0.0
    for x in buf:
        y += a * (x - y)
        out.append(y)
    return out


def metal(n, rate, freqs):
    """The 808's cymbal source: square waves at inharmonic ratios."""
    ph = [0.0] * len(freqs)
    out = []
    for _ in range(n):
        s = 0.0
        for j, f in enumerate(freqs):
            ph[j] += f / rate
            ph[j] -= int(ph[j])
            s += 1.0 if ph[j] < 0.5 else -1.0
        out.append(s / len(freqs))
    return out


def drum(kind, nz):
    R = DRUM_RATE
    if kind in ("kick", "kick2"):
        n = int(R * (0.45 if kind == "kick2" else 0.38))
        f_hi, f_lo = (130, 42) if kind == "kick2" else (160, 50)
        out, ph = [], 0.0
        for i in range(n):
            t = i / R
            f = f_lo + (f_hi - f_lo) * math.exp(-t / 0.035)
            ph += 2 * math.pi * f / R
            out.append(math.sin(ph) * env_exp(i, R, 0.16))
        for i in range(int(R * 0.003)):
            out[i] += nz.white() * 0.5 * (1 - i / (R * 0.003))
        return out
    if kind in ("snare", "snare2"):
        n = int(R * 0.30)
        bright = kind == "snare2"
        noise = hp([nz.white() for _ in range(n)], 0.7 if bright else 0.8)
        out, ph1, ph2 = [], 0.0, 0.0
        for i in range(n):
            ph1 += 2 * math.pi * (190 if bright else 175) / R
            ph2 += 2 * math.pi * 330 / R
            tone = (math.sin(ph1) + 0.5 * math.sin(ph2)) * env_exp(i, R, 0.05)
            out.append(0.8 * tone + noise[i] * 1.4 * env_exp(i, R, 0.09 if bright else 0.11))
        return out
    if kind == "sidestick":
        n = int(R * 0.06)
        return [(math.sin(2 * math.pi * 1650 * i / R) * 0.7 + nz.white() * 0.5)
                * env_exp(i, R, 0.012) for i in range(n)]
    if kind == "clap":
        n = int(R * 0.30)
        src = lp(hp([nz.white() for _ in range(n)], 0.6), 0.5)
        out = []
        for i in range(n):
            t = i / R
            e = 0.0
            for k in range(3):
                if t >= k * 0.011:
                    e = max(e, math.exp(-(t - k * 0.011) / 0.006))
            if t >= 0.033:
                e = max(e, 0.6 * math.exp(-(t - 0.033) / 0.07))
            out.append(src[i] * e * 2.0)
        return out
    if kind == "tom":
        n = int(R * 0.6)
        out, ph = [], 0.0
        for i in range(n):
            t = i / R
            f = 110 * (1 + 0.35 * math.exp(-t / 0.06))
            ph += 2 * math.pi * f / R
            out.append((math.sin(ph) + 0.15 * nz.white() * math.exp(-t / 0.02))
                       * env_exp(i, R, 0.20))
        return out
    if kind == "conga":
        n = int(R * 0.35)
        out, ph = [], 0.0
        for i in range(n):
            t = i / R
            f = 230 * (1 + 0.12 * math.exp(-t / 0.03))
            ph += 2 * math.pi * f / R
            out.append((math.sin(ph) + 0.3 * math.sin(2.3 * ph)
                        + 0.4 * nz.white() * math.exp(-t / 0.006)) * env_exp(i, R, 0.09))
        return out
    if kind in ("hat_closed", "hat_pedal", "hat_open"):
        dur = {"hat_closed": 0.09, "hat_pedal": 0.14, "hat_open": 0.7}[kind]
        dec = {"hat_closed": 0.022, "hat_pedal": 0.04, "hat_open": 0.25}[kind]
        n = int(R * dur)
        m = metal(n, R, [205.3 * 2, 304.4 * 2, 369.6 * 2, 522.7 * 2, 540 * 2, 800 * 2])
        src = hp([m[i] * 0.7 + nz.white() * 0.4 for i in range(n)], 0.5)
        src = hp(src, 0.5)
        return [src[i] * env_exp(i, R, dec) * 1.6 for i in range(n)]
    if kind in ("crash", "ride"):
        n = int(R * (1.2 if kind == "crash" else 1.0))
        m = metal(n, R, [263, 371, 457, 587, 691, 853] if kind == "crash"
                  else [321, 449, 523, 701, 797, 950])
        src = hp([m[i] * (0.5 if kind == "crash" else 0.8) + nz.white() * 0.6
                  for i in range(n)], 0.6)
        dec = 0.38 if kind == "crash" else 0.45
        out = []
        for i in range(n):
            e = env_exp(i, R, dec)
            if kind == "ride":
                e = 0.5 * e + 0.5 * env_exp(i, R, 0.06)
            out.append(src[i] * e)
        return out
    if kind == "ridebell":
        n = int(R * 0.8)
        return [sum(a * math.sin(2 * math.pi * f * i / R) for f, a in
                    ((820, 1.0), (1230, 0.6), (2050, 0.4), (3190, 0.25)))
                * env_exp(i, R, 0.25) for i in range(n)]
    if kind == "tamb":
        n = int(R * 0.30)
        m = metal(n, R, [5000, 6100, 7300, 8100])
        src = hp([m[i] * 0.5 + nz.white() for i in range(n)], 0.4)
        return [src[i] * env_exp(i, R, 0.06) for i in range(n)]
    if kind == "cowbell":
        n = int(R * 0.30)
        src = [(1 if (i * 562 / R) % 1 < 0.5 else -1) + (1 if (i * 845 / R) % 1 < 0.5 else -1)
               for i in range(n)]
        src = lp(hp(src, 0.9), 0.5)
        return [src[i] * 0.6 * (0.6 * env_exp(i, R, 0.02) + 0.4 * env_exp(i, R, 0.10))
                for i in range(n)]
    if kind == "agogo":
        n = int(R * 0.35)
        return [(math.sin(2 * math.pi * 900 * i / R) + 0.4 * math.sin(2 * math.pi * 2430 * i / R))
                * env_exp(i, R, 0.09) for i in range(n)]
    if kind == "shaker":
        n = int(R * 0.09)
        src = hp([nz.white() for _ in range(n)], 0.3)
        return [src[i] * math.sin(math.pi * i / n) for i in range(n)]
    if kind == "whistle":
        n = int(R * 0.35)
        return [math.sin(2 * math.pi * (2400 + 40 * math.sin(2 * math.pi * 30 * i / R)) * i / R)
                * min(1, i / (R * 0.01)) * min(1, (n - i) / (R * 0.03)) for i in range(n)]
    if kind == "guiro":
        n = int(R * 0.30)
        src = hp([nz.white() for _ in range(n)], 0.5)
        return [src[i] * (0.4 + 0.6 * ((i * 38 / R) % 1 < 0.3)) * min(1, (n - i) / (R * 0.05))
                for i in range(n)]
    if kind == "claves":
        n = int(R * 0.06)
        return [math.sin(2 * math.pi * 2500 * i / R) * env_exp(i, R, 0.015) for i in range(n)]
    if kind == "woodblock":
        n = int(R * 0.08)
        return [(math.sin(2 * math.pi * 1100 * i / R) + 0.3 * nz.white() * env_exp(i, R, 0.002))
                * env_exp(i, R, 0.018) for i in range(n)]
    if kind == "cuica":
        n = int(R * 0.22)
        out, ph = [], 0.0
        for i in range(n):
            ph += 2 * math.pi * (380 + 260 * i / n) / R
            out.append(math.sin(ph) * math.sin(math.pi * i / n))
        return out
    if kind == "triangle":
        n = int(R * 1.0)
        return [sum(a * math.sin(2 * math.pi * f * i / R) for f, a in
                    ((4600, 1.0), (6980, 0.5), (8900, 0.3)))
                * env_exp(i, R, 0.35) for i in range(n)]
    raise ValueError(kind)


# key: (sample, semitones above the sample, pan -500..500, exclusive, level dB)
DRUM_MAP = {
    31: ("claves", -5, 0, 0, -6),
    35: ("kick2", 0, 0, 0, 0), 36: ("kick", 0, 0, 0, 0),
    37: ("sidestick", 0, 0, 0, -4), 38: ("snare", 0, 0, 0, 0),
    39: ("clap", 0, -100, 0, -2), 40: ("snare2", 0, 0, 0, 0),
    41: ("tom", -7, -300, 0, 0), 42: ("hat_closed", 0, 250, 1, -5),
    43: ("tom", -4, -200, 0, 0), 44: ("hat_pedal", 0, 250, 1, -6),
    45: ("tom", -1, -100, 0, 0), 46: ("hat_open", 0, 250, 1, -6),
    47: ("tom", 2, 0, 0, 0), 48: ("tom", 5, 100, 0, 0),
    49: ("crash", 0, -250, 0, -4), 50: ("tom", 8, 200, 0, 0),
    51: ("ride", 0, 300, 0, -6), 52: ("crash", -4, -350, 0, -5),
    53: ("ridebell", 0, 300, 0, -6), 54: ("tamb", 0, 150, 0, -6),
    55: ("crash", 5, 250, 0, -6), 56: ("cowbell", 0, 150, 0, -6),
    57: ("crash", 2, 350, 0, -4), 58: ("guiro", -12, -250, 0, -6),
    59: ("ride", 3, 350, 0, -6),
    60: ("conga", 9, 250, 0, -3), 61: ("conga", 5, 250, 0, -3),
    62: ("conga", 2, -250, 0, -3), 63: ("conga", 0, -250, 0, -3),
    64: ("conga", -4, -250, 0, -3),
    65: ("conga", 7, 350, 0, -4), 66: ("conga", 3, 350, 0, -4),
    67: ("agogo", 0, 350, 0, -6), 68: ("agogo", -3, 350, 0, -6),
    69: ("shaker", 0, -350, 0, -8), 70: ("shaker", 4, -350, 0, -7),
    71: ("whistle", 0, 350, 3, -10), 72: ("whistle", -2, 350, 3, -10),
    73: ("guiro", 3, -350, 4, -7), 74: ("guiro", 0, -350, 4, -7),
    75: ("claves", 0, 300, 0, -6),
    76: ("woodblock", 3, 300, 0, -5), 77: ("woodblock", 0, 300, 0, -5),
    78: ("cuica", 3, -300, 5, -7), 79: ("cuica", 0, -300, 5, -7),
    80: ("triangle", 0, 350, 2, -10), 81: ("triangle", 0, 350, 2, -9),
}

# --- the General MIDI programs ------------------------------------------
#
# (name, timbre, preset-level generator OFFSETS). Offsets, because a
# preset zone's generators ADD to its instrument's -- which is how 128
# programs share twenty instruments.

G = dict(COARSE=51, FINE=52, ATT=48, FC=8, ATTACK=34, DECAY=36, RELEASE=38,
         PAN=17, VIB_PITCH=6, MODLFO_VOL=13, MODLFO_FREQ=22, SUSTAIN=37)


def p(name, timbre, layer=0, **gens):
    return (name, timbre, layer, gens)


GM = [
    p("Acoustic Grand Piano", "piano"), p("Bright Acoustic Piano", "piano", FC=900),
    p("Electric Grand Piano", "piano", FC=400, DECAY=-600),
    p("Honky-tonk Piano", "piano", layer=12),
    p("Electric Piano 1", "epiano"), p("Electric Piano 2", "epiano", FC=0, layer=8),
    p("Harpsichord", "harpsi"), p("Clavinet", "harpsi", DECAY=-1400, ATT=20),
    p("Celesta", "bell", COARSE=12, DECAY=-900), p("Glockenspiel", "bell", COARSE=24, DECAY=-600),
    p("Music Box", "bell", COARSE=12, DECAY=-300), p("Vibraphone", "mallet", DECAY=1500,
                                                    MODLFO_VOL=40, MODLFO_FREQ=-300),
    p("Marimba", "mallet"), p("Xylophone", "mallet", COARSE=12, DECAY=-900),
    p("Tubular Bells", "bell", DECAY=900), p("Dulcimer", "harpsi", DECAY=-300),
    p("Drawbar Organ", "organ"), p("Percussive Organ", "organ", ATT=20),
    p("Rock Organ", "organ", MODLFO_VOL=30, MODLFO_FREQ=500), p("Church Organ", "church", layer=6),
    p("Reed Organ", "reed", ATT=30), p("Accordion", "reed", layer=10),
    p("Harmonica", "reed", COARSE=12, ATT=20), p("Tango Accordion", "reed", layer=14),
    p("Acoustic Guitar (nylon)", "pluck"), p("Acoustic Guitar (steel)", "pluck", FC=0),
    p("Electric Guitar (jazz)", "pluck", DECAY=500), p("Electric Guitar (clean)", "pluck", DECAY=800),
    p("Electric Guitar (muted)", "pluck", DECAY=-3000, RELEASE=-1500),
    p("Overdriven Guitar", "dist"), p("Distortion Guitar", "dist", ATT=-20),
    p("Guitar Harmonics", "bell", COARSE=12),
    p("Acoustic Bass", "bass"), p("Electric Bass (finger)", "bass", DECAY=300),
    p("Electric Bass (pick)", "bass", DECAY=200), p("Fretless Bass", "bass", DECAY=900),
    p("Slap Bass 1", "bass", ATT=-20), p("Slap Bass 2", "bass", ATT=-20),
    p("Synth Bass 1", "synthbass"), p("Synth Bass 2", "synthbass", layer=10),
    p("Violin", "strings", COARSE=0), p("Viola", "strings"), p("Cello", "strings"),
    p("Contrabass", "strings"), p("Tremolo Strings", "strings", layer=8, MODLFO_VOL=60,
                                  MODLFO_FREQ=1100),
    p("Pizzicato Strings", "pluck", DECAY=-2400), p("Orchestral Harp", "harpsi", DECAY=900),
    p("Timpani", "mallet", COARSE=-12, DECAY=600),
    p("String Ensemble 1", "strings", layer=8), p("String Ensemble 2", "strings", layer=8, ATTACK=900),
    p("Synth Strings 1", "pad", layer=10, ATTACK=-800), p("Synth Strings 2", "pad", layer=12),
    p("Choir Aahs", "choir", layer=8), p("Voice Oohs", "choir", layer=6, FC=-3500),
    p("Synth Voice", "choir", layer=12), p("Orchestra Hit", "brass", layer=14, DECAY=0),
    p("Trumpet", "brass"), p("Trombone", "brass"), p("Tuba", "brass"),
    p("Muted Trumpet", "brass", ATT=40), p("French Horn", "brass", ATTACK=600, ATT=30),
    p("Brass Section", "brass", layer=10), p("Synth Brass 1", "saw", layer=10),
    p("Synth Brass 2", "saw", layer=14, ATTACK=900),
    p("Soprano Sax", "reed"), p("Alto Sax", "reed"), p("Tenor Sax", "reed"),
    p("Baritone Sax", "reed"), p("Oboe", "reed", ATT=30), p("English Horn", "reed", ATT=30),
    p("Bassoon", "reed", ATT=30), p("Clarinet", "clarinet"),
    p("Piccolo", "flute", COARSE=12), p("Flute", "flute"), p("Recorder", "flute"),
    p("Pan Flute", "flute", layer=6), p("Blown Bottle", "flute"), p("Shakuhachi", "flute"),
    p("Whistle", "flute", COARSE=12), p("Ocarina", "flute"),
    p("Lead 1 (square)", "square"), p("Lead 2 (sawtooth)", "saw"),
    p("Lead 3 (calliope)", "flute", layer=8), p("Lead 4 (chiff)", "flute"),
    p("Lead 5 (charang)", "dist"), p("Lead 6 (voice)", "choir"),
    p("Lead 7 (fifths)", "saw", layer=700), p("Lead 8 (bass + lead)", "saw", layer=1200),
    p("Pad 1 (new age)", "pad", layer=8), p("Pad 2 (warm)", "pad", layer=10),
    p("Pad 3 (polysynth)", "saw", layer=10, ATTACK=800), p("Pad 4 (choir)", "choir", layer=10, ATTACK=800),
    p("Pad 5 (bowed)", "strings", layer=10, ATTACK=1200), p("Pad 6 (metallic)", "bell", layer=8, DECAY=2000),
    p("Pad 7 (halo)", "choir", layer=12, ATTACK=1200), p("Pad 8 (sweep)", "pad", layer=10, ATTACK=1400),
    p("FX 1 (rain)", "bell", COARSE=12), p("FX 2 (soundtrack)", "pad", layer=8),
    p("FX 3 (crystal)", "bell", COARSE=24), p("FX 4 (atmosphere)", "pluck", DECAY=1500),
    p("FX 5 (brightness)", "pad", layer=10), p("FX 6 (goblins)", "choir", COARSE=-12),
    p("FX 7 (echoes)", "epiano", DECAY=1500), p("FX 8 (sci-fi)", "bell", layer=10),
    p("Sitar", "harpsi", DECAY=900), p("Banjo", "pluck", DECAY=-1000),
    p("Shamisen", "pluck", DECAY=-1200), p("Koto", "harpsi"),
    p("Kalimba", "mallet", COARSE=12), p("Bag pipe", "reed", layer=6),
    p("Fiddle", "strings"), p("Shanai", "reed"),
    p("Tinkle Bell", "bell", COARSE=24), p("Agogo", "bell", COARSE=12, DECAY=-2000),
    p("Steel Drums", "mallet", DECAY=600), p("Woodblock", "mallet", COARSE=24, DECAY=-2400),
    p("Taiko Drum", "drum:tom", COARSE=-12), p("Melodic Tom", "drum:tom"),
    p("Synth Drum", "drum:tom", COARSE=12), p("Reverse Cymbal", "drum:crash"),
    p("Guitar Fret Noise", "drum:guiro"), p("Breath Noise", "flute", ATT=100),
    p("Seashore", "drum:crash", COARSE=-24), p("Bird Tweet", "flute", COARSE=24),
    p("Telephone Ring", "bell", COARSE=24), p("Helicopter", "drum:tom", COARSE=-24),
    p("Applause", "drum:clap", COARSE=-5), p("Gunshot", "drum:snare", COARSE=-12),
]
assert len(GM) == 128, len(GM)


# --- the RIFF writer ----------------------------------------------------

def chunk(cid, data):
    pad = b"\0" if len(data) & 1 else b""
    return cid + struct.pack("<I", len(data)) + data + pad


def lst(ltype, *chunks):
    return chunk(b"LIST", ltype + b"".join(chunks))


def zstr(s):
    b = s.encode("ascii") + b"\0"
    return b + (b"\0" if len(b) & 1 else b"")


def name20(s):
    return s.encode("ascii")[:19].ljust(20, b"\0")


class Bank:
    def __init__(self):
        self.samples = []       # (name, pcm ints, rate, loop_s, loop_e, root)
        self.smpl = bytearray()
        self.shdr = []
        self.insts = []         # (name, [zone gens list])
        self.presets = []       # (name, program, bank, [zone gens list])

    def add_sample(self, name, pcm, rate, ls, le, root):
        start = len(self.smpl) // 2
        self.smpl += struct.pack("<%dh" % len(pcm), *pcm)
        self.smpl += b"\0" * (46 * 2)
        self.shdr.append((name, start, start + len(pcm), start + ls, start + le,
                          rate, root))
        return len(self.shdr) - 1

    def add_inst(self, name, zones):
        self.insts.append((name, zones))
        return len(self.insts) - 1

    def add_preset(self, name, program, bank, zones):
        self.presets.append((name, program, bank, zones))

    def build(self):
        def gen_bytes(g):
            op, amt = g
            if isinstance(amt, tuple):
                return struct.pack("<HBB", op, amt[0], amt[1])
            return struct.pack("<Hh", op, amt)

        phdr, pbag, pgen = b"", b"", b""
        nbag = ngen = 0
        for name, prog, bank, zones in self.presets:
            phdr += name20(name) + struct.pack("<HHHIII", prog, bank, nbag, 0, 0, 0)
            for z in zones:
                pbag += struct.pack("<HH", ngen, 0)
                nbag += 1
                for g in z:
                    pgen += gen_bytes(g)
                    ngen += 1
        phdr += name20("EOP") + struct.pack("<HHHIII", 0, 0, nbag, 0, 0, 0)
        pbag += struct.pack("<HH", ngen, 0)
        pgen += struct.pack("<HH", 0, 0)

        inst, ibag, igen = b"", b"", b""
        nbag = ngen = 0
        for name, zones in self.insts:
            inst += name20(name) + struct.pack("<H", nbag)
            for z in zones:
                ibag += struct.pack("<HH", ngen, 0)
                nbag += 1
                for g in z:
                    igen += gen_bytes(g)
                    ngen += 1
        inst += name20("EOI") + struct.pack("<H", nbag)
        ibag += struct.pack("<HH", ngen, 0)
        igen += struct.pack("<HH", 0, 0)

        shdr = b""
        for name, s, e, ls, le, rate, root in self.shdr:
            shdr += name20(name) + struct.pack("<IIIIIBbHH", s, e, ls, le, rate, root, 0, 0, 1)
        shdr += name20("EOS") + struct.pack("<IIIIIBbHH", 0, 0, 0, 0, 0, 0, 0, 0, 0)
        mod_term = b"\0" * 10

        info = lst(b"INFO",
                   chunk(b"ifil", struct.pack("<HH", 2, 1)),
                   chunk(b"isng", zstr("EMU8000")),
                   chunk(b"INAM", zstr("toy-os GM")),
                   chunk(b"ICOP", zstr("toy-os contributors, MIT licence")),
                   chunk(b"ICMT", zstr("Generated by tools/gen_sf2.py")),
                   chunk(b"ISFT", zstr("gen_sf2.py")))
        sdta = lst(b"sdta", chunk(b"smpl", bytes(self.smpl)))
        pdta = lst(b"pdta", chunk(b"phdr", phdr), chunk(b"pbag", pbag),
                   chunk(b"pmod", mod_term), chunk(b"pgen", pgen),
                   chunk(b"inst", inst), chunk(b"ibag", ibag),
                   chunk(b"imod", mod_term), chunk(b"igen", igen),
                   chunk(b"shdr", shdr))
        return chunk(b"RIFF", b"sfbk" + info + sdta + pdta)


def to_pcm(buf, peak=0.9):
    m = max(abs(v) for v in buf) or 1.0
    return [int(round(v / m * peak * 32767)) for v in buf]


def rms(pcm, a, b):
    seg = pcm[a:b] or [0]
    return math.sqrt(sum(v * v for v in seg) / len(seg)) / 32767


def inst_env_gens(t):
    env, out = t["env"], []
    if "attack" in env:
        out.append((34, tc(env["attack"])))
    if "decay" in env:
        out.append((36, tc(env["decay"])))
        out.append((37, env.get("sustain_cb", 0)))
    if "key_decay" in env:
        out.append((40, env["key_decay"]))
    out.append((38, tc(env["release"])))
    f = t.get("filt")
    if f:
        out.append((8, abs_cents(f["fc"])))
        if "q" in f:
            out.append((9, f["q"]))
        if "env_cents" in f:
            out.append((11, f["env_cents"]))
            out.append((26, tc(f.get("env_attack", 0.001))))
            out.append((28, tc(f["env_decay"])))
            out.append((29, int(1000 * (1 - f.get("env_sustain", 0.0)))))
            out.append((30, tc(env["release"])))
    v = t.get("vib")
    if v:
        rate_hz, delay, depth = v
        out.append((24, abs_cents(rate_hz)))
        out.append((23, tc(delay)))
        out.append((6, depth))
    return out


def build_bank(verbose):
    bank = Bank()
    inst_index = {}

    for tname, t in TIMBRES.items():
        roots = t.get("roots", ROOTS)
        zones = []
        levels = []
        for i, root in enumerate(roots):
            buf, rate, ls, le = render_melodic(t, root)
            pcm = to_pcm(buf)
            sid = bank.add_sample("%s%d" % (tname[:14], root), pcm, rate, ls, le, root)
            lo = 0 if i == 0 else root - 6
            hi = 127 if i == len(roots) - 1 else root + 5
            # Loudness: the loop's RMS, so a dense organ and a thin flute
            # sit near each other once the bank is played.
            levels.append(rms(pcm, ls, le))
            zones.append([(43, (lo, hi)), (54, 1), (53, sid)])
        level = sum(levels) / len(levels)
        # Divided by 0.4 because every synth that matters reads the
        # attenuation generator as 0.4 dB a unit (usnd_synth.c's
        # ATTEN_SCALE, FluidSynth's ALT_ATTENUATION_SCALE).
        att = max(0, int(round(200 * math.log10(max(level, 1e-6) / 0.22) / 0.4)))
        glob = [(48, att)] + inst_env_gens(t)
        inst_index[tname] = bank.add_inst(tname, [glob] + zones)
        if verbose:
            print("  %-10s %d roots, attenuation %d cB" % (tname, len(roots), att),
                  file=sys.stderr)

    nz = Noise()
    drum_samples = {}
    for key, (kind, semis, pan, excl, db) in sorted(DRUM_MAP.items()):
        if kind not in drum_samples:
            pcm = to_pcm(drum(kind, nz))
            drum_samples[kind] = bank.add_sample(kind[:19], pcm, DRUM_RATE, 0, 0, 60)
    drum_zones = [[(38, tc(1.5))]]      # global: a key-up never cuts a drum short
    for key, (kind, semis, pan, excl, db) in sorted(DRUM_MAP.items()):
        z = [(43, (key, key)), (58, key - semis), (17, pan), (48, int(-db * 10 / 0.4))]
        if excl:
            z.append((57, excl))
        z.append((53, drum_samples[kind]))
        drum_zones.append(z)
    drum_inst = bank.add_inst("standard kit", drum_zones)

    # Melodic presets may borrow a drum sound, played pitched from 60.
    for kind in ("tom", "crash", "guiro", "clap", "snare"):
        inst_index["drum:" + kind] = bank.add_inst(
            "melodic " + kind, [[(38, tc(1.5)), (58, 60), (53, drum_samples[kind])]])

    for prog, (name, tname, layer, gens) in enumerate(GM):
        base = [(G[k], v) for k, v in gens.items()]
        ii = inst_index[tname]
        if layer:
            # Two detuned copies, spread: the chorus a real ensemble has.
            # `layer` is the spread in cents (a fifth or octave above
            # 100 cents is a second voice, for the two-oscillator leads).
            if layer >= 100:
                zones = [base + [(41, ii)], base + [(51, layer // 100), (48, 30), (41, ii)]]
            else:
                zones = [base + [(52, -layer // 2), (17, -300), (41, ii)],
                         base + [(52, layer - layer // 2), (17, 300), (41, ii)]]
        else:
            zones = [base + [(41, ii)]]
        bank.add_preset(name, prog, 0, zones)
    bank.add_preset("Standard Kit", 0, 128, [[(41, drum_inst)]])
    return bank.build()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args()
    data = build_bank(not args.quiet)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(data)
    print("wrote %s (%d bytes)" % (args.out, len(data)), file=sys.stderr)


if __name__ == "__main__":
    main()
