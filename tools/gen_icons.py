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

**PILLOW ENCODES THEM**, and that is what keeps userland/lib/uimg_qoi.c's
DECODER honest: these files come from a foreign implementation, so a
misread chunk type cannot round-trip through a matching bug of our own.
(The repo does write QOI now -- the screenshot tool does -- and its
encoder is kept honest the other way round, by Pillow decoding what it
wrote: tools/uimg_codec_hostcheck.py. Neither side is ever checked
against the other.)

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
import math
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


def icon_player():
    # A speaker cone with two waves -- what the app DOES, and it reads at
    # 16px where a musical note's stem does not.
    im, d = tile((90, 130, 185))
    d.rectangle([16, 26, 24, 38], fill=WHITE)                     # the box
    d.polygon([(24, 26), (34, 16), (34, 48), (24, 38)], fill=WHITE)  # the cone
    d.arc([32, 20, 46, 44], start=-60, end=60, fill=INK, width=3)
    d.arc([36, 14, 54, 50], start=-60, end=60, fill=INK, width=3)
    return im


def icon_about():
    im, d = tile((70, 150, 190))
    d.ellipse([16, 16, 48, 48], outline=WHITE, width=4)
    d.ellipse([30, 23, 34, 27], fill=WHITE)
    d.rectangle([30, 30, 34, 42], fill=WHITE)
    return im


def icon_help():
    # A QUESTION MARK AS STROKES, not a glyph: the icon set carries no
    # font, and an outlined '?' drawn as a polygon loses its counter at
    # menu-row size the way icon_fontdemo()'s A did.
    im, d = tile((90, 120, 200))
    d.arc([18, 12, 46, 40], start=160, end=20, fill=WHITE, width=5)
    d.line([32, 30, 32, 40], fill=WHITE, width=5)
    d.ellipse([29, 45, 35, 51], fill=WHITE)
    return im


def icon_logview():
    # RULED LINES OF DIFFERENT LENGTHS, with one marked: a log is text in
    # rows, and "one row stands out" is what the app is for. Equal-length
    # bars read as a menu or a list rather than as a log.
    im, d = tile((110, 125, 140))
    for i, (y, w) in enumerate(((16, 30), (24, 24), (32, 32), (40, 20), (48, 28))):
        colour = (250, 190, 90) if i == 3 else WHITE
        d.rectangle([14, y, 14 + w, y + 4], fill=colour)
    return im


def icon_screenshot():
    # A CAMERA BODY WITH A LENS, which is what a screenshot icon is
    # everywhere -- the alternative, a dashed rectangle "selection",
    # reads as a crop tool and is invisible at 20px once the dashes go.
    im, d = tile((70, 110, 160))
    d.rectangle([22, 15, 34, 21], fill=WHITE)              # the viewfinder hump
    d.rounded_rectangle([12, 20, 52, 48], radius=5, fill=WHITE)
    d.ellipse([24, 26, 40, 42], fill=(70, 110, 160, 255))
    d.ellipse([28, 30, 36, 38], fill=WHITE)
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



def icon_folder():
    # NOT an app icon: a plain folder glyph for the file manager's icons
    # view, drawn with no tile() plate -- a listing entry sits in a pane,
    # not on a wallpaper, and a plate would read as a button.
    im = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([6, 14, 34, 24], radius=4, fill=(230, 176, 60, 255))
    d.rounded_rectangle([6, 20, 58, 52], radius=4, fill=(240, 190, 70, 255))
    d.rounded_rectangle([6, 20, 58, 26], radius=2, fill=(214, 162, 52, 255))
    return im


def icon_file():
    # A page with a folded corner, the universal "some file" glyph --
    # per-type artwork is the icon THEME's future problem, not this
    # function's.
    im = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    grey = (140, 145, 155, 255)
    d.rounded_rectangle([14, 8, 50, 56], radius=3, fill=WHITE, outline=grey, width=2)
    d.polygon([(38, 8), (50, 20), (38, 20)], fill=(210, 214, 222, 255), outline=grey)
    for yy in (28, 35, 42):
        d.line([20, yy, 44, yy], fill=(170, 175, 185, 255), width=2)
    return im



# --- toolbar glyphs ----------------------------------------------------
#
# Dark ink, no plate: these sit on the toolbar's near-white chrome, the
# opposite situation from the desktop's white-on-wallpaper app icons.
# Bold strokes, because they are drawn at ~20px from this 64px master.

TB_INK = (55, 60, 72, 255)


def _tb():
    im = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    return im, ImageDraw.Draw(im)


def icon_tb_up():
    im, d = _tb()
    d.polygon([(32, 8), (54, 32), (40, 32), (40, 54), (24, 54), (24, 32), (10, 32)],
              fill=TB_INK)
    return im


# BACK AND FORWARD ARE THE SAME ARROW MIRRORED, drawn from one point
# list so the pair cannot drift apart -- a forward arrow a few pixels
# fatter than its back twin is the kind of thing nobody reports and
# everybody sees.
def _tb_arrow(points):
    im, d = _tb()
    d.polygon(points, fill=TB_INK)
    return im


def icon_tb_back():
    return _tb_arrow([(8, 32), (32, 10), (32, 24), (56, 24),
                      (56, 40), (32, 40), (32, 54)])


def icon_tb_forward():
    return _tb_arrow([(56, 32), (32, 10), (32, 24), (8, 24),
                      (8, 40), (32, 40), (32, 54)])


def icon_tb_home():
    # A house: roof, walls and a door cut out of them.
    im, d = _tb()
    d.polygon([(32, 6), (58, 30), (6, 30)], fill=TB_INK)
    d.rectangle([14, 28, 50, 56], fill=TB_INK)
    d.rectangle([27, 38, 37, 56], fill=(0, 0, 0, 0))
    return im


# --- file types, places and drives (the File Manager) ------------------
#
# The file-type glyphs are the generic page with an emblem, so a listing
# reads as "a page of some kind" first and "which kind" second -- the
# Breeze and Windows 11 convention. Places and drives are flat ink, no
# plate: they sit in a sidebar, not on a wallpaper.

PAGE_EDGE = (140, 145, 155, 255)
PLACE_INK = (91, 127, 191, 255)


def _page():
    im = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([14, 8, 50, 56], radius=3, fill=WHITE, outline=PAGE_EDGE, width=2)
    d.polygon([(38, 8), (50, 20), (38, 20)], fill=(210, 214, 222, 255), outline=PAGE_EDGE)
    return im, d


def icon_file_text():
    im, d = _page()
    for yy in (28, 35, 42, 49):
        d.line([20, yy, 44 if yy != 49 else 36, yy], fill=(150, 156, 168, 255), width=2)
    return im


def icon_file_doc():
    # Markdown: a document with a heading bar.
    im, d = _page()
    d.rectangle([20, 26, 40, 31], fill=(50, 90, 160, 255))
    for yy in (37, 43, 49):
        d.line([20, yy, 44, yy], fill=(150, 156, 168, 255), width=2)
    return im


