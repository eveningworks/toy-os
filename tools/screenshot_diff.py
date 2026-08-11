#!/usr/bin/env python3
"""tools/screenshot_diff.py -- pixel-diffs a new QMP screenshot against
a previously committed one, to catch rendering regressions that manual
eyeballing misses (a few shifted pixels on a redraw is easy to miss by
eye across dozens of screenshots in a session, especially anything
subtle like a 1px border offset or a wrong color).

Not a general image-diff tool -- built specifically for this project's
screenshots (fixed 1280x720 QEMU boot resolution, PNG output from
qmp_test.py's screenshot()). Requires Pillow, same as qmp_test.py.

Usage:
    python3 tools/screenshot_diff.py OLD.png NEW.png [--out diff.png] [--threshold 0.002]

Exit code 0 = images match within threshold (fraction of differing
pixels, default 0.2%). Exit code 1 = they differ more than that, or
dimensions don't match at all. Either way prints a one-line summary;
--out writes a diff-highlight PNG (differing pixels in magenta over a
dimmed copy of the new image) so a real regression is easy to spot at
a glance instead of having to alt-tab between two screenshots.

This is a plain pass/fail check, not a test framework -- pair it with
tools/gui_flow.py's screenshot_named() to build a small named-baseline
suite over time if that's ever worth doing (e.g. keep one
screenshots/baselines/<flow-name>.png per known-good state, diff a
fresh run against it before delivering a rendering-adjacent change).
"""

import argparse
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("old", help="baseline/previous screenshot")
    ap.add_argument("new", help="new screenshot to compare")
    ap.add_argument("--out", help="write a diff-highlight PNG here")
    ap.add_argument("--threshold", type=float, default=0.002,
                     help="max fraction of differing pixels to still count as a match (default 0.002 = 0.2%%)")
    args = ap.parse_args()

    try:
        from PIL import Image, ImageChops
    except ImportError as e:
        print("screenshot_diff: Pillow required -- pip install pillow --break-system-packages", file=sys.stderr)
        raise SystemExit(2) from e

    old_img = Image.open(args.old).convert("RGB")
    new_img = Image.open(args.new).convert("RGB")

    if old_img.size != new_img.size:
        print(f"screenshot_diff: FAIL -- size mismatch {old_img.size} vs {new_img.size}")
        return 1

    diff = ImageChops.difference(old_img, new_img)
    bbox = diff.getbbox()
    w, h = old_img.size
    total = w * h

    if bbox is None:
        differing = 0
    else:
        # Count pixels with any channel difference, not just the bbox area.
        diff_l = diff.convert("L")
        differing = sum(1 for px in diff_l.getdata() if px > 0)

    frac = differing / total if total else 0.0

    if args.out:
        highlighted = Image.blend(new_img, Image.new("RGB", new_img.size, (40, 40, 40)), 0.5)
        if bbox is not None:
            diff_l = diff.convert("L")
            mask_pixels = highlighted.load()
            diff_pixels = diff_l.load()
            for y in range(h):
                for x in range(w):
                    if diff_pixels[x, y] > 0:
                        mask_pixels[x, y] = (255, 0, 255)
        highlighted.save(args.out)

    status = "PASS" if frac <= args.threshold else "FAIL"
    print(f"screenshot_diff: {status} -- {differing}/{total} pixels differ ({frac:.4%}), "
          f"threshold {args.threshold:.4%}" + (f" -- diff written to {args.out}" if args.out else ""))
    return 0 if status == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
