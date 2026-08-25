#!/usr/bin/env python3
"""tools/seed_disk.py -- format-aware seeding front-end.

The Makefile's `seed` target calls this instead of hardwiring one
writer tool: it probes the image's magic and delegates to the matching
writer's `sync`, so an EXISTING checkout's TFS2 disk.img keeps working
untouched while a fresh (blank) image gets today's default format,
TFS3 -- the same policy kernel/fs/vfs.c's fs_init() applies to a blank
disk. `make clean-disk && make iso` is therefore how a checkout moves
to TFS3 on purpose; nothing migrates or reformats by surprise.

**A BLANK IMAGE IS PARTITIONED NOW.** It gets a GPT with the filesystem
inside partition 1, which is what a real OS's disk looks like and what
makes the partition path (kernel/fs/vfs.c's scan,
kernel/drivers/block/block_part.c's window) the one every test
exercises rather than an on-demand tool. `--partition mbr` writes the
legacy table instead; `--flat` writes the old whole-disk volume, which
is still a supported layout and is what the live ISO's RAM image is.

An EXISTING image keeps whatever layout it has -- partitioned or flat,
found by tools/mkpart_test.py's volume_of() -- so a checkout does not
change shape under anybody. `make clean-disk && make iso` is how one
adopts the new default on purpose, the same opt-in that moved TFS2 to
TFS3.

Usage: seed_disk.py <disk.img> <seed-dir> [--partition gpt|mbr|--flat]
                                          [--layout SPEC]
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def probe(disk, base_lba=0):
    """'tfs3', or None (blank/unknown). Reads at the VOLUME's base.

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
    at = base_lba * 512
    with open(disk, "rb") as f:
        f.seek(at)
        lba0 = f.read(5)
        f.seek(at + 8 * 4096)
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
             else mkpart_test.real_mbr(disk, layout))  # noqa: E501
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


DEFAULT_PARTITION = "gpt"   # what a BLANK image gets


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("disk")
    ap.add_argument("seed_dir")
    ap.add_argument("--partition", choices=("gpt", "mbr"),
                    help=f"table kind for a blank image (default: "
                         f"{DEFAULT_PARTITION})")
    ap.add_argument("--flat", action="store_true",
                    help="build a whole-disk volume at LBA 0 instead of "
                         "partitioning -- still supported, and what the live "
                         "ISO's RAM image is")
    ap.add_argument("--layout", default="rest", metavar="SIZE[,SIZE...]",
                    help="partition sizes, e.g. '64M,rest' (default: one "
                         "partition filling the disk)")
    args = ap.parse_args()
    disk, seed_dir = args.disk, args.seed_dir

    sys.path.insert(0, HERE)
    import mkpart_test

    # WHAT SHAPE IS THIS IMAGE ALREADY? Asked before anything is
    # decided, because an existing image's layout always wins -- a
    # checkout must not change shape because the default moved.
    base, sectors = mkpart_test.volume_of(disk)
    partitioned = base != 0
    fmt = probe(disk, base)

    if fmt is None and not partitioned:
        # Genuinely blank. This is the ONLY branch the default applies
        # to, and it is why moving the default is safe: nothing that
        # already holds a filesystem is touched.
        if args.flat:
            print(f"seed_disk: {disk} is blank -- flat tfs3 at LBA 0 (--flat)")
            run("tfs3_writer.py", "format", disk)
            base, sectors = 0, 0
        else:
            kind = args.partition or DEFAULT_PARTITION
            print(f"seed_disk: {disk} is blank -- {kind.upper()} with tfs3 in "
                  f"partition 1 (--flat for the old whole-disk layout)")
            seed_partitioned(disk, seed_dir, kind, args.layout)
            return
    elif fmt is None:
        # A table, but no filesystem in partition 1 -- a `mkpart`ed disk
        # that was never formatted. Format into the partition rather
        # than treating the image as blank and laying a volume over the
        # table, which is the same refusal the kernel makes at boot.
        print(f"seed_disk: {disk} is partitioned but partition 1 is empty -- "
              f"formatting it")
        run("tfs3_writer.py", "format", disk,
            "--at-lba", str(base), "--sectors", str(sectors), "--force")

    where = f"partition 1 (LBA {base}, {sectors} sectors)" if partitioned else "flat at LBA 0"
    print(f"seed_disk: {disk} is tfs3, {where}")
    extra = ["--at-lba", str(base), "--sectors", str(sectors)] if partitioned else []
    run("tfs3_writer.py", "sync", disk, seed_dir, *extra)


if __name__ == "__main__":
    main()
