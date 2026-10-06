#!/usr/bin/env python3
"""Check uimg's QOI, PNG, BMP and GIF codecs against Pillow, zlib and ImageMagick, on the HOST.

WHY A FOREIGN DECODER IS THE WHOLE POINT. An encoder tested by this
repo's own decoder passes whenever the two share a mistake, and the two
mistakes an image encoder actually makes are exactly that shape: a
QOI index table updated on the wrong chunk, or a Huffman code packed
least-significant-bit-first. Both produce a file that round-trips
perfectly here and that nothing else in the world can open.

So every file this writes is opened by Pillow, and the PNG's deflate
stream is additionally inflated by Python's zlib and unfiltered by hand
-- three implementations that share no code with userland/lib/.

Both directions, because they fail differently: an ENCODER is checked by
a foreign decoder reading what it wrote, and a DECODER by reading what a
foreign encoder wrote. Neither is ever checked against the other half of
this repo, which is the whole point -- a shared mistake passes that.

    python3 tools/uimg_codec_hostcheck.py
    python3 tools/uimg_codec_hostcheck.py --positive-control
    python3 tools/uimg_codec_hostcheck.py --keep /tmp/shots

BMP and GIF are read-only here, so they get the DECODE half only: files
Pillow and ImageMagick wrote, every header version and depth for BMP,
interlacing, local colour tables and all three disposals for GIF -- each
frame of an animation compared, through the codec's own iterator. A few
variants neither tool writes (RLE4, a V4 header, an RLE delta, a top-down
file) are built by hand, their expected pixels from a reading of the
format written here in Python.

The positive control is not optional reading: a clean run proves nothing
until the harness has been seen to fail. It breaks the bit reversal in
the deflate writer -- the single most likely real bug here -- the PNG
unfilter, BMP's bottom-up row order and GIF's KwKwK case, and every
PNG, BMP and GIF check family must go red while the QOI ones stay green.

Exit status is non-zero on any mismatch. Needs Pillow, ImageMagick
(`magick`) and gcc.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
import zlib

try:
    from PIL import Image
except ImportError:
    sys.exit("uimg_codec_hostcheck.py needs Pillow: pip install --user pillow")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def pixels(im):
    """Pillow 14 renames getdata(); both spellings have to work here."""
    if hasattr(im, "get_flattened_data"):
        return list(im.get_flattened_data())
    return list(im.getdata())

# Reads a raw 0xAARRGGBB dump and writes both formats. Deliberately thin:
# everything being checked is in the two codec files it links.
DRIVER = r"""
#include "lib/uimg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_err = "";
void uimg_set_error(const char *m) { g_err = m; }

static int write_all(const char *path, const unsigned char *d, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t w = fwrite(d, 1, n, f);
    fclose(f);
    return w == n;
}

