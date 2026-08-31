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

## It cannot actually run yet, and that is the interesting part

**Every backend keeps its volume in module-level state.** `tfs3_format()`
begins with `set_flat_volume(dev)` and then writes `g_sb`, `g_vol` and
the geometry; `mount_wipe_others()` calls `wipe(dev)` on every *other*
backend, which reaches the same globals through tfs3 even when the
target is FAT32.

So formatting **any** device, as **any** type, repoints whatever is
mounted, and the running root starts reading the wrong disk. That was
measured twice — each time as a crash that took `/bin` with it, once
formatting TFS3 and once FAT32, both with a TFS3 root.

The kernel therefore refuses while any **disk-backed** filesystem is
mounted. A `ramfs` mount does not count, because it has no volume — but
no boot mode today reaches that state: even the live image's root is
TFS3 on a RAM disk.

What ships is the syscall, the checks and the refusal, which turns a
root-destroying crash into a message. Making it *useful* needs
per-volume state in the backends — an instance handle rather than
globals — and `docs/bugs.md` records that as the installer's real
prerequisite.

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
