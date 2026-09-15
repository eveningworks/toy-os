// Two kernel contexts, two stacks, suspended and resumed mid-call.
//
// This is the assumption the whole of docs/blocking-design.md rests on:
// that a caller deep inside kernel code can be parked and picked up
// again where it left off, which is what a sleeping lock needs and what
// block_common() cannot do (it saves the RING-3 trapframe and the
// syscall re-runs).
//
// **IT CANNOT BE SHOWN WITHIN ONE CALL CHAIN**, and that is why there is
// a second stack here rather than a second save point:
// process_context_restore() puts RSP back inside the frames the save
// captured, and a caller that has since returned from them has given
// that memory to whatever ran next. The frames have to still be LIVE,
// so the two contexts have to be on different stacks -- which is
// exactly the shape a blocked process and its scheduler are in.
#include "ktest.h"
#include "context_switch.h"
#include "heap.h"
#include "scheduler.h"

// 16 KiB, the same size a process's kernel stack is -- this runs real
// kernel code and a KTEST has no more right to a small stack than
// anything else does.
#define CO_STACK 16384

static struct kernel_context g_main;   // the test's own context
static struct kernel_context g_co;     // the coroutine's
static volatile int g_steps;
// The first byte of the coroutine's locals found changed across the
// park, +1, or 0 if they all survived.
static volatile int g_clobbered;

// Runs on the OTHER stack. Never returns: it hands control back by
// restoring the test's context, which is what a blocked kernel caller
// would do to its scheduler.
static void co_entry(void *arg) {
    (void)arg;
    // **LOCALS THAT MUST SURVIVE THE PARK, and they are the whole
    // point.** An earlier version of this test kept its state in
    // globals, and then PASSED with the stack switch removed -- the
    // coroutine never read anything off its own stack, so nothing
    // noticed when that memory was handed to the test's next call.
    // Resuming mid-call is only worth anything if what the call had on
    // its stack is still there.
    volatile unsigned char keep[512];
    for (int i = 0; i < (int)sizeof keep; i++) keep[i] = (unsigned char)(i * 7 + 3);
    g_steps = 1;

    // Park MID-CALL and hand control back. The frames from here down
    // stay live on the coroutine's own stack while the test runs.
    if (process_context_save(&g_co) == 0)
        process_context_restore(&g_main, 1);

    // Resumed inside the same call. Everything above must be intact.
    for (int i = 0; i < (int)sizeof keep; i++)
        if (keep[i] != (unsigned char)(i * 7 + 3)) { g_clobbered = i + 1; break; }
    g_steps = 2;
    process_context_restore(&g_main, 2);
}

// Work of the kind that happens between a park and a resume -- a
// scheduler picking the next runnable does not do it in fifty bytes.
// **WITHOUT THIS THE POSITIVE CONTROL DOES NOT BITE**: the test's own
// calls between the two halves were too shallow to reach the
// coroutine's locals, so removing the stack switch changed no result
// and the test proved nothing about needing one.
// RECURSIVE, with a small frame each: one 4 KB local would blow the
// kernel's -Wframe-larger-than=1024 budget, and DEPTH is what this
// needs rather than width.
//
// **THE RETURN VALUE IS LOad-BEARING.** Written first as a plain
// `if (depth) stack_churn(depth - 1);`, which is a TAIL CALL that -O2
// turns into a loop reusing one frame -- 256 bytes of churn instead of
// four kilobytes, and the positive control silently stopped biting.
// Using the result after the call forces a real frame per level.
static int stack_churn(int depth) {
    volatile unsigned char scratch[256];
    for (int i = 0; i < (int)sizeof scratch; i++) scratch[i] = (unsigned char)(i ^ 0x5A);
    int deeper = depth > 0 ? stack_churn(depth - 1) : 0;
    return deeper + scratch[0];
}

KTEST("kctx", "a context runs on its own stack and resumes mid-call") {
    void *stack = kmalloc(CO_STACK);
    KTEST_ASSERT(stack != 0);
    g_steps = 0;
    g_clobbered = 0;

    // PREEMPTION OFF for the whole thing: this hands the CPU between two
    // contexts by hand, and a tick landing in the middle would find a
    // scheduler that knows nothing about either of them.
    scheduler_preempt_disable();

    int rc = process_context_save(&g_main);
    if (rc == 0) {
        process_context_enter((char *)stack + CO_STACK, co_entry, 0);
        // unreachable: enter() never returns
    }

    // rc == 1: the coroutine parked itself. Its stack is intact and it
    // has not finished.
    if (rc == 1) {
        KTEST_ASSERT_EQ(g_steps, 1);
        (void)stack_churn(15);  // ~4 KB of depth the coroutine's frames must survive
        int rc2 = process_context_save(&g_main);
        if (rc2 == 0) process_context_restore(&g_co, 9);
        // rc2 == 2: it ran on from where it parked.
        KTEST_ASSERT_EQ(rc2, 2);
    }

    scheduler_preempt_enable();
    KTEST_ASSERT_EQ(g_steps, 2);   // both halves ran, in order
    // The parked call's own stack survived the excursion.
    KTEST_ASSERT_EQ(g_clobbered, 0);
    kfree(stack);
}
