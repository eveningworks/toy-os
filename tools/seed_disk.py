#!/usr/bin/env python3
"""tools/seed_disk.py -- format-aware seeding front-end.

The Makefile's `seed` target calls this instead of hardwiring one
writer tool: it probes the image's magic and delegates to the matching
writer's `sync`, so an EXISTING checkout's disk.img keeps working
untouched while a fresh (blank) image gets today's default format,
TFS3 -- the same policy kernel/fs/vfs.c's fs_init() applies to a blank
disk. `make clean-disk && make iso` is therefore how a checkout moves
to TFS3 on purpose; nothing migrates or reformats by surprise.

**A BLANK IMAGE IS PARTITIONED, AND BOOTABLE.** It gets a GPT holding a
BIOS boot partition, a FAT32 /boot and the filesystem -- what a real
OS's disk looks like, and what lets GRUB boot the kernel off the disk
instead of off the ISO (see DEFAULT_LAYOUT below, and
tools/install_grub.py for why /boot cannot be TFS3). It also makes the
partition path (kernel/fs/vfs.c's scan, block_part.c's window) the one
every test exercises rather than an on-demand tool. `--partition mbr`
writes the legacy table instead. **There is no `--flat` any more**: the
kernel takes a root from a partition and refuses a whole-disk volume
(kernel/fs/vfs.c), so this tool no longer has a way to write one. An
image that already IS flat is still seeded -- a host tool may
legitimately want files in one -- with a loud warning that the kernel
will not mount it.

An EXISTING image keeps whatever layout it has -- partitioned or flat,
found by tools/mkpart_test.py's volume_of() -- so a checkout does not
change shape under anybody. `make clean-disk && make iso` is how one
adopts the new default on purpose.

Usage: seed_disk.py <disk.img> <seed-dir> [--partition gpt|mbr]
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

    """
    at = base_lba * 512
    with open(disk, "rb") as f:
        f.seek(at + 8 * 4096)   # block 8 -- TFS3's superblock
        blk8 = f.read(5)
    if blk8[:4] == b"TFS3" and 1 <= blk8[4] <= 2:
        return "tfs3"
    return None


def run(tool, *args):
    r = subprocess.run([sys.executable, os.path.join(HERE, tool), *args])
    if r.returncode != 0:
        sys.exit(r.returncode)


def seed_partitioned(disk, seed_dir, kind, layout):
    """Write a table, then put TFS3 in the FILESYSTEM partition and seed it.

    TFS3 is not a parameter: a backend can only live in a partition if
    it is volume-relative (kernel/include/kernel/fs_ops.h), and it is
    the only one there is. FAT32 makes this a choice.

    NOT partition 1 -- the default layout puts a BIOS boot partition and
    a FAT32 /boot in front of it so the disk can be booted from
    (tools/install_grub.py). mkpart_test.volume_of() is what finds the
    right one, and every host tool asks it rather than counting.
    """
    sys.path.insert(0, HERE)
    import mkpart_test

    if os.path.getsize(disk) == 0:
        sys.exit(f"seed_disk: {disk} is empty -- create it at a real size first")

    parts = (mkpart_test.real_gpt(disk, layout) if kind == "gpt"
             else mkpart_test.real_mbr(disk, layout))  # noqa: E501
    print(f"seed_disk: wrote a {kind.upper()} table -- "
          + ", ".join(f"p{i + 1} {n // 2048} MiB" for i, (_lba, n) in enumerate(parts)))
    start, sectors = mkpart_test.volume_of(disk)
    index = next(i + 1 for i, (lba, _n) in enumerate(parts) if lba == start)
    # Named, and machine-readable, because it is no longer "partition 1"
    # -- tools/partition_test.py reads this line to know which partition
    # to make its assertions about.
    print(f"seed_disk: filesystem in partition {index} -- "
          f"LBA {start}, {sectors} sectors")

    # --force because the sectors inside a fresh partition are whatever
    # the image held there before, which may well be an old filesystem's
    # middle -- there is nothing to preserve and refusing would make a
    # re-partition a two-step dance.
    run("tfs3_writer.py", "format", disk,
        "--at-lba", str(start), "--sectors", str(sectors), "--force")
    run("tfs3_writer.py", "sync", disk, seed_dir,
        "--at-lba", str(start), "--sectors", str(sectors))


DEFAULT_PARTITION = "gpt"   # what a BLANK image gets

