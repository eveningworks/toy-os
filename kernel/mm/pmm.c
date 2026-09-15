// Bitmap-based physical frame allocator. One bit per 4KiB frame, 1 =
// used/reserved, 0 = free, plus a 16-bit owner count per frame so two
// address spaces can share one (docs/fork-design.md). Built once at
// boot from the Multiboot2 memory map (see multiboot.c) and never
// resized -- deliberately simple: correct and easy to reason about,
// not optimized.
//
// THE BITMAPS ARE SIZED FROM THE MEMORY MAP AND LIVE IN LOW RAM THEY
// RESERVE FOR THEMSELVES. They used to be two static 128 KiB arrays
// sized for 4 GiB, which capped what the allocator could even see. Now
// pmm_init() finds the highest usable address, carves both bitmaps out
// of the first low region that holds them clear of the image, the
// modules and the multiboot info, and marks that carve used -- so the
// allocator's own books are the first allocation it makes.
//
// ZONES: frames below 4 GiB are DMA32, the rest are the high zone.
// Both are managed; only the consumers decide what they can reach.
#include "pmm.h"
#include "bootstage.h"
#include "multiboot.h"
#include "klog.h"
#include "kfmt.h"
#include "reloc.h"
#include "uaddr.h"
#include "paging.h"
#include "string.h"
#include <stddef.h>

// Linker symbols from linker.ld: the running image's extent. With
// kernel ASLR, `_kernel_end - kernel_reloc_delta()` is the abandoned
// original's end, and that copy is still LIVE until gdt_init().
extern char _kernel_end[];
extern char __kimage_start[];

#define FRAME_SIZE 4096
#define DMA32_FRAMES ((uint64_t)4 * 1024 * 1024 * 1024 / FRAME_SIZE)
#define ZONE_COUNT 2

static uint8_t *bitmap;   // 1 = used or reserved
// Which frames pmm accounts for at all -- set once from the firmware
// map, never cleared. `bitmap` alone cannot tell an unmanaged frame
// (MMIO, a framebuffer) from an allocated one: both are a set bit.
static uint8_t *managed;
// Owners per frame, for a frame two address spaces share (copy-on-write
// after a fork, docs/fork-design.md). 1 for an ordinary allocation, 0
// for a free frame and for one reserved at boot -- the image and the
// books are "used, refcount 0" and can never be shared or freed.
static uint16_t *refs;
static uint64_t max_frames = 0;    // frames the bitmaps cover
static uint64_t bitmap_bytes = 0;  // per bitmap
static uint64_t zone_total[ZONE_COUNT]; // managed frames per zone
static uint64_t zone_free[ZONE_COUNT];  // allocatable per zone
static uint64_t zone_hint[ZONE_COUNT];  // avoids rescanning from the zone's start
static uint64_t firmware_bytes = 0;     // usable per the firmware map, uncapped
// The floor an ANY allocation will not drive DMA32 below, so a device
// that can only reach a low frame still finds one after user pages have
// taken everything above 4 GiB. Zero when there is no high zone: with
// nothing to fall back FROM, a floor there would only lose memory.
// Linux keeps the same kind of floor with lowmem_reserve_ratio.
static uint64_t dma32_reserve = 0;

// Index into the per-zone counters for a frame: the HIGH zone is index
// 1, which is also PMM_ZONE_ANY -- the two coincide on purpose, since a
// count "for ANY" has no meaning and a reader asking about frames
// above 4 GiB is what pmm_zone_total_frames(PMM_ZONE_ANY) answers.
static inline int zone_of(uint64_t frame) { return frame >= DMA32_FRAMES; }

static inline void mark_used_bit(uint64_t frame) {
    if (frame >= max_frames) return;
    bitmap[frame / 8] |= (uint8_t)(1u << (frame % 8));
}

static inline void mark_free_bit(uint64_t frame) {
    if (frame >= max_frames) return;
    bitmap[frame / 8] &= (uint8_t)~(1u << (frame % 8));
}

static inline int bit_is_used(uint64_t frame) {
    if (frame >= max_frames) return 1;
    return (bitmap[frame / 8] & (1u << (frame % 8))) != 0;
}

static inline uint64_t align_up(uint64_t v)   { return (v + FRAME_SIZE - 1) & ~((uint64_t)FRAME_SIZE - 1); }
static inline uint64_t align_down(uint64_t v) { return v & ~((uint64_t)FRAME_SIZE - 1); }

// ---- pass 1: how much is there? ----------------------------------

static uint64_t highest_usable = 0; // frame-aligned end of the last usable region

