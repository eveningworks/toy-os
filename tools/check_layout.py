#!/usr/bin/env python3
"""Verify disk.img's directory layout matches docs/filesystem-layout.md.

The doc is the source of truth; this reads its table and compares it
against a built image, failing on drift in EITHER direction:

  - a directory on the image that no row describes  (someone added a
    directory without documenting it)
  - a row marked `present` with nothing on the image (the doc describes
    something that isn't there any more)

Both matter. This project has repeatedly found docs that quietly stopped
matching reality -- stale milestone numbers, a comment citing a function
that had been deleted, a README claiming a widget existed. A layout
document is exactly the kind of thing that rots that way, so it gets a
check rather than a promise.

Deliberately scoped to DIRECTORIES, not files. Files churn constantly
(every `/bin` binary, every config key); directories are the structural
decision the doc is actually about, and the thing worth a build failure.

Usage:
    python3 tools/check_layout.py [--disk disk.img] [--doc docs/filesystem-layout.md]

Exits 0 on match, 1 on drift, 2 if it couldn't run (no image, no doc).
"""

import argparse
import re
import subprocess
import sys
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def parse_doc(path):
    """Pull {dirpath: (status, creator)} out of the doc's markdown table.

    Rows look like:  | `/bin` | Real user programs | build | present |

    `creator` matters because this check runs against a FRESHLY BUILT
    image that has never been booted. A directory the build seeds must
    be there; one `kernel_main()` creates on first boot legitimately
    isn't yet, and demanding it would make the check fail for a correct
    tree -- the fastest way to teach everyone to ignore it.
    """
    rows = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith("|"):
                continue
            cells = [c.strip() for c in line.strip("|").split("|")]
            if len(cells) < 4:
                continue
            m = re.match(r"^`(/[^`]*)`$", cells[0])
            if not m:
                continue  # header row, separator row, or prose table
            status = cells[-1].lower()
            if status not in ("present", "reserved"):
                sys.exit(f"check_layout: row for {m.group(1)} has unknown status "
                         f"{cells[-1]!r} (expected 'present' or 'reserved')")
            creator = cells[-2].lower()
            rows[m.group(1)] = (status, creator)
    if not rows:
        sys.exit(f"check_layout: no layout table rows found in {path} -- has its "
                 f"format changed? This check reads `| `/path` | ... | status |` rows.")
    return rows


def dirs_on_image(disk, writer):
    """Every directory on the image, walked breadth-first from /."""
    found = set()
    queue = ["/"]
    while queue:
        cur = queue.pop(0)
        r = subprocess.run([sys.executable, writer, "ls", disk, cur],
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"check_layout: couldn't list {cur} on {disk}:\n{r.stderr.strip()}")
        for line in r.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 2 and parts[0] == "DIR":
                found.add(parts[1])
                queue.append(parts[1])
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--disk", default=os.path.join(REPO, "disk.img"))
    ap.add_argument("--doc", default=os.path.join(REPO, "docs/filesystem-layout.md"))
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.disk):
        print(f"check_layout: {args.disk} not found -- run `make iso` first", file=sys.stderr)
        return 2
    if not os.path.exists(args.doc):
        print(f"check_layout: {args.doc} not found", file=sys.stderr)
        return 2

    writer = os.path.join(REPO, "tools/tfs2_writer.py")
    documented = parse_doc(args.doc)
    on_disk = dirs_on_image(args.disk, writer)

    problems = []

    for d in sorted(on_disk):
        if d not in documented:
            problems.append(
                f"  {d} exists on the image but no row describes it.\n"
                f"      Add it to {os.path.relpath(args.doc, REPO)}'s table, or don't create it.")

    for d, (status, creator) in sorted(documented.items()):
        if status != "present" or d in on_disk:
            continue
        if "build" not in creator:
            continue  # boot-created: not expected on an unbooted image
        problems.append(
            f"  {d} is documented as `present` and built by `{creator}`,\n"
            f"      but isn't on the image. Either the seed step stopped creating\n"
            f"      it, or its row should say `reserved`.")

    if problems:
        print("check_layout: FAIL -- disk.img and docs/filesystem-layout.md disagree\n")
        print("\n".join(problems))
        print("\nThe doc is the source of truth: decide what the layout SHOULD be,")
        print("write that down, then make the build match it.")
        return 1

    if not args.quiet:
        present = sum(1 for st, _ in documented.values() if st == "present")
        reserved = len(documented) - present
        print(f"check_layout: PASS -- {present} documented director"
              f"{'y' if present == 1 else 'ies'} all present, "
              f"{reserved} reserved, nothing undocumented on the image")
    return 0


if __name__ == "__main__":
    sys.exit(main())
