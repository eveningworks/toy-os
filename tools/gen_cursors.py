#!/usr/bin/env python3
"""Generate the cursor themes into data/cursors/<theme>/.

The Makefile stages data/cursors/ to /usr/share/cursors/ on the image.
A shape's file is a small key=value descriptor (shape, width, height,
hotspot) and then ONE of two bodies -- Xcursor's split between a mask
cursor and an ARGB one:

  * MASKS: two coverage grids, outline and fill, which the compositor
    colours at draw time. The format and the loader keep them; NO
    shipped theme uses them since Classic was redrawn (2026-10-01).
  * an IMAGE (every shipped theme): `image=` names a straight-alpha QOI
    beside the descriptor, and `image2=`/`image3=` the same shape
    rendered natively at 2x and 3x for the size setting. Its colours
    are its own; the compositor only composites it.

An image theme is a row of IMAGE_THEMES -- fill, rim and busy-ring
colours, and optionally a heavier rim and body -- over ONE supersampled
geometry, so every set has the same shapes and differs only in paint.
Pillow encodes the QOI files, as tools/gen_icons.py's do.

    python3 tools/gen_cursors.py            # write the themes
    python3 tools/gen_cursors.py --check    # fail if they are stale (CI)
"""

import argparse
import io
import math
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_ROOT = os.path.join(REPO, "data", "cursors")

SS = 8  # supersampling: drawn at 8x the target, box-reduced to it


def g_arrow(d, s):
    pts = [(0, 0), (0, 15.5), (3.9, 12.0), (6.6, 17.8), (9.0, 16.7), (6.4, 11.1), (11.2, 11.1)]
    d.polygon([((x + 2) * s, (y + 2) * s) for x, y in pts], fill=255)
    return (2, 2)


def g_ibeam(d, s):
    cx, top, bot = 5.5, 2.0, 19.0
    d.rounded_rectangle([(cx - 0.9) * s, top * s, (cx + 0.9) * s, bot * s], radius=0.6 * s, fill=255)
    for y in (top, bot - 1.8):
        d.rounded_rectangle([(cx - 3.4) * s, y * s, (cx + 3.4) * s, (y + 1.8) * s],
                            radius=0.9 * s, fill=255)
    return (5, 10)


def g_double_arrow(d, s, angle, cx=12.0, cy=12.0, L=18.0, H=6.0, hw=4.8, sw=1.3):
    half = L / 2
    pts = [(-half, 0), (-half + H, -hw), (-half + H, -sw), (half - H, -sw), (half - H, -hw),
           (half, 0), (half - H, hw), (half - H, sw), (-half + H, sw), (-half + H, hw)]
    ca, sa = math.cos(angle), math.sin(angle)
    d.polygon([((cx + x * ca - y * sa) * s, (cy + x * sa + y * ca) * s) for x, y in pts], fill=255)
    return (12, 12)


RING = (11.0, 11.0, 8.0)  # the busy ring's centre and outer radius, 1x


def g_ring(d, s, w=3.4):
    cx, cy, r = RING
    d.ellipse([(cx - r) * s, (cy - r) * s, (cx + r) * s, (cy + r) * s], fill=255)
    d.ellipse([(cx - r + w) * s, (cy - r + w) * s, (cx + r - w) * s, (cy + r - w) * s], fill=0)
    return (11, 11)


def g_hand(d, s):
    """A pointing hand: index finger up, three folded knuckles, a thumb."""
    ox, oy = 3.0, 1.5

    def R(x0, y0, x1, y1, r):
        d.rounded_rectangle([(ox + x0) * s, (oy + y0) * s, (ox + x1) * s, (oy + y1) * s],
                            radius=r * s, fill=255)
    R(4.6, 0.0, 7.8, 12.0, 1.6)     # index
    R(7.4, 6.4, 10.4, 12.6, 1.5)    # middle knuckle
    R(10.0, 7.2, 13.0, 13.2, 1.5)   # ring knuckle
    R(12.6, 8.2, 15.4, 13.8, 1.4)   # little knuckle
    R(4.6, 9.6, 15.4, 20.0, 3.2)    # palm
    d.polygon([((ox + x) * s, (oy + y) * s) for x, y in
               [(5.2, 11.2), (2.4, 8.4), (0.9, 8.2), (0.2, 9.6), (3.4, 15.8), (5.6, 17.2)]],
              fill=255)             # thumb
    return (9, 2)                   # the index fingertip


