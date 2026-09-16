#!/usr/bin/env python3
"""Check userland/lib/uimg_jpeg.c against libjpeg, on the HOST, over many files.

WHY A HOST HARNESS EXISTS BESIDE THE GUEST TEST. /tests/uimg_test runs
the decoder where it actually lives and proves it works in toy-os, but a
guest test can only carry the handful of vectors somebody committed --
and a JPEG decoder's bugs hide in the combinations (this subsampling at
that quality with those dimensions), which is a sweep of a few hundred
files, not nine. This compiles the SAME .c file with the host gcc and
runs it against as many Pillow-generated images as asked for, comparing
every pixel against libjpeg's own decode.

The two are complementary and neither replaces the other: this one
cannot see anything about ring 3, the guest heap, or the file syscalls,
and the guest one cannot afford the breadth.

  python3 tools/uimg_hostcheck.py                 # the standard sweep
  python3 tools/uimg_hostcheck.py --file some.jpg # one real photograph
  python3 tools/uimg_hostcheck.py --tolerance 2   # tighten the bar

Exit status is non-zero if any image is off by more than --tolerance,
which makes it usable from a script. Needs Pillow and gcc.
"""
import argparse
import io
import math
import os
import subprocess
import sys
import tempfile

try:
    from PIL import Image, ImageOps
except ImportError:
    sys.exit("uimg_hostcheck.py needs Pillow: pip install --user pillow")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DRIVER = r"""
#include "lib/uimg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_err = "";
void uimg_set_error(const char *m) { g_err = m; }

// ENCODE MODE: argv = "encode" <raw RGB> <w> <h> <out.jpg>.
static int do_encode(const char *raw, int w, int h, const char *out) {
    FILE *f = fopen(raw, "rb");
    if (!f) return 2;
    struct uimg im;
    im.w = w;
    im.h = h;
    im.has_alpha = 0;
    im.px = malloc((size_t)w * h * sizeof *im.px);
    if (!im.px) return 2;
    for (int i = 0; i < w * h; i++) {
        int r = fgetc(f), g = fgetc(f), b = fgetc(f);
        if (b < 0) return 2;
        im.px[i] = 0xFF000000u | ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
    }
    fclose(f);

    unsigned char *buf;
    size_t len;
    int rc = uimg_codec_jpeg.encode(&im, &buf, &len);
    if (rc < 0) { fprintf(stderr, "rc=%d %s\n", rc, g_err); return 1; }
    FILE *o = fopen(out, "wb");
    if (!o) return 2;
    fwrite(buf, 1, len, o);
    fclose(o);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 6 && strcmp(argv[1], "encode") == 0)
        return do_encode(argv[2], atoi(argv[3]), atoi(argv[4]), argv[5]);
    if (argc != 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);

    struct uimg im;
    int rc = uimg_codec_jpeg.decode(buf, (size_t)n, &im);
    if (rc < 0) {
        fprintf(stderr, "rc=%d %s\n", rc, g_err);
        return 1;
    }
    fprintf(stderr, "%dx%d\n", im.w, im.h);
    for (int i = 0; i < im.w * im.h; i++) {
        unsigned p = im.px[i];
        putchar((p >> 16) & 0xFF);
        putchar((p >> 8) & 0xFF);
        putchar(p & 0xFF);
    }
    return 0;
}
"""


# THE POSITIVE CONTROL BREAKS THE SUCCESSIVE-APPROXIMATION CORRECTION
# BIT, which is the single most likely real bug in the progressive
# decoder: it is the one place a coefficient is adjusted rather than
# assigned, and dropping it leaves a picture that is entirely plausible
# and slightly wrong. Every progressive check must go red while every
# baseline one stays green -- a control that reddens both has broken
# something shared and proves nothing about this path.
#
# **THE `gradient` PATTERN IS EXCLUDED FROM THE VERDICT, and that is
# about the FIXTURE rather than the code.** A smooth ramp quantises to
# almost no nonzero AC coefficients, so the refinement scans have
# nothing already-nonzero to correct and the sabotaged line is never
# reached: five of them decoded identically with it removed. Judging
# them would make the control's own bar a measurement of how flat the
# test images are. The other three patterns reach it every time.
BREAK_AC_REFINE = ("*pc += (*pc > 0) ? (int16_t)bit : (int16_t)-bit;",
                   "(void)bit;")


