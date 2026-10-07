#!/usr/bin/env python3
"""tools/bigdisk_test.py -- a disk past 2 TiB, through each 64-bit driver.

A sparse 2200 GiB image is attached as a SECOND disk on virtio-blk, AHCI
and NVMe in turn, beside the ordinary boot disk (pinned with
`root=ata0p3`, since a blank disk on a faster controller outranks IDE for
the root). The guest partitions it with a small partition 2 lying wholly
PAST sector 2^32, formats it TFS3, mounts it, writes a marker file,
checks it, and unmounts. Then the HOST reads the image itself.

WHAT A BROKEN VERSION WOULD STILL PASS, and the check that stops it:
  * a driver still clamping to 2 TiB would partition and format happily
    -- inside the clamp. So the kernel must REPORT the full sector count,
    and partition 2 must start past 2^32 in the GPT the host reads back.
  * a 32-bit LBA anywhere on the path WRAPS: the marker would land
    2^32 sectors early and read back fine through the same wrap. So the
    host looks for the marker's bytes INSIDE partition 2's range of the
    image file, and at the wrapped offset too, where they must NOT be.
  * GPT's backup header lives at the disk's LAST sector; computed in 32
    bits it lands somewhere else. The host checks it is at the true end.

Formatting the whole disk is deliberately avoided: a multi-terabyte TFS3
writes gigabytes of inode tables, which TCG would take hours over.

On demand, not in the gate: it boots three guests with extra hardware.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from harness import copy_disk  # noqa: E402
import install_grub  # noqa: E402

DISK_BYTES = 2200 * 1024 ** 3        # sparse on the host
SECTOR = 512
TWO_TIB_SECTORS = 1 << 32
MARKER = "bigdisk-marker-past-two-tebibytes"
NAMES = {"virtio": "virtio0", "ahci": "ahci0", "nvme": "nvme0"}


class Result:
    def __init__(self):
        self.passes, self.fails = [], []

    def check(self, name, ok, detail=""):
        print(f"  {'ok   ' if ok else 'FAIL '} {name}" + (f"  -- {detail}" if detail else ""))
        (self.passes if ok else self.fails).append(name)
        return ok

    def report(self):
        print(f"\nbigdisk_test: {len(self.passes)} passed, {len(self.fails)} failed")
        for f in self.fails:
            print(f"  FAILED: {f}")
        return 1 if self.fails else 0


def gpt(img, lba):
    """The GPT header at `lba` and its entry array, or None."""
    with open(img, "rb") as f:
        f.seek(lba * SECTOR)
        h = f.read(SECTOR)
        if h[:8] != b"EFI PART":
            return None
        my_lba, alt_lba = struct.unpack_from("<QQ", h, 24)
        entries_lba, = struct.unpack_from("<Q", h, 72)
        count, size = struct.unpack_from("<II", h, 80)
        f.seek(entries_lba * SECTOR)
        raw = f.read(count * size)
    parts = []
    for i in range(count):
        e = raw[i * size:(i + 1) * size]
        if e[:16] == bytes(16):
            continue
        first, last = struct.unpack_from("<QQ", e, 32)
        parts.append((first, last))
    return {"my": my_lba, "alt": alt_lba, "parts": parts}


def extents(img, start_sector, sectors):
    """The image's ALLOCATED byte ranges inside [start, start + sectors),
    found with SEEK_DATA/SEEK_HOLE so a sparse 2 TiB file is not read."""
    lo, hi = start_sector * SECTOR, (start_sector + sectors) * SECTOR
    out = []
    fd = os.open(img, os.O_RDONLY)
    try:
        off = lo
        while off < hi:
            try:
                d = os.lseek(fd, off, os.SEEK_DATA)
            except OSError:
                break
            if d >= hi:
                break
            h = min(os.lseek(fd, d, os.SEEK_HOLE), hi)
            out.append((d, h))
            off = h
    finally:
        os.close(fd)
    return out


def contains(img, start_sector, sectors, needle):
    """Whether `needle` occurs in the image's [start, start + sectors)."""
    want = needle.encode()
    with open(img, "rb") as f:
        for d, h in extents(img, start_sector, sectors):
            f.seek(d)
            if want in f.read(h - d):
                return True
    return False


