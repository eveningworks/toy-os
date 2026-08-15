// Tests for the ring-3 address-space map (kernel/include/kernel/uaddr.h)
// -- the layout invariants that make the stack guard work, and the
// classifier idt.c uses to name a fault in it.
//
// These are compile-time-ish facts asserted at runtime on purpose. The
// values are macros, so a wrong one is a silent behaviour change rather
// than a build error: widening the guard without moving the heap's
// ceiling, or reordering the two regions, both produce a kernel that
// boots and runs and has simply stopped protecting anything. That is
// exactly the failure a test is for.
//
// What is NOT here, because a KTEST cannot see it: the sbrk refusal and
// the crash report themselves, which need a ring-3 process. See
// userland/tests/guard_test.c (sbrk, in usertest_run.py) and
// userland/tests/stackovf_test.c (the overflow, which faults on purpose
// and so is excluded from that runner).
#include "ktest.h"
#include "uaddr.h"

KTEST("uaddr", "the heap cannot reach the stack") {
    // The whole point of the map: heap grows up, stack grows down, and
    // the guard sits between them with neither able to enter it. If
    // these ever touch, sbrk() maps over live stack pages silently --
    // which is what this layout was introduced to stop.
    KTEST_ASSERT(UADDR_HEAP_BASE < UADDR_HEAP_LIMIT);
    KTEST_ASSERT(UADDR_HEAP_LIMIT <= UADDR_GUARD_BASE);
    KTEST_ASSERT(UADDR_GUARD_BASE < UADDR_STACK_BOTTOM);
    KTEST_ASSERT(UADDR_STACK_BOTTOM <= UADDR_STACK_VADDR);
}

KTEST("uaddr", "the guard region is a whole number of real pages") {
    KTEST_ASSERT(UADDR_GUARD_PAGES >= 1);
    KTEST_ASSERT_EQ(UADDR_STACK_BOTTOM - UADDR_GUARD_BASE,
                     (uint64_t)UADDR_GUARD_PAGES * 4096);
    KTEST_ASSERT_EQ(UADDR_GUARD_BASE & 4095, 0);
    KTEST_ASSERT_EQ(UADDR_STACK_BOTTOM & 4095, 0);
}

KTEST("uaddr", "the stack is as many pages as it claims") {
    KTEST_ASSERT_EQ(UADDR_STACK_VADDR - UADDR_STACK_BOTTOM,
                     (uint64_t)(UADDR_STACK_PAGES - 1) * 4096);
}

KTEST("uaddr", "guard classification is exact at both edges") {
    // Off-by-one here is the difference between naming an overflow and
    // mislabelling the lowest live stack page as one.
    KTEST_ASSERT(uaddr_is_stack_guard(UADDR_GUARD_BASE));
    KTEST_ASSERT(uaddr_is_stack_guard(UADDR_STACK_BOTTOM - 1));
    KTEST_ASSERT(!uaddr_is_stack_guard(UADDR_GUARD_BASE - 1));
    KTEST_ASSERT(!uaddr_is_stack_guard(UADDR_STACK_BOTTOM));
}

KTEST("uaddr", "guard classification rejects everything else") {
    // A wild pointer must NOT be reported as a stack overflow -- the
    // label is only worth having if it is specific.
    KTEST_ASSERT(!uaddr_is_stack_guard(0));
    KTEST_ASSERT(!uaddr_is_stack_guard(UADDR_HEAP_BASE));
    KTEST_ASSERT(!uaddr_is_stack_guard(UADDR_STACK_VADDR));
    KTEST_ASSERT(!uaddr_is_stack_guard(0xFFFFFFFFFFFFFFFFULL));
}
