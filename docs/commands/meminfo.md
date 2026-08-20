# meminfo

**a `/bin` program.**

**Category:** System information

## Synopsis

    meminfo [--map | --audit | --list]

## Description

The memory map, the physical frame allocator and the kernel heap. Reads through the FACT registry (`SYS_QUERY`), so it and `/bin/meminfo` are one reader and cannot report different numbers.