# parttable

**a `/bin` program.**

**Category:** System information

## Synopsis

    parttable

## Description

`/bin/parttable` — the disk's MBR/GPT table, read-only. Two query classes back it, because a list alone cannot tell "a table with no partitions" from "no table at all".

This page used to say the stock `disk.img` has **no** table and that "none" is the normal answer. That stopped being true when the image became partitioned and bootable (`tools/seed_disk.py --partition`, `tools/install_grub.py`): a stock disk today is a GPT holding a BIOS boot partition, a FAT32 `/boot` and TFS3, and `parttable` prints three entries. "None" is now what a BLANK image looks like, not what yours does.

**It reads ONE disk — the root's.** `blk_whole_disk()`, so on a machine with several drives this answers for the one the root came from and says nothing about the others. `lsblk` is what lists every device the kernel found; the two disagree usefully, since this reports what is written on a disk and `lsblk` reports what the kernel named.