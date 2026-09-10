#!/usr/bin/env python3
"""Characterise HOW two copies of a file differ, not just whether.

Written for the silent download corruption in `docs/bugs.md`: a 16 MB
fetch to disk comes back the right LENGTH with the wrong bytes, and a
checksum only says "wrong". What names the layer is the SHAPE of the
damage, and this prints it:

  - where the differing runs are, and how long each one is
  - whether they are block-ALIGNED (512 / 4096 / 8192), which separates
    a block-layer fault from a byte-level one
  - what the bad bytes ARE: zeros (an unwritten block), a run copied
    from somewhere else in the same file (a cache handing back the
    wrong block, and the offset delta names it), or neither

A run copied from elsewhere in the file is the finding worth the whole
script: it turns "the data is wrong" into "block N was served where
block M belonged", which is a different bug from "the bytes decayed".

    python3 tools/corrupt_diff.py good.bin suspect.bin
"""
import argparse
import sys

BLOCK_SIZES = (512, 4096, 8192, 65536)


def runs_of_difference(a, b):
    """[(start, length)] for every maximal differing run."""
    out = []
    n = min(len(a), len(b))
    i = 0
    while i < n:
        if a[i] == b[i]:
            i += 1
            continue
        start = i
        while i < n and a[i] != b[i]:
            i += 1
        out.append((start, i - start))
    return out


def alignment_of(off, length):
    """The largest block size this run starts AND ends on, or 0."""
    best = 0
    for bs in BLOCK_SIZES:
        if off % bs == 0 and length % bs == 0:
            best = bs
    return best


def find_source(good, chunk, exclude_at):
    """Where else in `good` this exact chunk appears -- which is what a
    misdirected block looks like. Returns an offset or None. Bounded to
    the first hit: a repeating file would otherwise scan forever."""
    if len(chunk) < 64:
        return None                     # too short to be meaningful
    probe = chunk[:64]
    at = good.find(probe)
    while at != -1:
        if at != exclude_at and good[at:at + len(chunk)] == chunk:
            return at
        at = good.find(probe, at + 1)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("good", help="the file as it should be")
    ap.add_argument("suspect", help="the file as it came back")
    ap.add_argument("--max-runs", type=int, default=20,
                    help="how many differing runs to describe (default 20)")
    args = ap.parse_args()

    good = open(args.good, "rb").read()
    bad = open(args.suspect, "rb").read()

    print(f"good    {len(good)} bytes  {args.good}")
    print(f"suspect {len(bad)} bytes  {args.suspect}")
    if len(good) != len(bad):
        print(f"LENGTHS DIFFER by {len(bad) - len(good):+d} -- a truncation, not a corruption")

    runs = runs_of_difference(good, bad)
    if not runs:
        print("identical over the common length")
        return 0

    total = sum(n for _, n in runs)
    print(f"\n{len(runs)} differing run(s), {total} bytes total "
          f"({100.0 * total / min(len(good), len(bad)):.3f}% of the file)")

    for off, length in runs[:args.max_runs]:
        chunk = bad[off:off + length]
        align = alignment_of(off, length)
        note = f"aligned to {align}" if align else "NOT block-aligned"
        print(f"\n  offset {off} (0x{off:x})  length {length}  [{note}]")

        if not any(chunk):
            print("    the bad bytes are ALL ZERO -- an unwritten or lost block")
            continue

        src = find_source(good, chunk, off)
        if src is not None:
            delta = src - off
            print(f"    these bytes are the file's own content from offset {src} "
                  f"(0x{src:x}), delta {delta:+d}")
            print("    -> a block served from the WRONG PLACE, not damaged data")
            continue

        same = sum(1 for i in range(length) if good[off + i] == bad[off + i])
        print(f"    neither zeros nor a copy from elsewhere; {same}/{length} bytes "
              f"happen to match")
        print(f"    good {good[off:off + 16].hex()}")
        print(f"    bad  {bad[off:off + 16].hex()}")

    if len(runs) > args.max_runs:
        print(f"\n  ... {len(runs) - args.max_runs} more run(s) not described")

    starts = [o for o, _ in runs]
    for bs in BLOCK_SIZES:
        if all(o % bs == 0 for o in starts):
            print(f"\nEVERY run starts on a {bs}-byte boundary")
            break
    return 0


if __name__ == "__main__":
    sys.exit(main())
