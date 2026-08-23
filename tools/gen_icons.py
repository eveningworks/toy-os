#!/usr/bin/env python3
"""Draw the application icons into data/icons/*.qoi.

WHY THE REPO DRAWS ITS OWN. Same reason tools/gen_cursors.py and
gen_imgdata.py exist: every pixel this project ships should be one it
owns, and an icon set lifted from somewhere else brings a licence with
it. These are deliberately simple -- a rounded tile in a per-app hue and
a white pictogram -- which is also what reads at 20 pixels in a menu row.

WHY QOI AND NOT JPEG. An icon needs an ALPHA channel (it sits on a
wallpaper) and lossless edges (at 48px an icon is almost entirely edge,
which is what a DCT rings around). See userland/lib/uimg_qoi.c.

**PILLOW ENCODES THEM.** Nothing in this repo writes a QOI file, which
is what keeps userland/lib/uimg_qoi.c honest: the decoder is checked
against files a foreign implementation produced, so a misread chunk type
cannot round-trip through a matching bug of our own.

ONE MASTER SIZE, scaled at runtime. 64x64 is drawn here and the window
manager's icon cache resamples to whatever a site needs (48 on the
desktop, ~20 in a menu row) through uimg_scale()'s box filter. Real
systems keep per-size ART instead -- freedesktop has 16x16/, 32x32/,
48x48/ directories; Windows packs several sizes into one ICO -- because
a downscaled 48px icon loses its silhouette at 16px. That is a real
limit and it is accepted for now: these shapes are simple enough to
survive the reduction, and per-size directories are a change to the
LOOKUP, not to anything that would have to be undone.

    python3 tools/gen_icons.py            # rewrite data/icons/
    python3 tools/gen_icons.py --check    # fail if they are stale

Needs Pillow (see docs/tools.md's host-tools section).
"""
import argparse
import io
import os
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("gen_icons.py needs Pillow: pip install --user pillow")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICON_DIR = os.path.join(ROOT, "data", "icons")

SIZE = 64
PAD = 4                      # margin inside the 64px box
RADIUS = 12
WHITE = (255, 255, 255, 255)
INK = (255, 255, 255, 235)   # pictogram ink: white, a shade softer


def tile(colour):
    """The rounded plate every icon sits on, in that app's hue."""
    im = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([PAD, PAD, SIZE - PAD - 1, SIZE - PAD - 1],
                        radius=RADIUS, fill=colour + (255,))
    return im, d


# --- one function per icon --------------------------------------------
#
# Each draws a pictogram that says what the app IS, not what it is
# called: a page for the editor, a prompt for the terminal, a gear for
# settings. That is the difference between an icon and a letter tile, and
# it is the whole reason this file exists.

def icon_notepad():
    im, d = tile((66, 133, 244))
    d.rectangle([20, 14, 44, 50], fill=WHITE)
    for i, y in enumerate(range(21, 46, 6)):
        d.line([25, y, 39 if i % 2 else 35, y], fill=(120, 140, 170, 255), width=2)
    return im


def icon_terminal():
    im, d = tile((60, 64, 72))
    d.rectangle([14, 16, 50, 48], fill=(24, 26, 30, 255))
    d.line([20, 26, 26, 32], fill=(120, 230, 140, 255), width=3)
    d.line([26, 32, 20, 38], fill=(120, 230, 140, 255), width=3)
    d.line([30, 40, 42, 40], fill=(120, 230, 140, 255), width=3)
    return im


def icon_calculator():
    im, d = tile((235, 148, 42))
    d.rectangle([16, 12, 48, 24], fill=WHITE)
    for row in range(3):
        for col in range(3):
            x = 17 + col * 11
            y = 29 + row * 8
            d.rectangle([x, y, x + 7, y + 5], fill=INK)
    return im


def icon_settings():
    im, d = tile((120, 128, 140))
    # A gear: a thick ring with eight RADIAL teeth. Drawn as spokes
    # rather than as rotated rectangles, which PIL has no primitive for
    # -- a stroke from r=15 to r=23 is the same shape at this size.
    import math as _m
    for k in range(8):
        a = _m.pi * k / 4.0
        d.line([32 + 15 * _m.cos(a), 32 + 15 * _m.sin(a),
                32 + 23 * _m.cos(a), 32 + 23 * _m.sin(a)], fill=WHITE, width=7)
    d.ellipse([18, 18, 46, 46], outline=WHITE, width=6)
    d.ellipse([27, 27, 37, 37], fill=(120, 128, 140, 255))
    return im


def icon_taskmgr():
    im, d = tile((90, 160, 110))
    for i, h in enumerate((14, 24, 10, 30)):
        x = 15 + i * 10
        d.rectangle([x, 48 - h, x + 6, 48], fill=INK)
    return im


