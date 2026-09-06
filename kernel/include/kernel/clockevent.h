#ifndef CLOCKEVENT_H
#define CLOCKEVENT_H

#include <stdint.h>

// The device that decides WHEN to interrupt -- Linux's
// clock_event_device, and the half clocksource.h says is missing here.
// A clocksource is READ; a clockevent FIRES. Keeping them apart is what
// stops a tick rate meaning both "how often we interrupt" and "how
// precisely we can measure".
//
// Two implementations: the 8259-routed PIT, and the Local APIC timer.
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
