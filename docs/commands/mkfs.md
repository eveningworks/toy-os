# mkfs

**a `/bin` program.**

**Category:** Storage

## Synopsis

    mkfs [-t <type>] <partition> confirm

## Options

- `-t <type>` -- the filesystem to write: `tfs3` (the default) or
  `fat32`.

## Description

`/bin/mkfs` — put an empty filesystem on a partition.

The partition is named the way `lsblk` prints it — `ahci0p3`,
`virtio0p1`.

**`confirm` is a word you type**, not a flag, following `fsformat tfs3
confirm` and `mkpart`'s. There is no privilege model in this OS to gate
a destructive storage operation with, so the stand-in is that the person
asking spells it out. It is a speed bump and not a permission check, and
the ABI says so rather than letting anyone mistake it for one.

## Why this is not `fsformat`

`fsformat` reformats the volume this machine is **running from**: it
unmounts everything, formats, and re-probes the world. That is the right
shape for "wipe this machine" and the wrong shape for "put a filesystem
on that other partition", which is what an installer wants and which
must disturb nothing.

## What it can and cannot do

**It formats some other partition and disturbs nothing.** A target that
is itself mounted is refused (`EBUSY`) rather than unmounted for you --
a permanent rule, and the one thing this command will not do for an
installer.

## Exit status

0 on success; 1 with a reason on failure. The reason comes from
`sys_errno()`, **not** from the return value -- every wrapper here
converts a negative errno to `-1` and stashes the code, and `-1` is
`-EPERM`, so a chain of comparisons against the return value reports
every failure as whichever it happens to equal last. `EINVAL` is no such
partition or no such type, `EBUSY` a target that is mounted, `EPERM` a
missing `confirm`, and `EIO` a format that failed -- `dmesg` says why.

## See also

`fsformat` (the kernel shell's, for the running root), `mkpart`,
`mount`, `lsblk`, `df`.
