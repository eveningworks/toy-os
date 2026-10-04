#!/usr/bin/env python3
"""Check userland/lib/uregion.c -- the compositor's visible-region
arithmetic -- with the host gcc, against a BITMAP: every operation is
replayed on a set of pixels in Python, and the region must cover exactly
those pixels.

What it asserts, over seeded random sequences of intersect/subtract and
over the shapes the compositor actually cuts (a stack of equal windows
with rounded-corner holes):

  - the rects are DISJOINT and non-empty
  - without `overflow`, the region is EXACTLY the bitmap
  - with `overflow`, it is a SUPERSET of the bitmap -- the side a clip may
    err on (uregion.h) -- and never a subset
  - the bbox and the area agree with the rects

and of uregion_diff(), a present's damage, over random edits to a pixel
buffer (odd widths included -- its loads are unaligned): every changed
pixel is covered, by at most `max` disjoint bands, nothing for equal
buffers, and each band tight to its changes whenever none were folded.

    python3 tools/uregion_hostcheck.py
    python3 tools/uregion_hostcheck.py --positive-control [subtract|diff]   # must FAIL

Needs only gcc and the standard library.
"""

import argparse
import random
import subprocess
import sys
import tempfile
import hostcheck  # noqa: E402

# stdin: one op per line -- "I x y w h" (init), "X x y w h" (intersect),
# "S x y w h" (subtract), "P" (print: "n overflow area bx by bw bh" then
# one "x y w h" per rect).
DRIVER = r"""
#include "uregion.h"
#include <stdint.h>
#include <stdio.h>

int main(void) {
    struct uregion g;
    uregion_init(&g);
    char op;
    int x, y, w, h;
    while (scanf(" %c", &op) == 1) {
        if (op == 'P') {
            struct urect b;
            uregion_bbox(&g, &b);
            printf("%d %d %lld %d %d %d %d\n", g.n, g.overflow, uregion_area(&g),
                   b.x, b.y, b.w, b.h);
            for (int i = 0; i < g.n; i++)
                printf("%d %d %d %d\n", g.r[i].x, g.r[i].y, g.r[i].w, g.r[i].h);
            fflush(stdout);
            continue;
        }
        if (op == 'D') {
            // D w h max gap k, then k "x y" pixels that differ.
            int mx, gap, k;
            if (scanf("%d %d %d %d %d", &w, &h, &mx, &gap, &k) != 5) return 2;
            static uint32_t a[64 * 64], b[64 * 64];
            for (int i = 0; i < w * h; i++) a[i] = b[i] = (uint32_t)i * 2654435761u;
            for (int i = 0; i < k; i++) {
                if (scanf("%d %d", &x, &y) != 2) return 2;
                b[y * w + x] ^= 0x00FF00FFu;
            }
            uregion_diff(&g, a, b, w, h, w, mx, gap);
            continue;
        }
        if (scanf("%d %d %d %d", &x, &y, &w, &h) != 4) return 2;
        if (op == 'I') uregion_init_rect(&g, x, y, w, h);
        else if (op == 'X') uregion_intersect_rect(&g, x, y, w, h);
        else if (op == 'S') uregion_subtract_rect(&g, x, y, w, h);
    }
    return 0;
}
"""


# The controls, one per half. `subtract` forgets the band BELOW a cut, so
# a subtraction from the middle of a rect loses pixels it must keep;
# `diff` stops widening a band rightwards, so a damage list misses the
# right-hand part of a change.
CONTROLS = {
    "subtract": [("if (ay1 > cy1) piece[np++]", "if (0) piece[np++]")],
    "diff": [("if (r + 1 > x1) x1 = r + 1;", "if (l + 1 > x1) x1 = l + 1;")],
}


def build(tmp, poison=None):
    hostcheck.stage(tmp, "userland/lib/uregion.h")
    src = hostcheck.stage(
        tmp, "userland/lib/uregion.c", apply=bool(poison), tool="uregion_hostcheck",
        edits=CONTROLS.get(poison, []))
    drv = hostcheck.write(tmp, "driver.c", DRIVER)
    return hostcheck.compile(tmp, "uregion_host", [src, drv], includes=[tmp],
                             tool="uregion_hostcheck")


def pixels(x, y, w, h):
    return {(i, j) for i in range(x, x + w) for j in range(y, y + h)} if w > 0 and h > 0 else set()


def run_case(exe, ops):
    """ops: list of (op, x, y, w, h). Returns (expected set, parsed reply)."""
    text = "".join(f"{o} {x} {y} {w} {h}\n" for o, x, y, w, h in ops) + "P\n"
    out = subprocess.run([exe], input=text, capture_output=True, text=True,
                         check=True).stdout.split("\n")
    n, ovf, area, bx, by, bw, bh = map(int, out[0].split())
    rects = [tuple(map(int, out[1 + i].split())) for i in range(n)]
    want = set()
    for o, x, y, w, h in ops:
        p = pixels(x, y, w, h)
        if o == "I":
            want = p
        elif o == "X":
            want &= p
        elif o == "S":
            want -= p
    return want, (ovf, area, (bx, by, bw, bh), rects)


