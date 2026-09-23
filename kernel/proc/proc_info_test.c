// Tests for the per-process accounting behind SYS_PROC_INFO -- the
// numbers Task Manager shows.
//
// WHY THESE ARE KTESTS
// --------------------
// Two of the three fields are only checkable from inside the kernel.
// `mem_bytes` is maintained by vmm as pages are mapped, so the honest
// assertion is "map a page, the number goes up by exactly one page" --
// a ring-3 program can observe its own total but cannot make a mapping
// happen at a known moment. And a slot's accounting must be RESET when
// the slot is reused, which needs two processes in the same slot and is
// invisible from userland entirely.
//
// The memory tests build their own address space with
// vmm_create_address_space() rather than spawning anything: the
// property is about vmm's counter, and a real process drags the ELF
// loader, a stack and a heap into a number that should be exactly
// predictable.
//
// POSITIVE CONTROL, run when these were written: delete the counter
// decrement in vmm_unmap_user_page(). Measured -- exactly one check
// goes red ("unmapping a page gives the memory back"), and notably NOT
// the mapping one, which is the pair worth having: the two directions
// are separate code and a test that only ever mapped would call a
// one-way counter correct.

#include "ktest.h"
#include "scheduler.h"
#include "vmm.h"
#include "pmm.h"
#include "proc_info.h"
#include "string.h"

#define TEST_VADDR 0x9000000000ULL // clear of every uaddr.h region

KTEST("procinfo", "an empty slot is a SUCCESSFUL report of pid 0") {
    // The contract enumeration depends on: a caller walking the table
    // must skip empty slots, not stop at the first one. If this
    // returned 0 the obvious loop would stop dead at slot 0 whenever
    // nothing was running.
    struct proc_info info;
    k_memset(&info, 0xAA, sizeof info);

    int last = scheduler_max_procs() - 1;
    KTEST_ASSERT_EQ(scheduler_proc_info(last, &info), 1);
    KTEST_ASSERT_EQ(info.pid, 0);
    KTEST_ASSERT_EQ(info.state, (uint32_t)PROC_STATE_UNUSED);
    KTEST_ASSERT_EQ(info.name[0], '\0');
    // The 0xAA fill above is what gives this one teeth: an empty slot
    // must report `ready` as 0 because the field was WRITTEN, not
    // because it happened to be zero already. init treats the bit as
    // "this service is up", so a garbage read here would be a desktop
    // announced as usable before it existed.
    KTEST_ASSERT_EQ(info.ready, (uint32_t)0);
}

KTEST("procinfo", "the kernel context cannot announce readiness") {
    // SYS_NOTIFY_READY records the CALLER, and the kernel context is not
    // a process -- it has no slot to record it in. Refused rather than
    // written somewhere harmless, because the one thing this bit must
    // never do is appear on a row that did not ask for it: init reads
    // the table by slot and would attribute it to whichever service
    // that slot holds.
    //
    // A KTEST runs in exactly that context, which is why this is
    // testable here at all and why it is the one half of the syscall a
    // KTEST can reach -- the ring-3 half is proved by tools/init_test.py
    // watching a real service announce itself.
    KTEST_ASSERT_EQ(scheduler_mark_current_ready(), 0);
}

KTEST("procinfo", "an out-of-range slot is refused") {
    struct proc_info info;
    KTEST_ASSERT_EQ(scheduler_proc_info(-1, &info), 0);
    KTEST_ASSERT_EQ(scheduler_proc_info(scheduler_max_procs(), &info), 0);
    KTEST_ASSERT_EQ(scheduler_proc_info(0, 0), 0);
}