def g_move(d, s, c=12.0):
    for a in (0, math.pi / 2):
        g_double_arrow(d, s, a, cx=c, cy=c, L=20.0, H=5.5, hw=4.2, sw=1.2)
    return (12, 12)


def g_not_allowed(d, s, cx=11.0, cy=11.0, r=8.0, w=2.6):
    d.ellipse([(cx - r) * s, (cy - r) * s, (cx + r) * s, (cy + r) * s], fill=255)
    d.ellipse([(cx - r + w) * s, (cy - r + w) * s, (cx + r - w) * s, (cy + r - w) * s], fill=0)
    ca = sa = math.cos(math.pi / 4)
    hw, half = w / 2, r - w / 2
    pts = [(-half, -hw), (half, -hw), (half, hw), (-half, hw)]
    d.polygon([((cx + x * ca - y * sa) * s, (cy + x * sa + y * ca) * s) for x, y in pts], fill=255)
    return (11, 11)


GEOM = {
    "arrow": ((16, 23), g_arrow),
    "text": ((11, 22), g_ibeam),
    "wait": ((23, 23), g_ring),
    "resize-h": ((25, 25), lambda d, s: g_double_arrow(d, s, 0)),
    "resize-v": ((25, 25), lambda d, s: g_double_arrow(d, s, math.pi / 2)),
    "resize-diag": ((25, 25), lambda d, s: g_double_arrow(d, s, math.pi / 4, L=19.5)),
    "resize-diag2": ((25, 25), lambda d, s: g_double_arrow(d, s, -math.pi / 4, L=19.5)),
    "hand": ((22, 25), g_hand),
    "move": ((25, 25), g_move),
    "not-allowed": ((23, 23), g_not_allowed),
}


def dilate(m, px):
    """Grow a mask by `px` canvas pixels, rounded at the corners.

    Repeated 3x3 max rather than one wide kernel: a rank filter's cost
    is quadratic in its size, and a 3x shape's rim is a 60-pixel kernel.
    """
    for _ in range(max(1, int(round(px)))):
        m = m.filter(ImageFilter.MaxFilter(3))
    return m.filter(ImageFilter.GaussianBlur(px * 0.28)).point(lambda v: 255 if v > 110 else 0)


def geom_mask(name, k):
    """(supersampled mask, its canvas size at k, the 1x hotspot)."""
    (w, h), fn = GEOM[name]
    s = SS * k
    m = Image.new("L", (w * s, h * s), 0)
    hot = fn(ImageDraw.Draw(m), s)
    return m, (w * k, h * k), hot


def paint(mask, spec, size):
    """`mask` as alpha over a flat, vertical or diagonal fill of `size`."""
    w, h = size
    im = Image.new("RGB", size)
    px = im.load()
    for y in range(h):
        for x in range(w):
            if spec[0] == "flat":
                c = spec[1]
            else:
                t = y / max(1, h - 1) if spec[0] == "v" else (x + y) / max(1, w + h - 2)
                c = tuple(int(spec[1][i] + (spec[2][i] - spec[1][i]) * t) for i in range(3))
            px[x, y] = c
    im.putalpha(mask)
    return im


# An image theme: the fill, the rim, and the busy ring's track and
# 120-degree arc. Colours are the theme's OWN -- nothing repaints them.
WHITE = ("flat", (255, 255, 255))
DARK = ("flat", (20, 20, 22))
IMAGE_THEMES = {
    # Classic: white with a dark rim, the pointer every desktop started
    # from -- in the same shapes and shadow as the coloured sets.
    "default": dict(fill=WHITE, rim=DARK, track=WHITE, arc=("flat", (70, 70, 80))),
    # Classic bold: the same, heavier -- a thicker rim and a fuller body,
    # the accessibility reason to pick it over a bigger size. Rim plus
    # grow stays inside the 2 px margin every shape keeps on its canvas.
    "bold": dict(fill=WHITE, rim=DARK, track=WHITE, arc=("flat", (70, 70, 80)),
                 rim_px=1.6, grow=0.35),
    # Plasma/Adwaita/macOS: dark, white rim; the accent only in the ring.
    "graphite": dict(fill=("flat", (30, 30, 34)), rim=WHITE,
                     track=("flat", (70, 70, 78)), arc=("diag", (110, 160, 230), (70, 110, 160))),
    # Windows 11's pointer colour, in the theme's accent blue.
    "accent": dict(fill=("v", (104, 150, 214), (60, 100, 156)), rim=WHITE, arc=WHITE),
    # Bibata Modern Amber: a warm gradient, a dark rim.
    "amber": dict(fill=("v", (255, 200, 90), (236, 128, 0)), rim=("flat", (40, 26, 8)),
                  arc=("flat", (40, 26, 8))),
    # Teal to violet, from the default wallpaper.
    "aurora": dict(fill=("diag", (40, 200, 180), (120, 90, 240)), rim=WHITE,
                   arc=("diag", (255, 255, 255), (220, 230, 255))),
}
RIM_PX = 1.25     # the rim's width, at 1x
SHADOW_A = 0.38   # the drop shadow's peak opacity
SCALES = (1, 2, 3)


