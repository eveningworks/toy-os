#!/usr/bin/env python3
"""Verify disk.img's directory layout matches docs/filesystem-layout.md.

The doc is the source of truth; this reads its table and compares it
against a built image, failing on drift in EITHER direction:

  - a directory on the image that no row describes  (someone added a
    directory without documenting it)
  - a row marked `present` with nothing on the image (the doc describes
    something that isn't there any more)

Three statuses: `present` (must exist), `optional` (may or may not --
documented so it isn't flagged as undocumented, but never required), and
`reserved` (a name spoken for by a future milestone, nothing creates it
yet). `optional` exists for /usr/share/kbs, which the build only produces when
xkbcli is installed -- requiring it would fail the check on any machine
without that tool, and dropping the row would let a genuinely
undocumented directory hide behind the same name.

Both matter. This project has repeatedly found docs that quietly stopped
matching reality -- stale milestone numbers, a comment citing a function
that had been deleted, a README claiming a widget existed. A layout
document is exactly the kind of thing that rots that way, so it gets a
check rather than a promise.

Deliberately scoped to DIRECTORIES, not files. Files churn constantly
(every `/bin` binary, every config key); directories are the structural
decision the doc is actually about, and the thing worth a build failure.

The one file-level exception is ORPHANS, reported as a warning at the
end, and it is not a reopening of the churn problem above: it never asks
which files SHOULD exist, only whether a seeded directory holds a binary
that `seed/sync/` no longer places there. That is a specific, silent
failure mode rather than churn -- `sync` is additive and cannot delete,
so a binary that MOVED (say `/tests/calculator` -> `/bin/calculator`)
leaves its old copy behind forever, still runnable, frozen at whatever
build produced it. Two versions of a program under two paths, one of
them permanently stale. It has happened here for real, to all four
ring-3 GUI apps.

It warns rather than fails, on purpose. A dev image legitimately
accumulates state, a freshly built one can never trip this, and failing
the gate over harmless stale bytes is the fastest way to teach everyone
to ignore the tool -- the same reasoning that made `optional` a status.

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
            if status not in ("present", "reserved", "optional"):
                sys.exit(f"check_layout: row for {m.group(1)} has unknown status "
                         f"{cells[-1]!r} (expected 'present', 'optional' or 'reserved')")
            creator = cells[-2].lower()
            rows[m.group(1)] = (status, creator)
    if not rows:
        sys.exit(f"check_layout: no layout table rows found in {path} -- has its "
                 f"format changed? This check reads `| `/path` | ... | status |` rows.")
    return rows


def volume(disk):
    """(base_lba, sectors) for the image's filesystem volume.

    Partition 1 on a partitioned image, the whole image on a flat one.
    Asked rather than assumed, because disk.img is partitioned by
    default now and the live ISO's image is not -- and this check runs
    in the preflight gate, so getting it wrong fails the build for the
    wrong reason.
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import mkpart_test
    return mkpart_test.volume_of(disk)


def image_format(disk, base_lba=0):
    """'tfs3' by magic -- same probe rule as the kernel and seed_disk.py.

    Kept as a lookup returning a NAME rather than collapsed to a
    constant: FAT32 is the next backend (docs/roadmap.md) and will need
    a second row here and a `fat_writer.py` beside the others, exactly
    as TFS3 did.
    """
    at = base_lba * 512
    with open(disk, "rb") as f:
        f.seek(at + 8 * 4096)   # block 8 -- TFS3's superblock
        blk8 = f.read(5)
    if blk8[:4] == b"TFS3":
        return "tfs3"
    sys.exit(f"check_layout: {disk} carries no filesystem magic")


def dirs_on_image(disk, writer_dir):
    """Every directory on the image, walked breadth-first from /.
    Format-aware by writer name -- tfs3_writer prints
    `d <size> ino=N <name>` (names, not paths)."""
    base, sectors = volume(disk)
    fmt = image_format(disk, base)
    writer = os.path.join(writer_dir, f"{fmt}_writer.py")
    vol = ["--at-lba", str(base), "--sectors", str(sectors)] if base else []
    found = set()
    queue = ["/"]
    while queue:
        cur = queue.pop(0)
        r = subprocess.run([sys.executable, writer, "ls", disk, cur, *vol],
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"check_layout: couldn't list {cur} on {disk}:\n{r.stderr.strip()}")
        for line in r.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 4 and parts[0] == "d":
                name = parts[3]
                if name in (".", ".."):
                    continue
                full = (cur.rstrip("/") + "/" + name) if cur != "/" else "/" + name
                found.add(full)
                queue.append(full)
    return found


def orphans_on_image(disk, writer_dir, seed_root):
    """Files under a seeded directory that seed/sync/ no longer places there.

    Returns [(image_path, seed_dir)]. Only directories that seed/sync/
    actually mirrors are examined -- anything the OS itself writes
    (/etc/history, a user's saved file) is none of this check's business
    and is never looked at.
    """
    base, sectors = volume(disk)
    fmt = image_format(disk, base)
    writer = os.path.join(writer_dir, f"{fmt}_writer.py")
    vol = ["--at-lba", str(base), "--sectors", str(sectors)] if base else []
    found = []
    for sub in sorted(os.listdir(seed_root)):
        seed_dir = os.path.join(seed_root, sub)
        if not os.path.isdir(seed_dir):
            continue
        expected = {n for n in os.listdir(seed_dir)
                    if os.path.isfile(os.path.join(seed_dir, n))}
        if not expected:
            continue  # a directory seeded only with subdirectories
        r = subprocess.run([sys.executable, writer, "ls", disk, "/" + sub, *vol],
                           capture_output=True, text=True)
        if r.returncode != 0:
            continue  # not on the image at all -- the directory check owns that
        for line in r.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 4 and parts[0] == "-":
                name = parts[3]
            else:
                continue
            if name not in expected:
                found.append((f"/{sub}/{name}", os.path.relpath(seed_dir, REPO)))
    return found