def icon_file_image():
    im, d = _page()
    d.rectangle([19, 26, 45, 48], fill=(120, 170, 220, 255))
    d.polygon([(19, 48), (28, 36), (34, 43), (38, 39), (45, 48)], fill=(60, 140, 90, 255))
    d.ellipse([36, 29, 41, 34], fill=(250, 220, 90, 255))
    return im


def icon_file_audio():
    im, d = _page()
    ink = (192, 102, 28, 255)
    d.line([36, 26, 36, 45], fill=ink, width=4)
    d.line([36, 26, 44, 30], fill=ink, width=4)
    d.ellipse([25, 40, 37, 50], fill=ink)
    return im


def icon_file_config():
    # Settings: two slider tracks with their knobs.
    im, d = _page()
    ink = (63, 143, 138, 255)
    d.line([20, 32, 44, 32], fill=ink, width=3)
    d.line([20, 44, 44, 44], fill=ink, width=3)
    d.ellipse([24, 27, 34, 37], fill=WHITE, outline=ink, width=3)
    d.ellipse([32, 39, 42, 49], fill=WHITE, outline=ink, width=3)
    return im


def icon_file_font():
    im, d = _page()
    ink = (70, 70, 80, 255)
    d.line([22, 50, 32, 26], fill=ink, width=4)
    d.line([32, 26, 42, 50], fill=ink, width=4)
    d.line([26, 42, 38, 42], fill=ink, width=3)
    return im


def icon_file_app():
    # A launcher (.desktop): an app tile on the page.
    im, d = _page()
    d.rounded_rectangle([21, 28, 43, 50], radius=5, fill=(50, 90, 160, 255))
    d.rectangle([26, 37, 38, 41], fill=WHITE)
    return im


def _place():
    im = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    return im, ImageDraw.Draw(im)


def icon_place_home():
    im, d = _place()
    d.polygon([(32, 8), (58, 32), (6, 32)], fill=PLACE_INK)
    d.rectangle([14, 30, 50, 56], fill=PLACE_INK)
    d.rectangle([27, 40, 37, 56], fill=(0, 0, 0, 0))
    return im


def icon_place_desktop():
    im, d = _place()
    d.rounded_rectangle([6, 10, 58, 44], radius=3, fill=PLACE_INK)
    d.rectangle([12, 16, 52, 38], fill=(200, 215, 240, 255))
    d.rectangle([28, 44, 36, 52], fill=PLACE_INK)
    d.rectangle([18, 52, 46, 56], fill=PLACE_INK)
    return im


def icon_place_documents():
    im, d = _place()
    d.rounded_rectangle([14, 6, 50, 58], radius=3, fill=WHITE, outline=PLACE_INK, width=4)
    for yy in (22, 31, 40, 49):
        d.line([22, yy, 42, yy], fill=PLACE_INK, width=3)
    return im


def icon_place_music():
    im, d = _place()
    d.line([24, 46, 24, 12], fill=PLACE_INK, width=5)
    d.line([50, 40, 50, 8], fill=PLACE_INK, width=5)
    d.polygon([(22, 10), (52, 4), (52, 14), (22, 20)], fill=PLACE_INK)
    d.ellipse([10, 40, 26, 54], fill=PLACE_INK)
    d.ellipse([36, 34, 52, 48], fill=PLACE_INK)
    return im


def icon_place_pictures():
    im, d = _place()
    d.rounded_rectangle([6, 10, 58, 54], radius=3, fill=WHITE, outline=PLACE_INK, width=4)
    d.polygon([(10, 50), (26, 30), (36, 42), (44, 34), (54, 50)], fill=PLACE_INK)
    d.ellipse([40, 16, 48, 24], fill=PLACE_INK)
    return im


# --- coloured places and folder kinds (the File Manager, F1) -----------
#
# A PLACE IS A COLOURED TILE with its glyph in white: the glyph functions
# above are reused, their dark ink becoming white and their light fills
# letting the tile through, so a place reads by colour before shape.

