// Tests for the kernel's own W^X enforcement (paging_enforce_wx() in
// paging.c). These run in the live booted kernel against the live page
// tables -- there is no fixture and nothing to set up, which is the
// point: what they assert is the state the machine is actually running
// under right now, not a simulation of it.
//
// Why the assertions read page-table entries rather than trying the
// forbidden access: a kernel-mode write to read-only memory or a jump
// into NX memory is a page fault, and a page fault in ring 0 here ends
// the boot. There is no way to catch one and carry on, so "prove it
// faults" is a manual, reboot-required check (the same category as
// `ring3test`), not something a suite can run. Reading the bits back is
// the strongest thing that can be asserted without bricking the run --
// and it is not weak, because the bits ARE the mechanism.

#include "ktest.h"
#include "paging.h"
#include "pmm.h"
#include <stdint.h>

#define PAGE_PRESENT  (1ULL << 0)
#define PAGE_WRITABLE (1ULL << 1)
#define PAGE_NX       (1ULL << 63)

extern char __kimage_start[];
extern char __ktext_start[];
extern char __ktext_end[];
extern char __kdata_start[];

KTEST("paging", "no page in the kernel map is both writable and executable") {
    KTEST_ASSERT_EQ(paging_wx_violations(), 0);
}

KTEST("paging", ".text is executable and read-only") {
    // Both ends, not just one: an off-by-one in the split loop that
    // covered only the first page would pass a single-sample check.
    uint64_t first = paging_kernel_leaf((uint64_t)(uintptr_t)__ktext_start);
    uint64_t last  = paging_kernel_leaf((uint64_t)(uintptr_t)__ktext_end - 1);

    KTEST_ASSERT(first & PAGE_PRESENT);
    KTEST_ASSERT(!(first & PAGE_NX));
    KTEST_ASSERT(!(first & PAGE_WRITABLE));

    KTEST_ASSERT(last & PAGE_PRESENT);
    KTEST_ASSERT(!(last & PAGE_NX));
    KTEST_ASSERT(!(last & PAGE_WRITABLE));
}

KTEST("paging", ".rodata is read-only and not executable") {
    // __ktext_end is the first byte past .text, i.e. the first page of
    // the read-only-data band -- the page whose permissions would be
    // wrong if linker.ld ever lost its ALIGN(4096) between the two.
    uint64_t ro = paging_kernel_leaf((uint64_t)(uintptr_t)__ktext_end);
    KTEST_ASSERT(ro & PAGE_PRESENT);
    KTEST_ASSERT(ro & PAGE_NX);
    KTEST_ASSERT(!(ro & PAGE_WRITABLE));
}

KTEST("paging", ".boot is read-only and not executable") {
    uint64_t boot = paging_kernel_leaf((uint64_t)(uintptr_t)__kimage_start);
    KTEST_ASSERT(boot & PAGE_PRESENT);
    KTEST_ASSERT(boot & PAGE_NX);
    KTEST_ASSERT(!(boot & PAGE_WRITABLE));
}

KTEST("paging", ".data is writable and not executable") {
    // The other half of the assertion: W^X that made everything
    // read-only would pass every check above and boot into nothing.
    uint64_t data = paging_kernel_leaf((uint64_t)(uintptr_t)__kdata_start);
    KTEST_ASSERT(data & PAGE_PRESENT);
    KTEST_ASSERT(data & PAGE_NX);
    KTEST_ASSERT(data & PAGE_WRITABLE);
}

KTEST("paging", "the low 1MiB stays writable, and RAM above the image is NX") {
    // 0xB8000 is the VGA text buffer boot.asm's error path writes to;
    // making it read-only would break a path that only runs when
    // something has already gone wrong, i.e. the worst place for it.
    uint64_t vga = paging_kernel_leaf(0xB8000);
    KTEST_ASSERT(vga & PAGE_PRESENT);
    KTEST_ASSERT(vga & PAGE_WRITABLE);
    KTEST_ASSERT(vga & PAGE_NX);

    // A 2MiB slot well past the kernel image: still huge, still
    // writable, and NX -- this is the blanket that stops the heap, the
    // framebuffer and every stack from being executable.
    uint64_t far = paging_kernel_leaf(0x40000000); // 1GiB
    KTEST_ASSERT(far & PAGE_PRESENT);
    KTEST_ASSERT(far & PAGE_WRITABLE);
    KTEST_ASSERT(far & PAGE_NX);
}

KTEST("paging", "RAM above 4 GiB is mapped huge, writable and NX") {
    uint64_t four_gib = (uint64_t)4 * 1024 * 1024 * 1024;
    if (pmm_zone_total_frames(PMM_ZONE_ANY) == 0) KTEST_SKIP("guest has no memory above 4 GiB");
    KTEST_ASSERT(paging_identity_limit() > four_gib);
    // The first managed high frame's slot, not 4 GiB itself: a hole may
    // start there on a real machine.
    uint64_t probe = pmm_alloc_frame(PMM_ZONE_ANY);
    KTEST_ASSERT(probe >= four_gib);
    uint64_t e = paging_kernel_leaf(probe);
    pmm_free_frame(probe);
    KTEST_ASSERT(e & PAGE_PRESENT);
    KTEST_ASSERT(e & (1ULL << 7)); // huge
    KTEST_ASSERT(e & PAGE_WRITABLE);
    KTEST_ASSERT(e & PAGE_NX);
    // And nothing is mapped past the limit.
    KTEST_ASSERT_EQ((int64_t)paging_kernel_leaf(paging_identity_limit()), 0);
}

KTEST("paging", "CR0.WP is set, so ring 0 honours the read-only bit") {
    // Without this the read-only half of everything above is decorative:
    // a supervisor write ignores the read-write bit when WP is clear,
    // and the page tables still read exactly as they do now.
    uint64_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    KTEST_ASSERT(cr0 & (1ULL << 16));
}

// Write-combining is a 4 KiB decision: typing one frame must not type
// the frames beside it, which means splitting the 2 MiB page it sits
// in. Under TCG the type is ignored but the bits are what is asserted.
KTEST("paging", "write-combining a frame splits its huge page and leaves its neighbours cached") {
    uint64_t phys = pmm_alloc_contiguous(2, PMM_ZONE_DMA32);
    KTEST_ASSERT(phys != 0);
    int huge0 = paging_kernel_leaf_is_huge(phys + 4096);
    KTEST_ASSERT(huge0 >= 0);
    KTEST_ASSERT_EQ(PAGING_LEAF_WC(paging_kernel_leaf(phys + 4096), huge0), 0);
    int how = paging_set_write_combining(phys, 4096);
    if (how != PAGING_WC_PAT) {
        pmm_free_contiguous(phys, 2);
        KTEST_SKIP("write-combining is not PAT-backed here");
    }
    int huge_a = paging_kernel_leaf_is_huge(phys), huge_b = paging_kernel_leaf_is_huge(phys + 4096);
    KTEST_ASSERT_EQ(huge_a, 0);                                            // split
    KTEST_ASSERT_EQ(PAGING_LEAF_WC(paging_kernel_leaf(phys), huge_a), 1);  // the frame asked for
    KTEST_ASSERT_EQ(PAGING_LEAF_WC(paging_kernel_leaf(phys + 4096), huge_b), 0); // its neighbour
    KTEST_ASSERT_EQ(paging_clear_write_combining(phys, 4096), 1);
    KTEST_ASSERT_EQ(PAGING_LEAF_WC(paging_kernel_leaf(phys), paging_kernel_leaf_is_huge(phys)), 0);
    pmm_free_contiguous(phys, 2);
}
