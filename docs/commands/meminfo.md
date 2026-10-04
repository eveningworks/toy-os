# meminfo

**a `/bin` program.**

**Category:** System information

## Synopsis

    meminfo [--map | --audit | --list]

## Description

The memory map, the physical frame allocator and the kernel heap. Reads through the FACT registry (`SYS_QUERY`), so it and `/bin/meminfo` are one reader and cannot report different numbers.

On a machine with more than 4 GiB the frame section adds two rows. `above 4 GiB` is how much of `total`/`free` lies up there; every CPU-only allocation prefers it, so a busy machine's free count in that row is the one that moves. `DMA32 reserve` is the floor an allocation that could go anywhere will not drive the low zone below, printed beside what is actually left there -- the pair answers "is a driver about to be refused?", which is a different question from "is the machine out of memory?". A caller that specifically needs a low frame ignores the floor; see `api/pmm.h`.

`In use, by kind` splits `used` into four rows that sum to it, the ones Task Manager draws (`lib/umemcomp.h`): `apps` is every process's private memory (a copy-on-write page divided between the processes sharing it), `shared` every shared-memory object counted once (window buffers), `graphics` the RAM held for the screen (extra scanouts, a cursor image, the console's back buffer -- not VRAM or the firmware's stolen memory), and `kernel` the remainder: heap, page tables, the kernel image, drivers' rings and the disk and library caches. Kernel is computed, not counted, so it absorbs anything the other three miss.