def icon_imgview():
    im, d = tile((150, 110, 200))
    d.rectangle([14, 18, 50, 46], fill=WHITE)
    d.ellipse([20, 23, 28, 31], fill=(250, 210, 120, 255))       # sun
    d.polygon([(17, 44), (29, 29), (38, 44)], fill=(110, 170, 130, 255))
    d.polygon([(31, 44), (41, 33), (48, 44)], fill=(80, 140, 110, 255))
    return im


def icon_about():
    im, d = tile((70, 150, 190))
    d.ellipse([16, 16, 48, 48], outline=WHITE, width=4)
    d.ellipse([30, 23, 34, 27], fill=WHITE)
    d.rectangle([30, 30, 34, 42], fill=WHITE)
    return im


def icon_shapes():
    im, d = tile((215, 95, 120))
    d.polygon([(15, 37), (26, 16), (37, 37)], fill=WHITE)
    d.ellipse([30, 31, 50, 51], outline=WHITE, width=5)
    return im


def icon_fontdemo():
    im, d = tile((180, 140, 90))
    # A letter A as three STROKES. The filled-polygon version read as a
    # tent at 20px, because its counter (the hole) closed up.
    d.line([23, 48, 32, 16], fill=WHITE, width=5)
    d.line([32, 16, 41, 48], fill=WHITE, width=5)
    d.line([26, 38, 38, 38], fill=WHITE, width=4)
    return im


def icon_uidemo():
    im, d = tile((80, 150, 170))
    d.rounded_rectangle([16, 18, 48, 28], radius=5, fill=(30, 60, 70, 255))
    d.ellipse([37, 19, 47, 27], fill=WHITE)                       # a switch, on
    d.line([18, 38, 46, 38], fill=(30, 60, 70, 255), width=4)      # a slider
    d.ellipse([26, 33, 36, 43], fill=WHITE)
    return im


def icon_start():
    """The Start button's mark -- NOT an app icon, and deliberately not
    shaped like one.

    Every other icon here is a pictogram of what an app IS. This one has
    no app behind it: it opens a menu of all of them. So it is an
    abstract mark -- four rounded quadrants around a gap, the "all your
    things" shape Windows' logo tile and KDE's launcher both settle on
    -- rather than a picture of anything, which would read as a shortcut
    to one particular program in the very menu it opens.

    It also has NO PLATE. The Start button draws its own background
    (uui_button_draw, in the taskbar's accent colour), so a plate here
    would be a second rounded rectangle inside the first.
    """
    im = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # A 2x2 grid of rounded squares -- GNOME's "show applications" mark
    # and the shape KDE's default launcher icon reduces to at small
    # sizes. Drawn nearly edge to edge because it has no plate to sit
    # inside: at the ~12px the taskbar scales it to, a 4px margin is a
    # third of the artwork.
    gap, cell, m = 5, 23, 6
    for cx in (m, m + cell + gap):
        for cy in (m, m + cell + gap):
            d.rounded_rectangle([cx, cy, cx + cell, cy + cell], radius=5, fill=INK)
    return im


ICONS = {
    "start": icon_start,
    "notepad": icon_notepad,
    "terminal": icon_terminal,
    "calculator": icon_calculator,
    "settings": icon_settings,
    "taskmgr": icon_taskmgr,
    "imgview": icon_imgview,
    "about": icon_about,
    "shapes": icon_shapes,
    "fontdemo": icon_fontdemo,
    "uidemo": icon_uidemo,
}

# Crash Test deliberately gets NO icon file. It is the one entry that
# exercises the letter-tile fallback on every boot -- the same trick
# data/fonts/ plays by shipping vera-mono with no bold companion, so the
# synthesized-bold path is run rather than merely written. Give it an
# icon and nothing on the image tests what happens when a file is
# missing.


def render(name):
    buf = io.BytesIO()
    ICONS[name]().save(buf, format="QOI")
    return buf.getvalue()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero if any file on disk is stale")
    args = ap.parse_args()

    os.makedirs(ICON_DIR, exist_ok=True)
    stale = []
    for name in sorted(ICONS):
        data = render(name)
        path = os.path.join(ICON_DIR, name + ".qoi")
        old = None
        if os.path.exists(path):
            with open(path, "rb") as f:
                old = f.read()
        if old == data:
            continue
        if args.check:
            stale.append(name)
            continue
        with open(path, "wb") as f:
            f.write(data)
        print("wrote %s (%d bytes)" % (os.path.relpath(path, ROOT), len(data)))

    if args.check:
        if stale:
            print("gen_icons: STALE -- %s" % ", ".join(stale))
            return 1
        print("gen_icons: ok -- data/icons/ matches this script")
    return 0


if __name__ == "__main__":
    sys.exit(main())
