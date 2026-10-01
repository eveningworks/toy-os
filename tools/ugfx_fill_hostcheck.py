#!/usr/bin/env python3
"""Check ugfx's anti-aliased fills against Pillow, on the HOST.

WHAT IS UNDER TEST. userland/ui/ugfx_fill.c: ugfx_fill_circle(),
_ellipse() and _polygon(), which blend their edge pixels by coverage for
every caller. This compiles the REAL file with the host gcc, with a stub
ugfx_blend_pixel() that records the alpha each pixel was given, and
compares that coverage map against an INDEPENDENT rasteriser: Pillow
drawing the same shape at 64x and box-reducing it.

THE CONVENTION BOTH SIDES SHARE is the one ugfx.h states: an integer
point is a pixel's CENTRE, and a circle reaches r + 1/2 from it -- so
the oracle draws at ((x + 0.5) * 16, ...). Anything else would compare
two different shapes.

WHAT IT ASSERTS, per shape:
  - a pixel deep inside is fully covered and one well outside untouched
    (the footprint is the aliased fill's);
  - the EDGE is graded: some pixels are partly covered -- an aliased fill
    has none, which is the property the change is for;
  - every pixel's coverage is within TOLERANCE of the oracle's, and the
    mean error over the edge is small.

--positive-control rounds every recorded alpha to 0 or 255 -- what an
aliased fill produces -- and requires the "graded edge" and coverage
checks to go RED.

Exit status is non-zero on any violation. Needs gcc and Pillow.
"""

import argparse
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W, H = 96, 96
TOLERANCE = 16        # per pixel, of 255 -- measured worst 8 (2026-10-01)
MEAN_TOLERANCE = 4.5   # measured worst 3.0

HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/ugfx.h"

#define W %(W)d
#define H %(H)d
static int g_alpha[H][W];
static int g_aliased;

void ugfx_blend_pixel(struct ugfx_surface *s, int x, int y, uint32_t c, uint8_t a) {
    (void)s; (void)c;
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    int v = g_aliased ? (a >= 128 ? 255 : 0) : a;
    if (v > g_alpha[y][x]) g_alpha[y][x] = v;
}

