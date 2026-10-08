#!/usr/bin/env python3
"""Generate the image data this repo ships: the test vectors and the wallpapers.

TWO OUTPUTS, ONE TOOL, because both are "a JPEG produced by libjpeg for
toy-os to read back" and splitting them would mean two scripts with the
same encoder settings to keep in step.

  --vectors     userland/tests/uimg_vectors.h -- small JPEGs and QOIs as
                byte arrays, each with the RGBA **Pillow** decodes them
                to. That is the whole point of the file: /tests/uimg_test
                compares userland/lib/uimg_*.c's output against a
                different implementation's, not against its own. A
                decoder tested only against itself is the AES-with-a-
                wrong-round-key problem docs/roadmap-details.md warns
                about -- self-consistent and worthless.

                THE QOI VECTORS ARE ENCODED BY PILLOW TOO, which matters
                more than it does for JPEG: QOI is simple enough that
                this repo could have written its own encoder, and then a
                misread chunk type would round-trip perfectly through
                the matching bug. A foreign encoder cannot do that.
                Their tolerance is 0 -- QOI is lossless, so "close
                enough" is not a thing that exists.

                BMP and GIF the same way, Pillow writing and reading,
                tolerance 0; and one ANIMATION, every frame as Pillow
                composites it, for the frame iterator. The breadth for
                both lives in tools/uimg_codec_hostcheck.py; these few
                are what proves the codecs in ring 3.

  --wallpapers  data/wallpapers/*.jpg and animated/rain.gif -- the backgrounds, drawn
                here rather than committed as somebody's photograph so
                the repo carries no image it does not own.

  --pictures    data/usr/share/pictures/ -- sample BMPs and GIFs, so a
                machine can open each variant the decoders read with
                no file brought from elsewhere: 24-bit, 8-bit palette,
                RLE8 and a 32-bit alpha BMP; a still GIF and two
                animated ones, one over transparency. The RLE8 and alpha
                BMPs are packed HERE, since Pillow writes neither.

(The app icons have their own generator, tools/gen_icons.py, because
what they need is drawing code rather than encoder settings.)

Both outputs are COMMITTED, like kernel/drivers/font_ttf.c and the
cursor themes: a checkout without Pillow still builds. Re-run this only
when the vectors or the wallpapers should actually change, and say so in
the commit message when you do.

Needs Pillow (see docs/tools.md's host-tools section).
"""
import argparse
import io
import math
import random
import os
import struct
import sys

try:
    from PIL import Image, ImageOps, ImageDraw, ImageFilter
except ImportError:
    sys.exit("gen_imgdata.py needs Pillow: pip install --user pillow")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VECTORS_H = os.path.join(ROOT, "userland", "tests", "uimg_vectors.h")
WALLPAPER_DIR = os.path.join(ROOT, "data", "wallpapers")

# errno values from kernel/include/abi/errno.h, as POSITIVE numbers; the
# test compares them against a negative return.
EINVAL = 22
ENOTSUP = 95
# ENOTSUP (95) had a vector here until progressive JPEG stopped being a
# refusal; the remaining -ENOTSUP paths (arithmetic coding, 12-bit, CMYK)
# have no encoder here to produce a file with.


def gradient(w, h):
    """A smooth two-axis gradient with a hard edge in it.

    Both halves matter. The gradient exercises the DC path and the
    chroma resampler; the hard edge is what produces large AC
    coefficients, so an IDCT with a wrong constant shows up there and
    nowhere else -- a decoder that only ever sees flat colour passes
    with the entire butterfly deleted.
    """
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            r = 255 * x // max(1, w - 1)
            g = 255 * y // max(1, h - 1)
            b = 128 + 127 * int(math.sin(6.0 * x / max(1, w)) >= 0)
            if x > w // 2 and y > h // 2:
                r, g, b = 250, 12, 30       # the hard edge
            px[x, y] = (r, g, b)
    return im


