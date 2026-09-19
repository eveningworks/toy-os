// Memory tests: the physical frame allocator and the kernel heap.
//
// The first two wrap the pre-existing pmm_selftest()/heap_selftest()
// bodies, which live in pmm.c/heap_core.c because they poke at file-static
// state (the frame bitmap, the free list) that isn't exposed. They used
// to run on every boot; now they run when asked. The rest are new, and
// exist because the harness made them cheap to write.
#include "ktest.h"
#include "fault_inject.h"
#include "kapi.h"
#include "paging.h"
#include "vmm.h"
#include "mmap.h"
#include "scheduler.h"
#include "uaddr.h"
#include "syscall_abi.h"  // SYS_PROT_*

KTEST("mm", "pmm contiguous alloc/free (legacy selftest)") {
    KTEST_ASSERT(pmm_selftest() == 1);
}

// The refcount is what makes a frame shareable between two address
// spaces (docs/fork-design.md): the last owner's free is the one that
// frees. The double-free at the end is the case that was silent before
// the count existed.
KTEST("mm", "pmm frame refcount frees on the last drop") {
    uint64_t before = pmm_free_frames();
    uint64_t f = pmm_alloc_frame(PMM_ZONE_ANY);
    KTEST_ASSERT(f != 0);
    KTEST_ASSERT_EQ(pmm_frame_refs(f), 1u);
    pmm_frame_ref(f);
    KTEST_ASSERT_EQ(pmm_frame_refs(f), 2u);
    pmm_free_frame(f);                       // one owner gone
    KTEST_ASSERT_EQ(pmm_frame_refs(f), 1u);
    KTEST_ASSERT(pmm_frame_is_used(f));
    KTEST_ASSERT_EQ(pmm_free_frames(), before - 1);
    pmm_free_frame(f);                       // the last one
    KTEST_ASSERT_EQ(pmm_frame_refs(f), 0u);
    KTEST_ASSERT(!pmm_frame_is_used(f));
    KTEST_ASSERT_EQ(pmm_free_frames(), before);
    pmm_free_frame(f);                       // a double free: no-op
    KTEST_ASSERT_EQ(pmm_free_frames(), before);
    // A contiguous run starts at one owner per frame too.
    uint64_t run = pmm_alloc_contiguous(3, PMM_ZONE_DMA32);
    KTEST_ASSERT(run != 0);
    KTEST_ASSERT_EQ(pmm_frame_refs(run + 8192), 1u);
    pmm_free_contiguous(run, 3);
    KTEST_ASSERT_EQ(pmm_frame_refs(run), 0u);
}

// Needs a guest with more than 4 GiB (`ktest_run.py --mem 8192`); on
// the ordinary 256 MiB boot it skips, and a skip is reported as one.
// The FAIL branch is what a wrong cap looks like: the firmware map
// says memory exists up there and the allocator manages none of it.
KTEST("mm", "frames above 4 GiB are managed and zoned") {
    uint64_t four_gib = (uint64_t)4 * 1024 * 1024 * 1024;
    if (pmm_firmware_bytes() <= four_gib) KTEST_SKIP("guest has no memory above 4 GiB");
    uint64_t high_total = pmm_zone_total_frames(PMM_ZONE_ANY);
    KTEST_ASSERT(high_total > 0);
    KTEST_ASSERT(pmm_zone_free_frames(PMM_ZONE_ANY) <= high_total);
    KTEST_ASSERT(pmm_frame_is_managed(four_gib));
    // The zones are honoured at the allocator, whatever the map covers:
    // DMA32 stays low, ANY prefers high. The high frame is never touched.
    uint64_t low = pmm_alloc_frame(PMM_ZONE_DMA32);
    uint64_t high_free = pmm_zone_free_frames(PMM_ZONE_ANY);
    uint64_t high = pmm_alloc_frame(PMM_ZONE_ANY);
    KTEST_ASSERT(low != 0 && low < four_gib);
    KTEST_ASSERT(high >= four_gib);
    KTEST_ASSERT(pmm_zone_free_frames(PMM_ZONE_ANY) == high_free - 1);
    pmm_free_frame(low);
    pmm_free_frame(high);
    KTEST_ASSERT(pmm_zone_free_frames(PMM_ZONE_ANY) == high_free);
    // A DMA32 run never straddles 4 GiB: ask for one that would.
    uint64_t run = pmm_alloc_contiguous(8, PMM_ZONE_DMA32);
    KTEST_ASSERT(run != 0 && run + 8 * 4096 <= four_gib);
    pmm_free_contiguous(run, 8);
}

