# fsformat

**a shell builtin.**

**Category:** Disk and filesystem maintenance

## Synopsis

    fsformat <tfs3> confirm

## Description

**Destroys the disk's contents**, reformats with the named filesystem and
remounts live. Physical shell only.

It formats **whatever block device is active** — which, on a partitioned
disk, is a partition and not the whole disk. That is what makes
`mkpart` → reboot → `fsformat tfs3 confirm` put a filesystem *inside*
partition 1. See `docs/commands/mkpart.md`.

`tfs3` is the only name today. It reads as a list of one because it is a
list: FAT32 is the next backend (`docs/roadmap.md`), and TFS2 was the
previous second name until it was removed.