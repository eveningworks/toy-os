"""The COLOUR icons -- apps, folders, file types, drives and the Start
menu's and System Settings' categories -- for tools/gen_icons.py.

THE LOOK IS THE OBJECT ITSELF, NO PLATE (chosen from mockups,
2026-10-05): a page for the editor, a folder for the file manager, a
terminal window for the terminal, each in its own colours with a soft
vertical gradient and a soft shadow -- Windows 11's and GNOME's app
icons. The symbolic glyphs (toolbar, tray) stay in gen_icons.py: they
are tinted at draw time, so they must stay one flat ink.

DRAWN AT 4x AND BOX-REDUCED. Pillow's ImageDraw does not anti-alias, so
a shape drawn at 64 px has stair-stepped edges; drawn at 256 and reduced
by an exact 4x4 box (in premultiplied alpha, so an edge does not pick up
a dark fringe from the transparent black around it) every edge gets 16
samples. Coordinates below are all in the 64-px space; `u()` and `D4`
scale them, so a shape reads the same as the master it ships as.
"""
import math

from PIL import Image, ImageChops, ImageDraw, ImageFilter

SIZE = 64
K = 4                     # supersampling factor
S = SIZE * K


# --- the 4x drawing kit ------------------------------------------------

def u(*v):
    """64-px coordinates -> draw pixels, for a box or a point list."""
    return [round(x * K) for x in v]


def canvas():
    return Image.new("RGBA", (S, S), (0, 0, 0, 0))


def finish(im):
    """The 4x drawing, reduced to the 64-px master by an exact box filter
    in premultiplied alpha."""
    return im.convert("RGBa").reduce(K).convert("RGBA")


def blank():
    return Image.new("L", (S, S), 0)


def m_rr(x0, y0, x1, y1, r):
    m = blank()
    ImageDraw.Draw(m).rounded_rectangle(u(x0, y0, x1, y1), radius=round(r * K), fill=255)
    return m


def m_ellipse(x0, y0, x1, y1):
    m = blank()
    ImageDraw.Draw(m).ellipse(u(x0, y0, x1, y1), fill=255)
    return m


def m_poly(*pts):
    m = blank()
    ImageDraw.Draw(m).polygon(u(*pts), fill=255)
    return m


def m_and(a, b):
    return ImageChops.multiply(a, b)


def m_or(a, b):
    return ImageChops.lighter(a, b)


def m_minus(a, b):
    return ImageChops.subtract(a, b)