// DECODE MODE: argv = "decode" <file> <out.raw>. Writes w, h and the
// pixels as 0xAARRGGBB to stdout's file, and reports the return code on
// stderr so a refusal can be told from a crash.
static int do_decode(const char *in, const char *out) {
    FILE *f = fopen(in, "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);

    // DISPATCHED HERE, not through uimg_decode(): uimg.c includes
    // rt/sys.h and lib/ufile.h, which exist only in the guest. The
    // codec rows are what is under test anyway.
    const struct uimg_codec *c = NULL;
    if (uimg_codec_png.probe(buf, (size_t)n)) c = &uimg_codec_png;
    else if (uimg_codec_qoi.probe(buf, (size_t)n)) c = &uimg_codec_qoi;
    else if (uimg_codec_bmp.probe(buf, (size_t)n)) c = &uimg_codec_bmp;
    else if (uimg_codec_gif.probe(buf, (size_t)n)) c = &uimg_codec_gif;
    if (!c) { fprintf(stderr, "rc=-22 no codec claimed it\n"); return 1; }
    if (!c->decode) { fprintf(stderr, "rc=-95 no decoder\n"); return 1; }

    struct uimg im;
    int rc = c->decode(buf, (size_t)n, &im);
    fprintf(stderr, "rc=%d %s\n", rc, g_err);
    if (rc < 0) return 1;

    FILE *o = fopen(out, "wb");
    if (!o) return 2;
    fprintf(o, "%d %d %d\n", im.w, im.h, im.has_alpha);
    for (int i = 0; i < im.w * im.h; i++) {
        unsigned p = im.px[i];
        fputc((int)((p >> 24) & 0xFF), o);
        fputc((int)((p >> 16) & 0xFF), o);
        fputc((int)((p >> 8) & 0xFF), o);
        fputc((int)(p & 0xFF), o);
    }
    fclose(o);
    return 0;
}

// ANIM MODE: argv = "anim" <file.gif> <out>. Every frame through the
// codec's own iterator: "w h frames loops", then per frame a line with
// its delay and the canvas as 0xAARRGGBB. The delay is the FILE's --
// uimg.c's clamp of 0/10 ms is not under test here.
static int do_anim(const char *in, const char *out) {
    FILE *f = fopen(in, "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);

    struct uimg_anim a;
    memset(&a, 0, sizeof a);
    int rc = uimg_codec_gif.anim_open(buf, (size_t)n, &a);
    fprintf(stderr, "rc=%d %s\n", rc, g_err);
    if (rc < 0) return 1;
    FILE *o = fopen(out, "wb");
    if (!o) return 2;
    fprintf(o, "%d %d %d %d\n", a.frame.w, a.frame.h, a.frames, a.loops);
    for (int k = 0; k < a.frames; k++) {
        int delay = 0;
        rc = a.next(&a, &delay);
        if (rc < 0) { fprintf(stderr, "frame %d rc=%d %s\n", k, rc, g_err); return 1; }
        fprintf(o, "%d\n", delay);
        for (int i = 0; i < a.frame.w * a.frame.h; i++) {
            unsigned p = a.frame.px[i];
            fputc((int)((p >> 24) & 0xFF), o);
            fputc((int)((p >> 16) & 0xFF), o);
            fputc((int)((p >> 8) & 0xFF), o);
            fputc((int)(p & 0xFF), o);
        }
    }
    fclose(o);
    a.release(&a);
    free(a.frame.px);
    return 0;
}

// argv: <raw> <w> <h> <alpha 0|1> <out.qoi> <out.png>
int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "decode") == 0)
        return do_decode(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "anim") == 0)
        return do_anim(argv[2], argv[3]);
    if (argc != 7) return 2;
    struct uimg im;
    im.w = atoi(argv[2]);
    im.h = atoi(argv[3]);
    im.has_alpha = atoi(argv[4]);
    size_t n = (size_t)im.w * im.h * 4;
    im.px = malloc(n);
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(im.px, 1, n, f) != n) return 2;
    fclose(f);

    unsigned char *out = NULL;
    size_t len = 0;
    int rc = uimg_codec_qoi.encode(&im, &out, &len);
    if (rc < 0) { fprintf(stderr, "qoi rc=%d %s\n", rc, g_err); return 1; }
    if (!write_all(argv[5], out, len)) return 2;
    free(out);

    rc = uimg_codec_png.encode(&im, &out, &len);
    if (rc < 0) { fprintf(stderr, "png rc=%d %s\n", rc, g_err); return 1; }
    if (!write_all(argv[6], out, len)) return 2;
    free(out);
    return 0;
}
"""


def build(tmp, sabotage=False):
    src = os.path.join(tmp, "driver.c")
    with open(src, "w") as f:
        f.write(DRIVER)
    # kernel/include/api CANNOT go on the include path here: it carries
    # its own string.h, which would shadow the host's and take memcpy
    # with it. One forwarding header is what the codecs actually need
    # from it.
    shim = os.path.join(tmp, "shim")
    os.makedirs(shim, exist_ok=True)
    with open(os.path.join(shim, "kcrc.h"), "w") as f:
        f.write('#include "%s"\n'
                % os.path.join(ROOT, "kernel", "include", "api", "kcrc.h"))

    png_c = os.path.join(ROOT, "userland", "lib", "uimg_png.c")
    infl_c = os.path.join(ROOT, "userland", "lib", "uinflate.c")
    if sabotage:
        # THE control: a Huffman code that is not reversed. The file
        # stays structurally valid -- signature, chunk lengths and CRCs
        # all correct -- and the pixels are noise, which is exactly the
        # bug a self-decoding test cannot see. Patched into a COPY, so
        # the checked-in source is never edited to run a control.
        body = ("    uint32_t r = 0;\n"
                "    for (int i = 0; i < n; i++) { r = (r << 1) | (v & 1); v >>= 1; }\n"
                "    return r;\n")
        # bit_reverse() moved to uinflate.c with the rest of the
        # compressor; the control follows it.
        text = open(infl_c).read()
        if body not in text:
            sys.exit("positive control: bit_reverse() no longer looks as expected")
        infl_c = os.path.join(tmp, "uinflate_broken.c")
        with open(infl_c, "w") as f:
            f.write(text.replace(body, "    (void)n;\n    return v;\n"))

        # AND A SECOND, DECODE-ONLY SABOTAGE. The one above breaks only
        # the compressor, so on its own it leaves every decode check
        # green -- a control that cannot redden half the harness does not
        # test that half. This one drops the Paeth predictor from
        # UNFILTERING, which is where a decoder most plausibly goes wrong
        # and which Pillow's files exercise constantly.
        # `c` becomes unused once Paeth goes, and the harness builds
        # with -Werror -- so the substitute still mentions it.
        pbody = "        case 4: v += paeth(a, b, c); break;\n"
        ptext = open(png_c).read()
        if ptext.count(pbody) != 1:
            sys.exit("positive control: png_unfilter()'s Paeth case no longer "
                     "looks as expected")
        png_c = os.path.join(tmp, "uimg_png_broken.c")
        with open(png_c, "w") as f:
            f.write(ptext.replace(pbody,
                                   "        case 4: v += a + 0 * c; break;\n"))

    bmp_c = os.path.join(ROOT, "userland", "lib", "uimg_bmp.c")
    gif_c = os.path.join(ROOT, "userland", "lib", "uimg_gif.c")
    if sabotage:
        # BMP: every row where it would be had the file been top-down,
        # which is what a decoder that forgets BMP is bottom-up draws.
        # GIF: the KwKwK code (one the decoder is defining as it reads it)
        # without its repeated first character -- LZW's classic slip,
        # which any file with a run of one colour exercises.
        for path, old, new, name in (
                (bmp_c, "(size_t)(b.top_down ? y : b.h - 1 - y) * b.w",
                 "(size_t)(b.top_down ? y : y) * b.w", "uimg_bmp_broken.c"),
                (gif_c, "if (code == next) { g->stack[sp++] = (uint8_t)firstc; c = old; }",
                 "if (code == next) { c = old; }", "uimg_gif_broken.c")):
            text = open(path).read()
            if text.count(old) != 1:
                sys.exit("positive control: %s no longer looks as expected"
                         % os.path.basename(path))
            broken = os.path.join(tmp, name)
            with open(broken, "w") as f:
                f.write(text.replace(old, new))
            if name.startswith("uimg_bmp"):
                bmp_c = broken
            else:
                gif_c = broken

    exe = os.path.join(tmp, "uimgenc")
    cmd = ["gcc", "-O2", "-Wall", "-Wextra", "-Werror", "-o", exe, src,
           os.path.join(ROOT, "userland", "lib", "uimg_qoi.c"), png_c, infl_c,
           bmp_c, gif_c,
           os.path.join(ROOT, "kernel", "lib", "kcrc.c"),
           "-I" + shim,
           "-I" + os.path.join(ROOT, "userland"),
           "-I" + os.path.join(ROOT, "kernel", "include", "abi")]
    subprocess.run(cmd, check=True)
    return exe


def patterns(kind, w, h):
    """Images chosen for what they do to each encoder, not for looking nice."""
    im = Image.new("RGBA", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            if kind == "flat":                       # QOI runs, deflate matches
                px[x, y] = (40, 44, 52, 255)
            elif kind == "gradient":                 # QOI diff/luma chunks
                px[x, y] = (x * 255 // max(w - 1, 1),
                            y * 255 // max(h - 1, 1), 128, 255)
            elif kind == "noise":                    # no chunk helps; literals
                v = (x * 7919 + y * 104729) & 0xFFFFFF
                px[x, y] = ((v >> 16) & 255, (v >> 8) & 255, v & 255, 255)
            elif kind == "ui":                       # what a desktop looks like
                bar = y < h // 8 or y > h - h // 8
                box = (w // 4 < x < 3 * w // 4) and (h // 3 < y < 2 * h // 3)
                px[x, y] = ((60, 64, 72, 255) if bar else
                            (250, 250, 252, 255) if box else (30, 120, 200, 255))
            elif kind == "alpha":                    # forces QOI_OP_RGBA
                px[x, y] = (x * 4 & 255, y * 4 & 255, 90, (x * y) & 255)
            elif kind == "palette":                  # forces QOI_OP_INDEX hits
                c = [(200, 30, 30), (30, 200, 30), (30, 30, 200), (220, 220, 40)]
                px[x, y] = c[(x // 3 + y // 5) % 4] + (255,)
    return im


def raw_argb(im):
    out = bytearray()
    for (r, g, b, a) in pixels(im):
        out += struct.pack("<I", (a << 24) | (r << 16) | (g << 8) | b)
    return bytes(out)


def write_png_filtered(path, im, filt):
    """A truecolour PNG whose every row uses filter `filt`.

    The five filters are the decoder's five unfilter paths, and this is
    the only way to be sure each one is entered: an encoder picks per
    row, so a file that happens to use Paeth everywhere leaves Sub and
    Average untested.
    """
    im = im.convert("RGB")
    w, h = im.size
    px = pixels(im)
    raw = bytearray()
    prev = bytearray(w * 3)
    for y in range(h):
        row = bytearray()
        for x in range(w):
            row += bytes(px[y * w + x][:3])
        raw.append(filt)
        for i in range(len(row)):
            a = row[i - 3] if i >= 3 else 0
            b = prev[i]
            c = prev[i - 3] if i >= 3 else 0
            if filt == 0:
                v = row[i]
            elif filt == 1:
                v = row[i] - a
            elif filt == 2:
                v = row[i] - b
            elif filt == 3:
                v = row[i] - ((a + b) >> 1)
            else:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                v = row[i] - pr
            raw.append(v & 0xFF)
        prev = row

    def chunk(typ, data):
        return (struct.pack(">I", len(data)) + typ + data
                + struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    out = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
    open(path, "wb").write(out)


def unfilter_png(path):
    """Inflate and unfilter by hand -- the second independent decode."""
    d = open(path, "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n", "bad signature"
    pos, idat, ihdr = 8, b"", None
    while pos < len(d):
        (ln,) = struct.unpack(">I", d[pos:pos + 4])
        typ = d[pos + 4:pos + 8]
        body = d[pos + 8:pos + 8 + ln]
        crc = struct.unpack(">I", d[pos + 8 + ln:pos + 12 + ln])[0]
        if zlib.crc32(typ + body) & 0xFFFFFFFF != crc:
            raise ValueError("chunk %s has a bad CRC" % typ.decode())
        if typ == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif typ == b"IDAT":
            idat += body
        pos += 12 + ln

    w, h, depth, colour, comp, filt, inter = ihdr
    assert (depth, comp, filt, inter) == (8, 0, 0, 0), "unexpected IHDR"
    bpp = 4 if colour == 6 else 3
    raw = zlib.decompress(idat)
    assert len(raw) == (w * bpp + 1) * h, "inflated to the wrong length"

    out, prev = [], bytearray(w * bpp)
    p = 0
    for _ in range(h):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + w * bpp])
        p += w * bpp
        for i in range(len(line)):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
            elif f != 0:
                raise ValueError("unknown filter %d" % f)
        out.append(bytes(line))
        prev = line
    return w, h, bpp, b"".join(out)


def compare(name, want, got, alpha):
    """Exact, both ways lossless. Reports the first differing pixel."""
    for i, (a, b) in enumerate(zip(want, got)):
        if not alpha:
            a, b = a[:3], b[:3]
        if a != b:
            return "%s: pixel %d is %s, want %s" % (name, i, b, a)
    if len(want) != len(got):
        return "%s: %d pixels, want %d" % (name, len(got), len(want))
    return None


# --- BMP and GIF: the decode half only -----------------------------------

def magick(*argv):
    subprocess.run(["magick", *argv], check=True, capture_output=True)


def run_decode(exe, src, tmp):
    """(stderr, w, h, RGBA tuples or None) from the driver's decode mode."""
    raw = os.path.join(tmp, "d.raw")
    r = subprocess.run([exe, "decode", src, raw], capture_output=True)
    err = r.stderr.decode().strip()
    if r.returncode:
        return err, 0, 0, None
    body = open(raw, "rb").read()
    nl = body.index(b"\n")
    gw, gh, _alpha = (int(v) for v in body[:nl].split())
    data = body[nl + 1:]
    got = [(data[i * 4 + 1], data[i * 4 + 2], data[i * 4 + 3], data[i * 4])
           for i in range(gw * gh)]
    return err, gw, gh, got