def _tile(colour, glyph_fn):
    glyph = glyph_fn().convert("RGBA")
    im = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([2, 2, 61, 61], radius=14, fill=colour + (255,))
    small = glyph.resize((40, 40), Image.LANCZOS)
    white = Image.new("RGBA", small.size, (255, 255, 255, 0))
    px, wp = small.load(), white.load()
    for y in range(small.size[1]):
        for x in range(small.size[0]):
            r, g, b, a = px[x, y]
            ink = 255 - (r * 3 + g * 6 + b) // 10      # dark ink -> opaque white
            wp[x, y] = (255, 255, 255, a * ink // 255)
    im.alpha_composite(white, (12, 12))
    return im


def icon_place_home_tile():      return _tile((70, 110, 160), icon_place_home)
def icon_place_desktop_tile():   return _tile((47, 154, 146), icon_place_desktop)
def icon_place_documents_tile(): return _tile((74, 134, 214), icon_place_documents)
def icon_place_music_tile():     return _tile((214, 92, 143), icon_place_music)
def icon_place_pictures_tile():  return _tile((74, 163, 107), icon_place_pictures)


# A FOLDER OF A KNOWN KIND is the plain folder's shape in the kind's
# colour, with a white emblem on its front -- Breeze's folder-music and
# Explorer's special folders. lib/ufiletype.c maps names to these.
def _folder_kind(body, tab, emblem):
    im = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([6, 14, 34, 24], radius=4, fill=tab + (255,))
    d.rounded_rectangle([6, 20, 58, 52], radius=4, fill=body + (255,))
    emblem(d, (255, 255, 255, 255))
    return im


def _em_music(d, w):
    d.line([28, 46, 28, 28], fill=w, width=3)
    d.line([40, 43, 40, 25], fill=w, width=3)
    d.polygon([(27, 27), (41, 24), (41, 29), (27, 32)], fill=w)
    d.ellipse([22, 42, 30, 49], fill=w)
    d.ellipse([34, 39, 42, 46], fill=w)


def _em_pictures(d, w):
    d.polygon([(18, 47), (27, 35), (33, 42), (38, 37), (46, 47)], fill=w)
    d.ellipse([39, 27, 45, 33], fill=w)


def _em_documents(d, w):
    for yy in (31, 37, 43):
        d.line([20, yy, 44 if yy < 43 else 36, yy], fill=w, width=3)


def _em_fonts(d, w):
    d.line([24, 48, 32, 28], fill=w, width=4)
    d.line([32, 28, 40, 48], fill=w, width=4)
    d.line([27, 41, 37, 41], fill=w, width=3)


def _em_sounds(d, w):
    d.polygon([(22, 34), (27, 34), (34, 28), (34, 48), (27, 42), (22, 42)], fill=w)
    d.arc([32, 30, 44, 46], start=-60, end=60, fill=w, width=3)


def _em_icons(d, w):
    for (x, y) in ((22, 29), (33, 29), (22, 39), (33, 39)):
        d.rectangle([x, y, x + 8, y + 7], fill=w)


def _em_cursors(d, w):
    d.polygon([(26, 26), (26, 47), (31, 42), (35, 50), (38, 48), (34, 41), (41, 41)], fill=w)


def _em_terminal(d, w):
    d.line([22, 31, 28, 37, 22, 43], fill=w, width=3)
    d.line([31, 45, 42, 45], fill=w, width=3)


def _em_services(d, w):
    d.ellipse([26, 31, 38, 43], outline=w, width=3)
    for (x0, y0, x1, y1) in ((32, 25, 32, 29), (32, 45, 32, 49), (20, 37, 24, 37), (40, 37, 44, 37)):
        d.line([x0, y0, x1, y1], fill=w, width=3)


def _em_hwdata(d, w):
    d.rectangle([25, 30, 39, 44], outline=w, width=3)
    for x in (28, 32, 36):
        d.line([x, 26, x, 30], fill=w, width=2)
        d.line([x, 44, x, 48], fill=w, width=2)


def _em_soundfonts(d, w):
    d.rectangle([21, 30, 43, 46], outline=w, width=2)
    for x in (27, 32, 37):
        d.line([x, 30, x, 40], fill=w, width=3)


def _em_home(d, w):
    d.polygon([(32, 26), (44, 37), (20, 37)], fill=w)
    d.rectangle([24, 36, 40, 48], fill=w)


FOLDER_KINDS = {
    "folder-music":      ((214, 92, 143), (185, 70, 118), _em_music),
    "folder-pictures":   ((74, 163, 107), (55, 135, 84), _em_pictures),
    "folder-documents":  ((74, 134, 214), (58, 111, 184), _em_documents),
    "folder-fonts":      ((138, 99, 201), (113, 80, 173), _em_fonts),
    "folder-sounds":     ((224, 104, 79), (196, 83, 59), _em_sounds),
    "folder-icons":      ((224, 138, 60), (199, 116, 40), _em_icons),
    "folder-cursors":    ((107, 127, 179), (85, 104, 154), _em_cursors),
    "folder-terminal":   ((61, 67, 80), (43, 48, 57), _em_terminal),
    "folder-services":   ((125, 134, 150), (102, 111, 128), _em_services),
    "folder-hwdata":     ((47, 154, 146), (36, 125, 119), _em_hwdata),
    "folder-soundfonts": ((176, 92, 201), (149, 71, 174), _em_soundfonts),
    "folder-home":       ((70, 110, 160), (55, 88, 132), _em_home),
}


def icon_drive():
    # A drive seen from the front: a slab with its activity light.
    im, d = _place()
    d.rounded_rectangle([6, 20, 58, 46], radius=6, fill=(223, 227, 234, 255),
                        outline=(106, 116, 134, 255), width=3)
    d.ellipse([44, 30, 52, 38], fill=(58, 138, 74, 255))
    d.line([14, 34, 34, 34], fill=(106, 116, 134, 255), width=3)
    return im


def icon_drive_ram():
    # Memory: a chip with its legs, for a filesystem that lives in RAM.
    im, d = _place()
    ink = (106, 90, 150, 255)
    d.rounded_rectangle([14, 14, 50, 50], radius=4, fill=(231, 226, 243, 255), outline=ink, width=3)
    for x in (22, 32, 42):
        d.line([x, 6, x, 14], fill=ink, width=3)
        d.line([x, 50, x, 58], fill=ink, width=3)
    return im


def icon_tb_cut():
    im, d = _tb()
    d.ellipse([8, 38, 26, 56], outline=TB_INK, width=5)
    d.ellipse([38, 38, 56, 56], outline=TB_INK, width=5)
    d.line([22, 40, 46, 6], fill=TB_INK, width=5)
    d.line([42, 40, 18, 6], fill=TB_INK, width=5)
    return im


def icon_tb_paste():
    im, d = _tb()
    d.rounded_rectangle([12, 10, 52, 58], radius=4, outline=TB_INK, width=5)
    d.rounded_rectangle([22, 4, 42, 16], radius=3, fill=TB_INK)
    d.line([22, 32, 42, 32], fill=TB_INK, width=4)
    d.line([22, 42, 36, 42], fill=TB_INK, width=4)
    return im


def icon_tb_new():
    im, d = _tb()
    d.line([32, 8, 32, 56], fill=TB_INK, width=8)
    d.line([8, 32, 56, 32], fill=TB_INK, width=8)
    return im


def icon_tb_sort():
    im, d = _tb()
    d.line([16, 8, 16, 52], fill=TB_INK, width=5)
    d.polygon([(6, 42), (26, 42), (16, 56)], fill=TB_INK)
    for y, x1 in ((14, 58), (30, 52), (46, 44)):
        d.line([32, y, x1, y], fill=TB_INK, width=5)
    return im


def icon_tb_view():
    im, d = _tb()
    for x, y in ((8, 8), (36, 8), (8, 36), (36, 36)):
        d.rectangle([x, y, x + 20, y + 20], fill=TB_INK)
    return im


def icon_tb_pane():
    # The details pane: a window with its right-hand column.
    im, d = _tb()
    d.rectangle([6, 10, 58, 54], outline=TB_INK, width=5)
    d.rectangle([38, 10, 58, 54], fill=TB_INK)
    return im


def icon_tb_more():
    im, d = _tb()
    for x in (12, 32, 52):
        d.ellipse([x - 6, 26, x + 6, 38], fill=TB_INK)
    return im


def icon_tb_refresh():
    im, d = _tb()
    d.arc([10, 10, 54, 54], start=30, end=300, fill=TB_INK, width=8)
    d.polygon([(56, 24), (40, 28), (52, 42)], fill=TB_INK)
    return im


def icon_tb_details():
    im, d = _tb()
    for i, yy in enumerate((12, 27, 42)):
        d.rectangle([8, yy, 16, yy + 8], fill=TB_INK)
        d.rectangle([22, yy, 56, yy + 8], fill=TB_INK)
    return im


# Task Manager's rail: Performance is a trend over an axis, Services a
# gear -- the toolbar's ink, so the three rail rows read as one set.
def icon_tb_chart():
    im, d = _tb()
    d.line([(8, 8), (8, 56), (58, 56)], fill=TB_INK, width=6)
    d.line([(14, 44), (26, 28), (36, 38), (54, 14)], fill=TB_INK, width=7, joint="curve")
    return im


def icon_tb_gear():
    im, d = _tb()
    for k in range(8):
        a = k * math.pi / 4
        cx, cy = 32 + 22 * math.cos(a), 32 + 22 * math.sin(a)
        d.ellipse([cx - 6, cy - 6, cx + 6, cy + 6], fill=TB_INK)
    d.ellipse([12, 12, 52, 52], fill=TB_INK)
    d.ellipse([24, 24, 40, 40], fill=(0, 0, 0, 0))
    return im


def icon_tb_icons():
    im, d = _tb()
    for yy in (8, 34):
        for xx in (8, 34):
            d.rectangle([xx, yy, xx + 22, yy + 22], fill=TB_INK)
    return im


def icon_tb_panes():
    im, d = _tb()
    d.rectangle([6, 10, 58, 54], outline=TB_INK, width=6)
    d.rectangle([29, 10, 35, 54], fill=TB_INK)
    return im


def icon_tb_tree():
    im, d = _tb()
    d.rectangle([8, 8, 26, 20], fill=TB_INK)
    d.rectangle([28, 26, 46, 38], fill=TB_INK)
    d.rectangle([28, 44, 46, 56], fill=TB_INK)
    d.rectangle([14, 20, 20, 52], fill=TB_INK)
    d.rectangle([14, 29, 28, 35], fill=TB_INK)
    d.rectangle([14, 47, 28, 53], fill=TB_INK)
    return im


# The tray's volume item. Three states, because a speaker glyph with no
# waves reads as "no sound" and one with waves reads as "sound" -- which
# is the whole information a tray icon carries at a glance. Drawn in the
# toolbar ink, since the tray sits on the same panel the toolbars do.


def _speaker(d):
    """The cone and box, shared by all three states."""
    d.rectangle([10, 26, 20, 38], fill=TB_INK)
    d.polygon([(20, 26), (34, 12), (34, 52), (20, 38)], fill=TB_INK)


def icon_tray_volume_high():
    im, d = _tb()
    _speaker(d)
    d.arc([28, 16, 48, 48], start=300, end=60, fill=TB_INK, width=5)
    d.arc([32, 6, 60, 58], start=300, end=60, fill=TB_INK, width=5)
    return im


def icon_tray_volume_low():
    im, d = _tb()
    _speaker(d)
    d.arc([28, 16, 48, 48], start=300, end=60, fill=TB_INK, width=5)
    return im


def icon_tray_volume_muted():
    im, d = _tb()
    _speaker(d)
    d.line([(40, 22), (58, 42)], fill=TB_INK, width=5)
    d.line([(58, 22), (40, 42)], fill=TB_INK, width=5)
    return im


# The tray's brightness item: a sun, one state -- the level lives on
# the slider, and a sun that changed with it would be a second gauge.
def icon_tray_brightness():
    im, d = _tb()
    d.ellipse([20, 20, 44, 44], fill=TB_INK)
    for (x0, y0, x1, y1) in [(32, 4, 32, 12), (32, 52, 32, 60), (4, 32, 12, 32),
                             (52, 32, 60, 32), (12, 12, 18, 18), (46, 46, 52, 52),
                             (46, 18, 52, 12), (12, 52, 18, 46)]:
        d.line([(x0, y0), (x1, y1)], fill=TB_INK, width=5)
    return im


# The tray's remote-activity item: a monitor with a link leaving it, one
# state. It is shown ONLY while somebody is connected, so "nobody is on
# this machine" is said by the item's ABSENCE rather than by a second
# glyph -- which is how krfb and every screen-share indicator behave.
def icon_tray_remote():
    im, d = _tb()
    d.rectangle([12, 14, 52, 40], outline=TB_INK, width=5)
    d.rectangle([26, 44, 38, 50], fill=TB_INK)
    d.line([(18, 54), (46, 54)], fill=TB_INK, width=5)
    # The link: two arcs leaving the top-right corner, the universal
    # "something is reaching this" mark.
    d.arc([36, -2, 62, 24], start=180, end=270, fill=TB_INK, width=4)
    d.arc([42, 4, 56, 18], start=180, end=270, fill=TB_INK, width=4)
    return im


# The tray's on-screen-keyboard item: a key grid, one state. It toggles
# a panel rather than opening a flyout, so unlike the volume speaker
# there is no level for it to show.
def icon_tray_keyboard():
    im, d = _tb()
    d.rectangle([4, 16, 60, 48], outline=TB_INK, width=4)
    for row, (x0, n) in enumerate([(11, 6), (14, 5), (11, 6)]):
        y = 23 + row * 8
        for i in range(n):
            x = x0 + i * 7
            d.rectangle([x, y, x + 3, y + 3], fill=TB_INK)
    d.rectangle([22, 39, 42, 43], fill=TB_INK)   # the space bar
    return im


# The tray's network item, in three states. They differ by SHAPE only:
# a tray icon is blitted TINTED to the panel's own ink (wm_tray.c), so
# colour cannot carry state and a red "disconnected" is not available.
#
# A three-node graph rather than a plug or a globe: it stays legible at
# the ~26px a tray icon is actually drawn at, and it is the glyph
# Nautilus and Android already use for "network". A globe would be
# Windows' "connected but no internet", which is a claim nothing here
# can check -- there is no reachability probe.
NET_NODES = [(32, 17), (15, 47), (49, 47)]


def _net_nodes(d, lines=True, r=8):
    if lines:
        for (cx, cy) in NET_NODES[1:]:
            d.line([(32, 17), (cx, cy)], fill=TB_INK, width=6)
    for (cx, cy) in NET_NODES:
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=TB_INK)


