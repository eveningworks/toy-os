#!/usr/bin/env python3
"""Where is this tree actually duplicating code? -- measure, don't guess.

WHY IT EXISTS. "Is there code we should be sharing?" is a question that
gets answered by instinct, and instinct is wrong in both directions: it
proposes extractions for code that merely looks similar, and it misses
the ones that matter because they are spread over files nobody reads
together. This answers it in one command, and the first answer it gave
was **no** -- 71 duplicated lines across 27 GUI files, 75 across 88
`/bin` programs -- which is a result worth being able to reproduce
cheaply after any large refactor.

WHAT IT MEASURES, AND WHAT IT CANNOT. It normalises away comments,
blank lines, brace-only lines and whitespace, hashes a sliding window of
`--window` lines, and reports windows that occur in at least two
DIFFERENT files, each extended to its maximal run. So it finds
copy-and-paste. It does NOT find the thing that usually matters more --
the same idea written twice in different words, or one API read two
different ways by its callers. The menubar sentinel that
`docs/conventions/gui.md` now records was found by counting callers of
one function, not by this; treat a clean run as "no copy-paste", never
as "no duplication".

THE BAR FOR ACTING ON A HIT is CLAUDE.md's: a second REAL caller, not a
plausible one -- and `userland/lib/uline.h`'s rule on top of it, share
the part that does not differ rather than a loop whose differences
become callbacks. Two hits were declined on exactly that in the run
that produced this tool.

    python3 tools/dup_scan.py                     # userland/ by default
    python3 tools/dup_scan.py userland/gui kernel
    python3 tools/dup_scan.py --window 10 --top 5
"""
import argparse
import hashlib
import os
import re
import sys
from collections import defaultdict


def normalise(path):
    """(normalised line, original 1-based line number) for the real code.

    Comments and braces go because a run of them is not duplication in
    any sense a reader cares about -- and a licence header repeated
    across thirty files would otherwise be the biggest 'hit' in the
    tree, drowning everything real.
    """
    src = open(path, encoding="utf-8", errors="replace").read()
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    out = []
    for i, line in enumerate(src.splitlines(), 1):
        stripped = re.sub(r"//.*$", "", line).strip()
        stripped = re.sub(r"\s+", " ", stripped)
        if not stripped or stripped in ("{", "}", "};"):
            continue
        out.append((stripped, i))
    return out


def collect(roots, exts):
    files = {}
    for root in roots:
        if os.path.isfile(root):
            files[root] = normalise(root)
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            # Vendored code is somebody else's duplication, not ours.
            dirnames[:] = [d for d in dirnames if d not in ("ports", "build")]
            for fn in filenames:
                if fn.endswith(exts):
                    p = os.path.join(dirpath, fn)
                    files[p] = normalise(p)
    return files


def scan(files, window):
    index = defaultdict(list)
    for path, lines in files.items():
        for i in range(len(lines) - window + 1):
            key = hashlib.md5("\n".join(l for l, _ in lines[i:i + window]).encode()).hexdigest()
            index[key].append((path, i))

    # Claimed positions, so one 40-line clone is reported once rather
    # than as 35 overlapping windows.
    claimed = set()
    hits = []
    for locs in index.values():
        paths = {p for p, _ in locs}
        if len(paths) < 2:
            continue
        first = locs[0]
        second = next(l for l in locs if l[0] != first[0])
        if first in claimed or second in claimed:
            continue
        (pa, ia), (pb, ib) = first, second
        n = window
        while (ia + n < len(files[pa]) and ib + n < len(files[pb])
               and files[pa][ia + n][0] == files[pb][ib + n][0]):
            n += 1
        for k in range(n):
            claimed.add((pa, ia + k))
            claimed.add((pb, ib + k))
        hits.append((n, pa, files[pa][ia][1], pb, files[pb][ib][1], len(paths)))
    hits.sort(reverse=True)
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__)
    ap.add_argument("roots", nargs="*", default=["userland"],
                    help="directories or files to scan (default: userland)")
    ap.add_argument("--window", type=int, default=6,
                    help="shortest run counted as duplication (default 6)")
    ap.add_argument("--top", type=int, default=20, help="how many to print")
    ap.add_argument("--ext", default=".c,.h", help="comma-separated suffixes")
    args = ap.parse_args()

    exts = tuple(e if e.startswith(".") else "." + e for e in args.ext.split(","))
    files = collect(args.roots, exts)
    if not files:
        print("dup_scan: nothing to scan")
        return 1

    hits = scan(files, args.window)
    total = sum(h[0] for h in hits)
    print(f"dup_scan: {len(files)} file(s), {len(hits)} duplicated block(s) "
          f">= {args.window} lines, {total} duplicated line(s)\n")
    for n, pa, la, pb, lb, nfiles in hits[:args.top]:
        extra = f"   (+{nfiles - 2} more file(s))" if nfiles > 2 else ""
        print(f"  {n:3d} lines  {pa}:{la}  <->  {pb}:{lb}{extra}")
    if len(hits) > args.top:
        print(f"\n  ... {len(hits) - args.top} more; raise --top to see them")
    # Always 0: this REPORTS, it does not gate. Duplication is a
    # judgement call (see the module docstring's bar), and a checker that
    # failed a build on it would be wrong most of the time.
    return 0


if __name__ == "__main__":
    sys.exit(main())
