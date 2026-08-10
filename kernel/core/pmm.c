// Bitmap-based physical frame allocator. One bit per 4KiB frame, 1 =
// used/reserved, 0 = free. Built once at boot from the Multiboot2
// memory map (see multiboot.c) and never resized -- this is deliberately
// simple, matching the rest of toy-os's approach so far: correct and
// easy to reason about, not optimized.
#include "pmm.h"
#include "multiboot.h"
#include <stddef.h>

// Linker symbol from linker.ld: the first physical address after
// everything the kernel image occupies (code, data, bss). Reserving up
// to here (rather than a hardcoded guess) means this stays correct if
// the kernel grows.
extern char _kernel_end[];

#define FRAME_SIZE 4096

// Frames are only ever handed out within the first 4GiB, matching the
// range boot.asm identity-maps -- allocating a frame beyond that would
// produce a physical address paging.c has no mapping for yet.
#define PMM_MAX_FRAMES ((uint64_t)4 * 1024 * 1024 * 1024 / FRAME_SIZE)
#define BITMAP_BYTES (PMM_MAX_FRAMES / 8)

static uint8_t bitmap[BITMAP_BYTES];
static uint64_t total_frames = 0; // frames within regions firmware reported as available
static uint64_t free_frames = 0;  // currently allocatable (total minus reservations)
static uint64_t alloc_hint = 0;   // avoids rescanning from frame 0 on every alloc

static inline void mark_used_bit(uint64_t frame) {
    if (frame >= PMM_MAX_FRAMES) return;
    bitmap[frame / 8] |= (uint8_t)(1u << (frame % 8));
}

static inline void mark_free_bit(uint64_t frame) {
    if (frame >= PMM_MAX_FRAMES) return;
    bitmap[frame / 8] &= (uint8_t)~(1u << (frame % 8));
}

static inline int bit_is_used(uint64_t frame) {
    return (bitmap[frame / 8] & (1u << (frame % 8))) != 0;
}

static void mark_available_cb(const struct multiboot_mmap_region *region) {
    if (region->type != 1) return; // only "available" RAM

    uint64_t start = region->base;
    uint64_t end = region->base + region->length;

    // Only free whole frames fully inside the region -- round the start
    // up and the end down, rather than risk treating a partial frame at
    // either edge as usable.
    start = (start + FRAME_SIZE - 1) & ~((uint64_t)FRAME_SIZE - 1);
    end = end & ~((uint64_t)FRAME_SIZE - 1);
    if (end > PMM_MAX_FRAMES * FRAME_SIZE) end = PMM_MAX_FRAMES * FRAME_SIZE;

    for (uint64_t addr = start; addr < end; addr += FRAME_SIZE) {
        mark_free_bit(addr / FRAME_SIZE);
        total_frames++;
    }
}

static void reserve_range(uint64_t start, uint64_t end) {
    start &= ~((uint64_t)FRAME_SIZE - 1);
    end = (end + FRAME_SIZE - 1) & ~((uint64_t)FRAME_SIZE - 1);
    if (end > PMM_MAX_FRAMES * FRAME_SIZE) end = PMM_MAX_FRAMES * FRAME_SIZE;

    for (uint64_t addr = start; addr < end; addr += FRAME_SIZE) {
        mark_used_bit(addr / FRAME_SIZE);
    }
}

void pmm_init(void) {
    for (uint64_t i = 0; i < BITMAP_BYTES; i++) bitmap[i] = 0xFF; // start fully reserved
    total_frames = 0;

    multiboot_mmap_foreach(mark_available_cb);

    // Reserve everything below and including the kernel image,
    // regardless of what the memory map claims about that range --
    // BIOS/GRUB commonly report it as "available" since they have no
    // idea a kernel is sitting inside it.
    reserve_range(0, (uint64_t)(uintptr_t)_kernel_end);

    // Also reserve any Multiboot2 module (e.g. userland/hello.elf,
    // Also reserve any Multiboot2 module(s) -- e.g. userland/hello.elf,
    // loaded by GRUB via grub.cfg's `module2` line(s) -- it's placed
    // somewhere in physical memory GRUB picked, which is NOT necessarily
    // covered by the kernel image's own range above. Missing this would
    // let pmm_alloc_frame() hand out a module's own memory to whoever
    // asks first, silently corrupting it out from under anyone still
    // reading it (see elf.c / elf_test.c, and the changelog entry this
    // bug is documented under). There can be more than one module (see
    // syscall_test.c), so reserve all of them, not just the first.
    for (int i = 0; ; i++) {
        struct multiboot_module_info mod;
        if (!multiboot_get_module(i, &mod)) break;
        reserve_range(mod.start, mod.end);
    }

    // Also reserve the Multiboot2 INFO STRUCTURE itself (the tag list
    // multiboot_get_module()/multiboot_mmap_foreach() etc. all read) --
    // a different region from the modules' own content above, and one
    // that stays alive for the whole boot (M16's scheduler re-reads
    // module info via multiboot_get_module() well after boot, to spawn
    // a second process -- see scheduler.c). Found the hard way: without
    // this, spawning one ring-3 process could hand out this exact range
    // via pmm_alloc_frame() (e.g. for that process's user stack), and
    // the very next multiboot_get_module() call -- looking up a LATER
    // module index -- would read corrupted tag data and silently fail,
    // even though the module was really there. Same root cause as the
    // module-content bug above, different region.
    uint64_t info_start, info_end;
    if (multiboot_get_info_range(&info_start, &info_end)) {
        reserve_range(info_start, info_end);
    }

    free_frames = 0;
    for (uint64_t f = 0; f < PMM_MAX_FRAMES; f++) {
        if (!bit_is_used(f)) free_frames++;
    }
}

uint64_t pmm_alloc_frame(void) {
    for (uint64_t i = 0; i < PMM_MAX_FRAMES; i++) {
        uint64_t f = (alloc_hint + i) % PMM_MAX_FRAMES;
        if (!bit_is_used(f)) {
            mark_used_bit(f);
            free_frames--;
            alloc_hint = f + 1;
            return f * FRAME_SIZE;
        }
    }
    return 0; // out of memory
}

void pmm_free_frame(uint64_t phys_addr) {
    uint64_t f = phys_addr / FRAME_SIZE;
    if (f >= PMM_MAX_FRAMES) return;
    if (bit_is_used(f)) {
        mark_free_bit(f);
        free_frames++;
    }
}

uint64_t pmm_total_frames(void) { return total_frames; }
uint64_t pmm_free_frames(void) { return free_frames; }
