// Tests for the swap area (kernel/mm/swap.c).
//
// They run in a LIVE kernel with a real root mounted, so they must not
// point swap at the machine's own disk. Each one registers a scratch
// RAM block device, does its work, and PUTS THE REAL ONE BACK BEFORE
// ANY ASSERTION -- a failing KTEST_ASSERT returns from the test
// function, so an assert before the restore would leave the machine
// running on a 512 KiB RAM disk with no filesystem, taking every later
// test and the boot with it. Same discipline as partition_test.c, and
// preemption stays off across the swap.
#include "ktest.h"
#include "swap.h"
#include "block.h"
#include "heap.h"
#include "string.h"
#include "scheduler.h"
#include "vmm.h"
#include "pmm.h"
#include "uaddr.h"

// 16 pages: enough that "allocate until it runs out" is a short loop
// and the exhaustion bound is a real number rather than 1.
#define SCRATCH_BYTES (16 * SWAP_PAGE_SIZE)
#define SCRATCH_SLOTS (SCRATCH_BYTES / SWAP_PAGE_SIZE)

// A pattern that depends on the OFFSET, never a constant fill. A
// constant cannot tell a correct round trip from one that read back a
// different page, or from one that read nothing and found the buffer
// already holding the value.
static void fill_pattern(uint8_t *p, uint32_t len, uint32_t salt) {
    for (uint32_t i = 0; i < len; i++)
        p[i] = (uint8_t)((i * 31u) ^ salt ^ (i >> 8));
}

static int pattern_ok(const uint8_t *p, uint32_t len, uint32_t salt) {
    for (uint32_t i = 0; i < len; i++)
        if (p[i] != (uint8_t)((i * 31u) ^ salt ^ (i >> 8))) return 0;
    return 1;
}

// Everything each test needs to borrow the block device and give it
// back. `begin` returns 0 if the scratch could not be set up, in which
// case `end` is still safe to call.
struct scratch {
    void *mem;
    const struct block_device *saved;
    int registered;
};

static int scratch_begin(struct scratch *s) {
    s->mem = kmalloc(SCRATCH_BYTES);
    s->saved = 0;
    s->registered = 0;
    if (!s->mem) return 0;
    k_memset(s->mem, 0, SCRATCH_BYTES);
    scheduler_preempt_disable();
    s->saved = blk_active();
    // NOTHING TO PUT BACK IS A REASON NOT TO BORROW. On a diskless boot
    // there is no active device, and registering the scratch would make
    // it the machine's disk with no way to undo that -- one skipped
    // test against a kernel left running on a 64 KiB RAM disk.
    if (!s->saved) { scheduler_preempt_enable(); kfree(s->mem); s->mem = 0; return 0; }
    s->registered = blk_ram_register((uint64_t)(uintptr_t)s->mem, SCRATCH_BYTES);
    return s->registered;
}

static void scratch_end(struct scratch *s) {
    if (s->mem) {
        // FREE EVERY SLOT FIRST. swap_off() rightly refuses while any
        // page is still out, so a test that fails midway would leave
        // swap ON, pointed at memory about to be freed -- and every
        // later test in this file would then skip on "that device is in
        // use as swap" rather than run. A skipped test reports as a
        // pass, which is how a positive control here first appeared to
        // prove something it had not exercised at all.
        if (swap_active()) {
            uint32_t total = 0;
            swap_stats(&total, 0);
            for (uint32_t i = 1; i < total; i++) swap_slot_free(i);
            const char *why = "";
            swap_off(&why);
        }
        if (s->saved) blk_register(s->saved);
        scheduler_preempt_enable();
        kfree(s->mem);
        s->mem = 0;
    }
}

KTEST("swap", "swapon REFUSES a device that is not a swap area") {
    struct scratch s;
    if (!scratch_begin(&s)) { scratch_end(&s); KTEST_SKIP("no scratch disk"); }

    // The scratch is all zeroes -- no header, no magic. This is the
    // safety property of the whole file: `swapon` on the wrong name
    // must refuse, not format.
    const char *why = "";
    int accepted = swap_on(blk_active(), &why);
    int said_something = why && why[0];

    // ...and it must not have WRITTEN anything while refusing. Checked
    // rather than assumed, because "refused" and "refused after
    // scribbling on sector 0" look identical from the return value.
    int untouched = 1;
    const uint8_t *raw = s.mem;
    for (uint32_t i = 0; i < 512; i++) if (raw[i]) { untouched = 0; break; }

    scratch_end(&s);
    KTEST_ASSERT(!accepted);
    KTEST_ASSERT(said_something);
    KTEST_ASSERT(untouched);
}