static void size_cb(const struct multiboot_mmap_region *region) {
    if (region->type != 1) return;
    uint64_t start = align_up(region->base);
    uint64_t end = align_down(region->base + region->length);
    if (end <= start) return;
    firmware_bytes += end - start;              // uncapped: what the machine HAS
    if (end > UADDR_KDEV_BASE) end = UADDR_KDEV_BASE; // the identity map can never reach past here
    if (end > highest_usable) highest_usable = end;
}

// ---- where do the bitmaps go? ------------------------------------

// Ranges pmm must not hand out and must not place its own bitmaps on.
// The image (both copies under ASLR), every Multiboot2 module (GRUB
// places one wherever it likes), and the multiboot info block itself
// (the tag list multiboot_*() keeps reading for the whole boot).
static int overlaps(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) {
    return a0 < b1 && b0 < a1;
}

// If [start, end) touches a reserved range, return 1 and set *bump to
// the first frame-aligned address past that range.
static int hits_reserved(uint64_t start, uint64_t end, uint64_t *bump) {
    uint64_t kend = (uint64_t)(uintptr_t)_kernel_end;
    uint64_t delta = kernel_reloc_delta();
    if (overlaps(start, end, 0, delta ? kend - delta : kend)) {
        *bump = align_up(delta ? kend - delta : kend);
        return 1;
    }
    if (delta && overlaps(start, end, (uint64_t)(uintptr_t)__kimage_start, kend)) {
        *bump = align_up(kend);
        return 1;
    }
    uint64_t s, e;
    if (multiboot_get_info_range(&s, &e) && overlaps(start, end, s, e)) {
        *bump = align_up(e);
        return 1;
    }
    for (int i = 0; ; i++) {
        struct multiboot_module_info mod;
        if (!multiboot_get_module(i, &mod)) break;
        if (overlaps(start, end, mod.start, mod.end)) {
            *bump = align_up(mod.end);
            return 1;
        }
    }
    return 0;
}

static uint64_t bitmap_home = 0;  // physical address of the books: both bitmaps, then the refcounts
static uint64_t bitmap_need = 0;  // bytes for all three

static void place_cb(const struct multiboot_mmap_region *region) {
    if (bitmap_home || region->type != 1) return;
    uint64_t start = align_up(region->base);
    uint64_t end = align_down(region->base + region->length);
    if (end > DMA32_FRAMES * FRAME_SIZE) end = DMA32_FRAMES * FRAME_SIZE; // must be identity-mapped
    if (start < 0x100000) start = 0x100000; // the legacy area stays reserved
    while (start + bitmap_need <= end) {
        uint64_t bump;
        if (!hits_reserved(start, start + bitmap_need, &bump)) {
            bitmap_home = start;
            return;
        }
        if (bump <= start) return; // cannot happen; refuse to spin
        start = bump;
    }
}

// ---- pass 2: the books --------------------------------------------

static void manage_range(uint64_t start, uint64_t end) {
    for (uint64_t addr = start; addr < end; addr += FRAME_SIZE) {
        uint64_t f = addr / FRAME_SIZE;
        managed[f / 8] |= (uint8_t)(1u << (f % 8));
        mark_free_bit(f);
        zone_total[zone_of(f)]++;
    }
}

static void mark_available_cb(const struct multiboot_mmap_region *region) {
    if (region->type != 1) return; // only "available" RAM
    uint64_t start = align_up(region->base);
    uint64_t end = align_down(region->base + region->length);
    if (end > max_frames * FRAME_SIZE) end = max_frames * FRAME_SIZE;
    uint64_t four_gib = DMA32_FRAMES * FRAME_SIZE;
    // Below 4 GiB every whole frame is managed. Above it only whole
    // 2 MiB granules are, because that is what paging_extend_identity_map()
    // maps -- a managed high frame must always be a mapped one.
    if (start < four_gib) manage_range(start, end < four_gib ? end : four_gib);
    if (end > four_gib) {
        uint64_t hs = start > four_gib ? start : four_gib;
        hs = (hs + PAGING_HUGE_SIZE - 1) & ~(PAGING_HUGE_SIZE - 1);
        uint64_t he = end & ~(PAGING_HUGE_SIZE - 1);
        if (he > hs) manage_range(hs, he);
    }
}

