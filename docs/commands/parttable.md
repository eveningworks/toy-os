# parttable

**a `/bin` program.**

**Category:** System information

## Synopsis

    parttable [DISK]

## Description

`/bin/parttable` — a disk's MBR/GPT table, read-only: the boot disk's, or `DISK`'s (`parttable virtio0`; `lsblk` names the disks). Two query classes back it, because a list alone cannot tell "a table with no partitions" from "no table at all".

**A stock disk is partitioned and bootable** (`tools/seed_disk.py --partition`, `tools/install_grub.py`): a GPT holding a BIOS boot partition, a FAT32 `/boot` and TFS3, so `parttable` prints three entries. "None" is what a BLANK image looks like, not what a seeded one does.

**Every disk's table is queryable**: `QUERY_PARTTABLE` has a record per whole disk, the boot disk's first, and `QUERY_PARTITION` carries each entry's `disk` and slot `number`. `lsblk` lists what the kernel NAMED; this reports what is WRITTEN on a disk, and the two disagree usefully -- a table rewritten on the boot disk is read here at once and named by `lsblk` only after the next boot.

## Options

- `-h`, `--help` -- print the usage and exit.
