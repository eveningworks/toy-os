#!/usr/bin/env python3
"""Check userland/ui/ucrt.c -- the CRT effect -- with the host gcc, against
a plain integer model of its passes written here in Python.

ucrt is vectorised (SSE2 through GCC's vector types) and packs channels
into shared words, all for speed; the model is the straightforward
per-channel arithmetic it must still equal: the 2x2 average, the box blur
by `sum * (65536 / (2r+1)) >> 16`, the glow's strength, the screen blend,
the mask, the row x column gain, and the bilinear warp. Allowed off by
ONE per channel, which is the screen's 255 + glow rounding to 256 and
being clamped before the gain rather than after.

The curve's GEOMETRY is not modelled: the warp is checked through the
code's own map (where each glass pixel samples), so this covers the
sampling and not the float maths that places it -- tools/crt_test.py
clicks through the curve for that. Sizes include odd halves, because the
glow pads its half image to even sizes and only an odd one reaches that.

    python3 tools/ucrt_hostcheck.py
    python3 tools/ucrt_hostcheck.py --positive-control   # must FAIL
    python3 tools/ucrt_hostcheck.py --bench              # ms per preset, informational

Compiled with -ffreestanding like the tree, which matters: it implies
-fno-builtin, and a small memcpy() that is free here is a call there.
Needs only gcc and the standard library.
"""

import argparse
import os
import random
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostcheck  # noqa: E402

FLAGS = ("-O2", "-Wall", "-Wextra", "-Werror", "-ffreestanding", "-mno-red-zone")

# The stub: ucrt reads a surface's pixels, w and h and nothing else, and
# the real ugfx.h drags in the window protocol.
UGFX_STUB = """#ifndef UGFX_H
#define UGFX_H
#include <stdint.h>
struct ugfx_surface { uint32_t *pixels; int w, h; };
#endif
"""

# argv: in out scan glow vig curve mask src_lines period [bench_reps]
# in/out: "w h" then w*h little-endian uint32. With a curve, the map
# follows the output, w*h more words.
DRIVER = r"""
#include "ui/ucrt.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv) {
    if (argc < 10) return 2;
    FILE *f = fopen(argv[1], "rb");
    int w, h;
    if (!f || fread(&w, 4, 1, f) != 1 || fread(&h, 4, 1, f) != 1) return 3;
    uint32_t *src = malloc((size_t)w * h * 4), *dst = calloc((size_t)w * h, 4);
    if (fread(src, 4, (size_t)w * h, f) != (size_t)w * h) return 3;
    fclose(f);
    struct ucrt c;
    ucrt_init(&c);
    struct ucrt_look l = { atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), atoi(argv[6]),
                           atoi(argv[7]), 0, 0 };
    c.look = l;
    c.src_lines = atoi(argv[8]);
    c.period = atoi(argv[9]);
    struct ugfx_surface s = { dst, w, h };
    if (ucrt_apply_from(&c, src, w, &s, 0, 0, w, h) != 0) return 4;
    if (argc > 10) {
        int reps = atoi(argv[10]);
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (int i = 0; i < reps; i++) ucrt_apply_from(&c, src, w, &s, 0, 0, w, h);
        clock_gettime(CLOCK_MONOTONIC, &b);
        printf("%.2f\n", ((b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6) / reps);
        return 0;
    }
    f = fopen(argv[2], "wb");
    fwrite(dst, 4, (size_t)w * h, f);
    if (l.curve) fwrite(c.map, 4, (size_t)w * h, f);
    fclose(f);
    return 0;
}
"""

# ucrt.c's tables -- the model's half of the contract.
SCAN = [0, 40, 77, 115]
GLOW = [0, 90, 170, 230]
VIG = [0, 64, 115, 166]
MASK_DIM = 56
BEZEL = 0xFFFFFFFF


def f32(x):
    return struct.unpack("f", struct.pack("f", x))[0]


def vig_gain(i, n, v):
    """C's float arithmetic, rounded to float after every operation."""
    u = f32(f32(f32(f32(2.0 * i) + 1.0) / n) - 1.0)
    f = f32(u * u)
    return 256 - int(f32(f32(v * f) * f))


