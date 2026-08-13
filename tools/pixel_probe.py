#!/usr/bin/env python3
"""Read exact pixel values out of a screenshot, and compare them across
screenshots.

`docs/gui-guidelines.md` requires GUI changes to be verified by pixel
value rather than by eye, and this is the tool for that. The rule exists
because a hover state that shifted a 235/255 background by TWO units
looked completely plausible in a PNG -- it was measured, not noticed,
and it would otherwise have shipped invisible.

The other half of the rule is checking a control that should NOT have
changed. `--compare` prints every sampled point across every image side
by side, so "this one moved, that one didn't" is one command.

    # one image, a couple of points
    python3 tools/pixel_probe.py shot.png 85,100 215,100

    # the same points across an interaction sequence -- the usual case
    python3 tools/pixel_probe.py --compare rest.png hover.png pressed.png \\
        --at 85,100 --at 215,100

    # average a small box instead of one pixel, for anti-aliased edges
    python3 tools/pixel_probe.py shot.png 85,100 --box 4

Exits non-zero if an image or a point is out of range, so it can be used
as a check and not just for reading.
"""

import argparse
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("pixel_probe: needs Pillow (pip install pillow)")


def parse_point(s):
    try:
        x, y = s.split(",")
        return int(x), int(y)
    except ValueError:
        raise argparse.ArgumentTypeError(f"expected X,Y (e.g. 85,100), got {s!r}")


def sample(im, x, y, box):
    """Average colour of the `box`x`box` square centred on (x, y).

    box=1 is a single pixel. A larger box is for anti-aliased edges and
    text, where one pixel is a coin toss between foreground and
    background and tells you nothing about the fill behind it.
    """
    if not (0 <= x < im.width and 0 <= y < im.height):
        sys.exit(f"pixel_probe: ({x},{y}) is outside {im.width}x{im.height}")
    if box <= 1:
        return im.getpixel((x, y))
    half = box // 2
    r = g = b = n = 0
    for yy in range(max(0, y - half), min(im.height, y + half + 1)):
        for xx in range(max(0, x - half), min(im.width, x + half + 1)):
            p = im.getpixel((xx, yy))
            r += p[0]; g += p[1]; b += p[2]; n += 1
    return (r // n, g // n, b // n)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("images", nargs="+", help="one or more PNG screenshots")
    ap.add_argument("points", nargs="*", type=parse_point, default=[],
                    help="X,Y points (single-image form)")
    ap.add_argument("--at", action="append", type=parse_point, default=[],
                    dest="at", help="X,Y point to sample in every image (repeatable)")
    ap.add_argument("--compare", action="store_true",
                    help="tabulate --at points across all images, and flag which moved")
    ap.add_argument("--box", type=int, default=1,
                    help="average a BOXxBOX square instead of one pixel (default 1)")
    args = ap.parse_args()

    points = args.at + args.points
    if not points:
        sys.exit("pixel_probe: no points given -- pass X,Y or --at X,Y")

    images = [(p, Image.open(p).convert("RGB")) for p in args.images]

    if not args.compare:
        for path, im in images:
            print(f"{path}  ({im.width}x{im.height})")
            for (x, y) in points:
                print(f"   ({x:>4},{y:>4}) = {sample(im, x, y, args.box)}")
        return 0

    name_w = max(len(p) for p, _ in images)
    header = " " * (name_w + 2) + "".join(f"{f'({x},{y})':>20}" for x, y in points)
    print(header)
    rows = []
    for path, im in images:
        vals = [sample(im, x, y, args.box) for x, y in points]
        rows.append(vals)
        print(f"{path:<{name_w}}  " + "".join(f"{str(v):>20}" for v in vals))

    # Which points actually moved across the sequence -- the assertion
    # most GUI checks are really making.
    print()
    for i, (x, y) in enumerate(points):
        seen = {r[i] for r in rows}
        verdict = "CHANGED" if len(seen) > 1 else "unchanged"
        print(f"   ({x},{y}): {verdict}  ({len(seen)} distinct value(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
