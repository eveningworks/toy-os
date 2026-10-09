#!/usr/bin/env python3
"""Check userland/lib/usnd_mp3.c against ffmpeg and usnd_flac.c against flac, on the HOST.

WHY A HOST HARNESS EXISTS BESIDE THE GUEST TEST. The same split
tools/uimg_hostcheck.py makes for JPEG, for the same reason:
`/tests/usnd_test` runs the decoder where it actually lives and proves it
works in toy-os, but a guest test can only carry the vectors somebody
committed -- and an MP3 decoder's bugs hide in the COMBINATIONS (this
bitrate at that mode with those block types), which is a sweep, not three
files. This compiles the SAME .c with the host gcc and runs it against as
many lame-encoded files as asked for, comparing every sample against
ffmpeg's decode.

**THE COMPARISON IS NOT EXACT, AND THAT IS THE STANDARD'S OWN POSITION.**
ISO/IEC 11172-4 defines a compliant decoder by ERROR BOUND, not by
bit-equality: implementations differ in IMDCT and filterbank rounding and
are all correct. So this reports RMS error relative to full scale and
fails on a threshold, the way uimg_hostcheck tolerates IDCT differences.
A real bug is not subtle here -- a wrong Huffman table or a misplaced
region boundary produces garbage, not a slightly different waveform.

**FLAC IS THE OPPOSITE CASE: EXACT OR WRONG.** A lossless decoder has no
error bound to hide in, so every FLAC sample must equal what `flac -d`
(Xiph's reference decoder) produces, after this codec's s32 output is
shifted back down to the file's own depth. The sweep is shaped by what
the format can do rather than by what is common: 8 to 32 bits, mono and
stereo, compression levels 0-8 (fixed predictors, LPC, every stereo
decorrelation), exhaustive model search, block sizes from 192 to 32768,
files written by ffmpeg's encoder as well as Xiph's, wasted bits, a
silent stretch (CONSTANT subframes) and a noise burst (VERBATIM). Then
SEEKING, with and without a seek table: each landing must equal the
same stretch of a straight decode.

  python3 tools/usnd_hostcheck.py                  # the standard sweep
  python3 tools/usnd_hostcheck.py --file x.mp3     # one real MP3
  python3 tools/usnd_hostcheck.py --keep DIR       # keep the artifacts

Exit status is non-zero if any MP3 exceeds --tolerance or any FLAC
sample differs. Needs gcc, lame, ffmpeg and flac.
"""
import argparse
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# A ring-3 program reaches the filesystem through rt/sys.h, which does not
# exist on the host. The shim is POSIX underneath the same names -- it is
# the ONLY thing that differs between this build and the real one, which
# is what makes the comparison meaningful.
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
#include "rt/sys.h"

extern const struct usnd_codec usnd_codec_mp3;
int usnd_mp3_selftest(void);

