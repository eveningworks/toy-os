// The monotonic-time registry. See kernel/clocksource.h for what a
// clocksource IS and why wall-clock time is deliberately not one.
//
// The accumulate-on-read design, and why it is not just "return
// read() * mult >> shift": a raw counter can WRAP (the mask), and a
// better source can arrive partway through boot. Both are handled by
// keeping a running nanosecond total plus the last raw value seen, so
// the only thing ever converted is a DELTA -- which makes wraparound a
// subtraction that works by construction, and makes switching sources
// a matter of resetting the last-raw without touching the total.
//
// The cost is that it must be READ often enough that a delta cannot
// overflow the multiply. That is what max_delta is, and the scheduler
// reading it on every context switch is what guarantees it in practice.
#include "clocksource.h"
#include "timer.h"
#include "barrier.h" // cpu_relax -- the delay below spins
#include "klog.h"
#include "kfmt.h"
#include "driver.h" // DRIVER_DECLARE, driver_bound -- `lsdrv`
#include "multiboot.h" // clocksource=
#include "string.h"

static const struct clocksource *g_cs;
static uint64_t g_last_raw;   // last raw value read from g_cs
static uint64_t g_acc_ns;     // nanoseconds accumulated before that point
static uint64_t g_max_delta;  // largest delta g_cs->mult can convert safely
static uint64_t g_max_idle_ns;

int clocksource_deadline_capable(void) {
    const struct clocksource *cs = clocksource_current();
    return cs && cs->irq_independent;
}

const struct clocksource *clocksource_current(void) { return g_cs; }

void clocksource_calc_mult_shift(uint32_t *mult, uint32_t *shift,
                                  uint64_t freq, uint32_t maxsec) {
    if (!freq) { *mult = 0; *shift = 0; return; }

    // The largest shift that keeps (maxsec worth of cycles) * mult
    // inside 64 bits. Walking DOWN from the biggest useful shift finds
    // the most precise pair rather than a merely adequate one -- and
    // the loop terminates at 0, which is always representable for any
    // frequency at or below 1GHz-per-unit.
    uint64_t maxcycles = (uint64_t)maxsec * freq;
    for (int sh = 32; sh >= 0; sh--) {
        uint64_t m = ((uint64_t)1000000000ULL << sh) / freq;
        if (m == 0) continue;              // shift too small to represent
        if (m > 0xFFFFFFFFULL) continue;   // multiplier does not fit
        // The product has to survive too, or the precision we just
        // bought is spent on an overflow.
        if (maxcycles && m > 0xFFFFFFFFFFFFFFFFULL / maxcycles) continue;
        *mult = (uint32_t)m;
        *shift = (uint32_t)sh;
        return;
    }
    // Unreachable for any sane frequency; a mult of 0 is refused by
    // clocksource_register() rather than silently stopping time.
    *mult = 0;
    *shift = 0;
}

// READ, CONVERT AND ACCUMULATE ARE ONE CRITICAL SECTION, and interrupts
// are what this is protecting against rather than preemption: the timer
// ISR reads this clock too (CPU accounting), so a caller that has read
// `raw` and not yet stored it can be overtaken by an ISR that stores a
// LATER one. The caller then resumes and computes raw - g_last_raw with
// g_last_raw one tick AHEAD -- a delta of (uint64_t)-1, which the clamp
// below turns into a ~7 second jump forward in accumulated time.
// scheduler_preempt_disable() cannot help here; the racing party is an
// interrupt handler, not another process.
static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}

static inline void irq_restore(uint64_t f) {
    if (f & (1ull << 9)) __asm__ volatile ("sti" ::: "memory");
}

static uint64_t clocksource_now_ns_locked(void);

int clocksource_register(const struct clocksource *cs) {
    // The honesty check. A source that cannot be read, cannot wrap, or
    // converts every delta to zero is not a worse clock -- it is a
    // stopped one, and a stopped clock installed over a working one is
    // the failure this refuses to perform quietly.
    if (!cs || !cs->read || !cs->mask || !cs->mult) {
        klog_printf(KLOG_ERR "clocksource: REFUSED %s -- incomplete (read=%d mask=%d mult=%u)\n",
                     cs && cs->name ? cs->name : "(unnamed)",
                     cs && cs->read ? 1 : 0, cs && cs->mask ? 1 : 0,
                     cs ? cs->mult : 0);
        return 0;
    }

    // Ties keep the incumbent, so registration ORDER cannot decide
    // which source wins -- only the rating can. `clocksource=` is the
    // one exception, in both directions.
    const char *forced = clocksource_forced();
    int is_forced = forced && k_strcmp(forced, cs->name) == 0;
    if (g_cs && forced && k_strcmp(forced, g_cs->name) == 0) {
        klog_printf("clocksource: %s kept behind %s -- clocksource=%s\n",
                     cs->name, g_cs->name, forced);
        return 0;
    }
    if (g_cs && !is_forced && cs->rating <= g_cs->rating) {
        klog_printf("clocksource: %s (rating %d) kept behind %s (rating %d)\n",
                     cs->name, cs->rating, g_cs->name, g_cs->rating);
        return 0;
    }

    // THE SWITCH IS ONE CRITICAL SECTION. The timer interrupt reads this
    // clock, and one landing between publishing `g_cs` and setting its
    // limits found a max_delta of 0: every read clamped, and at 1000 Hz
    // the ISR's warning outlasted the tick, so registration never
    // resumed -- a boot that spun printing "exceeds max 0" forever.
    uint64_t max_delta = 0xFFFFFFFFFFFFFFFFULL / cs->mult;
    uint64_t span = cs->mask < max_delta ? cs->mask : max_delta;
    uint64_t flags = irq_save();

    // Fold everything the outgoing source measured into the total
    // BEFORE switching, or the interval between its last read and now
    // is simply lost -- and with it, monotonicity across the switch.
    int had_source = g_cs != 0;
    if (had_source) (void)clocksource_now_ns_locked();

    g_last_raw = cs->read() & cs->mask;
    g_max_delta = max_delta;
    // THE FIRST SOURCE STARTS THE CLOCK AT ITS OWN ORIGIN, not at zero:
    // the PIT's tick count began before this ran, and pit_ticks() --
    // counted until now, derived from this clock later -- must not step
    // backwards at the switch.
    if (!had_source && g_last_raw <= g_max_delta)
        g_acc_ns = (g_last_raw * cs->mult) >> cs->shift;
    g_max_idle_ns = ((span >> 1) * cs->mult) >> cs->shift;
    g_cs = cs;
    irq_restore(flags);

    // Here, not at registration: a source that was refused or out-rated
    // drives nothing, and said so on the way past.
    driver_bound(cs->name, "clock0");

    klog_printf("clocksource: using %s (rating %d, mult %u shift %u)\n",
                 cs->name, cs->rating, cs->mult, cs->shift);
    return 1;
}