KTEST("swap", "a header that is plausible but not OURS is refused") {
    struct scratch s;
    if (!scratch_begin(&s)) { scratch_end(&s); KTEST_SKIP("no scratch disk"); }

    // THE FIXTURE IS THE POINT HERE. The zeroed scratch above is
    // refused by the VERSION check before the magic is ever consulted,
    // so deleting the magic check entirely leaves that test green --
    // measured, not assumed. This one fills in every other field
    // correctly, so the magic is the only thing left that can say no.
    struct swap_header *h = (struct swap_header *)s.mem;
    k_memset(h, 0, sizeof *h);
    k_strlcpy(h->magic, "NOTSWAP", SWAP_MAGIC_N);
    h->version   = SWAP_VERSION;
    h->slots     = SCRATCH_SLOTS;
    h->page_size = SWAP_PAGE_SIZE;

    const char *why = "";
    int accepted = swap_on(blk_active(), &why);

    scratch_end(&s);
    KTEST_ASSERT(!accepted);
}

KTEST("swap", "mkswap then swapon reports the device's own page count") {
    struct scratch s;
    if (!scratch_begin(&s)) { scratch_end(&s); KTEST_SKIP("no scratch disk"); }

    const char *why = "";
    int formatted = swap_format(blk_active(), &why);
    int on = formatted ? swap_on(blk_active(), &why) : 0;
    uint32_t total = 0, used = 0;
    if (on) swap_stats(&total, &used);

    scratch_end(&s);
    KTEST_ASSERT(formatted);
    KTEST_ASSERT(on);
    KTEST_ASSERT_EQ(total, SCRATCH_SLOTS);
    // Slot 0 is the header and is charged from the moment swap is on,
    // which is what makes "used == total" mean genuinely full.
    KTEST_ASSERT_EQ(used, 1u);
}

KTEST("swap", "a page round-trips through a slot byte for byte") {
    struct scratch s;
    if (!scratch_begin(&s)) { scratch_end(&s); KTEST_SKIP("no scratch disk"); }

    uint8_t *out = kmalloc(SWAP_PAGE_SIZE);
    uint8_t *in  = kmalloc(SWAP_PAGE_SIZE);
    const char *why = "";
    int ready = out && in && swap_format(blk_active(), &why)
                         && swap_on(blk_active(), &why);

    uint32_t slot = 0;
    int wrote = 0, read_back = 0, matched = 0, in_was_different = 0;
    if (ready) {
        fill_pattern(out, SWAP_PAGE_SIZE, 0xA5);
        // The destination starts holding a DIFFERENT pattern, so a read
        // that does nothing at all cannot pass by leaving the right
        // bytes already in place.
        fill_pattern(in, SWAP_PAGE_SIZE, 0x5A);
        in_was_different = !pattern_ok(in, SWAP_PAGE_SIZE, 0xA5);

        slot = swap_slot_alloc();
        if (slot) {
            wrote = swap_write_page(slot, (uint64_t)(uintptr_t)out);
            if (wrote) read_back = swap_read_page(slot, (uint64_t)(uintptr_t)in);
            if (read_back) matched = pattern_ok(in, SWAP_PAGE_SIZE, 0xA5);
            swap_slot_free(slot);
        }
    }

    scratch_end(&s);
    if (out) kfree(out);
    if (in) kfree(in);
    KTEST_ASSERT(ready);
    KTEST_ASSERT(in_was_different);
    KTEST_ASSERT(slot != 0);
    KTEST_ASSERT(wrote);
    KTEST_ASSERT(read_back);
    KTEST_ASSERT(matched);
}

