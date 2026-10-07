#!/usr/bin/env python3
"""tools/rootdisk_test.py -- every SATA drive, and which disk is the root.

WHAT THIS COVERS
----------------
Two things that used to go wrong only on a machine with more than one
disk, which no other test here boots:

  * AHCI drove ONE drive: the first SATA disk on the first HBA. A second
    disk on the same controller, or anything on a second controller, did
    not exist -- no device, no name, nothing to mount.
  * The root disk was whichever driver PROBED LAST (blk_register() is
    last-writer-wins), so a blank disk on a faster controller took the
    root and the machine booted into ramfs beside its real system disk.
    Now grub.cfg passes `bootpart=<PARTUUID>` and the kernel roots on
    the disk carrying it; without it, the first disk with a boot
    partition wins (kernel/fs/mount.c, choose_root_disk()).

THE BOOTS, and what a broken version would still pass
-----------------------------------------------------
  A. SATA: the system disk on HBA 0 port 1 (SeaBIOS boots it by
     bootindex), a data disk with a host-written marker on HBA 0 port 0,
     and a blank disk on a SECOND HBA. Three drives must exist, the root
     must come from port 1 -- NOT the first drive -- and the port-0 disk
     must mount, read the marker AND keep a file written to it across a
     remount, which is the half a driver sharing one bounce buffer or
     one interrupt between drives cannot pass. The AHCI KTESTs run with
     every drive present, and must not skip.
  B. A BLANK virtio disk beside an IDE system disk -- virtio outranks
     IDE. With `bootpart=` the boot disk wins by name; with it REMOVED
     from grub.cfg the content rule must still pick IDE. The old kernel
     mounted ramfs here.
  C. TWO system disks, the second on virtio with its partition GUIDs
     changed. bootpart= must pick the one GRUB booted (IDE); removing it
     must flip the root to virtio -- which is what proves bootpart= is
     the thing deciding, rather than the content rule agreeing with it.

    python3 tools/rootdisk_test.py
"""

import argparse
import os
import struct
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import install_grub  # noqa: E402
from harness import Results  # noqa: E402
from qmp_test import guarded_boot_args  # noqa: E402
from multidisk_test import (connect, kill, build_second_disk,  # noqa: E402
                            MARK_PATH, MARK_TEXT)

BLANK_MB = 256
MOUNT_WAIT_S = 60.0


def sh(cmd):
    subprocess.run(cmd, shell=True, check=True)


def copy_disk(tmp, name):
    img = os.path.join(tmp, name)
    sh(f"cp --reflink=auto --sparse=always {ROOT}/disk.img {img}")
    return img


def blank_disk(tmp, name):
    img = os.path.join(tmp, name)
    if os.path.exists(img):
        os.remove(img)
    sh(f"truncate -s {BLANK_MB}M {img}")
    return img


def reguid(img):
    """Give every partition of a GPT image a different unique GUID, in
    both headers, CRCs fixed -- a copy of disk.img otherwise carries the
    same PARTUUIDs as the original, and bootpart= could not tell them
    apart (two cloned disks cannot be told apart on Linux either)."""
    with open(img, "r+b") as f:
        f.seek(512)
        primary = bytearray(f.read(512))
        alt_lba = struct.unpack_from("<Q", primary, 32)[0]
        for lba in (1, alt_lba):
            f.seek(lba * 512)
            hdr = bytearray(f.read(512))
            assert hdr[:8] == b"EFI PART", f"no GPT header at LBA {lba}"
            hsize, = struct.unpack_from("<I", hdr, 12)
            ent_lba, = struct.unpack_from("<Q", hdr, 72)
            n, esize = struct.unpack_from("<II", hdr, 80)
            f.seek(ent_lba * 512)
            ents = bytearray(f.read(n * esize))
            for i in range(n):
                e = i * esize
                if any(ents[e:e + 16]):            # a used entry
                    ents[e + 16] ^= 0x5A           # its unique GUID's first byte
            struct.pack_into("<I", hdr, 88, zlib.crc32(bytes(ents)) & 0xFFFFFFFF)
            struct.pack_into("<I", hdr, 16, 0)
            struct.pack_into("<I", hdr, 16, zlib.crc32(bytes(hdr[:hsize])) & 0xFFFFFFFF)
            f.seek(ent_lba * 512)
            f.write(ents)
            f.seek(lba * 512)
            f.write(hdr)


