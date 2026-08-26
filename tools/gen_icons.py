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


def icon_diskmark():
    # A PLATTER WITH A NEEDLE, not a bar chart. The disc says storage at
    # 20px where bars say "some app with numbers", and the sweep says
    # measurement -- which is the same pairing CrystalDiskMark, GNOME
    # Disks and macOS's Blackmagic test all reach for.
    im, d = tile((58, 122, 168))
    cx, cy = 32, 32
    d.ellipse([cx - 19, cy - 19, cx + 19, cy + 19], outline=WHITE, width=4)
    d.ellipse([cx - 5, cy - 5, cx + 5, cy + 5], fill=WHITE)
    # The needle, up and to the right: a reading part-way up its scale
    # rather than pinned at either end.
    d.line([cx, cy, cx + 13, cy - 13], fill=WHITE, width=4)
    return im


def icon_mines():
    # A MINE, not a flag. Both are the game's symbols, but a flag at
    # 20px is a wedge that could be anything, while a spiked ball is
    # unmistakable -- and it is what winmine.exe, KMines and gnome-mines
    # all put on the board itself.
    im, d = tile((190, 78, 70))
    cx, cy, r = 32, 32, 11
    for dx, dy in ((0, 1), (1, 0), (1, 1), (1, -1)):
        d.line([cx - dx * 17, cy - dy * 17, cx + dx * 17, cy + dy * 17],
               fill=WHITE, width=4)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=WHITE)
    # The highlight, in the tile's own hue rather than a fourth colour --
    # it is what stops the ball reading as a flat blob.
    d.ellipse([cx - 6, cy - 6, cx - 3, cy - 3], fill=(190, 78, 70, 255))
    return im


def icon_doom():
    # A HEALTH/ARMOR-STYLE CHEVRON, not a face and not a gun.
    #
    # The obvious pick is the marine's face from the status bar, and it
    # is the wrong one twice over: it is id's artwork rather than a
    # pictogram we drew, and at 20px on a taskbar a face becomes three
    # smudges. A firearm is worse -- it says "shooter" without saying
    # WHICH, and it is the one subject an icon set for a desktop should
    # not lean on.
    #
    # So: the downward chevron stack that every Doom HUD and box has
    # used since 1993, in the series' own red. Reads at 20px as a shape
    # rather than as a picture, which is the whole test.
    im, d = tile((166, 42, 38))
    for i, y in enumerate((16, 30, 44)):
        inset = 4 + i * 3
        d.polygon([(12 + inset, y), (32, y + 12), (52 - inset, y),
                   (32, y + 5)], fill=WHITE)
    return im


def icon_files():
    # A folder, which is what every file manager on every desktop uses:
    # a tab along the top of a body, drawn as two rectangles so the fold
    # reads at 20px as well as at 64. The second, offset folder behind it
    # is what says MANAGER rather than "a folder" -- Dolphin, Nautilus
    # and Explorer all carry the same doubling.
    im, d = tile((240, 190, 70))
    d.rounded_rectangle([12, 20, 44, 46], radius=3, fill=(200, 150, 40, 255))
    d.rectangle([16, 16, 30, 22], fill=WHITE)
    d.rounded_rectangle([18, 22, 52, 50], radius=3, fill=WHITE)
    d.rectangle([22, 18, 36, 25], fill=WHITE)
    d.line([24, 32, 46, 32], fill=(190, 160, 90, 255), width=2)
    d.line([24, 39, 40, 39], fill=(190, 160, 90, 255), width=2)
    return im


