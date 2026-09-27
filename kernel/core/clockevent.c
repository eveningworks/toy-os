// The tick-source registry. See kernel/clockevent.h for what a
// clockevent IS and why it is a separate concept from a clocksource.
#include "clockevent.h"
#include "timer.h"      // timer_tick_advance(), CONFIG_HZ, TICK_NS
#include "scheduler.h"  // scheduler_tick(), scheduler_timer_event()
#include "clocksource.h"
#include "serial.h"     // serial_tx_pending()
#include "multiboot.h"  // nohz=off, highres=off
#include "string.h"
#include "irq.h"
#include "klog.h"
#include "kfmt.h"
#include "kdebug.h"     // kdebug_poll(), and a tick that stays on for it

static const struct clockevent *g_ce;
static uint32_t g_hz;

static int g_oneshot;            // armed per deadline, not ticking
static int g_nohz;               // the idle helper may stop the tick
static volatile int g_idle;      // kernel context is inside the idle helper
static int g_stopped;            // the tick is out of the deadline minimum
static uint64_t g_next_tick_ns;  // when the next periodic tick is due
static uint64_t g_idle_wake_ns;  // idle pollers' earliest ask, 0 = none
static uint64_t g_stopped_at_ns;

static uint64_t g_events, g_ticks, g_idle_stops, g_stopped_ns;

static uint64_t next_boundary(uint64_t now) {
    return now - now % TICK_NS + TICK_NS;
}

// THE ONE-SHOT TICK BODY. The periodic tick is one deadline among
// several here, so an interrupt may be a tick, a sleeper coming due, a
// slice ending -- or early, which is harmless: nothing is due, and the
// device is simply re-armed.
void clockevent_tick(uint64_t *regs) {
    g_events++;
    kdebug_poll(regs);
    if (!g_oneshot) {
        g_ticks++;
        timer_tick_advance();
        scheduler_tick(regs);
        return;
    }

    uint64_t now = clocksource_now_ns();
    if (!g_stopped && now >= g_next_tick_ns) {
        g_ticks++;
        timer_tick_advance();
        g_next_tick_ns += TICK_NS;
        if (g_next_tick_ns <= now) g_next_tick_ns = next_boundary(now);
    }
    // MAY NOT RETURN until this context is resumed: a switch re-arms the
    // device itself (clockevent_reprogram() from the scheduler's switch),
    // so the re-arm below is for the path that stayed put.
    scheduler_timer_event(regs, now);
    clockevent_reprogram();
}

// A process is about to run, so a stopped tick restarts -- whichever
// path chose it. Folded into reprogram so no switch can forget.
static void tick_restart(uint64_t now) {
    if (!g_stopped) return;
    g_stopped = 0;
    g_stopped_ns += now - g_stopped_at_ns;
    g_next_tick_ns = next_boundary(now);
}

void clockevent_reprogram(void) {
    if (!g_oneshot) return;
    uint64_t now = clocksource_now_ns();

    // Stopped only while the kernel context idles with nothing to run.
    // Anything else -- a switch into a process from the stopped idle, a
    // wake by an IRQ -- restarts it here.
    if (g_stopped && !(g_idle && scheduler_kernel_running() && !scheduler_any_ready()))
        tick_restart(now);

    uint64_t next = g_stopped ? now + clocksource_max_idle_ns() : g_next_tick_ns;
    uint64_t sched = scheduler_next_event_ns();
    if (sched && sched < next) next = sched;
    if (g_stopped && g_idle_wake_ns && g_idle_wake_ns < next) next = g_idle_wake_ns;

    g_ce->set_oneshot(next > now ? next - now : 0);
}

void clockevent_idle_wake_by(uint64_t deadline_ns) {
    if (!g_idle_wake_ns || deadline_ns < g_idle_wake_ns) g_idle_wake_ns = deadline_ns;
}

int clockevent_in_idle(void) { return g_idle; }

static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}

