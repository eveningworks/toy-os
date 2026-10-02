// SYS_FUTEX_WAIT / SYS_FUTEX_WAKE: block on a word in memory until
// somebody changes it and says so.
//
// **THE KEY IS THE PHYSICAL ADDRESS, and that is the whole design.**
// Two processes sharing an shm page see the same word at DIFFERENT
// virtual addresses, so a channel built from the caller's own pointer
// would park them on two unrelated addresses and a wake would reach
// nobody. What they agree on is the frame. Linux keys a shared futex on
// (inode, offset) for this reason; here the physical address IS that
// pair, already computed by the page walk.
//
// The private case comes out right at no cost: two processes' private
// pages are different frames, so their keys differ even at the same
// virtual address.
//
// A channel is any address the scheduler can compare (api/scheduler.h),
// and a user frame's physical address cannot collide with the kernel
// objects already used as channels -- those live in the kernel image
// and the kernel heap, whose frames are never handed to a user mapping.
//
// WHY THE COMPARE-THEN-BLOCK IS SAFE HERE: a syscall handler runs with
// interrupts off, so nothing else observes the word between the compare
// and the park. That is a property of this kernel being single-core,
// not of the design -- an SMP port needs a real lock around the pair,
// and this comment is where that will be looked for.
#include "heap.h"   // kzalloc -- the wakeword table, sized at first use
#include "syscalls.h"
#include "syscall_abi.h"
#include "errno.h"
#include "scheduler.h"
#include "vmm.h"
#include "clocksource.h"
#include "ktest.h"
#include "pmm.h"
#include "string.h"
#include "futex.h"
#include "win_input.h"   // the event post the wakeword rides
#include "win_role.h"    // the KTEST below gives a spare pid the role
#include <stddef.h>

// The word is 32 bits and must be aligned, so a key names one word
// rather than a range: two futexes in the same page are two channels.
static int futex_key(uint64_t pml4, uint64_t uaddr, const void **out) {
    if (uaddr & 3) return -EINVAL;
    uint32_t probe;
    // Through the copy helper rather than the raw walk, because that is
    // what faults a demand-paged page in -- vmm_user_phys() deliberately
    // does not, so asking it first would refuse a legal address the
    // caller simply has not touched yet.
    if (!vmm_copy_from_user(pml4, &probe, uaddr, sizeof probe)) return -EFAULT;
    uint64_t phys = vmm_user_phys(pml4, uaddr);
    if (!phys) return -EFAULT;
    *out = (const void *)(uintptr_t)phys;
    return 0;
}

int sys_futex_wait(struct syscall_ctx *c) {
    const void *key;
    int64_t ret = futex_key(c->pml4, c->a0, &key);
    if (ret < 0) { c->regs[14] = (uint64_t)ret; return 0; }

    uint32_t cur;
    if (!vmm_copy_from_user(c->pml4, &cur, c->a0, sizeof cur)) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    // THE WORD ALREADY MOVED, so there is nothing to wait for. Reporting
    // this rather than blocking is what closes the lost-wakeup race: a
    // caller that read the word, was preempted while the waker changed
    // it, and then parked would wait for a wake that already happened.
    if (cur != (uint32_t)c->a1) {
        c->regs[14] = (uint64_t)(int64_t)-EAGAIN;
        return 0;
    }

    // MONOTONIC, like every other bounded wait here -- a deadline
    // measured against the wall clock moves when the clock is set.
    uint64_t deadline = c->a2 ? clocksource_now_ns() + c->a2 * 1000000ULL : 0;
    c->regs[14] = 0;   // what a wake overwrites, and what a wake returns
    scheduler_block_current_until(c->regs, key, SCHED_WAIT_FUTEX, deadline);
    return 0;
}

int sys_futex_wake(struct syscall_ctx *c) {
    const void *key;
    int64_t ret = futex_key(c->pml4, c->a0, &key);
    if (ret < 0) { c->regs[14] = (uint64_t)ret; return 0; }
    // a1 = how many to release, 0 for all. A lock's unlock wants one.
    c->regs[14] = (uint64_t)(int64_t)scheduler_wake_n(key, 0, (int)c->a1);
    return 0;
}


// --- the wakeword ----------------------------------------------------
//
// See kernel/futex.h for what this is for. Kept here rather than in the
// scheduler because it is the futex's key derivation that makes it
// work: the word is named by its FRAME, so the kernel can bump it from
// any context without caring which address space is loaded.

