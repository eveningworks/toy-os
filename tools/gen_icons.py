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
    "cat-time": icon_cat_time,
    "cat-appearance": icon_cat_appearance,
    "cat-input": icon_cat_input,
    "cat-kernel": icon_cat_kernel,
    "cat-system": icon_cat_system,
    "cat-desktop": icon_cat_desktop,
    "cat-network": icon_cat_network,
    "cat-display": icon_cat_display,
    "cat-storage": icon_cat_storage,
    "cat-sound": icon_cat_sound,
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
    "tb-refresh": icon_tb_refresh,
    "tb-details": icon_tb_details,
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