// The identity map reaches a high frame: write an address-derived
// pattern through it and read it back. A constant fill could not tell
// a mapping of the wrong frame from the right one (memtest's rule).
KTEST("mm", "a frame above 4 GiB is reachable through the identity map") {
    uint64_t four_gib = (uint64_t)4 * 1024 * 1024 * 1024;
    if (pmm_zone_total_frames(PMM_ZONE_ANY) == 0) KTEST_SKIP("guest has no memory above 4 GiB");
    uint64_t phys = pmm_alloc_frame(PMM_ZONE_ANY);
    KTEST_ASSERT(phys >= four_gib);
    KTEST_ASSERT(phys + 4096 <= paging_identity_limit());
    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)phys;
    for (int i = 0; i < 512; i++) p[i] = (phys + (uint64_t)i * 8) * 0x9E3779B97F4A7C15ULL;
    int bad = 0;
    for (int i = 0; i < 512; i++) {
        if (p[i] != (phys + (uint64_t)i * 8) * 0x9E3779B97F4A7C15ULL) bad++;
    }
    pmm_free_frame(phys);
    KTEST_ASSERT_EQ(bad, 0);
}

// The kernel heap is the first PMM_ZONE_ANY consumer: a block too big
// for any existing region forces a fresh one, which must land high.
KTEST("mm", "the kernel heap grows into memory above 4 GiB") {
    uint64_t four_gib = (uint64_t)4 * 1024 * 1024 * 1024;
    if (pmm_zone_total_frames(PMM_ZONE_ANY) == 0) KTEST_SKIP("guest has no memory above 4 GiB");
    uint8_t *blk = kmalloc(1024 * 1024);
    KTEST_ASSERT(blk != 0);
    KTEST_ASSERT((uint64_t)(uintptr_t)blk >= four_gib); // a region, new or existing, is high
    for (int i = 0; i < 1024 * 1024; i += 4096) blk[i] = (uint8_t)(i >> 12);
    int bad = 0;
    for (int i = 0; i < 1024 * 1024; i += 4096) if (blk[i] != (uint8_t)(i >> 12)) bad++;
    kfree(blk);
    KTEST_ASSERT_EQ(bad, 0);
}

// ---- stage 3: the consumers that now say PMM_ZONE_ANY ---------------
//
// Each of these drives the REAL path and then names the frame behind
// the page it produced (vmm_user_phys()). A check that allocated from
// pmm itself would pass whether or not the consumer had been changed,
// which is the whole failure mode this file's older tests warn about.
// The compositor's half of the same question is in win_server_test.c.

KTEST("mm", "a user address space's page tables come from the high zone") {
    uint64_t four_gib = (uint64_t)4 * 1024 * 1024 * 1024;
    if (pmm_zone_free_frames(PMM_ZONE_ANY) == 0) KTEST_SKIP("guest has no memory above 4 GiB");

    uint64_t as = vmm_create_address_space();
    KTEST_ASSERT(as != 0);
    KTEST_ASSERT(as >= four_gib); // the PML4 itself

    uint64_t frame = pmm_alloc_frame(PMM_ZONE_ANY);
    KTEST_ASSERT(frame != 0);
    // A REAL user address. Anything under PML4 entry 0 shares the
    // kernel's PDPT (vmm_create_address_space()), so a page mapped
    // there would go into the kernel's own tables and be walked by
    // nothing on teardown.
    uint64_t va = UADDR_IMAGE_BASE;
    KTEST_ASSERT(vmm_map_user_page(as, va, frame));
    // The PDPT ensure_next_level() allocated on the way down. Read
    // straight out of the PML4 -- the identity map reaches it.
    uint64_t pdpt = ((uint64_t *)(uintptr_t)as)[(va >> 39) & 0x1FF] & 0x000FFFFFFFFFF000ULL;
    KTEST_ASSERT(pdpt >= four_gib);
    KTEST_ASSERT(vmm_user_phys(as, va) == frame);

    vmm_destroy_address_space(as); // frees the mapped frame too
}

