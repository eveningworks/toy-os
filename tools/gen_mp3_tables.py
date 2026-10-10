#!/usr/bin/env python3
"""tools/gen_mp3_tables.py -- regenerate userland/lib/usnd_mp3_tables.h.

WHY A GENERATOR AND NOT JUST THE HEADER
---------------------------------------
Three tables in MPEG-1 Layer III have no generating formula: the Huffman
codeword tables, the 512-tap polyphase synthesis window, and the
scalefactor band edges. They are ISO/IEC 11172-3's data, so they were
TAKEN rather than written (LICENSE records where from, and that nothing
is owed for them -- both sources are public domain).

Data that was taken needs a way to be CHECKED, or the header is 473
lines nobody can audit without redoing the work. This is that way. It
also makes the header a generated file rather than hand-written source,
which is what keeps tools/loc.py honest.

**IT VERIFIES MORE THAN IT COPIES.** Two independent implementations
store the Huffman tables in two completely different packed formats;
this walks BOTH back to the standard's plain (length, codeword) per
(x, y) form and refuses to write anything unless they agree entry for
entry -- 1378 entries across 15 tables. On top of that, every table must
be a complete prefix code (Kraft sum exactly 1). That pair of checks is
what makes the output trustworthy: a transcription error cannot survive
two unrelated sources agreeing, and a structural error cannot survive
Kraft.

Both checks earned their place. An early hand-written table 7 failed
Kraft, which is how transcribing from memory was abandoned; and a first
version of the pdmp3 walk below quietly dropped that format's long-jump
escape, which showed up as table 24 disagreeing in 63 of 256 entries --
the disagreement was the walker, not the data, and only having two
sources made that visible at all.

USAGE
    python3 tools/gen_mp3_tables.py            # fetch the sources, verify, write
    python3 tools/gen_mp3_tables.py --check    # verify only; write nothing
    python3 tools/gen_mp3_tables.py --from DIR # use local copies instead

`--from` takes a directory holding `minimp3.h` and `pdmp3.c`. Without it
this DOWNLOADS them, which is the only reason it is not run by the build
-- and it is not needed by the build, because the header it writes is
committed.
"""
import argparse
import os
import re
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fetch_extras import urlopen  # noqa: E402 -- names itself; see USER_AGENT
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
OUT = os.path.join(REPO, "userland", "lib", "usnd_mp3_tables.h")

SOURCES = {
    # (filename, url, licence) -- both public domain dedications, so
    # nothing is owed; the provenance is recorded because a reader of
    # LICENSE should be able to learn where data in the tree came from.
    "minimp3.h": ("https://raw.githubusercontent.com/lieff/minimp3/master/minimp3.h",
                  "CC0-1.0"),
    "pdmp3.c": ("https://raw.githubusercontent.com/technosaurus/PDMP3/master/pdmp3.c",
                "the Unlicense"),
}

# (x, y) dimension per table number. 0, 4 and 14 are unused by the
# standard; 17-23 share table 16's codewords and 25-31 share table 24's,
# differing only in linbits -- the standard's own structure.
DIM = {1: 2, 2: 3, 3: 3, 5: 4, 6: 4, 7: 6, 8: 6, 9: 6,
       10: 8, 11: 8, 12: 8, 13: 16, 15: 16, 16: 16, 24: 16}
LINBITS = [0] * 16 + [1, 2, 3, 4, 6, 8, 10, 13, 4, 5, 6, 7, 8, 9, 11, 13]

# Count1 table A (ISO table 32): four values per codeword, each -1/0/+1.
# Small enough to write out, and cross-checked against minimp3's packed
# form by check_count1() below rather than trusted.
Q1A_LEN = [1, 4, 4, 5, 4, 6, 5, 6, 4, 5, 5, 6, 5, 6, 6, 6]
Q1A_CODE = [1, 5, 4, 5, 6, 5, 4, 4, 7, 3, 6, 0, 7, 2, 3, 1]


def fetch(src_dir):
    """The two sources, from disk or from the network."""
    out = {}
    for name, (url, lic) in SOURCES.items():
        if src_dir:
            path = os.path.join(src_dir, name)
            if not os.path.exists(path):
                sys.exit(f"gen_mp3_tables: {path} not found")
            out[name] = open(path, encoding="utf-8", errors="replace").read()
            print(f"  read {path} ({lic})")
        else:
            print(f"  fetching {url} ({lic})")
            with urlopen(url, timeout=60) as r:
                out[name] = r.read().decode("utf-8", "replace")
    return out


# --- source A: minimp3's packed multi-level lookup --------------------

