#!/usr/bin/env python3
"""Check uimg's QOI and PNG codecs against Pillow and zlib, on the HOST.

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

// argv: <raw> <w> <h> <alpha 0|1> <out.qoi> <out.png>
int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "decode") == 0)
        return do_decode(argv[2], argv[3]);
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

    exe = os.path.join(tmp, "uimgenc")
    cmd = ["gcc", "-O2", "-Wall", "-Wextra", "-Werror", "-o", exe, src,
           os.path.join(ROOT, "userland", "lib", "uimg_qoi.c"), png_c, infl_c,
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
                kw = {}
                if filt is not None:
                    kw = {"compress_level": 9, "bits": 8}
                im.save(src, format="PNG", **({} if filt is None else {}))
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

    if args.positive_control:
        # BOTH HALVES MUST GO RED, AND QOI MUST NOT. The two sabotages
        # are deliberately in different files and different directions,
        # so this says which half of the harness is actually live rather
        # than only that something failed.
        enc_bad = [f for f in failures if "png/" in f]
        dec_bad = [f for f in failures if f.startswith("decode ")]
        qoi_bad = [f for f in failures if "qoi" in f]
        print("positive control: %d encode, %d decode, %d QOI checks failed"
              % (len(enc_bad), len(dec_bad), len(qoi_bad)))
        bad = False
        if not enc_bad:
            print("FAIL: the encode half did not redden -- it is untested")
            bad = True
        if not dec_bad:
            print("FAIL: the decode half did not redden -- it is untested")
            bad = True
        if qoi_bad:
            print("FAIL: the control also broke QOI, so it isolates nothing")
            for f in qoi_bad:
                print("   ", f)
            bad = True
        if bad:
            return 1
        print("OK: both halves fail when the codec is wrong")
        return 0

    print("%d checks over %d images" % (checks, len(cases)))
    for f in failures:
        print("FAIL:", f)
    print("uimg_codec_hostcheck: %s" % ("FAILED" if failures else "all passed"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