KTEST("swap", "two slots hold two different pages") {
    struct scratch s;
    if (!scratch_begin(&s)) { scratch_end(&s); KTEST_SKIP("no scratch disk"); }

    uint8_t *buf = kmalloc(SWAP_PAGE_SIZE);
    const char *why = "";
    int ready = buf && swap_format(blk_active(), &why) && swap_on(blk_active(), &why);

    uint32_t a = 0, b = 0;
    int distinct = 0, a_ok = 0, b_ok = 0;
    if (ready) {
        a = swap_slot_alloc();
        b = swap_slot_alloc();
        distinct = a && b && a != b;
        if (distinct) {
            // Written in one order and read back in the other, so a
            // slot_lba() that ignored the slot number -- every page
            // landing on top of the last -- fails rather than passing
            // on whichever page happened to be written last.
            fill_pattern(buf, SWAP_PAGE_SIZE, 0x11);
            swap_write_page(a, (uint64_t)(uintptr_t)buf);
            fill_pattern(buf, SWAP_PAGE_SIZE, 0x22);
            swap_write_page(b, (uint64_t)(uintptr_t)buf);

            k_memset(buf, 0, SWAP_PAGE_SIZE);
            if (swap_read_page(b, (uint64_t)(uintptr_t)buf))
                b_ok = pattern_ok(buf, SWAP_PAGE_SIZE, 0x22);
            k_memset(buf, 0, SWAP_PAGE_SIZE);
            if (swap_read_page(a, (uint64_t)(uintptr_t)buf))
                a_ok = pattern_ok(buf, SWAP_PAGE_SIZE, 0x11);
        }
        swap_slot_free(a);
        swap_slot_free(b);
    }

    scratch_end(&s);
    if (buf) kfree(buf);
    KTEST_ASSERT(ready);
    KTEST_ASSERT(distinct);
    KTEST_ASSERT(a_ok);
    KTEST_ASSERT(b_ok);
}

KTEST("swap", "slots run out at the device's capacity, and slot 0 is never one") {
    struct scratch s;
    if (!scratch_begin(&s)) { scratch_end(&s); KTEST_SKIP("no scratch disk"); }

    const char *why = "";
    int ready = swap_format(blk_active(), &why) && swap_on(blk_active(), &why);

    uint32_t handed_out = 0;
    int saw_zero = 0, exhausted = 0;
    uint32_t total = 0, used = 0;
    if (ready) {
        for (;;) {
            uint32_t slot = swap_slot_alloc();
            if (!slot) { exhausted = 1; break; }
            if (slot == 0) saw_zero = 1;
            handed_out++;
            if (handed_out > SCRATCH_SLOTS) break;   // runaway guard
        }
        swap_stats(&total, &used);
        for (uint32_t i = 1; i < total; i++) swap_slot_free(i);
    }

    scratch_end(&s);
    KTEST_ASSERT(ready);
    KTEST_ASSERT(exhausted);
    KTEST_ASSERT(!saw_zero);
    // Every slot but the header, and not one more.
    KTEST_ASSERT_EQ(handed_out, SCRATCH_SLOTS - 1);
    KTEST_ASSERT_EQ(used, total);
}

KTEST("swap", "swapoff REFUSES while a page is still out, and a double free is a no-op") {
    struct scratch s;
    if (!scratch_begin(&s)) { scratch_end(&s); KTEST_SKIP("no scratch disk"); }

    const char *why = "";
    int ready = swap_format(blk_active(), &why) && swap_on(blk_active(), &why);

    uint32_t slot = 0;
    int refused = 0, said_something = 0, allowed_after = 0;
    uint32_t used_after_double_free = 0;
    if (ready) {
        slot = swap_slot_alloc();
        why = "";
        refused = !swap_off(&why);
        said_something = why && why[0];

        swap_slot_free(slot);
        // Freeing it twice must not decrement the count a second time.
        // A slot count that drifts down shows up much later as a slot
        // handed out to two pages, which is the worst bug this file
        // could have and the hardest to trace back to here.
        swap_slot_free(slot);
        swap_stats(0, &used_after_double_free);

        why = "";
        allowed_after = swap_off(&why);
    }

    scratch_end(&s);
    KTEST_ASSERT(ready);
    KTEST_ASSERT(slot != 0);
    KTEST_ASSERT(refused);
    KTEST_ASSERT(said_something);
    KTEST_ASSERT_EQ(used_after_double_free, 1u);   // the header alone
    KTEST_ASSERT(allowed_after);
}

