// SYS_WAIT_READY: a wait with a deadline that CONSUMES NOTHING.
//
// Driven by kernel/proc/win_input_test.c, which spawns this, gives it a
// head start with an empty queue, then pushes exactly one event. The
// exit code IS the assertion -- a BITMASK of the sub-checks that passed,
// so a partial failure names WHICH one (a count cannot: 3 of 4 is four
// different failures) -- which is why this prints nothing
// (same reasoning as event_test.c, whose serial console is shared with
// the KTEST report being parsed).
#include <stdint.h>
#include "rt/sys.h"

// 100 ms against a 100 Hz tick. The floor below is 8 rather than 10
// because a sleep's resolution is one tick and the arithmetic rounds:
// asserting the exact figure would make this a test of the tick rate.
#define TIMEOUT_MS   100
#define TIMEOUT_MIN  8
#define TIMEOUT_MAX  60   // generously above 10 -- a hang is what this catches

// Long enough that being woken by the pushed event is unmistakable
// against it. The KTEST pushes at about 40 ticks.
#define WAKE_MS      3000
#define WAKE_MAX     200  // 2 s in ticks; anything near 300 means it never woke

int main(void) {
    int passed = 0; // bit per sub-check, 1<<0 .. 1<<3

    // Holding the compositor role for the test means the devices may
    // have reported already; the queue has to be EMPTY for check 1.
    { struct win_event drain; while (sys_poll_event(&drain) == 1) { } }

    // 1. IT TIMES OUT. Nothing is queued yet, so this must block for
    //    about the whole timeout and report "not ready".
    unsigned long t0 = sys_ticks();
    int r = sys_wait_ready(TIMEOUT_MS);
    unsigned long spent = sys_ticks() - t0;
    if (r == 0 && spent >= TIMEOUT_MIN && spent <= TIMEOUT_MAX) passed |= 1 << 0;

    // 2. IT WAKES EARLY. The KTEST pushes one event while this is
    //    parked, and the deadline is far enough out that returning
    //    anywhere near it would mean the wake never landed.
    //
    //    The return value is deliberately NOT checked: a caller woken
    //    from the block gets 0, while one that found the event already
    //    queued gets 1, and which of those happens is a race with the
    //    pusher. That is exactly the distinction SYS_WAIT_READY says it
    //    does not make.
    t0 = sys_ticks();
    sys_wait_ready(WAKE_MS);
    spent = sys_ticks() - t0;
    if (spent <= WAKE_MAX) passed |= 1 << 1;

    // 3. AN EVENT ALREADY QUEUED IS REPORTED AT ONCE. A separate call
    //    from 2 on purpose: that one exercised the BLOCK-then-wake path,
    //    and this is the other one -- the queue is non-empty on entry,
    //    so it must answer 1 without parking. Without this the check
    //    below cannot fail, because the wake path has no opportunity to
    //    consume anything (a woken syscall returns through its saved
    //    trapframe and never re-runs the body). Found by a positive
    //    control that made the call consume and reddened nothing.
    if (sys_wait_ready(0) == 1) passed |= 1 << 2;

    // 4. AND IT CONSUMED NOTHING -- the load-bearing check. A wait that
    //    DELIVERED the event would leave the queue empty here, and the
    //    compositor that drains its own queue would silently lose one
    //    event per wait.
    struct win_event ev;
    if (sys_poll_event(&ev) == 1) passed |= 1 << 3;

    sys_exit(passed);
}