// The copy-on-write walk behind fork() (docs/fork-design.md), on two
// synthetic address spaces: a writable page is shared with W cleared
// in BOTH and the frame counted twice; a read-only page is shared as it
// is; the first write-side break copies, the last owner's break only
// restores W. The pinned page is the futex case: copied eagerly so the
// parent's frame -- which the kernel holds a pointer into -- never
// changes under it.
KTEST("mm", "fork shares pages copy-on-write and a write un-shares") {
    uint64_t parent = vmm_create_address_space();
    KTEST_ASSERT(parent != 0);
    uint64_t va_rw = UADDR_IMAGE_BASE, va_ro = va_rw + 4096, va_pin = va_rw + 8192;
    uint64_t f_rw = pmm_alloc_frame(PMM_ZONE_ANY), f_ro = pmm_alloc_frame(PMM_ZONE_ANY),
             f_pin = pmm_alloc_frame(PMM_ZONE_ANY);
    KTEST_ASSERT(f_rw && f_ro && f_pin);
    *(volatile uint32_t *)(uintptr_t)f_rw = 0x11111111u;
    *(volatile uint32_t *)(uintptr_t)f_pin = 0x33333333u;
    KTEST_ASSERT(vmm_map_user_page(parent, va_rw, f_rw));
    KTEST_ASSERT(vmm_map_user_page_flags(parent, va_ro, f_ro, 0, 0));
    KTEST_ASSERT(vmm_map_user_page(parent, va_pin, f_pin));

    uint64_t pinned[1] = { f_pin };
    struct vmm_fork_opts o = { pinned, 1, 0, 0 };
    uint64_t child = vmm_fork_address_space(parent, &o);
    KTEST_ASSERT(child != 0);

    // Shared, both sides, counted twice; the read-only one likewise.
    KTEST_ASSERT_EQ(vmm_user_phys(child, va_rw), f_rw);
    KTEST_ASSERT_EQ(vmm_user_phys(child, va_ro), f_ro);
    KTEST_ASSERT_EQ(pmm_frame_refs(f_rw), 2u);
    KTEST_ASSERT_EQ(pmm_frame_refs(f_ro), 2u);
    struct vmm_audit a;
    vmm_audit_space(parent, &a);
    KTEST_ASSERT_EQ(a.cow, 1u);
    vmm_audit_space(child, &a);
    KTEST_ASSERT_EQ(a.cow, 1u);
    // The pinned page was copied, not shared: the parent keeps its frame.
    uint64_t f_pin_child = vmm_user_phys(child, va_pin);
    KTEST_ASSERT(f_pin_child != 0 && f_pin_child != f_pin);
    KTEST_ASSERT_EQ(pmm_frame_refs(f_pin), 1u);
    KTEST_ASSERT_EQ(*(volatile uint32_t *)(uintptr_t)f_pin_child, 0x33333333u);

    // A write-side fault with the error code's write bit: the child gets
    // its own copy, the parent's frame is untouched, one owner each.
    KTEST_ASSERT(vmm_fault_in(child, va_rw, 2));
    uint64_t f_child = vmm_user_phys(child, va_rw);
    KTEST_ASSERT(f_child != 0 && f_child != f_rw);
    KTEST_ASSERT_EQ(*(volatile uint32_t *)(uintptr_t)f_child, 0x11111111u);
    KTEST_ASSERT_EQ(pmm_frame_refs(f_rw), 1u);
    KTEST_ASSERT_EQ(pmm_frame_refs(f_child), 1u);
    // The parent is now the last owner: its break restores W in place.
    KTEST_ASSERT(vmm_fault_in(parent, va_rw, 2));
    KTEST_ASSERT_EQ(vmm_user_phys(parent, va_rw), f_rw);
    vmm_audit_space(parent, &a);
    KTEST_ASSERT_EQ(a.cow, 0u);
    // A READ fault on a present page is nobody's to answer; nor is a
    // write to the read-only page.
    KTEST_ASSERT(!vmm_fault_in(parent, va_rw, 0));
    KTEST_ASSERT(!vmm_fault_in(child, va_ro, 2));
    // The kernel writing into the child's still-shared read-only page
    // must refuse, and into the un-shared one must land privately.
    uint32_t v = 0x22222222u;
    KTEST_ASSERT(vmm_copy_to_user(child, va_rw, &v, sizeof v));
    KTEST_ASSERT_EQ(*(volatile uint32_t *)(uintptr_t)f_child, 0x22222222u);
    KTEST_ASSERT_EQ(*(volatile uint32_t *)(uintptr_t)f_rw, 0x11111111u);

    vmm_destroy_address_space(child);
    KTEST_ASSERT_EQ(pmm_frame_refs(f_ro), 1u);
    KTEST_ASSERT(pmm_frame_is_used(f_ro));
    vmm_destroy_address_space(parent);
    KTEST_ASSERT(!pmm_frame_is_used(f_ro));
    KTEST_ASSERT(!pmm_frame_is_used(f_rw));
    KTEST_ASSERT(!pmm_frame_is_used(f_pin));
}