def run_anim(exe, src, tmp):
    """(stderr, w, h, loops, [(delay, RGBA tuples)]) from the anim mode."""
    raw = os.path.join(tmp, "a.raw")
    r = subprocess.run([exe, "anim", src, raw], capture_output=True)
    err = r.stderr.decode().strip()
    if r.returncode:
        return err, 0, 0, 0, None
    body = open(raw, "rb").read()
    nl = body.index(b"\n")
    w, h, frames, loops = (int(v) for v in body[:nl].split())
    p, out = nl + 1, []
    for _ in range(frames):
        nl = body.index(b"\n", p)
        delay = int(body[p:nl])
        p = nl + 1
        px = [(body[p + i * 4 + 1], body[p + i * 4 + 2], body[p + i * 4 + 3], body[p + i * 4])
              for i in range(w * h)]
        p += w * h * 4
        out.append((delay, px))
    return err, w, h, loops, out


def near(name, want, got, tol=0, clear_eq=False):
    """Colour within `tol`, alpha EXACT. `clear_eq`: two fully transparent
    pixels match whatever colour each carries -- Pillow keeps the palette
    entry's RGB under alpha 0, this decoder writes 0."""
    if len(want) != len(got):
        return "%s: %d pixels, want %d" % (name, len(got), len(want))
    for i, (a, b) in enumerate(zip(want, got)):
        if clear_eq and a[3] == 0 and b[3] == 0:
            continue
        if a[3] != b[3] or any(abs(a[k] - b[k]) > tol for k in range(3)):
            return "%s: pixel %d is %s, want %s" % (name, i, b, a)
    return None


