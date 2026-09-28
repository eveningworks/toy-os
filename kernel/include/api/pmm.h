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
//
// EVERY ALLOCATION NAMES A ZONE. The identity map reaches every managed
// frame (paging_extend_identity_map()), so the zones are about DEVICES,
// not about what the kernel can dereference: the ATA PRD and AC97 BDL
// registers are 32-bit by specification, and a card without `ac64`
// cannot address a high frame at all. A caller feeding a DMA engine says
// DMA32; anything CPU-only says ANY, and ANY prefers the high zone so
// the low one is kept for the callers that need it (Linux's gfp zone
// fallback order, in miniature).
//
// The bitmaps are sized from the memory map at pmm_init(), not from a
// constant, and carved out of low usable RAM before anything else is
// handed out -- so the frames above 4 GiB are MANAGED (accounted for,
// auditable) from stage 1 on, whether or not anything can map them.
enum pmm_zone {
    PMM_ZONE_DMA32 = 0, // below 4 GiB: identity-mapped, reachable by every DMA engine
    PMM_ZONE_ANY   = 1, // anywhere managed; tries above 4 GiB first
};

void pmm_init(void);

// Returns the physical address of a free 4KiB-aligned frame in `zone`
// (and marks it used), or 0 if none are left there.
uint64_t pmm_alloc_frame(enum pmm_zone zone);

// Drops one REFERENCE to a frame, and frees it when the last one goes.
// A fresh allocation holds one, pmm_frame_ref() adds one, so for every
// caller that never shares a frame this is the plain "free" it always
// was. Freeing an address pmm doesn't recognize as an allocated frame
// is a no-op -- logged, since it is the shape a double free takes.
void pmm_free_frame(uint64_t phys_addr);

// A SECOND OWNER of a frame -- a copy-on-write share, a page two
// address spaces map. Each owner's pmm_free_frame() drops one; the
// frame goes back to the pool at zero. A reserved frame (the image, the
// books) carries no count and cannot be shared.
void pmm_frame_ref(uint64_t phys_addr);
// How many owners a frame has: 0 for a free or reserved frame.
unsigned pmm_frame_refs(uint64_t phys_addr);

// Returns the physical address of the first frame of `count` physically
// CONTIGUOUS free 4KiB frames (marking all of them used), or 0 if no
// run that long exists. `count == 1` is just pmm_alloc_frame() under
// the hood. The run lies entirely inside `zone` -- a DMA32 run never
// straddles 4 GiB. For `count > 1`: a linear scan of the same bitmap
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
uint64_t pmm_alloc_contiguous(uint64_t count, enum pmm_zone zone);

// Frees `count` frames starting at `phys_addr` (a value previously
// returned by pmm_alloc_contiguous() with the same `count`) -- mirrors
// pmm_alloc_contiguous() the way pmm_free_frame() mirrors
// pmm_alloc_frame(), so a caller frees a DMA buffer as the one block it
// allocated, not as `count` separate pmm_free_frame() calls it has to
// remember to make. Frames pmm doesn't recognize as allocated are
// silently skipped, same as pmm_free_frame().
void pmm_free_contiguous(uint64_t phys_addr, uint64_t count);

// THE DMA32 RESERVE. An ANY allocation prefers the high zone and falls
// back into DMA32 when it is empty -- and once user pages, page tables
// and window buffers all say ANY, that fallback can drain the one zone a
// 32-bit DMA engine can reach. So the fallback stops at a floor, and a
// caller that NAMED DMA32 ignores it. Zero on a machine with no memory
// above 4 GiB, where there is nothing to fall back from.
//
// Settable so the floor is a knob rather than a constant -- Linux's
// equivalent, lowmem_reserve_ratio, is a sysctl. pmm_init() sets the
// policy value; the `mm` KTEST drives it to prove the refusal fires.
uint64_t pmm_dma32_reserve_frames(void);
void pmm_set_dma32_reserve_frames(uint64_t frames);

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

// Over EVERY managed frame, both zones. A reader that can only use low
// memory (ramfs's budget, About's "usable") asks the per-zone pair --
// until the consumers move to ANY, a free frame above 4 GiB is free
// in the books and unusable in practice.
uint64_t pmm_total_frames(void);
uint64_t pmm_free_frames(void);
uint64_t pmm_zone_total_frames(enum pmm_zone zone);
uint64_t pmm_zone_free_frames(enum pmm_zone zone);

// Bytes of RAM the firmware map calls usable, whole frames only and
// uncapped -- what a machine has. Differs from pmm_total_frames() by
// the reservations (the image, the bitmaps, the multiboot info) and by
// anything above the ring-3 map's base, which is never managed.
uint64_t pmm_firmware_bytes(void);

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

// KASAN=1 only (kasan_init): poisons the shadow of every free frame.
void pmm_kasan_poison_free(void);

#endif