def without_bootpart(img):
    ok, why = install_grub.drop_boot_word(img, "bootpart=")
    if not ok:
        sys.exit(f"rootdisk_test: {why}")


def boot_to_root(boot_img, devices, tmp, tag):
    """Boot, wait for the root to mount (or for ramfs), return the log."""
    log = os.path.abspath(os.path.join(tmp, f"rootdisk_{tag}.log"))
    pidfile = os.path.abspath(os.path.join(tmp, f"rootdisk_{tag}.pid"))
    for p in (log, pidfile):
        if os.path.exists(p):
            os.remove(p)
    cmd = (f"qemu-system-x86_64 {' '.join(guarded_boot_args(boot_img, check_disk=False))}"
           f" {devices} -m 512 -display none -no-reboot -serial file:{log}"
           f" -daemonize -pidfile {pidfile}")
    sh(cmd)
    text, deadline = "", time.time() + MOUNT_WAIT_S
    try:
        while time.time() < deadline:
            time.sleep(0.5)
            with open(log, "rb") as f:
                text = f.read().decode("utf-8", "replace")
            if "mounted at / on" in text or "mounted at / on (no device)" in text:
                break
    finally:
        kill(pidfile)
    return text


def fs_lines(log):
    return " | ".join(ln.strip() for ln in log.splitlines()
                      if "bootpart" in ln or "root disk" in ln or "mounted at /" in ln)[:400]


def ide_boot(img):
    return (f"-drive if=none,id=d0,file={img},format=raw"
            f" -device ide-hd,drive=d0,bus=ide.0,bootindex=0")


def virtio(img):
    return (f"-drive if=none,id=v0,file={img},format=raw"
            f" -device virtio-blk-pci,drive=v0,disable-legacy=on")


def phase_sata(res, tmp):
    print("rootdisk_test: A -- three SATA drives on two HBAs, the root on port 1")
    boot = copy_disk(tmp, "rootdisk_a_boot.img")
    data = build_second_disk(tmp)
    blank = blank_disk(tmp, "rootdisk_a_blank.img")
    sock = os.path.abspath(os.path.join(tmp, "rootdisk_a.serial"))
    pidfile = os.path.abspath(os.path.join(tmp, "rootdisk_a.pid"))
    for p in (sock, pidfile):
        if os.path.exists(p):
            os.remove(p)
    sh(f"qemu-system-x86_64 {' '.join(guarded_boot_args(boot, check_disk=False))}"
       f" -device ich9-ahci,id=h0 -device ich9-ahci,id=h1"
       f" -drive if=none,id=s0,file={data},format=raw"
       f" -device ide-hd,drive=s0,bus=h0.0"
       f" -drive if=none,id=s1,file={boot},format=raw"
       f" -device ide-hd,drive=s1,bus=h0.1,bootindex=0"
       f" -drive if=none,id=s2,file={blank},format=raw"
       f" -device ide-hd,drive=s2,bus=h1.0"
       f" -m 512 -display none -no-reboot -serial unix:{sock},server,nowait"
       f" -daemonize -pidfile {pidfile}")
    shell = None
    try:
        shell = connect(sock)
        if shell is None:
            res.check("the three-drive machine booted", False, "no serial console")
            return
        log = shell.run("dmesg")
        found = [ln.strip() for ln in log.splitlines() if 'ahci: port' in ln and '"' in ln]
        res.check("both HBAs were driven", log.count("ahci: HBA ") == 2,
                  f"{log.count('ahci: HBA ')} HBA lines")
        res.check("all three SATA disks were identified", len(found) == 3, " | ".join(found))
        res.check("...and named ahci0, ahci1, ahci2",
                  all(f"ahci{i}" in log for i in range(3)),
                  " | ".join(ln.strip() for ln in log.splitlines() if "block:" in ln)[:300])
        res.check("the root is the disk GRUB booted -- port 1, not the first drive",
                  "is on ahci1 -- the boot disk" in log and "mounted at / on ahci1p3" in log,
                  fs_lines(log))

        out = shell.run("ahci")
        res.check("`ahci` reports both HBAs and marks all three drives",
                  out.count("ahci: HBA version") == 2 and
                  all(f"<- ahci{i}" in out for i in range(3)), out.strip()[-500:])

        shell.run("mkdir /mnt2")
        shell.run("mount ahci0p1 /mnt2")
        got = shell.run(f"cat /mnt2/{MARK_PATH}")
        res.check("the port-0 data disk mounts and reads the host's marker",
                  MARK_TEXT in got, got.strip()[-200:])
        shell.run(f"cp /mnt2/{MARK_PATH} /mnt2/written.txt")
        shell.run("sync")
        shell.run("umount /mnt2")
        shell.run("mount ahci0p1 /mnt2")
        got = shell.run("cat /mnt2/written.txt")
        res.check("...and a file written to it survives a remount",
                  MARK_TEXT in got, got.strip()[-200:])

        out = shell.verb("ktest ahci", timeout=120.0)
        summary = [ln.strip() for ln in out.splitlines() if "ktest:" in ln and "passed" in ln]
        res.check("the AHCI KTESTs pass with every drive present, none skipped",
                  bool(summary) and "0 failed" in summary[-1] and "0 skipped" in summary[-1],
                  " | ".join(summary)[:300] or out.strip()[-300:])
    finally:
        if shell:
            shell.close()
        kill(pidfile)