def render_image(theme, name, k):
    """One shape of an image theme at scale k, rendered natively."""
    spec = IMAGE_THEMES[theme]
    m, size, hot = geom_mask(name, k)
    s = SS * k
    if spec.get("grow"):
        m = dilate(m, spec["grow"] * s)
    rim = dilate(m, spec.get("rim_px", RIM_PX) * s)
    shadow = ImageChops.offset(rim, int(0.8 * s), int(1.4 * s))
    shadow = shadow.filter(ImageFilter.GaussianBlur(s * 1.3)).point(lambda v: int(v * SHADOW_A))
    down = lambda im: im.resize(size, Image.LANCZOS)  # noqa: E731
    out = Image.new("RGBA", size, (0, 0, 0, 0))
    out.alpha_composite(Image.merge("RGBA", (*[Image.new("L", size, 0)] * 3, down(shadow))))
    out.alpha_composite(paint(down(rim), spec["rim"], size))
    out.alpha_composite(paint(down(m), spec.get("track", spec["fill"]) if name == "wait"
                              else spec["fill"], size))
    if name == "wait":
        # a 120-degree arc from twelve o'clock, centred on the ring
        cx, cy, r = RING
        arc = Image.new("L", m.size, 0)
        R = (r + 2) * s
        ImageDraw.Draw(arc).pieslice([cx * s - R, cy * s - R, cx * s + R, cy * s + R],
                                     -90, 30, fill=255)
        out.alpha_composite(paint(down(ImageChops.multiply(m, arc)), spec["arc"], size))
    return out, hot


def qoi_bytes(im):
    buf = io.BytesIO()
    im.save(buf, format="QOI")
    return buf.getvalue()


def image_theme(theme):
    """{path relative to the theme dir: bytes} for every shape and scale."""
    out = {}
    for name in GEOM:
        (w, h), _ = GEOM[name]
        files = []
        hot = None
        for k in SCALES:
            im, hot = render_image(theme, name, k)
            fname = f"{name}.qoi" if k == 1 else f"{name}@{k}x.qoi"
            out[fname] = qoi_bytes(im)
            files.append((k, fname))
        lines = [
            "# toy-os cursor shape -- generated by tools/gen_cursors.py",
            "# An IMAGE shape: straight-alpha ARGB in the QOI files named",
            "# below, one per scale. Its colours are its own.",
            f"shape={name}",
            f"width={w}",
            f"height={h}",
            f"hotspot={hot[0]},{hot[1]}",
        ]
        for k, fname in files:
            lines.append(f"image{'' if k == 1 else k}={fname}")
        out[name] = ("\n".join(lines) + "\n").encode()
    return out


def theme_files(theme):
    """{filename: bytes} for one theme."""
    return image_theme(theme)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero if any file would change")
    args = ap.parse_args()

    stale, written = [], 0
    for theme in IMAGE_THEMES:
        d = os.path.join(OUT_ROOT, theme)
        if not args.check:
            os.makedirs(d, exist_ok=True)
        for name, body in theme_files(theme).items():
            path = os.path.join(d, name)
            old = open(path, "rb").read() if os.path.exists(path) else None
            if old == body:
                continue
            if args.check:
                stale.append(os.path.relpath(path, REPO))
            else:
                open(path, "wb").write(body)
                written += 1

    if args.check:
        if stale:
            print("gen_cursors: STALE -- re-run tools/gen_cursors.py:")
            for p in stale:
                print(f"  {p}")
            return 1
        print("gen_cursors: up to date")
        return 0

    print(f"gen_cursors: wrote {written} file(s) under "
          f"{os.path.relpath(OUT_ROOT, REPO)}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
