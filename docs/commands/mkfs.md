# mkfs

**a `/bin` program.**

**Category:** Storage

## Synopsis

    mkfs [-t <type>] <partition> confirm

## Description

`/bin/mkfs` — put an empty filesystem on a partition.

`-t` is the filesystem type (`tfs3` by default, or `fat32`), and the
partition is named the way `lsblk` prints it — `ahci0p3`, `virtio0p1`.

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

**Formatting a second volume works.** It did not until 2026-08-31: every
backend keeps its volume in module-level state, and both `tfs3_format()`
and `mount_wipe_others()` repointed it at the target, so the mounted root
began reading the wrong disk. That crashed twice, each time taking `/bin`
with it — once with a FAT32 target, because the wipe reaches tfs3's
globals whatever is being formatted. `format()` and `wipe()` now save and
restore that state on every path (`tfs3.c`'s `struct t3_saved`).

**Mounting a second volume of the same type still does not**, and that is
the remaining half of the same problem: `fs_ops.max_mounts` is 1. So you
can format an install target but not yet copy anything onto it. The fix
for both is per-volume state — an instance handle rather than globals —
and `docs/bugs.md` tracks it as the installer's prerequisite.

The kernel still refuses to format a volume that is **itself** mounted,
which is a different and permanent rule.

## Exit status

0 on success; 1 with a reason on failure. The reason comes from
`sys_errno()`, **not** from the return value — every wrapper here
converts a negative errno to `-1` and stashes the code, and `-1` is
`-EPERM`, so a chain of comparisons against the return value reports
every failure as whichever it happens to equal last. This program got
that wrong first and reported a missing partition as "refused without
confirmation".

## See also

`fsformat` (the kernel shell's, for the running root), `mkpart`,
`mount`, `lsblk`, `df`.