# Has an address: the nodes, joined.
def icon_tray_network():
    im, d = _tb()
    _net_nodes(d, lines=True)
    return im


# A device, but nothing usable on it -- no address, a link-local one, or
# a link the driver KNOWS is down. The nodes are there and unjoined,
# which is the same "present but not connected" reading as a broken
# chain and needs no second glyph beside it.
def icon_tray_network_limited():
    im, d = _tb()
    _net_nodes(d, lines=False)
    return im


# No network device at all. Only reachable with `desktop.tray_network =
# always`, since `auto` hides the item outright in this case -- it
# exists so that pinning the item never shows a state it has no icon
# for.
def icon_tray_network_off():
    im, d = _tb()
    _net_nodes(d, lines=True)
    # CUT, then stroke: a transparent gap under the bar is what keeps it
    # readable as an overlay once the whole glyph is tinted one colour.
    d.line([(8, 56), (56, 8)], fill=(0, 0, 0, 0), width=14)
    d.line([(8, 56), (56, 8)], fill=TB_INK, width=6)
    return im


# The five file verbs. Copy/Move are a PAIR and read as one: two sheets
# for copy, one sheet plus an arrow for move -- which is what Explorer's
# ribbon and every commander's F5/F6 have always drawn.


def _sheet(d, x, y, w, h):
    d.rectangle([x, y, x + w, y + h], outline=TB_INK, width=4)


def icon_tb_copy():
    im, d = _tb()
    _sheet(d, 6, 6, 30, 38)
    _sheet(d, 26, 20, 30, 38)
    return im


