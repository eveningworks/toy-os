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

// Returns the physical address of the first frame of `count` physically
// CONTIGUOUS free 4KiB frames (marking all of them used), or 0 if no
// run that long exists. `count == 1` is just pmm_alloc_frame() under
// the hood. For `count > 1`: a linear scan of the same bitmap
// pmm_alloc_frame() uses, looking for a run of `count` consecutive
// free bits instead of just one -- no new data structure, and no
// fragmentation-avoidance machinery (a buddy allocator, say) beyond
// that scan. Deliberately minimal: this only gets called rarely (a
// driver setting up a DMA descriptor ring once at init, not a hot
// path) and, in this kernel, always early at boot before much of the
// general pool has been touched -- see this header's top comment and
// docs/decisions.md for the fuller reasoning, including what a real
// buddy allocator would buy over this if fragmentation ever became a
// real problem. First real caller: a future NIC driver's descriptor
// ring (see README.md's TCP/IP entry).
uint64_t pmm_alloc_contiguous(uint64_t count);

// Frees `count` frames starting at `phys_addr` (a value previously
// returned by pmm_alloc_contiguous() with the same `count`) -- mirrors
// pmm_alloc_contiguous() the way pmm_free_frame() mirrors
// pmm_alloc_frame(), so a caller frees a DMA buffer as the one block it
// allocated, not as `count` separate pmm_free_frame() calls it has to
// remember to make. Frames pmm doesn't recognize as allocated are
// silently skipped, same as pmm_free_frame().
void pmm_free_contiguous(uint64_t phys_addr, uint64_t count);

// For an auditor comparing page tables against the allocator (see
// vmm_audit_space()): is this frame pmm's to account for, and does pmm
// think it is handed out? The pair matters because "not mine" (MMIO, a
// framebuffer) and "mine and free" are completely different answers --
// the second means a live mapping points at memory pmm may hand to
// somebody else.
int pmm_frame_is_managed(uint64_t phys_addr);
int pmm_frame_is_used(uint64_t phys_addr);

// Bytes in one frame. Exposed so a caller reporting BYTES multiplies by
// this rather than by a literal 4096 -- which is the assumption that
// would silently produce wrong numbers the day this allocator gains a
// second page size. sys_sysinfo() open-coded it as `* 4` for KB.
uint64_t pmm_frame_size(void);

uint64_t pmm_total_frames(void);
uint64_t pmm_free_frames(void);

// Exercises pmm_alloc_contiguous()/pmm_free_contiguous() once and logs
// pass/fail via klog_write() -- there's no driver calling these yet (the
// intended first caller is a future NIC descriptor ring), so without
// this a regression here would only be caught by reading the code, not
// by anything a boot actually exercises. Called once from kernel_main()
// right after pmm_init(); cheap (a handful of frames, once, at boot) and
// left in permanently rather than treated as throwaway -- same idea as
// the klog_write() lines around it in kernel.c proving other
// no-GUI-surface infrastructure initialized correctly.
int pmm_selftest(void); // 1 = passed, 0 = failed (details logged) -- wrapped by a KTEST

#endif
