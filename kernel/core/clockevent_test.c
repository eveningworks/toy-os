// Tests for the tick source. See kernel/clockevent.h.
#include "ktest.h"
#include "clockevent.h"
#include "lapic.h"
#include "timer.h"
#include "string.h"
#include "barrier.h" // cpu_relax()
#include "clocksource.h"
#include "scheduler.h"
#include "multiboot.h"

// How long to wait for the tick to advance. A COUNT rather than a
// deadline, because the thing under test IS the clock: bounding a wait
// for the tick with the tick is how a dead one hangs the suite instead
// of failing it.
#define TICK_WAIT_SPINS 200000000u

// Spins until a tick INTERRUPT is delivered. Returns 0 if none was.
// Not pit_ticks(): that is derived from the clocksource wherever it can
// be, so it moves with no interrupt at all -- which let this pass, and
// the delivery check below fail, depending on where a 10 ms edge fell.
static int wait_a_tick(void) {
    uint64_t t0 = timer_irq_ticks();
    for (uint32_t i = 0; i < TICK_WAIT_SPINS; i++) {
        if (timer_irq_ticks() != t0) return 1;
        cpu_relax();
    }
    return 0;
}

KTEST("clockevent", "some device holds the tick") {
    const struct clockevent *ce = clockevent_current();
    KTEST_ASSERT(ce != 0);
    KTEST_ASSERT(ce->name != 0);
    KTEST_ASSERT_EQ((int)clockevent_hz(), CONFIG_HZ);
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

// --- one-shot mode and the tickless idle --------------------------------

static int highres_off(void) {
    char v[8];
    return multiboot_cmdline_value("highres=", v, sizeof v) && k_strcmp(v, "off") == 0;
}

// Whether the MODE was taken where it could be. A kernel that quietly
// stayed periodic passes every other test in this file.
KTEST("clockevent", "one-shot where the device, the clock and the build allow it") {
    if (!lapic_present()) KTEST_SKIP("no LAPIC -- the PIT is periodic-only");
    if (!clocksource_deadline_capable()) KTEST_SKIP("the clocksource stops with the tick");
    if (!CONFIG_HIGHRES || highres_off()) KTEST_SKIP("highres is off");

    struct clockevent_stats st;
    clockevent_get_stats(&st);
    KTEST_ASSERT_EQ(st.oneshot, 1);
}

// THE TICK STOPS IN THE IDLE HELPER. Counted in periodic ticks across a
// 50 ms idle: a running tick delivers 50*HZ/1000 of them, a stopped one
// next to none. Retried, because a process waking mid-window restarts
// the tick for as long as it runs -- which is correct, and not this.
KTEST("clockevent", "the idle helper stops the tick") {
    struct clockevent_stats a, b;
    clockevent_get_stats(&a);
    if (!a.nohz) KTEST_SKIP("the idle keeps its tick (periodic, nohz=off or option tick)");

    uint64_t periodic = 50ull * CONFIG_HZ / 1000;
    uint64_t got = 0;
    for (int attempt = 0; attempt < 5; attempt++) {
        clockevent_get_stats(&a);
        uint64_t deadline = clocksource_now_ns() + 50000000ull;
        while (clocksource_now_ns() < deadline) {
            clockevent_idle_wake_by(deadline);
            scheduler_idle_halt();
        }
        clockevent_get_stats(&b);
        got = b.ticks - a.ticks;
        if (b.idle_stops > a.idle_stops && got < periodic / 4) break;
    }
    KTEST_ASSERT(b.idle_stops > a.idle_stops);
    KTEST_ASSERT(got < periodic / 4);
}

// A DEADLINE ASKED FOR BY IDLE WORK IS KEPT, and kept precisely: each
// one sits half a tick past a tick boundary, where a periodic tick
// would be half a tick late every time.
KTEST("clockevent", "an idle deadline wakes on time, not on a tick") {
    struct clockevent_stats st;
    clockevent_get_stats(&st);
    if (!st.nohz) KTEST_SKIP("the idle keeps its tick");

    uint64_t late[9];
    int n = 0;
    for (int i = 0; i < 9; i++) {
        uint64_t now = clocksource_now_ns();
        uint64_t deadline = now - now % TICK_NS + 3 * TICK_NS + TICK_NS / 2;
        uint64_t t;
        while ((t = clocksource_now_ns()) < deadline) {
            clockevent_idle_wake_by(deadline);
            scheduler_idle_halt();
        }
        late[n++] = t - deadline;
    }
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && late[j - 1] > late[j]; j--) {
            uint64_t x = late[j]; late[j] = late[j - 1]; late[j - 1] = x;
        }
    KTEST_ASSERT(late[n / 2] < TICK_NS / 4);
}

// pit_ticks() RUNS WITH INTERRUPTS OFF where the clocksource does -- the
// property every `while (pit_ticks() - t < N)` inside a syscall needed
// and did not have.
KTEST("clockevent", "pit_ticks advances with interrupts off on a free-running clock") {
    if (!clocksource_deadline_capable()) KTEST_SKIP("the clocksource is the tick");
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    uint64_t t0 = pit_ticks();
    uint64_t end = clocksource_now_ns() + 3 * (1000000000ull / PIT_HZ);
    while (clocksource_now_ns() < end) cpu_relax();
    uint64_t t1 = pit_ticks();
    if (flags & (1ull << 9)) __asm__ volatile ("sti" ::: "memory");
    KTEST_ASSERT(t1 - t0 >= 2);
}

KTEST("clockevent", "the time slice refuses what it cannot honour") {
    uint32_t was = scheduler_timeslice_ms();
    KTEST_ASSERT_EQ(scheduler_set_timeslice_ms(0), 0);
    KTEST_ASSERT_EQ(scheduler_set_timeslice_ms(SCHED_TIMESLICE_MAX_MS + 1), 0);
    KTEST_ASSERT_EQ(scheduler_timeslice_ms(), was);
    KTEST_ASSERT_EQ(scheduler_set_timeslice_ms(7), 1);
    KTEST_ASSERT_EQ(scheduler_timeslice_ms(), 7u);
    scheduler_set_timeslice_ms(was);
}
