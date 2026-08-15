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

// Applies W^X to the kernel's own identity map, which boot.asm leaves
// flat present+writable across all 4GiB. Call once, as early in
// kernel_main() as possible -- it needs no allocator, no heap and no
// interrupts, only the linker symbols describing where .text ends.
//
// Afterwards exactly one range in the kernel map is executable (.text,
// which is also read-only), the rest of the kernel image is read-only
// and NX, and every other page -- RAM, the framebuffer, MMIO -- is
// writable and NX. It also sets CR0.WP, without which ring 0 would
// ignore the read-only bit entirely; see paging.c for why that half is
// easy to leave out and hard to notice.
//
// Returns 1 on success, 0 if the fixed pool of split page tables wasn't
// big enough to cover the read-only part of the kernel image (see
// MAX_WX_TABLES in paging.c). A 0 means the map is left partly or
// wholly unprotected -- it is a real failure, not a nicety, and the
// "paging" KTEST asserts against it.
int paging_enforce_wx(void);

// The leaf page-table entry the kernel's own map resolves `vaddr`
// through -- a 2MiB PDE where the map is still huge, a 4KiB PTE where
// paging_enforce_wx() split it. Returns 0 if nothing is mapped. For
// tests and diagnostics; nothing in the kernel's normal operation needs
// to ask.
uint64_t paging_kernel_leaf(uint64_t vaddr);

// How many pages in the kernel's own map are simultaneously writable
// and executable, i.e. how many times W^X is violated. Should be 0
// after paging_enforce_wx(); walks all 2048 PDEs and every split table
// under them.
int paging_wx_violations(void);

#endif