def icon_start():
    """The Start button's mark -- NOT an app icon, and deliberately not
    shaped like a UI convention either.

    Every other icon here is a pictogram of what an app IS. This one has
    no app behind it: it opens a menu of all of them. So it is an
    identity rather than a signpost -- three stacked toy bricks, which is
    what this OS is called.

    THE FIRST VERSION WAS A 2x2 OF ROUNDED PANES AND WAS WRONG: that is
    the Windows logo, near enough that the button read as somebody
    else's. A 3x3 dot grid (GNOME's "show applications") has the same
    problem one step removed -- it is a borrowed convention rather than
    a mark. Every distro solves this with a shape of its own (Debian's
    swirl, Fedora's f, Arch's mountain) and so does this.

    NO PLATE. The Start button draws its own background
    (uui_button_draw, in the taskbar's accent colour), so a plate here
    would be a second rounded rectangle inside the first.

    DRAWN AS SOLID BLOCKS WITH GAPS, not outlines: at the ~12px the
    taskbar scales it to, an outline's interior closes up and the whole
    mark becomes a smudge -- the same failure icon_fontdemo() records
    for a filled letter A.
    """
    im = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    w, h, gap = 22, 20, 5
    # Two on the bottom, one centred on top -- a stack, which is what
    # makes it read as bricks rather than as three rectangles.
    by = 34
    d.rounded_rectangle([6, by, 6 + w, by + h], radius=4, fill=INK)
    d.rounded_rectangle([6 + w + gap, by, 6 + w + gap + w, by + h], radius=4, fill=INK)
    ty = by - h - gap
    tx = 6 + (w + gap + w - w) // 2
    d.rounded_rectangle([tx, ty, tx + w, ty + h], radius=4, fill=INK)
    return im


# --- SETTINGS CATEGORIES ----------------------------------------------
#
# One per top-level category in System Settings' sidebar, named
# `cat-<something>` so they sort together in /usr/share/icons and cannot
# collide with an app's. Drawn flatter and simpler than an app icon on
# purpose: these are read at about a text row's height beside a bold
# label, not at 48px on a wallpaper, so a pictogram with any detail in
# it turns to mush. The tile hue carries most of the recognition at that
# size; the shape confirms it.

def icon_cat_time():
    im, d = tile((70, 130, 180))
    d.ellipse([14, 14, 50, 50], outline=WHITE, width=5)
    d.line([32, 32, 32, 20], fill=WHITE, width=5)   # hour hand
    d.line([32, 32, 42, 38], fill=WHITE, width=5)   # minute hand
    return im


def icon_cat_appearance():
    # A half-filled disc: the universal "contrast/theme" mark, and the
    # one shape that still reads when it is twelve pixels across.
    im, d = tile((150, 100, 190))
    d.ellipse([14, 14, 50, 50], outline=WHITE, width=5)
    d.pieslice([14, 14, 50, 50], 90, 270, fill=WHITE)
    return im


def icon_cat_input():
    # A keyboard: an outline with three key rows, the middle one broken
    # so it does not read as a filled block.
    im, d = tile((90, 160, 120))
    d.rounded_rectangle([10, 20, 54, 46], radius=4, outline=WHITE, width=4)
    for y in (27, 33):
        d.line([17, y, 47, y], fill=WHITE, width=3)
    d.line([24, 39, 40, 39], fill=WHITE, width=3)   # the space bar
    return im


def icon_cat_startup():
    # A power symbol -- a broken ring with a stem, which is what every
    # system has used for "boot" since the IEC standardised it.
    im, d = tile((210, 140, 70))
    d.arc([15, 15, 49, 49], start=300, end=240, fill=WHITE, width=5)
    d.line([32, 12, 32, 30], fill=WHITE, width=5)
    return im


def icon_cat_kernel():
    # A chip: a square die with legs on all four sides. The one category
    # that is about the machine rather than about the session.
    im, d = tile((110, 120, 135))
    d.rounded_rectangle([18, 18, 46, 46], radius=3, outline=WHITE, width=4)
    for t in (25, 32, 39):
        d.line([t, 10, t, 18], fill=WHITE, width=3)   # top
        d.line([t, 46, t, 54], fill=WHITE, width=3)   # bottom
        d.line([10, t, 18, t], fill=WHITE, width=3)   # left
        d.line([46, t, 54, t], fill=WHITE, width=3)   # right
    return im


ICONS = {
    "cat-time": icon_cat_time,
    "cat-appearance": icon_cat_appearance,
    "cat-input": icon_cat_input,
    "cat-startup": icon_cat_startup,
    "cat-kernel": icon_cat_kernel,
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
    "mines": icon_mines,
    "doom": icon_doom,
    "files": icon_files,
    "diskmark": icon_diskmark,
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