# --- untracked files staged straight into seed/sync/ -------------------
#
# `seed/sync/` IS A BUILD-STAGING TREE, NOT A SOURCE TREE. It is
# gitignored (`/seed/sync/` in .gitignore) and `make clean` deletes it
# wholesale; everything in it is COPIED there by the Makefile's seed step
# from a tracked source under `data/`, `build/` or the repo root.
#
# So a hand-authored file written straight into it works perfectly on the
# machine that made it, is never committed, and vanishes at the next
# `make clean`. That has now happened twice: the cursor themes shipped
# absent and the desktop silently fell back to its built-in shapes (the
# Makefile's own comment records it), and a new app's .desktop entry went
# missing so the app was on the image with no way to launch it.
#
# Neither failure is loud. This is the check that makes it loud.
SEED_SOURCES = {
    "usr/wm/applications":    "data/wm/applications",
    # The desktop's seeded launchers are copies of application entries,
    # staged under seed/once/ (copied only when missing) -- see unsourced_staged_files().
    "../once/home/desktop": "data/wm/applications",
    "usr/wm/startup":    "data/wm/startup",
    "usr/wm/savers":     "data/wm/savers",
    "usr/share/icons":   "data/icons",
    "usr/share/music":   "data/usr/share/music",
    "usr/share/terminal": "data/usr/share/terminal",
    "usr/share/fonts":   "data/fonts",
    "usr/share/wallpapers": "data/wallpapers",
    "usr/share/cursors": "data/cursors",
    # The one staged tree whose source is NOT under data/: the command
    # pages are the repository's own docs, seeded unconverted.
    "usr/share/doc/cmd": "docs/commands",
    "usr/share/doc/guide": "data/usr/share/doc/guide",
}


def unsourced_staged_files(seed_root):
    """Files staged under seed/sync/ with no tracked source behind them."""
    out = []
    for rel, src in sorted(SEED_SOURCES.items()):
        staged = os.path.join(seed_root, rel)
        source = os.path.join(REPO, src)
        if not os.path.isdir(staged) or not os.path.isdir(source):
            continue
        # Cursor themes are a directory per theme; everything else is flat.
        for root, _dirs, files in os.walk(staged):
            for f in files:
                sub = os.path.relpath(os.path.join(root, f), staged)
                if not os.path.exists(os.path.join(source, sub)):
                    out.append((os.path.join(rel, sub), src))
    return out


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

    documented = parse_doc(args.doc)
    on_disk = dirs_on_image(args.disk, os.path.join(REPO, "tools"))

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

    # A file staged into seed/sync/ with nothing tracked behind it is a
    # FAILURE, not a warning: it is already invisible to git and will be
    # gone at the next `make clean`, so the build that produced it is the
    # only one that will ever have it.
    seed_root = os.path.join(REPO, "seed", "sync")
    if os.path.isdir(seed_root):
        unsourced = unsourced_staged_files(seed_root)
        if unsourced:
            print("check_layout: FAIL -- file(s) staged into seed/sync/ with no "
                  "tracked source.\n")
            for path, src in unsourced:
                print(f"  seed/sync/{path}")
                print(f"      Nothing in {src}/ puts it there. seed/sync/ is "
                      f"gitignored and\n      `make clean` deletes it -- move the "
                      f"file to {src}/ and re-run `make iso`.")
            return 1

    # Warning, not failure -- see the module docstring on why.
    if os.path.isdir(seed_root):
        stale = orphans_on_image(args.disk, os.path.join(REPO, "tools"), seed_root)
        if stale:
            # WITH THE VOLUME'S BASE. Without it this reads LBA 0 of a
            # PARTITIONED disk, finds no magic and exits -- failing the
            # gate for the wrong reason, which is the one thing this
            # module's own docstring says not to do. Only reachable when
            # the image has orphans, which is why it survived.
            fmt = image_format(args.disk, volume(args.disk)[0])
            print(f"check_layout: WARNING -- {len(stale)} orphaned file(s) on the image.")
            print("  `sync` is additive and never deletes, so a binary that MOVED "
                  "leaves\n  its old copy behind forever, frozen at an old build:\n")
            for path, seed_dir in stale:
                print(f"    {path}  (nothing in {seed_dir}/ places it there any more)")
            print("\n  Remove them with:")
            for path, _ in stale:
                print(f"    python3 tools/{fmt}_writer.py delete "
                      f"{os.path.relpath(args.disk, REPO)} {path}")
            print("  ...or `make clean-disk && make iso` for a fresh image "
                  "(wipes saved files).\n")

    if not args.quiet:
        present = sum(1 for st, _ in documented.values() if st == "present")
        optional = sum(1 for st, _ in documented.values() if st == "optional")
        reserved = len(documented) - present - optional
        print(f"check_layout: PASS -- {present} documented director"
              f"{'y' if present == 1 else 'ies'} all present, "
              f"{optional} optional, {reserved} reserved, "
              f"nothing undocumented on the image")
    return 0


if __name__ == "__main__":
    sys.exit(main())