def bmp16_ref(path):
    """A 16-bit bit-mask BMP read HERE: each mask's bits widened by
    repetition, an alpha channel that is zero everywhere taken as opaque.

    The oracle for ARGB4444/1555, which neither foreign reader can be:
    Pillow refuses 4444 and drops 1555's alpha bit, and ImageMagick
    widens 4 bits by shifting, so its white is 240 -- and reads its own
    1555 alpha as 128."""
    d = open(path, "rb").read()
    off, = struct.unpack("<I", d[10:14])
    w, h = struct.unpack("<ii", d[18:26])
    masks = struct.unpack("<IIII", d[54:70])

    def widen(v, m):
        if not m:
            return 0
        shift = (m & -m).bit_length() - 1
        bits = bin(m).count("1")
        x = (v & m) >> shift
        r, got = 0, 0
        while got < 8:
            r, got = (r << bits) | x, got + bits
        return r >> (got - 8)

    stride = (w * 2 + 3) & ~3
    out = []
    for y in range(h):
        row = off + (h - 1 - y) * stride
        for x in range(w):
            v, = struct.unpack("<H", d[row + 2 * x:row + 2 * x + 2])
            out.append(tuple(widen(v, m) for m in masks))
    if not any(p[3] for p in out):
        out = [p[:3] + (255,) for p in out]
    return out


def bmp_build(w, h, bpp, comp, data, palette=b"", masks=b"", hs=40, clr=0, planes=1):
    """A BMP from parts. `masks` go inside the header from V2 (hs >= 52)
    on, after a Windows 3 one otherwise -- the two places the format
    keeps them."""
    info = struct.pack("<IiiHHIIiiII", hs, w, h, planes, bpp, comp, len(data),
                       2835, 2835, clr, 0)
    after = masks
    if hs > 40:
        info = (info + masks.ljust(16, b"\0")[:hs - 40]).ljust(hs, b"\0")
        after = b""
    off = 14 + len(info) + len(after) + len(palette)
    return (b"BM" + struct.pack("<IHHI", off + len(data), 0, 0, off)
            + info + after + palette + data)


def bgra_rows(im, bottom_up=True, alpha_byte=None):
    """32-bit pixel rows, bottom row first unless told otherwise."""
    w, h = im.size
    px = pixels(im.convert("RGBA"))
    rows = range(h - 1, -1, -1) if bottom_up else range(h)
    out = bytearray()
    for y in rows:
        for x in range(w):
            r, g, b, a = px[y * w + x]
            out += bytes((b, g, r, a if alpha_byte is None else alpha_byte))
    return bytes(out)


