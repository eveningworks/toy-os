# parttable

**a `/bin` program.**

**Category:** System information

## Synopsis

    parttable

## Description

`/bin/parttable` — the disk's MBR/GPT table, read-only. Two query classes back it, because a list alone cannot tell "a table with no partitions" from "no table at all".

**A stock disk is partitioned and bootable** (`tools/seed_disk.py --partition`, `tools/install_grub.py`): a GPT holding a BIOS boot partition, a FAT32 `/boot` and TFS3, so `parttable` prints three entries. "None" is what a BLANK image looks like, not what a seeded one does.

**It reads ONE disk — the root's.** `blk_root_disk()`, so on a machine with several drives this answers for the one the root came from and says nothing about the others. `lsblk` is what lists every device the kernel found; the two disagree usefully, since this reports what is written on a disk and `lsblk` reports what the kernel named.