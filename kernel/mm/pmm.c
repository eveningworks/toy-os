// Bitmap-based physical frame allocator. One bit per 4KiB frame, 1 =
// used/reserved, 0 = free. Built once at boot from the Multiboot2
// memory map (see multiboot.c) and never resized -- this is deliberately
// simple, matching the rest of toy-os's approach so far: correct and
// easy to reason about, not optimized.
#include "pmm.h"
#include "bootstage.h"
#include "multiboot.h"
#include "klog.h"
#include "reloc.h"
#include <stddef.h>

// Linker symbol from linker.ld: the first physical address after
// everything the kernel image occupies (code, data, bss). Reserving up
// to here (rather than a hardcoded guess) means this stays correct if
// the kernel grows.
extern char _kernel_end[];
// The first address the image occupies. Only needed once the image can
// MOVE -- see pmm_init()'s two-range reservation under kernel ASLR.
extern char __kimage_start[];

#define FRAME_SIZE 4096

// Frames are only ever handed out within the first 4GiB, matching the
// range boot.asm identity-maps -- allocating a frame beyond that would
// produce a physical address paging.c has no mapping for yet.
#define PMM_MAX_FRAMES ((uint64_t)4 * 1024 * 1024 * 1024 / FRAME_SIZE)
#define BITMAP_BYTES (PMM_MAX_FRAMES / 8)

static uint8_t bitmap[BITMAP_BYTES];
// Which frames pmm accounts for at all -- set once, from the firmware's
// memory map, and never cleared. Distinct from `bitmap`, which says
// allocated-or-not: an unmanaged frame (MMIO, a framebuffer) and an
// allocated one look identical there, and an auditor needs to tell them
// apart. One bit per frame, 128 KiB, same as the bitmap beside it.
static uint8_t managed[BITMAP_BYTES];
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
        uint64_t f = addr / FRAME_SIZE;
        managed[f / 8] |= (uint8_t)(1u << (f % 8));
        mark_free_bit(f);
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
    //
    // With kernel ASLR this is TWO ranges, not one. `_kernel_end` is a
    // relocated symbol, so it names the end of the image that is
    // actually running -- reserving [0, _kernel_end) would cover the
    // abandoned original too, but at the cost of reserving every free
    // byte between them, which on a randomized base is most of RAM.
    // So the two are reserved separately, and the abandoned image is
    // reserved rather than reclaimed because it is still LIVE: the CPU
    // is using the GDT inside it until gdt_init() replaces it, and
    // handing those frames out would corrupt it long before then.
    uint64_t reloc_delta = kernel_reloc_delta();
    if (reloc_delta) {
        reserve_range(0, (uint64_t)(uintptr_t)_kernel_end - reloc_delta);
        reserve_range((uint64_t)(uintptr_t)__kimage_start, (uint64_t)(uintptr_t)_kernel_end);
    } else {
        reserve_range(0, (uint64_t)(uintptr_t)_kernel_end);
    }

    // Also reserve any Multiboot2 module -- grub.cfg has none as of the
    // ELF64-binaries-to-/bin migration (see docs/decisions.md), so this
    // loop is a no-op today, but kept as real infrastructure rather
    // than deleted: any `module2` line placed a module somewhere in
    // physical memory GRUB picked, which is NOT necessarily covered by
    // the kernel image's own range above, and missing this would let
    // pmm_alloc_frame() hand out a module's own memory to whoever asks
    // first, silently corrupting it out from under anyone still reading
    // it (see elf.c, and the changelog entry this bug is documented
    // under). Reserves however many modules exist, not just the first.
    for (int i = 0; ; i++) {
        struct multiboot_module_info mod;
        if (!multiboot_get_module(i, &mod)) break;
        reserve_range(mod.start, mod.end);
    }

    // Also reserve the Multiboot2 INFO STRUCTURE itself (the tag list
    // multiboot_get_module()/multiboot_mmap_foreach() etc. all read) --
    // a different region from any modules' own content above, and one
    // that stays alive for the whole boot. Found the hard way, back
    // when M16's scheduler still spawned processes via
    // multiboot_get_module() (see scheduler.c's spawn_from_fs() now --
    // it reads from the persistent filesystem instead, see
    // docs/decisions.md): without reserving this range, spawning one
    // ring-3 process could hand out this exact range via
    // pmm_alloc_frame() (e.g. for that process's user stack), and the
    // very next multiboot_get_module() call -- looking up a LATER
    // module index -- would read corrupted tag data and silently fail,
    // even though the module was really there. Kept reserved regardless
    // of whether any modules exist today, same "real infrastructure,
    // not deleted" reasoning as the loop above.
    uint64_t info_start, info_end;
    if (multiboot_get_info_range(&info_start, &info_end)) {
        reserve_range(info_start, info_end);
    }

    free_frames = 0;
    for (uint64_t f = 0; f < PMM_MAX_FRAMES; f++) {
        if (!bit_is_used(f)) free_frames++;
    }

    // See bootstage.h: allocating before this point returns 0, which
    // every caller reports as "out of memory" -- a driver probing too
    // early therefore looks like a device or a memory problem. Marked
    // at the end of pmm_init() itself so it cannot drift.
    boot_subsystem_up(BOOT_SUB_PMM);
}

uint64_t pmm_alloc_frame(void) {
    BOOT_REQUIRE(BOOT_SUB_PMM);
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
    BOOT_REQUIRE(BOOT_SUB_PMM);
    uint64_t f = phys_addr / FRAME_SIZE;
    if (f >= PMM_MAX_FRAMES) return;
    if (bit_is_used(f)) {
        mark_free_bit(f);
        free_frames++;
    }
}