def bmp_hand_cases():
    """(tag, bytes, w, h, want RGBA) for what neither tool writes."""
    pal = b"".join(bytes((b, g, r, 0)) for (r, g, b) in
                   ((0, 0, 0), (200, 30, 30), (30, 200, 30), (30, 30, 200)))
    colour = [(0, 0, 0, 255), (200, 30, 30, 255), (30, 200, 30, 255), (30, 30, 200, 255)]
    clear = (0, 0, 0, 0)

    def grid(rows):  # rows TOP first, "." transparent, digits palette indices
        return [clear if c == "." else colour[int(c)] for r in rows for c in r]

    cases = []
    # RLE4: an encoded run (alternating nibbles), a literal run padded to
    # a 16-bit boundary, an end of line, an end of bitmap.
    # (A literal run is 3 or more: 0, 2 is the DELTA escape.)
    rle4 = bytes([4, 0x12, 0, 3, 0x30, 0x10, 0, 0,          # bottom row
                  7, 0x33, 0, 0,
                  0, 5, 0x12, 0x30, 0x10, 0x00, 2, 0x20, 0, 0,
                  0, 1])
    cases.append(("rle4 runs", bmp_build(7, 3, 4, 2, rle4, pal, clr=4), 7, 3,
                  grid(["1230120", "3333333", "1212301"])))
    # RLE8: an early end of line and a delta leave pixels unwritten,
    # which are TRANSPARENT (uimg_bmp.c's browser rule).
    rle8 = bytes([5, 1, 0, 0,
                  2, 2, 0, 2, 1, 1,
                  2, 3, 0, 0,
                  0, 3, 1, 2, 3, 0,
                  0, 1])
    cases.append(("rle8 delta + early eol", bmp_build(5, 4, 8, 1, rle8, pal, clr=4), 5, 4,
                  grid(["123..", "...33", "22...", "11111"])))

    src = patterns("alpha", 11, 6)
    want_a = pixels(src)
    opaque = [(r, g, b, 255) for (r, g, b, _a) in want_a]
    m32 = struct.pack("<IIII", 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    cases.append(("top-down 32 BI_RGB", bmp_build(11, -6, 32, 0, bgra_rows(src, False)),
                  11, 6, opaque))
    cases.append(("32 BI_RGB, reserved byte ignored",
                  bmp_build(11, 6, 32, 0, bgra_rows(src, alpha_byte=0x5A)), 11, 6, opaque))
    cases.append(("V3 header, alpha mask", bmp_build(11, 6, 32, 3, bgra_rows(src), masks=m32, hs=56),
                  11, 6, want_a))
    cases.append(("V4 header, alpha mask", bmp_build(11, 6, 32, 3, bgra_rows(src), masks=m32, hs=108),
                  11, 6, want_a))
    cases.append(("V5 alpha all zero is opaque",
                  bmp_build(11, 6, 32, 3, bgra_rows(src, alpha_byte=0), masks=m32, hs=124),
                  11, 6, opaque))
    # Three palette entries declared and stored (biClrUsed), 8-bit.
    idx = bytes([0, 1, 2, 1, 0, 0, 0, 0])                    # one row, padded
    cases.append(("8-bit, biClrUsed 3", bmp_build(5, 1, 8, 0, idx, pal[:12], clr=3), 5, 1,
                  grid(["01210"])))
    return cases


def bmp_refusals():
    """(tag, bytes, rc) -- -95 the file is fine and we are not, -22 broken."""
    px = bytes(4 * 2 * 2)
    good = bmp_build(2, 2, 32, 0, px)
    return [
        ("BI_JPEG", bmp_build(2, 2, 24, 4, b"\xff\xd8\xff\xd9"), -95),
        ("OS/2 Huffman", bmp_build(2, 2, 1, 3, px, hs=64), -95),
        ("truncated pixels", good[:-5], -22),
        ("two planes", bmp_build(2, 2, 32, 0, px, planes=2), -22),
        ("7-bit depth", bmp_build(2, 2, 7, 0, px), -22),
        ("top-down RLE8", bmp_build(2, -2, 8, 1, b"\0\1", b"\0" * 8), -22),
        ("header size 41", good[:14] + struct.pack("<I", 41) + good[18:], -22),
    ]


def gif_descriptors(d):
    """[(offset of the 0x2C, interlaced?, end of its data)] by walking the
    blocks -- what the truncation and interlace checks need to aim."""
    gct = (2 << (d[10] & 7)) * 3 if d[10] & 0x80 else 0
    p, out = 13 + gct, []
    while p < len(d):
        b = d[p]
        if b == 0x3B:
            break
        if b == 0x21:
            p += 2
        else:
            start, packed = p, d[p + 9]
            p += 10
            if packed & 0x80:
                p += (2 << (packed & 7)) * 3
            p += 1
        while d[p]:
            p += d[p] + 1
        p += 1
        if b == 0x2C:
            out.append((start, bool(packed & 0x40), p))
    return out


def sweep_bmp_gif(exe, tmp, keep, quiet):
    """The BMP and GIF decode checks. Returns (checks, failures); every
    failure starts with "bmp " or "gif ", which the positive control
    counts by."""
    failures, checks = [], 0
    where = keep or tmp

    def say(tag, what="ok"):
        if not quiet:
            print("  %-34s %s" % (tag, what))

    # --- BMP, written by Pillow and ImageMagick -------------------------
    files = []
    for (w, h) in ((1, 1), (7, 3), (13, 7), (129, 77)):
        base = patterns("alpha", w, h)
        for mode in ("1", "L", "P", "RGB", "RGBA"):
            im = base.convert("RGB").quantize(37) if mode == "P" else base.convert(mode)
            path = os.path.join(where, "pil_%s_%dx%d.bmp" % (mode, w, h))
            im.save(path)
            files.append(("bmp pillow %s %dx%d" % (mode, w, h), path, w, h, "pillow", 0))
    for (w, h) in ((13, 7), (129, 77)):
        rgba = os.path.join(tmp, "src_%dx%d.png" % (w, h))
        rgb = os.path.join(tmp, "src_rgb_%dx%d.png" % (w, h))
        bits = os.path.join(tmp, "src_bits_%dx%d.png" % (w, h))
        patterns("alpha", w, h).save(rgba)
        # Alpha that SURVIVES one bit: the "alpha" pattern's stays under
        # 128 on a small image, which 1555 stores as all-clear -- and that
        # decodes opaque by the zero-everywhere rule, testing nothing.
        stripes = patterns("gradient", w, h)
        stripes.putalpha(Image.eval(stripes.getchannel("R"), lambda v: 255 if v & 32 else 0))
        stripes.save(bits)
        patterns("gradient", w, h).convert("RGB").save(rgb)
        # ORACLE PER FILE: Pillow wherever it reads the variant, bmp16_ref()
        # where neither foreign reader can be one. 565/555 allow 1: Pillow
        # widens 5 bits by v*255/31 rounded DOWN, this decoder (and
        # Chromium) by repeating them, and the two differ by one.
        for name, argv, oracle, tol in (
                ("OS/2 1.x 24", [rgb, "BMP2:%s"], "pillow", 0),
                ("OS/2 1.x 8", [rgb, "-type", "Palette", "BMP2:%s"], "pillow", 0),
                ("win3 24", [rgb, "BMP3:%s"], "pillow", 0),
                ("win3 4-bit", [rgb, "-colors", "9", "-type", "Palette", "BMP3:%s"], "pillow", 0),
                ("win3 RLE8", [rgb, "-colors", "40", "-type", "Palette", "-compress", "RLE",
                               "BMP3:%s"], "pillow", 0),
                ("V5 24", [rgb, "BMP:%s"], "pillow", 0),
                ("V5 32 alpha", [rgba, "BMP:%s"], "pillow", 0),
                ("V5 RGB565", [rgb, "-define", "bmp:subtype=RGB565", "BMP:%s"], "pillow", 1),
                ("V5 RGB555", [rgb, "-define", "bmp:subtype=RGB555", "BMP:%s"], "pillow", 1),
                ("V5 ARGB4444", [rgba, "-define", "bmp:subtype=ARGB4444", "BMP:%s"], "bits", 0),
                ("V5 ARGB1555", [bits, "-define", "bmp:subtype=ARGB1555", "BMP:%s"], "bits", 0)):
            slug = "".join(ch if ch.isalnum() else "_" for ch in name)
            path = os.path.join(where, "im_%s_%dx%d.bmp" % (slug, w, h))
            magick(*[a % path if "%s" in a else a for a in argv])
            files.append(("bmp magick %s %dx%d" % (name, w, h), path, w, h, oracle, tol))

    for tag, path, w, h, oracle, tol in files:
        checks += 1
        err, gw, gh, got = run_decode(exe, path, tmp)
        if got is None:
            failures.append("%s: %s" % (tag, err))
            continue
        if (gw, gh) != (w, h):
            failures.append("%s: decoded %dx%d" % (tag, gw, gh))
            continue
        want = (pixels(Image.open(path).convert("RGBA")) if oracle == "pillow"
                else bmp16_ref(path))
        e = near(tag, want, got, tol, clear_eq=True)
        if e:
            failures.append(e)
        else:
            say(tag)

    for tag, blob, w, h, want in bmp_hand_cases():
        tag = "bmp hand " + tag
        path = os.path.join(where, "".join(ch if ch.isalnum() else "_" for ch in tag) + ".bmp")
        open(path, "wb").write(blob)
        checks += 1
        err, gw, gh, got = run_decode(exe, path, tmp)
        if got is None:
            failures.append("%s: %s" % (tag, err))
        elif (gw, gh) != (w, h):
            failures.append("%s: decoded %dx%d" % (tag, gw, gh))
        else:
            e = near(tag, want, got)
            if e:
                failures.append(e)
            else:
                say(tag)

    for tag, blob, rc in bmp_refusals():
        tag = "bmp refuse " + tag
        path = os.path.join(tmp, "refuse.bmp")
        open(path, "wb").write(blob)
        checks += 1
        err, _w, _h, _got = run_decode(exe, path, tmp)
        if ("rc=%d" % rc) not in err:
            failures.append("%s: wanted rc=%d, got %s" % (tag, rc, err))
        else:
            say(tag, "refused with the right errno")

    # --- GIF ------------------------------------------------------------
    gifs = []
    noise = patterns("noise", 40, 37).convert("RGB")
    p = os.path.join(where, "pil_256.gif")
    noise.quantize(256).save(p)
    gifs.append(("gif pillow 256 colours", p, False))
    p = os.path.join(where, "pil_2.gif")
    noise.convert("1").save(p)
    gifs.append(("gif pillow 2 colours", p, False))
    p = os.path.join(where, "pil_1x1.gif")
    patterns("flat", 1, 1).convert("RGB").save(p)
    gifs.append(("gif pillow 1x1", p, False))
    p = os.path.join(where, "pil_interlaced.gif")
    patterns("gradient", 33, 29).convert("RGB").quantize(200).save(p, interlace=True)
    gifs.append(("gif pillow interlaced", p, True))
    src = os.path.join(tmp, "noise.png")
    noise.save(src)
    p = os.path.join(where, "im_interlaced.gif")
    magick(src, "-colors", "64", "-interlace", "GIF", p)
    gifs.append(("gif magick interlaced", p, True))

    # Pillow's animation: disposals 2, 3, 1 over a transparent index,
    # each frame its own delay.
    pal = [255, 0, 0, 0, 255, 0, 0, 0, 255, 9, 9, 9] + [0] * 756
    fr = []
    for k in range(3):
        f = Image.new("P", (12, 9))
        f.putpalette(pal)
        for y in range(9):
            for x in range(12):
                f.putpixel((x, y), 3 if (k == 1 and x < 5) else (x + y + k) % 3)
        fr.append(f)
    p = os.path.join(where, "pil_anim.gif")
    fr[0].save(p, save_all=True, append_images=fr[1:], duration=[50, 70, 90], loop=0,
               disposal=[2, 3, 1], transparency=3, optimize=False)
    gifs.append(("gif pillow anim, disposals 2 3 1", p, False))

    # ImageMagick's: frames smaller than the screen at offsets, their own
    # colour tables, a fully transparent first frame, all three disposals.
    parts = []
    for k, c in enumerate([(255, 0, 0), (0, 200, 0), (0, 0, 255), (200, 200, 0)]):
        f = Image.new("RGBA", (10 + k * 3, 8 + k * 2), c + (255,))
        for x in range(f.width):
            f.putpixel((x, 0), (0, 0, 0, 0))
        fp = os.path.join(tmp, "f%d.png" % k)
        f.save(fp)
        parts.append(fp)
    p = os.path.join(where, "im_anim.gif")
    # `-set` per parenthesised frame: a bare -page is a setting that ends
    # up applied to every frame at once.
    argv = ["-size", "30x24", "xc:none", "-set", "page", "30x24+0+0"]
    for fp, page, delay, disp in ((parts[0], "+0+0", 7, "None"),
                                  (parts[1], "+5+3", 12, "Background"),
                                  (parts[2], "+9+6", 0, "Previous"),
                                  (parts[3], "+2+9", 30, "None")):
        argv += ["(", fp, "-set", "page", page, "-set", "delay", str(delay),
                 "-set", "dispose", disp, ")"]
    magick(*argv, "-loop", "3", p)
    gifs.append(("gif magick anim, offsets + disposals", p, False))

    for tag, path, interlaced in gifs:
        d = open(path, "rb").read()
        if interlaced and not gif_descriptors(d)[0][1]:
            # The PNG harness's lesson: a fixture that is not what its
            # name says tests nothing and passes.
            failures.append("%s: the fixture is not interlaced" % tag)
            continue
        ref = Image.open(path)
        n = getattr(ref, "n_frames", 1)
        checks += 1
        err, w, h, loops, frames = run_anim(exe, path, tmp)
        if frames is None:
            failures.append("%s: %s" % (tag, err))
            continue
        if (w, h) != ref.size or len(frames) != n:
            failures.append("%s: %dx%d, %d frames; Pillow says %dx%d, %d"
                            % (tag, w, h, len(frames), ref.size[0], ref.size[1], n))
            continue
        bad = None
        for k in range(n):
            ref.seek(k)
            want = pixels(ref.convert("RGBA"))
            e = near("%s frame %d" % (tag, k), want, frames[k][1], clear_eq=True)
            if not e and n > 1 and frames[k][0] != ref.info.get("duration", 0):
                e = "%s frame %d: delay %d, Pillow says %s" % (tag, k, frames[k][0],
                                                              ref.info.get("duration"))
            if e:
                bad = e
                break
        file_loop = ref.info.get("loop")
        want_loops = 1 if file_loop is None else (0 if file_loop == 0 else file_loop + 1)
        if not bad and loops != want_loops:
            bad = "%s: loops %d, want %d (NETSCAPE says %s)" % (tag, loops, want_loops, file_loop)
        # uimg_decode() is frame 0 of the same walk.
        if not bad:
            _e, _w, _h, still = run_decode(exe, path, tmp)
            if still != frames[0][1]:
                bad = "%s: the still is not frame 0" % tag
        if bad:
            failures.append(bad)
        else:
            say(tag, "ok, %d frame(s)" % n)

    # --- a truncated GIF: the whole frames before the damage play ---------
    d = open(os.path.join(where, "pil_anim.gif"), "rb").read()
    desc = gif_descriptors(d)
    cut = os.path.join(tmp, "cut.gif")
    open(cut, "wb").write(d[:desc[2][0] + 14])          # inside frame 2's header
    checks += 1
    err, _w, _h, _l, frames = run_anim(exe, cut, tmp)
    if frames is None or len(frames) != 2:
        failures.append("gif truncated after two frames: %s"
                        % (err if frames is None else "%d frames" % len(frames)))
    else:
        say("gif truncated after two frames", "plays the two")
    open(cut, "wb").write(d[:desc[0][2] - 9])             # inside frame 0's data
    checks += 1
    err, _w, _h, _got = run_decode(exe, cut, tmp)
    if "rc=-22" not in err:
        failures.append("gif truncated inside frame 0: wanted rc=-22, got %s" % err)
    else:
        say("gif truncated inside frame 0", "refused")
    return checks, failures


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true",
                    help="break each codec but QOI; its checks MUST fail")
    ap.add_argument("--keep", metavar="DIR", help="keep the encoded files here")
    args = ap.parse_args()

    cases = [("flat", 64, 48, False), ("gradient", 97, 61, False),
             ("noise", 40, 40, False), ("ui", 320, 200, False),
             ("palette", 71, 53, False), ("alpha", 48, 48, True),
             ("ui", 1, 1, False), ("gradient", 1, 300, False)]

    failures, checks = [], 0
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp, args.positive_control)
        keep = args.keep
        if keep:
            os.makedirs(keep, exist_ok=True)

        for kind, w, h, alpha in cases:
            tag = "%s-%dx%d%s" % (kind, w, h, "-a" if alpha else "")
            im = patterns(kind, w, h)
            want = pixels(im)

            rawp = os.path.join(tmp, tag + ".raw")
            open(rawp, "wb").write(raw_argb(im))
            qoip = os.path.join(keep or tmp, tag + ".qoi")
            pngp = os.path.join(keep or tmp, tag + ".png")
            r = subprocess.run([exe, rawp, str(w), str(h), "1" if alpha else "0",
                                qoip, pngp], capture_output=True)
            if r.returncode:
                failures.append("%s: encoder exited %d: %s"
                                % (tag, r.returncode, r.stderr.decode().strip()))
                continue

            # 1. QOI, decoded by Pillow's plugin.
            checks += 1
            try:
                got = pixels(Image.open(qoip).convert("RGBA"))
                e = compare(tag + " qoi/Pillow", want, got, alpha)
            except Exception as ex:                    # noqa: BLE001
                e = "%s qoi/Pillow: %s" % (tag, ex)
            if e:
                failures.append(e)

            # 2. PNG, decoded by Pillow.
            checks += 1
            try:
                got = pixels(Image.open(pngp).convert("RGBA"))
                e = compare(tag + " png/Pillow", want, got, alpha)
            except Exception as ex:                    # noqa: BLE001
                e = "%s png/Pillow: %s" % (tag, ex)
            if e:
                failures.append(e)

            # 3. PNG again, inflated by zlib and unfiltered here.
            checks += 1
            try:
                pw, ph, bpp, raw = unfilter_png(pngp)
                got = []
                for i in range(pw * ph):
                    c = raw[i * bpp:(i + 1) * bpp]
                    got.append((c[0], c[1], c[2], c[3] if bpp == 4 else 255))
                e = compare(tag + " png/zlib", want, got, alpha)
                if not e and (pw, ph) != (w, h):
                    e = "%s png/zlib: %dx%d, want %dx%d" % (tag, pw, ph, w, h)
            except Exception as ex:                    # noqa: BLE001
                e = "%s png/zlib: %s" % (tag, ex)
            if e:
                failures.append(e)

            if not args.positive_control:
                q, p = os.path.getsize(qoip), os.path.getsize(pngp)
                print("  %-18s %7d px  qoi %7d  png %7d"
                      % (tag, w * h, q, p))

        # --- the DECODE sweep: files Pillow wrote, read by us --------------
        #
        # The axes are what a PNG decoder actually has to get right: every
        # colour type, every row filter (Pillow picks per row unless told),
        # a palette with and without transparency, and sizes whose stride is
        # not a round number. Lossless, so the comparison is EXACT -- there
        # is no tolerance to hide behind, unlike the JPEG harness.
        dec_cases = []
        for mode in ("L", "RGB", "P", "LA", "RGBA"):
            for (w, h) in ((1, 1), (7, 3), (64, 48), (129, 77)):
                dec_cases.append((mode, w, h, None))
        # Explicit per-row filters, which is where unfiltering goes wrong.
        for f in (0, 1, 2, 3, 4):
            dec_cases.append(("RGB", 61, 23, f))
            dec_cases.append(("RGBA", 61, 23, f))

        with tempfile.TemporaryDirectory() as tmp2:
            exe2 = exe  # built in the OUTER temp dir, which is still open
            for (mode, w, h, filt) in dec_cases:
                tag = "decode %s %dx%d%s" % (mode, w, h,
                                              " filter%d" % filt if filt is not None else "")
                im = patterns("gradient" if mode != "P" else "palette", w, h)
                im = im.convert(mode)
                src = os.path.join(keep or tmp2, tag.replace(" ", "_") + ".png")
                if filt is None:
                    im.save(src, format="PNG")
                else:
                    # BUILT BY HAND, because Pillow chooses filters per row
                    # and gives no way to demand one. Asking it politely
                    # produced cases whose names said "filter3" and whose
                    # bytes were whatever Pillow preferred -- four checks
                    # that tested nothing they claimed to.
                    write_png_filtered(src, im, filt)
                raw = os.path.join(tmp2, "d.raw")
                r = subprocess.run([exe2, "decode", src, raw], capture_output=True)
                checks += 1
                if r.returncode != 0:
                    failures.append("%s: %s" % (tag, r.stderr.decode().strip()))
                    continue
                want = pixels(im.convert("RGBA"))
                body = open(raw, "rb").read()
                nl = body.index(b"\n")
                gw, gh, _alpha = (int(v) for v in body[:nl].split())
                data = body[nl + 1:]
                got = [(data[i * 4 + 1], data[i * 4 + 2], data[i * 4 + 3], data[i * 4])
                       for i in range(gw * gh)]
                if (gw, gh) != (w, h):
                    failures.append("%s: decoded %dx%d" % (tag, gw, gh))
                    continue
                e = compare(tag, want, got, True)
                if e:
                    failures.append(e)
                elif not args.positive_control:
                    print("  %-30s %s" % (tag, "ok"))

            # --- what it must REFUSE, and with WHICH error ------------------
            #
            # -ENOTSUP and -EINVAL are not interchangeable here: one says the
            # file is fine and this build is not, the other says the file is
            # broken. An app prints a different sentence for each.
            def make_interlaced(path):
                """An Adam7 IHDR, built BY HAND.

                Pillow silently ignores `interlace=1` and writes a
                progressive-free file, so asking it for one produced a
                fixture that never reached the code under test -- and the
                check passed for the wrong reason. Flipping the byte and
                repairing the IHDR's CRC is the smallest thing that is
                actually an interlaced header; the pixel data behind it
                is not valid Adam7, which does not matter because the
                refusal happens in the header, before anything reads it.
                """
                patterns("gradient", 32, 16).convert("RGB").save(path)
                b = bytearray(open(path, "rb").read())
                b[8 + 8 + 12] = 1                      # IHDR interlace
                crc = zlib.crc32(bytes(b[12:12 + 4 + 13])) & 0xFFFFFFFF
                b[12 + 4 + 13:12 + 4 + 13 + 4] = struct.pack(">I", crc)
                open(path, "wb").write(bytes(b))

            refusals = [
                ("16-bit", lambda p: patterns("gradient", 32, 16).convert("I;16").save(p), -95),
                ("interlaced", make_interlaced, -95),
            ]
            for tag, make, want_rc in refusals:
                src = os.path.join(tmp2, tag + ".png")
                try:
                    make(src)
                except Exception:                        # noqa: BLE001
                    continue                              # Pillow cannot make it here
                r = subprocess.run([exe2, "decode", src, os.path.join(tmp2, "x.raw")],
                                    capture_output=True)
                checks += 1
                err = r.stderr.decode()
                ok = ("rc=%d" % want_rc) in err
                if not ok:
                    failures.append("%s: wanted rc=%d, got %s" % (tag, want_rc, err.strip()))
                elif not args.positive_control:
                    print("  %-30s refused with the right errno" % ("refuse " + tag))

            # A CORRUPT chunk must be -EINVAL, not a picture.
            good = os.path.join(tmp2, "ok.png")
            patterns("ui", 40, 30).convert("RGB").save(good)
            blob = bytearray(open(good, "rb").read())
            blob[-6] ^= 0xFF                              # inside IEND's CRC
            bad = os.path.join(tmp2, "bad.png")
            open(bad, "wb").write(bytes(blob))
            r = subprocess.run([exe2, "decode", bad, os.path.join(tmp2, "x.raw")],
                                capture_output=True)
            checks += 1
            if "rc=-22" not in r.stderr.decode():
                failures.append("a corrupted chunk decoded anyway: %s"
                                % r.stderr.decode().strip())
            elif not args.positive_control:
                print("  %-30s rejected" % "corrupt chunk")

        c2, f2 = sweep_bmp_gif(exe, tmp, keep, args.positive_control)
        checks += c2
        failures += f2

    if args.positive_control:
        # BOTH HALVES MUST GO RED, AND QOI MUST NOT. The two sabotages
        # are deliberately in different files and different directions,
        # so this says which half of the harness is actually live rather
        # than only that something failed.
        enc_bad = [f for f in failures if "png/" in f]
        dec_bad = [f for f in failures if f.startswith("decode ")]
        bmp_bad = [f for f in failures if f.startswith("bmp ")]
        gif_bad = [f for f in failures if f.startswith("gif ")]
        qoi_bad = [f for f in failures if "qoi" in f]
        print("positive control: %d encode, %d decode, %d BMP, %d GIF, %d QOI checks failed"
              % (len(enc_bad), len(dec_bad), len(bmp_bad), len(gif_bad), len(qoi_bad)))
        bad = False
        if not enc_bad:
            print("FAIL: the encode half did not redden -- it is untested")
            bad = True
        if not dec_bad:
            print("FAIL: the decode half did not redden -- it is untested")
            bad = True
        for name, hits in (("BMP", bmp_bad), ("GIF", gif_bad)):
            if not hits:
                print("FAIL: the %s checks did not redden -- they are untested" % name)
                bad = True
        if qoi_bad:
            print("FAIL: the control also broke QOI, so it isolates nothing")
            for f in qoi_bad:
                print("   ", f)
            bad = True
        if bad:
            return 1
        print("OK: every check family fails when its codec is wrong")
        return 0

    print("%d checks over %d images" % (checks, len(cases)))
    for f in failures:
        print("FAIL:", f)
    print("uimg_codec_hostcheck: %s" % ("FAILED" if failures else "all passed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
