#!/usr/bin/env python3
"""Check the MIDI codec and SoundFont synth on the HOST, against FluidSynth.

The guest test (`/tests/midi_test`) proves the sequencer and the synth do
what its byte-built fixtures say -- a sine bank, exact pitches, exact
onsets. What it cannot say is whether a REAL bank and a REAL song come
out right: whether every generator is read the way the SoundFont spec
means it. So this compiles the same .c files with the host gcc and plays
the shipped bank and songs through them AND through FluidSynth, which
shares no code with them.

**THE COMPARISON IS OF SHAPE, NOT SAMPLES.** Two correct SoundFont synths
never agree sample for sample -- interpolation, envelope granularity and
filter design all legitimately differ, and FluidSynth's master gain is
not ours. So it compares what a wrong implementation gets wrong:

  - the LOUDNESS ENVELOPE over time (20 ms RMS windows, correlated) --
    a note that never stops, a release read in the wrong unit, a tempo
    change missed, all move this;
  - the LEVEL RATIO per section, which must be CONSTANT -- an
    attenuation or velocity curve read wrongly makes some instruments
    louder relative to others, and the ratio then wanders;
  - the ZERO-CROSSING RATE of each instrument in a GM tour -- a proxy
    for pitch and brightness that a wrong root key, tuning or filter
    moves by a semitone's worth or more.

It also FUZZES both parsers under AddressSanitizer: mutated songs and
mutated banks, opened and played. A parser of untrusted input that has
never been fed garbage has not been tested.

  python3 tools/midi_hostcheck.py                   # oracle + fuzz
  python3 tools/midi_hostcheck.py --fuzz 0          # oracle only
  python3 tools/midi_hostcheck.py --sf2 X.sf2       # another bank
  python3 tools/midi_hostcheck.py --render song.mid out.wav   # just listen

Needs gcc and fluidsynth (the oracle is skipped, loudly, without it).
"""
import argparse
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
import array

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
BANK = os.path.join(REPO, "data/usr/share/soundfonts/toy-gm.sf2")
SONG = os.path.join(REPO, "data/usr/share/music/first-boot.mid")
SOURCES = ["usnd.c", "usnd_wav.c", "usnd_mp3.c", "usnd_mid.c", "usnd_sf2.c", "usnd_synth.c"]

# The same shim tools/usnd_hostcheck.py uses: rt/sys.h's names over POSIX.
SHIM_SYS_H = r"""
#ifndef HOST_SYS_H
#define HOST_SYS_H
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <unistd.h>
#define SYS_SEEK_SET 0
#define SYS_SEEK_CUR 1
#define SYS_SEEK_END 2
static inline int64_t sys_read(int fd, void *b, size_t n) { return read(fd, b, n); }
static inline int sys_open(const char *p, int f) { (void)f; return open(p, O_RDONLY); }
static inline int sys_close(int fd) { return close(fd); }
static inline long long sys_lseek(int fd, long long o, int w) { return lseek(fd, o, w); }
#endif
"""

MAIN_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lib/usnd.h"
#include "lib/usnd_internal.h"

