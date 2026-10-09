#!/usr/bin/env python3
"""Check userland/lib/uvid*.c (MPEG-1 video, the program stream) and
usnd_mp2.c (MPEG-1 Layer II) against FFmpeg, on the HOST.

The split tools/usnd_hostcheck.py makes for MP3, for the same reason: the
guest's /tests/uvid_test proves the decoders work in toy-os on the
fixtures somebody committed, but a video decoder's bugs hide in
COMBINATIONS -- this quantiser with that motion range, a B picture whose
skipped macroblocks repeat a bidirectional vector -- which is a sweep.
This compiles the SAME .c files with the host gcc and decodes as many
ffmpeg-encoded files as asked for, comparing every frame and sample with
ffmpeg's own decode.

**THE COMPARISON IS NOT BIT-EXACT, AND THAT IS THE STANDARD'S POSITION.**
ISO/IEC 11172 defines a compliant decoder by IDCT ACCURACY (IEEE 1180),
not by its exact output, so two correct decoders differ by a rounding
here and there -- and in a P picture that difference is carried forward
until the next I. So video is judged by PSNR against ffmpeg's decode
with `-idct int` (libjpeg's integer IDCT, the one uvid_mpeg1.c uses),
and the count of frames that came out BIT-EXACT is reported beside it
(none do: ffmpeg's integer IDCT rounds its last pass differently). The
bar is set from measurement -- see --min-psnr -- because a subtle bug
(a rounding bias in motion compensation) costs 10 dB, not a garbage
frame, and "looks the same" lets it through. Audio is RMS error
relative to full scale, as for MP3.

  python3 tools/uvid_hostcheck.py                 # the standard sweep
  python3 tools/uvid_hostcheck.py --file x.mpg    # one real file
  python3 tools/uvid_hostcheck.py --keep DIR      # keep the artifacts
  python3 tools/uvid_hostcheck.py --positive-control
                                                  # a decoder with a broken
                                                  # half-pel average; must FAIL

Exit status is non-zero if any check fails. Needs gcc and ffmpeg.
"""
import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
LIB = os.path.join(REPO, "userland", "lib")

# The ring-3 syscalls the decoders use, as POSIX underneath the same names.
SHIM_SYS_H = r"""
#ifndef HOST_SYS_H
#define HOST_SYS_H
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#define SYS_SEEK_SET 0
#define SYS_SEEK_CUR 1
#define SYS_SEEK_END 2
struct sys_stat { uint64_t size; };
static inline int64_t sys_read(int fd, void *b, size_t n) { return read(fd, b, n); }
static inline int sys_open(const char *p, int f) { (void)f; return open(p, O_RDONLY); }
static inline int sys_close(int fd) { return close(fd); }
static inline long long sys_lseek(int fd, long long o, int w) { return lseek(fd, o, w); }
static inline int sys_fstat(int fd, struct sys_stat *s) {
    struct stat st; if (fstat(fd, &st)) return -1; s->size = (uint64_t)st.st_size; return 0;
}
static inline uint64_t sys_monotonic_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
#endif
"""

MAIN_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lib/uvid.h"
#include "lib/uvid_internal.h"
#include "lib/usnd.h"
#include "lib/usnd_internal.h"
#include "rt/sys.h"

// Motion JPEG goes through lib/uimg.h, judged by uimg_hostcheck; not here.
const struct uvid_codec uvid_codec_mjpeg = { .id = UVID_VC_MJPEG, .name = "mjpeg" };

static const char *g_err = "";
const char *usnd_last_error(void) { return g_err; }
void usnd_fail(const char *m) { g_err = m; }

static void put_frame(const struct uvid_frame *f) {
    int cw = (f->w + 1) / 2, ch = (f->h + 1) / 2;
    for (int y = 0; y < f->h; y++) fwrite(f->y + (size_t)y * f->y_stride, 1, (size_t)f->w, stdout);
    for (int y = 0; y < ch; y++) fwrite(f->cb + (size_t)y * f->c_stride, 1, (size_t)cw, stdout);
    for (int y = 0; y < ch; y++) fwrite(f->cr + (size_t)y * f->c_stride, 1, (size_t)cw, stdout);
}