void clockevent_idle_halt(void) {
    uint64_t flags = irq_save();
    g_idle = 1;
    if (g_oneshot) {
        uint64_t now = clocksource_now_ns();
        // The tick is also what hears a debugger's break-in.
        if (g_nohz && !scheduler_any_ready() && !serial_tx_pending() && !kdebug_armed()) {
            if (!g_stopped) {
                g_stopped = 1;
                g_stopped_at_ns = now;
                g_idle_stops++;
            }
        } else if (scheduler_any_ready()) {
            // Idle with a process ready: hand over NOW, not at the next
            // tick. scheduler_timer_event() sees the idle flag and rotates.
            g_ce->set_oneshot(0);
            goto halt;
        }
        clockevent_reprogram();
    }
halt:
    // STI's one-instruction shadow makes `sti; hlt` atomic: an interrupt
    // that became pending while IF was clear wakes this hlt rather than
    // landing between the two and leaving it asleep.
    __asm__ volatile ("sti; hlt; cli" ::: "memory");
    g_idle = 0;
    g_idle_wake_ns = 0;
    if (g_stopped) {
        tick_restart(clocksource_now_ns());
        clockevent_reprogram();
    }
    if (flags & (1ull << 9)) __asm__ volatile ("sti" ::: "memory");
}

static int boot_flag_off(const char *key) {
    char v[8];
    return multiboot_cmdline_value(key, v, sizeof v) && k_strcmp(v, "off") == 0;
}

void clockevent_select_mode(void) {
    const char *why = 0;
    if (!CONFIG_HIGHRES) why = "option highres = no";
    else if (boot_flag_off("highres=")) why = "highres=off";
    else if (!g_ce || !g_ce->set_oneshot) why = "the tick device is periodic-only";
    else if (!clocksource_deadline_capable()) why = "the clocksource stops with the tick";
    if (why) {
        klog_printf("clockevent: periodic at %u Hz -- %s\n", g_hz, why);
        return;
    }

    uint64_t flags = irq_save();
    g_oneshot = 1;
    g_nohz = CONFIG_TICK_IDLE && !boot_flag_off("nohz=");
    g_next_tick_ns = next_boundary(clocksource_now_ns());
    clockevent_reprogram();
    if (flags & (1ull << 9)) __asm__ volatile ("sti" ::: "memory");

    klog_printf("clockevent: one-shot on %s, tick %u Hz, idle %s\n", g_ce->name, g_hz,
                g_nohz ? "tickless" : (CONFIG_TICK_IDLE ? "ticking (nohz=off)"
                                                        : "ticking (option tick = periodic)"));
}

void clockevent_get_stats(struct clockevent_stats *out) {
    uint64_t flags = irq_save();
    out->oneshot = (uint8_t)g_oneshot;
    out->nohz = (uint8_t)g_nohz;
    out->stopped = (uint8_t)g_stopped;
    out->events = g_events;
    out->ticks = g_ticks;
    out->idle_stops = g_idle_stops;
    out->stopped_ns = g_stopped_ns
        + (g_stopped ? clocksource_now_ns() - g_stopped_at_ns : 0);
    if (flags & (1ull << 9)) __asm__ volatile ("sti" ::: "memory");
}

const struct clockevent *clockevent_current(void) { return g_ce; }
uint32_t clockevent_hz(void) { return g_hz; }

int clockevent_register(const struct clockevent *ce) {
    if (!ce || !ce->start || !ce->stop || ce->rating <= 0) return 0;
    if (g_ce && ce->rating <= g_ce->rating) return 0;

    uint32_t hz = g_hz ? g_hz : CONFIG_HZ;
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
    k_snprintf(buf, cap, "%s at %u Hz%s%s", g_ce->name, g_hz,
               g_oneshot ? (g_nohz ? ", one-shot, tickless idle" : ", one-shot") : ", periodic",
               g_ce->per_cpu ? ", per cpu" : "");
    return 1;
}

// --- the PIT ---------------------------------------------------------

// Programming the hardware is pit_init()'s; this only decides whether
// the line is unmasked, because that is what "is this device driving
// the tick" means for something routed through the 8259.
static int pit_ce_start(uint32_t hz) {
    pit_init(hz);
    irq_unmask(0);
    return 1;
}

static void pit_ce_stop(void) {
    // Masked, not stopped. Channel 0 keeps counting so that anything
    // calibrating against the PIT still can, and re-taking the tick is
    // one write rather than a reprogram.
    irq_mask(0);
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
