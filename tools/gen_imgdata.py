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

  --wallpapers  data/wallpapers/*.jpg -- the desktop backgrounds, drawn
                here rather than committed as somebody's photograph so
                the repo carries no image it does not own.

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
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFilter
except ImportError:
    sys.exit("gen_imgdata.py needs Pillow: pip install --user pillow")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VECTORS_H = os.path.join(ROOT, "userland", "tests", "uimg_vectors.h")
WALLPAPER_DIR = os.path.join(ROOT, "data", "wallpapers")

# errno values from kernel/include/abi/errno.h, as POSITIVE numbers; the
# test compares them against a negative return.
EINVAL = 22
ENOTSUP = 95


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

    def add(name, data, tol):
        """One decodable vector: the bytes, and what PILLOW decodes them to."""
        ref = Image.open(io.BytesIO(data)).convert("RGBA")
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

    prog = encode(gradient(16, 16), quality=85, progressive=True)
    bad("progressive is refused", prog, ENOTSUP)
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

    return vectors


def write_vectors(vectors):
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


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--vectors", action="store_true")
    ap.add_argument("--wallpapers", action="store_true")
    args = ap.parse_args()
    if not args.vectors and not args.wallpapers:
        args.vectors = args.wallpapers = True
    if args.vectors:
        write_vectors(build_vectors())
    if args.wallpapers:
        build_wallpapers()


if __name__ == "__main__":
    main()
