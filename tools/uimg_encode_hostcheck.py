#!/usr/bin/env python3
"""Check uimg's ENCODERS against Pillow and zlib, on the HOST.

WHY A FOREIGN DECODER IS THE WHOLE POINT. An encoder tested by this
repo's own decoder passes whenever the two share a mistake, and the two
mistakes an image encoder actually makes are exactly that shape: a
QOI index table updated on the wrong chunk, or a Huffman code packed
least-significant-bit-first. Both produce a file that round-trips
perfectly here and that nothing else in the world can open.

So every file this writes is opened by Pillow, and the PNG's deflate
stream is additionally inflated by Python's zlib and unfiltered by hand
-- three implementations that share no code with userland/lib/.

    python3 tools/uimg_encode_hostcheck.py
    python3 tools/uimg_encode_hostcheck.py --positive-control
    python3 tools/uimg_encode_hostcheck.py --keep /tmp/shots

The positive control is not optional reading: a clean run proves nothing
until the harness has been seen to fail. It breaks the bit reversal in
the deflate writer -- the single most likely real bug here -- and every
PNG check must go red while the QOI ones stay green.

Exit status is non-zero on any mismatch. Needs Pillow and gcc.
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
    sys.exit("uimg_encode_hostcheck.py needs Pillow: pip install --user pillow")

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

// argv: <raw> <w> <h> <alpha 0|1> <out.qoi> <out.png>
int main(int argc, char **argv) {
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
    if sabotage:
        # THE control: a Huffman code that is not reversed. The file
        # stays structurally valid -- signature, chunk lengths and CRCs
        # all correct -- and the pixels are noise, which is exactly the
        # bug a self-decoding test cannot see. Patched into a COPY, so
        # the checked-in source is never edited to run a control.
        body = ("    uint32_t r = 0;\n"
                "    for (int i = 0; i < n; i++) { r = (r << 1) | (v & 1); v >>= 1; }\n"
                "    return r;\n")
        text = open(png_c).read()
        if body not in text:
            sys.exit("positive control: bit_reverse() no longer looks as expected")
        png_c = os.path.join(tmp, "uimg_png_broken.c")
        with open(png_c, "w") as f:
            f.write(text.replace(body, "    (void)n;\n    return v;\n"))

    exe = os.path.join(tmp, "uimgenc")
    cmd = ["gcc", "-O2", "-Wall", "-Wextra", "-Werror", "-o", exe, src,
           os.path.join(ROOT, "userland", "lib", "uimg_qoi.c"), png_c,
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


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--positive-control", action="store_true",
                    help="break the deflate bit order; the PNG checks MUST fail")
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

    if args.positive_control:
        # The sabotage is in the deflate writer only, so QOI must survive
        # it. A control that reddens everything has not isolated anything.
        png_bad = [f for f in failures if "png" in f]
        qoi_bad = [f for f in failures if "qoi" in f]
        print("positive control: %d PNG checks failed, %d QOI checks failed"
              % (len(png_bad), len(qoi_bad)))
        if not png_bad:
            print("FAIL: the control changed nothing -- the harness cannot fail")
            return 1
        if qoi_bad:
            print("FAIL: the control also broke QOI, so it isolates nothing")
            for f in qoi_bad:
                print("   ", f)
            return 1
        print("OK: the harness fails when the encoder is wrong")
        return 0

    print("%d checks over %d images" % (checks, len(cases)))
    for f in failures:
        print("FAIL:", f)
    print("uimg_encode_hostcheck: %s" % ("FAILED" if failures else "all passed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
