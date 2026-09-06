// The tick-source registry. See kernel/clockevent.h for what a
// clockevent IS and why it is a separate concept from a clocksource.
#include "clockevent.h"
#include "timer.h"      // timer_tick_advance(), PIT_HZ
#include "scheduler.h"  // scheduler_tick()
#include "pic.h"
#include "klog.h"
#include "kfmt.h"

static const struct clockevent *g_ce;
static uint32_t g_hz;

void clockevent_tick(uint64_t *regs) {
    timer_tick_advance();
    scheduler_tick(regs);
}

const struct clockevent *clockevent_current(void) { return g_ce; }
uint32_t clockevent_hz(void) { return g_hz; }

int clockevent_register(const struct clockevent *ce) {
    if (!ce || !ce->start || !ce->stop || ce->rating <= 0) return 0;
    if (g_ce && ce->rating <= g_ce->rating) return 0;

    uint32_t hz = g_hz ? g_hz : PIT_HZ;
    if (!ce->start(hz)) {
        klog_printf("clockevent: %s did not start -- tick stays on %s\n",
                    ce->name, g_ce ? g_ce->name : "nothing");
        return 0;
    }

    // THE OUTGOING DEVICE IS STOPPED ONLY ONCE ITS REPLACEMENT IS
    // RUNNING. The other order leaves a failed start() with no tick at
    // all -- and nothing able to deliver one to notice. The cost is a
    // window of a few microseconds where both fire, which double-counts
    // at most one tick.
    const struct clockevent *old = g_ce;
    g_ce = ce;
    g_hz = hz;
    if (old) old->stop();

    klog_printf("clockevent: tick on %s at %u Hz%s\n",
                ce->name, hz, ce->per_cpu ? ", per cpu" : "");
    return 1;
}

int clockevent_summary(char *buf, uint32_t cap) {
    if (!buf || cap == 0) return 0;
    if (!g_ce) {
        k_snprintf(buf, cap, "none");
        return 1;
    }
    k_snprintf(buf, cap, "%s at %u Hz%s", g_ce->name, g_hz,
               g_ce->per_cpu ? ", per cpu" : "");
    return 1;
}

// --- the PIT ---------------------------------------------------------

// Programming the hardware is pit_init()'s; this only decides whether
// the line is unmasked, because that is what "is this device driving
// the tick" means for something routed through the 8259.
static int pit_ce_start(uint32_t hz) {
    pit_init(hz);
    pic_clear_mask(0);
    return 1;
}

static void pit_ce_stop(void) {
    // Masked, not stopped. Channel 0 keeps counting so that anything
    // calibrating against the PIT still can, and re-taking the tick is
    // one write rather than a reprogram.
    pic_set_mask(0);
}

static const struct clockevent g_pit_ce = {
    .name    = "pit",
    .start   = pit_ce_start,
    .stop    = pit_ce_stop,
    .rating  = CLOCKEVENT_RATING_PIT,
    .per_cpu = 0,
};

void clockevent_init(void) {
    clockevent_register(&g_pit_ce);
}
