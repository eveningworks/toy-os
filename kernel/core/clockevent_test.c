// Tests for the tick source. See kernel/clockevent.h.
#include "ktest.h"
#include "clockevent.h"
#include "lapic.h"
#include "timer.h"
#include "string.h"
#include "barrier.h" // cpu_relax()

// How long to wait for the tick to advance. A COUNT rather than a
// deadline, because the thing under test IS the clock: bounding a wait
// for the tick with the tick is how a dead one hangs the suite instead
// of failing it.
#define TICK_WAIT_SPINS 200000000u

// Spins until pit_ticks() moves. Returns 0 if it never did.
static int wait_a_tick(void) {
    uint64_t t0 = pit_ticks();
    for (uint32_t i = 0; i < TICK_WAIT_SPINS; i++) {
        if (pit_ticks() != t0) return 1;
        cpu_relax();
    }
    return 0;
}

KTEST("clockevent", "some device holds the tick") {
    const struct clockevent *ce = clockevent_current();
    KTEST_ASSERT(ce != 0);
    KTEST_ASSERT(ce->name != 0);
    KTEST_ASSERT_EQ((int)clockevent_hz(), PIT_HZ);
}

KTEST("clockevent", "the tick advances") {
    KTEST_ASSERT(wait_a_tick());
}

// THE DELIVERY CHECK, and the reason it counts interrupts rather than
// reading a register: a LAPIC timer that is configured and not
// delivering leaves the tick on a masked PIT, which is a machine that
// has already stopped. Asking the LVT what it holds would pass either
// way. Removing the arming write in lapic_ce_start() reddens this.
KTEST("clockevent", "the lapic timer delivers when it holds the tick") {
    if (!lapic_present()) KTEST_SKIP("no LAPIC (nomsi, or a CPU without one)");

    const struct clockevent *ce = clockevent_current();
    KTEST_ASSERT(ce != 0);
    KTEST_ASSERT_EQ(k_strcmp(ce->name, "lapic-timer"), 0);
    KTEST_ASSERT(ce->per_cpu != 0);
    KTEST_ASSERT(lapic_timer_rate() > 0);

    uint32_t before = lapic_timer_ticks();
    KTEST_ASSERT(wait_a_tick());
    KTEST_ASSERT(lapic_timer_ticks() > before);
}

// A device that cannot say what it is must not take the tick -- the
// same honesty check clocksource_register() applies.
KTEST("clockevent", "an incomplete device is refused") {
    struct clockevent broken = { .name = "broken", .rating = 9999 };
    KTEST_ASSERT_EQ(clockevent_register(&broken), 0);
    KTEST_ASSERT_EQ(clockevent_register(0), 0);

    // ...and the incumbent still holds it.
    KTEST_ASSERT(clockevent_current() != 0);
    KTEST_ASSERT_EQ(k_strcmp(clockevent_current()->name, "broken") != 0, 1);
}
