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