def tables_from_minimp3(text):
    """{table -> {(x, y): (length, codeword)}}.

    The format is a multi-level LUT: a negative entry means "consume the
    current width and continue in a sub-table", a non-negative one is a
    leaf whose high byte is how many of the peeked bits it actually used.
    """
    tabs = [int(v) for v in re.findall(r"-?\d+", re.search(
        r"static const int16_t tabs\[\] = \{(.*?)\};", text, re.S).group(1))]
    index = [int(v) for v in re.findall(r"-?\d+", re.search(
        r"static const int16_t tabindex\[2\*16\] = \{(.*?)\};", text, re.S).group(1))]

    def walk(base):
        found = {}

        def rec(off, width, prefix, plen):
            for p in range(1 << width):
                leaf = tabs[off + p]
                if leaf < 0:
                    rec(base + (-(leaf >> 3)), leaf & 7,
                        (prefix << width) | p, plen + width)
                else:
                    n = leaf >> 8
                    code = (prefix << n) | (p >> (width - n)) if n else prefix
                    found[(leaf & 0x0F, (leaf >> 4) & 0x0F)] = (plen + n, code)
        rec(base, 5, 0, 0)
        return found

    return {t: walk(index[t]) for t in DIM}


def count1_from_minimp3(text):
    """The same recovery for ISO table 32, so Q1A_* above is checked."""
    tab32 = [int(v) for v in re.findall(r"\d+", re.search(
        r"static const uint8_t tab32\[\] = \{(.*?)\};", text, re.S).group(1))]
    found = {}

    def rec(base, width, prefix, plen):
        for p in range(1 << width):
            leaf = tab32[base + p]
            if not (leaf & 8):
                rec(leaf >> 3, leaf & 3, (prefix << width) | p, plen + width)
            else:
                n = leaf & 7
                extra = n - plen
                code = (prefix << extra) | (p >> (width - extra)) if extra > 0 else prefix
                found[(leaf >> 4) & 0x0F] = (n, code)
    rec(0, 4, 0, 0)
    return found


# --- source B: pdmp3's binary tree, in plain ISO order ----------------

def tables_from_pdmp3(text):
    """The same mapping, from a completely different representation.

    Each 16-bit entry is an interior node whose HIGH byte is the step to
    take on a 0 and LOW byte the step on a 1, or a leaf (high byte zero)
    holding x and y in its nibbles. **The `>= 250` escape is not
    optional**: an offset that does not fit in a byte chains through
    intermediate entries, and ignoring it silently corrupts exactly the
    largest table (24), which is the one where offsets get big.
    """
    body = re.sub(r"//[^\n]*", "", re.search(
        r"static const unsigned short g_huffman_table\[\] = \{(.*?)\};",
        text, re.S).group(1))
    tab = [int(v, 16) for v in re.findall(r"0x[0-9a-fA-F]+", body)]

    rows = {}
    for line in re.search(r"hufftables g_huffman_main \[34\] = \{(.*?)\};",
                          text, re.S).group(1).splitlines():
        m = re.search(r"\{g_huffman_table\s*\+?\s*(\d*)\s*,\s*(\d+)\s*,"
                      r"\s*(\d+)\s*\}.*Table\s+(\d+)", line)
        if m:
            rows[int(m.group(4))] = (int(m.group(1) or 0), int(m.group(3)))

    def step(base, point, bit):
        if bit:
            while (tab[base + point] & 0xFF) >= 250:
                point += tab[base + point] & 0xFF
            return point + (tab[base + point] & 0xFF)
        while (tab[base + point] >> 8) >= 250:
            point += tab[base + point] >> 8
        return point + (tab[base + point] >> 8)

    def walk(base):
        found = {}

        def rec(point, code, ln):
            if ln > 22:
                return
            v = tab[base + point]
            if (v & 0xFF00) == 0:
                found[((v >> 4) & 0x0F, v & 0x0F)] = (ln, code)
                return
            rec(step(base, point, 1), (code << 1) | 1, ln + 1)
            rec(step(base, point, 0), code << 1, ln + 1)
        rec(0, 0, 0)
        return found

    out = {}
    for t in DIM:
        base, _lin = rows[t]
        out[t] = walk(base)
    return out, rows


def window_from_pdmp3(text):
    d = [float(v) for v in re.findall(r"-?\d+\.\d+", re.search(
        r"g_synth_dtbl\[512\] = \{(.*?)\};", text, re.S).group(1))]
    if len(d) != 512:
        sys.exit(f"gen_mp3_tables: expected 512 window taps, got {len(d)}")
    return d