def vgrad(top, bottom):
    g = Image.new("RGBA", (1, S))
    for y in range(S):
        t = y / (S - 1)
        g.putpixel((0, y), tuple(round(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,))
    return g.resize((S, S))


def fill(im, mask, colour):
    """Paint `colour` -- an RGB tuple, or a (top, bottom) gradient pair --
    through `mask`."""
    if isinstance(colour[0], tuple):
        layer = vgrad(*colour)
    else:
        layer = Image.new("RGBA", (S, S), tuple(colour[:3]) + (255,))
    im.paste(layer, (0, 0), mask)


def shadow(im, mask=None, dy=1.5, blur=1.6, alpha=85):
    """A soft shadow under `mask` (default: everything drawn so far),
    composited UNDER the drawing."""
    if mask is None:
        mask = im.getchannel("A")
    sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    sh.paste(Image.new("RGBA", (S, S), (12, 22, 40, 255)), (0, round(dy * K)),
             mask.point(lambda v: v * alpha // 255))
    sh = sh.filter(ImageFilter.GaussianBlur(blur * K))
    return Image.alpha_composite(sh, im)


def edge(im, mask, colour, width=1.0):
    """A line just inside `mask`'s border -- what keeps a light object
    (a page, a drive) from dissolving into a white menu at 20 px."""
    n = max(3, round(width * K) * 2 + 1)
    inner = mask.filter(ImageFilter.MinFilter(n))
    fill(im, ImageChops.subtract(mask, inner), colour)


def lighter(rgb, n):
    return tuple(min(255, c + n) for c in rgb)


def darker(rgb, n):
    return tuple(max(0, c - n) for c in rgb)


class D4:
    """ImageDraw in the 64-px space, drawing at 4x. A box (x0, y0, x1, y1)
    covers the same pixels it would at 64 px -- x1 inclusive -- and a
    point is a pixel's CENTRE, so an older 64-px drawing ports verbatim."""

    def __init__(self, im):
        self.d = ImageDraw.Draw(im)

    @staticmethod
    def _box(xy):
        x0, y0, x1, y1 = xy
        return [round(x0 * K), round(y0 * K), round((x1 + 1) * K) - 1, round((y1 + 1) * K) - 1]

    @staticmethod
    def _pts(xy):
        flat = []
        for p in xy:
            if isinstance(p, (tuple, list)):
                flat.extend(p)
            else:
                flat.append(p)
        return [round(v * K + K / 2) for v in flat]

    @staticmethod
    def _w(width):
        return max(1, round(width * K))

    def line(self, xy, fill=None, width=1, joint="curve"):
        self.d.line(self._pts(xy), fill=fill, width=self._w(width), joint=joint)

    def polygon(self, xy, fill=None, outline=None, width=1):
        self.d.polygon(self._pts(xy), fill=fill, outline=outline, width=self._w(width))

    def ellipse(self, xy, fill=None, outline=None, width=1):
        self.d.ellipse(self._box(xy), fill=fill, outline=outline, width=self._w(width))

    def rectangle(self, xy, fill=None, outline=None, width=1):
        self.d.rectangle(self._box(xy), fill=fill, outline=outline, width=self._w(width))

    def rounded_rectangle(self, xy, radius=0, fill=None, outline=None, width=1):
        self.d.rounded_rectangle(self._box(xy), radius=round(radius * K), fill=fill,
                                 outline=outline, width=self._w(width))

    def arc(self, xy, start, end, fill=None, width=1):
        self.d.arc(self._box(xy), start, end, fill=fill, width=self._w(width))

    def pieslice(self, xy, start, end, fill=None, outline=None, width=1):
        self.d.pieslice(self._box(xy), start, end, fill=fill, outline=outline, width=self._w(width))


WHITE = (255, 255, 255, 255)


def gear_mask(cx, cy, r_out, r_in, teeth, hole):
    m = blank()
    d = ImageDraw.Draw(m)
    pts = []
    for i in range(teeth * 2):
        a0 = math.pi * 2 * i / (teeth * 2)
        r = r_out if i % 2 == 0 else r_in
        for da in (-0.17, 0.17):
            pts.append(cx + r * math.cos(a0 + da))
            pts.append(cy + r * math.sin(a0 + da))
    d.polygon(u(*pts), fill=255)
    d.ellipse(u(cx - hole, cy - hole, cx + hole, cy + hole), fill=0)
    return m


# --- shared objects ----------------------------------------------------

def page(x0=14, y0=6, x1=50, y1=58, fold=10, top=(252, 252, 255), bottom=(222, 228, 238)):
    """A sheet with its top-right corner folded: the base of every
    document, log and file-type icon. Returns (image, page mask)."""
    im = canvas()
    m = m_rr(x0, y0, x1, y1, 4)
    if fold:
        m = m_minus(m, m_poly(x1 - fold, y0, x1 + 1, y0, x1 + 1, y0 + fold))
    fill(im, m, (top, bottom))
    edge(im, m, (170, 180, 198))
    if fold:
        fill(im, m_poly(x1 - fold, y0, x1 - fold, y0 + fold, x1, y0 + fold), ((205, 212, 224), (186, 194, 208)))
    return im, m


def lines(d, x0, rows, w=2.4, ink=(150, 162, 184, 255)):
    """Text rows: (y, x_end) pairs."""
    for y, x1 in rows:
        d.rounded_rectangle((x0, y, x1, y + w), radius=w / 2, fill=ink)


def folder(body, tab=None):
    """A folder, open-fronted: the back with its tab, the front lower.
    Returns (image, union mask)."""
    tab = tab or darker(body, 22)
    im = canvas()
    back = m_or(m_rr(6, 10, 30, 22, 4), m_rr(6, 15, 58, 52, 5))
    fill(im, back, (tab, darker(tab, 18)))
    front = m_rr(6, 21, 58, 54, 5)
    fill(im, front, (lighter(body, 24), body))
    # the front's lip: a light line along its top edge
    fill(im, m_and(front, m_rr(6, 21, 58, 23, 1)), lighter(body, 50))
    return im, m_or(back, front)


def window(top=(250, 251, 253), bottom=(228, 233, 241), bar=(72, 128, 230)):
    """An app window: a card with a coloured title strip. (im, mask)."""
    im = canvas()
    m = m_rr(6, 10, 58, 54, 6)
    fill(im, m, (top, bottom))
    fill(im, m_and(m, m_rr(6, 10, 58, 19, 0)), (lighter(bar, 20), bar))
    return im, m


# =====================================================================
# THE APPS
# =====================================================================

def notepad():
    im, m = page(fold=0)
    fill(im, m_and(m, m_rr(14, 6, 50, 19, 0)), ((86, 146, 255), (44, 104, 232)))
    lines(D4(im), 20, [(26, 44), (33, 40), (40, 44), (47, 34)], ink=(120, 138, 170, 255))
    return finish(shadow(im))


def files():
    im, _ = folder((250, 192, 60), (226, 156, 34))
    d = D4(im)
    d.rounded_rectangle((6, 44, 58, 54), radius=5, fill=(46, 112, 220, 255))
    d.rectangle((6, 44, 58, 47), fill=(46, 112, 220, 255))
    return finish(shadow(im))


def terminal():
    im = canvas()
    m = m_rr(6, 10, 58, 54, 6)
    fill(im, m, ((60, 66, 80), (28, 31, 40)))
    fill(im, m_and(m, m_rr(6, 10, 58, 19, 0)), ((86, 94, 110), (72, 80, 96)))
    d = D4(im)
    for i, c in enumerate([(255, 96, 92), (255, 190, 50), (40, 200, 90)]):
        d.ellipse((10 + i * 5, 13, 13 + i * 5, 16), fill=c + (255,))
    d.line([(14, 27), (21, 33), (14, 39)], fill=(96, 232, 156, 255), width=3)
    d.rounded_rectangle((25, 37, 39, 40), radius=1.5, fill=(222, 226, 236, 255))
    return finish(shadow(im))


def calculator():
    im = canvas()
    m = m_rr(12, 4, 52, 60, 7)
    fill(im, m, ((250, 251, 253), (220, 224, 232)))
    d = D4(im)
    d.rounded_rectangle((17, 9, 47, 21), radius=3, fill=(46, 58, 78, 255))
    d.rounded_rectangle((31, 13, 43, 17), radius=1, fill=(150, 232, 190, 255))
    for r in range(3):
        for c in range(3):
            col = (255, 140, 40) if (r, c) == (2, 2) else (190, 197, 210)
            d.rounded_rectangle((17 + c * 11, 26 + r * 10, 25 + c * 11, 33 + r * 10),
                                radius=2.5, fill=col + (255,))
    return finish(shadow(im))


def imgview():
    im = canvas()
    m = m_rr(6, 12, 58, 52, 6)
    fill(im, m, ((116, 192, 255), (70, 140, 235)))
    D4(im).ellipse((40, 18, 49, 27), fill=(255, 214, 80, 255))
    fill(im, m_and(m, m_poly(6, 52, 23, 31, 36, 44, 45, 35, 58, 47, 58, 52)),
         ((70, 196, 126), (30, 140, 90)))
    return finish(shadow(im))


def player():
    im = canvas()
    m = m_ellipse(6, 6, 57, 57)
    fill(im, m, ((255, 122, 92), (222, 48, 92)))
    d = D4(im)
    d.rounded_rectangle((30, 17, 33, 41), radius=1.5, fill=WHITE)
    d.polygon([(31, 17), (44, 21), (44, 28), (31, 25)], fill=WHITE)
    d.ellipse((20, 35, 32, 46), fill=WHITE)
    return finish(shadow(im))


def settings():
    im = canvas()
    m = gear_mask(32, 32, 27, 21, 8, 9)
    fill(im, m, ((158, 168, 188), (92, 102, 122)))
    D4(im).ellipse((22, 22, 41, 41), outline=(206, 212, 224, 255), width=2)
    return finish(shadow(im))


def taskmgr():
    im, m = window(bar=(36, 168, 108))
    fill(im, m_and(m, m_poly(10, 50, 10, 42, 20, 36, 28, 42, 38, 27, 46, 33, 54, 22, 54, 50)),
         ((112, 214, 160), (214, 244, 228)))
    D4(im).line([(10, 42), (20, 36), (28, 42), (38, 27), (46, 33), (54, 22)],
                fill=(20, 150, 90, 255), width=2.5)
    return finish(shadow(im))


def mines():
    im = canvas()
    m = m_rr(6, 6, 58, 58, 9)
    fill(im, m, ((174, 218, 86), (128, 184, 58)))
    d = D4(im)
    ink = (40, 44, 54, 255)
    d.ellipse((20, 20, 43, 43), fill=ink)
    for a, b in [((32, 13), (32, 50)), ((13, 32), (50, 32)), ((19, 19), (45, 45)), ((45, 19), (19, 45))]:
        d.line([a, b], fill=ink, width=3)
    d.ellipse((25, 25, 29, 29), fill=(255, 255, 255, 220))
    return finish(shadow(im))


def screenshot():
    im = canvas()
    m = m_or(m_rr(6, 16, 58, 52, 7), m_rr(22, 10, 42, 20, 3))
    fill(im, m, ((86, 116, 168), (46, 70, 115)))
    d = D4(im)
    d.ellipse((20, 22, 43, 45), fill=(232, 238, 248, 255))
    d.ellipse((25, 27, 38, 40), fill=(60, 160, 255, 255))
    d.ellipse((28, 29, 31, 32), fill=(255, 255, 255, 210))
    return finish(shadow(im))


def about():
    im = canvas()
    m = m_ellipse(6, 6, 57, 57)
    fill(im, m, ((88, 150, 255), (40, 96, 220)))
    d = D4(im)
    d.ellipse((28, 15, 35, 22), fill=WHITE)
    d.rounded_rectangle((28, 26, 35, 47), radius=2, fill=WHITE)
    return finish(shadow(im))


def help_app():
    im = canvas()
    m = m_or(m_rr(6, 8, 58, 46, 10), m_poly(16, 44, 16, 56, 30, 44))
    fill(im, m, ((168, 120, 255), (112, 70, 220)))
    d = D4(im)
    d.arc((22, 13, 41, 31), 180, 45, fill=WHITE, width=4.5)
    d.line([(38, 27), (32, 31), (32, 34)], fill=WHITE, width=4.5)
    d.ellipse((29, 37, 35, 43), fill=WHITE)
    return finish(shadow(im))


def logview():
    im, m = page()
    d = D4(im)
    for y, col, x1 in [(19, (232, 70, 70), 42), (27, (240, 176, 40), 38), (35, (60, 180, 110), 44),
                       (43, (60, 180, 110), 36)]:
        d.ellipse((19, y - 0.5, 22.5, y + 3), fill=col + (255,))
        d.rounded_rectangle((26, y, x1, y + 2.4), radius=1.2, fill=(150, 162, 184, 255))
    return finish(shadow(im))


def crashreports():
    im, m = page()
    lines(D4(im), 20, [(16, 40), (23, 44), (30, 36)])
    tri = m_poly(32, 30, 52, 58, 12, 58)
    im2 = canvas()
    fill(im2, tri, ((255, 120, 80), (220, 50, 50)))
    d = D4(im2)
    d.rounded_rectangle((30.5, 39, 33.5, 50), radius=1.5, fill=WHITE)
    d.ellipse((30.5, 52, 33.5, 55), fill=WHITE)
    im = Image.alpha_composite(im, shadow(im2, dy=1, blur=1, alpha=70))
    return finish(shadow(im))


def shapes():
    im = canvas()
    fill(im, m_ellipse(6, 8, 36, 38), ((90, 156, 255), (46, 106, 230)))
    sq = canvas()
    fill(sq, m_rr(26, 26, 58, 58, 6), ((88, 210, 140), (36, 160, 96)))
    im = Image.alpha_composite(im, shadow(sq, dy=1, blur=1.2, alpha=60))
    tri = canvas()
    fill(tri, m_poly(14, 58, 26, 34, 38, 58), ((255, 176, 64), (240, 120, 30)))
    im = Image.alpha_composite(im, shadow(tri, dy=1, blur=1.2, alpha=60))
    return finish(shadow(im))


def fontdemo():
    im = canvas()
    m = m_rr(8, 8, 56, 56, 9)
    fill(im, m, ((255, 250, 238), (240, 228, 204)))
    a = m_minus(m_poly(13, 48, 25, 16, 31, 16, 43, 48, 37, 48, 34.5, 40, 21.5, 40, 19, 48),
                m_poly(23.3, 34, 28, 21, 32.7, 34))
    fill(im, a, ((70, 82, 112), (36, 44, 66)))
    d = D4(im)
    d.ellipse((38, 31, 51, 46), outline=(220, 90, 60, 255), width=3.2)
    d.rounded_rectangle((48, 31, 51, 48), radius=1.5, fill=(220, 90, 60, 255))
    return finish(shadow(im))


def uidemo():
    im, m = window(bar=(120, 96, 220))
    d = D4(im)
    d.rounded_rectangle((12, 25, 30, 33), radius=4, fill=(120, 96, 220, 255))
    d.ellipse((23, 26, 29, 32), fill=WHITE)
    d.rounded_rectangle((12, 40, 52, 42.5), radius=1.25, fill=(196, 202, 216, 255))
    d.rounded_rectangle((12, 40, 34, 42.5), radius=1.25, fill=(120, 96, 220, 255))
    d.ellipse((31, 37, 37, 45), fill=WHITE, outline=(120, 96, 220, 255), width=1.5)
    d.rounded_rectangle((36, 25, 52, 33), radius=2.5, fill=(226, 230, 238, 255))
    return finish(shadow(im))


def doom():
    im = canvas()
    flame = m_poly(32, 4, 42, 18, 48, 14, 52, 30, 50, 44, 44, 54, 32, 59, 20, 54, 14, 44, 12, 30,
                   17, 20, 22, 26, 25, 14)
    fill(im, flame, ((255, 196, 60), (214, 44, 30)))
    inner = m_poly(32, 24, 38, 34, 42, 32, 43, 44, 38, 52, 32, 55, 26, 52, 21, 44, 23, 34, 27, 38)
    fill(im, inner, ((255, 236, 150), (255, 150, 40)))
    return finish(shadow(im))


def devmgr():
    im = canvas()
    m = m_rr(6, 8, 58, 56, 6)
    fill(im, m, ((66, 176, 120), (30, 128, 84)))
    d = D4(im)
    gold = (232, 196, 96, 255)
    for x in (22, 28, 34, 40):
        d.line([(x, 12), (x, 18)], fill=gold, width=2)
        d.line([(x, 46), (x, 52)], fill=gold, width=2)
    for y in (24, 30, 36):
        d.line([(12, y), (18, y)], fill=gold, width=2)
        d.line([(46, y), (52, y)], fill=gold, width=2)
    chip = canvas()
    fill(chip, m_rr(18, 18, 46, 44, 3), ((60, 66, 80), (30, 34, 44)))
    D4(chip).ellipse((21, 21, 24, 24), fill=(150, 160, 178, 255))
    im = Image.alpha_composite(im, shadow(chip, dy=1, blur=1, alpha=80))
    return finish(shadow(im))


def bootmgr():
    im, m = window(bar=(72, 96, 160))
    d = D4(im)
    for i, y in enumerate((23, 32, 41)):
        on = i == 1
        if on:
            d.rounded_rectangle((10, y - 2, 54, y + 6), radius=3, fill=(72, 128, 230, 255))
        d.polygon([(14, y), (19, y + 2), (14, y + 4)], fill=WHITE if on else (150, 160, 180, 255))
        d.rounded_rectangle((23, y + 1, 46 if i != 2 else 40, y + 3.4), radius=1.2,
                            fill=WHITE if on else (150, 162, 184, 255))
    return finish(shadow(im))


def sysupdate():
    im = canvas()
    m = m_ellipse(6, 6, 57, 57)
    fill(im, m, ((72, 206, 140), (24, 150, 96)))
    d = D4(im)
    d.arc((16, 16, 47, 47), 200, 500, fill=WHITE, width=4.5)
    d.polygon([(11, 30), (21, 30), (16, 38)], fill=WHITE)
    d.rounded_rectangle((30, 20, 33.5, 36), radius=1.5, fill=WHITE)
    d.polygon([(25, 32), (38.5, 32), (31.75, 40)], fill=WHITE)
    return finish(shadow(im))


def diskmark():
    im = canvas()
    m = m_rr(6, 30, 58, 56, 6)
    fill(im, m, ((226, 230, 238), (176, 184, 198)))
    gauge = m_minus(m_ellipse(10, 6, 54, 50), m_rr(0, 29, 64, 64, 0))
    fill(im, gauge, ((70, 82, 112), (40, 48, 70)))
    d = D4(im)
    d.arc((14, 10, 50, 46), 200, 340, fill=(80, 210, 140, 255), width=3)
    d.arc((14, 10, 50, 46), 300, 340, fill=(255, 110, 80, 255), width=3)
    d.line([(32, 28), (43, 17)], fill=WHITE, width=2.5)
    d.ellipse((29, 25, 35, 31), fill=WHITE)
    d.ellipse((47, 41, 52, 46), fill=(60, 190, 110, 255))
    d.rounded_rectangle((12, 42, 34, 44.5), radius=1.25, fill=(150, 158, 174, 255))
    return finish(shadow(im))


def properties():
    im, m = page()
    lines(D4(im), 20, [(18, 40), (25, 44), (32, 36)])
    badge = canvas()
    fill(badge, m_ellipse(30, 34, 56, 60), ((96, 156, 255), (44, 100, 222)))
    d = D4(badge)
    d.ellipse((41.5, 38, 44.5, 41), fill=WHITE)
    d.rounded_rectangle((41.5, 43, 44.5, 55), radius=1.5, fill=WHITE)
    im = Image.alpha_composite(im, shadow(badge, dy=1, blur=1, alpha=70))
    return finish(shadow(im))


# =====================================================================
# FOLDERS AND FILES
# =====================================================================

def plain_folder():
    im, _ = folder((250, 192, 60), (226, 156, 34))
    return finish(shadow(im))


def kind_folder(body, tab, emblem):
    """A folder of a known kind -- Plasma's folder-music, Explorer's
    special folders: the folder in the kind's colour, its emblem in white
    on the front. `emblem(d, ink)` is a 64-px drawing (gen_icons.py's)."""
    im, _ = folder(body, tab)
    emblem(D4(im), WHITE)
    return finish(shadow(im))


def plain_file():
    im, _ = page()
    lines(D4(im), 20, [(28, 44), (35, 44), (42, 38)])
    return finish(shadow(im))


def typed_file(draw):
    """A page with a type's emblem drawn on it in the 64-px space."""
    im, _ = page()
    draw(D4(im))
    return finish(shadow(im))


# The type emblems, drawn on the page in the 64-px space.

def _text(d):
    lines(d, 20, [(26, 44), (33, 44), (40, 44), (47, 36)])


def _doc(d):
    d.rounded_rectangle((20, 24, 40, 29), radius=1.5, fill=(50, 104, 220, 255))
    lines(d, 20, [(35, 44), (42, 44), (49, 36)])


def _image(d):
    d.rounded_rectangle((19, 25, 45, 49), radius=3, fill=(116, 186, 250, 255))
    d.ellipse((35, 28, 41, 34), fill=(255, 214, 80, 255))
    d.polygon([(19, 49), (28, 36), (34, 43), (38, 39), (45, 49)], fill=(50, 170, 104, 255))


def _audio(d):
    ink = (232, 110, 40, 255)
    d.rounded_rectangle((35, 25, 38, 44), radius=1.5, fill=ink)
    d.polygon([(36, 25), (45, 28), (45, 33), (36, 30)], fill=ink)
    d.ellipse((25, 39, 37, 50), fill=ink)


def _config(d):
    ink = (36, 150, 140, 255)
    d.rounded_rectangle((20, 31, 44, 33.5), radius=1.25, fill=ink)
    d.rounded_rectangle((20, 43, 44, 45.5), radius=1.25, fill=ink)
    d.ellipse((24, 27, 33, 36), fill=WHITE, outline=ink, width=3)
    d.ellipse((32, 39, 41, 48), fill=WHITE, outline=ink, width=3)


def _font(d):
    ink = (56, 66, 96, 255)
    d.line([(22, 50), (32, 25)], fill=ink, width=4)
    d.line([(32, 25), (42, 50)], fill=ink, width=4)
    d.line([(26, 42), (38, 42)], fill=ink, width=3)


def _app(d):
    d.rounded_rectangle((21, 28, 43, 50), radius=5, fill=(50, 104, 220, 255))
    d.rounded_rectangle((26, 37, 38, 41), radius=1.5, fill=WHITE)


FILE_TYPES = {"file-text": _text, "file-doc": _doc, "file-image": _image, "file-audio": _audio,
              "file-config": _config, "file-font": _font, "file-app": _app}


def drive():
    im = canvas()
    m = m_rr(6, 20, 58, 46, 6)
    fill(im, m, ((236, 239, 244), (188, 196, 210)))
    edge(im, m, (150, 160, 178))
    fill(im, m_and(m, m_rr(6, 36, 58, 46, 0)), ((176, 184, 200), (150, 160, 178)))
    d = D4(im)
    d.ellipse((46, 25, 51, 30), fill=(60, 190, 110, 255))
    d.rounded_rectangle((12, 26, 32, 28.5), radius=1.25, fill=(150, 158, 174, 255))
    return finish(shadow(im))


def drive_ram():
    im = canvas()
    d = D4(im)
    gold = (220, 182, 80, 255)
    for x in (21, 27, 33, 39):
        d.line([(x, 8), (x, 15)], fill=gold, width=2.5)
        d.line([(x, 49), (x, 56)], fill=gold, width=2.5)
    m = m_rr(14, 14, 50, 50, 4)
    fill(im, m, ((126, 106, 196), (84, 66, 156)))
    D4(im).ellipse((18, 18, 22, 22), fill=(200, 190, 240, 255))
    return finish(shadow(im))


# =====================================================================
# CATEGORIES (the Start menu's sidebar and System Settings')
#
# Read at about a text row's height beside a bold label, so each is ONE
# bold object in a strong hue, with no detail finer than the 20-px row
# can hold -- the hue still carries most of the recognition.
# =====================================================================

def disc(top, bottom, glyph):
    im = canvas()
    fill(im, m_ellipse(5, 5, 58, 58), (top, bottom))
    glyph(D4(im))
    return finish(shadow(im))


def rounded(top, bottom, glyph, box=(6, 6, 57, 57), r=12):
    im = canvas()
    fill(im, m_rr(*box, r), (top, bottom))
    glyph(D4(im))
    return finish(shadow(im))


def _clock(d):
    d.ellipse((13, 13, 50, 50), fill=WHITE)
    d.line([(32, 32), (32, 19)], fill=(40, 90, 170, 255), width=4)
    d.line([(32, 32), (41, 37)], fill=(40, 90, 170, 255), width=4)


def cat_time():
    return disc((90, 156, 240), (40, 96, 200), _clock)


def cat_appearance():
    def g(d):
        d.ellipse((13, 13, 50, 50), outline=WHITE, width=5)
        d.pieslice((13, 13, 50, 50), 90, 270, fill=WHITE)
    return disc((196, 130, 240), (130, 70, 200), g)


def cat_input():
    im = canvas()
    fill(im, m_rr(4, 16, 59, 48, 7), ((250, 251, 253), (212, 218, 230)))
    d = D4(im)
    for r, y in enumerate((22, 30)):
        for c in range(6):
            d.rounded_rectangle((9 + c * 8, y, 14 + c * 8, y + 5), radius=1.5, fill=(150, 160, 180, 255))
    d.rounded_rectangle((16, 38, 47, 43), radius=1.5, fill=(80, 170, 120, 255))
    return finish(shadow(im))


def cat_shortcuts():
    def g(d):
        d.rounded_rectangle((11, 18, 30, 44), radius=4, fill=WHITE)
        d.rounded_rectangle((34, 18, 53, 44), radius=4, fill=WHITE)
        d.line([(16, 33), (25, 33)], fill=(200, 120, 40, 255), width=3)
        d.line([(43.5, 25), (43.5, 38)], fill=(200, 120, 40, 255), width=3)
        d.line([(37, 31.5), (50, 31.5)], fill=(200, 120, 40, 255), width=3)
    return rounded((255, 176, 70), (226, 120, 30), g)


def cat_kernel():
    def g(d):
        gold = (255, 236, 180, 255)
        for x in (22, 28, 34, 40):
            d.line([(x, 10), (x, 16)], fill=gold, width=2.5)
            d.line([(x, 47), (x, 53)], fill=gold, width=2.5)
        for y in (22, 28, 34, 40):
            d.line([(10, y), (16, y)], fill=gold, width=2.5)
            d.line([(47, y), (53, y)], fill=gold, width=2.5)
        d.rounded_rectangle((18, 18, 45, 45), radius=4, fill=WHITE)
        d.rounded_rectangle((25, 25, 38, 38), radius=2, fill=(214, 100, 80, 255))
    return rounded((240, 112, 90), (196, 60, 52), g)


def cat_display():
    im = canvas()
    fill(im, m_rr(5, 8, 58, 44, 5), ((60, 66, 80), (34, 38, 48)))
    fill(im, m_rr(9, 12, 54, 40, 2), ((110, 190, 255), (60, 130, 230)))
    d = D4(im)
    d.rounded_rectangle((28, 44, 35, 51), radius=1, fill=(120, 128, 144, 255))
    d.rounded_rectangle((18, 51, 45, 55), radius=2, fill=(150, 158, 174, 255))
    return finish(shadow(im))


def cat_storage():
    im = canvas()
    for i, y in enumerate((8, 24, 40)):
        m = m_rr(8, y, 55, y + 14, 4)
        fill(im, m, ((236, 239, 244), (188, 196, 210)))
        D4(im).ellipse((44, y + 4.5, 49, y + 9.5), fill=(60, 190, 110, 255) if i == 0 else (150, 160, 178, 255))
    return finish(shadow(im))


def cat_system():
    im = canvas()
    m = gear_mask(32, 32, 26, 20, 8, 8)
    fill(im, m, ((150, 162, 184), (86, 96, 118)))
    return finish(shadow(im))


def cat_favourites():
    im = canvas()
    pts = []
    for i in range(10):
        a = -math.pi / 2 + i * math.pi / 5
        r = 28 if i % 2 == 0 else 13
        pts += [32 + r * math.cos(a), 34 + r * math.sin(a)]
    fill(im, m_poly(*pts), ((255, 214, 70), (246, 160, 30)))
    return finish(shadow(im))


def cat_recent():
    def g(d):
        d.ellipse((13, 13, 50, 50), fill=WHITE)
        d.line([(32, 32), (32, 20)], fill=(40, 150, 140, 255), width=4)
        d.line([(32, 32), (24, 37)], fill=(40, 150, 140, 255), width=4)
    return disc((76, 206, 190), (30, 150, 140), g)


def cat_all():
    im = canvas()
    cols = [((92, 156, 255), (46, 106, 230)), ((255, 176, 64), (240, 120, 30)),
            ((88, 210, 140), (36, 160, 96)), ((240, 112, 90), (196, 60, 52))]
    for i, (x, y) in enumerate(((6, 6), (34, 6), (6, 34), (34, 34))):
        fill(im, m_rr(x, y, x + 23, y + 23, 6), cols[i])
    return finish(shadow(im))


def cat_utility():
    im = canvas()
    fill(im, m_rr(10, 4, 54, 60, 7), ((250, 251, 253), (214, 220, 232)))
    d = D4(im)
    d.rounded_rectangle((15, 9, 49, 21), radius=3, fill=(46, 58, 78, 255))
    for r in range(3):
        for c in range(3):
            d.rounded_rectangle((15 + c * 12, 26 + r * 11, 24 + c * 12, 34 + r * 11), radius=2,
                                fill=(255, 140, 40, 255) if (r, c) == (2, 2) else (176, 184, 200, 255))
    return finish(shadow(im))


def cat_graphics():
    im = canvas()
    m = m_rr(5, 9, 58, 55, 7)
    fill(im, m, ((206, 140, 255), (140, 80, 220)))
    d = D4(im)
    d.ellipse((38, 15, 48, 25), fill=(255, 220, 100, 255))
    fill(im, m_and(m, m_poly(5, 55, 22, 32, 35, 46, 43, 38, 58, 52, 58, 55)), ((255, 255, 255), (230, 220, 250)))
    return finish(shadow(im))


def cat_multimedia():
    def g(d):
        d.polygon([(24, 17), (47, 32), (24, 47)], fill=WHITE)
    return disc((255, 122, 92), (222, 48, 92), g)


def cat_games():
    im = canvas()
    m = m_or(m_rr(5, 16, 59, 46, 15), m_or(m_ellipse(4, 26, 26, 56), m_ellipse(38, 26, 60, 56)))
    fill(im, m, ((120, 132, 160), (66, 76, 100)))
    d = D4(im)
    d.rounded_rectangle((13, 28, 26, 32), radius=1.5, fill=WHITE)
    d.rounded_rectangle((17.5, 23.5, 21.5, 36.5), radius=1.5, fill=WHITE)
    d.ellipse((41, 22, 47, 28), fill=(255, 110, 100, 255))
    d.ellipse((47, 29, 53, 35), fill=(110, 200, 255, 255))
    return finish(shadow(im))


def cat_development():
    def g(d):
        d.line([(22, 20), (12, 32), (22, 44)], fill=WHITE, width=5)
        d.line([(42, 20), (52, 32), (42, 44)], fill=WHITE, width=5)
        d.line([(36, 16), (28, 48)], fill=(255, 255, 255, 190), width=4)
    return rounded((126, 112, 230), (80, 66, 190), g)


def cat_desktop():
    im = canvas()
    fill(im, m_rr(5, 8, 58, 44, 5), ((60, 66, 80), (34, 38, 48)))
    fill(im, m_rr(9, 12, 54, 40, 2), ((70, 160, 160), (30, 110, 120)))
    d = D4(im)
    for x in (13, 21):
        d.rounded_rectangle((x, 16, x + 5, 21), radius=1.5, fill=(255, 214, 90, 255))
    d.rounded_rectangle((9, 35, 54, 40), radius=0, fill=(30, 34, 44, 255))
    d.rounded_rectangle((28, 44, 35, 51), radius=1, fill=(120, 128, 144, 255))
    d.rounded_rectangle((18, 51, 45, 55), radius=2, fill=(150, 158, 174, 255))
    return finish(shadow(im))


def cat_network():
    def g(d):
        d.line([(32, 20), (18, 42)], fill=WHITE, width=3.5)
        d.line([(32, 20), (46, 42)], fill=WHITE, width=3.5)
        d.line([(18, 42), (46, 42)], fill=WHITE, width=3.5)
        for x, y in ((32, 20), (18, 42), (46, 42)):
            d.ellipse((x - 6, y - 6, x + 6, y + 6), fill=WHITE)
    return disc((76, 170, 255), (36, 110, 220), g)


def cat_sound():
    def g(d):
        d.polygon([(15, 26), (23, 26), (32, 18), (32, 46), (23, 38), (15, 38)], fill=WHITE)
        d.arc((28, 22, 42, 42), -55, 55, fill=WHITE, width=3.5)
        d.arc((28, 15, 50, 49), -55, 55, fill=WHITE, width=3.5)
    return disc((255, 150, 80), (230, 90, 40), g)


# The names gen_icons.py ships each one under (data/icons/<name>.qoi).
ART = {
    "notepad": notepad, "files": files, "terminal": terminal, "calculator": calculator,
    "imgview": imgview, "player": player, "settings": settings, "taskmgr": taskmgr,
    "mines": mines, "screenshot": screenshot, "about": about, "help": help_app,
    "logview": logview, "crashreports": crashreports, "shapes": shapes, "fontdemo": fontdemo,
    "uidemo": uidemo, "doom": doom, "devmgr": devmgr, "bootmgr": bootmgr,
    "sysupdate": sysupdate, "diskmark": diskmark, "properties": properties,
    "folder": plain_folder, "file": plain_file, "drive": drive, "drive-ram": drive_ram,
    "cat-time": cat_time, "cat-appearance": cat_appearance, "cat-input": cat_input,
    "cat-shortcuts": cat_shortcuts, "cat-kernel": cat_kernel, "cat-display": cat_display,
    "cat-storage": cat_storage, "cat-system": cat_system, "cat-favourites": cat_favourites,
    "cat-recent": cat_recent, "cat-all": cat_all, "cat-utility": cat_utility,
    "cat-graphics": cat_graphics, "cat-multimedia": cat_multimedia, "cat-games": cat_games,
    "cat-development": cat_development, "cat-desktop": cat_desktop, "cat-network": cat_network,
    "cat-sound": cat_sound,
}
for _name, _draw in FILE_TYPES.items():
    ART[_name] = (lambda dr=_draw: typed_file(dr))