// A sixteenth of DMA32, clamped to [16 MiB, 128 MiB] and never more
// than half the zone. A fraction rather than a constant because the
// devices that need low memory scale with the machine, not with a
// number chosen here.
static uint64_t reserve_policy(uint64_t dma32_total, uint64_t high_total) {
    if (!high_total) return 0;
    const uint64_t lo_frames = 16ull * 1024 * 1024 / FRAME_SIZE;
    const uint64_t hi_frames = 128ull * 1024 * 1024 / FRAME_SIZE;
    uint64_t r = dma32_total / 16;
    if (r < lo_frames) r = lo_frames;
    if (r > hi_frames) r = hi_frames;
    if (r > dma32_total / 2) r = dma32_total / 2;
    return r;
}

uint64_t pmm_dma32_reserve_frames(void) { return dma32_reserve; }
void pmm_set_dma32_reserve_frames(uint64_t frames) { dma32_reserve = frames; }

static void reserve_range(uint64_t start, uint64_t end) {
    start = align_down(start);
    end = align_up(end);
    if (end > max_frames * FRAME_SIZE) end = max_frames * FRAME_SIZE;
    for (uint64_t addr = start; addr < end; addr += FRAME_SIZE) mark_used_bit(addr / FRAME_SIZE);
}

void pmm_init(void) {
    firmware_bytes = 0;
    highest_usable = 0;
    multiboot_mmap_foreach(size_cb);

    max_frames = highest_usable / FRAME_SIZE;
    bitmap_bytes = (max_frames + 7) / 8;
    bitmap_need = align_up(bitmap_bytes * 2 + max_frames * sizeof(uint16_t));
    bitmap_home = 0;
    multiboot_mmap_foreach(place_cb);
    if (!bitmap_home) {
        // No low region holds the books: nothing can be allocated, and
        // every caller reports that as out of memory. Say why once.
        klog_printf("pmm: no room for %lu-byte frame bitmaps below 4 GiB -- no memory managed\n",
                    bitmap_need);
        max_frames = 0;
        boot_subsystem_up(BOOT_SUB_PMM);
        return;
    }
    bitmap = (uint8_t *)(uintptr_t)bitmap_home;
    managed = bitmap + bitmap_bytes;
    refs = (uint16_t *)(bitmap + bitmap_bytes * 2);
    k_memset(bitmap, 0xFF, bitmap_bytes); // start fully reserved
    k_memset(managed, 0, bitmap_bytes);
    k_memset(refs, 0, max_frames * sizeof(uint16_t));
    for (int z = 0; z < ZONE_COUNT; z++) zone_total[z] = zone_free[z] = zone_hint[z] = 0;

    multiboot_mmap_foreach(mark_available_cb);

    // Reserve the image regardless of what the map says about that
    // range -- the firmware has no idea a kernel is sitting in it. Under
    // ASLR that is two ranges, reserved separately: [0, original end)
    // and the relocated copy, never the span between them.
    uint64_t reloc_delta = kernel_reloc_delta();
    if (reloc_delta) {
        reserve_range(0, (uint64_t)(uintptr_t)_kernel_end - reloc_delta);
        reserve_range((uint64_t)(uintptr_t)__kimage_start, (uint64_t)(uintptr_t)_kernel_end);
    } else {
        reserve_range(0, (uint64_t)(uintptr_t)_kernel_end);
    }
    // Every Multiboot2 module (none today -- see docs/decisions.md on
    // the move to /bin -- but a `module2` line lands wherever GRUB
    // chose), and the info block itself, which is read all boot long.
    for (int i = 0; ; i++) {
        struct multiboot_module_info mod;
        if (!multiboot_get_module(i, &mod)) break;
        reserve_range(mod.start, mod.end);
    }
    uint64_t info_start, info_end;
    if (multiboot_get_info_range(&info_start, &info_end)) reserve_range(info_start, info_end);
    // And the books themselves.
    reserve_range(bitmap_home, bitmap_home + bitmap_need);

    for (uint64_t f = 0; f < max_frames; f++) {
        if (!bit_is_used(f)) zone_free[zone_of(f)]++;
    }
    zone_hint[1] = DMA32_FRAMES;
    dma32_reserve = reserve_policy(zone_total[0], zone_total[1]);

    klog_printf("pmm: %lu frames managed (%lu above 4 GiB, %lu reserved for DMA32), "
                "books %lu KiB at 0x%lx\n",
                zone_total[0] + zone_total[1], zone_total[1], dma32_reserve,
                bitmap_need / 1024, bitmap_home);

    // See bootstage.h: allocating before this point returns 0, which
    // every caller reports as "out of memory". Marked at the end of
    // pmm_init() itself so it cannot drift.
    boot_subsystem_up(BOOT_SUB_PMM);
}