# THE BOOTABLE LAYOUT, and the reason a blank image gets three
# partitions rather than one: toy-os boots from its own disk, and GRUB
# cannot read TFS3. So the kernel lives in a filesystem GRUB already
# understands, in front of the one the OS uses.
#
#   p1  1 MiB   BIOS boot   GRUB's core.img, embedded, no filesystem
#   p2  64 MiB  ESP/FAT32   /boot/kernel.bin + /boot/grub
#   p3  rest    data        TFS3, the OS's own filesystem
#
# tools/install_grub.py writes p1 and p2; this tool writes p3 and never
# touches the other two. See that file for why /boot is FAT.
DEFAULT_LAYOUT = "1M:bios,64M:esp,rest"

# ...and the MBR's, which needs no BIOS boot partition: an MBR leaves a
# gap between the boot sector and the first partition, and that is where
# core.img has always been embedded. GPT has no gap, which is the entire
# reason its own partition type exists.
DEFAULT_LAYOUT_MBR = "64M:esp,rest"


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("disk")
    ap.add_argument("seed_dir")
    ap.add_argument("--partition", choices=("gpt", "mbr"),
                    help=f"table kind for a blank image (default: "
                         f"{DEFAULT_PARTITION})")
    ap.add_argument("--layout", default=None, metavar="SIZE[:KIND][,...]",
                    help=f"partition sizes, e.g. '64M,rest'; a :KIND suffix "
                         f"names the type (bios, esp, data). Default: "
                         f"'{DEFAULT_LAYOUT}' for a GPT, "
                         f"'{DEFAULT_LAYOUT_MBR}' for an MBR")
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
        # Genuinely blank, and it gets a TABLE -- there is no longer a
        # flat option to choose. The kernel mounts a root from a
        # partition and refuses a whole-disk volume (kernel/fs/vfs.c),
        # so writing one here would produce an image that seeds
        # perfectly and then will not mount.
        kind = args.partition or DEFAULT_PARTITION
        layout = args.layout
        if layout is None:
            layout = DEFAULT_LAYOUT if kind == "gpt" else DEFAULT_LAYOUT_MBR
        print(f"seed_disk: {disk} is blank -- {kind.upper()}, "
              f"layout '{layout}'")
        seed_partitioned(disk, seed_dir, kind, layout)
        return
    elif fmt is None:
        # A table, but no filesystem in it -- a `mkpart`ed disk that was
        # never formatted. Format into the partition rather than
        # treating the image as blank and laying a volume over the
        # table, which is the same refusal the kernel makes at boot.
        print(f"seed_disk: {disk} is partitioned but has no filesystem -- "
              f"formatting partition at LBA {base}")
        run("tfs3_writer.py", "format", disk,
            "--at-lba", str(base), "--sectors", str(sectors), "--force")

    where = f"a partition (LBA {base}, {sectors} sectors)" if partitioned else "flat at LBA 0"
    print(f"seed_disk: {disk} is tfs3, {where}")
    extra = ["--at-lba", str(base), "--sectors", str(sectors)] if partitioned else []
    run("tfs3_writer.py", "sync", disk, seed_dir, *extra)

    # A FLAT IMAGE IS SEEDED, AND WILL NOT MOUNT. Seeding it anyway is
    # deliberate: this tool's job is to put files where an image keeps
    # them, and refusing here would break staging a fixture for a host
    # tool that legitimately reads one. What it must not do is stay
    # quiet, because everything downstream succeeds and the failure
    # appears three minutes later as a kernel that boots to ramfs.
    if not partitioned:
        print(f"seed_disk: WARNING -- {disk} is a WHOLE-DISK volume, and the "
              f"kernel will refuse to mount it: a root comes from a partition "
              f"now (docs/rootfs-design.md). `make clean-disk && make iso` "
              f"rebuilds it partitioned, DESTROYING what is on it.")

    # An image built before the boot partition existed still WORKS -- it
    # just cannot be booted from, so every launcher here falls back to
    # the ISO for it (tools/install_grub.py's is_bootable()). Say so
    # once, rather than leaving the difference to be noticed as a
    # mysteriously slower boot.
    import install_grub
    if not install_grub.is_bootable(disk):
        print(f"seed_disk: {disk} has no boot partition -- booting from the ISO. "
              f"`make clean-disk && make iso` rebuilds it bootable (it DESTROYS "
              f"what is on the image, which is why nothing does it for you).")


if __name__ == "__main__":
    main()