KTEST("procinfo", "mapping a page adds exactly one page of memory") {
    uint64_t as = vmm_create_address_space();
    KTEST_ASSERT(as != 0);

    KTEST_ASSERT_EQ(vmm_user_bytes(as), (uint64_t)0);

    uint64_t frame = pmm_alloc_frame(PMM_ZONE_DMA32);
    KTEST_ASSERT(frame != 0);
    KTEST_ASSERT(vmm_map_user_page(as, TEST_VADDR, frame));

    KTEST_ASSERT_EQ(vmm_user_bytes(as), (uint64_t)4096);

    vmm_destroy_address_space(as);
}

KTEST("procinfo", "unmapping a page gives the memory back") {
    uint64_t as = vmm_create_address_space();
    KTEST_ASSERT(as != 0);

    uint64_t frame = pmm_alloc_frame(PMM_ZONE_DMA32);
    KTEST_ASSERT(frame != 0);
    KTEST_ASSERT(vmm_map_user_page(as, TEST_VADDR, frame));
    KTEST_ASSERT_EQ(vmm_user_bytes(as), (uint64_t)4096);

    KTEST_ASSERT(vmm_unmap_user_page(as, TEST_VADDR));
    KTEST_ASSERT_EQ(vmm_user_bytes(as), (uint64_t)0);

    pmm_free_frame(frame);
    vmm_destroy_address_space(as);
}

KTEST("procinfo", "remapping the same address does not count twice") {
    // win_server.c does exactly this on a window resize: the same
    // virtual address is pointed at a new frame. Counting it again
    // would make a resizing window's memory climb forever.
    uint64_t as = vmm_create_address_space();
    KTEST_ASSERT(as != 0);

    uint64_t a = pmm_alloc_frame(PMM_ZONE_DMA32), b = pmm_alloc_frame(PMM_ZONE_DMA32);
    KTEST_ASSERT(a != 0 && b != 0);

    KTEST_ASSERT(vmm_map_user_page(as, TEST_VADDR, a));
    KTEST_ASSERT(vmm_map_user_page(as, TEST_VADDR, b)); // replaces
    KTEST_ASSERT_EQ(vmm_user_bytes(as), (uint64_t)4096);

    pmm_free_frame(a);
    pmm_free_frame(b);
    vmm_destroy_address_space(as);
}

KTEST("procinfo", "a destroyed address space does not haunt its successor") {
    // pmm hands the same frame out again, so a later address space can
    // be born at the SAME physical PML4 address. Without releasing the
    // accounting slot it would inherit the dead one's page count -- and
    // the symptom would be a brand new process reporting somebody
    // else's memory, which reads as a leak rather than as bookkeeping.
    uint64_t as = vmm_create_address_space();
    KTEST_ASSERT(as != 0);

    uint64_t frame = pmm_alloc_frame(PMM_ZONE_DMA32);
    KTEST_ASSERT(frame != 0);
    KTEST_ASSERT(vmm_map_user_page(as, TEST_VADDR, frame));
    KTEST_ASSERT(vmm_user_bytes(as) > 0);

    vmm_destroy_address_space(as);
    KTEST_ASSERT_EQ(vmm_user_bytes(as), (uint64_t)0);

    uint64_t again = vmm_create_address_space();
    KTEST_ASSERT(again != 0);
    KTEST_ASSERT_EQ(vmm_user_bytes(again), (uint64_t)0);
    vmm_destroy_address_space(again);
}

// --- what a blocked process is waiting for ---------------------------
//
// scheduler_test_park() fabricates a slot rather than spawning
// anything, for the reason api/scheduler.h gives: a real process
// parking on a CHOSEN channel at a chosen moment is a race. Both rules
// it states are kept below -- the preemption guard spans the whole
// window, and the slot is released before anything is asserted.

