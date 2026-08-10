#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

// Marks the single 4KB page containing `vaddr` as user-accessible (adds
// the USER bit to its page-table entry), splitting the 2MB huge page
// that currently covers it into individual 4KB pages first if needed.
// Everything else in that 2MB region keeps its original identity mapping
// (present + writable, no USER bit) -- only the specific page(s) you
// call this on become reachable from ring 3.
//
// vaddr must fall within the first 4GiB (the range boot.asm identity-maps
// with 2MiB pages). Returns 1 on success, 0 if vaddr is out of range or
// the small fixed pool of split page tables is exhausted (see
// MAX_SPLIT_TABLES in paging.c -- this isn't a general-purpose allocator,
// just enough for a handful of user-test regions).
int paging_make_user_page(uint64_t vaddr);

#endif
