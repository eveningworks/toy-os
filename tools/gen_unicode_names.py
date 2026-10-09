#!/usr/bin/env python3
"""tools/gen_unicode_names.py -- the Unicode NAMES and BLOCKS of every
character the shipped fonts can draw, for the Character Map.

Two small text files into data/usr/share/unicode/, read by
userland/lib/uunicode.c:

    names    one line per character: "00E9;LATIN SMALL LETTER E WITH ACUTE"
    blocks   one line per block that holds any of them: "0080;00FF;Latin-1 Supplement"

ONLY WHAT THE FONTS COVER. The whole database names ~150,000 characters
(~2 MB of names); the fonts in data/fonts have a few thousand between them, and
a name for a character nothing can draw is a row the Character Map would
have to hide. The cmaps are read here, from the same files the build
seeds. Re-run when a font is added.

THE SOURCES ARE ON THIS MACHINE, NOTHING IS FETCHED: the names are
Python's own `unicodedata` (the Unicode Character Database, at the
version the interpreter carries -- printed below), and the block ranges
are the database's Blocks.txt, which Perl ships (the --blocks path's
default). Both are Unicode, Inc.'s data under the Unicode License v3,
whose notice travels as data/licenses/unicode.txt.

    python3 tools/gen_unicode_names.py [--blocks PATH] [--out DIR]
"""
import argparse
import os
import struct
import sys
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
FONTS = os.path.join(REPO, "data", "fonts")
DEFAULT_BLOCKS = "/usr/share/perl5/core_perl/unicore/Blocks.txt"


def cmap_codepoints(path):
    """Every code point a TrueType font maps, from its cmap (formats 4 and 12)."""
    d = open(path, "rb").read()
    ntab = struct.unpack(">H", d[4:6])[0]
    tables = {}
    for i in range(ntab):
        tag, _, off, ln = struct.unpack(">4sIII", d[12 + 16 * i:28 + 16 * i])
        tables[tag] = off
    base = tables[b"cmap"]
    nsub = struct.unpack(">H", d[base + 2:base + 4])[0]
    cps = set()
    for i in range(nsub):
        pid, eid, so = struct.unpack(">HHI", d[base + 4 + 8 * i:base + 12 + 8 * i])
        p = base + so
        fmt = struct.unpack(">H", d[p:p + 2])[0]
        if fmt == 4 and pid in (0, 3):
            segx2 = struct.unpack(">H", d[p + 6:p + 8])[0]
            seg = segx2 // 2
            ends = struct.unpack(">%dH" % seg, d[p + 14:p + 14 + segx2])
            starts = struct.unpack(">%dH" % seg, d[p + 16 + segx2:p + 16 + 2 * segx2])
            for s, e in zip(starts, ends):
                if s != 0xFFFF:
                    cps.update(range(s, e + 1))
        elif fmt == 12:
            n = struct.unpack(">I", d[p + 12:p + 16])[0]
            for k in range(n):
                s, e, _ = struct.unpack(">III", d[p + 16 + 12 * k:p + 28 + 12 * k])
                cps.update(range(s, e + 1))
    return cps


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--blocks", default=DEFAULT_BLOCKS, help="Unicode's Blocks.txt")
    ap.add_argument("--out", default=os.path.join(REPO, "data", "usr", "share", "unicode"))
    args = ap.parse_args()
    if not os.path.exists(args.blocks):
        sys.exit(f"gen_unicode_names: no Blocks.txt at {args.blocks} (pass --blocks)")

    cps = set()
    for f in sorted(os.listdir(FONTS)):
        if f.endswith(".ttf"):
            cps |= cmap_codepoints(os.path.join(FONTS, f))
    named = {}
    for cp in sorted(cps):
        try:
            named[cp] = unicodedata.name(chr(cp))
        except ValueError:
            pass    # controls and unassigned have no name: nothing to show

    blocks = []
    for line in open(args.blocks, encoding="utf-8"):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        rng, name = line.split(";")
        lo, hi = (int(x, 16) for x in rng.split(".."))
        if any(lo <= cp <= hi for cp in named):
            blocks.append((lo, hi, name.strip()))

    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "names"), "w", encoding="ascii") as f:
        for cp, name in named.items():
            f.write("%04X;%s\n" % (cp, name))
    with open(os.path.join(args.out, "blocks"), "w", encoding="ascii") as f:
        for lo, hi, name in blocks:
            f.write("%04X;%04X;%s\n" % (lo, hi, name))
    print(f"gen_unicode_names: Unicode {unicodedata.unidata_version}, "
          f"{len(named)} names in {len(blocks)} blocks -> {args.out}")


if __name__ == "__main__":
    main()