// PER SLOT, with the owner's pid: a lookup checks it, so a slot reused by
// another process never inherits the last one's word.
//
// SIZED FOR THE PROCESS LIMIT on the first registration (a syscall, so
// the heap is safe); every reader treats a NULL table as "nobody
// registered", which keeps futex_note_ready() callable from anywhere.
struct wakeword {
    int pid;
    uint64_t phys;   // 0 = this process registered none
    uint64_t pml4;   // whose it is, so a teardown can drop it
};
static struct wakeword *g_wakeword;
static int g_ww_cap;

static int ww_slot(int pid) {
    int s = scheduler_pid_slot(pid);
    return s >= 0 && s < g_ww_cap && g_wakeword[s].pid == pid ? s : -1;
}

void futex_note_ready(int pid) {
    int s = ww_slot(pid);
    if (s < 0) return;
    uint64_t phys = g_wakeword[s].phys;
    if (!phys) return;
    // The bump is what closes the lost-wakeup race, not the wake: a
    // waiter that sampled the word before this and parks after it finds
    // the value already moved and does not park at all.
    (*(volatile uint32_t *)(uintptr_t)phys)++;
    scheduler_wake_n((const void *)(uintptr_t)phys, 0, 0);
}

uint64_t futex_wakeword_phys(int pid) {
    int s = ww_slot(pid);
    return s < 0 ? 0 : g_wakeword[s].phys & ~0xFFFULL;
}

void futex_wakeword_release(uint64_t pml4_phys) {
    for (int i = 0; i < g_ww_cap; i++)
        if (g_wakeword[i].pml4 == pml4_phys) {
            g_wakeword[i].pid = 0;
            g_wakeword[i].phys = 0;
            g_wakeword[i].pml4 = 0;
        }
}

static void ww_set(int pid, uint64_t phys, uint64_t pml4) {
    int s = scheduler_pid_slot(pid);
    if (s < 0) return;
    if (!g_wakeword) {
        int cap = scheduler_max_procs();
        g_wakeword = kzalloc((uint32_t)cap * sizeof *g_wakeword);
        if (!g_wakeword) return;
        g_ww_cap = cap;
    }
    if (s >= g_ww_cap) return;
    g_wakeword[s].pid = phys ? pid : 0;
    g_wakeword[s].phys = phys;
    g_wakeword[s].pml4 = phys ? pml4 : 0;
}

int sys_wakeword(struct syscall_ctx *c) {
    int pid = scheduler_current_pid();
    if (scheduler_pid_slot(pid) < 0) {
        c->regs[14] = (uint64_t)(int64_t)-EPERM;
        return 0;
    }
    if (!c->a0) {                     // 0 deregisters
        ww_set(pid, 0, 0);
        c->regs[14] = 0;
        return 0;
    }
    const void *key;
    int64_t ret = futex_key(c->pml4, c->a0, &key);
    if (ret < 0) { c->regs[14] = (uint64_t)ret; return 0; }
    ww_set(pid, (uint64_t)(uintptr_t)key, c->pml4);
    c->regs[14] = 0;
    return 0;
}

// --- tests -----------------------------------------------------------
//
// A KTEST runs on the kernel context, which owns no user memory -- so
// these build an address space with one user page in it, the same shape
// kernel/mm/uaccess_test.c uses and for the same reason. A futex word
// on the kernel stack would be refused by the copy helper, correctly.

#define FUTEX_TEST_VADDR 0x9000000000ULL

struct futex_fixture {
    uint64_t as;
    uint64_t frame;
};

static int futex_fixture_up(struct futex_fixture *f) {
    f->as = vmm_create_address_space();
    if (!f->as) return 0;
    f->frame = pmm_alloc_frame(PMM_ZONE_ANY);
    if (!f->frame) return 0;
    k_memset((void *)(uintptr_t)f->frame, 0, 4096);
    return vmm_map_user_page(f->as, FUTEX_TEST_VADDR, f->frame);
}

static void futex_fixture_down(struct futex_fixture *f) {
    if (f->as) vmm_destroy_address_space(f->as);
    f->as = 0;
}

// The word, written straight through the frame -- the test is the
// "other side", so it does not need a copy helper to reach it.
static void futex_set(struct futex_fixture *f, uint32_t v) {
    *(volatile uint32_t *)(uintptr_t)f->frame = v;
}

