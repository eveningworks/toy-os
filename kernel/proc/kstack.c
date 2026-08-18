// Kernel-stack safety: the guard page, the canary and the poison fill.
// See kernel/include/kernel/kstack.h for what each is for and why there
// are two owners; the reasoning is in docs/decisions.md.

#include "kstack.h"
#include "paging.h"

void kstack_arm(struct kstack *k, uint32_t reserve_top, uint32_t *peak) {
    uint64_t base = kstack_base(k);
    uint64_t top  = kstack_top(k);
    // Everything except the canary word and whatever the caller has
    // already written at the top (a synthesized trapframe, for a
    // process that has never run).
    for (uint64_t a = base + 8; a < top - reserve_top; a++) {
        *(volatile uint8_t *)a = KSTACK_POISON;
    }
    *(volatile uint64_t *)base = KSTACK_MAGIC;
    if (peak) *peak = 0;
}

int kstack_canary_ok(const struct kstack *k) {
    return *(volatile uint64_t *)kstack_base(k) == KSTACK_MAGIC;
}

uint32_t kstack_used(const struct kstack *k, uint32_t *peak) {
    uint64_t base = kstack_base(k);
    uint64_t top  = kstack_top(k);
    uint32_t known = peak ? *peak : 0;

    // Scan UP from the base for the first byte the poison no longer
    // covers, which is the deepest point ever reached.
    //
    // NOT down from the top, which is the obvious cheap version and is
    // WRONG: real stack data contains 0xAA bytes, so a downward scan
    // stops at the first coincidental one and under-reports. Measured
    // -- a path that genuinely used ~9 KiB reported 184 bytes, exactly
    // one trapframe, because byte 185 happened to be 0xAA. Linux's
    // stack_not_used() scans from the bottom for the same reason.
    //
    // The known high-water bounds the walk, so it shortens as the peak
    // grows and never re-reads a region already proven used.
    uint64_t limit = top - known;
    uint64_t a = base + 8;
    while (a < limit && *(volatile uint8_t *)a == KSTACK_POISON) a++;
    uint32_t used = (uint32_t)(top - a);
    if (used < known) used = known;
    if (peak) *peak = used;
    return used;
}

int kstack_guard_arm(struct kstack *k) {
    return paging_unmap_kernel_page((uint64_t)(uintptr_t)&k->guard[0]);
}

int kstack_guard_contains(const struct kstack *k, uint64_t addr) {
    uint64_t lo = (uint64_t)(uintptr_t)&k->guard[0];
    return addr >= lo && addr < lo + KSTACK_GUARD_BYTES;
}