def _folder_outline(d, x, y, w, h):
    # The body, then the tab on its top-left -- the folder every desktop
    # draws, as an outline so it matches the sheets beside it.
    tab_w, tab_h = w * 2 // 5, 6
    d.rounded_rectangle([x, y + tab_h, x + w, y + h], radius=3,
                        outline=TB_INK, width=4)
    d.rectangle([x, y, x + tab_w, y + tab_h + 2], fill=TB_INK)


def icon_tb_move():
    # A sheet leaving through the right edge: the arrow starts INSIDE
    # the sheet and crosses its border, which is what says "this one
    # goes there" rather than "a sheet, and separately an arrow".
    im, d = _tb()
    _sheet(d, 6, 8, 30, 44)
    d.rectangle([20, 27, 46, 35], fill=TB_INK)
    d.polygon([(60, 31), (44, 17), (44, 45)], fill=TB_INK)
    return im


def icon_tb_mkdir():
    # A folder with a plus in it -- freedesktop's folder-new.
    im, d = _tb()
    _folder_outline(d, 4, 12, 56, 42)
    d.rectangle([22, 34, 46, 40], fill=TB_INK)
    d.rectangle([31, 25, 37, 49], fill=TB_INK)
    return im


def icon_tb_rename():
    # A text field with the caret in it: rename is TYPING a name, and
    # an I-beam is the one glyph that means "edit this text" on every
    # desktop (edit-rename in Breeze, the F2 field in Explorer).
    im, d = _tb()
    d.rounded_rectangle([4, 18, 60, 46], radius=3, outline=TB_INK, width=4)
    d.rectangle([20, 26, 24, 38], fill=TB_INK)      # the caret's stem
    d.rectangle([15, 24, 29, 27], fill=TB_INK)      # ...and its serifs
    d.rectangle([15, 37, 29, 40], fill=TB_INK)
    d.rectangle([32, 30, 50, 34], fill=TB_INK)      # a line of text after it
    return im


def icon_tb_delete():
    im, d = _tb()
    d.rectangle([18, 6, 46, 14], fill=TB_INK)      # the lid
    d.rectangle([8, 16, 56, 24], fill=TB_INK)
    d.polygon([(14, 26), (50, 26), (46, 58), (18, 58)], outline=TB_INK, width=4)
    d.rectangle([26, 32, 32, 52], fill=TB_INK)
    d.rectangle([38, 32, 44, 52], fill=TB_INK)
    return im


# --- Image Viewer's command bar ---------------------------------------
#
# Drawn in TB_INK like the rest, and SYMBOLIC in use: the viewer tints
# each by its group (uui_toolbar_item.tint), so only the alpha matters.

def _tb_lens(d):
    d.ellipse([6, 6, 42, 42], outline=TB_INK, width=7)
    d.line([38, 38, 56, 56], fill=TB_INK, width=10)


def icon_tb_zoom_in():
    im, d = _tb()
    _tb_lens(d)
    d.rectangle([15, 21, 33, 27], fill=TB_INK)
    d.rectangle([21, 15, 27, 33], fill=TB_INK)
    return im


def icon_tb_zoom_out():
    im, d = _tb()
    _tb_lens(d)
    d.rectangle([15, 21, 33, 27], fill=TB_INK)
    return im


def icon_tb_fit():
    # Four corners pointing out: the picture fills the frame.
    im, d = _tb()
    def bar(x0, y0, x1, y1):
        d.rectangle([min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1)], fill=TB_INK)
    for (x, y, dx, dy) in ((6, 6, 1, 1), (58, 6, -1, 1), (6, 58, 1, -1), (58, 58, -1, -1)):
        bar(x, y, x + dx * 20, y + dy * 6)    # the corner's arm along x
        bar(x, y, x + dx * 6, y + dy * 20)    # ...and along y
    return im


def icon_tb_actual():
    # "1:1" -- two strokes and a colon, the label every viewer uses.
    im, d = _tb()
    for x in (8, 44):
        d.rectangle([x + 4, 12, x + 11, 52], fill=TB_INK)
        d.polygon([(x + 4, 12), (x + 11, 12), (x, 20), (x - 2, 16)], fill=TB_INK)
    d.rectangle([28, 22, 35, 29], fill=TB_INK)
    d.rectangle([28, 38, 35, 45], fill=TB_INK)
    return im


def _tb_rotate(mirror):
    im, d = _tb()
    d.arc([10, 12, 54, 56], start=200, end=500, fill=TB_INK, width=8)
    d.polygon([(4, 18), (24, 14), (14, 34)], fill=TB_INK)
    return im.transpose(Image.FLIP_LEFT_RIGHT) if mirror else im


def icon_tb_rotate_left():
    return _tb_rotate(False)


def icon_tb_rotate_right():
    return _tb_rotate(True)


def icon_tb_wallpaper():
    # A monitor on its stand, with a hill in it.
    im, d = _tb()
    d.rounded_rectangle([4, 8, 60, 46], radius=4, outline=TB_INK, width=6)
    d.polygon([(12, 40), (26, 24), (36, 34), (42, 28), (54, 40)], fill=TB_INK)
    d.rectangle([28, 46, 36, 54], fill=TB_INK)
    d.rectangle([18, 54, 46, 60], fill=TB_INK)
    return im


def icon_tb_slideshow():
    im, d = _tb()
    d.polygon([(16, 8), (56, 32), (16, 56)], fill=TB_INK)
    return im


def icon_tb_open():
    # A folder, its flap open: Open... in the Audio Player.
    im, d = _tb()
    d.polygon([(4, 14), (24, 14), (30, 20), (56, 20), (56, 52), (4, 52)], outline=TB_INK, width=5)
    d.polygon([(4, 52), (14, 30), (62, 30), (54, 52)], fill=TB_INK)
    return im


def icon_tb_shuffle():
    # Two crossing paths, each ending in an arrowhead.
    im, d = _tb()
    d.line([(4, 16), (18, 16), (42, 48), (52, 48)], fill=TB_INK, width=6, joint="curve")
    d.line([(4, 48), (18, 48), (42, 16), (52, 16)], fill=TB_INK, width=6, joint="curve")
    d.polygon([(50, 6), (62, 16), (50, 26)], fill=TB_INK)
    d.polygon([(50, 38), (62, 48), (50, 58)], fill=TB_INK)
    return im


def icon_tb_repeat():
    # A loop with an arrowhead on each run.
    im, d = _tb()
    d.line([(10, 36), (10, 18), (50, 18)], fill=TB_INK, width=6, joint="curve")
    d.line([(54, 28), (54, 46), (14, 46)], fill=TB_INK, width=6, joint="curve")
    d.polygon([(46, 8), (60, 18), (46, 28)], fill=TB_INK)
    d.polygon([(18, 36), (4, 46), (18, 56)], fill=TB_INK)
    return im


def icon_tb_playlist():
    # Lines of a list, the last one shorter, and a play mark.
    im, d = _tb()
    for y, x1 in ((12, 58), (26, 58), (40, 30)):
        d.line([6, y, x1, y], fill=TB_INK, width=6)
    d.polygon([(38, 34), (60, 46), (38, 58)], fill=TB_INK)
    return im