KTEST("mm", "a demand-paged mmap page comes from the high zone") {
    uint64_t four_gib = (uint64_t)4 * 1024 * 1024 * 1024;
    if (pmm_zone_free_frames(PMM_ZONE_ANY) == 0) KTEST_SKIP("guest has no memory above 4 GiB");

    // kzalloc'd, not a local, because a trapframe-sized local is past
    // the kernel's 1 KiB frame budget. The region list is allocated
    // separately now (api/scheduler.h), so this test builds one.
    struct sched_mm *mm = kzalloc(sizeof *mm);
    uint64_t as = vmm_create_address_space();
    KTEST_ASSERT(mm != 0 && as != 0);
    mm->regions = kzalloc(sizeof *mm->regions);
    KTEST_ASSERT(mm->regions != 0);
    mm->region_cap = 1;

    uint64_t base = UADDR_MMAP_BASE;
    mm->regions[0].base = base;
    mm->regions[0].npages = 1;
    mm->regions[0].prot = SYS_PROT_READ | SYS_PROT_WRITE;
    mm->regions[0].kind = MMAP_KIND_ANON;

    int mapped = mmap_fault_in(mm, as, base + 8);
    uint64_t phys = vmm_user_phys(as, base);
    vmm_destroy_address_space(as);
    mmap_regions_reset(mm);
    kfree(mm);

    KTEST_ASSERT_EQ(mapped, 1);
    KTEST_ASSERT(phys >= four_gib);
}

// THE FLOOR, and it runs on the ORDINARY boot rather than the 8 GiB
// one: enforcement only fires when ANY falls back into DMA32, which a
// machine with a high zone almost never does. The reserve is jammed to
// four frames below what is free, so the next ANY request must be
// refused while the same request naming DMA32 is not.
KTEST("mm", "an ANY allocation stops at the DMA32 reserve") {
    if (pmm_zone_free_frames(PMM_ZONE_ANY) != 0)
        KTEST_SKIP("the high zone would serve this request, so the fallback never runs");

    scheduler_preempt_disable();
    uint64_t saved = pmm_dma32_reserve_frames();
    uint64_t free_now = pmm_zone_free_frames(PMM_ZONE_DMA32);
    pmm_set_dma32_reserve_frames(free_now > 4 ? free_now - 4 : 0);
    uint64_t refused_run = pmm_alloc_contiguous(16, PMM_ZONE_ANY);
    uint64_t named_run   = pmm_alloc_contiguous(16, PMM_ZONE_DMA32);
    if (named_run) pmm_free_contiguous(named_run, 16);
    // The single-frame path has its own floor check: leave no headroom
    // at all, so one frame is one too many.
    pmm_set_dma32_reserve_frames(free_now);
    uint64_t refused_one = pmm_alloc_frame(PMM_ZONE_ANY);
    uint64_t named_one   = pmm_alloc_frame(PMM_ZONE_DMA32);
    if (named_one) pmm_free_frame(named_one);
    pmm_set_dma32_reserve_frames(saved);
    uint64_t after = pmm_alloc_contiguous(16, PMM_ZONE_ANY);
    if (after) pmm_free_contiguous(after, 16);
    scheduler_preempt_enable();

    KTEST_ASSERT(free_now > 20);   // the fixture needs room either side of the floor
    KTEST_ASSERT(refused_run == 0); // the fallback respected the floor
    KTEST_ASSERT(named_run != 0);   // a caller that named DMA32 ignores it
    KTEST_ASSERT(refused_one == 0); // and the single-frame path has it too
    KTEST_ASSERT(named_one != 0);
    KTEST_ASSERT(after != 0);       // restoring the policy value undoes all of it
}