static int64_t call(int (*fn)(struct syscall_ctx *), uint64_t as,
                    uint64_t a0, uint64_t a1, uint64_t a2) {
    struct syscall_ctx c = {0};
    uint64_t regs[24] = {0};
    c.pml4 = as;
    c.a0 = a0; c.a1 = a1; c.a2 = a2;
    c.regs = regs;
    fn(&c);
    return (int64_t)c.regs[14];
}

KTEST("futex", "a word that already moved is not waited on") {
    // The lost-wakeup race in one assertion: the caller's expected value
    // is stale, so the only correct answer is "look again", never a
    // park. A version that blocked here would hang the suite rather than
    // fail it, which is why every check below keeps the word moving.
    struct futex_fixture f = {0};
    if (!futex_fixture_up(&f)) { futex_fixture_down(&f); KTEST_SKIP("out of memory"); }
    futex_set(&f, 7);
    KTEST_ASSERT_EQ(call(sys_futex_wait, f.as, FUTEX_TEST_VADDR, 8, 0), -EAGAIN);
    futex_fixture_down(&f);
}

KTEST("futex", "an unaligned or unmapped word is refused, not guessed") {
    struct futex_fixture f = {0};
    if (!futex_fixture_up(&f)) { futex_fixture_down(&f); KTEST_SKIP("out of memory"); }
    KTEST_ASSERT_EQ(call(sys_futex_wake, f.as, FUTEX_TEST_VADDR + 1, 0, 0), -EINVAL);
    KTEST_ASSERT_EQ(call(sys_futex_wake, f.as, FUTEX_TEST_VADDR + 0x100000, 0, 0), -EFAULT);
    futex_fixture_down(&f);
}

KTEST("futex", "waking a word nobody waits on releases nobody") {
    struct futex_fixture f = {0};
    if (!futex_fixture_up(&f)) { futex_fixture_down(&f); KTEST_SKIP("out of memory"); }
    KTEST_ASSERT_EQ(call(sys_futex_wake, f.as, FUTEX_TEST_VADDR, 0, 0), 0);
    futex_fixture_down(&f);
}

KTEST("futex", "two words in ONE page are two different channels") {
    // What fails if the key were page-granular, which is the easy way to
    // write this and would make every futex in a heap wake every other.
    struct futex_fixture f = {0};
    if (!futex_fixture_up(&f)) { futex_fixture_down(&f); KTEST_SKIP("out of memory"); }
    const void *k0 = NULL, *k1 = NULL;
    KTEST_ASSERT_EQ(futex_key(f.as, FUTEX_TEST_VADDR, &k0), 0);
    KTEST_ASSERT_EQ(futex_key(f.as, FUTEX_TEST_VADDR + 4, &k1), 0);
    KTEST_ASSERT(k0 && k1 && k0 != k1);
    futex_fixture_down(&f);
}

// The wakeword's OWNER: a fabricated parked slot, so its pid is a real
// one (wakewords are kept per slot). BLOCKED on a private channel with no
// deadline, nothing can schedule it, so the test runs with the guard down.
static const char g_owner_chan;
static uint64_t g_owner_tf[SCHED_TF_SLOTS];

static int owner_park(void) {
    scheduler_preempt_disable();
    int s = scheduler_test_park(g_owner_tf, &g_owner_chan, 0);
    scheduler_preempt_enable();
    return s;
}

static void owner_release(int s) {
    scheduler_preempt_disable();
    scheduler_test_release(s);
    (void)scheduler_test_take_resched();
    scheduler_preempt_enable();
}

