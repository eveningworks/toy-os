#!/usr/bin/env python3
"""tools/seed_disk.py -- format-aware seeding front-end.

The Makefile's `seed` target calls this instead of hardwiring one
writer tool: it probes the image's magic and delegates to the matching
writer's `sync`, so an EXISTING checkout's TFS2 disk.img keeps working
untouched while a fresh (blank) image gets today's default format,
TFS3 -- the same policy kernel/fs/vfs.c's fs_init() applies to a blank
disk. `make clean-disk && make iso` is therefore how a checkout moves
to TFS3 on purpose; nothing migrates or reformats by surprise.

`--partition gpt|mbr` builds a PARTITIONED image instead: a real table
(tools/mkpart_test.py --layout) with the filesystem inside partition 1
rather than flat at LBA 0. That is what exercises kernel/fs/vfs.c's
partition scan and kernel/drivers/block/block_part.c end to end -- and
it is not the default, because this repo's disk.img is deliberately a
flat volume and every existing test depends on that.

Usage: seed_disk.py <disk.img> <seed-dir> [--partition gpt|mbr] [--layout SPEC]
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def probe(disk):
    """'tfs3', or None (blank/unknown).

    Any TFS3 format version claims the image -- the writer reads both
    v1 (four journal slots) and v2 (32), and picking a version here
    would make a v2 image look BLANK and get reformatted, which is the
    one outcome this probe exists to prevent.

    A TFS2 image is REFUSED by name rather than falling through to
    None. None means "blank, format it", and formatting over somebody's
    TFS2 disk because this build stopped speaking the format is the
    host-side twin of the data-loss bug kernel/fs/vfs.c's
    disk_is_tfs2() exists to prevent.
    """
    with open(disk, "rb") as f:
        lba0 = f.read(5)
        f.seek(8 * 4096)
        blk8 = f.read(5)
    if blk8[:4] == b"TFS3" and 1 <= blk8[4] <= 2:
        return "tfs3"
    if lba0[:4] == b"TFS2" and lba0[4:5] == b"\x03":
        sys.exit(f"seed_disk: {disk} is TFS2, which this build no longer "
                 f"supports.\n  Back it up if you need it, then "
                 f"`make clean-disk && make iso`.")
    return None


def run(tool, *args):
    r = subprocess.run([sys.executable, os.path.join(HERE, tool), *args])
    if r.returncode != 0:
        sys.exit(r.returncode)


ALIGN_LBA = 2048  # mkpart_test.py's, and /bin/mkpart's


def seed_partitioned(disk, seed_dir, kind, layout):
    """Write a table, then put TFS3 inside partition 1 and seed it.

    TFS2 is not an option here and never will be: it addresses the disk
    absolutely and puts its superblock at LBA 0, which is the MBR. See
    kernel/include/kernel/fs_ops.h's `volume_relative`.
    """
    sys.path.insert(0, HERE)
    import mkpart_test

    if os.path.getsize(disk) == 0:
        sys.exit(f"seed_disk: {disk} is empty -- create it at a real size first")

    parts = (mkpart_test.real_gpt(disk, layout) if kind == "gpt"
             else mkpart_test.real_mbr(disk, layout))
    start, sectors = parts[0]
    print(f"seed_disk: wrote a {kind.upper()} table; "
          f"partition 1 is LBA {start}, {sectors} sectors")

    # --force because the sectors inside a fresh partition are whatever
    # the image held there before, which may well be an old filesystem's
    # middle -- there is nothing to preserve and refusing would make a
    # re-partition a two-step dance.
    run("tfs3_writer.py", "format", disk,
        "--at-lba", str(start), "--sectors", str(sectors), "--force")
    run("tfs3_writer.py", "sync", disk, seed_dir,
        "--at-lba", str(start), "--sectors", str(sectors))


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("disk")
    ap.add_argument("seed_dir")
    ap.add_argument("--partition", choices=("gpt", "mbr"),
                    help="build a partitioned image with the filesystem inside "
                         "partition 1, instead of a flat volume at LBA 0")
    ap.add_argument("--layout", default="rest", metavar="SIZE[,SIZE...]",
                    help="partition sizes, e.g. '64M,rest' (default: one "
                         "partition filling the disk)")
    args = ap.parse_args()
    disk, seed_dir = args.disk, args.seed_dir

    if args.partition:
        seed_partitioned(disk, seed_dir, args.partition, args.layout)
        return

    fmt = probe(disk)
    if fmt is None:
        print(f"seed_disk: {disk} is blank -- formatting with the default (tfs3)")
        run("tfs3_writer.py", "format", disk)
        fmt = "tfs3"
    print(f"seed_disk: {disk} is {fmt}")
    run(f"{fmt}_writer.py", "sync", disk, seed_dir)


if __name__ == "__main__":
    main()