def icon_tb_fullscreen():
    # Four corners pointing out.
    im, d = _tb()
    for (x, y, dx, dy) in ((6, 6, 1, 1), (58, 6, -1, 1), (6, 58, 1, -1), (58, 58, -1, -1)):
        d.line([x, y, x + 18 * dx, y], fill=TB_INK, width=6)
        d.line([x, y, x, y + 18 * dy], fill=TB_INK, width=6)
    return im


def icon_tb_info():
    im, d = _tb()
    d.ellipse([4, 4, 60, 60], outline=TB_INK, width=6)
    d.rectangle([28, 27, 36, 48], fill=TB_INK)
    d.ellipse([27, 14, 37, 23], fill=TB_INK)
    return im


def icon_properties():
    """The Properties window's own icon: a sheet with an information
    mark. Not a `tb-` glyph -- it names a WINDOW, so it is drawn like
    the other app icons, on a plate."""
    im, d = tile((92, 104, 126))
    d.rectangle([16, 12, 44, 52], fill=(250, 250, 252, 255))
    d.ellipse([26, 18, 34, 26], fill=(92, 104, 126, 255))
    d.rectangle([27, 30, 33, 46], fill=(92, 104, 126, 255))
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
    brick_stack(ImageDraw.Draw(im), w=22, h=20, gap=5, bottom_y=34, left_x=6)
    return im


def brick_stack(d, w, h, gap, bottom_y, left_x):
    """Three bricks: two on the bottom, one centred on top.

    ONE IMPLEMENTATION, because two marks draw it -- the Start button
    (`icon_start`, inked and unplated, recoloured by the panel) and the
    OS's own logo (`icon_toyos`, on a plate). They are the same identity
    at two sizes and must not drift into two similar-but-different
    shapes, which is exactly what a second copy of these six lines
    would eventually become.

    The stack is what makes it read as bricks rather than as three
    rectangles, and the gaps are what keep it legible when the taskbar
    scales it to ~12px -- see icon_start's own note on why this is solid
    blocks and not outlines.
    """
    d.rounded_rectangle([left_x, bottom_y, left_x + w, bottom_y + h],
                        radius=4, fill=INK)
    d.rounded_rectangle([left_x + w + gap, bottom_y,
                         left_x + w + gap + w, bottom_y + h], radius=4, fill=INK)
    ty = bottom_y - h - gap
    tx = left_x + (w + gap) // 2
    d.rounded_rectangle([tx, ty, tx + w, ty + h], radius=4, fill=INK)


def icon_toyos():
    """toy-os's own logo: the Start button's mark, on a plate.

    The identity already existed -- `icon_start` chose three stacked
    bricks over a 2x2 of panes (Windows) and a 3x3 dot grid (GNOME), and
    the reasoning is in its docstring. A logo drawing something ELSE
    would give the project two marks, so this is the same shape with the
    one thing the Start button deliberately has not got: a plate. The
    button paints its own background in the taskbar's accent, so a plate
    there would be a rectangle inside a rectangle; here there is nothing
    behind it.

    Sized in from icon_start's geometry rather than reusing it whole:
    the plate's own PAD means the button's 22px bricks touch the
    rounding. Verified at 20px, which is what a menu row gets.
    """
    im, d = tile((40, 96, 230))
    brick_stack(d, w=19, h=17, gap=5, bottom_y=36, left_x=9)
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


def icon_cat_shortcuts():
    # A single keycap with a chevron on it -- a KEY being pressed, which
    # is what a shortcut is. Deliberately NOT another full keyboard:
    # cat-input already is one, and two keyboards in the same sidebar
    # would be two rows nobody can tell apart at 20px.
    im, d = tile((120, 135, 185))
    d.rounded_rectangle([16, 16, 48, 48], radius=5, outline=WHITE, width=4)
    # The chevron, pointing down-right the way a "press" arrow does.
    d.line([25, 27, 32, 34], fill=WHITE, width=4)
    d.line([32, 34, 25, 41], fill=WHITE, width=4)
    d.line([36, 41, 42, 41], fill=WHITE, width=4)
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


def icon_cat_display():
    # A monitor: a screen on a stand. The category is about the panel
    # itself -- brightness, resolution, scaling -- not about what is
    # drawn on it, which is Appearance.
    im, d = tile((70, 130, 180))
    d.rounded_rectangle([12, 16, 52, 42], radius=3, outline=WHITE, width=4)
    d.line([32, 42, 32, 50], fill=WHITE, width=4)      # stand
    d.line([22, 51, 42, 51], fill=WHITE, width=4)      # foot
    return im


def icon_cat_storage():
    # A disk platter seen edge-on: the stacked-cylinder shape every file
    # manager and every OS has used for a drive since the 1980s.
    im, d = tile((120, 125, 140))
    d.ellipse([12, 12, 52, 24], outline=WHITE, width=4)   # top rim
    d.line([12, 18, 12, 44], fill=WHITE, width=4)         # left wall
    d.line([52, 18, 52, 44], fill=WHITE, width=4)         # right wall
    d.arc([12, 32, 52, 44], 0, 180, fill=WHITE, width=4)  # middle band
    d.arc([12, 40, 52, 52], 0, 180, fill=WHITE, width=4)  # bottom
    return im


def icon_cat_system():
    # A tower: the machine itself, which is what this category is about
    # (the shell it runs, the machine's own configuration) rather than
    # the session on it. A TOWER and not a monitor, because Display is
    # already a monitor and two boxes would be one too many; the power
    # dot and the drive slots are what separate them at a glance.
    im, d = tile((100, 110, 125))
    d.rounded_rectangle([20, 10, 44, 54], radius=3, outline=WHITE, width=4)
    d.ellipse([28, 16, 36, 24], outline=WHITE, width=3)   # power button
    d.line([26, 32, 38, 32], fill=WHITE, width=3)         # drive slot
    d.line([26, 40, 38, 40], fill=WHITE, width=3)         # drive slot
    return im


def icon_devmgr():
    # An expansion card: a board, a chip on it, and the gold fingers
    # along its edge -- the DEVICE rather than the machine (System is a
    # tower, Kernel a bare chip).
    im, d = tile((70, 110, 160))
    d.rounded_rectangle([10, 14, 54, 42], radius=3, outline=WHITE, width=4)
    d.rectangle([18, 21, 31, 34], fill=WHITE)            # the chip
    d.line([36, 24, 46, 24], fill=WHITE, width=3)         # traces
    d.line([36, 31, 46, 31], fill=WHITE, width=3)
    for x in range(16, 50, 6):
        d.line([x, 44, x, 51], fill=WHITE, width=3)       # the fingers
    return im