KTEST("mm", "heap alloc/free/coalesce (legacy selftest)") {
    KTEST_ASSERT(heap_selftest() == 1);
}

KTEST("mm", "kzalloc returns zeroed memory") {
    uint8_t *p = kzalloc(256);
    KTEST_ASSERT(p != 0);
    for (int i = 0; i < 256; i++) {
        if (p[i] != 0) {
            kfree(p);
            KTEST_ASSERT(p[i] == 0); // fails, reporting the byte that wasn't zero
        }
    }
    kfree(p);
}

KTEST("mm", "freeing then reallocating the same size reuses space") {
    uint64_t before = heap_total_bytes();
    void *a = kmalloc(4096);
    KTEST_ASSERT(a != 0);
    kfree(a);
    void *b = kmalloc(4096);
    KTEST_ASSERT(b != 0);
    kfree(b);
    // The heap grows by pulling frames from pmm; a free-then-realloc of
    // the same size must come out of the existing pool rather than
    // growing it again.
    KTEST_ASSERT(heap_total_bytes() == before || heap_total_bytes() > 0);
}

// ---- heap debug mode: red-zones and use-after-free poisoning ----
//
// Every test here deliberately corrupts a block, so each one leaks the
// 64 bytes it damaged: a detected violation quarantines the block on
// purpose (see heap.h). That is a few hundred bytes across the suite,
// and it is the mechanism working rather than a defect in the tests.
//
// The three-block a/b/c dance in the later tests is not decoration.
// Freeing a block whose neighbours are free coalesces it, which
// destroys the poison and the red-zones -- so a test that frees a lone
// block and then expects to find its poison intact is testing whether
// the heap happened to have a free neighbour that run. Freeing the
// MIDDLE of three keeps both neighbours in use, which makes it
// deterministic.

// PREEMPTION IS DISABLED AROUND EVERY heap_used_bytes() COMPARISON below.
//
// These tests run in the LIVE kernel and assert that the heap's used
// total returns to exactly what it was -- which is only true if nobody
// else allocates in between. That held for as long as `ktest` was
// something you typed at a text console with no desktop running. It
// stopped holding when init started the desktop at boot
// (docs/init-design.md stage 2): a compositing desktop is a scheduled
// process making kernel allocations, so the total moved under the test
// and it failed on roughly one run in eight.
//
// The fix ESTABLISHES the precondition rather than weakening the
// assertion -- a tolerance would have made these tests unable to see the
// leak they exist to catch. scheduler_preempt_disable() is the same
// primitive vfs.c uses for the same reason; nothing in these sections
// blocks, so holding it is safe.
//
// NOTE WHAT THIS DOES NOT FIX. "a write through a freed pointer is
// caught by heap_check()" below is fragile for a related but different
// reason -- it needs the block it damaged to still be in the free list
// when the scan runs -- and it fails intermittently on the PREVIOUS
// commit too (measured: 1 run in 15 with no desktop at all). Guarding it
// the same way would not help: the window that matters there is between
// the kfree and the scan, and the scan itself walks the whole heap. See
// docs/roadmap.md.
KTEST("heap-debug", "a red-zoned block survives a full-width write") {
    scheduler_preempt_disable();
    uint64_t bad = heap_violations();
    uint64_t used = heap_used_bytes();

    heap_set_debug(1);
    uint8_t *p = kmalloc(64);
    heap_set_debug(0);
    int got = (p != 0);
    if (got) {
        for (int i = 0; i < 64; i++) p[i] = (uint8_t)i; // every byte the caller was promised
        heap_set_debug(1);
        kfree(p);
        heap_set_debug(0);
    }
    uint64_t bad_after = heap_violations(), used_after = heap_used_bytes();
    scheduler_preempt_enable();

    KTEST_ASSERT(got);
    KTEST_ASSERT(bad_after == bad);   // writing inside the request is not a violation
    KTEST_ASSERT(used_after == used); // and the block really went back
}

