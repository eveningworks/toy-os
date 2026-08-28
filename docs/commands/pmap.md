# pmap

**a `/bin` program.**

**Category:** System information

## Synopsis

    pmap [pid]

## Description

One process's address space, region by region: the image, the heap, the stack, and every `mmap` region (anonymous or file-backed, with its protection and backing path). With no argument it maps itself. Reads `QUERY_PROCMAP` through the FACT registry.

Every span printed is a RESERVATION, not memory: `sbrk` and `mmap` map nothing, and pages arrive on first touch — so a 2 GiB heap line is address space, and `ps`/Task Manager carry the resident side. That is the number this command exists to make visible; do not read its sizes as usage.