uint64_t clocksource_max_idle_ns(void) { return g_max_idle_ns; }

const char *clocksource_forced(void) {
    static char name[16];
    static int looked;
    if (!looked) {
        looked = 1;
        if (!multiboot_cmdline_value("clocksource=", name, sizeof name)) name[0] = 0;
    }
    return name[0] ? name : 0;
}

// The body, for a caller that already holds interrupts off. Reports a
// clamp through `*clamped`/`*reported` rather than logging it here: a
// print is far longer than the arithmetic, and inside the critical
// section it is inside the timer ISR's way too.
static uint64_t now_locked(int *clamped, uint64_t *reported) {
    uint64_t raw = g_cs->read() & g_cs->mask;
    // Masked subtraction, so a counter that wrapped since the last read
    // still yields the right delta -- this is the whole reason the
    // conversion is done on deltas rather than on the absolute value.
    uint64_t delta = (raw - g_last_raw) & g_cs->mask;

    *clamped = delta > g_max_delta;
    *reported = delta;   // before the clamp -- the offending value
    if (*clamped) {
        // Nothing read the clock for long enough that converting the
        // delta would overflow. Clamping loses time, which is bad --
        // but wrapping makes it go BACKWARDS, which breaks every
        // caller's arithmetic silently, so say so and lose it.
        delta = g_max_delta;
    }

    g_acc_ns += (delta * g_cs->mult) >> g_cs->shift;
    g_last_raw = raw;
    return g_acc_ns;
}

static uint64_t clocksource_now_ns_locked(void) {
    int clamped; uint64_t reported;
    return now_locked(&clamped, &reported);
}

uint64_t clocksource_now_ns(void) {
    if (!g_cs) return 0;

    uint64_t flags = irq_save();
    int clamped;
    uint64_t reported;
    uint64_t now = now_locked(&clamped, &reported);
    irq_restore(flags);

    if (clamped)
        klog_printf("clocksource: %s delta %lu exceeds max %lu -- time clamped\n",
                     g_cs->name, (unsigned long)reported,
                     (unsigned long)g_max_delta);

    return now;
}

// --- the PIT source ---------------------------------------------------
//
// Correct and coarse: it counts tick INTERRUPTS, so it advances in
// 1/CONFIG_HZ steps and can say nothing about anything shorter -- and
// nothing at all while the tick is stopped, which is why a tickless idle
// refuses to run on it. That is precisely the limitation the TSC and
// ACPI PM sources remove.
static uint64_t pit_cs_read(void) { return timer_irq_ticks(); }

DRIVER_DECLARE("pit", "clock", "8253/8254 interval timer, counted per tick");

static struct clocksource g_pit_cs = {
    .name   = "pit",
    .read   = pit_cs_read,
    .mask   = CLOCKSOURCE_MASK(64),
    .rating = CLOCKSOURCE_RATING_PIT,
};

void clocksource_delay_ms(uint32_t ms) {
    if (!ms) return;
    if (clocksource_deadline_capable()) {
        uint64_t end = clocksource_now_ns() + (uint64_t)ms * 1000000ull;
        while (clocksource_now_ns() < end) cpu_relax();
        return;
    }
    // THE FIRST TICK MAY LAND IMMEDIATELY, so ask for one more than the
    // arithmetic needs -- a caller that wanted 1 ms must not get 0.
    uint64_t start = timer_irq_ticks();
    uint64_t want = (uint64_t)ms * CONFIG_HZ / 1000 + 1;
    while (timer_irq_ticks() - start < want) cpu_relax();
}

void clocksource_init(void) {
    // A generous maxsec: this counter is 64-bit and increments
    // CONFIG_HZ times a second, so nothing here can overflow in any
    // realistic uptime -- the bound exists to pick the shift, not to guard.
    clocksource_calc_mult_shift(&g_pit_cs.mult, &g_pit_cs.shift, CONFIG_HZ, 3600);
    clocksource_register(&g_pit_cs);
}
