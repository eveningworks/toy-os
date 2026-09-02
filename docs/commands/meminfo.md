# meminfo

**a `/bin` program.**

**Category:** System information

## Synopsis

    meminfo [--map | --audit | --list]

## Description

The memory map, the physical frame allocator and the kernel heap. Reads through the FACT registry (`SYS_QUERY`), so it and `/bin/meminfo` are one reader and cannot report different numbers.

On a machine with more than 4 GiB the frame section adds an `above 4 GiB` row: those frames are managed and audited but handed out only to `PMM_ZONE_ANY` callers, of which there are none until the identity map is extended ("More than 4 GiB of RAM" in `docs/roadmap.md`). `total` and `free` include them; the row is what to subtract for what the machine can use today.