def build(tmp, break_refine=False):
    src = os.path.join(tmp, "driver.c")
    with open(src, "w") as f:
        f.write(DRIVER)
    codec = os.path.join(ROOT, "userland", "lib", "uimg_jpeg.c")
    if break_refine:
        with open(codec) as f:
            text = f.read()
        if text.count(BREAK_AC_REFINE[0]) != 2:
            sys.exit("positive control: the line it sabotages has moved -- "
                     "update BREAK_AC_REFINE in this file")
        codec = os.path.join(tmp, "uimg_jpeg.c")
        with open(codec, "w") as f:
            f.write(text.replace(*BREAK_AC_REFINE))
    exe = os.path.join(tmp, "uimgdec")
    cmd = ["gcc", "-O2", "-Wall", "-Wextra", "-o", exe, src, codec,
           os.path.join(ROOT, "userland", "lib", "uimg_jpeg_enc.c"),
           "-I" + os.path.join(ROOT, "userland"),
           "-I" + os.path.join(ROOT, "kernel", "include", "abi")]
    subprocess.run(cmd, check=True)
    return exe


def pattern(w, h, kind):
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            if kind == "gradient":
                px[x, y] = (255 * x // max(1, w - 1), 255 * y // max(1, h - 1), 90)
            elif kind == "checks":
                v = 255 if ((x // 3) + (y // 3)) % 2 else 10
                px[x, y] = (v, 255 - v, (x * 7 + y * 3) % 256)
            elif kind == "noise":
                s = (x * 1103515245 + y * 12345) & 0x7FFFFFFF
                px[x, y] = ((s >> 7) & 255, (s >> 15) & 255, (s >> 3) & 255)
            else:  # rings -- smooth, with a sharp centre
                d = math.hypot(x - w / 2, y - h / 2)
                v = int(127 + 120 * math.sin(d / 2.5))
                px[x, y] = (v, (v * 2) % 256, 255 - v)
    return im


def compare(exe, data, name, tolerance, verbose, exif=False):
    with tempfile.NamedTemporaryFile(suffix=".jpg", delete=False) as f:
        f.write(data)
        path = f.name
    try:
        r = subprocess.run([exe, path], capture_output=True)
        ref = Image.open(io.BytesIO(data))
        # THE REFERENCE HAS TO BE TURNED TOO. Pillow's plain open()
        # ignores the Exif tag exactly as libjpeg does, so comparing an
        # oriented decode against it would fail every rotated file and
        # pass a decoder that ignored the tag -- the assertion backwards.
        if exif:
            ref = ImageOps.exif_transpose(ref)
        ref = ref.convert("RGB")
        if r.returncode != 0:
            return name, "decoder refused it: %s" % r.stderr.decode().strip()
        got = r.stdout
        want = ref.tobytes()
        if len(got) != len(want):
            return name, "size mismatch: got %d bytes, libjpeg gave %d" % (
                len(got), len(want))
        worst = 0
        total = 0
        for a, b in zip(got, want):
            d = abs(a - b)
            total += d
            if d > worst:
                worst = d
        mean = total / max(1, len(want))
        if verbose:
            print("  %-34s max %2d  mean %.3f" % (name, worst, mean))
        if worst > tolerance:
            return name, "max channel error %d (> %d), mean %.3f" % (
                worst, tolerance, mean)
        return None
    finally:
        os.unlink(path)


# uimg_jpeg_enc.c's J_ENC_QUALITY. It is a fixed constant there (that
# file says why there is no knob), so the reference encode has to use
# the same number or the comparison below is against a different file.
ENC_QUALITY = 85
# How much worse than libjpeg the encoder may be, in mean levels per
# channel. Measured at 0.17 at worst across every pattern here; a real
# encoder bug is off by tens.
ENC_LOSS_SLACK = 0.5


def mean_error(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / max(1, len(b))


def check_encoder(exe, tmp, tolerance, verbose):
    """What THIS writes, read by libjpeg. Both directions of the same claim.

    TWO COMPARISONS, because they fail differently and neither alone is
    enough.

    1. OUR OWN DECODE of the file against libjpeg's decode of the SAME
       BYTES. Tight -- the decoder sweep's tolerance -- because two
       independent decoders reading one bitstream must agree to a
       rounding step. Anything wrong with the Huffman codes, the 0xFF
       stuffing or the quantiser shows up here.

    2. HOW MUCH THE FILE LOST, against how much LIBJPEG LOSES ENCODING
       THE SAME IMAGE AT THE SAME SETTINGS. **Not an absolute error
       bar**: 4:2:0 at q85 moves a 3-pixel checkerboard by 180 levels
       and that is the format working as designed, so any threshold
       loose enough to pass it would pass real damage too. Measured
       against libjpeg the bar is meaningful and cannot go stale --
       a wrong zigzag or a mis-scaled quantiser is far worse than
       libjpeg, while an honest encoder lands within a fraction of a
       level of it (measured: within 0.17 mean across every pattern).

    The first is the strong one. The second is what catches our encoder
    and our decoder sharing a mistake, which is the failure an encoder
    tested only by its own decoder passes every time.
    """
    failures = []
    checked = 0
    for kind in ("gradient", "checks", "noise", "rings"):
        for (w, h) in ((16, 16), (33, 31), (64, 48), (127, 65)):
            im = pattern(w, h, kind)
            raw = os.path.join(tmp, "in.raw")
            with open(raw, "wb") as f:
                f.write(im.tobytes())
            jpg = os.path.join(tmp, "out.jpg")
            r = subprocess.run([exe, "encode", raw, str(w), str(h), jpg],
                               capture_output=True)
            name = "encode %s %dx%d" % (kind, w, h)
            checked += 2
            if r.returncode != 0:
                failures.append((name, "encoder failed: %s"
                                 % r.stderr.decode().strip()))
                continue
            with open(jpg, "rb") as f:
                data = f.read()

            try:
                got = Image.open(io.BytesIO(data)).convert("RGB")
            except Exception as exc:                      # noqa: BLE001
                failures.append((name, "libjpeg could not open it: %s" % exc))
                continue
            if got.size != (w, h):
                failures.append((name, "libjpeg read it as %dx%d, not %dx%d"
                                 % (got.size[0], got.size[1], w, h)))
                continue

            orig = im.tobytes()
            ours = mean_error(got.tobytes(), orig)
            ref = io.BytesIO()
            im.save(ref, format="JPEG", quality=ENC_QUALITY, subsampling=2)
            theirs = mean_error(
                Image.open(io.BytesIO(ref.getvalue())).convert("RGB").tobytes(),
                orig)
            if verbose:
                print("  %-34s lost %.2f/level, libjpeg %.2f" %
                      (name, ours, theirs))
            if ours > theirs + ENC_LOSS_SLACK:
                failures.append((name, "lost %.2f levels per channel against "
                                 "libjpeg's %.2f at the same quality"
                                 % (ours, theirs)))

            # 2. And our decoder reads the same bytes the same way.
            bad = compare(exe, data, name + " round trip", tolerance, verbose)
            if bad:
                failures.append(bad)
    return checked, failures


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--tolerance", type=int, default=3,
                    help="largest per-channel difference from libjpeg to accept")
    ap.add_argument("--file", action="append", default=[],
                    help="also check a real JPEG from disk (repeatable)")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--positive-control", action="store_true",
                    help="sabotage the progressive correction bit; every "
                         "progressive check must fail and every baseline "
                         "one must still pass")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp, break_refine=args.positive_control)
        failures = []
        prog_failures = 0
        prog_checked = 0
        prog_flat_survivors = []
        baseline_bad = 0   # COUNTED, never derived by subtracting the judged
                           # progressive ones from the total: the excluded
                           # gradients fail too, and subtraction files every
                           # one of them as a baseline failure
        checked = 0
        for kind in ("gradient", "checks", "noise", "rings"):
            for (w, h) in ((16, 16), (17, 9), (33, 31), (64, 48), (127, 65)):
                for sub, subname in ((0, "4:4:4"), (1, "4:2:2"), (2, "4:2:0")):
                    for q in (30, 75, 95):
                        im = pattern(w, h, kind)
                        for prog in (False, True):
                            buf = io.BytesIO()
                            im.save(buf, format="JPEG", quality=q,
                                    subsampling=sub, progressive=prog)
                            name = "%s %dx%d %s q%d%s" % (
                                kind, w, h, subname, q,
                                " progressive" if prog else "")
                            checked += 1
                            judged = prog and kind != "gradient"
                            prog_checked += judged
                            bad = compare(exe, buf.getvalue(), name,
                                          args.tolerance, args.verbose)
                            if bad:
                                failures.append(bad)
                                prog_failures += judged
                                baseline_bad += not prog
                            elif prog and not judged:
                                prog_flat_survivors.append(name)
        # Grayscale and restart markers, which the grid above cannot express.
        for name, kw, conv in (
                ("grayscale q90", dict(quality=90), "L"),
                ("grayscale q90 progressive", dict(quality=90,
                                                   progressive=True), "L"),
                ("restart markers q85", dict(quality=85, subsampling=2,
                                             restart_marker_rows=1), "RGB"),
                ("restart markers q85 progressive",
                 dict(quality=85, subsampling=2, restart_marker_rows=1,
                      progressive=True), "RGB")):
            im = pattern(64, 48, "rings")
            if conv == "L":
                im = im.convert("L")
            buf = io.BytesIO()
            im.save(buf, format="JPEG", **kw)
            checked += 1
            prog_checked += "progressive" in name
            bad = compare(exe, buf.getvalue(), name, args.tolerance, args.verbose)
            if bad:
                failures.append(bad)
                prog_failures += "progressive" in name
                baseline_bad += "progressive" not in name

        # EXIF ORIENTATION, all eight, against Pillow's own transpose.
        # The four that transpose change the image's DIMENSIONS, which is
        # what a decoder ignoring the tag gets visibly wrong.
        for o in range(1, 9):
            im = pattern(64, 48, "checks")
            ex = Image.Exif()
            ex[0x0112] = o
            buf = io.BytesIO()
            im.save(buf, format="JPEG", quality=92, exif=ex.tobytes())
            checked += 1
            bad = compare(exe, buf.getvalue(), "exif orientation %d" % o,
                          args.tolerance, args.verbose, exif=True)
            if bad:
                failures.append(bad)
                baseline_bad += 1

        enc_checked, enc_bad = check_encoder(exe, tmp, args.tolerance,
                                            args.verbose)
        checked += enc_checked
        failures.extend(enc_bad)
        baseline_bad += len(enc_bad)

        for path in args.file:
            with open(path, "rb") as f:
                data = f.read()
            checked += 1
            bad = compare(exe, data, os.path.basename(path), args.tolerance,
                          args.verbose, exif=True)
            if bad:
                failures.append(bad)

        print("checked %d images against libjpeg (%d progressive), tolerance %d" %
              (checked, prog_checked, args.tolerance))

        if args.positive_control:
            print("positive control: %d/%d judged progressive failed, "
                  "%d baseline failed" % (prog_failures, prog_checked,
                                          baseline_bad))
            if prog_flat_survivors:
                print("  %d smooth-gradient images decoded identically "
                      "(too few nonzero AC coefficients to correct); "
                      "not judged" % len(prog_flat_survivors))
            if prog_failures < prog_checked:
                print("CONTROL DID NOT FIRE -- %d progressive images still "
                      "matched libjpeg with the correction bit removed" %
                      (prog_checked - prog_failures))
                return 1
            if baseline_bad:
                print("CONTROL TOO BROAD -- it reddened %d baseline images, "
                      "so it proves nothing about the progressive path" %
                      baseline_bad)
                return 1
            print("control fired on the progressive path and only there")
            return 0

        for name, why in failures:
            print("FAIL %-44s %s" % (name, why))
        if failures:
            print("%d of %d differ" % (len(failures), checked))
            return 1
        print("all match")
        return 0


if __name__ == "__main__":
    sys.exit(main())
