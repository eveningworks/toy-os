# The filesystem

**Category:** Getting started

## Description

Filesystems live behind one interface. TFS3 is the root, with inodes,
block groups, a journal and indirect blocks; FAT32 holds `/boot`, where
GRUB can read it. A filesystem talks to a block device rather than to a
disk, so the same code serves an IDE controller, AHCI and a virtio disk
without knowing which.

Each mount has one lock, and its holder may sleep while it waits for
the disk. A ring-3 process is preemptible inside a syscall, so without
that lock two interleaved reads would overwrite each other's scratch
state -- which once presented as files that plainly existed being
reported as missing on about one boot in three.

## See also

`ls`, `mount`, `df`, `lsblk`, `fsck`