def check(name, want, reply, fails):
    ovf, area, bbox, rects = reply
    got = set()
    for (x, y, w, h) in rects:
        if w <= 0 or h <= 0:
            fails.append(f"{name}: an empty rect {x, y, w, h} was stored")
            return
        p = pixels(x, y, w, h)
        if got & p:
            fails.append(f"{name}: rects overlap at {sorted(got & p)[0]}")
            return
        got |= p
    if area != len(got):
        fails.append(f"{name}: area {area} != {len(got)} covered pixels")
    if got:
        xs = [x for x, _ in got]
        ys = [y for _, y in got]
        b = (min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1)
        if bbox != b:
            fails.append(f"{name}: bbox {bbox} != {b}")
    if not ovf and got != want:
        miss, extra = want - got, got - want
        fails.append(f"{name}: not exact -- {len(miss)} missing (e.g. {sorted(miss)[:1]}), "
                     f"{len(extra)} extra (e.g. {sorted(extra)[:1]})")
    if ovf and not want <= got:
        fails.append(f"{name}: OVERFLOWED to a subset -- {len(want - got)} pixels lost")


def run_diff(exe, w, h, mx, gap, changed):
    text = f"D {w} {h} {mx} {gap} {len(changed)}\n" + "".join(
        f"{x} {y}\n" for x, y in changed) + "P\n"
    out = subprocess.run([exe], input=text, capture_output=True, text=True,
                         check=True).stdout.split("\n")
    n = int(out[0].split()[0])
    return [tuple(map(int, out[1 + i].split())) for i in range(n)]


def check_diff(name, rects, changed, mx, fails):
    """A present's damage: covers every changed pixel, at most `mx` disjoint
    bands, nothing when nothing changed, and -- when there was room -- each
    band's four edges touch a change (a band no larger than it must be)."""
    got = set()
    for (x, y, w, h) in rects:
        p = pixels(x, y, w, h)
        if not p or got & p:
            fails.append(f"{name}: band {x, y, w, h} is empty or overlaps another")
            return
        got |= p
    if len(rects) > mx:
        fails.append(f"{name}: {len(rects)} bands, more than the {mx} allowed")
    if not set(changed) <= got:
        miss = set(changed) - got
        fails.append(f"{name}: MISSED {len(miss)} changed pixel(s), e.g. {sorted(miss)[0]}")
    if not changed and rects:
        fails.append(f"{name}: damage reported for equal buffers")
    if len(rects) < mx:   # nothing was folded: every band is tight
        for (x, y, w, h) in rects:
            inside = [(i, j) for i, j in changed if x <= i < x + w and y <= j < y + h]
            if not ({j for _, j in inside} >= {y, y + h - 1} and
                    {i for i, _ in inside} >= {x, x + w - 1}):
                fails.append(f"{name}: band {x, y, w, h} is not tight to its changes")
                return


def window_stack(n, r):
    """What the compositor cuts: a window's padded rect minus `n` equal
    windows above it, each opaque but for r x r corner squares."""
    x, y, w, h, m = 10, 10, 40, 30, 4
    ops = [("I", x - m, y - m, w + 2 * m, h + 2 * m), ("X", 0, 0, 80, 60)]
    for k in range(n):
        ox, oy = x + k * 3, y + k * 2
        ops += [("S", ox + r, oy, w - 2 * r, h),
                ("S", ox, oy + r, r, h - 2 * r),
                ("S", ox + w - r, oy + r, r, h - 2 * r)]
    return ops


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", nargs="?", const="subtract",
                    choices=sorted(CONTROLS),
                    help="break subtraction (default) or the diff; the run must then fail")
    args = ap.parse_args()

    fails = []
    cases = 0
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp, poison=args.positive_control)
        rng = random.Random(1)
        for c in range(300):
            ops = [("I", rng.randint(-5, 20), rng.randint(-5, 20),
                    rng.randint(0, 50), rng.randint(0, 40))]
            for _ in range(rng.randint(1, 14)):
                op = "S" if rng.random() < 0.8 else "X"
                ops.append((op, rng.randint(-10, 50), rng.randint(-10, 45),
                            rng.randint(-2, 30), rng.randint(-2, 25)))
            want, reply = run_case(exe, ops)
            check(f"random #{c}", want, reply, fails)
            cases += 1
        # Many small holes: forces the bound, so the overflow path is
        # exercised and must still err large.
        ops = [("I", 0, 0, 70, 70)]
        for i in range(10):
            for j in range(10):
                ops.append(("S", 2 + i * 7, 2 + j * 7, 3, 3))
        want, reply = run_case(exe, ops)
        check("holes (overflow)", want, reply, fails)
        if not reply[0]:
            fails.append("holes: 100 holes did not overflow a 32-rect region -- "
                         "the overflow path went untested")
        cases += 1
        for n in (1, 2, 5, 14):
            for r in (0, 3, 6):
                want, reply = run_case(exe, window_stack(n, r))
                check(f"stack n={n} r={r}", want, reply, fails)
                cases += 1
        # uregion_diff: a caret's few pixels, scattered edits, a full
        # repaint, nothing at all, odd widths (unaligned 8-byte loads).
        for c in range(200):
            w, h = rng.choice([(64, 64), (63, 40), (1, 30), (17, 1), (5, 5)])
            k = rng.choice([0, 1, 2, 5, 30, w * h])
            changed = sorted({(rng.randrange(w), rng.randrange(h)) for _ in range(k)})
            mx, gap = rng.choice([(3, 16), (1, 1), (32, 1), (2, 4)])
            check_diff(f"diff #{c} {w}x{h} k={k} max={mx} gap={gap}",
                       run_diff(exe, w, h, mx, gap, changed), changed, mx, fails)
            cases += 1

    if args.positive_control:
        if fails:
            print(f"uregion_hostcheck: positive control FAILED as it must "
                  f"({len(fails)} finding(s), first: {fails[0]})")
            return 0
        print("uregion_hostcheck: positive control passed -- the harness "
              "cannot see a lost band", file=sys.stderr)
        return 1
    for f in fails[:20]:
        print("  FAIL  " + f)
    print(f"uregion_hostcheck: {cases} cases, {len(fails)} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
