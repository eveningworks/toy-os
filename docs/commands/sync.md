# sync

**a `/bin` program.**

**Category:** Files and the filesystem

## Synopsis

    sync

## Description

Flushes everything buffered on the way to the platter, on **every
mounted volume**, whatever filesystem is on it and whatever driver is
under it. Runs automatically at shutdown and reboot, and at the
journal's barriers, so this is for "write it out NOW" rather than
routine use. Says so loudly if anything could not be written, since that
data then exists only in RAM or only in the drive.

**Two stages, because there are two places data sits:**

1. a driver's **software** write-back cache -- only the ATA path has one
   -- which holds sectors in RAM;
2. the **drive's own** volatile cache, which only a device flush empties.

The sector count reports stage 1 only, so **zero is normal and does not
mean nothing happened**: on a machine with no software cache the real
work is the flush every mounted disk just took.

**`fsync(2)` is the per-file version.** `SYS_FSYNC` takes a descriptor,
commits the backend holding that file's path and flushes the device
under it, leaving other mounts alone. A program that wants its own data
durable at a moment of its choosing calls that; this command is for the
whole machine. It matters most under `storage.sync = batched`, where a
write returns before its commit -- see `docs/pagecache-design.md`.