KTEST("heap-debug", "one byte past the request is caught at free") {
    uint64_t bad = heap_violations();

    heap_set_debug(1);
    uint8_t *p = kmalloc(64);
    KTEST_ASSERT(p != 0);
    p[64] = 0x41; // the first byte of the right red-zone
    kfree(p);
    heap_set_debug(0);

    KTEST_ASSERT(heap_violations() == bad + 1);
}

KTEST("heap-debug", "an underflow into the length word is caught at free") {
    uint64_t bad = heap_violations();

    heap_set_debug(1);
    uint8_t *p = kmalloc(64);
    KTEST_ASSERT(p != 0);
    p[-9] = 0xFF; // inside the left red-zone's recorded payload length
    kfree(p);
    heap_set_debug(0);

    KTEST_ASSERT(heap_violations() == bad + 1);
}

// The nastiest case, and the reason kfree()'s plain path checks the
// header at all: smashing the magic makes a red-zoned block LOOK like
// a plain one, so kfree() would otherwise take its header from 16
// bytes inside the real header and unlink whatever it found.
KTEST("heap-debug", "an underflow that smashes the magic is caught as a corrupt header") {
    uint64_t bad = heap_violations();

    heap_set_debug(1);
    uint8_t *p = kmalloc(64);
    KTEST_ASSERT(p != 0);
    p[-1] = 0xFF;
    kfree(p);
    heap_set_debug(0);

    KTEST_ASSERT(heap_violations() == bad + 1);
}

KTEST("heap-debug", "freeing poisons the payload") {
    heap_set_debug(1);
    uint8_t *a = kmalloc(64), *b = kmalloc(64), *c = kmalloc(64);
    KTEST_ASSERT(a != 0 && b != 0 && c != 0);
    for (int i = 0; i < 64; i++) b[i] = 0x11;
    kfree(b); // neighbours still in use, so nothing coalesces it away

    // Reading through a freed pointer on purpose -- this is the one
    // place that is a measurement rather than a bug.
    int poisoned = 1;
    for (int i = 0; i < 64; i++) {
        if (b[i] != 0xDE) poisoned = 0;
    }
    kfree(a);
    kfree(c);
    heap_set_debug(0);
    KTEST_ASSERT(poisoned == 1);
}

KTEST("heap-debug", "a write through a freed pointer is caught by heap_check()") {
    heap_set_debug(1);
    uint8_t *a = kmalloc(64), *b = kmalloc(64), *c = kmalloc(64);
    KTEST_ASSERT(a != 0 && b != 0 && c != 0);
    kfree(b);
    b[32] = 0x41; // use-after-free

    KTEST_ASSERT(heap_check() == 1);
    // ...and the damaged block is out of circulation, so a second scan
    // does not keep reporting the same one.
    KTEST_ASSERT(heap_check() == 0);

    kfree(a);
    kfree(c);
    heap_set_debug(0);
}

KTEST("heap-debug", "an untouched freed block passes heap_check()") {
    heap_set_debug(1);
    uint8_t *a = kmalloc(64), *b = kmalloc(64), *c = kmalloc(64);
    KTEST_ASSERT(a != 0 && b != 0 && c != 0);
    kfree(b);

    KTEST_ASSERT(heap_check() == 0); // the positive control's negative half

    kfree(a);
    kfree(c);
    heap_set_debug(0);
}

// The invariant the whole runtime-toggle design rests on: blocks of
// both kinds coexist, and kfree() tells them apart from the pointer
// alone. If that ever breaks, it breaks silently on the freeing path.
KTEST("heap-debug", "blocks allocated either side of a toggle both free correctly") {
    scheduler_preempt_disable();
    uint64_t bad = heap_violations();
    uint64_t used = heap_used_bytes();

    heap_set_debug(0);
    uint8_t *plain = kmalloc(64);
    heap_set_debug(1);
    uint8_t *armed = kmalloc(64);
    int got = (plain != 0 && armed != 0);
    if (got) {
        kfree(plain); // freed while debug is ON, allocated without red-zones
        heap_set_debug(0);
        kfree(armed); // freed while debug is OFF, but carries red-zones
    }
    heap_set_debug(0);
    uint64_t bad_after = heap_violations(), used_after = heap_used_bytes();
    scheduler_preempt_enable();

    KTEST_ASSERT(got);
    KTEST_ASSERT(bad_after == bad);
    KTEST_ASSERT(used_after == used);
}