def icon_sysupdate():
    # A circular arrow around a down-arrow: "fetch, and refresh" -- the
    # shape Windows Update and KDE Discover both use for the job.
    im, d = tile((60, 130, 170))
    d.arc([12, 12, 52, 52], start=200, end=500, fill=WHITE, width=5)
    d.polygon([(44, 8), (52, 22), (38, 22)], fill=WHITE)   # the arc's arrowhead
    d.line([32, 22, 32, 38], fill=WHITE, width=5)
    d.polygon([(24, 36), (40, 36), (32, 45)], fill=WHITE)
    return im


def icon_dev_usb():
    # The USB trident: a stem with an arrowhead, a branch ending in a
    # circle and one ending in a square, on a base dot.
    im, d = tile((95, 120, 140))
    d.line([32, 18, 32, 48], fill=WHITE, width=4)
    d.polygon([(32, 9), (25, 19), (39, 19)], fill=WHITE)             # arrowhead
    d.line([32, 38, 20, 30], fill=WHITE, width=4)
    d.line([20, 30, 20, 25], fill=WHITE, width=4)
    d.ellipse([15, 19, 25, 29], fill=WHITE)                          # circle end
    d.line([32, 42, 44, 34], fill=WHITE, width=4)
    d.line([44, 34, 44, 28], fill=WHITE, width=4)
    d.rectangle([39, 22, 49, 30], fill=WHITE)                        # square end
    d.ellipse([27, 46, 37, 56], fill=WHITE)                          # base
    return im


def icon_badge_warning():
    # A STATUS BADGE, not an app icon: drawn over another icon's corner at
    # two-thirds of the text height (uui_tree's `badge`), so it fills the
    # whole box and the "!" is thick enough to survive ~9 pixels. Windows
    # Device Manager's yellow triangle; the dark rim keeps it readable on
    # any icon under it.
    im = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.polygon([(32, 2), (62, 60), (2, 60)], fill=(110, 80, 0, 255))
    d.polygon([(32, 11), (55, 55), (9, 55)], fill=(242, 180, 0, 255))
    d.rectangle([28, 24, 36, 42], fill=(30, 30, 34, 255))
    d.rectangle([28, 46, 36, 52], fill=(30, 30, 34, 255))
    return im


def icon_badge_disabled():
    # Windows' "disabled" badge: a light disc with a dark down-arrow.
    im = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse([2, 2, 61, 61], fill=(70, 70, 80, 255))
    d.ellipse([8, 8, 55, 55], fill=(250, 250, 250, 255))
    d.polygon([(16, 24), (48, 24), (32, 46)], fill=(30, 30, 34, 255))
    return im


def icon_cat_favourites():
    # A STAR, which is what a pinned thing is called everywhere. Drawn
    # as a polygon rather than a glyph so it stays sharp at the folder
    # size the Start menu asks for, which is smaller than an app icon.
    im, d = tile((240, 186, 60))
    pts = []
    import math
    for i in range(10):
        r = 22 if i % 2 == 0 else 9
        a = -math.pi / 2 + i * math.pi / 5
        pts.append((32 + r * math.cos(a), 32 + r * math.sin(a)))
    d.polygon(pts, fill=WHITE)
    return im


def icon_cat_recent():
    # A CLOCK, and deliberately not an hourglass: this folder is
    # ordered by WHEN, and an hourglass means "wait" in every toolkit
    # including this one's wait cursor.
    im, d = tile((120, 140, 170))
    d.ellipse([14, 14, 50, 50], outline=WHITE, width=4)
    d.line([32, 32, 32, 21], fill=WHITE, width=4)   # hour hand
    d.line([32, 32, 41, 36], fill=WHITE, width=4)   # minute hand
    return im


def icon_cat_all():
    # A GRID: every app, which is what the folder holds. Nine squares
    # rather than four, because four reads as a window layout.
    im, d = tile((110, 120, 135))
    for row in range(3):
        for col in range(3):
            x = 14 + col * 13
            y = 14 + row * 13
            d.rounded_rectangle([x, y, x + 8, y + 8], radius=2, fill=WHITE)
    return im


def icon_cat_utility():
    # A WRENCH at an angle -- the small tools category. The handle is a
    # thick line and the head an open ring, which survives being drawn
    # at half an app icon's size where a detailed spanner would not.
    im, d = tile((90, 150, 120))
    d.line([22, 42, 44, 20], fill=WHITE, width=7)
    d.ellipse([16, 36, 30, 50], outline=WHITE, width=5)
    return im


def icon_cat_graphics():
    # A PICTURE: a frame with a hill and a sun, the same pictogram the
    # Image Viewer's own icon uses -- a category and the app most in it
    # sharing a visual language is a feature.
    im, d = tile((200, 120, 190))
    d.rounded_rectangle([14, 16, 50, 48], radius=3, outline=WHITE, width=4)
    d.polygon([(20, 44), (30, 30), (40, 44)], fill=WHITE)
    d.ellipse([36, 22, 44, 30], fill=WHITE)
    return im


def icon_cat_multimedia():
    # A PLAY TRIANGLE, which means media everywhere and nothing else
    # anywhere.
    im, d = tile((220, 110, 90))
    d.polygon([(24, 16), (48, 32), (24, 48)], fill=WHITE)
    return im


def icon_cat_games():
    # A GAMEPAD, reduced to what survives at this size: a rounded body,
    # a d-pad cross and two buttons.
    im, d = tile((110, 160, 90))
    d.rounded_rectangle([12, 22, 52, 44], radius=10, fill=WHITE)
    d.line([20, 33, 30, 33], fill=(110, 160, 90, 255), width=4)
    d.line([25, 28, 25, 38], fill=(110, 160, 90, 255), width=4)
    d.ellipse([38, 28, 44, 34], fill=(110, 160, 90, 255))
    d.ellipse([44, 34, 50, 40], fill=(110, 160, 90, 255))
    return im


def icon_cat_development():
    # ANGLE BRACKETS, the universal "this is code" mark -- and the one
    # pictogram here that is literally two characters, which is why it
    # is drawn as lines rather than set as text: the interface face is
    # not guaranteed to have a weight that reads at 20 pixels.
    im, d = tile((130, 120, 200))
    d.line([26, 22, 16, 32], fill=WHITE, width=5)
    d.line([16, 32, 26, 42], fill=WHITE, width=5)
    d.line([38, 22, 48, 32], fill=WHITE, width=5)
    d.line([48, 32, 38, 42], fill=WHITE, width=5)
    return im


def icon_cat_desktop():
    # A desktop with a panel along the bottom and two icons on it: the
    # furniture of the session, which is what this category holds
    # (wallpaper, icon size, taskbar, tray, screensaver). Distinct from
    # Display's monitor, which is about the panel the pixels land on.
    im, d = tile((80, 140, 150))
    d.rounded_rectangle([10, 12, 54, 50], radius=3, outline=WHITE, width=4)
    d.line([12, 42, 52, 42], fill=WHITE, width=3)        # the panel
    d.rectangle([17, 20, 25, 28], outline=WHITE, width=3)  # an icon
    d.rectangle([17, 31, 25, 38], outline=WHITE, width=3)  # another
    return im


