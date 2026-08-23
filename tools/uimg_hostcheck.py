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
    from PIL import Image
except ImportError:
    sys.exit("uimg_hostcheck.py needs Pillow: pip install --user pillow")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DRIVER = r"""
#include "lib/uimg.h"
#include <stdio.h>
#include <stdlib.h>

static const char *g_err = "";
void uimg_set_error(const char *m) { g_err = m; }

int main(int argc, char **argv) {
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


def build(tmp):
    src = os.path.join(tmp, "driver.c")
    with open(src, "w") as f:
        f.write(DRIVER)
    exe = os.path.join(tmp, "uimgdec")
    cmd = ["gcc", "-O2", "-Wall", "-Wextra", "-o", exe, src,
           os.path.join(ROOT, "userland", "lib", "uimg_jpeg.c"),
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


def compare(exe, data, name, tolerance, verbose):
    with tempfile.NamedTemporaryFile(suffix=".jpg", delete=False) as f:
        f.write(data)
        path = f.name
    try:
        r = subprocess.run([exe, path], capture_output=True)
        ref = Image.open(io.BytesIO(data)).convert("RGB")
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


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--tolerance", type=int, default=3,
                    help="largest per-channel difference from libjpeg to accept")
    ap.add_argument("--file", action="append", default=[],
                    help="also check a real JPEG from disk (repeatable)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        failures = []
        checked = 0
        for kind in ("gradient", "checks", "noise", "rings"):
            for (w, h) in ((16, 16), (17, 9), (33, 31), (64, 48), (127, 65)):
                for sub, subname in ((0, "4:4:4"), (1, "4:2:2"), (2, "4:2:0")):
                    for q in (30, 75, 95):
                        im = pattern(w, h, kind)
                        buf = io.BytesIO()
                        im.save(buf, format="JPEG", quality=q, subsampling=sub)
                        name = "%s %dx%d %s q%d" % (kind, w, h, subname, q)
                        checked += 1
                        bad = compare(exe, buf.getvalue(), name,
                                      args.tolerance, args.verbose)
                        if bad:
                            failures.append(bad)
        # Grayscale and restart markers, which the grid above cannot express.
        for name, kw, conv in (
                ("grayscale q90", dict(quality=90), "L"),
                ("restart markers q85", dict(quality=85, subsampling=2,
                                             restart_marker_rows=1), "RGB")):
            im = pattern(64, 48, "rings")
            if conv == "L":
                im = im.convert("L")
            buf = io.BytesIO()
            im.save(buf, format="JPEG", **kw)
            checked += 1
            bad = compare(exe, buf.getvalue(), name, args.tolerance, args.verbose)
            if bad:
                failures.append(bad)

        for path in args.file:
            with open(path, "rb") as f:
                data = f.read()
            checked += 1
            bad = compare(exe, data, os.path.basename(path), args.tolerance,
                          args.verbose)
            if bad:
                failures.append(bad)

        print("checked %d images against libjpeg, tolerance %d" %
              (checked, args.tolerance))
        for name, why in failures:
            print("FAIL %-34s %s" % (name, why))
        if failures:
            print("%d of %d differ" % (len(failures), checked))
            return 1
        print("all match")
        return 0


if __name__ == "__main__":
    sys.exit(main())