static const char *g_err = "";
const char *usnd_last_error(void) { return g_err; }
void usnd_fail(const char *m) { g_err = m; }

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) {
        int rc = usnd_mp3_selftest();
        fprintf(stderr, "selftest %d\n", rc);
        return rc != 0;
    }
    if (argc < 2) return 2;

    struct usnd_stream s;
    memset(&s, 0, sizeof s);
    s.fd = sys_open(argv[1], 0);
    if (s.fd < 0) { fprintf(stderr, "open failed\n"); return 2; }
    s.codec = &usnd_codec_mp3;
    int rc = usnd_codec_mp3.open(&s);
    if (rc != 0) { fprintf(stderr, "open: %s (%d)\n", usnd_last_error(), rc); return 3; }
    fprintf(stderr, "rate=%u ch=%u detail=%s\n", s.fmt.rate, s.fmt.channels, s.detail);

    int32_t buf[4096 * 2];
    for (;;) {
        long n = usnd_codec_mp3.read(&s, buf, 4096);
        if (n <= 0) break;
        fwrite(buf, sizeof(int32_t), (size_t)n * s.fmt.channels, stdout);
    }
    usnd_codec_mp3.close(&s);
    return 0;
}
"""

# (label, lame arguments). Chosen so the set covers what actually breaks a
# decoder: both stereo modes, mono, CBR and VBR, the bitrate extremes, and
# -- via the transient source below -- short blocks.
CASES = [
    ("cbr128-joint", ["-b", "128", "-m", "j"]),
    ("cbr128-stereo", ["-b", "128", "-m", "s"]),
    ("cbr320-stereo", ["-b", "320", "-m", "s"]),
    ("cbr32-mono", ["-b", "32", "-m", "m", "--resample", "44.1"]),
    ("cbr64-mono", ["-b", "64", "-m", "m", "--resample", "44.1"]),
    ("vbr0-joint", ["-V", "0", "-m", "j"]),
    ("vbr6-joint", ["-V", "6", "-m", "j"]),
    ("vbr4-stereo", ["-V", "4", "-m", "s"]),
]


def write_wav(path, chans, rate=44100):
    n = len(chans[0])
    body = bytearray()
    for i in range(n):
        for c in chans:
            body += struct.pack("<h", max(-32768, min(32767, int(c[i]))))
    block = len(chans) * 2
    hdr = b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, len(chans), rate,
                                 rate * block, block, 16)
    hdr += b"data" + struct.pack("<I", len(body))
    open(path, "wb").write(hdr + bytes(body))


def source(rate=44100, secs=3.0):
    """Two channels of deliberately awkward content.

    A pure tone exercises long blocks and nothing else. The CLICKS are the
    point: a transient is what makes an encoder switch to short blocks,
    and the short-block path (three IMDCTs, subblock gains, the reorder)
    is the half a tone-only fixture would leave completely untested.
    """
    n = int(rate * secs)
    seed = 0x12345678
    L, R = [], []
    for i in range(n):
        t = i / rate
        seed = (seed * 1103515245 + 12345) & 0xFFFFFFFF
        noise = (((seed >> 16) & 0xFFFF) / 32768.0 - 1.0)
        v = 0.32 * math.sin(2 * math.pi * 440 * t)
        v += 0.18 * math.sin(2 * math.pi * 1731 * t)
        v += 0.06 * noise
        # A click every 250 ms, hard enough to force a window switch.
        if i % (rate // 4) < 24:
            v += 0.55 * (1.0 - (i % (rate // 4)) / 24.0)
        L.append(v * 32767 * 0.8)
        # The right channel differs, or mid/side would be indistinguishable
        # from a working decoder that ignored side entirely.
        R.append((0.7 * v + 0.25 * math.sin(2 * math.pi * 997 * t)) * 32767 * 0.8)
    return L, R


def decode_ffmpeg(path, channels, rate):
    """s32, like the decoder's own output -- the comparison keeps every bit
    the float synthesis produces rather than ffmpeg's rounding to 16."""
    out = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-f", "s32le",
         "-ac", str(channels), "-ar", str(rate), "-"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True).stdout
    return struct.unpack("<%di" % (len(out) // 4), out)


def compare(ours, theirs, channels):
    """RMS error relative to full scale, after aligning on the best lag.

    **THE LAG IS NOT ALWAYS A WHOLE FRAME.** A file carrying a LAME/Xing
    gapless tag makes ffmpeg drop the encoder delay (typically 1105
    samples, which is 529 of decoder delay plus 576 of encoder padding),
    and that is not a multiple of 1152. A frame-stepped search cannot land
    on it, and reports a correct decoder as badly wrong -- which is what
    it did to the shipped VBR file before this searched every sample.

    Coarse-to-fine: pick the lag on a short window, then measure the full
    RMS there. A lag that is NOT ~0 or ~1105 is itself a finding.
    """
    step = max(1, channels)
    win = 4000
    probe = min(len(theirs) // 2, 200000)
    best_lag, best_err = 0, None
    for lag in range(0, 2400):
        if (lag + probe // step) * step + win >= len(ours):
            break
        err = 0.0
        for i in range(0, win, step):
            d = ours[(lag * step) + probe + i] - theirs[probe + i]
            err += d * d
        if best_err is None or err < best_err:
            best_err, best_lag = err, lag

    a = ours[best_lag * step:]
    n = min(len(a), len(theirs))
    if n < 44100:
        return None
    n -= n % step
    err = 0.0
    peak = 0
    for i in range(0, n, step):
        d = a[i] - theirs[i]
        err += d * d
        if abs(d) > peak:
            peak = abs(d)
    rms = math.sqrt(err / (n / step)) / 2147483648.0
    return (rms, best_lag, n, peak // 65536)


# --- FLAC: exact, against Xiph's own decoder -------------------------------

FLAC_MAIN_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lib/usnd.h"
#include "lib/usnd_internal.h"
#include "rt/sys.h"

extern const struct usnd_codec usnd_codec_flac;
static const char *g_err = "";
const char *usnd_last_error(void) { return g_err; }
void usnd_fail(const char *m) { g_err = m; }

// argv: <file> [seek target...]. With no targets, the whole stream as
// s32 on stdout; with targets, for each one a seek and 1000 frames.
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    struct usnd_stream s;
    memset(&s, 0, sizeof s);
    s.fd = sys_open(argv[1], 0);
    if (s.fd < 0) return 2;
    s.codec = &usnd_codec_flac;
    int rc = usnd_codec_flac.open(&s);
    if (rc != 0) { fprintf(stderr, "open: %s (%d)\n", usnd_last_error(), rc); return 3; }
    fprintf(stderr, "rate=%u ch=%u bps=%u frames=%llu\n", s.fmt.rate, s.fmt.channels,
            s.fmt.bits, (unsigned long long)s.frames);
    static int32_t buf[4096 * 2];
    if (argc == 2) {
        for (;;) {
            long n = usnd_codec_flac.read(&s, buf, 4096);
            if (n < 0) { fprintf(stderr, "read: %s\n", usnd_last_error()); return 4; }
            if (n == 0) break;
            fwrite(buf, sizeof(int32_t), (size_t)n * s.fmt.channels, stdout);
        }
    } else {
        for (int i = 2; i < argc; i++) {
            if (usnd_codec_flac.seek(&s, strtoull(argv[i], 0, 10)) != 0) return 5;
            long got = 0;
            while (got < 1000) {
                long n = usnd_codec_flac.read(&s, buf, 1000 - got);
                if (n <= 0) break;
                fwrite(buf, sizeof(int32_t), (size_t)n * s.fmt.channels, stdout);
                got += n;
            }
            if (got != 1000) { fprintf(stderr, "seek %s: %ld frames\n", argv[i], got); return 6; }
        }
    }
    usnd_codec_flac.close(&s);
    return 0;
}
"""


def flac_source(bps, channels, rate, secs=1.5, wasted=0):
    """Integer samples at `bps`, shaped for what each subframe type needs:
    tones and clicks (LPC/fixed), a silent stretch (CONSTANT), a noise
    burst (VERBATIM wins there), and the channels different enough that
    each stereo decorrelation is a real choice."""
    n = int(rate * secs)
    full = (1 << (bps - 1)) - 1
    seed = 0x2468ACE1
    out = []
    for i in range(n):
        t = i / rate
        v = 0.4 * math.sin(2 * math.pi * 440 * t) + 0.2 * math.sin(2 * math.pi * 1733 * t)
        if i % (rate // 4) < 30:
            v += 0.3
        seed = (seed * 1103515245 + 12345) & 0xFFFFFFFF
        noise = ((seed >> 8) & 0xFFFF) / 32768.0 - 1.0
        frame = []
        for c in range(channels):
            x = v if c == 0 else 0.6 * v + 0.3 * math.sin(2 * math.pi * 997 * t)
            if n * 0.40 < i < n * 0.50:
                x = 0.0                                      # silence
            elif n * 0.70 < i < n * 0.75:
                x = 0.95 * noise * (1 if c == 0 else -1)     # a noise burst
            q = int(x * full * 0.9)
            q = max(-full - 1, min(full, q))
            if wasted:
                q = (q >> wasted) << wasted
            frame.append(q)
        out.append(frame)
    return out


def write_raw(path, frames, bps):
    width = (bps + 7) // 8
    body = bytearray()
    for frame in frames:
        for v in frame:
            body += (v & ((1 << (8 * width)) - 1)).to_bytes(width, "little")
    open(path, "wb").write(bytes(body))


def write_wav_any(path, frames, bps, rate):
    """For ffmpeg, which reads WAV: 8 unsigned, 16/24/32 signed."""
    ch = len(frames[0])
    width = (bps + 7) // 8
    body = bytearray()
    for frame in frames:
        for v in frame:
            if bps == 8:
                body += bytes(((v + 128) & 0xFF,))
            else:
                body += (v & ((1 << (8 * width)) - 1)).to_bytes(width, "little")
    block = ch * width
    hdr = b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, ch, rate, rate * block, block, width * 8)
    hdr += b"data" + struct.pack("<I", len(body))
    open(path, "wb").write(hdr + bytes(body))


def write_wav_ext(path, frames, bps, rate):
    """WAVE_FORMAT_EXTENSIBLE with wValidBitsPerSample = bps, the samples
    LEFT-justified in their container -- how a 12- or 20-bit stream reaches
    `flac`, whose raw input takes only whole bytes of depth."""
    ch = len(frames[0])
    width = (bps + 7) // 8
    up = width * 8 - bps
    body = bytearray()
    for frame in frames:
        for v in frame:
            body += ((v << up) & ((1 << (8 * width)) - 1)).to_bytes(width, "little")
    block = ch * width
    guid = bytes.fromhex("0100000000001000800000aa00389b71")      # KSDATAFORMAT_SUBTYPE_PCM
    fmt = struct.pack("<HHIIHHHHI", 0xFFFE, ch, rate, rate * block, block, width * 8,
                      22, bps, 3 if ch == 2 else 4) + guid
    hdr = b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(body)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<I", len(fmt)) + fmt
    hdr += b"data" + struct.pack("<I", len(body))
    open(path, "wb").write(hdr + bytes(body))


def flac_reference(path, bps):
    if bps % 8:
        # `flac -d` writes raw only at whole bytes of depth; anything else
        # comes out as a WAV with the samples left-justified, shifted back.
        wav = subprocess.run(["flac", "-d", "-s", "-c", path], stdout=subprocess.PIPE,
                             check=True).stdout
        p, width = 12, 0
        while p + 8 <= len(wav):
            cid, clen = wav[p:p + 4], struct.unpack("<I", wav[p + 4:p + 8])[0]
            if cid == b"fmt ":
                ch_, blk = struct.unpack("<H", wav[p + 10:p + 12])[0], struct.unpack("<H", wav[p + 20:p + 22])[0]
                width = blk // ch_
            elif cid == b"data":
                data = wav[p + 8:p + 8 + clen]
                up = width * 8 - bps
                fmtc = {2: "h", 4: "i"}.get(width)
                if fmtc:
                    vals = struct.unpack("<%d%s" % (len(data) // width, fmtc), data)
                else:
                    vals = [int.from_bytes(data[i:i + 3], "little", signed=True)
                            for i in range(0, len(data), 3)]
                return [v >> up for v in vals]
            p += 8 + clen + (clen & 1)
        raise ValueError("flac -d wrote no data chunk")
    raw = subprocess.run(["flac", "-d", "-s", "-c", "--force-raw-format", "--endian=little",
                          "--sign=signed", path], stdout=subprocess.PIPE, check=True).stdout
    width = (bps + 7) // 8
    if width == 2:
        return list(struct.unpack("<%dh" % (len(raw) // 2), raw))
    if width == 4:
        return list(struct.unpack("<%di" % (len(raw) // 4), raw))
    if width == 1:
        return list(struct.unpack("<%db" % len(raw), raw))
    return [int.from_bytes(raw[i:i + 3], "little", signed=True) for i in range(0, len(raw), 3)]


# (label, bps, channels, rate, encoder, args, wasted)
FLAC_CASES = [
    ("16-bit stereo -0", 16, 2, 44100, "flac", ["-0"], 0),
    ("16-bit stereo -5", 16, 2, 44100, "flac", ["-5"], 0),
    ("16-bit stereo -8 -e -p", 16, 2, 44100, "flac", ["-8", "-e", "-p"], 0),
    ("16-bit mono -8", 16, 1, 48000, "flac", ["-8"], 0),
    ("8-bit mono 22.05k", 8, 1, 22050, "flac", ["-5"], 0),
    ("12-bit stereo", 12, 2, 32000, "flac", ["-8"], 0),
    ("20-bit stereo", 20, 2, 48000, "flac", ["-8"], 0),
    ("24-bit stereo 96k", 24, 2, 96000, "flac", ["-8"], 0),
    ("32-bit stereo", 32, 2, 48000, "flac", ["-5"], 0),
    ("24-bit, low 8 bits wasted", 24, 2, 48000, "flac", ["-5"], 8),
    ("blocks of 192", 16, 2, 44100, "flac", ["-5", "-b", "192"], 0),
    ("blocks of 32768, no seektable", 16, 2, 44100, "flac",
     ["--lax", "-5", "-b", "32768", "--no-seektable"], 0),
    ("ffmpeg level 12", 16, 2, 44100, "ffmpeg", ["-compression_level", "12"], 0),
    ("ffmpeg 24-bit", 24, 2, 48000, "ffmpeg", ["-compression_level", "8"], 0),
]


def flac_sweep(work, sabotage):
    """Builds the codec (broken on purpose under --positive-control),
    encodes every case, compares. Returns (fails, checks)."""
    src = os.path.join(REPO, "userland", "lib", "usnd_flac.c")
    if sabotage:
        # THE CONTROL: the zig-zag that turns a Rice code back into a
        # signed residual, dropped. Every case has Rice-coded frames.
        text = open(src).read()
        old = "out[i] = (int64_t)(u >> 1) ^ -(int64_t)(u & 1);"
        if text.count(old) != 1:
            sys.exit("positive control: residual()'s zig-zag no longer looks as expected")
        src = os.path.join(work, "usnd_flac_broken.c")
        open(src, "w").write(text.replace(old, "out[i] = (int64_t)(u >> 1);"))
    main_c = os.path.join(work, "flac_main.c")
    open(main_c, "w").write(FLAC_MAIN_C)
    binary = os.path.join(work, "usnd_flac_host")
    r = subprocess.run(["gcc", "-O2", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-I" + os.path.join(work, "inc"), "-I" + os.path.join(REPO, "userland"),
                        src, main_c, "-o", binary], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr)
        print("usnd_hostcheck: the FLAC codec did not compile on the host")
        return 1, 1

    fails, checks = 0, 0
    for label, bps, ch, rate, enc, args, wasted in FLAC_CASES:
        frames = flac_source(bps, ch, rate, wasted=wasted)
        slug = "".join(c if c.isalnum() else "_" for c in label)
        path = os.path.join(work, slug + ".flac")
        if enc == "flac" and bps % 8:
            wav = os.path.join(work, slug + ".wav")
            write_wav_ext(wav, frames, bps, rate)
            subprocess.run(["flac", "-s", "-f"] + args + ["-o", path, wav], check=True)
        elif enc == "flac":
            raw = os.path.join(work, slug + ".raw")
            write_raw(raw, frames, bps)
            subprocess.run(["flac", "-s", "-f", "--force-raw-format", "--endian=little",
                            "--sign=signed", "--channels=%d" % ch, "--bps=%d" % bps,
                            "--sample-rate=%d" % rate] + args + ["-o", path, raw], check=True)
        else:
            wav = os.path.join(work, slug + ".wav")
            write_wav_any(wav, frames, bps, rate)
            fmt = ["-sample_fmt", "s32"] if bps > 16 else []
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", wav, "-c:a", "flac"] + fmt
                           + args + [path], check=True)
        checks += 1
        r = subprocess.run([binary, path], capture_output=True)
        if r.returncode != 0:
            print(f"  FAIL  flac {label} -- decoder exited {r.returncode}: "
                  f"{r.stderr.decode(errors='replace').strip()}")
            fails += 1
            continue
        ours = struct.unpack("<%di" % (len(r.stdout) // 4), r.stdout)
        want = flac_reference(path, bps)
        shift = 32 - bps
        bad = None
        if len(ours) != len(want):
            bad = f"{len(ours)} samples, the reference has {len(want)}"
        else:
            for i, (a, b) in enumerate(zip(ours, want)):
                if (a >> shift) != b or (shift and a & ((1 << shift) - 1)):
                    bad = f"sample {i} is {a >> shift} (s32 {a:#x}), want {b}"
                    break
        if bad:
            fails += 1
        print(f"  {'FAIL' if bad else 'ok  '}  flac {label:30s} "
              f"{bad or 'exact, %d samples' % len(want)}")

        if label in ("16-bit stereo -5", "blocks of 32768, no seektable") and not bad:
            checks += 1
            targets = [0, 1, 4095, 4096, 33333, len(want) // ch - 1000]
            r = subprocess.run([binary, path] + [str(t) for t in targets], capture_output=True)
            got = struct.unpack("<%di" % (len(r.stdout) // 4), r.stdout) if r.returncode == 0 else ()
            sbad = None if r.returncode == 0 else r.stderr.decode(errors="replace").strip()
            per = 1000 * ch
            for k, t in enumerate(targets):
                if sbad:
                    break
                if list(got[k * per:(k + 1) * per]) != list(ours[t * ch:t * ch + per]):
                    sbad = f"a seek to {t} lands somewhere else"
            if sbad:
                fails += 1
            print(f"  {'FAIL' if sbad else 'ok  '}  flac {label + ' -- seeks':30s} "
                  f"{sbad or 'all %d land exactly' % len(targets)}")
    return fails, checks


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--file", help="check one existing MP3 instead of the sweep")
    # 1e-3 is ~600x the observed agreement (1.6e-6, i.e. last-bit rounding)
    # and ~12x below the weakest corruption the positive control produces.
    # The first value here was 0.02, picked by eye; --positive-control then
    # showed a deliberately broken decoder passing two checks under it,
    # which is what a threshold chosen without a control is worth.
    ap.add_argument("--tolerance", type=float, default=1e-3,
                    help="max RMS error relative to full scale (default 1e-3)")
    ap.add_argument("--keep", help="keep artifacts in this directory")
    ap.add_argument("--positive-control", action="store_true",
                    help="build a deliberately broken decoder; every audio "
                         "check MUST go red, and the run fails if any passes")
    args = ap.parse_args()

    for tool in ("gcc", "ffmpeg", "lame", "flac"):
        if not shutil.which(tool):
            print(f"usnd_hostcheck: {tool} is not on PATH")
            return 2

    work = args.keep or tempfile.mkdtemp(prefix="usndcheck.")
    os.makedirs(work, exist_ok=True)
    inc = os.path.join(work, "inc")
    os.makedirs(os.path.join(inc, "rt"), exist_ok=True)
    open(os.path.join(inc, "rt", "sys.h"), "w").write(SHIM_SYS_H)
    main_c = os.path.join(work, "main.c")
    open(main_c, "w").write(MAIN_C)

    binary = os.path.join(work, "usnd_mp3_host")
    cc = ["gcc", "-O2", "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter"]
    if args.positive_control:
        cc += ["-DUSND_MP3_POISON=1"]
    cc += [
          "-I" + inc, "-I" + os.path.join(REPO, "userland"),
          os.path.join(REPO, "userland", "lib", "usnd_mp3.c"),
          os.path.join(REPO, "userland", "lib", "usnd_mpsynth.c"), main_c,
          "-o", binary, "-lm"]
    r = subprocess.run(cc, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr)
        print("usnd_hostcheck: the decoder did not compile on the host")
        return 1
    if r.stderr.strip():
        print(r.stderr.strip())

    # The structural check first: it needs no audio, and a build that
    # fails it cannot produce a meaningful comparison anyway.
    r = subprocess.run([binary, "--selftest"], capture_output=True, text=True)
    ok_selftest = r.returncode == 0
    print(f"  {'ok  ' if ok_selftest else 'FAIL'}  Huffman tables are complete prefix codes"
          f"  -- {r.stderr.strip()}")
    if not ok_selftest:
        return 1

    fails = 0
    cases = []
    if args.file:
        cases = [(os.path.basename(args.file), args.file, None)]
    else:
        L, R = source()
        src = os.path.join(work, "src.wav")
        write_wav(src, (L, R))
        srcm = os.path.join(work, "srcm.wav")
        write_wav(srcm, (L,))
        for label, opts in CASES:
            mp3 = os.path.join(work, label + ".mp3")
            mono = "-m" in opts and opts[opts.index("-m") + 1] == "m"
            subprocess.run(["lame", "--quiet"] + opts + ["-t",
                           srcm if mono else src, mp3], check=True)
            cases.append((label, mp3, 1 if mono else 2))

    for label, path, ch in cases:
        r = subprocess.run([path and binary, path], capture_output=True)
        if r.returncode != 0:
            print(f"  FAIL  {label}  -- decoder exited {r.returncode}: "
                  f"{r.stderr.decode(errors='replace').strip()}")
            fails += 1
            continue
        info = r.stderr.decode(errors="replace").strip()
        rate = 44100
        channels = ch
        for tok in info.split():
            if tok.startswith("rate="):
                rate = int(tok[5:])
            if tok.startswith("ch="):
                channels = int(tok[3:])
        ours = struct.unpack("<%di" % (len(r.stdout) // 4), r.stdout)
        theirs = decode_ffmpeg(path, channels, rate)
        got = compare(ours, theirs, channels)
        if not got:
            print(f"  FAIL  {label}  -- too little output to compare "
                  f"({len(ours)} samples)")
            fails += 1
            continue
        rms, lag, n, peak = got
        ok = rms <= args.tolerance
        if not ok:
            fails += 1
        # Scientific notation, and the WORST single sample beside the RMS:
        # a mean over 260 000 samples hides a small systematic error, which
        # is exactly what a weak positive control looks like.
        print(f"  {'ok  ' if ok else 'FAIL'}  {label:16s} rms {rms:.2e}  "
              f"worst {peak:5d}/32768  lag {lag}  samples {n}")

    if not args.file:
        ffails, fchecks = flac_sweep(work, args.positive_control)
        fails += ffails
        cases += [None] * fchecks

    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    if args.positive_control:
        # Inverted: a broken decoder that still passes means the comparison
        # is not comparing, which is worse than a failing decoder.
        print(f"\nusnd_hostcheck --positive-control: {fails} of {len(cases)} "
              f"checks went red")
        if fails == len(cases):
            print("  the harness can fail -- a clean run means something")
            return 0
        print("  THE HARNESS DID NOT FAIL ON A BROKEN DECODER")
        return 1
    print(f"\nusnd_hostcheck: {len(cases) - fails} passed, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