def sfb_from_pdmp3(text):
    block = re.search(r"g_sf_band_indices\[3[^]]*\] = \{(.*?)\n  \};",
                      text, re.S).group(1)
    groups = re.findall(r"\{([0-9,\s]+)\}", block)
    long_, short_ = [], []
    for i in range(3):
        long_.append([int(v) for v in groups[i * 2].split(",")])
        short_.append([int(v) for v in groups[i * 2 + 1].split(",")])
    return long_, short_


# --- the checks that make the output trustworthy ----------------------

def verify(a, b, rows, long_, short_):
    """Every disagreement, as a list of complaints. Empty means good."""
    bad = []
    for t, dim in sorted(DIM.items()):
        ta, tb = a[t], b[t]
        kraft = sum(Fraction(1, 1 << ln) for ln, _ in ta.values())
        if kraft != 1:
            bad.append(f"table {t}: Kraft sum is {float(kraft)}, not 1 "
                       f"-- not a complete prefix code")
        if len(ta) != dim * dim:
            bad.append(f"table {t}: {len(ta)} entries, expected {dim * dim}")
        for x in range(dim):
            for y in range(dim):
                if ta.get((x, y)) != tb.get((x, y)):
                    bad.append(f"table {t} ({x},{y}): minimp3 says "
                               f"{ta.get((x, y))}, pdmp3 says {tb.get((x, y))}")
        if rows[t][1] != LINBITS[t]:
            bad.append(f"table {t}: linbits {rows[t][1]} != expected {LINBITS[t]}")
    for i, l in enumerate(long_):
        if len(l) != 23 or l[-1] != 576:
            bad.append(f"long scalefactor bands[{i}] malformed: {l}")
    for i, s in enumerate(short_):
        if len(s) != 14 or s[-1] != 192:
            bad.append(f"short scalefactor bands[{i}] malformed: {s}")
    return bad


def check_count1(recovered):
    bad = []
    k = sum(Fraction(1, 1 << ln) for ln in Q1A_LEN)
    if k != 1:
        bad.append(f"count1 table A: Kraft sum {float(k)}, not 1")
    for v in range(16):
        if recovered.get(v) != (Q1A_LEN[v], Q1A_CODE[v]):
            bad.append(f"count1 table A, symbol {v}: written "
                       f"{(Q1A_LEN[v], Q1A_CODE[v])}, minimp3 says "
                       f"{recovered.get(v)}")
    return bad


# --- emitting ---------------------------------------------------------

HEADER = '''// usnd_mp3_tables.h -- the data ISO/IEC 11172-3 defines for Layer III.
//
// GENERATED by tools/gen_mp3_tables.py. Don't hand-edit: that script
// verifies what it writes (two independent sources agreeing entry for
// entry, plus a complete-prefix-code check on every table), and an edit
// here is an edit nothing checks.
//
// Separated from usnd_mp3.c because it is DATA and that file is LOGIC:
// nothing here is a decision anybody made about this OS, and a thousand
// constants inline would bury the twelve stages worth reading.
//
// **THREE OF THESE CANNOT BE COMPUTED, ONLY LOOKED UP** -- the Huffman
// codes, the 512-tap synthesis window, and the scalefactor band edges.
// Everything else a decoder needs IS derivable and is derived at runtime
// in usnd_mp3.c rather than frozen here: the alias coefficients from
// ci[], the intensity ratios from tan(i*pi/12), the IMDCT windows from
// sin(). A table is what you write when there is no formula, not a place
// to cache one.
//
// The Huffman tables are checked again at RUNTIME by
// usnd_mp3_selftest(): every one a complete prefix code over exactly
// dim*dim pairs. That check needs no audio and no reference, which is
// why it ships rather than living only in the generator.
#ifndef USND_MP3_TABLES_H
#define USND_MP3_TABLES_H

#include <stdint.h>
'''


