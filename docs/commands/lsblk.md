# lsblk

**a `/bin` program.**

**Category:** System information

## Synopsis

    lsblk

## Description

`/bin/lsblk` — every block device this boot found: each disk a driver
enumerated, each partition the kernel named, which one carries the root,
and whether it survives a power cycle.

**The names it prints are the names `root=` and `mount` take**, and that
is most of the reason to print them. `mount ahci0p1 /mnt` is not
something anyone can guess, and before this there was no way to find out
what the second disk was called — or that it existed.

A name is `<driver><index>` for a disk and `<disk>p<n>` for a partition:
`ata0`, `ahci0`, `virtio0`, `ram0`, `ahci0p1`. The partition number is
the **partition table's**, so it matches what `parttable` prints — the
third entry is `p3` even when the two ahead of it are the firmware's and
were skipped.

## What it is not

**Not `parttable`.** That reads a table off a *disk* and reports what is
written there; this reports what the *kernel* found and named. They can
disagree, usefully: a partition whose window the kernel refused (one
extending past the disk's end, say) is in `parttable`'s output and not
in this one, and that difference is the diagnosis.

**Not `df`.** `df` lists what is *mounted*. Most devices here are not,
and that is the normal state — a second drive is enumerated so that it
*can* be mounted.

## Output

    NAME        SIZE      TYPE   ROOT  WHERE
    ata0        9.0G      disk         disk
    ata0p1      1.0M      part         ata0 at lba 2048
    ata0p2      64.0M     part         ata0 at lba 4096
    ata0p3      8.9G      part   yes   ata0 at lba 135168

`WHERE` is the parent disk and starting LBA for a partition — the number
that lines a row up against `parttable`. For a whole disk it says
`disk`, or `memory (not persistent)` for a RAM-backed live image, since
"this will not survive a reboot" is the one property a person most needs
not to have to infer.

A machine with no disk at all prints `no block devices` rather than a
bare header: that is a real supported state (the root is ramfs then),
and it should not read like the command failed.

## See also

`parttable` for a disk's own table, `mount` for attaching one of these,
`df` for what is mounted, and `docs/boot-flags.md` for `root=`.