def phase_blank(res, tmp):
    print("rootdisk_test: B -- a blank virtio disk beside an IDE system disk")
    boot = copy_disk(tmp, "rootdisk_b_boot.img")
    blank = blank_disk(tmp, "rootdisk_b_blank.img")
    devs = f"{ide_boot(boot)} {virtio(blank)}"

    log = boot_to_root(boot, devs, tmp, "b1")
    res.check("bootpart= names the IDE disk, and the root mounts from it",
              "is on ata0 -- the boot disk" in log and "mounted at / on ata0p3" in log,
              fs_lines(log))

    without_bootpart(boot)
    log = boot_to_root(boot, devs, tmp, "b2")
    res.check("without bootpart=, the blank virtio disk is passed over by content",
              "virtio0 has no partition table, so the root disk is ata0" in log and
              "mounted at / on ata0p3" in log, fs_lines(log))


def phase_two_systems(res, tmp):
    print("rootdisk_test: C -- two system disks, GRUB booting the slower one")
    boot = copy_disk(tmp, "rootdisk_c_boot.img")
    other = copy_disk(tmp, "rootdisk_c_other.img")
    reguid(other)
    devs = f"{ide_boot(boot)} {virtio(other)}"

    log = boot_to_root(boot, devs, tmp, "c1")
    res.check("bootpart= picks the disk GRUB booted over a faster bootable one",
              "is on ata0 -- the boot disk" in log and "mounted at / on ata0p3" in log,
              fs_lines(log))

    without_bootpart(boot)
    log = boot_to_root(boot, devs, tmp, "c2")
    res.check("...and without it precedence picks virtio -- bootpart= was what decided",
              "mounted at / on virtio0p3" in log, fs_lines(log))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--tmp", default="/tmp")
    ap.add_argument("--phase", choices=("sata", "blank", "two", "all"), default="all")
    args = ap.parse_args()

    res = Results()
    if args.phase in ("sata", "all"):
        phase_sata(res, args.tmp)
    if args.phase in ("blank", "all"):
        phase_blank(res, args.tmp)
    if args.phase in ("two", "all"):
        phase_two_systems(res, args.tmp)

    print("\n" + res.summary("rootdisk_test"))
    return 1 if res.fails else 0


if __name__ == "__main__":
    sys.exit(main())