KTEST("futex", "queueing an EVENT bumps the waiter's wakeword") {
    // THE WIRING, and the only check that covers it: a futex waits on
    // one word, so a process waiting for a message and for a window
    // event needs both to touch the SAME word. If the event path stops
    // bumping it, a compositor blocks on a channel and stops seeing
    // input -- and nothing else here would notice.
    struct futex_fixture f = {0};
    if (!futex_fixture_up(&f)) { futex_fixture_down(&f); KTEST_SKIP("out of memory"); }
    // The queue is the compositor's, so the owner has to BE the
    // compositor for the duration -- which a live desktop forbids.
    if (win_server_compositor_pid()) { futex_fixture_down(&f); KTEST_SKIP("a compositor holds the role"); }
    int slot = owner_park();
    if (slot < 0) { futex_fixture_down(&f); KTEST_SKIP("no free process slot"); }
    int pid = scheduler_slot_pid(slot);

    const void *key = NULL;
    int keyed = futex_key(f.as, FUTEX_TEST_VADDR, &key) == 0;
    ww_set(pid, (uint64_t)(uintptr_t)key, f.as);
    futex_set(&f, 0);
    win_server_set_compositor(pid, 0);

    struct win_event ev = { .type = WIN_EV_RAW_KEY, .a = 'x' };
    int p1 = win_input_push(&ev);
    uint32_t v1 = *(volatile uint32_t *)(uintptr_t)f.frame;
    // A SECOND event moves it again -- a word that only ever reached 1
    // would let a waiter that sampled 1 park through everything after.
    int p2 = win_input_push(&ev);
    uint32_t v2 = *(volatile uint32_t *)(uintptr_t)f.frame;
    // And the registration goes with the address space, or the kernel
    // writes into whatever the allocator hands out next.
    futex_wakeword_release(f.as);
    int p3 = win_input_push(&ev);
    uint32_t v3 = *(volatile uint32_t *)(uintptr_t)f.frame;

    win_server_set_compositor(0, 0);
    owner_release(slot);
    futex_fixture_down(&f);
    KTEST_ASSERT(keyed && p1 && p2 && p3);
    KTEST_ASSERT_EQ(v1, 1u);
    KTEST_ASSERT_EQ(v2, 2u);
    KTEST_ASSERT_EQ(v3, 2u);
}

KTEST("futex", "a SECOND source bumps the same wakeword") {
    // THE GENERALITY CLAIM, checked rather than asserted: the wakeword
    // is supposed to be one word EVERY source bumps, so that a process
    // waiting on several things needs only one wait. A window event was
    // the first source; a child dying is the second, and it is what lets
    // a supervisor serve requests and reap children in one loop.
    //
    // If this is ever the only source left that works, a supervisor
    // parked on its wakeword stops noticing deaths -- which reads as a
    // service that exited and was never restarted.
    struct futex_fixture f = {0};
    if (!futex_fixture_up(&f)) { futex_fixture_down(&f); KTEST_SKIP("out of memory"); }
    int slot = owner_park();
    if (slot < 0) { futex_fixture_down(&f); KTEST_SKIP("no free process slot"); }
    int pid = scheduler_slot_pid(slot);

    const void *key = NULL;
    int keyed = futex_key(f.as, FUTEX_TEST_VADDR, &key) == 0;
    ww_set(pid, (uint64_t)(uintptr_t)key, f.as);
    futex_set(&f, 0);

    // THE HELPER, NOT THE CALL SITE, and that gap is worth stating: a
    // real child death cannot be provoked from a KTEST, so what proves
    // sched_exit.c's notify_parent() actually calls this is init serving
    // its channel while reaping -- an end-to-end check, not this one.
    futex_note_ready(pid);
    uint32_t v = *(volatile uint32_t *)(uintptr_t)f.frame;

    futex_wakeword_release(f.as);
    owner_release(slot);
    futex_fixture_down(&f);
    KTEST_ASSERT(keyed);
    KTEST_ASSERT_EQ(v, 1u);
}

KTEST("futex", "the SAME frame at two addresses is ONE channel") {
    // THE PROPERTY THE WHOLE DESIGN EXISTS FOR: two processes map an shm
    // page wherever they like, and must still meet on one channel. A key
    // built from the caller's pointer passes every other check here and
    // fails this one.
    struct futex_fixture f = {0};
    if (!futex_fixture_up(&f)) { futex_fixture_down(&f); KTEST_SKIP("out of memory"); }
    uint64_t other = FUTEX_TEST_VADDR + 0x40000000ULL;
    if (!vmm_map_user_page(f.as, other, f.frame)) {
        futex_fixture_down(&f);
        KTEST_SKIP("out of memory");
    }
    const void *k0 = NULL, *k1 = NULL;
    KTEST_ASSERT_EQ(futex_key(f.as, FUTEX_TEST_VADDR, &k0), 0);
    KTEST_ASSERT_EQ(futex_key(f.as, other, &k1), 0);
    KTEST_ASSERT(k0 && k0 == k1);
    futex_fixture_down(&f);
}