def chans(p):
    return [p >> 16 & 0xFF, p >> 8 & 0xFF, p & 0xFF]


def box(img, w, h, r, along_rows):
    inv = 65536 // (2 * r + 1)
    out = [[0, 0, 0] for _ in range(w * h)]
    n, lines = (w, h) if along_rows else (h, w)
    for line in range(lines):
        def at(i):
            i = min(max(i, 0), n - 1)
            return img[line * w + i] if along_rows else img[i * w + line]
        for i in range(n):
            s = [0, 0, 0]
            for j in range(i - r, i + r + 1):
                for k, ch in enumerate(at(j)):
                    s[k] += ch
            o = [x * inv >> 16 for x in s]
            if along_rows:
                out[line * w + i] = o
            else:
                out[i * w + line] = o
    return out


def model(src, w, h, look, src_lines, period):
    scan, glow, vig, curve, mask = look
    period = max(period, 2)
    gl = None
    if glow:
        hw, hh = (w + 1) // 2, (h + 1) // 2
        half = []
        for y in range(hh):
            for x in range(hw):
                x0, y0 = 2 * x, 2 * y
                x1, y1 = min(x0 + 1, w - 1), min(y0 + 1, h - 1)
                ps = [chans(src[yy * w + xx]) for yy, xx in ((y0, x0), (y0, x1), (y1, x0), (y1, x1))]
                half.append([sum(p[k] for p in ps) // 4 for k in range(3)])
        r = min(period // 2 + glow, 100)
        for _ in range(2):
            half = box(half, hw, hh, r, True)
            half = box(half, hw, hh, r, False)
        gl = [[c * GLOW[glow] >> 8 for c in p] for p in half]
    v = VIG[vig]
    col = [vig_gain(i, w, v) if v else 256 for i in range(w)]
    rows = []
    for r_ in range(h):
        g = 256
        if src_lines > 0:
            l0, l1, l2 = r_ * src_lines // h, (r_ + 1) * src_lines // h, (r_ + 2) * src_lines // h
            if l1 != l0:
                g -= SCAN[scan]
            elif h >= 4 * src_lines and l2 != l0:
                g -= SCAN[scan] // 2
        else:
            phase = r_ % period
            if phase == period - 1:
                g -= SCAN[scan]
            elif period >= 4 and phase == period - 2:
                g -= SCAN[scan] // 2
        vv = vig_gain(r_, h, v) if v else 256
        rows.append(g * vv // 256 * 256 // 256)
    out = []
    for r_ in range(h):
        shift = 1 if mask == 2 and (r_ // period) % 2 else 0
        for i in range(w):
            ch = chans(src[r_ * w + i])
            if gl:
                q = gl[(r_ // 2) * ((w + 1) // 2) + i // 2]
                ch = [c + g - (c * g >> 8) for c, g in zip(ch, q)]
            if mask:
                t = (i + shift) % 3
                ch = [c * (256 if k == t else 256 - MASK_DIM) >> 8 for k, c in enumerate(ch)]
            cg = col[i] * rows[r_] >> 8
            ch = [min(max(c * cg >> 8, 0), 255) for c in ch]
            out.append(ch)
    return out


def warp(composed, w, h, mapw):
    out = []
    for e in mapw:
        if e == BEZEL:
            out.append(None)
            continue
        idx, ax, ay = e >> 8, e >> 4 & 15, e & 15
        y0, x0 = divmod(idx, w)
        x1, y1 = min(x0 + 1, w - 1), min(y0 + 1, h - 1)
        p00, p01 = composed[y0 * w + x0], composed[y0 * w + x1]
        p10, p11 = composed[y1 * w + x0], composed[y1 * w + x1]
        out.append([((p00[k] * (16 - ax) + p01[k] * ax) * (16 - ay) +
                     (p10[k] * (16 - ax) + p11[k] * ax) * ay) >> 8 for k in range(3)])
    return out


def picture(w, h, seed):
    """Hard edges and flat runs, like text and DOOM: random blocks."""
    rnd = random.Random(seed)
    px = [0] * (w * h)
    for _ in range(w * h // 20):
        x, y = rnd.randrange(w), rnd.randrange(h)
        c = rnd.choice([0xFFFFFF, 0x000000, 0xDCDCCC, 0x232629, rnd.getrandbits(24)])
        for yy in range(y, min(y + rnd.randint(1, 4), h)):
            for xx in range(x, min(x + rnd.randint(1, 6), w)):
                px[yy * w + xx] = c
    return px


def run(exe, tmp, src, w, h, look, src_lines, period, reps=None):
    pin, pout = os.path.join(tmp, "in.bin"), os.path.join(tmp, "out.bin")
    with open(pin, "wb") as fh:
        fh.write(struct.pack("<ii", w, h) + struct.pack(f"<{w * h}I", *src))
    argv = [exe, pin, pout, *map(str, look), str(src_lines), str(period)]
    if reps:
        return float(subprocess.run(argv + [str(reps)], capture_output=True, text=True,
                                    check=True).stdout)
    subprocess.run(argv, check=True)
    with open(pout, "rb") as fh:
        data = fh.read()
    n = w * h
    out = list(struct.unpack(f"<{n}I", data[:4 * n]))
    mapw = list(struct.unpack(f"<{n}I", data[4 * n:8 * n])) if look[3] else None
    return out, mapw


LOOKS = {
    "subtle": (1, 1, 1, 0, 0), "classic": (2, 2, 2, 1, 0), "curved": (3, 3, 3, 2, 1),
    "slot": (3, 3, 3, 2, 2), "scan only": (3, 0, 0, 0, 0), "glow only": (0, 3, 0, 0, 0),
    "mask, flat": (0, 0, 0, 0, 1),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true",
                    help="drop the glow's padding-row copy between passes; must FAIL")
    ap.add_argument("--bench", action="store_true", help="time each preset, informational")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        hostcheck.write(tmp, "ui/ugfx.h", UGFX_STUB)
        hostcheck.stage(tmp, "userland/ui/ucrt.h", dest="ui/ucrt.h")
        src_c = hostcheck.stage(tmp, "userland/ui/ucrt.c", apply=args.positive_control,
                                tool="ucrt_hostcheck",
                                edits=[("        pad_row(c->half, hw, nh, hh);\n    }", "    }")])
        drv = hostcheck.write(tmp, "driver.c", DRIVER)
        exe = hostcheck.compile(tmp, "ucrt_host", [src_c, drv], flags=FLAGS,
                                includes=[tmp], tool="ucrt_hostcheck")

        if args.bench:
            for w, h in ((640, 480), (1440, 1080)):
                src = picture(w, h, 7) if w * h < 400000 else [0x808080 ^ i for i in range(w * h)]
                cols = []
                for name in ("subtle", "classic", "curved"):
                    ms = run(exe, tmp, src, w, h, LOOKS[name], 200, h // 200, reps=20)
                    cols.append(f"{name} {ms:.2f} ms")
                print(f"ucrt_hostcheck: {w}x{h}: " + ", ".join(cols))
            return 0

        fails, checks = [], 0
        cases = [(64, 48, 20, 3), (63, 47, 0, 3), (82, 50, 0, 4), (41, 37, 9, 2), (9, 13, 0, 2)]
        for w, h, src_lines, period in cases:
            src = picture(w, h, w * 1000 + h)
            for name, look in LOOKS.items():
                checks += 1
                got, mapw = run(exe, tmp, src, w, h, look, src_lines, period)
                want = model(src, w, h, look, src_lines, period)
                if look[3]:
                    want = warp(want, w, h, mapw)
                worst, where = 0, None
                for i, (g, m) in enumerate(zip(got, want)):
                    if m is None:
                        continue
                    d = max(abs(a - b) for a, b in zip(chans(g), m))
                    if d > worst:
                        worst, where = d, i
                if worst > 1:
                    fails.append(f"{w}x{h} {name}: off by {worst} at ({where % w}, {where // w})")
        if fails:
            for f in fails[:12]:
                print("ucrt_hostcheck: FAIL", f)
            print(f"ucrt_hostcheck: {len(fails)} of {checks} cases differ from the model")
            return 1
        print(f"ucrt_hostcheck: all {checks} cases within 1 of the model")
        return 0


if __name__ == "__main__":
    sys.exit(main())
