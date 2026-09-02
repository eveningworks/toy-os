# meminfo

**a `/bin` program.**

**Category:** System information

## Synopsis

    meminfo [--map | --audit | --list]

## Description

The memory map, the physical frame allocator and the kernel heap. Reads through the FACT registry (`SYS_QUERY`), so it and `/bin/meminfo` are one reader and cannot report different numbers.

On a machine with more than 4 GiB the frame section adds an `above 4 GiB` row: those frames are managed, mapped and used by the kernel heap, but ring-3 pages still come from below 4 GiB ("More than 4 GiB of RAM" in `docs/roadmap.md`). `total` and `free` include them; the row is what to subtract for what a process can get today.