def icon_cat_network():
    # Three nodes on a stem: one above, two below. The shape every
    # settings panel uses for a network -- a connection between things
    # rather than a cable or a globe, neither of which is what this
    # category is about (an address, a name, a log).
    im, d = tile((70, 120, 165))
    d.line([32, 20, 32, 32], fill=WHITE, width=4)        # stem down
    d.line([16, 32, 48, 32], fill=WHITE, width=4)        # crossbar
    d.line([16, 32, 16, 40], fill=WHITE, width=4)        # left drop
    d.line([48, 32, 48, 40], fill=WHITE, width=4)        # right drop
    d.ellipse([26, 10, 38, 22], outline=WHITE, fill=WHITE)   # top node
    d.ellipse([10, 40, 22, 52], outline=WHITE, fill=WHITE)   # left node
    d.ellipse([42, 40, 54, 52], outline=WHITE, fill=WHITE)   # right node
    return im


def icon_cat_sound():
    # A speaker with one arc. Deliberately ONE arc, not the tray
    # volume icon's three: this is the category heading, and it should
    # not read as a live volume level.
    im, d = tile((90, 150, 120))
    d.polygon([(16, 26), (26, 26), (38, 14), (38, 50), (26, 38), (16, 38)],
              outline=WHITE, fill=WHITE)
    d.arc([36, 18, 54, 46], -60, 60, fill=WHITE, width=4)
    return im


ICONS = {
    # The Start menu's folders. `cat-<key>` is the name start_menu.c
    # derives from a `.desktop` Category=, so an icon appears the day a
    # category does -- and a category with no icon file simply draws
    # none rather than breaking the column.
    "cat-favourites": icon_cat_favourites,
    "cat-recent": icon_cat_recent,
    "cat-all": icon_cat_all,
    "cat-utility": icon_cat_utility,
    "cat-graphics": icon_cat_graphics,
    "cat-multimedia": icon_cat_multimedia,
    "cat-games": icon_cat_games,
    "cat-development": icon_cat_development,
    "cat-time": icon_cat_time,
    "cat-appearance": icon_cat_appearance,
    "cat-input": icon_cat_input,
    "cat-shortcuts": icon_cat_shortcuts,
    "cat-kernel": icon_cat_kernel,
    "cat-system": icon_cat_system,
    "cat-desktop": icon_cat_desktop,
    "cat-network": icon_cat_network,
    "cat-display": icon_cat_display,
    "cat-storage": icon_cat_storage,
    "cat-sound": icon_cat_sound,
    "devmgr": icon_devmgr,
    "sysupdate": icon_sysupdate,
    "dev-usb": icon_dev_usb,
    "badge-warning": icon_badge_warning,
    "badge-disabled": icon_badge_disabled,
    "start": icon_start,
    "toyos": icon_toyos,
    "notepad": icon_notepad,
    "terminal": icon_terminal,
    "calculator": icon_calculator,
    "settings": icon_settings,
    "taskmgr": icon_taskmgr,
    "imgview": icon_imgview,
    "player": icon_player,
    "about": icon_about,
    "help": icon_help,
    "logview": icon_logview,
    "screenshot": icon_screenshot,
    "shapes": icon_shapes,
    "fontdemo": icon_fontdemo,
    "uidemo": icon_uidemo,
    "mines": icon_mines,
    "doom": icon_doom,
    "files": icon_files,
    "folder": icon_folder,
    "file": icon_file,
    "tb-up": icon_tb_up,
    "tb-zoom-in": icon_tb_zoom_in,
    "tb-zoom-out": icon_tb_zoom_out,
    "tb-fit": icon_tb_fit,
    "tb-actual": icon_tb_actual,
    "tb-rotate-left": icon_tb_rotate_left,
    "tb-rotate-right": icon_tb_rotate_right,
    "tb-wallpaper": icon_tb_wallpaper,
    "tb-slideshow": icon_tb_slideshow,
    "tb-info": icon_tb_info,
    "tb-open": icon_tb_open,
    "tb-shuffle": icon_tb_shuffle,
    "tb-repeat": icon_tb_repeat,
    "tb-playlist": icon_tb_playlist,
    "tb-fullscreen": icon_tb_fullscreen,
    "tb-back": icon_tb_back,
    "tb-forward": icon_tb_forward,
    "tb-refresh": icon_tb_refresh,
    "tb-home": icon_tb_home,
    "tb-cut": icon_tb_cut,
    "tb-paste": icon_tb_paste,
    "tb-new": icon_tb_new,
    "tb-sort": icon_tb_sort,
    "tb-view": icon_tb_view,
    "tb-pane": icon_tb_pane,
    "tb-more": icon_tb_more,
    "file-text": icon_file_text,
    "file-doc": icon_file_doc,
    "file-image": icon_file_image,
    "file-audio": icon_file_audio,
    "file-config": icon_file_config,
    "file-font": icon_file_font,
    "file-app": icon_file_app,
    "place-home": icon_place_home_tile,
    "place-desktop": icon_place_desktop_tile,
    "place-documents": icon_place_documents_tile,
    "place-music": icon_place_music_tile,
    "place-pictures": icon_place_pictures_tile,
    "drive": icon_drive,
    "drive-ram": icon_drive_ram,
    "tb-details": icon_tb_details,
    "tb-chart": icon_tb_chart,
    "tb-gear": icon_tb_gear,
    "tb-icons": icon_tb_icons,
    "tb-panes": icon_tb_panes,
    "tb-tree": icon_tb_tree,
    "tb-copy": icon_tb_copy,
    "tb-move": icon_tb_move,
    "tb-mkdir": icon_tb_mkdir,
    "tb-rename": icon_tb_rename,
    "tb-delete": icon_tb_delete,
    "properties": icon_properties,
    "diskmark": icon_diskmark,
    "tray-volume-high": icon_tray_volume_high,
    "tray-volume-low": icon_tray_volume_low,
    "tray-volume-muted": icon_tray_volume_muted,
    "tray-brightness": icon_tray_brightness,
    "tray-keyboard": icon_tray_keyboard,
    "tray-network": icon_tray_network,
    "tray-network-limited": icon_tray_network_limited,
    "tray-network-off": icon_tray_network_off,
    "tray-remote": icon_tray_remote,
}

# Crash Test deliberately gets NO icon file. It is the one entry that
# exercises the letter-tile fallback on every boot -- the same trick
# data/fonts/ plays by shipping vera-mono with no bold companion, so the
# synthesized-bold path is run rather than merely written. Give it an
# icon and nothing on the image tests what happens when a file is
# missing.


for _name, (_body, _tab, _em) in FOLDER_KINDS.items():
    ICONS[_name] = (lambda b=_body, t=_tab, e=_em: _folder_kind(b, t, e))


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