def emit(tables, window, long_, short_):
    out = [HEADER]
    w = out.append
    w("// --- Huffman: one (length, codeword) per (x, y), row-major in x ----")
    w("//")
    w("// Tables 17-23 share table 16's codewords and 25-31 share table 24's,")
    w("// differing only in linbits -- the standard's own structure, not a")
    w("// saving invented here.")
    for t in sorted(tables):
        dim = DIM[t]
        lens, codes = [], []
        for x in range(dim):
            for y in range(dim):
                ln, code = tables[t][(x, y)]
                lens.append(ln)
                codes.append(code)
        w(f"static const uint8_t mp3_hlen{t}[{dim * dim}] = {{")
        for i in range(0, len(lens), 16):
            w("    " + " ".join("%2d," % v for v in lens[i:i + 16]))
        w("};")
        w(f"static const uint16_t mp3_hcode{t}[{dim * dim}] = {{")
        for i in range(0, len(codes), 16):
            w("    " + " ".join("%4d," % v for v in codes[i:i + 16]))
        w("};")
    w("")
    w("struct mp3_htable { const uint8_t *len; const uint16_t *code; "
      "uint8_t dim; uint8_t linbits; };")
    w("")
    w("// Indexed by table_select. 0, 4 and 14 are unused by the standard;")
    w("// table 0 means \"this region is all zeroes\" and consumes no bits.")
    w("static const struct mp3_htable mp3_htables[32] = {")
    for t in range(32):
        if t in (0, 4, 14):
            w("    { 0, 0, 0, 0 },")
            continue
        src = 16 if 16 <= t <= 23 else 24 if t >= 24 else t
        w(f"    {{ mp3_hlen{src}, mp3_hcode{src}, {DIM[src]}, {LINBITS[t]} }},")
    w("};")
    w("")
    w("// The count1 region: four values per codeword, each -1, 0 or +1.")
    w("// Table B is a flat 4-bit code and is handled inline by the decoder,")
    w("// which is why only table A needs a shape here.")
    w("static const uint8_t mp3_q1a_len[16] = { "
      + ", ".join(str(v) for v in Q1A_LEN) + " };")
    w("static const uint8_t mp3_q1a_code[16] = { "
      + ", ".join(str(v) for v in Q1A_CODE) + " };")
    w("")
    w("// --- the polyphase synthesis window, 512 taps ----------------------")
    w("//")
    w("// ISO's D[] table. There is no formula: it is a prototype filter the")
    w("// standard's authors designed and tabulated, so it is looked up.")
    w("static const float mp3_synth_window[512] = {")
    for i in range(0, 512, 4):
        w("    " + " ".join("%12.9ff," % v for v in window[i:i + 4]))
    w("};")
    w("")
    w("// --- scalefactor band edges, by MPEG-1 sample-rate index ------------")
    w("//")
    w("// Index 0 = 44100, 1 = 48000, 2 = 32000, the header's own order.")
    w("// `long` runs to 576 and `short` to 192 (times three windows), and")
    w("// both carry the closing edge so a band's width is a subtraction.")
    w("static const uint16_t mp3_sfb_long[3][23] = {")
    for l in long_:
        w("    { " + ", ".join(str(v) for v in l) + " },")
    w("};")
    w("static const uint16_t mp3_sfb_short[3][14] = {")
    for s in short_:
        w("    { " + ", ".join(str(v) for v in s) + " },")
    w("};")
    w("")
    w("// Preemphasis, added to the scalefactor when a granule sets preflag.")
    w("static const uint8_t mp3_pretab[22] = {")
    w("    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 3, 2, 0")
    w("};")
    w("")
    w("#endif // USND_MP3_TABLES_H")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="verify only; write nothing and report whether the "
                         "committed header matches")
    ap.add_argument("--from", dest="src", metavar="DIR",
                    help="use local copies of minimp3.h and pdmp3.c")
    args = ap.parse_args()

    print("gen_mp3_tables: sources")
    src = fetch(args.src)

    print("gen_mp3_tables: recovering the standard's tables from both")
    a = tables_from_minimp3(src["minimp3.h"])
    b, rows = tables_from_pdmp3(src["pdmp3.c"])
    window = window_from_pdmp3(src["pdmp3.c"])
    long_, short_ = sfb_from_pdmp3(src["pdmp3.c"])

    bad = verify(a, b, rows, long_, short_)
    bad += check_count1(count1_from_minimp3(src["minimp3.h"]))
    entries = sum(DIM[t] * DIM[t] for t in DIM)
    if bad:
        print(f"\ngen_mp3_tables: {len(bad)} DISAGREEMENT(S) -- writing nothing")
        for line in bad[:20]:
            print(f"  {line}")
        if len(bad) > 20:
            print(f"  ... and {len(bad) - 20} more")
        return 1
    print(f"  {len(DIM)} Huffman tables, {entries} entries: two independent "
          f"sources agree, every table a complete prefix code")
    print(f"  synthesis window: 512 taps, peak {max(window):.9f}")
    print("  scalefactor bands: 3 rates, long and short")

    text = emit(a, window, long_, short_)
    if args.check:
        current = open(OUT).read() if os.path.exists(OUT) else ""
        if current == text:
            print(f"\ngen_mp3_tables: {os.path.relpath(OUT, REPO)} is current")
            return 0
        print(f"\ngen_mp3_tables: {os.path.relpath(OUT, REPO)} DIFFERS from "
              f"what the sources produce -- rerun without --check")
        return 1
    with open(OUT, "w") as f:
        f.write(text)
    print(f"\ngen_mp3_tables: wrote {os.path.relpath(OUT, REPO)} "
          f"({len(text.splitlines())} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