KTEST("procinfo", "a blocked process reports what it waits on") {
    uint64_t tf[SCHED_TF_SLOTS] = {0};
    static const char chan;

    scheduler_preempt_disable();
    int idx = scheduler_test_park(tf, &chan, SCHED_WAIT_PIPE);
    struct proc_info info;
    int got = idx >= 0 ? scheduler_proc_info(idx, &info) : 0;
    if (idx >= 0) scheduler_test_release(idx);
    scheduler_preempt_enable();

    if (idx < 0) KTEST_SKIP("no free process slots to fabricate");
    KTEST_ASSERT_EQ(got, 1);
    KTEST_ASSERT_EQ(info.state, (uint32_t)PROC_STATE_BLOCKED);
    KTEST_ASSERT_EQ(info.wait_reason, (uint32_t)PROC_WAIT_PIPE);
}

KTEST("procinfo", "every wait reason is reported, and none as PROC_WAIT_NONE") {
    // THE CHECK THAT KEEPS TWO ENUMERATIONS IN STEP. The scheduler's
    // SCHED_WAIT_* and the ABI's PROC_WAIT_* are deliberately separate
    // (abi/proc_info.h), which means a reason added kernel-side and not
    // added to scheduler_proc_info()'s mapping would report as "not
    // waiting for anything" -- a wrong answer that looks like a valid
    // one. Walking the whole range turns that into a red check.
    //
    // The upper bound is the LAST SCHED_WAIT_*: adding one without
    // extending this loop is the one drift it cannot catch, so
    // scheduler.h's own comment lists this as the third of three edits.
    // It HAD drifted -- the bound sat at SCHED_WAIT_FUTEX while
    // SCHED_WAIT_SIGNAL was already past it, so the one reason this
    // check could not see was the one added last. Extending it is not
    // optional maintenance; it is the check.
    static const char chan;
    uint32_t seen[SCHED_WAIT_DISK + 1] = {0};
    int parked_ok = 1;

    for (int r = SCHED_WAIT_EVENT; r <= SCHED_WAIT_DISK; r++) {
        uint64_t tf[SCHED_TF_SLOTS] = {0};
        struct proc_info info;
        scheduler_preempt_disable();
        int idx = scheduler_test_park(tf, &chan, r);
        int got = idx >= 0 ? scheduler_proc_info(idx, &info) : 0;
        if (idx >= 0) scheduler_test_release(idx);
        scheduler_preempt_enable();
        if (!got) { parked_ok = 0; break; }
        seen[r] = info.wait_reason;
    }

    if (!parked_ok) KTEST_SKIP("no free process slots to fabricate");
    for (int r = SCHED_WAIT_EVENT; r <= SCHED_WAIT_DISK; r++) {
        KTEST_ASSERT(seen[r] != PROC_WAIT_NONE);
        // Distinct, too: a mapping that collapsed two reasons onto one
        // value would pass the non-zero check and still lie.
        for (int q = SCHED_WAIT_EVENT; q < r; q++) KTEST_ASSERT(seen[q] != seen[r]);
    }
}

KTEST("procinfo", "a runnable process waits on nothing") {
    // The other half of the field's contract: wait_reason is meaningless
    // outside PROC_STATE_BLOCKED, so it must be cleared rather than left
    // holding whatever the slot last parked on. Without the reset a
    // woken process would keep reading as `block(pipe)` in `ps`.
    uint64_t tf[SCHED_TF_SLOTS] = {0};
    static const char chan;

    scheduler_preempt_disable();
    int idx = scheduler_test_park(tf, &chan, SCHED_WAIT_TIMER);
    if (idx >= 0) scheduler_wake(&chan, 0); // now READY, still fabricated
    struct proc_info info;
    int got = idx >= 0 ? scheduler_proc_info(idx, &info) : 0;
    if (idx >= 0) scheduler_test_release(idx);
    scheduler_preempt_enable();

    if (idx < 0) KTEST_SKIP("no free process slots to fabricate");
    KTEST_ASSERT_EQ(got, 1);
    KTEST_ASSERT_EQ(info.state, (uint32_t)PROC_STATE_READY);
    KTEST_ASSERT_EQ(info.wait_reason, (uint32_t)PROC_WAIT_NONE);
}