def noise(w, h):
    """Deterministic per-pixel noise -- no two neighbours alike.

    For QOI this is the anti-run image: almost every pixel is a literal
    or an index hit, so the 64-entry hash table is actually exercised.
    """
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            s = (x * 1103515245 + y * 12345 + 7) & 0x7FFFFFFF
            px[x, y] = ((s >> 7) & 255, (s >> 15) & 255, (s >> 3) & 255)
    return im


def alpha_disc(w, h):
    """A soft-edged disc on transparency -- an icon's shape, in miniature.

    The point is the EDGE: alpha steps through every value between 0 and
    255 there, so a decoder that dropped the alpha channel, or one that
    read RGBA chunks as RGB, is wrong in a way an all-or-nothing mask
    would hide.
    """
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    px = im.load()
    cx, cy, r = (w - 1) / 2.0, (h - 1) / 2.0, min(w, h) * 0.42
    for y in range(h):
        for x in range(w):
            d = math.hypot(x - cx, y - cy)
            a = 255 if d <= r - 1 else (0 if d >= r + 1 else int(255 * (r + 1 - d) / 2))
            if a:
                px[x, y] = (240, 90 + int(120 * x / w), 40, a)
    return im


def encode(im, **kw):
    buf = io.BytesIO()
    im.save(buf, format="JPEG", **kw)
    return buf.getvalue()


def c_bytes(name, data):
    out = ["static const unsigned char %s[] = {" % name]
    for i in range(0, len(data), 16):
        row = ", ".join("0x%02x" % b for b in data[i:i + 16])
        out.append("    %s," % row)
    out.append("};")
    return "\n".join(out)