// See pmm.h's doc comment -- linear scan for a run of `count`
// consecutive free bits, the same bitmap pmm_alloc_frame() uses. Always
// scans from frame 0 (not alloc_hint) since this is a rare, not-hot-path
// call where simplicity matters more than skipping already-scanned
// ground -- unlike pmm_alloc_frame()'s hint, which earns its keep by
// running on every single-frame allocation.
uint64_t pmm_alloc_contiguous(uint64_t count) {
    BOOT_REQUIRE(BOOT_SUB_PMM);
    if (count == 0) return 0;
    if (count == 1) return pmm_alloc_frame(); // fast path, identical to before this existed

    uint64_t run_start = 0;
    uint64_t run_len = 0;
    for (uint64_t f = 0; f < PMM_MAX_FRAMES; f++) {
        if (!bit_is_used(f)) {
            if (run_len == 0) run_start = f;
            run_len++;
            if (run_len == count) {
                for (uint64_t i = 0; i < count; i++) mark_used_bit(run_start + i);
                free_frames -= count;
                alloc_hint = run_start + count; // keep pmm_alloc_frame()'s hint sensible too
                return run_start * FRAME_SIZE;
            }
        } else {
            run_len = 0;
        }
    }
    return 0; // no run of `count` contiguous free frames anywhere in range
}

void pmm_free_contiguous(uint64_t phys_addr, uint64_t count) {
    BOOT_REQUIRE(BOOT_SUB_PMM);
    uint64_t f = phys_addr / FRAME_SIZE;
    for (uint64_t i = 0; i < count; i++) {
        if (f + i >= PMM_MAX_FRAMES) break;
        if (bit_is_used(f + i)) {
            mark_free_bit(f + i);
            free_frames++;
        }
    }
}

// Is this physical address one pmm accounts for at all? False for MMIO
// and anything the firmware never reported as available RAM -- a
// framebuffer, most obviously. An auditor has to tell "not mine" from
// "mine and free": the first is normal, the second is a live mapping
// pointing at memory the allocator is free to hand out.
int pmm_frame_is_managed(uint64_t phys_addr) {
    uint64_t f = phys_addr / FRAME_SIZE;
    if (f >= PMM_MAX_FRAMES) return 0;
    // A frame inside a region the firmware reported as available is
    // accounted for whether it is currently allocated or not; one
    // outside every such region is not pmm's business. `managed` is
    // recorded at init because the bitmap alone cannot answer it -- an
    // unmanaged frame and an allocated one are both a set bit.
    return (managed[f / 8] & (1u << (f % 8))) != 0;
}

// Does pmm consider this frame handed out? Only meaningful for a frame
// pmm_frame_is_managed() claims.
int pmm_frame_is_used(uint64_t phys_addr) {
    uint64_t f = phys_addr / FRAME_SIZE;
    if (f >= PMM_MAX_FRAMES) return 1; // outside the bitmap: never "free"
    return bit_is_used(f);
}

uint64_t pmm_frame_size(void) { return FRAME_SIZE; }

uint64_t pmm_total_frames(void) { return total_frames; }
uint64_t pmm_free_frames(void) { return free_frames; }

// See pmm.h's doc comment. Deliberately checks both the returned address
// and the underlying bitmap/free_frames bookkeeping directly (not just
// "did it return nonzero") -- a bug that marks the wrong frames used, or
// gets the free_frames count wrong, would still return a plausible
// address and pass a shallower check.
int pmm_selftest(void) {
    uint64_t before = free_frames;

    uint64_t base = pmm_alloc_contiguous(4);
    if (base == 0) {
        klog_write("toy-os: PMM SELFTEST FAILED -- pmm_alloc_contiguous(4) returned 0\n");
        return 0; // failure -- see the message above
    }
    if (base % FRAME_SIZE != 0) {
        klog_write("toy-os: PMM SELFTEST FAILED -- unaligned address from pmm_alloc_contiguous\n");
        return 0; // failure -- see the message above
    }

    uint64_t f = base / FRAME_SIZE;
    for (uint64_t i = 0; i < 4; i++) {
        if (!bit_is_used(f + i)) {
            klog_write("toy-os: PMM SELFTEST FAILED -- frame not marked used after alloc\n");
            return 0; // failure -- see the message above
        }
    }
    if (free_frames != before - 4) {
        klog_write("toy-os: PMM SELFTEST FAILED -- free_frames count wrong after alloc\n");
        return 0; // failure -- see the message above
    }

    pmm_free_contiguous(base, 4);
    for (uint64_t i = 0; i < 4; i++) {
        if (bit_is_used(f + i)) {
            klog_write("toy-os: PMM SELFTEST FAILED -- frame still marked used after free\n");
            return 0; // failure -- see the message above
        }
    }
    if (free_frames != before) {
        klog_write("toy-os: PMM SELFTEST FAILED -- free_frames count wrong after free\n");
        return 0; // failure -- see the message above
    }

    // A second alloc of the same size should land back at the same
    // address -- proof the free above actually cleared those bits,
    // rather than just leaving the count right by accident.
    uint64_t base2 = pmm_alloc_contiguous(4);
    if (base2 != base) {
        klog_write("toy-os: PMM SELFTEST FAILED -- reuse after free landed at a different address\n");
        pmm_free_contiguous(base2, 4);
        return 0; // failure -- see the message above
    }
    pmm_free_contiguous(base2, 4);

    klog_write("toy-os: PMM contiguous-allocation self-test passed\n");
    return 1;
}