static int audio(const struct usnd_codec *codec, const char *path) {
    struct usnd_stream s;
    memset(&s, 0, sizeof s);
    s.fd = sys_open(path, 0);
    if (s.fd < 0) return 2;
    s.codec = codec;
    int rc = codec->open(&s);
    if (rc) { fprintf(stderr, "open: %s (%d)\n", usnd_last_error(), rc); return 3; }
    fprintf(stderr, "rate=%u ch=%u frames=%llu\n", s.fmt.rate, s.fmt.channels, (unsigned long long)s.frames);
    static int32_t buf[4096 * 2];
    long n;
    while ((n = codec->read(&s, buf, 4096)) > 0) fwrite(buf, 4, (size_t)n * s.fmt.channels, stdout);
    codec->close(&s);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "selftest")) {
        int bad = uvid_mpeg1_selftest();
        fprintf(stderr, "prefix clashes: %d\n", bad);
        return bad != 0;
    }
    if (argc < 3) return 2;
    if (!strcmp(argv[1], "mp2")) return audio(&usnd_codec_mp2, argv[2]);
    if (!strcmp(argv[1], "audio")) return audio(&usnd_codec_vid, argv[2]);
    struct uvid *v;
    int rc = uvid_open(argv[2], &v);
    if (rc) { fprintf(stderr, "open: %s (%d)\n", uvid_last_error(), rc); return 3; }
    const struct uvid_info *in = uvid_info(v);
    fprintf(stderr, "info %d %d %u %u %u %u\n", in->w, in->h, in->fps_num, in->fps_den, in->ms, in->frames);
    if (!strcmp(argv[1], "seek") && argc > 3) {
        rc = uvid_seek(v, (uint32_t)atoi(argv[3]), UVID_SEEK_EXACT);
        if (rc) { fprintf(stderr, "seek: %s (%d)\n", uvid_last_error(), rc); return 4; }
        const struct uvid_frame *f;
        if (uvid_next(v, &f) != 1) { fprintf(stderr, "no frame after seek\n"); return 5; }
        fprintf(stderr, "frame %lld %c\n", (long long)f->pts_ms, f->type);
        put_frame(f);
        uvid_close(v);
        return 0;
    }
    const struct uvid_frame *f;
    while ((rc = uvid_next(v, &f)) == 1) {
        fprintf(stderr, "frame %lld %c\n", (long long)f->pts_ms, f->type);
        put_frame(f);
    }
    uvid_close(v);
    return rc < 0 ? 6 : 0;
}
"""

SOURCES = ["uvid.c", "uvid_avi.c", "uvid_ps.c", "uvid_mpeg1.c", "uvid_draw.c",
           "usnd_vid.c", "usnd_mp2.c", "usnd_mpsynth.c"]

# (label, size, lavfi source, ffmpeg video options). Chosen to cover what
# breaks an MPEG-1 decoder: every quantiser end, I-only, no B, long GOPs
# (drift), big motion (f_code > 1), custom matrices, sizes that are not a
# multiple of 16, skipped macroblocks (a still background), and the
# rate-distortion search that picks unusual macroblock types.
VIDEO_CASES = [
    ("q2-ibbp", "320x240", "testsrc2", ["-q:v", "2", "-bf", "2", "-g", "12"]),
    ("q31-ibbp", "320x240", "testsrc2", ["-q:v", "31", "-bf", "2", "-g", "12"]),
    ("q1-intra", "176x144", "testsrc2", ["-q:v", "1", "-g", "1"]),
    ("q6-noB-gop60", "352x288", "testsrc2", ["-q:v", "6", "-bf", "0", "-g", "60"]),
    ("bitrate-rd", "352x240", "testsrc2", ["-b:v", "600k", "-bf", "3", "-g", "18", "-mbd", "rd", "-trellis", "1"]),
    ("odd-size", "100x58", "testsrc2", ["-q:v", "4", "-bf", "2", "-g", "9"]),
    ("fast-motion", "320x240", "fastmotion", ["-q:v", "5", "-bf", "2", "-g", "15", "-me_range", "120"]),
    ("still-skips", "320x240", "still", ["-q:v", "4", "-bf", "2", "-g", "30"]),
    ("matrices", "320x240", "testsrc2", ["-q:v", "4", "-bf", "1", "-g", "10",
                                         "-intra_matrix", ",".join(["%d" % (8 + i % 40) for i in range(64)]),
                                         "-inter_matrix", ",".join(["%d" % (16 + (i * 7) % 30) for i in range(64)])]),
    ("hq-mbd-qpel", "320x240", "mandel", ["-q:v", "3", "-bf", "2", "-g", "12", "-mbd", "rd", "-cmp", "2", "-subcmp", "2"]),
]

AUDIO_CASES = [
    # (label, rate, channels, bitrate, extra)
    ("44k-stereo-192", 44100, 2, "192k", []),
    ("48k-stereo-384", 48000, 2, "384k", []),
    ("32k-mono-64", 32000, 1, "64k", []),
    ("44k-joint-128", 44100, 2, "128k", ["-joint_stereo", "1"]),
    ("48k-stereo-64", 48000, 2, "64k", []),
    ("32k-stereo-56", 32000, 2, "56k", []),
    ("44k-mono-32", 44100, 1, "32k", []),
]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, **kw)


def ff(args):
    r = run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y"] + args)
    if r.returncode != 0:
        sys.exit("uvid_hostcheck: ffmpeg failed: %s\n%s" % (" ".join(args), r.stderr.decode()))


def lavfi(kind, size, seconds):
    if kind == "testsrc2":
        return "testsrc2=s=%s:r=25:d=%s" % (size, seconds)
    if kind == "mandel":
        return "mandelbrot=s=%s:r=25,trim=duration=%s" % (size, seconds)
    if kind == "fastmotion":
        return ("testsrc2=s=%s:r=25:d=%s,scroll=h=0.05:v=0.03" % (size, seconds))
    if kind == "still":
        return ("color=c=0x335577:s=%s:r=25:d=%s,drawbox=x='mod(t*40,200)':y=60:w=40:h=40:color=yellow:t=fill"
                % (size, seconds))
    raise ValueError(kind)


def psnr(a, b):
    if len(a) != len(b) or not a:
        return 0.0
    se = 0
    for x, y in zip(a, b):
        d = x - y
        se += d * d
    if se == 0:
        return float("inf")
    return 10 * math.log10(255 * 255 * len(a) / se)


def compare_video(binary, path, work, label, min_psnr):
    ours_p = run([binary, "video", path])
    if ours_p.returncode != 0:
        return False, "decoder exit %d: %s" % (ours_p.returncode, ours_p.stderr.decode().strip()[-200:])
    info = [l.split() for l in ours_p.stderr.decode().splitlines() if l.startswith("info")]
    w, h = int(info[0][1]), int(info[0][2])
    ref = os.path.join(work, label + ".ref.yuv")
    ff(["-idct", "int", "-i", path, "-f", "rawvideo", "-pix_fmt", "yuv420p", ref])
    theirs = open(ref, "rb").read()
    ours = ours_p.stdout
    fsize = w * h + 2 * ((w + 1) // 2) * ((h + 1) // 2)
    nf_ours, nf_ref = len(ours) // fsize, len(theirs) // fsize
    if nf_ours != nf_ref:
        return False, "%d frames, ffmpeg has %d" % (nf_ours, nf_ref)
    worst, exact = float("inf"), 0
    for i in range(nf_ref):
        a = ours[i * fsize:(i + 1) * fsize]
        b = theirs[i * fsize:(i + 1) * fsize]
        if a == b:
            exact += 1
            continue
        p = psnr(a[:w * h], b[:w * h])
        worst = min(worst, p)
    ok = worst >= min_psnr
    return ok, "%d frames, %d bit-exact, worst luma PSNR %s" % (
        nf_ref, exact, "inf" if worst == float("inf") else "%.1f dB" % worst)


def read_s32(data):
    n = len(data) // 4
    return struct.unpack("<%di" % n, data[:n * 4])


def compare_audio(binary, mode, path, work, label, tol):
    ours_p = run([binary, mode, path])
    if ours_p.returncode != 0:
        return False, "decoder exit %d: %s" % (ours_p.returncode, ours_p.stderr.decode().strip()[-200:])
    head = ours_p.stderr.decode().split()
    chans = int([x for x in head if x.startswith("ch=")][0][3:])
    ref = os.path.join(work, label + ".ref.s32")
    ff(["-c:a", "mp2float", "-i", path, "-vn", "-f", "s32le", "-ac", str(chans), ref])
    theirs = read_s32(open(ref, "rb").read())
    ours = read_s32(ours_p.stdout)
    n = min(len(ours), len(theirs))
    if n == 0:
        return False, "no samples (ours %d, ffmpeg %d)" % (len(ours), len(theirs))
    se = 0.0
    for i in range(n):
        d = (ours[i] - theirs[i]) / 2147483648.0
        se += d * d
    rms = math.sqrt(se / n)
    length_ok = abs(len(ours) - len(theirs)) <= 1152 * chans
    return rms <= tol and length_ok, "%d samples (ffmpeg %d), RMS error %.2e" % (len(ours), len(theirs), rms)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--file", help="check one real .mpg instead of the sweep")
    ap.add_argument("--keep", help="keep the artifacts in DIR")
    ap.add_argument("--seconds", default="2", help="length of each generated video (default 2)")
    # MEASURED, not chosen: the decoder's worst frame over the sweep is
    # ~60 dB (the 60-frame GOP, where IDCT rounding drifts longest), and
    # the positive control's half-pel rounding bias drops that case to
    # ~49 dB. 40 dB -- "looks the same" -- let the control pass.
    ap.add_argument("--min-psnr", type=float, default=55.0, help="worst luma PSNR allowed (default 55 dB)")
    ap.add_argument("--tolerance", type=float, default=1e-3, help="audio RMS error allowed")
    ap.add_argument("--positive-control", action="store_true",
                    help="build with a broken half-pel average; the sweep must FAIL")
    args = ap.parse_args()

    work = args.keep or tempfile.mkdtemp(prefix="uvidcheck.")
    os.makedirs(os.path.join(work, "inc", "rt"), exist_ok=True)
    open(os.path.join(work, "inc", "rt", "sys.h"), "w").write(SHIM_SYS_H)
    main_c = os.path.join(work, "main.c")
    open(main_c, "w").write(MAIN_C)

    srcs = [os.path.join(LIB, s) for s in SOURCES]
    if args.positive_control:
        broken = os.path.join(work, "uvid_mpeg1_broken.c")
        text = open(os.path.join(LIB, "uvid_mpeg1.c")).read()
        needle = "else if (hx) v = (a + bb + 1) >> 1;"
        if needle not in text:
            sys.exit("uvid_hostcheck: the positive control's needle is gone from uvid_mpeg1.c")
        open(broken, "w").write(text.replace(needle, "else if (hx) v = (a + bb) >> 1;"))
        srcs = [broken if s.endswith("uvid_mpeg1.c") else s for s in srcs]

    binary = os.path.join(work, "uvid_host")
    cc = ["gcc", "-O2", "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter",
          "-I" + os.path.join(work, "inc"), "-I" + os.path.join(REPO, "userland"),
          "-include", "string.h"] + srcs + [main_c, "-o", binary, "-lm", "-lpthread"]
    r = run(cc, text=True)
    if r.returncode != 0:
        print(r.stderr)
        print("uvid_hostcheck: the decoders did not compile on the host")
        return 1
    if r.stderr.strip():
        print(r.stderr.strip())

    fails = passes = 0

    def report(ok, label, detail):
        nonlocal fails, passes
        print("  %-5s %-28s %s" % ("ok" if ok else "FAIL", label, detail))
        if ok:
            passes += 1
        else:
            fails += 1

    r = run([binary, "selftest"])
    report(r.returncode == 0, "tables are prefix codes", r.stderr.decode().strip())

    if args.file:
        ok, detail = compare_video(binary, args.file, work, "file", args.min_psnr)
        report(ok, os.path.basename(args.file) + " video", detail)
        ok, detail = compare_audio(binary, "audio", args.file, work, "file-audio", args.tolerance)
        report(ok, os.path.basename(args.file) + " sound", detail)
    else:
        for label, size, kind, opts in VIDEO_CASES:
            path = os.path.join(work, label + ".mpg")
            ff(["-f", "lavfi", "-i", lavfi(kind, size, args.seconds), "-c:v", "mpeg1video"] + opts +
               ["-an", "-f", "mpeg", path])
            ok, detail = compare_video(binary, path, work, label, args.min_psnr)
            report(ok, "mpeg1 " + label, detail)

        for label, rate, chans, br, extra in AUDIO_CASES:
            path = os.path.join(work, label + ".mp2")
            src = ("aevalsrc=exprs='0.4*sin(2*PI*440*t)+0.2*sin(2*PI*3000*t*(1+t))+0.05*(random(0)-0.5)'"
                   ":s=%d:d=3" % rate)
            ff(["-f", "lavfi", "-i", src, "-ac", str(chans), "-c:a", "mp2", "-b:a", br] + extra + [path])
            ok, detail = compare_audio(binary, "mp2", path, work, "mp2-" + label, args.tolerance)
            report(ok, "mp2 " + label, detail)

        # The sound and the pictures of one program stream, and seeking.
        mux = os.path.join(work, "muxed.mpg")
        ff(["-f", "lavfi", "-i", lavfi("testsrc2", "320x240", "6"),
            "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=44100:d=6",
            "-c:v", "mpeg1video", "-q:v", "4", "-bf", "2", "-g", "15",
            "-c:a", "mp2", "-b:a", "192k", "-f", "mpeg", mux])
        ok, detail = compare_audio(binary, "audio", mux, work, "muxed-audio", args.tolerance)
        report(ok, "mpeg-ps sound", detail)
        ok, detail = compare_video(binary, mux, work, "muxed", args.min_psnr)
        report(ok, "mpeg-ps pictures", detail)
        ref = open(os.path.join(work, "muxed.ref.yuv"), "rb").read()
        fsize = 320 * 240 * 3 // 2
        # The frame SHOWING at a time: frames are 40 ms apart from the
        # first one's time, which is not 0 when the sound starts first
        # (the shared clock's origin is the earlier of the two streams).
        first = None
        for ms in (0, 1000, 2520, 4999):
            r = run([binary, "seek", mux, str(ms)])
            if first is None:
                pts = [l.split()[1] for l in r.stderr.decode().split("\n") if l.startswith("frame")]
                first = int(pts[0]) if pts else 0
            idx = max(0, (ms - first) // 40)
            want = ref[idx * fsize:(idx + 1) * fsize]
            lines = r.stderr.decode().split("\n")
            got_pts = [l for l in lines if l.startswith("frame")]
            p = psnr(r.stdout[:320 * 240], want[:320 * 240]) if r.returncode == 0 else 0.0
            ok = r.returncode == 0 and p >= args.min_psnr
            report(ok, "seek to %d ms" % ms, "%s, frame %d PSNR %s" % (
                got_pts[0] if got_pts else "no frame", idx, "inf" if p == float("inf") else "%.1f" % p))

        # EVERY 10 ms through a SHORT-GOP file with small packets: most
        # pictures carry no PTS of their own, so a seek that lands mid-GOP
        # must date them from the GOP -- a wrongly dated frame is "the
        # frame showing then" by its label and not by its content.
        tiny = os.path.join(REPO, "data", "tests", "tiny.mpg")
        bad = []
        for ms in range(0, 600, 10):
            r = run([binary, "seek", tiny, str(ms)])
            got = [l.split() for l in r.stderr.decode().split("\n") if l.startswith("frame")]
            if r.returncode != 0 or not got:
                bad.append((ms, "none"))
                continue
            pts = int(got[0][1])
            if not (pts <= max(ms, 15) and pts + 40 > ms):
                bad.append((ms, pts))
        report(not bad, "seek sweep, tiny.mpg", "60 seeks, every 10 ms" if not bad else "wrong: %s" % bad[:6])

    print("\nuvid_hostcheck: %d passed, %d failed" % (passes, fails))
    if args.positive_control:
        print("uvid_hostcheck: positive control %s" % ("FAILED as it must" if fails else
                                                      "PASSED -- the check cannot see a broken decoder"))
        return 0 if fails else 1
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