// The zone's frame range: DMA32 is [0, 4 GiB) clipped to what exists;
// the high zone is everything from 4 GiB up, and may be empty.
static void zone_range(int z, uint64_t *lo, uint64_t *hi) {
    if (z == 0) {
        *lo = 0;
        *hi = max_frames < DMA32_FRAMES ? max_frames : DMA32_FRAMES;
    } else {
        *lo = DMA32_FRAMES;
        *hi = max_frames > DMA32_FRAMES ? max_frames : DMA32_FRAMES;
    }
}

// `floor` is the free count the zone must still hold AFTER the
// allocation -- 0 for a caller that named this zone, the reserve for an
// ANY request falling back into it.
static uint64_t alloc_one_in(int z, uint64_t floor) {
    uint64_t lo, hi;
    zone_range(z, &lo, &hi);
    if (hi <= lo) return 0;
    if (zone_free[z] < floor + 1) return 0;
    uint64_t span = hi - lo;
    uint64_t start = zone_hint[z] >= lo && zone_hint[z] < hi ? zone_hint[z] : lo;
    for (uint64_t i = 0; i < span; i++) {
        uint64_t f = lo + (start - lo + i) % span;
        if (!bit_is_used(f)) {
            mark_used_bit(f);
            refs[f] = 1;
            zone_free[z]--;
            zone_hint[z] = f + 1;
            return f * FRAME_SIZE;
        }
    }
    return 0;
}

uint64_t pmm_alloc_frame(enum pmm_zone zone) {
    BOOT_REQUIRE(BOOT_SUB_PMM);
    // ANY tries the high zone first so DMA32 is kept for the callers
    // that have no choice; a machine with nothing up there falls
    // straight through.
    if (zone == PMM_ZONE_ANY) {
        uint64_t p = alloc_one_in(1, 0);
        if (p) return p;
        return alloc_one_in(0, dma32_reserve);
    }
    return alloc_one_in(0, 0);
}

// One owner lets go of frame `f`. The frame is freed only when the
// count reaches zero; a reserved frame (count 0, bit set) is freed as
// it always was, since nothing can have shared it.
//
// A FREE OF A FREE FRAME IS LOGGED, not just ignored: it is the shape a
// double free takes, and before the count existed it was silent -- the
// first of two owners returned a frame the second was still mapping.
// Capped, so a buggy loop cannot flood the ring.
static void free_one(uint64_t f) {
    if (f >= max_frames) return;
    if (!bit_is_used(f)) {
        static int reported;
        if (reported < 4) {
            reported++;
            klog_printf("pmm: free of frame %#lx, which is already free\n",
                        f * FRAME_SIZE);
        }
        return;
    }
    if (refs[f] > 1) { refs[f]--; return; }
    refs[f] = 0;
    mark_free_bit(f);
    zone_free[zone_of(f)]++;
}

void pmm_free_frame(uint64_t phys_addr) {
    BOOT_REQUIRE(BOOT_SUB_PMM);
    free_one(phys_addr / FRAME_SIZE);
}

void pmm_frame_ref(uint64_t phys_addr) {
    uint64_t f = phys_addr / FRAME_SIZE;
    if (f >= max_frames || !bit_is_used(f) || refs[f] == 0) return;
    if (refs[f] == 0xFFFF) return; // saturated: the frame is simply never freed
    refs[f]++;
}

unsigned pmm_frame_refs(uint64_t phys_addr) {
    uint64_t f = phys_addr / FRAME_SIZE;
    if (f >= max_frames || !bit_is_used(f)) return 0;
    return refs[f];
}

static uint64_t alloc_run_in(int z, uint64_t count, uint64_t floor) {
    uint64_t lo, hi;
    zone_range(z, &lo, &hi);
    if (zone_free[z] < floor + count) return 0;
    uint64_t run_start = 0, run_len = 0;
    for (uint64_t f = lo; f < hi; f++) {
        if (!bit_is_used(f)) {
            if (run_len == 0) run_start = f;
            if (++run_len == count) {
                for (uint64_t i = 0; i < count; i++) {
                    mark_used_bit(run_start + i);
                    refs[run_start + i] = 1;
                }
                zone_free[z] -= count;
                zone_hint[z] = run_start + count; // keep the single-frame hint sensible too
                return run_start * FRAME_SIZE;
            }
        } else {
            run_len = 0;
        }
    }
    return 0; // no run of `count` contiguous free frames in this zone
}

