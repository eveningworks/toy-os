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
// and so is excluded from that runner), and userland/tests/stackgrow_test.c
// (that the stack GROWS, which no macro here can show).
#include "ktest.h"
#include "uaddr.h"

KTEST("uaddr", "the heap cannot reach the stack") {
    // The whole point of the map: heap grows up, stack grows down, and
    // the guard sits between them with neither able to enter it. If
    // these ever touch, sbrk() maps over live stack pages silently --
    // which is what this layout was introduced to stop.
    //
    // Against the FLOOR, not the initial bottom: the stack's reservation
    // is what the heap must clear, and testing the bottom would have
    // passed happily with an 8 MiB stack overlapping the heap by
    // 8 MiB minus four pages.
    KTEST_ASSERT(UADDR_HEAP_MIN_BASE < UADDR_HEAP_LIMIT);
    KTEST_ASSERT(UADDR_HEAP_LIMIT <= UADDR_GUARD_BASE);
    KTEST_ASSERT(UADDR_GUARD_BASE < UADDR_STACK_FLOOR);
    KTEST_ASSERT(UADDR_STACK_FLOOR <= UADDR_STACK_INIT_BOTTOM);
    KTEST_ASSERT(UADDR_STACK_INIT_BOTTOM <= UADDR_STACK_VADDR);
}

KTEST("uaddr", "the guard region is a whole number of real pages") {
    KTEST_ASSERT(UADDR_GUARD_PAGES >= 1);
    KTEST_ASSERT_EQ(UADDR_STACK_FLOOR - UADDR_GUARD_BASE,
                     (uint64_t)UADDR_GUARD_PAGES * 4096);
    KTEST_ASSERT_EQ(UADDR_GUARD_BASE & 4095, 0);
    KTEST_ASSERT_EQ(UADDR_STACK_FLOOR & 4095, 0);
    KTEST_ASSERT_EQ(UADDR_STACK_INIT_BOTTOM & 4095, 0);
}

KTEST("uaddr", "the stack is as many pages as it claims, both bounds") {
    KTEST_ASSERT_EQ(UADDR_STACK_VADDR - UADDR_STACK_INIT_BOTTOM,
                     (uint64_t)(UADDR_STACK_INIT_PAGES - 1) * 4096);
    KTEST_ASSERT_EQ(UADDR_STACK_VADDR - UADDR_STACK_FLOOR,
                     (uint64_t)(UADDR_STACK_MAX_PAGES - 1) * 4096);
    // A process must start with room to run and room to grow -- an
    // initial working set equal to the maximum would be the old fixed
    // stack wearing the new names, and every growth test would pass
    // vacuously.
    KTEST_ASSERT(UADDR_STACK_INIT_PAGES < UADDR_STACK_MAX_PAGES);
}

KTEST("uaddr", "the growth gap cannot be leapt by a permitted frame") {
    // THE PAIR THAT MAKES GROWTH SAFE, asserted rather than described:
    // a frame larger than the gap can touch a page deeper than the
    // handler will grow to, and die on a stack that was willing to grow
    // for it. The compiler refuses those frames
    // (-Wframe-larger-than=2048 in USERLAND_CFLAGS), so the two numbers
    // have to stay on the right side of each other -- raising the frame
    // limit past the gap is exactly the edit this catches.
    KTEST_ASSERT(UADDR_STACK_GROW_GAP > 2048);
    // And the gap must be a whole number of pages, since growth walks
    // page by page.
    KTEST_ASSERT_EQ(UADDR_STACK_GROW_GAP & 4095, 0);
}

KTEST("uaddr", "the stack range covers the reservation and nothing else") {
    KTEST_ASSERT(uaddr_is_stack_range(UADDR_STACK_FLOOR));
    KTEST_ASSERT(uaddr_is_stack_range(UADDR_STACK_INIT_BOTTOM));
    KTEST_ASSERT(uaddr_is_stack_range(UADDR_STACK_VADDR));
    KTEST_ASSERT(uaddr_is_stack_range(UADDR_STACK_VADDR + 4095));
    // The two addresses either side. A heap page must never be mistaken
    // for a stack page: the fault handler would then grow the stack over
    // it rather than answering from the break.
    KTEST_ASSERT(!uaddr_is_stack_range(UADDR_STACK_FLOOR - 1));
    KTEST_ASSERT(!uaddr_is_stack_range(UADDR_STACK_VADDR + 4096));
    KTEST_ASSERT(!uaddr_is_stack_range(UADDR_HEAP_MIN_BASE));
    KTEST_ASSERT(!uaddr_is_stack_range(0));
}

KTEST("uaddr", "guard classification is exact at both edges") {
    // Off-by-one here is the difference between naming an overflow and
    // mislabelling the lowest page the stack may legally reach as one.
    KTEST_ASSERT(uaddr_is_stack_guard(UADDR_GUARD_BASE));
    KTEST_ASSERT(uaddr_is_stack_guard(UADDR_STACK_FLOOR - 1));
    KTEST_ASSERT(!uaddr_is_stack_guard(UADDR_GUARD_BASE - 1));
    KTEST_ASSERT(!uaddr_is_stack_guard(UADDR_STACK_FLOOR));
}

KTEST("uaddr", "guard classification rejects everything else") {
    // A wild pointer must NOT be reported as a stack overflow -- the
    // label is only worth having if it is specific.
    KTEST_ASSERT(!uaddr_is_stack_guard(0));
    KTEST_ASSERT(!uaddr_is_stack_guard(UADDR_HEAP_MIN_BASE));
    KTEST_ASSERT(!uaddr_is_stack_guard(UADDR_STACK_VADDR));
    // The growable region is NOT the guard: a fault in here is answered
    // with a page, and reporting it as an overflow would name every
    // ordinary deep call chain a crash.
    KTEST_ASSERT(!uaddr_is_stack_guard(UADDR_STACK_INIT_BOTTOM - 4096));
    KTEST_ASSERT(!uaddr_is_stack_guard(0xFFFFFFFFFFFFFFFFULL));
}