// render <song> <out.wav or -> <sf2> [max seconds]
// Exit 0 played, 3 refused (a clean refusal is a PASS under fuzzing).
int main(int argc, char **argv) {
    if (argc < 4) return 2;
    usnd_mid_set_soundfont(argv[3]);
    long max = argc > 4 ? atol(argv[4]) * USND_RATE : -1;
    struct usnd_stream s;
    int rc = usnd_open(argv[1], &s);
    if (rc) { fprintf(stderr, "refused: %s (%d)\n", usnd_last_error(), rc); return 3; }
    FILE *f = strcmp(argv[2], "-") ? fopen(argv[2], "wb") : 0;
    static const unsigned char hdr[44] = {
        'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,
        0x80,0xBB,0,0,0x00,0xEE,0x02,0,4,0,16,0,'d','a','t','a',0,0,0,0 };
    if (f) fwrite(hdr, 1, 44, f);
    static int16_t buf[4096 * 2];
    long n, tot = 0;
    while ((max < 0 || tot < max) && (n = usnd_read(&s, buf, 4096)) > 0) {
        if (f) fwrite(buf, 4, (size_t)n, f);
        tot += n;
    }
    if (f) {
        uint32_t d = (uint32_t)tot * 4, r = d + 36;
        fseek(f, 4, SEEK_SET); fwrite(&r, 4, 1, f);
        fseek(f, 40, SEEK_SET); fwrite(&d, 4, 1, f);
        fclose(f);
    }
    fprintf(stderr, "%s: %ld frames\n", s.detail, tot);
    usnd_close(&s);
    return 0;
}
"""


def build(tmp, sanitize):
    os.makedirs(os.path.join(tmp, "rt"), exist_ok=True)
    with open(os.path.join(tmp, "rt", "sys.h"), "w") as f:
        f.write(SHIM_SYS_H)
    main = os.path.join(tmp, "main.c")
    with open(main, "w") as f:
        f.write(MAIN_C)
    exe = os.path.join(tmp, "midi_asan" if sanitize else "midi_render")
    cmd = ["gcc", "-O1" if sanitize else "-O2", "-g", "-w", "-I" + tmp,
           "-I" + os.path.join(REPO, "userland"), "-o", exe, main]
    cmd += [os.path.join(REPO, "userland", "lib", s) for s in SOURCES]
    if sanitize:
        cmd += ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]
    cmd += ["-lm"]
    subprocess.run(cmd, check=True)
    return exe


# --- a GM tour: one instrument family a second, then the kit ------------

def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(out))


def track(evs):
    out, t = b"", 0
    for tick, data in sorted(evs, key=lambda e: e[0]):
        out += vlq(tick - t) + data
        t = tick
    out += b"\x00\xff\x2f\x00"
    return b"MTrk" + struct.pack(">I", len(out)) + out


TOUR = [0, 4, 6, 11, 16, 19, 24, 29, 32, 38, 40, 48, 52, 56, 61, 65,
        71, 73, 80, 81, 88, 95, 104, 114]


def tour_midi():
    D = 480
    evs, t = [], 0
    for i, prog in enumerate(TOUR):
        ch = i % 9
        # All Sound Off for the previous instrument, so each measured
        # window holds ONE instrument rather than a long pad's tail under
        # the next attack. Releases in context are first-boot.mid's job.
        if i:
            evs.append((t, bytes([0xB0 | ((i - 1) % 9), 120, 0])))
        evs.append((t, bytes([0xC0 | ch, prog])))
        for k, n in enumerate((60, 64, 67, 72)):
            evs.append((t + k * D // 4, bytes([0x90 | ch, n, 100])))
            evs.append((t + k * D // 4 + D // 4 - 10, bytes([0x80 | ch, n, 0])))
        evs.append((t + D, bytes([0x90 | ch, 48, 100])))
        evs.append((t + 2 * D - 20, bytes([0x80 | ch, 48, 0])))
        t += 2 * D
    drums = []
    for b in range(16):
        tk = t + b * D // 2
        hat = 46 if b % 4 == 0 else 42
        drums += [(tk, bytes([0x99, hat, 80])), (tk + 60, bytes([0x89, hat, 0]))]
        if b % 4 == 0:
            drums += [(tk, bytes([0x99, 36, 110])), (tk + 60, bytes([0x89, 36, 0]))]
        if b % 4 == 2:
            drums += [(tk, bytes([0x99, 38, 100])), (tk + 60, bytes([0x89, 38, 0]))]
    drums += [(t + 8 * D, bytes([0x99, 49, 110])), (t + 8 * D + 60, bytes([0x89, 49, 0]))]
    tracks = [track([(0, b"\xff\x51\x03" + (500000).to_bytes(3, "big"))]),
              track(evs), track(drums)]
    return b"MThd" + struct.pack(">IHHH", 6, 1, len(tracks), D) + b"".join(tracks)


# --- measuring ----------------------------------------------------------

def load_mono(path):
    w = wave.open(path)
    a = array.array("h", w.readframes(w.getnframes()))
    if w.getnchannels() == 2:
        return [(a[i] + a[i + 1]) / 2 for i in range(0, len(a), 2)], w.getframerate()
    return list(a), w.getframerate()


def rms_env(x, rate, win=0.02):
    n = int(rate * win)
    return [math.sqrt(sum(v * v for v in x[i:i + n]) / n) for i in range(0, len(x) - n, n)]


def correlate(a, b):
    m = min(len(a), len(b))
    a, b = a[:m], b[:m]
    ma, mb = sum(a) / m, sum(b) / m
    cov = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    va = math.sqrt(sum((x - ma) ** 2 for x in a))
    vb = math.sqrt(sum((y - mb) ** 2 for y in b))
    return cov / (va * vb) if va and vb else 0.0


def zc_rate(x, rate):
    return sum(1 for i in range(1, len(x)) if (x[i - 1] < 0) != (x[i] < 0)) * rate / len(x)


def pitch(x, rate):
    """Fundamental by normalised autocorrelation, at half rate for speed.
    The FIRST lag within 90% of the best one, so a strong second
    harmonic cannot pull the answer an octave up or down differently in
    the two renders."""
    x = [(x[i] + x[i + 1]) / 2 for i in range(0, len(x) - 1, 2)]
    r = rate / 2
    n = 768
    lo, hi = int(r / 1200), int(r / 60)
    seg = x[:n + hi]
    e0 = sum(v * v for v in seg[:n]) or 1.0
    score = {}
    for lag in range(lo, hi):
        c = sum(seg[i] * seg[i + lag] for i in range(n))
        el = sum(v * v for v in seg[lag:lag + n]) or 1.0
        score[lag] = c / math.sqrt(e0 * el)
    best = max(score.values())
    lag = min(k for k, v in score.items() if v >= 0.9 * best)
    return r / lag


def fluid(bank, song, out):
    subprocess.run(["fluidsynth", "-ni", "-q", "-R", "0", "-C", "0", "-r", "48000",
                    "-g", "1.0", "-F", out, bank, song],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)


def oracle(exe, bank, tmp, failures):
    tour = os.path.join(tmp, "tour.mid")
    with open(tour, "wb") as f:
        f.write(tour_midi())

    for label, song, seg_s in (("first-boot.mid", SONG, 8.0), ("GM tour", tour, 1.0)):
        ours, theirs = os.path.join(tmp, "ours.wav"), os.path.join(tmp, "fluid.wav")
        subprocess.run([exe, song, ours, bank], check=True, stderr=subprocess.DEVNULL)
        fluid(bank, song, theirs)
        a, rate = load_mono(ours)
        b, _ = load_mono(theirs)
        ea, eb = rms_env(a, rate), rms_env(b, rate)
        corr = correlate(ea, eb)
        seg = int(seg_s / 0.02)
        ratios = []
        for i in range(0, min(len(ea), len(eb)) - seg, seg):
            sa, sb = sum(ea[i:i + seg]) / seg, sum(eb[i:i + seg]) / seg
            if sa > 50 and sb > 50:
                ratios.append(20 * math.log10(sa / sb))
        spread = max(ratios) - min(ratios) if ratios else 99
        peak = max(abs(v) for v in a)
        ok = corr >= 0.93 and spread <= 3.5 and 3000 < peak < 32767
        print("  %-15s envelope r=%.3f  level spread %.1f dB  peak %d  %s"
              % (label, corr, spread, peak, "ok" if ok else "FAIL"))
        if not ok:
            failures.append(label)

        if song == tour:
            # A semitone is 6%; half of it is the bar. Brightness (the
            # crossing rate) is looser because filters legitimately differ.
            bad = []
            for i, prog in enumerate(TOUR):
                lo, hi = int((i + 0.05) * rate), int((i + 0.25) * rate)
                pa, pb = pitch(a[lo:hi], rate), pitch(b[lo:hi], rate)
                if abs(math.log2(pa / pb)) > 0.5 / 12:
                    bad.append("program %d: %.1f vs %.1f Hz" % (prog, pa, pb))
                za, zb = zc_rate(a[lo:hi], rate), zc_rate(b[lo:hi], rate)
                if abs(za - zb) > 0.25 * max(za, zb, 1):
                    bad.append("program %d: %d vs %d crossings/s" % (prog, za, zb))
            print("  %-15s per-instrument pitch/brightness: %s"
                  % ("", "ok" if not bad else "FAIL -- " + "; ".join(bad)))
            if bad:
                failures.append("tour pitch")


def fuzz(asan, bank, tmp, count, failures):
    rnd = random.Random(0x5EED)
    song = open(SONG, "rb").read()
    tour = tour_midi()
    bank_bytes = open(bank, "rb").read()
    # The pdta list is at the end of a bank: mutate there, where the
    # indices are, rather than in 1.4 MB of samples that parse as noise.
    pdta_at = bank_bytes.rfind(b"pdta")
    crashes = 0
    for i in range(count):
        target_bank = i % 3 == 2
        data = bytearray(bank_bytes if target_bank else (song if i % 2 else tour))
        lo = pdta_at if target_bank else 0
        for _ in range(rnd.randint(1, 8)):
            pos = rnd.randrange(lo, len(data))
            data[pos] = rnd.choice((0, 0xFF, 0x7F, 0x80, rnd.randrange(256)))
        path = os.path.join(tmp, "fuzz.sf2" if target_bank else "fuzz.mid")
        with open(path, "wb") as f:
            f.write(data)
        args = [asan, SONG, "-", path, "3"] if target_bank else [asan, path, "-", bank, "3"]
        r = subprocess.run(args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        if r.returncode not in (0, 3):
            crashes += 1
            keep = os.path.join(tmp, "crash%d%s" % (i, ".sf2" if target_bank else ".mid"))
            shutil.copy(path, keep)
            print("  fuzz case %d: exit %d, kept %s\n%s" % (i, r.returncode, keep, r.stderr[-1500:]))
    print("  %-15s %d mutated songs and banks under ASan/UBSan: %s"
          % ("fuzz", count, "ok" if not crashes else "%d CRASHED" % crashes))
    if crashes:
        failures.append("fuzz")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--sf2", default=BANK)
    ap.add_argument("--fuzz", type=int, default=300, help="mutations to try (0 = none)")
    ap.add_argument("--render", nargs=2, metavar=("SONG", "OUT_WAV"))
    ap.add_argument("--keep", help="keep the build and renders in DIR")
    args = ap.parse_args()

    if not shutil.which("gcc"):
        sys.exit("midi_hostcheck: needs gcc")
    tmp = args.keep or tempfile.mkdtemp(prefix="midi_hostcheck.")
    os.makedirs(tmp, exist_ok=True)
    failures = []
    try:
        exe = build(tmp, sanitize=False)
        if args.render:
            return subprocess.run([exe, args.render[0], args.render[1], args.sf2]).returncode
        print("midi_hostcheck: bank %s" % os.path.relpath(args.sf2, REPO))
        if shutil.which("fluidsynth"):
            oracle(exe, args.sf2, tmp, failures)
        else:
            print("  oracle          SKIPPED -- no fluidsynth on PATH")
        if args.fuzz:
            fuzz(build(tmp, sanitize=True), args.sf2, tmp, args.fuzz, failures)
    finally:
        if not args.keep and not failures:
            shutil.rmtree(tmp, ignore_errors=True)
    if failures:
        print("midi_hostcheck: FAILED (%s); artifacts in %s" % (", ".join(failures), tmp))
        return 1
    print("midi_hostcheck: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