KTEST("swap", "a header claiming more slots than the device has is refused") {
    struct scratch s;
    if (!scratch_begin(&s)) { scratch_end(&s); KTEST_SKIP("no scratch disk"); }

    const char *why = "";
    int formatted = swap_format(blk_active(), &why);

    // Forge a bigger slot count straight into the image. The header is
    // DATA the device carries, so it can legitimately disagree with the
    // device -- a partition that shrank, an image copied onto a smaller
    // disk -- and believing it would index the bitmap past its end.
    int accepted = 1;
    if (formatted) {
        struct swap_header *h = (struct swap_header *)s.mem;
        h->slots = SCRATCH_SLOTS * 4;
        why = "";
        accepted = swap_on(blk_active(), &why);
    }

    scratch_end(&s);
    KTEST_ASSERT(formatted);
    KTEST_ASSERT(!accepted);
}

// ---- the PTE side (stage 0) -----------------------------------------
//
// These test vmm.c's swap ENTRY rather than swap.c's area, and they
// live here because every one of them needs a real slot to point at:
// a swap entry whose slot nothing allocated cannot show that teardown
// gives it back, which is the leak these exist to catch.

// A scratch swap area plus a scratch address space -- what all three
// tests below need, and one place to get it wrong instead of three.
static int swap_and_space(struct scratch *s, uint64_t *out_as) {
    *out_as = 0;
    if (!scratch_begin(s)) return 0;
    const char *why = "";
    if (!swap_format(blk_active(), &why) || !swap_on(blk_active(), &why)) return 0;
    *out_as = vmm_create_address_space();
    return *out_as != 0;
}

KTEST("swap", "an evicted page reads as SWAPPED to the audit, not as dangling") {
    struct scratch s;
    uint64_t as = 0;
    if (!swap_and_space(&s, &as)) {
        if (as) vmm_destroy_address_space(as);
        scratch_end(&s);
        KTEST_SKIP("no scratch disk or address space");
    }

    uint64_t va = UADDR_IMAGE_BASE;
    uint64_t frame = pmm_alloc_frame(PMM_ZONE_ANY);
    int mapped = frame && vmm_map_user_page(as, va, frame);

    uint64_t resident_before = vmm_user_bytes(as);
    uint32_t slot = swap_slot_alloc();
    int wrote = slot && swap_write_page(slot, frame);
    int evicted = wrote && vmm_set_swap_entry(as, va, slot);

    uint32_t read_slot = vmm_swap_entry(as, va);
    uint64_t still_mapped = vmm_user_phys(as, va);
    uint64_t resident_after = vmm_user_bytes(as);
    uint64_t swapped_after = vmm_user_swapped_bytes(as);

    struct vmm_audit a;
    vmm_audit_space(as, &a);

    vmm_destroy_address_space(as);
    scratch_end(&s);

    KTEST_ASSERT(mapped);
    KTEST_ASSERT(evicted);
    KTEST_ASSERT_EQ(read_slot, slot);
    // The mapping is gone, so nothing can still reach the frame...
    KTEST_ASSERT_EQ(still_mapped, 0ull);
    // ...and the frame went back, which is the entire point of evicting.
    KTEST_ASSERT(!pmm_frame_is_used(frame));
    // A NAIVE ENCODING THAT LEFT PRESENT SET WOULD FAIL HERE, and fail
    // as `dangling` -- a present mapping of a freed frame is exactly
    // what the audit calls the violation.
    KTEST_ASSERT_EQ(a.dangling, 0ull);
    KTEST_ASSERT_EQ(a.swapped, 1ull);
    // Resident falls and swapped rises, rather than the page simply
    // vanishing from both.
    KTEST_ASSERT_EQ(resident_before, 4096ull);
    KTEST_ASSERT_EQ(resident_after, 0ull);
    KTEST_ASSERT_EQ(swapped_after, 4096ull);
}

