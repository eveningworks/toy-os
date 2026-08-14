#!/usr/bin/env python3
"""tools/seed_disk.py -- format-aware seeding front-end.

The Makefile's `seed` target calls this instead of hardwiring one
writer tool: it probes the image's magic and delegates to the matching
writer's `sync`, so an EXISTING checkout's TFS2 disk.img keeps working
untouched while a fresh (blank) image gets today's default format,
TFS3 -- the same policy kernel/fs/vfs.c's fs_init() applies to a blank
disk. `make clean-disk && make iso` is therefore how a checkout moves
to TFS3 on purpose; nothing migrates or reformats by surprise.

Usage: seed_disk.py <disk.img> <seed-dir>
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def probe(disk):
    """'tfs2', 'tfs3', or None (blank/unknown)."""
    with open(disk, "rb") as f:
        lba0 = f.read(5)
        f.seek(8 * 4096)
        blk8 = f.read(5)
    if blk8[:4] == b"TFS3" and blk8[4:5] == b"\x01":
        return "tfs3"
    if lba0[:4] == b"TFS2" and lba0[4:5] == b"\x03":
        return "tfs2"
    return None


def run(tool, *args):
    r = subprocess.run([sys.executable, os.path.join(HERE, tool), *args])
    if r.returncode != 0:
        sys.exit(r.returncode)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip())
    disk, seed_dir = sys.argv[1], sys.argv[2]
    fmt = probe(disk)
    if fmt is None:
        print(f"seed_disk: {disk} is blank -- formatting with the default (tfs3)")
        run("tfs3_writer.py", "format", disk)
        fmt = "tfs3"
    print(f"seed_disk: {disk} is {fmt}")
    run(f"{fmt}_writer.py", "sync", disk, seed_dir)


if __name__ == "__main__":
    main()