// A linear scan for a run, deliberately with no buddy structure (see
// pmm.h). count == 1 is the single-frame path with its hint.
uint64_t pmm_alloc_contiguous(uint64_t count, enum pmm_zone zone) {
    BOOT_REQUIRE(BOOT_SUB_PMM);
    if (count == 0) return 0;
    if (count == 1) return pmm_alloc_frame(zone);
    if (zone == PMM_ZONE_ANY) {
        uint64_t p = alloc_run_in(1, count, 0);
        if (p) return p;
        return alloc_run_in(0, count, dma32_reserve);
    }
    return alloc_run_in(0, count, 0);
}

void pmm_free_contiguous(uint64_t phys_addr, uint64_t count) {
    BOOT_REQUIRE(BOOT_SUB_PMM);
    uint64_t f = phys_addr / FRAME_SIZE;
    for (uint64_t i = 0; i < count; i++) {
        if (f + i >= max_frames) break;
        free_one(f + i);
    }
}

// Is this physical address one pmm accounts for at all? False for MMIO
// and anything the firmware never reported as available RAM. An
// auditor has to tell "not mine" from "mine and free": the second is a
// live mapping pointing at memory the allocator may hand out again.
int pmm_frame_is_managed(uint64_t phys_addr) {
    uint64_t f = phys_addr / FRAME_SIZE;
    if (f >= max_frames) return 0;
    return (managed[f / 8] & (1u << (f % 8))) != 0;
}

// Does pmm consider this frame handed out? Only meaningful for a frame
// pmm_frame_is_managed() claims.
int pmm_frame_is_used(uint64_t phys_addr) {
    return bit_is_used(phys_addr / FRAME_SIZE); // outside the bitmap: never "free"
}

uint64_t pmm_frame_size(void) { return FRAME_SIZE; }

uint64_t pmm_total_frames(void) { return zone_total[0] + zone_total[1]; }
uint64_t pmm_free_frames(void) { return zone_free[0] + zone_free[1]; }
uint64_t pmm_zone_total_frames(enum pmm_zone zone) { return zone_total[zone == PMM_ZONE_ANY]; }
uint64_t pmm_zone_free_frames(enum pmm_zone zone) { return zone_free[zone == PMM_ZONE_ANY]; }
uint64_t pmm_firmware_bytes(void) { return firmware_bytes; }

// See pmm.h's doc comment. Checks the bitmap and the free count
// directly, not just "did it return nonzero" -- a bug marking the wrong
// frames used would still return a plausible address.
int pmm_selftest(void) {
    uint64_t before = pmm_free_frames();

    uint64_t base = pmm_alloc_contiguous(4, PMM_ZONE_DMA32);
    if (base == 0) {
        klog_write(KLOG_ERR "toy-os: PMM SELFTEST FAILED -- pmm_alloc_contiguous(4) returned 0\n");
        return 0;
    }
    if (base % FRAME_SIZE != 0) {
        klog_write(KLOG_ERR "toy-os: PMM SELFTEST FAILED -- unaligned address from pmm_alloc_contiguous\n");
        return 0;
    }

    uint64_t f = base / FRAME_SIZE;
    for (uint64_t i = 0; i < 4; i++) {
        if (!bit_is_used(f + i)) {
            klog_write(KLOG_ERR "toy-os: PMM SELFTEST FAILED -- frame not marked used after alloc\n");
            return 0;
        }
    }
    if (pmm_free_frames() != before - 4) {
        klog_write(KLOG_ERR "toy-os: PMM SELFTEST FAILED -- free_frames count wrong after alloc\n");
        return 0;
    }

    pmm_free_contiguous(base, 4);
    for (uint64_t i = 0; i < 4; i++) {
        if (bit_is_used(f + i)) {
            klog_write(KLOG_ERR "toy-os: PMM SELFTEST FAILED -- frame still marked used after free\n");
            return 0;
        }
    }
    if (pmm_free_frames() != before) {
        klog_write(KLOG_ERR "toy-os: PMM SELFTEST FAILED -- free_frames count wrong after free\n");
        return 0;
    }

    // A second alloc of the same size lands back at the same address --
    // proof the free cleared those bits rather than just the count.
    uint64_t base2 = pmm_alloc_contiguous(4, PMM_ZONE_DMA32);
    if (base2 != base) {
        klog_write(KLOG_ERR "toy-os: PMM SELFTEST FAILED -- reuse after free landed at a different address\n");
        pmm_free_contiguous(base2, 4);
        return 0;
    }
    pmm_free_contiguous(base2, 4);

    klog_write("toy-os: PMM contiguous-allocation self-test passed\n");
    return 1;
}