int main(int argc, char **argv) {
    static uint32_t px[W * H];
    struct ugfx_surface s;
    memset(&s, 0, sizeof s);
    s.pixels = px; s.w = W; s.h = H;
    g_aliased = argc > 1 && !strcmp(argv[1], "aliased");
    char kind[16];
    while (scanf("%%15s", kind) == 1) {
        memset(g_alpha, 0, sizeof g_alpha);
        if (!strcmp(kind, "circle")) {
            int cx, cy, r; scanf("%%d %%d %%d", &cx, &cy, &r);
            ugfx_fill_circle(&s, cx, cy, r, 0);
        } else if (!strcmp(kind, "ellipse")) {
            int cx, cy, rx, ry; scanf("%%d %%d %%d %%d", &cx, &cy, &rx, &ry);
            ugfx_fill_ellipse(&s, cx, cy, rx, ry, 0);
        } else {
            int n, xs[64], ys[64]; scanf("%%d", &n);
            for (int i = 0; i < n; i++) scanf("%%d %%d", &xs[i], &ys[i]);
            ugfx_fill_polygon(&s, xs, ys, n, 0);
        }
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) printf("%%d%%c", g_alpha[y][x], x == W - 1 ? '\n' : ' ');
    }
    return 0;
}
"""

SHAPES = [
    ("circle", (48, 48, 3)),
    ("circle", (48, 48, 12)),
    ("circle", (40, 50, 30)),
    ("ellipse", (48, 48, 40, 14)),
    ("polygon", [(10, 10), (80, 30), (20, 85)]),           # a triangle, all slopes
    ("polygon", [(48, 6), (90, 48), (48, 90), (6, 48)]),    # a diamond
    ("polygon", [(20, 70), (48, 20), (76, 70)]),            # a play glyph's shape, upright
]


def build(tmp):
    src = os.path.join(tmp, "harness.c")
    with open(src, "w") as f:
        f.write(HARNESS % {"W": W, "H": H})
    exe = os.path.join(tmp, "harness")
    cmd = ["gcc", "-O2", "-Wall", "-o", exe, src, os.path.join(ROOT, "userland", "ui", "ugfx_fill.c"),
           "-I" + os.path.join(ROOT, "userland"),
           # AFTER the system's: kernel/include/api has a string.h of its own.
           "-idirafter", os.path.join(ROOT, "kernel", "include", "api"),
           "-idirafter", os.path.join(ROOT, "kernel", "include", "abi")]
    subprocess.run(cmd, check=True)
    return exe


def oracle(kind, args):
    from PIL import Image, ImageDraw
    # 64x, not 16x: Pillow's fills INCLUDE their boundary pixels, which
    # grows every shape by half a sample -- at 16x that alone was a
    # systematic +8 on each edge pixel, read at first as this file's error.
    k = 64
    im = Image.new("L", (W * k, H * k), 0)
    d = ImageDraw.Draw(im)
    if kind in ("circle", "ellipse"):
        cx, cy = (args[0] + 0.5) * k, (args[1] + 0.5) * k
        rx = (args[2] + 0.5) * k
        ry = (args[3 if kind == "ellipse" else 2] + 0.5) * k
        d.ellipse([cx - rx, cy - ry, cx + rx, cy + ry], fill=255)
    else:
        d.polygon([((x + 0.5) * k, (y + 0.5) * k) for x, y in args], fill=255)
    small = im.reduce(k)
    return [[small.getpixel((x, y)) for x in range(W)] for y in range(H)]


def stdin_for(kind, args):
    if kind == "polygon":
        return "polygon %d %s\n" % (len(args), " ".join("%d %d" % p for p in args))
    return "%s %s\n" % (kind, " ".join(str(a) for a in args))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--positive-control", action="store_true")
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        feed = "".join(stdin_for(k, a) for k, a in SHAPES)
        out = subprocess.run([exe] + (["aliased"] if args.positive_control else []),
                             input=feed, capture_output=True, text=True, check=True).stdout
    rows = [list(map(int, line.split())) for line in out.splitlines()]
    fails = 0
    for i, (kind, a) in enumerate(SHAPES):
        got = rows[i * H:(i + 1) * H]
        want = oracle(kind, a)
        diffs = [abs(got[y][x] - want[y][x]) for y in range(H) for x in range(W)]
        edge = [d for y in range(H) for x in range(W) for d in [want[y][x]] if 0 < d < 255]
        worst = max(diffs)
        mean = sum(diffs) / max(1, len(edge))
        graded = sum(1 for y in range(H) for x in range(W) if 0 < got[y][x] < 255)
        name = f"{kind} {a}"
        checks = [
            ("inside is fully covered, outside untouched",
             all(got[y][x] == 255 for y in range(H) for x in range(W) if want[y][x] == 255 and
                 all(want[yy][xx] == 255 for yy in (y - 1, y, y + 1) for xx in (x - 1, x, x + 1)
                     if 0 <= yy < H and 0 <= xx < W))
             and all(got[y][x] == 0 for y in range(H) for x in range(W) if want[y][x] == 0 and
                     all(want[yy][xx] == 0 for yy in (y - 1, y, y + 1) for xx in (x - 1, x, x + 1)
                         if 0 <= yy < H and 0 <= xx < W)), ""),
            ("the edge is graded, not stepped", graded >= len(edge) // 3,
             f"{graded} partial pixels, oracle has {len(edge)}"),
            (f"coverage within {TOLERANCE} of Pillow's", worst <= TOLERANCE, f"worst {worst}"),
            (f"mean edge error under {MEAN_TOLERANCE}", mean <= MEAN_TOLERANCE, f"mean {mean:.1f}"),
        ]
        for label, ok, detail in checks:
            print(f"  {'PASS' if ok else 'FAIL'}  {name}: {label}  {detail}")
            fails += not ok
    print(f"\nugfx_fill_hostcheck: {fails} failure(s)")
    if args.positive_control:
        print("positive control:", "FIRED -- the checks see aliasing" if fails else
              "DID NOT FIRE -- an aliased fill passes")
        return 0 if fails else 1
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
