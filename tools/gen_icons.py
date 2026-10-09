#!/usr/bin/env python3
"""Draw the application icons into data/icons/*.qoi.

WHY THE REPO DRAWS ITS OWN. Same reason tools/gen_cursors.py and
gen_imgdata.py exist: every pixel this project ships should be one it
owns, and an icon set lifted from somewhere else brings a licence with
it. The colour art -- apps, folders, files, drives, categories -- is
drawn in tools/icon_art.py (each app as its own object, at 4x); this
file holds the symbolic glyphs, tinted at draw time, and writes them all.

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
    """A rounded plate in one hue: the toy-os logo's and a device class's."""
    im = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([PAD, PAD, SIZE - PAD - 1, SIZE - PAD - 1],
                        radius=RADIUS, fill=colour + (255,))
    return im, d


# --- toolbar glyphs ----------------------------------------------------
#
# Dark ink, no plate: these sit on the toolbar's near-white chrome, the
# opposite situation from the desktop's white-on-wallpaper app icons.
# The command bar's own set is the OUTLINE set below; the rest are bold
# filled shapes, drawn at ~20px from this 64px master.

TB_INK = (55, 60, 72, 255)


def _tb():
    im = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    return im, ImageDraw.Draw(im)


# --- the command bar's OUTLINE set (chosen from mockups, 2026-10-06) ---
#
# Windows 11 Fluent's shape: thin rounded strokes on a 24-unit grid,
# round caps and joins. Drawn at 4x the 64px master and shrunk, which is
# the only anti-aliasing Pillow has; a stroke is a chain of lines with a
# disc at every joint, so a corner is round whatever its angle. SYMBOLIC:
# one colour, tinted by the toolbar's action roles.
OUTLINE_W = 1.7           # in grid units: 1.5 drawn, plus what two resamples thin
_K = SIZE * 4 / 24        # grid unit -> supersampled pixels


class _Pen:
    def __init__(self):
        self.im = Image.new("RGBA", (SIZE * 4, SIZE * 4), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.im)
        self.w = OUTLINE_W * _K

    def _p(self, x, y):
        return (x * _K, y * _K)

    def _dot(self, x, y):
        r = self.w / 2
        cx, cy = self._p(x, y)
        self.d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=TB_INK)

    def line(self, *pts):
        xy = [self._p(x, y) for x, y in pts]
        self.d.line(xy, fill=TB_INK, width=round(self.w))
        for x, y in pts:
            self._dot(x, y)

    def arc(self, cx, cy, r, a0, a1, steps=24):
        pts = [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / steps)),
                cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / steps)))
               for i in range(steps + 1)]
        self.line(*pts)

    def circle(self, cx, cy, r):
        self.arc(cx, cy, r, 0, 360, 48)

    def rrect(self, x, y, w, h, r):
        # Four sides and four quarter-arcs, one chain.
        self.arc(x + w - r, y + r, r, -90, 0, 8)
        self.arc(x + w - r, y + h - r, r, 0, 90, 8)
        self.arc(x + r, y + h - r, r, 90, 180, 8)
        self.arc(x + r, y + r, r, 180, 270, 8)
        self.line((x + r, y), (x + w - r, y))
        self.line((x + w, y + r), (x + w, y + h - r))
        self.line((x + r, y + h), (x + w - r, y + h))
        self.line((x, y + r), (x, y + h - r))

    def fill_rrect(self, x, y, w, h, r):
        self.d.rounded_rectangle([x * _K, y * _K, (x + w) * _K, (y + h) * _K],
                                 radius=r * _K, fill=TB_INK)

    def done(self):
        return self.im.resize((SIZE, SIZE), Image.LANCZOS)


def _arrow(pen, x0, y0, x1, y1, head=4.5):
    """A shaft with a two-stroke head at (x1, y1)."""
    pen.line((x0, y0), (x1, y1))
    a = math.atan2(y1 - y0, x1 - x0)
    for s in (-1, 1):
        b = a + math.pi + s * math.radians(42)
        pen.line((x1, y1), (x1 + head * math.cos(b), y1 + head * math.sin(b)))


def icon_tb_new():
    p = _Pen(); p.line((12, 4.5), (12, 19.5)); p.line((4.5, 12), (19.5, 12)); return p.done()


def icon_tb_cut():
    p = _Pen()
    p.line((9.2, 14.4), (17.5, 3)); p.line((14.8, 14.4), (6.5, 3))
    p.circle(6.6, 17.2, 3.1); p.circle(17.4, 17.2, 3.1)
    return p.done()


def icon_tb_copy():
    p = _Pen()
    p.rrect(8.5, 8, 11.5, 13, 2)
    p.line((4.5, 15), (4.5, 6)); p.arc(6.5, 6, 2, 180, 270, 6); p.line((6.5, 4), (15, 4))
    return p.done()


def icon_tb_paste():
    p = _Pen()
    p.line((8, 4.5), (6.5, 4.5)); p.arc(6.5, 6.5, 2, 180, 270, 6)
    p.line((4.5, 6.5), (4.5, 19)); p.arc(6.5, 19, 2, 90, 180, 6); p.line((6.5, 21), (17.5, 21))
    p.arc(17.5, 19, 2, 0, 90, 6); p.line((19.5, 19), (19.5, 6.5)); p.arc(17.5, 6.5, 2, 270, 360, 6)
    p.line((17.5, 4.5), (16, 4.5))
    p.rrect(8.5, 2.75, 7, 3.75, 1)
    p.line((8.5, 12), (15.5, 12)); p.line((8.5, 16), (13.5, 16))
    return p.done()


def icon_tb_rename():
    # A text field with the caret standing in it.
    p = _Pen()
    p.line((13, 7.5), (5, 7.5)); p.arc(5, 9.5, 2, 180, 270, 6); p.line((3, 9.5), (3, 14.5))
    p.arc(5, 14.5, 2, 90, 180, 6); p.line((5, 16.5), (13, 16.5))
    p.line((20.5, 9), (20.5, 15))
    p.line((17, 4.5), (17, 19.5)); p.line((15, 4.5), (19, 4.5)); p.line((15, 19.5), (19, 19.5))
    return p.done()


def _bin(p):
    p.line((3.5, 6.5), (20.5, 6.5))
    p.line((9, 6.5), (9, 4.5)); p.arc(10, 4.5, 1, 180, 270, 4); p.line((10, 3.5), (14, 3.5))
    p.arc(14, 4.5, 1, 270, 360, 4); p.line((15, 4.5), (15, 6.5))
    p.line((5.5, 6.5), (6.6, 19.4)); p.arc(8.1, 19.3, 1.5, 90, 180, 6)
    p.line((8.1, 20.8), (15.9, 20.8)); p.arc(15.9, 19.3, 1.5, 0, 90, 6); p.line((17.4, 19.4), (18.5, 6.5))


def icon_tb_delete():
    p = _Pen(); _bin(p); p.line((10, 10.5), (10, 17)); p.line((14, 10.5), (14, 17)); return p.done()


def icon_tb_bin_restore():
    p = _Pen(); _bin(p); _arrow(p, 12, 17.5, 12, 10.5, 3.2); return p.done()


def icon_tb_bin_empty():
    p = _Pen(); _bin(p); p.line((9.5, 11), (14.5, 16)); p.line((14.5, 11), (9.5, 16)); return p.done()


def icon_tb_sort():
    p = _Pen(); _arrow(p, 7.5, 4.5, 7.5, 19.5, 3.5); _arrow(p, 16.5, 19.5, 16.5, 4.5, 3.5); return p.done()


def icon_tb_view():
    p = _Pen()
    for x, y in ((4, 4), (13.5, 4), (4, 13.5), (13.5, 13.5)):
        p.rrect(x, y, 6.5, 6.5, 1.5)
    return p.done()


def icon_tb_icons():
    return icon_tb_view()


def icon_tb_details():
    p = _Pen()
    for y in (6.5, 12, 17.5):
        p.line((4, y), (5, y)); p.line((8.5, y), (20, y))
    return p.done()


def icon_tb_more():
    p = _Pen()
    for x in (6, 12, 18):
        p.fill_rrect(x - 1.4, 10.6, 2.8, 2.8, 1.4)
    return p.done()


def icon_tb_pane():
    p = _Pen(); p.rrect(3.5, 5, 17, 14, 2); p.line((14.5, 5), (14.5, 19)); return p.done()


def icon_tb_back():
    p = _Pen(); _arrow(p, 19.5, 12, 4.5, 12, 6.5); return p.done()


def icon_tb_forward():
    p = _Pen(); _arrow(p, 4.5, 12, 19.5, 12, 6.5); return p.done()


def icon_tb_up():
    p = _Pen(); _arrow(p, 12, 19.5, 12, 4.5, 6.5); return p.done()


def icon_tb_refresh():
    # An open circle, the gap on the right, and a head on its upper end
    # pointing along the turn -- Fluent's ArrowClockwise.
    p = _Pen()
    p.arc(12, 12, 7.5, 30, 330, 40)
    t = math.radians(330)
    tx, ty = 12 + 7.5 * math.cos(t), 12 + 7.5 * math.sin(t)
    ang = math.atan2(math.cos(t), -math.sin(t))      # the tangent, increasing angle
    for sgn in (-1, 1):
        b = ang + math.pi + sgn * math.radians(45)
        p.line((tx, ty), (tx + 4.5 * math.cos(b), ty + 4.5 * math.sin(b)))
    return p.done()


def icon_tb_home():
    # A house: roof, walls and a door cut out of them.
    im, d = _tb()
    d.polygon([(32, 6), (58, 30), (6, 30)], fill=TB_INK)
    d.rectangle([14, 28, 50, 56], fill=TB_INK)
    d.rectangle([27, 38, 37, 56], fill=(0, 0, 0, 0))
    return im


# --- places (the File Manager's sidebar) --------------------------------
#
# Flat ink, no plate, then recoloured onto a tile below. The file types,
# folders and drives are tools/icon_art.py's.

PLACE_INK = (91, 127, 191, 255)


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
def icon_place_trash_tile():     return _tile((104, 112, 125), icon_place_trash)
def icon_place_recent_tile():    return _tile((214, 133, 54), icon_place_recent)


# A FOLDER OF A KNOWN KIND is the plain folder's shape in the kind's
# colour, with a white emblem on its front -- Plasma's folder-music and
# Explorer's special folders. lib/ufiletype.c maps names to these; the
# emblems are drawn in the 64-px space and icon_art.kind_folder() puts
# them on its folder at 4x.


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


def icon_tb_find():
    # The lens alone: zoom-in's without the plus.
    im, d = _tb()
    _tb_lens(d)
    return im


def icon_tb_menu():
    # Three bars -- the menu button every app with a hidden menu bar has.
    im, d = _tb()
    for y in (14, 29, 44):
        d.rounded_rectangle([8, y, 56, y + 7], radius=3, fill=TB_INK)
    return im


def icon_tb_chevron():
    # A small down chevron, for a button that opens a list.
    im, d = _tb()
    d.line([18, 24, 32, 40, 46, 24], fill=TB_INK, width=7, joint="curve")
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


def icon_place_recent():
    im, d = _place()
    d.ellipse([6, 6, 58, 58], fill=PLACE_INK)
    cut = (200, 215, 240, 255)
    d.line([32, 32, 32, 15], fill=cut, width=5)
    d.line([32, 32, 44, 40], fill=cut, width=5)
    d.ellipse([28, 28, 36, 36], fill=cut)
    return im


def icon_place_trash():
    im, d = _place()
    d.rectangle([20, 6, 44, 12], fill=PLACE_INK)
    d.rectangle([8, 14, 56, 20], fill=PLACE_INK)
    d.polygon([(12, 22), (52, 22), (48, 58), (16, 58)], fill=PLACE_INK)
    for x in (24, 32, 40):
        d.line([x, 30, x, 50], fill=(200, 215, 240, 255), width=4)
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


def icon_tb_desktop_add():
    # A monitor on its stand with a plus on the screen: "Add to desktop".
    # Symbolic like every tb- glyph, so a menu's tint recolours it whole.
    im, d = _tb()
    d.rounded_rectangle([4, 8, 60, 46], radius=4, outline=TB_INK, width=6)
    d.line([32, 16, 32, 38], fill=TB_INK, width=6)
    d.line([21, 27, 43, 27], fill=TB_INK, width=6)
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


def icon_tb_camera():
    # A camera body, its lens and the bump on top: Save frame.
    im, d = _tb()
    d.rounded_rectangle([4, 18, 60, 54], radius=7, outline=TB_INK, width=5)
    d.polygon([(20, 18), (24, 10), (40, 10), (44, 18)], fill=TB_INK)
    d.ellipse([20, 24, 44, 48], outline=TB_INK, width=5)
    return im


def icon_tb_speed():
    # A dial with its needle past the middle: playback speed.
    im, d = _tb()
    d.arc([6, 12, 58, 64], 180, 360, fill=TB_INK, width=6)
    d.line([(32, 38), (46, 22)], fill=TB_INK, width=6)
    d.ellipse([27, 33, 37, 43], fill=TB_INK)
    return im


def icon_tb_fullscreen():
    # Four corners pointing out.
    im, d = _tb()
    for (x, y, dx, dy) in ((6, 6, 1, 1), (58, 6, -1, 1), (6, 58, 1, -1), (58, 58, -1, -1)):
        d.line([x, y, x + 18 * dx, y], fill=TB_INK, width=6)
        d.line([x, y, x, y + 18 * dy], fill=TB_INK, width=6)
    return im


# THE WINDOW MENU'S FOUR, drawn as the caption buttons draw them
# (wm_render.c) -- the menu row and the button it stands for share a glyph.
def icon_tb_minimize():
    im, d = _tb()
    d.rectangle([10, 40, 54, 46], fill=TB_INK)
    return im


def icon_tb_maximize():
    im, d = _tb()
    d.rounded_rectangle([10, 10, 54, 54], radius=4, outline=TB_INK, width=6)
    return im


def icon_tb_restore():
    im, d = _tb()
    d.rounded_rectangle([20, 8, 56, 44], radius=4, outline=TB_INK, width=6)
    d.rectangle([8, 20, 44, 56], fill=(0, 0, 0, 0))
    d.rounded_rectangle([8, 20, 44, 56], radius=4, outline=TB_INK, width=6)
    return im


def icon_tb_close():
    im, d = _tb()
    d.line([12, 12, 52, 52], fill=TB_INK, width=7)
    d.line([52, 12, 12, 52], fill=TB_INK, width=7)
    return im


def icon_tb_close_all():
    # Close's X in a window, another window's corner behind it: every
    # window of the app (the window menu's "Close all N windows").
    im, d = _tb()
    d.line([(20, 10), (54, 10), (54, 44)], fill=TB_INK, width=6, joint="curve")
    d.rounded_rectangle([8, 20, 44, 56], radius=4, outline=TB_INK, width=6)
    d.line([18, 30, 34, 46], fill=TB_INK, width=6)
    d.line([34, 30, 18, 46], fill=TB_INK, width=6)
    return im


def icon_tb_unfullscreen():
    # Four corners pointing in: the fullscreen glyph turned inside out.
    im, d = _tb()
    for (x, y, dx, dy) in ((24, 24, -1, -1), (40, 24, 1, -1), (24, 40, -1, 1), (40, 40, 1, 1)):
        d.line([x, y, x + 18 * dx, y], fill=TB_INK, width=6)
        d.line([x, y, x, y + 18 * dy], fill=TB_INK, width=6)
    return im


def icon_tb_pin():
    # A pushpin, head up-right: Explorer's "Pin to Start".
    im, d = _tb()
    d.polygon([(36, 6), (58, 28), (50, 32), (40, 42), (40, 50), (14, 24), (22, 24), (32, 14)],
              fill=TB_INK)
    d.line([24, 40, 8, 56], fill=TB_INK, width=6)
    return im


def icon_tb_power():
    # The IEC power symbol: a ring open at the top, a bar through the gap.
    im, d = _tb()
    d.arc([8, 10, 56, 58], start=-60, end=240, fill=TB_INK, width=6)
    d.line([32, 4, 32, 32], fill=TB_INK, width=7)
    return im


def icon_tb_star():
    # A five-pointed star: "the default", as favourites and Windows'
    # default-app marks draw it.
    im, d = _tb()
    pts = []
    for k in range(10):
        r = 28 if k % 2 == 0 else 12
        a = -math.pi / 2 + k * math.pi / 5
        pts.append((32 + r * math.cos(a), 34 + r * math.sin(a)))
    d.polygon(pts, fill=TB_INK)
    return im


def icon_tb_undo():
    # An arrow curling back to the left.
    im, d = _tb()
    d.arc([14, 16, 58, 56], start=180, end=450, fill=TB_INK, width=7)
    d.polygon([(4, 36), (24, 36), (14, 20)], fill=TB_INK)
    return im


def icon_tb_redo():
    # Undo's arrow, mirrored.
    return icon_tb_undo().transpose(Image.FLIP_LEFT_RIGHT)


def icon_tb_save():
    # A floppy: the body with its notched corner, the shutter, the label.
    im, d = _tb()
    d.polygon([(6, 6), (48, 6), (58, 16), (58, 58), (6, 58)], outline=TB_INK, width=5)
    d.rectangle([18, 6, 42, 22], fill=TB_INK)
    d.rounded_rectangle([16, 34, 48, 58], radius=3, outline=TB_INK, width=5)
    return im


def icon_tb_wrap():
    # A line that runs to the edge and turns back under itself.
    im, d = _tb()
    d.line([6, 14, 58, 14], fill=TB_INK, width=6)
    d.line([(6, 32), (46, 32)], fill=TB_INK, width=6)
    d.arc([34, 32, 58, 52], start=270, end=450, fill=TB_INK, width=6)
    d.line([(46, 52), (34, 52)], fill=TB_INK, width=6)
    d.polygon([(22, 52), (36, 42), (36, 62)], fill=TB_INK)
    d.line([6, 52, 16, 52], fill=TB_INK, width=6)
    return im


def icon_tb_preview():
    # Source beside its rendering: a page split down the middle, lines
    # on the left, a heading block and lines on the right.
    im, d = _tb()
    d.rounded_rectangle([4, 8, 60, 56], radius=4, outline=TB_INK, width=5)
    d.line([32, 8, 32, 56], fill=TB_INK, width=5)
    for y in (20, 30, 40):
        d.line([12, y, 24, y], fill=TB_INK, width=4)
    d.rectangle([38, 16, 54, 24], fill=TB_INK)
    for y in (32, 42):
        d.line([38, y, 54, y], fill=TB_INK, width=4)
    return im


def icon_tb_play():
    im, d = _tb()
    d.polygon([(14, 6), (58, 32), (14, 58)], fill=TB_INK)
    return im


def icon_tb_stop():
    im, d = _tb()
    d.rounded_rectangle([10, 10, 54, 54], radius=6, fill=TB_INK)
    return im


def icon_tb_history():
    # A clock face with an arrow running back round it.
    im, d = _tb()
    d.arc([6, 6, 58, 58], start=200, end=500, fill=TB_INK, width=6)
    d.polygon([(2, 22), (18, 34), (22, 16)], fill=TB_INK)
    d.line([(32, 18), (32, 34), (44, 42)], fill=TB_INK, width=6, joint="curve")
    return im


def icon_tb_region():
    # Corner brackets round a dashed square: "this part of the screen".
    im, d = _tb()
    for (x, y, dx, dy) in ((6, 6, 1, 1), (58, 6, -1, 1), (6, 58, 1, -1), (58, 58, -1, -1)):
        d.line([x, y, x + 14 * dx, y], fill=TB_INK, width=6)
        d.line([x, y, x, y + 14 * dy], fill=TB_INK, width=6)
    for k in range(4):
        d.rectangle([22 + k * 6, 22, 24 + k * 6, 24], fill=TB_INK)
        d.rectangle([22 + k * 6, 40, 24 + k * 6, 42], fill=TB_INK)
    return im


def icon_tb_screen():
    # A monitor on its stand.
    im, d = _tb()
    d.rounded_rectangle([4, 8, 60, 46], radius=4, outline=TB_INK, width=6)
    d.line([32, 46, 32, 56], fill=TB_INK, width=6)
    d.line([18, 58, 46, 58], fill=TB_INK, width=6)
    return im


def icon_tb_window():
    # A window: frame and title bar.
    im, d = _tb()
    d.rounded_rectangle([4, 8, 60, 56], radius=5, outline=TB_INK, width=6)
    d.rectangle([4, 8, 60, 22], fill=TB_INK)
    return im


def icon_tb_pointer():
    # The arrow pointer.
    im, d = _tb()
    d.polygon([(14, 4), (52, 38), (34, 40), (44, 60), (36, 62), (26, 44), (14, 56)], fill=TB_INK)
    return im


def icon_tb_timer():
    # A stopwatch: the face, the crown, a hand.
    im, d = _tb()
    d.ellipse([8, 12, 56, 60], outline=TB_INK, width=6)
    d.rectangle([24, 2, 40, 8], fill=TB_INK)
    d.line([32, 36, 32, 22], fill=TB_INK, width=6)
    return im


def icon_tb_info():
    im, d = _tb()
    d.ellipse([4, 4, 60, 60], outline=TB_INK, width=6)
    d.rectangle([28, 27, 36, 48], fill=TB_INK)
    d.ellipse([27, 14, 37, 23], fill=TB_INK)
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
    mark becomes a smudge.
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


# --- device classes (Device Manager) ------------------------------------


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


ICONS = {
    # The Start menu's folders. `cat-<key>` is the name start_menu.c
    # derives from a `.desktop` Category=, so an icon appears the day a
    # category does -- and a category with no icon file simply draws
    # none rather than breaking the column.
    "dev-usb": icon_dev_usb,
    "badge-warning": icon_badge_warning,
    "badge-disabled": icon_badge_disabled,
    "start": icon_start,
    "toyos": icon_toyos,
    "tb-up": icon_tb_up,
    "tb-zoom-in": icon_tb_zoom_in,
    "tb-zoom-out": icon_tb_zoom_out,
    "tb-fit": icon_tb_fit,
    "tb-actual": icon_tb_actual,
    "tb-rotate-left": icon_tb_rotate_left,
    "tb-rotate-right": icon_tb_rotate_right,
    "tb-wallpaper": icon_tb_wallpaper,
    "tb-desktop-add": icon_tb_desktop_add,
    "tb-slideshow": icon_tb_slideshow,
    "tb-info": icon_tb_info,
    "tb-open": icon_tb_open,
    "tb-shuffle": icon_tb_shuffle,
    "tb-repeat": icon_tb_repeat,
    "tb-playlist": icon_tb_playlist,
    "tb-camera": icon_tb_camera,
    "tb-speed": icon_tb_speed,
    "tb-fullscreen": icon_tb_fullscreen,
    "tb-unfullscreen": icon_tb_unfullscreen,
    "tb-minimize": icon_tb_minimize,
    "tb-maximize": icon_tb_maximize,
    "tb-restore": icon_tb_restore,
    "tb-close": icon_tb_close,
    "tb-close-all": icon_tb_close_all,
    "tb-pin": icon_tb_pin,
    "tb-power": icon_tb_power,
    "tb-star": icon_tb_star,
    "tb-undo": icon_tb_undo,
    "tb-redo": icon_tb_redo,
    "tb-save": icon_tb_save,
    "tb-wrap": icon_tb_wrap,
    "tb-preview": icon_tb_preview,
    "tb-play": icon_tb_play,
    "tb-stop": icon_tb_stop,
    "tb-history": icon_tb_history,
    "tb-region": icon_tb_region,
    "tb-screen": icon_tb_screen,
    "tb-window": icon_tb_window,
    "tb-pointer": icon_tb_pointer,
    "tb-timer": icon_tb_timer,
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
    "tb-find": icon_tb_find,
    "tb-menu": icon_tb_menu,
    "tb-chevron": icon_tb_chevron,
    "place-home": icon_place_home_tile,
    "place-desktop": icon_place_desktop_tile,
    "place-documents": icon_place_documents_tile,
    "place-music": icon_place_music_tile,
    "place-pictures": icon_place_pictures_tile,
    "place-trash": icon_place_trash_tile,
    "place-recent": icon_place_recent_tile,
    "tb-bin-restore": icon_tb_bin_restore,
    "tb-bin-empty": icon_tb_bin_empty,
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


# THE COLOUR ART -- apps, folders, files, drives, categories -- is
# tools/icon_art.py's, drawn at 4x; this file keeps the symbolic glyphs.
import icon_art  # noqa: E402

ICONS.update(icon_art.ART)
for _name, (_body, _tab, _em) in FOLDER_KINDS.items():
    ICONS[_name] = (lambda b=_body, t=_tab, e=_em: icon_art.kind_folder(b, t, e))


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