def build_vectors():
    vectors = []

    def add(name, data, tol, exif=False):
        """One decodable vector: the bytes, and what PILLOW decodes them to.

        `exif` turns the reference the way Pillow's own
        ImageOps.exif_transpose() does. The decoder applies the
        orientation tag (docs/decisions/gui.md), so a reference that did
        not would fail every rotated file and pass a decoder that
        ignored the tag -- the assertion backwards.
        """
        ref = Image.open(io.BytesIO(data))
        if exif:
            ref = ImageOps.exif_transpose(ref)
        ref = ref.convert("RGBA")
        vectors.append({
            "name": name, "jpeg": data, "w": ref.width, "h": ref.height,
            "rgb": ref.tobytes(), "err": 0, "tol": tol,
        })

    def ok(name, im, **kw):
        # JPEG: lossy, so the tolerance is the level two conforming IDCTs
        # are allowed to differ by. Alpha is still compared EXACTLY --
        # the decoder must fill 0xFF, and a vector that ignored alpha
        # would not notice if it filled 0.
        add(name, encode(im, **kw), 3)

    def qoi(name, im):
        buf = io.BytesIO()
        im.save(buf, format="QOI")
        add(name, buf.getvalue(), 0)   # lossless: exact, or it is wrong

    def bad(name, data, err):
        vectors.append({"name": name, "jpeg": data, "w": 0, "h": 0,
                        "rgb": None, "err": err, "tol": 0})

    ok("rgb 4:4:4 q95", gradient(16, 16), quality=95, subsampling=0)
    ok("rgb 4:2:2 q90", gradient(24, 16), quality=90, subsampling=1)
    ok("rgb 4:2:0 q80", gradient(32, 24), quality=80, subsampling=2)
    # 17x9 is deliberately not a multiple of an MCU in either axis: the
    # decoder pads its planes out to whole MCUs and must crop back, and
    # an off-by-one there produces a picture with a garbage right/bottom
    # edge that a square test image cannot show.
    ok("partial MCU 17x9", gradient(17, 9), quality=85, subsampling=2)
    ok("grayscale", gradient(16, 16).convert("L"), quality=90)
    # Restart markers reset the DC predictor mid-scan. A decoder that
    # ignores them decodes the first interval correctly and then drifts,
    # so this vector fails in bands rather than everywhere.
    ok("restart markers", gradient(32, 32), quality=85, subsampling=2,
       restart_marker_rows=1)

    # --- QOI, whose chunk types are what there is to get wrong --------
    #
    # Each image is shaped to force a different chunk: flat bands are
    # RUNs, a gradient is DIFF/LUMA, noise is literal RGB plus INDEX
    # hits off the 64-entry hash, and the alpha one is RGBA chunks. An
    # image that exercised only runs would pass with the hash function
    # wrong, which is this format's subtle failure.
    qoi("qoi flat runs", Image.new("RGB", (24, 16), (30, 90, 160)))
    qoi("qoi gradient", gradient(24, 16))
    qoi("qoi noise", noise(20, 12))
    qoi("qoi rgb, no alpha", gradient(16, 16).convert("RGB"))
    qoi("qoi with alpha", alpha_disc(24, 24))

    # PROGRESSIVE DECODES NOW. It is here as a vector rather than as a
    # refusal because it used to be the refusal, and a build that
    # regressed to one would otherwise look like a passing suite with a
    # missing feature. Noise rather than a gradient on purpose: a smooth
    # ramp has almost no AC coefficients, so the refinement scans this
    # is meant to cover carry nothing.
    ok("progressive 4:2:0", noise(32, 24), quality=85, subsampling=2,
       progressive=True)
    ok("progressive 4:4:4", noise(17, 9), quality=92, subsampling=0,
       progressive=True)

    # EXIF ORIENTATION 6 -- a quarter turn, so the DIMENSIONS swap. A
    # decoder ignoring the tag fails on the size before it gets to a
    # pixel, which is the loudest way for this to break.
    ex = Image.Exif()
    ex[0x0112] = 6
    add("exif orientation 6", encode(noise(24, 16), quality=92,
                                     exif=ex.tobytes()), 3, exif=True)
    # --- BMP and GIF -------------------------------------------------
    #
    # 13 wide on purpose: a BMP row pads to four bytes, and a width that
    # needs no padding cannot show a decoder that forgot it.
    def saved(im, fmt, **kw):
        buf = io.BytesIO()
        im.save(buf, format=fmt, **kw)
        return buf.getvalue()

    add("bmp 24-bit", saved(gradient(13, 7), "BMP"), 0)
    add("bmp 8-bit palette", saved(noise(13, 7).quantize(37), "BMP"), 0)
    add("bmp 1-bit", saved(gradient(13, 7).convert("1"), "BMP"), 0)
    add("gif 256 colours", saved(noise(20, 12).quantize(256), "GIF"), 0)
    add("gif interlaced", saved(gradient(24, 17).quantize(60), "GIF", interlace=True), 0)

    truncated = vectors[2]["jpeg"][:len(vectors[2]["jpeg"]) * 6 // 10]
    bad("truncated is refused", truncated, EINVAL)
    bad("not an image", b"this is not a JPEG, not even slightly\n", EINVAL)

    # A QOI truncated mid-chunk, and one whose header lies about its
    # channel count. Byte-level corruption rather than a short file,
    # because a decoder that trusts the header is the one that walks off
    # the end of the buffer.
    buf = io.BytesIO()
    alpha_disc(24, 24).save(buf, format="QOI")
    whole = buf.getvalue()
    bad("qoi truncated is refused", whole[:len(whole) * 6 // 10], EINVAL)
    bogus = bytearray(whole)
    bogus[12] = 7          # channels: legal values are 3 and 4
    bad("qoi with an impossible channel count", bytes(bogus), EINVAL)

    whole = saved(gradient(13, 7), "BMP")
    bad("bmp truncated is refused", whole[:len(whole) - 20], EINVAL)
    # A BMP that wraps a JPEG (BI_JPEG, a printer format): fine, refused.
    jpg = bytearray(whole[:54])
    jpg[30:34] = struct.pack("<I", 4)
    bad("bmp wrapping a JPEG is unsupported", bytes(jpg) + b"\xff\xd8\xff\xd9", ENOTSUP)
    gif = saved(noise(20, 12).quantize(256), "GIF")
    bad("gif truncated is refused", gif[:len(gif) // 2], EINVAL)

    return vectors


def build_anim_vectors():
    """One animation: three frames, disposals 2, 3 and 1 over a
    transparent index, and a delay of 0 that uimg_anim_next() must turn
    into 100 ms. The reference is each frame as PILLOW composites it,
    with its fully transparent pixels written as 0 -- Pillow keeps the
    palette's colour under alpha 0, the decoder writes nothing there."""
    pal = [255, 0, 0, 0, 255, 0, 0, 0, 255, 9, 9, 9] + [0] * 756
    frames = []
    for k in range(3):
        f = Image.new("P", (12, 9))
        f.putpalette(pal)
        for y in range(9):
            for x in range(12):
                f.putpixel((x, y), 3 if (k == 1 and x < 5) else (x + y + k) % 3)
        frames.append(f)
    buf = io.BytesIO()
    frames[0].save(buf, format="GIF", save_all=True, append_images=frames[1:],
                   duration=[50, 0, 90], loop=0, disposal=[2, 3, 1],
                   transparency=3, optimize=False)
    data = buf.getvalue()
    ref = Image.open(io.BytesIO(data))
    rgba = bytearray()
    for k in range(ref.n_frames):
        ref.seek(k)
        raw = ref.convert("RGBA").tobytes()
        for i in range(0, len(raw), 4):
            rgba += raw[i:i + 4] if raw[i + 3] else bytes(4)
    return [{"name": "gif three frames, disposals 2 3 1", "data": data,
             "w": ref.width, "h": ref.height, "frames": ref.n_frames, "loops": 0,
             "delays": [50, 100, 90], "rgba": bytes(rgba)}]


def write_vectors(vectors, anims):
    out = ['// GENERATED by tools/gen_imgdata.py -- do not edit by hand.',
           '//',
           '// Each vector is a file and the RGBA **Pillow** decodes it to --',
           '// libjpeg for the JPEGs, its own QOI plugin for the QOIs, and it',
           '// ENCODED the QOIs too. /tests/uimg_test decodes the same bytes',
           '// with userland/lib/uimg_*.c and compares, which is the only way',
           '// to tell a correct decoder from a self-consistent one.',
           '#ifndef UIMG_VECTORS_H',
           '#define UIMG_VECTORS_H',
           '',
           'struct uimg_vector {',
           '    const char *name;',
           '    const unsigned char *data;   // the FILE, whatever format it is',
           '    unsigned len;',
           '    int w, h;                  // expected size; 0 when err != 0',
           '    int err;                   // 0 = must decode; else the POSITIVE',
           '                               // errno the decoder must return',
           '    int tol;                   // largest per-channel difference to',
           '                               // accept: 3 for JPEG (two conforming',
           '                               // IDCTs may differ), 0 for QOI',
           '    const unsigned char *rgba; // w*h*4 reference pixels, or 0',
           '};',
           '']
    for i, v in enumerate(vectors):
        out.append(c_bytes("uimg_vec%d_file" % i, v["jpeg"]))
        if v["rgb"] is not None:
            out.append(c_bytes("uimg_vec%d_rgba" % i, v["rgb"]))
        out.append("")
    out.append("static const struct uimg_vector uimg_vectors[] = {")
    for i, v in enumerate(vectors):
        rgb = ("uimg_vec%d_rgba" % i) if v["rgb"] is not None else "0"
        out.append('    { "%s", uimg_vec%d_file, sizeof uimg_vec%d_file, '
                   '%d, %d, %d, %d, %s },' %
                   (v["name"], i, i, v["w"], v["h"], v["err"], v["tol"], rgb))
    out.append("};")
    out.append("")
    out.append("#define UIMG_VECTOR_COUNT "
               "((int)(sizeof uimg_vectors / sizeof uimg_vectors[0]))")
    out.append("")
    out += ['// An animation, stepped through uimg_anim_next(): every frame as',
            '// Pillow composites it, and each delay AFTER that call\'s clamp.',
            'struct uimg_anim_vector {',
            '    const char *name;',
            '    const unsigned char *data;',
            '    unsigned len;',
            '    int w, h, frames, loops;',
            '    const int *delays;',
            '    const unsigned char *rgba; // frames * w * h * 4',
            '};',
            '']
    for i, a in enumerate(anims):
        out.append(c_bytes("uimg_anim%d_file" % i, a["data"]))
        out.append(c_bytes("uimg_anim%d_rgba" % i, a["rgba"]))
        out.append("static const int uimg_anim%d_delays[] = { %s };"
                   % (i, ", ".join(str(d) for d in a["delays"])))
        out.append("")
    out.append("static const struct uimg_anim_vector uimg_anim_vectors[] = {")
    for i, a in enumerate(anims):
        out.append('    { "%s", uimg_anim%d_file, sizeof uimg_anim%d_file, %d, %d, %d, %d, '
                   'uimg_anim%d_delays, uimg_anim%d_rgba },'
                   % (a["name"], i, i, a["w"], a["h"], a["frames"], a["loops"], i, i))
    out.append("};")
    out.append("")
    out.append("#define UIMG_ANIM_VECTOR_COUNT "
               "((int)(sizeof uimg_anim_vectors / sizeof uimg_anim_vectors[0]))")
    out.append("")
    out.append("#endif")
    out.append("")
    with open(VECTORS_H, "w") as f:
        f.write("\n".join(out))
    total = sum(len(v["jpeg"]) for v in vectors)
    print("wrote %s: %d vectors, %d bytes of JPEG" %
          (os.path.relpath(VECTORS_H, ROOT), len(vectors), total))


# --- wallpapers -------------------------------------------------------
#
# 1280x720 is this project's default mode, so the common case needs no
# scaling at all. That was a real question rather than an obvious one --
# a half-size image would decode in a quarter of the time -- and it was
# settled by MEASURING instead: `imginfo -d` reports 50 ms for 960x540
# under TCG, so a full-size background costs the desktop about a tenth
# of a second at startup, once. At a different resolution uimg_scale()
# resamples it, which is what tools/hires_test.py's mode exercises.

def wallpaper_aurora(w, h):
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / (h - 1)
        d.line([(0, y), (w, y)],
               fill=(int(10 + 26 * t), int(22 + 60 * t), int(46 + 78 * t)))
    for i in range(5):
        phase = i * 0.7
        amp = h * (0.05 + 0.02 * i)
        pts = []
        for x in range(0, w + 8, 8):
            u = x / w
            y = h * (0.30 + 0.09 * i) + amp * math.sin(6.3 * u + phase)
            pts.append((x, y))
        pts += [(w, h), (0, h)]
        d.polygon(pts, fill=(int(14 + 8 * i), int(60 + 14 * i), int(96 + 10 * i)))
    return im.filter(ImageFilter.GaussianBlur(9))


def wallpaper_dusk(w, h):
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / (h - 1)
        d.line([(0, y), (w, y)],
               fill=(int(48 + 150 * (1 - t) ** 2), int(30 + 70 * (1 - t)),
                     int(60 + 40 * t)))
    d.ellipse([w * 0.62, h * 0.12, w * 0.62 + h * 0.30, h * 0.12 + h * 0.30],
              fill=(255, 214, 170))
    for i in range(4):
        base = h * (0.62 + 0.09 * i)
        pts = [(0, h), (0, base)]
        for x in range(0, w + 12, 12):
            u = x / w
            pts.append((x, base - h * 0.06 * math.sin(3.0 * u + i * 1.3) ** 2))
        pts.append((w, h))
        shade = 46 - 9 * i
        d.polygon(pts, fill=(shade, shade - 6, shade + 14))
    return im.filter(ImageFilter.GaussianBlur(2))


# THREE MORE, AND THE CONSTRAINT IS THE DESKTOP RATHER THAN THE PICTURE.
# Icons and their captions are drawn over these, so each keeps its
# detail LOW-FREQUENCY and its top-left quiet -- that is where the icon
# grid starts. Busy texture there costs legibility and JPEG bytes at
# once; `ugfx_draw_string_shadowed()` covers the rest.

def wallpaper_slate(w, h):
    """A cool neutral with a faint grid -- the quiet one, for reading."""
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / (h - 1)
        d.line([(0, y), (w, y)],
               fill=(int(36 + 22 * t), int(41 + 26 * t), int(50 + 32 * t)))
    # A grid a couple of units above the ground it sits on: visible as
    # structure, never as lines to read past.
    step = h // 12
    for x in range(0, w, step):
        d.line([(x, 0), (x, h)], fill=(52, 58, 70))
    for y in range(0, h, step):
        d.line([(0, y), (w, y)], fill=(52, 58, 70))
    # One soft diagonal light, bottom-right, away from the icon grid.
    glow = Image.new("RGB", (w, h), (0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse([w * 0.55, h * 0.45, w * 1.25, h * 1.35], fill=(40, 46, 58))
    glow = glow.filter(ImageFilter.GaussianBlur(h // 6))
    return Image.blend(im, Image.blend(im, glow, 0.5), 0.6)


def wallpaper_ember(w, h):
    """Warm and dark: a low horizon glow under a near-black sky."""
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / (h - 1)
        # The warmth arrives late, so the top two thirds stay dark
        # enough for white icon captions.
        k = max(0.0, (t - 0.45) / 0.55) ** 2
        d.line([(0, y), (w, y)],
               fill=(int(18 + 150 * k), int(16 + 62 * k), int(20 + 30 * k)))
    for i in range(6):
        r = h * (0.18 + 0.13 * i)
        cx, cy = w * 0.5, h * 1.02
        d.ellipse([cx - r * 1.6, cy - r, cx + r * 1.6, cy + r],
                  outline=(70 + 14 * i, 34 + 8 * i, 26), width=3)
    return im.filter(ImageFilter.GaussianBlur(h // 55))


def wallpaper_tide(w, h):
    """Daylight teal: layered water, lighter than aurora."""
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / (h - 1)
        d.line([(0, y), (w, y)],
               fill=(int(120 - 70 * t), int(178 - 74 * t), int(188 - 66 * t)))
    for i in range(6):
        base = h * (0.34 + 0.11 * i)
        amp = h * (0.030 + 0.008 * i)
        pts = [(0, h), (0, base)]
        for x in range(0, w + 10, 10):
            u = x / w
            pts.append((x, base + amp * math.sin(4.1 * u + i * 0.9)))
        pts.append((w, h))
        d.polygon(pts, fill=(int(96 - 11 * i), int(150 - 15 * i), int(166 - 13 * i)))
    return im.filter(ImageFilter.GaussianBlur(3))


def wallpaper_rain(w=320, h=180, n=16):
    """An ANIMATED picture -- a live wallpaper, listed beside the effects
    from /usr/share/wallpapers/animated (gui/wallpapers/players/gif.c). Low resolution and a loop
    of streaks over a night gradient: what a GIF wallpaper usually is, and
    small enough to ship. The loop is seamless because every streak falls
    a whole number of screen heights per cycle."""
    rnd = random.Random(7)
    drops = [(rnd.uniform(0, w), rnd.uniform(0, h), rnd.choice((1, 2)), rnd.uniform(5, 11))
             for _ in range(90)]
    frames = []
    for k in range(n):
        im = Image.new("RGB", (w, h))
        d = ImageDraw.Draw(im)
        for y in range(h):
            t = y / (h - 1)
            d.line([(0, y), (w, y)], fill=(int(12 + 14 * t), int(20 + 30 * t), int(38 + 44 * t)))
        for x0, y0, laps, length in drops:
            y = (y0 + laps * h * k / n) % h
            x = x0 - 0.18 * y
            d.line([(x, y), (x - 0.18 * length, y - length)],
                   fill=(int(110 + 20 * laps), int(140 + 20 * laps), 190))
        frames.append(im.quantize(colors=32, dither=Image.Dither.NONE))
    return frames


def build_wallpapers():
    os.makedirs(WALLPAPER_DIR, exist_ok=True)
    for name, fn in (("aurora", wallpaper_aurora), ("dusk", wallpaper_dusk),
                     ("slate", wallpaper_slate), ("ember", wallpaper_ember),
                     ("tide", wallpaper_tide)):
        im = fn(1280, 720)
        path = os.path.join(WALLPAPER_DIR, name + ".jpg")
        im.save(path, format="JPEG", quality=88, subsampling=2, optimize=True)
        print("wrote %s (%d bytes)" % (os.path.relpath(path, ROOT),
                                       os.path.getsize(path)))
    frames = wallpaper_rain()
    os.makedirs(os.path.join(WALLPAPER_DIR, "animated"), exist_ok=True)
    path = os.path.join(WALLPAPER_DIR, "animated", "rain.gif")
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=80, loop=0,
                   optimize=False)
    print("wrote %s (%d bytes)" % (os.path.relpath(path, ROOT), os.path.getsize(path)))


# --- sample pictures ---------------------------------------------------

PICTURE_DIR = os.path.join(ROOT, "data", "usr", "share", "pictures")


def bmp_bytes(w, h, bpp, comp, data, palette=b"", masks=b"", hs=40):
    """A BMP from parts; masks inside the header from V2 (hs >= 52) on."""
    info = struct.pack("<IiiHHIIiiII", hs, w, h, 1, bpp, comp, len(data), 2835, 2835,
                       len(palette) // 4, 0)
    if hs > 40:
        info = (info + masks.ljust(16, b"\0")[:hs - 40]).ljust(hs, b"\0")
    off = 14 + len(info) + len(palette)
    return b"BM" + struct.pack("<IHHI", off + len(data), 0, 0, off) + info + palette + data


def rle8(im):
    """An 8-bit palette image as BI_RLE8: encoded runs, an end of line per
    row, an end of bitmap -- bottom row first, as the format is."""
    w, h = im.size
    px = im.load()
    out = bytearray()
    for y in range(h - 1, -1, -1):
        x = 0
        while x < w:
            n = 1
            while x + n < w and n < 255 and px[x + n, y] == px[x, y]:
                n += 1
            out += bytes((n, px[x, y]))
            x += n
        out += b"\0\0"
    out[-2:] = b"\0\1"
    return bytes(out)


def palette_bytes(im):
    pal = im.getpalette()[:768]
    return b"".join(bytes((pal[i + 2], pal[i + 1], pal[i], 0)) for i in range(0, len(pal), 3))


def picture_orbit(n=24, size=200):
    """Twelve dots round a ring, a bright head chasing round: opaque."""
    frames = []
    for k in range(n):
        im = Image.new("RGB", (size, size), (22, 26, 34))
        d = ImageDraw.Draw(im)
        c = size / 2
        for i in range(12):
            a = 2 * math.pi * i / 12
            age = (k * 12 // n - i) % 12
            t = 1 - age / 12
            r = 6 + 9 * t
            x, y = c + 70 * math.cos(a), c + 70 * math.sin(a)
            col = (int(40 + 200 * t), int(90 + 120 * t), int(200 + 40 * t))
            d.ellipse([x - r, y - r, x + r, y + r], fill=col)
        frames.append(im.quantize(64))
    return frames


def picture_bounce(n=20, size=160):
    """A ball bouncing over TRANSPARENCY, each frame cleared after it
    (disposal 2) -- the stage's own tint shows round the ball."""
    frames = []
    for k in range(n):
        im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        # Height |sin| of a half turn per bounce: the floor at k = 0, the
        # top half way, squashed for the one frame it touches.
        y = 20 + (size - 70) * (1 - abs(math.sin(math.pi * k / n)))
        squash = 1.25 if k == 0 else 1.0
        rx, ry = 22 * squash, 22 / squash
        cx = size / 2
        d.ellipse([cx - rx, y - ry + 22, cx + rx, y + ry + 22], fill=(240, 120, 40, 255))
        d.ellipse([cx - rx / 2.5 - 6, y + 22 - ry / 2 - 4, cx - 2, y + 22 - 2], fill=(255, 210, 160, 255))
        frames.append(im)
    return frames


def build_pictures():
    os.makedirs(PICTURE_DIR, exist_ok=True)
    scene = wallpaper_dusk(320, 180)
    out = {}

    buf = io.BytesIO()
    scene.save(buf, format="BMP")
    out["dusk-24bit.bmp"] = buf.getvalue()

    pal = scene.quantize(256)
    buf = io.BytesIO()
    pal.save(buf, format="BMP")
    out["dusk-256-colours.bmp"] = buf.getvalue()

    flat = wallpaper_slate(320, 180).quantize(16)
    out["slate-rle8.bmp"] = bmp_bytes(320, 180, 8, 1, rle8(flat), palette_bytes(flat))

    # 32-bit with an alpha mask in a V5 header: a soft badge on nothing.
    badge = Image.new("RGBA", (128, 128), (0, 0, 0, 0))
    bd = ImageDraw.Draw(badge)
    for r in range(60, 0, -1):
        a = 255 if r < 54 else int(255 * (60 - r) / 6)
        bd.ellipse([64 - r, 64 - r, 64 + r, 64 + r],
                   fill=(int(40 + 3 * r), int(110 + r), 220, a))
    rows = bytearray()
    px = badge.load()
    for y in range(127, -1, -1):
        for x in range(128):
            r_, g_, b_, a_ = px[x, y]
            rows += bytes((b_, g_, r_, a_))
    masks = struct.pack("<IIII", 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    out["badge-alpha.bmp"] = bmp_bytes(128, 128, 32, 3, bytes(rows), masks=masks, hs=124)

    buf = io.BytesIO()
    sw = Image.new("RGB", (256, 128))
    sd = ImageDraw.Draw(sw)
    for i in range(128):
        hue = i / 128
        r_ = int(255 * max(0, min(1, abs(hue * 6 - 3) - 1)))
        g_ = int(255 * max(0, min(1, 2 - abs(hue * 6 - 2))))
        b_ = int(255 * max(0, min(1, 2 - abs(hue * 6 - 4))))
        sd.rectangle([(i % 16) * 16, (i // 16) * 16, (i % 16) * 16 + 15, (i // 16) * 16 + 15],
                     fill=(r_, g_, b_))
    sw.quantize(128).save(buf, format="GIF")
    out["swatches.gif"] = buf.getvalue()

    fr = picture_orbit()
    buf = io.BytesIO()
    fr[0].save(buf, format="GIF", save_all=True, append_images=fr[1:], duration=60, loop=0)
    out["orbit-animated.gif"] = buf.getvalue()

    fr = picture_bounce()
    buf = io.BytesIO()
    fr[0].save(buf, format="GIF", save_all=True, append_images=fr[1:], duration=50, loop=0,
               disposal=2, optimize=False)
    out["bounce-animated.gif"] = buf.getvalue()

    for name, data in out.items():
        with open(os.path.join(PICTURE_DIR, name), "wb") as f:
            f.write(data)
        print("wrote %s: %d bytes" % (os.path.relpath(os.path.join(PICTURE_DIR, name), ROOT),
                                      len(data)))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--vectors", action="store_true")
    ap.add_argument("--wallpapers", action="store_true")
    ap.add_argument("--pictures", action="store_true")
    args = ap.parse_args()
    if not args.vectors and not args.wallpapers and not args.pictures:
        args.vectors = args.wallpapers = args.pictures = True
    if args.vectors:
        write_vectors(build_vectors(), build_anim_vectors())
    if args.wallpapers:
        build_wallpapers()
    if args.pictures:
        build_pictures()


if __name__ == "__main__":
    main()
