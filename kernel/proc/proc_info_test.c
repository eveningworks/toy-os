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

    uint64_t frame = pmm_alloc_frame();
    KTEST_ASSERT(frame != 0);
    KTEST_ASSERT(vmm_map_user_page(as, TEST_VADDR, frame));

    KTEST_ASSERT_EQ(vmm_user_bytes(as), (uint64_t)4096);

    vmm_destroy_address_space(as);
}

KTEST("procinfo", "unmapping a page gives the memory back") {
    uint64_t as = vmm_create_address_space();
    KTEST_ASSERT(as != 0);

    uint64_t frame = pmm_alloc_frame();
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

    uint64_t a = pmm_alloc_frame(), b = pmm_alloc_frame();
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

    uint64_t frame = pmm_alloc_frame();
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
