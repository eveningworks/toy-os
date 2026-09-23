// The sleeping lock, and the one property that is hard to get right.
//
// WHAT THESE CANNOT REACH: contention between two contexts. A KTEST
// runs in the kernel context, which has no scheduler slot, so
// scheduler_block_kernel() refuses it and a second lock attempt here
// would spin rather than park -- forever, since nothing else is running
// to release it. The sleeping path is exercised by every ring-3
// process doing file I/O, which is `usertest_run.py` and the whole GUI
// suite; what is checked here is the state machine underneath it.
#include "ktest.h"
#include "kmutex.h"
#include "scheduler.h"
#include "fs.h"
#include "proc_info.h" // PROC_STATE_*

KTEST("kmutex", "a fresh lock is free, and lock/unlock move it") {
    struct kmutex m = {0};
    KTEST_ASSERT(!kmutex_held(&m));
    kmutex_lock(&m);
    KTEST_ASSERT(kmutex_held(&m));
    kmutex_unlock(&m);
    KTEST_ASSERT(!kmutex_held(&m));
}

KTEST("kmutex", "the holder is named while held, and nobody after") {
    struct kmutex m = {0};
    kmutex_lock(&m);
    // The kernel context is pid 0 here, which is the same answer
    // scheduler_current_pid() gives it -- the point is that it MATCHES,
    // not that it is nonzero.
    KTEST_ASSERT_EQ(kmutex_owner(&m), scheduler_current_pid());
    kmutex_unlock(&m);
    KTEST_ASSERT_EQ(kmutex_owner(&m), 0);
}

// **THE ONE THAT WOULD CATCH A LOCK TAKEN AND DROPPED AROUND THE EDGES
// OF A BACKEND CALL RATHER THAN HELD ACROSS IT.** An fs_list() callback
// runs in the middle of the backend's own walk, so if it can see the
// lock held, FS_OP is wrapping the whole call. A version that locked
// only to look up the mount and released before calling the backend
// would pass every other filesystem test in the tree and fail this one.
static int g_saw_held;
static int g_saw_owner;
static void lock_probe(const char *name, uint32_t size, int is_dir) {
    (void)name; (void)size; (void)is_dir;
    if (fs_lock_held()) g_saw_held = 1;
    g_saw_owner = fs_lock_owner();
}

KTEST("kmutex", "the filesystem holds it for the whole backend call") {
    g_saw_held = 0;
    g_saw_owner = -1;
    KTEST_ASSERT(!fs_lock_held());          // nothing in flight out here
    fs_list("/", lock_probe);
    KTEST_ASSERT(g_saw_held);               // ...and held in there
    KTEST_ASSERT_EQ(g_saw_owner, scheduler_current_pid());
    KTEST_ASSERT(!fs_lock_held());          // released on the way out
}

// **THE CHECK THAT FINDS A DEADLOCK BEFORE THE RACE DOES.** A caller
// holding the preemption guard can neither sleep nor be rotated away,
// so behind a holder asleep in a disk wait it spins forever. That race
// is rare; the call site is not, so the take is reported on ENTRY --
// contended or not. The nested take is the control: taking a lock you
// already hold waits for nobody, whatever the guard says.
KTEST("kmutex", "a take under the preemption guard is reported, a nested one is not") {
    struct kmutex m = {0};
    unsigned before = kmutex_atomic_takes();

    kmutex_lock(&m);                        // plain: not atomic
    unsigned plain = kmutex_atomic_takes() - before;

    scheduler_preempt_disable();
    kmutex_lock(&m);                        // nested under the guard
    unsigned nested = kmutex_atomic_takes() - before;
    kmutex_unlock(&m);
    kmutex_unlock(&m);

    kmutex_lock(&m);                        // fresh under the guard
    unsigned fresh = kmutex_atomic_takes() - before;
    kmutex_unlock(&m);
    scheduler_preempt_enable();

    KTEST_ASSERT_EQ(plain, 0);
    KTEST_ASSERT_EQ(nested, 0);
    KTEST_ASSERT_EQ(fresh, 1);
}

KTEST("kmutex", "trylock takes a free or own lock, and never waits") {
    struct kmutex m = {0};
    KTEST_ASSERT(kmutex_trylock(&m));        // free
    KTEST_ASSERT(kmutex_trylock(&m));        // ours: nests
    kmutex_unlock(&m);
    KTEST_ASSERT(kmutex_held(&m));           // still held once
    kmutex_unlock(&m);
    KTEST_ASSERT(!kmutex_held(&m));

    // Held by SOMEBODY ELSE: fabricated, since a KTEST has no second
    // context to hold it. A lock that waited here would hang the suite.
    m.depth = 1; m.owner = 9999; m.owned = 1;
    KTEST_ASSERT(!kmutex_trylock(&m));
    KTEST_ASSERT_EQ(m.depth, 1);
    m.depth = 0; m.owned = 0; m.owner = 0;
}

// **AN UNLOCK WITH A SLEEPER GIVES IT THE LOCK, rather than dropping it
// for whoever runs next.** Dropped, a releaser making back-to-back file
// calls can re-take it before the woken waiter is scheduled, every
// time. The waiter is FABRICATED (a KTEST has
// no second context to park); the release with nobody parked is the
// control, and must leave the lock free.
KTEST("kmutex", "an unlock hands the lock to a parked waiter, not to the floor") {
    struct kmutex m = {0};
    uint64_t tf[SCHED_TF_SLOTS] = {0};

    scheduler_preempt_disable();
    kmutex_lock(&m);
    int w = scheduler_test_park(tf, &m, SCHED_WAIT_LOCK);
    if (w >= 0) scheduler_test_park_deadline(w, 0, 1);   // parked mid-call
    kmutex_unlock(&m);
    int owner = kmutex_owner(&m), held = kmutex_held(&m), handed = m.handed;
    int state = w >= 0 ? scheduler_test_state(w) : -1;
    scheduler_test_park_deadline(w, 0, 0);
    scheduler_test_release(w);
    scheduler_test_take_resched();

    struct kmutex c = {0};                 // the control: nobody parked
    kmutex_lock(&c);
    kmutex_unlock(&c);
    scheduler_preempt_enable();

    if (w < 0) KTEST_SKIP("no free process slots to fabricate");
    KTEST_ASSERT(held);                    // not dropped...
    KTEST_ASSERT_EQ(owner, w + 1);         // ...but the waiter's now
    KTEST_ASSERT_EQ(handed, 1);            // for it to claim on resume
    KTEST_ASSERT_EQ(state, PROC_STATE_READY);
    KTEST_ASSERT(!kmutex_held(&c));
}
