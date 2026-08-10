#ifndef PMM_H
#define PMM_H

#include <stdint.h>

// A bitmap-based physical frame allocator, 4KiB granularity. Built from
// the Multiboot2 memory map (see multiboot.h) -- only regions GRUB
// reports as "available" are ever handed out, and the kernel's own
// image (from _kernel_end, the linker symbol marking where it ends) plus
// everything below 1MiB (BIOS/legacy area) are reserved regardless of
// what the memory map claims about them.
//
// This only manages *physical* frames -- it knows nothing about virtual
// addresses or page tables. Pairs with paging.c, which does the mapping
// once you have a frame.
void pmm_init(void);

// Returns the physical address of a free 4KiB-aligned frame (and marks
// it used), or 0 if none are left.
uint64_t pmm_alloc_frame(void);

// Marks a frame as free again. `phys_addr` should be a value previously
// returned by pmm_alloc_frame() -- freeing an address pmm doesn't
// recognize as an allocated frame is a no-op.
void pmm_free_frame(uint64_t phys_addr);

uint64_t pmm_total_frames(void);
uint64_t pmm_free_frames(void);

#endif
