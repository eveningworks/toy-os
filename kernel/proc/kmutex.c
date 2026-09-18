// The sleeping lock. See kmutex.h for what it is and what it replaced.
#include "kmutex.h"
#include "scheduler.h"
#include "barrier.h"   // cpu_relax()
#include "kfmt.h"      // klog_printf
#include "klog.h"

// The test-and-set has to be indivisible against everything else that
// can run on this CPU, and on one CPU that is exactly "no interrupt
// lands in the middle". A real atomic is what SMP needs and what
// docs/smp-design.md's later stage brings; `lock cmpxchg` here today
// would be a more expensive way to say the same thing.
static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    __asm__ volatile ("pushq %0; popfq" :: "r"(f) : "memory", "cc");
}

// **THE SPIN MUST NOT DISABLE PREEMPTION, and that is the opposite of
// what a spinlock usually does.** The holder of this lock is allowed to
// SLEEP -- that is the feature -- so a spinner that stopped the tick
// would stop the only thing that can ever schedule the holder again,
// and neither would move for the rest of the boot. So the spin is a
// plain `pause`: the tick rotates to the holder, it finishes, it
// unlocks, and this loop sees it.
//
// WHICH LEAVES ONE CASE THIS CANNOT FIX, and it reports rather than
// hangs: a spinner that ALREADY holds the preemption guard for its own
// reasons cannot be rotated away from either. scheduler_on_exit() is
// the live example -- it raises the guard and then releases descriptors,
// which can reach the disk. Nothing can sleep holding this lock yet
// (ata.c's wait is still a poll, docs/blocking-design.md stage 2), so
// the case is unreachable today; when that changes, the fix is for the
// guarded paths that reach the filesystem to stop being guarded, not
// for this loop to drop a guard it does not own.
#define SPIN_COMPLAINT 2000000u

static void spin_note(const struct kmutex *m, unsigned spins) {
    static int once;
    if (once || spins < SPIN_COMPLAINT || scheduler_preempt_depth() == 0) return;
    once = 1;
    klog_printf(KLOG_ERR "kmutex: spinning for pid %d with preemption off -- "
                "if the holder is asleep, neither of us can move\n", m->owner);
}

void kmutex_lock(struct kmutex *m) {
    unsigned spins = 0;
    for (;;) {
        int me = scheduler_current_pid();
        uint64_t f = irq_save();
        if (m->depth == 0) {
            m->depth = 1;
            m->owner = me;
            m->owned = 1;
            irq_restore(f);
            return;
        }
        // ALREADY OURS -- nest. Compared through `owned` rather than
        // against a sentinel pid, because the kernel context is pid 0
        // and is a real owner.
        if (m->owned && m->owner == me) {
            m->depth++;
            irq_restore(f);
            return;
        }
        irq_restore(f);

        // ARM BEFORE RE-TESTING. A kmutex_unlock() landing between the
        // test above and the park below would otherwise be lost, and
        // there is no ring-3 retry loop under this to paper over it --
        // the same rule scheduler_block_kernel() states.
        scheduler_wait_arm(m);
        if (m->depth == 0) { scheduler_wait_disarm(); continue; }
        if (scheduler_block_kernel(m, SCHED_WAIT_LOCK)) { spins = 0; continue; }
        scheduler_wait_disarm();

        spin_note(m, ++spins);
        cpu_relax();
    }
}

void kmutex_unlock(struct kmutex *m) {
    uint64_t f = irq_save();
    if (m->depth > 1) { m->depth--; irq_restore(f); return; }  // still ours
    m->depth = 0;
    m->owner = 0;
    m->owned = 0;
    irq_restore(f);
    // EVERY waiter, not one. Waking a single one is the cheaper move
    // and needs the wake to be guaranteed to reach a contender that
    // will actually take the lock; a loser here simply re-parks, and
    // the arm/armed_woken pair is what stops that re-park sleeping
    // through the next release.
    scheduler_wake(m, 0);
}

int kmutex_held(const struct kmutex *m)  { return m && m->depth > 0; }
int kmutex_owner(const struct kmutex *m) { return (m && m->owned) ? m->owner : 0; }
