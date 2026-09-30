#!/usr/bin/env python3
"""tools/sector4k_test.py -- a disk with 4096-byte sectors, end to end in QEMU.

WHAT THIS COVERS
----------------
Every LBA in toy-os's block layer counts 512-byte sectors, whatever the
device (Linux's `sector_t` rule, block.h). A 4K-sector disk is addressed
as eight of them per block; the block layer refuses a transfer that is
not whole blocks, and the partition code, TFS3, FAT32 and swap reach
anything smaller through blkdev_*_partial(). The KTESTs in
kernel/drivers/block/block_4k_test.c prove that over a RAM device; this
proves it over a REAL virtio-blk device QEMU presents with
`logical_block_size=4096`, which answers a misaligned request with IOERR
the way hardware would.

The machine boots its root off disk.img on IDE (`root=ata0p3` baked into
a copy's grub.cfg) and carries a blank 4K-sector virtio disk beside it.
A 4K disk is never the BOOT disk here: GRUB's BIOS path is 512-byte, and
`install` refuses one.

THE CHECKS, and what a broken version would still pass
-----------------------------------------------------
  1. The driver reports the 4096 and `lsblk` shows it. A kernel ignoring
     VIRTIO_BLK_F_BLK_SIZE still enumerates the disk; the NUMBER is the
     assertion.
  2. `mkpart`, `mkfs -t fat32` and `mkfs` (TFS3) succeed on it, and both
     partitions mount.
  3. A multi-block file copied onto each reads back with the same `sum`,
     in the same boot AND after a reboot. Same-boot alone passes a
     filesystem serving its own cache.
  4. HOST ORACLES, which the guest cannot fake: the GPT header is at byte
     4096 (block 1) and says first-usable LBA 6, not 34; the FAT32 BPB
     says 4096 bytes per sector; and mtools, reading the image with no
     toy-os code involved, extracts the FAT32 file byte-identical to the
     original.
  5. The kernel logged no misaligned-transfer refusal and no IOERR --
     the one line that would mean some consumer sent a sub-block request.

    python3 tools/sector4k_test.py
    echo $?
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from qmp_test import guarded_boot_args  # noqa: E402
from multidisk_test import Result, connect, kill  # noqa: E402
from install_grub import add_boot_word  # noqa: E402
from harness import copy_disk  # noqa: E402

DISK_MB = 1024
FAT_MB = 300      # >= 65525 clusters at 4 KiB, so other systems read it as FAT32
PAYLOAD = "/bin/ls"
PAYLOAD_HOST = os.path.join(ROOT, "seed", "sync", "bin", "ls")


def launch(boot_img, disk4k, tmp, tag):
    sock = os.path.join(tmp, f"s4k_{tag}.serial")
    pidfile = os.path.join(tmp, f"s4k_{tag}.pid")
    for f in (sock, pidfile):
        if os.path.exists(f):
            os.remove(f)
    cmd = (f"qemu-system-x86_64 {' '.join(guarded_boot_args(boot_img))}"
           f" -drive file={boot_img},format=raw,if=ide"
           f" -drive if=none,id=v4,file={disk4k},format=raw"
           f" -device virtio-blk-pci,drive=v4,disable-legacy=on,"
           f"logical_block_size=4096,physical_block_size=4096"
           f" -m 512 -display none -no-reboot"
           f" -serial unix:{sock},server,nowait"
           f" -daemonize -pidfile {pidfile}")
    subprocess.run(cmd, shell=True, check=True)
    return sock, pidfile


def sum_of(sh, path):
    """The checksum `sum` prints for `path`, or None."""
    out = sh.run(f"sum {path}")
    for line in out.splitlines():
        parts = line.split()
        if parts and path in line and parts[0].isalnum() and parts[0] != "sum":
            return parts[0]
    return None


def mounted_on(sh):
    """Which of the two mount points are REALLY on the 4K disk's
    partitions. The directories exist on the root either way, so a `cp`
    into one whose mount failed lands on the ROOT and reads back fine --
    which is how this test first passed a TFS3 mount that had failed."""
    out = sh.run("mount")
    lines = out.splitlines()
    fat = any("virtio0p1" in ln and "/m4f" in ln for ln in lines)
    tfs = any("virtio0p2" in ln and "/m4t" in ln for ln in lines)
    return fat, tfs, out


def gpt_header(img):
    with open(img, "rb") as f:
        f.seek(512)
        at512 = f.read(8)
        f.seek(4096)
        hdr = f.read(92)
        f.seek(2 * 4096)
        entries = f.read(2 * 128)
    first_usable = struct.unpack_from("<Q", hdr, 40)[0]
    starts = [struct.unpack_from("<QQ", entries, i * 128 + 32) for i in range(2)]
    return at512, hdr[:8], first_usable, starts


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--disk", default=os.path.join(ROOT, "disk.img"))
    ap.add_argument("--keep", action="store_true", help="keep the scratch directory")
    args = ap.parse_args()

    res = Result()
    tmp = tempfile.mkdtemp(prefix="s4k_")
    boot = os.path.join(tmp, "boot.img")
    disk4k = os.path.join(tmp, "four_k.img")
    copy_disk(args.disk, boot)
    ok, why = add_boot_word(boot, "root=ata0p3")
    if not ok:
        print(f"sector4k_test: {why}")
        return 1
    subprocess.run(["truncate", "-s", f"{DISK_MB}M", disk4k], check=True)

    sums = {}
    # --- boot 1: partition, format, write ---
    sock, pidfile = launch(boot, disk4k, tmp, "a")
    sh = None
    try:
        sh = connect(sock)
        if sh is None:
            res.check("the machine booted", False, "no serial console")
            return 1
        log = sh.run("dmesg")
        res.check("virtio-blk reports 4096-byte blocks",
                  "4096-byte blocks" in log,
                  " | ".join(ln for ln in log.splitlines() if "virtio-blk" in ln)[:300])
        res.check("the root is on the IDE disk, not the 4K one",
                  "mounted at / on ata0p3" in log)
        blk = sh.run("lsblk")
        row = next((ln for ln in blk.splitlines() if ln.startswith("virtio0 ")), "")
        res.check("lsblk shows virtio0 with SECTOR 4096", "4096" in row.split(), blk.strip()[:400])

        out = sh.run(f"mkpart --disk virtio0 {FAT_MB}M rest confirm")
        res.check("mkpart writes a GPT on the 4K disk", "GPT partition table written" in out, out[:300])
        blk = sh.run("lsblk")
        res.check("both partitions are named", "virtio0p1" in blk and "virtio0p2" in blk, blk[:400])

        out = sh.run("mkfs -t fat32 virtio0p1 confirm")
        res.check("mkfs -t fat32 on the 4K partition", "rror" not in out and "refused" not in out, out[:300])
        out = sh.run("mkfs virtio0p2 confirm")
        res.check("mkfs (tfs3) on the 4K partition", "rror" not in out and "refused" not in out, out[:300])

        sh.run("mkdir /m4f")
        sh.run("mkdir /m4t")
        sh.run("mount -t fat32 virtio0p1 /m4f")
        sh.run("mount virtio0p2 /m4t")
        fat, tfs, mounts = mounted_on(sh)
        res.check("both mount, each on its own 4K partition", fat and tfs, mounts[:400])

        sh.run(f"cp {PAYLOAD} /m4f/ls")
        sh.run(f"cp {PAYLOAD} /m4t/ls")
        sums["orig"] = sum_of(sh, PAYLOAD)
        same_f, same_t = sum_of(sh, "/m4f/ls"), sum_of(sh, "/m4t/ls")
        res.check("the payload reads back from FAT32 and TFS3 in the same boot",
                  fat and tfs and sums["orig"] and same_f == sums["orig"] and same_t == sums["orig"],
                  f"orig={sums['orig']} fat32={same_f} tfs3={same_t}")
        sh.run("umount /m4f")
        sh.run("umount /m4t")
        sh.run("sync")
        log = sh.run("dmesg")
        bad = [ln for ln in log.splitlines()
               if "not whole" in ln or "IOERR" in ln or "virtio-blk: request" in ln]
        res.check("no misaligned transfer and no device error was logged", not bad,
                  " | ".join(bad)[:300])
    finally:
        if sh:
            sh.close()
        kill(pidfile)

    # --- boot 2: the same bytes, from the disk rather than any cache ---
    time.sleep(1)
    sock, pidfile = launch(boot, disk4k, tmp, "b")
    sh = None
    try:
        sh = connect(sock)
        if sh is None:
            res.check("the machine booted a second time", False, "no serial console")
            return 1
        sh.run("mkdir /m4f")
        sh.run("mkdir /m4t")
        sh.run("mount -t fat32 virtio0p1 /m4f")
        sh.run("mount virtio0p2 /m4t")
        fat, tfs, _ = mounted_on(sh)
        f2, t2 = sum_of(sh, "/m4f/ls"), sum_of(sh, "/m4t/ls")
        res.check("after a reboot both files still match",
                  fat and tfs and sums.get("orig") and f2 == sums["orig"] and t2 == sums["orig"],
                  f"orig={sums.get('orig')} fat32={f2} tfs3={t2}")
        sh.run("umount /m4f")
        sh.run("umount /m4t")
    finally:
        if sh:
            sh.close()
        kill(pidfile)
    time.sleep(1)

    # --- host oracles ---
    at512, sig, first_usable, starts = gpt_header(disk4k)
    res.check("the GPT header is at byte 4096, not 512",
              sig == b"EFI PART" and at512 != b"EFI PART", f"@512={at512!r} @4096={sig!r}")
    res.check("first usable LBA is 6 (4K geometry), not 34", first_usable == 6, str(first_usable))
    fat_start = starts[0][0] * 4096
    with open(disk4k, "rb") as f:
        f.seek(fat_start)
        bpb = f.read(64)
    bps = struct.unpack_from("<H", bpb, 11)[0]
    res.check("the FAT32 BPB says 4096 bytes per sector", bps == 4096, str(bps))
    extracted = os.path.join(tmp, "ls.from_mtools")
    r = subprocess.run(["mcopy", "-n", "-i", f"{disk4k}@@{fat_start}", "::/LS", extracted],
                       capture_output=True, text=True)
    same = (r.returncode == 0 and os.path.exists(extracted)
            and open(extracted, "rb").read() == open(PAYLOAD_HOST, "rb").read())
    res.check("mtools reads the FAT32 file back byte-identical", same,
              (r.stderr or r.stdout)[:300])

    if not args.keep:
        subprocess.run(["rm", "-rf", tmp])
    print(f"sector4k_test: {len(res.passes)} passed, {len(res.fails)} failed")
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