KTEST("swap", "a BORROWED page is refused -- somebody else owns that frame") {
    struct scratch s;
    uint64_t as = 0;
    if (!swap_and_space(&s, &as)) {
        if (as) vmm_destroy_address_space(as);
        scratch_end(&s);
        KTEST_SKIP("no scratch disk or address space");
    }

    // Every shared, DMA and cross-process mapping in the system is
    // borrowed, so this one refusal is what keeps the sound ring, the
    // window buffers, the shared font and the poison page out of the
    // reclaimer's reach. Testing the BIT tests all of them.
    uint64_t va = UADDR_IMAGE_BASE;
    uint64_t frame = pmm_alloc_frame(PMM_ZONE_ANY);
    int mapped = frame && vmm_map_user_borrowed(as, va, frame, 1, 0, 0);

    uint32_t slot = swap_slot_alloc();
    int evicted = slot && vmm_set_swap_entry(as, va, slot);
    uint64_t still_mapped = vmm_user_phys(as, va);

    if (slot) swap_slot_free(slot);
    vmm_destroy_address_space(as);
    int frame_survived = pmm_frame_is_used(frame);
    if (frame) pmm_free_frame(frame);
    scratch_end(&s);

    KTEST_ASSERT(mapped);
    KTEST_ASSERT(!evicted);
    KTEST_ASSERT_EQ(still_mapped, frame);
    // And the refusal did not free somebody else's frame on its way out.
    KTEST_ASSERT(frame_survived);
}

KTEST("swap", "a process that dies swapped gives its slots back") {
    struct scratch s;
    uint64_t as = 0;
    if (!swap_and_space(&s, &as)) {
        if (as) vmm_destroy_address_space(as);
        scratch_end(&s);
        KTEST_SKIP("no scratch disk or address space");
    }

    // Three pages so the count is a number rather than a coin flip.
    uint32_t used_before = 0;
    swap_stats(0, &used_before);
    int evicted = 0;
    for (int i = 0; i < 3; i++) {
        uint64_t va = UADDR_IMAGE_BASE + (uint64_t)i * 4096;
        uint64_t frame = pmm_alloc_frame(PMM_ZONE_ANY);
        if (!frame || !vmm_map_user_page(as, va, frame)) break;
        uint32_t slot = swap_slot_alloc();
        if (!slot || !swap_write_page(slot, frame)) break;
        if (!vmm_set_swap_entry(as, va, slot)) break;
        evicted++;
    }
    uint32_t used_swapped = 0;
    swap_stats(0, &used_swapped);

    // NOTHING AUDITS SLOT USAGE the way meminfo audits frames, so a
    // slot leaked here would never be reported -- it would surface much
    // later as a swap area that fills up with nothing swapped.
    vmm_destroy_address_space(as);
    uint32_t used_after = 0;
    swap_stats(0, &used_after);

    scratch_end(&s);
    KTEST_ASSERT_EQ(evicted, 3);
    KTEST_ASSERT_EQ(used_swapped, used_before + 3);
    KTEST_ASSERT_EQ(used_after, used_before);
}

// A swapped page has no frame to share, and fork's walk refuses rather
// than guessing (docs/fork-design.md): the child must be gone, the
// parent whole, and the slot still the parent's to give back.
KTEST("swap", "fork refuses an address space with a swapped page") {
    struct scratch s;
    if (!scratch_begin(&s)) { scratch_end(&s); KTEST_SKIP("no scratch disk"); }
    const char *why = "";
    int on = swap_format(blk_active(), &why) && swap_on(blk_active(), &why);
    uint64_t parent = on ? vmm_create_address_space() : 0;
    uint64_t f = parent ? pmm_alloc_frame(PMM_ZONE_ANY) : 0;
    int mapped = f && vmm_map_user_page(parent, UADDR_IMAGE_BASE, f);
    uint32_t slot = mapped ? swap_slot_alloc() : 0;
    int swapped = slot && swap_write_page(slot, f) &&
                  vmm_set_swap_entry(parent, UADDR_IMAGE_BASE, slot);
    uint64_t before = pmm_free_frames();
    uint64_t child = swapped ? vmm_fork_address_space(parent, 0) : 0;
    uint64_t after = pmm_free_frames();
    uint32_t still = swapped ? vmm_swap_entry(parent, UADDR_IMAGE_BASE) : 0;
    if (parent) vmm_destroy_address_space(parent); // gives the slot back
    scratch_end(&s);
    KTEST_ASSERT(swapped);
    KTEST_ASSERT_EQ(child, 0ull);
    KTEST_ASSERT_EQ(after, before);
    KTEST_ASSERT_EQ(still, slot);
}
