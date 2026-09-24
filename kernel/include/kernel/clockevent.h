#ifndef CLOCKEVENT_H
#define CLOCKEVENT_H

#include <stdint.h>
#include "timer.h" // PIT_HZ

// The device that decides WHEN to interrupt -- Linux's
// clock_event_device, and the half clocksource.h says is missing here.
// A clocksource is READ; a clockevent FIRES. Keeping them apart is what
// stops a tick rate meaning both "how often we interrupt" and "how
// precisely we can measure".
//
// Two implementations: the 8259-routed PIT (periodic only), and the
// Local APIC timer (periodic or one-shot).
// The LAPIC one is what SMP needs -- the PIT delivers ONE interrupt for
// the whole machine, so a second core would have nothing to preempt it
// (docs/smp-design.md, stage 2).

// Matching clocksource.h's scale, and for the same reason: a number a
// reader can compare. The PIT is correct and machine-wide; the LAPIC
// timer is correct and per core.
#define CLOCKEVENT_RATING_PIT   100
#define CLOCKEVENT_RATING_LAPIC 200

struct clockevent {
    const char *name;

    // Begin delivering clockevent_tick() at `hz`. Returns 1, or 0 if
    // this device cannot -- no hardware, or a calibration that did not
    // converge -- in which case the incumbent keeps the tick.
    int (*start)(uint32_t hz);

    // Stop delivering. Called on the OUTGOING device once its
    // replacement is already running, so the machine is never without a
    // tick and never has to be rescued by one it just turned off.
    void (*stop)(void);

    // Interrupt ONCE, `ns` from now, replacing whatever was armed --
    // including a periodic tick, which it stops. NULL for a device that
    // can only tick periodically, which keeps the machine on the
    // periodic path. Linux's set_next_event().
    void (*set_oneshot)(uint64_t ns);

    int rating;      // higher wins; ties keep the incumbent
    uint8_t per_cpu; // one instance per core, rather than one per machine
};

// Registers a device, and hands it the tick if it outranks the
// incumbent AND its start() succeeds. REFUSED if it is
// self-contradictory -- no start(), no stop(), or a rating of zero.
int clockevent_register(const struct clockevent *ce);

// What a device's interrupt handler calls, and the reason a device
// registers rather than wiring itself to the scheduler: THE TICK BODY
// LIVES HERE, so a second device cannot drift from the first. `regs` is
// the saved-register block, which scheduler_tick() may repoint at
// another process -- see scheduler.c.
void clockevent_tick(uint64_t *regs);

const struct clockevent *clockevent_current(void);
uint32_t clockevent_hz(void);

// --- one-shot mode and the tickless idle -------------------------------
//
// ONE-SHOT ("highres"): the device is armed for the NEXT thing that has
// to happen -- a sleeper's deadline, the running slice's end, the next
// tick -- rather than ticking and checking. Linux's hrtimer mode. Needs a
// device with set_oneshot() AND a clocksource that runs without the tick
// (clocksource_deadline_capable()), or "when is the next thing" has no
// clock to be measured on. `option highres`, `highres=off`.
//
// TICKLESS IDLE: in one-shot mode, the tick is left out of that minimum
// while the kernel context halts in clockevent_idle_halt() with nothing
// runnable -- NO_HZ_IDLE. `option tick`, `nohz=off`. **ONLY THAT HELPER
// STOPS THE TICK**: any other wait loop keeps it, which is the safe
// default for a loop whose pollers nobody has taught to ask for a wake.

// Picks the mode, once the tick device and the clocksource are both
// final. From clockevent_init_lapic().
void clockevent_select_mode(void);

// Re-arms the one-shot device for whatever is next. Called on every
// context switch (the slice and the deadlines changed) and at the end of
// the tick handler. No-op in periodic mode. IF must be clear.
void clockevent_reprogram(void);

// The kernel context's idle wait: `sti; hlt`, with the tick stopped for
// the duration when nothing is runnable. Call it where a wait loop would
// `hlt`, AFTER scheduler_idle() -- whose pollers ask for their next wake
// through clockevent_idle_wake_by() in the same pass.
void clockevent_idle_halt(void);

// "Wake the idle loop by this clocksource_now_ns()" -- for idle work
// that is due at a time rather than on an interrupt: a cache flush, a
// polled device, a link check. Cleared after each halt, so a poller asks
// again every pass for as long as it has something due.
void clockevent_idle_wake_by(uint64_t deadline_ns);

// The same, for a deadline kept in pit_ticks() -- the coarse clock most
// idle work already times itself with.
static inline void clockevent_idle_wake_at_tick(uint64_t pit_tick) {
    clockevent_idle_wake_by(pit_tick * (1000000000ull / PIT_HZ));
}

// Is the kernel context inside clockevent_idle_halt()? The scheduler's
// "switch to a woken process NOW rather than at the slice's end" test.
int clockevent_in_idle(void);

struct clockevent_stats {
    uint8_t  oneshot;      // deadlines, not a periodic tick
    uint8_t  nohz;         // the idle loop may stop the tick
    uint8_t  stopped;      // it is stopped right now
    uint64_t events;       // device interrupts, of any kind
    uint64_t ticks;        // of those, periodic ticks
    uint64_t idle_stops;   // times the idle loop stopped the tick
    uint64_t stopped_ns;   // total time spent stopped
};
void clockevent_get_stats(struct clockevent_stats *out);

// Registers the PIT device, from idt_init() -- which has just
// programmed the hardware this describes.
void clockevent_init(void);

// Offers the LAPIC timer. From kernel_main(), after lapic_init(),
// because calibrating it needs a LAPIC to count AND a PIT already
// ticking to count against. **It also needs interrupts ON**: the
// reference is pit_ticks(), which only advances from the timer
// interrupt, so calibrating with IF clear waits forever -- the same
// deadlock cpuinfo.h describes for the TSC. lapic_ce_start() refuses
// rather than hanging.
void clockevent_init_lapic(void);

int clockevent_summary(char *buf, uint32_t cap);

#endif