def run_kind(kind, args, tmp, res):
    name = NAMES[kind]
    print(f"bigdisk_test: {kind} -- {name}")
    root = os.path.join(tmp, f"root-{kind}.img")
    big = os.path.join(tmp, f"big-{kind}.img")
    copy_disk("disk.img", root, cwd=REPO)
    ok, why = install_grub.add_boot_word(root, "root=ata0p3")
    if not res.check(f"[{kind}] the boot disk is pinned as the root", ok, why):
        return
    with open(big, "wb") as f:
        f.truncate(DISK_BYTES)

    def vm(*argv, timeout=300):
        r = subprocess.run([sys.executable, os.path.join(HERE, "vm.py"),
                            "--instance", str(args.instance), "--disk", root,
                            "--extra-disk", big, "--extra-disk-kind", kind, *argv],
                           cwd=REPO, capture_output=True, text=True, check=False,
                           timeout=timeout)
        return r.stdout + r.stderr

    vm("stop")
    if not res.check(f"[{kind}] the guest came up", "ready" in vm("start")):
        return
    try:
        total = DISK_BYTES // SECTOR
        log = vm("exec", "dmesg")
        res.check(f"[{kind}] the kernel reports all {total} sectors",
                  f"{name} active ({total} sectors)" in log or f"{total} sectors" in log,
                  next((ln for ln in log.splitlines() if "sectors" in ln and name[:4] in ln), "no line"))
        out = vm("exec", f"mkpart --disk {name} 2100G 64M confirm",
                 f"mkfs -t tfs3 {name}p2 confirm",
                 "mkdir /mnt", f"mount {name}p2 /mnt",
                 f"write /mnt/marker.txt {MARKER}", "sync",
                 "cat /mnt/marker.txt", "fsck /mnt", "umount /mnt", timeout=600)
        # The `cat` section ALONE: the command echo of the `write` line
        # carries the marker too, and counted with it a failed mount passed.
        cat = out.split("--- cat /mnt/marker.txt ---", 1)[-1].split("\n--- ", 1)[0]
        ok = cat.strip() == MARKER
        res.check(f"[{kind}] the marker reads back through the mount", ok,
                  "" if ok else out.strip()[-300:])
        ok = "fsck: clean." in out
        res.check(f"[{kind}] fsck calls partition 2 clean", ok, "" if ok else out.strip()[-200:])
    finally:
        vm("stop")

    hdr = gpt(big, 1)
    if not res.check(f"[{kind}] the host reads a GPT", hdr is not None):
        return
    last = DISK_BYTES // SECTOR - 1
    res.check(f"[{kind}] the primary header points at the TRUE last sector",
              hdr["alt"] == last, f"alternate {hdr['alt']}, disk ends at {last}")
    backup = gpt(big, last)
    res.check(f"[{kind}] the backup header is there", backup is not None and backup["my"] == last)
    if len(hdr["parts"]) < 2:
        res.check(f"[{kind}] two partitions in the table", False, str(hdr["parts"]))
        return
    first, end = hdr["parts"][1]
    res.check(f"[{kind}] partition 2 starts past sector 2^32", first > TWO_TIB_SECTORS,
              f"LBA {first}")
    sectors = end - first + 1
    res.check(f"[{kind}] the marker's bytes are INSIDE partition 2 on the host",
              contains(big, first, sectors, MARKER))
    # NOTHING AT ALL where a 32-bit wrap would land -- that range is the
    # middle of partition 1, which is never formatted. Checking for the
    # marker alone passed under a real wrap: the metadata wrapped, the
    # mount went wrong, and the file data reached the disk nowhere.
    wrapped = extents(big, first - TWO_TIB_SECTORS, sectors)
    res.check(f"[{kind}] ...and nothing was written where a 32-bit wrap would land",
              not wrapped, f"{len(wrapped)} extent(s) at sector {wrapped[0][0] // SECTOR}" if wrapped else "")
    if not args.keep:
        os.unlink(big)
        os.unlink(root)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--instance", type=int, default=0)
    ap.add_argument("--kind", choices=("virtio", "ahci", "nvme", "all"), default="all")
    ap.add_argument("--keep", action="store_true", help="leave the images behind")
    args = ap.parse_args()
    res = Result()
    tmp = tempfile.mkdtemp(prefix="bigdisk_test_")
    for kind in (("virtio", "ahci", "nvme") if args.kind == "all" else (args.kind,)):
        run_kind(kind, args, tmp, res)
    return res.report()


if __name__ == "__main__":
    sys.exit(main())