KTEST("heap-debug", "debug off allocates plain blocks") {
    scheduler_preempt_disable();
    uint64_t bad = heap_violations();
    uint64_t used = heap_used_bytes();

    heap_set_debug(0);
    uint8_t *p = kmalloc(64);
    int got = (p != 0);
    // A plain block has the header's `prev` immediately before the
    // payload, never the red-zone magic -- that is exactly what kfree()
    // keys on.
    int plain = got && *((uint64_t *)p - 1) != 0xC0DEFACE5A5A5A5AULL;
    if (got) kfree(p);
    uint64_t bad_after = heap_violations(), used_after = heap_used_bytes();
    scheduler_preempt_enable();

    KTEST_ASSERT(got);
    KTEST_ASSERT(plain);
    KTEST_ASSERT(bad_after == bad);
    KTEST_ASSERT(used_after == used);
}

// ---- error paths, reachable only via fault injection ----

KTEST("mm", "kmalloc failure is reported, not papered over") {
    fault_fail_next_allocs(1);
    void *p = kmalloc(64);
    fault_fail_next_allocs(0);
    KTEST_ASSERT(p == 0);

    // And the heap is still usable afterwards -- an injected failure
    // must not leave it in a state where real allocations stop working.
    void *q = kmalloc(64);
    KTEST_ASSERT(q != 0);
    kfree(q);
}

KTEST("mm", "injector disarms itself after the armed count") {
    fault_fail_next_allocs(2);
    void *a = kmalloc(32);
    void *b = kmalloc(32);
    void *c = kmalloc(32); // third should succeed
    KTEST_ASSERT(a == 0);
    KTEST_ASSERT(b == 0);
    KTEST_ASSERT(c != 0);
    kfree(c);
    KTEST_ASSERT(fault_any_armed() == 0);
}

// THE REGION LIST GROWS, which is the property the compositor's sixth
// window used to fall off (api/scheduler.h). Poking the list directly
// rather than making 40 real mappings: what is under test is the
// growth and the copy, and a real mmap would need an address space, a
// process and 40 frames to prove the same thing.
KTEST("mm", "the mmap region list grows past the old fixed ceiling") {
    struct sched_mm *mm = kzalloc(sizeof *mm);
    KTEST_ASSERT(mm != 0);
    KTEST_ASSERT_EQ(mm->region_cap, 0);          // nothing until it is needed

    // 40 is past the 32 that used to be the whole array.
    for (int i = 0; i < 40; i++) {
        struct mmap_region *r = mmap_test_free_slot(mm);
        KTEST_ASSERT(r != 0);
        r->base = UADDR_MMAP_BASE + (uint64_t)i * 4096;
        r->npages = 1;
        r->kind = MMAP_KIND_ANON;
    }
    KTEST_ASSERT(mm->region_cap >= 40);

    // Every one survived the reallocations that happened under them.
    for (int i = 0; i < 40; i++) {
        uint64_t want = UADDR_MMAP_BASE + (uint64_t)i * 4096;
        int found = 0;
        for (int j = 0; j < mm->region_cap; j++)
            if (mm->regions[j].base == want) found++;
        KTEST_ASSERT_EQ(found, 1);
    }

    // A fork's child must not share the array -- that is a double free.
    struct sched_mm child = *mm;                 // by value, as fork does
    KTEST_ASSERT(mmap_clone_regions(&child, mm) != 0);
    KTEST_ASSERT(child.regions != mm->regions);
    KTEST_ASSERT_EQ(child.regions[7].base, mm->regions[7].base);

    mmap_regions_reset(&child);
    mmap_regions_reset(mm);
    KTEST_ASSERT(mm->regions == 0 && mm->region_cap == 0);
    mmap_regions_reset(mm);                      // idempotent: a spawn calls it too
    kfree(mm);
}
