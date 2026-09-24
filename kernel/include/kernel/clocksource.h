#ifndef CLOCKSOURCE_H
#define CLOCKSOURCE_H

#include <stdint.h>

// A source of MONOTONIC TIME, registered the way a display_driver or an
// fs_ops backend is: several may exist, the best-rated one wins, and no
// caller names the hardware.
//
// This is Linux's `clocksource` in miniature, and the split it encodes
// is the one worth keeping: TIMEKEEPING (a counter you read, this file)
// is a different job from TIMER EVENTS (deciding when to interrupt,
// which is kernel/clockevent.h). Conflating them is how a tick rate
// ends up meaning both "how often we interrupt" and "how precisely we can
// measure", which is exactly the confusion that produced this kernel's
// CPU-accounting bug: billing incremented a TICK COUNTER instead of
// asking a clock how much time had passed, so a yield -- microseconds
// long -- was charged a whole 10ms tick. See docs/decisions.md.
//
// WHAT THIS IS NOT: a wall clock. `ktime_read()` and caltime.c's epoch
// conversion answer "what time is it", which is a separate concept with
// separate failure modes (it jumps when the user sets it, and it says
// nothing about elapsed time). Linux keeps clocksource and RTC apart
// deliberately and so does this; putting the RTC behind this interface
// would be the mistake this file exists to make hard.

// A raw counter of `bits` significant bits, for the wraparound mask.
// A 64-bit counter takes CLOCKSOURCE_MASK(64) and never wraps in any
// time this kernel will run for.
#define CLOCKSOURCE_MASK(bits) \
    ((bits) >= 64 ? (uint64_t)~0ULL : (((uint64_t)1 << (bits)) - 1))

// Ratings, matching Linux's scale so the numbers mean something to
// anyone who has seen them before: 1-99 unfit for real timekeeping,
// 100-199 functional but coarse, 200-299 good, 300-399 ideal.
#define CLOCKSOURCE_RATING_PIT 110 // 10ms resolution -- correct, coarse
#define CLOCKSOURCE_RATING_ACPI_PM 200 // 279 ns, one port read each
#define CLOCKSOURCE_RATING_TSC 300 // sub-nanosecond, and free to read

struct clocksource {
    const char *name;

    // The raw counter. Must be monotonically increasing within `mask`
    // and must not require interrupts to be on -- it is read from
    // inside the scheduler and from syscall handlers, both with IF
    // clear.
    uint64_t (*read)(void);

    uint64_t mask;   // significant bits of what read() returns
    uint32_t mult;   // ns = (delta * mult) >> shift -- see calc below
    uint32_t shift;
    int rating;      // higher wins; ties keep the incumbent

    // DOES read() ADVANCE WITH INTERRUPTS OFF? The TSC does -- it is a
    // free-running CPU counter. The PIT source does NOT: its read() is
    // pit_ticks(), a count the timer INTERRUPT increments, so with IF
    // clear it stands still however long the caller waits.
    //
    // The distinction matters to any bounded WAIT: a deadline computed
    // from a source that stops never expires, which is why the xHCI
    // driver bounds its waits by a poll count wherever this is 0.
    uint8_t irq_independent;
};

// Registers a source. The highest-rated one becomes current; a lower
// or equal rating is kept in the list's spirit but does not take over,
// so registration ORDER never decides the outcome. Switching preserves
// monotonicity: the accumulated nanosecond count carries across, so
// clocksource_now_ns() never goes backwards when a better source
// arrives partway through boot.
//
// REFUSED if the source is self-contradictory -- no read(), a zero
// mask, or a zero mult (which would make every delta zero and stop
// time silently). Same honesty check display_probe() applies to a
// driver whose capability bits and function pointers disagree.
int clocksource_register(const struct clocksource *cs);

// Nanoseconds since the first source was registered. Monotonic, never
// decreasing, and safe to call with interrupts off.
//
// NOT free of a caveat: it accumulates on each call, so the raw counter
// must not advance by more than `max_delta` (computed at registration
// from mult, so the multiply cannot overflow 64 bits) between two
// calls. At a 3GHz TSC that is over an hour, and the scheduler reads it
// on every context switch, so nothing here comes close -- but a source
// added later with a much larger mult shrinks that window, which is why
// the clamp logs rather than wrapping quietly.
uint64_t clocksource_now_ns(void);

// The current source, or NULL before any is registered. For `lscpu`-
// style reporting and for the KTESTs, which need to know which one they
// are actually measuring -- a test that cannot say whether it ran
// against the PIT or the TSC is measuring something it cannot name.
const struct clocksource *clocksource_current(void);

// Can a WAIT be bounded by a deadline right now? True only when the
// current source advances with interrupts off -- see irq_independent.
// A caller that gets 0 must bound itself some other way; it must not
// fall back to spinning on clocksource_now_ns() forever.
int clocksource_deadline_capable(void);

// THE LONGEST THE CLOCK MAY GO UNREAD, in nanoseconds: half of what
// the current source can count before it wraps or overflows the
// conversion. Accumulate-on-read loses time past it, so a tickless idle
// that sleeps longer than this has to wake just to read the clock. The
// PM timer's 24-bit counter wraps in 4.7 s, which is why this exists.
uint64_t clocksource_max_idle_ns(void);

// BUSY-WAIT `ms` MILLISECONDS, USING THE BEST SOURCE AVAILABLE.
//
// **THE POINT IS THAT IT DOES NOT NEED INTERRUPTS.** Five drivers had
// hand-rolled the same spin on `pit_ticks()`, and a tick counter only
// advances on a timer interrupt -- so every one of them was an infinite
// loop anywhere that interrupt cannot land. That is not hypothetical:
// it hung the machine when the xHCI recovery path called `power_ports()`
// from a syscall (docs/bugs.md).
//
// Falls back to ticks only when the current source cannot be read with
// interrupts off, which is the one case where there is nothing better
// -- and where the caller was going to spin on ticks anyway.
//
// Resolution follows the source: real milliseconds on a TSC, and the
// tick fallback still rounds UP to at least one whole 10 ms tick, so no
// caller ever gets a shorter delay than it asked for.
void clocksource_delay_ms(uint32_t ms);

// Works out a mult/shift pair for a counter running at `freq` Hz, such
// that ns = (delta * mult) >> shift holds to within rounding for any
// delta up to `maxsec` seconds' worth. Picks the LARGEST shift that
// keeps both the multiplier and the worst-case product inside their
// types -- more shift is more precision, and the limit is overflow.
//
// Exposed rather than kept private because each source calls it with
// its own frequency, and getting it wrong is silent: too small a shift
// quantises the conversion, too large a one overflows and makes time
// jump backwards.
void clocksource_calc_mult_shift(uint32_t *mult, uint32_t *shift,
                                  uint64_t freq, uint32_t maxsec);

// Registers the PIT-backed source. Called from kernel_main() early --
// before anything wants a timestamp, and specifically before
// cpu_info_init(), which calibrates the TSC against the PIT.
void clocksource_init(void);

// Registers the TSC-backed source if this CPU has an INVARIANT TSC and
// a usable calibrated frequency. Called from kernel_main() AFTER
// cpu_info_init(), because it needs the frequency that calibration
// produces -- and calibration needs the PIT already ticking, which is
// the ordering trap cpuinfo.h warns about (a lazy calibration inside a
// syscall deadlocks). This is why registration is two calls rather than
// one: the two sources become available at genuinely different moments
// in boot.
void clocksource_init_tsc(void);

// Registers the ACPI PM timer, if the FADT names one
// (kernel/acpi/acpi_pmtimer.c). From kernel_main() right after
// clocksource_init(): it needs no calibration -- its rate is fixed by
// the spec -- so it can replace the PIT before anything calibrates
// against the clock.
void clocksource_init_acpi_pm(void);

// The source named by `clocksource=` on the command line, or NULL.
// Linux's parameter: that source wins whatever its rating, the moment
// it registers; a name that never registers leaves rating order alone.
const char *clocksource_forced(void);

#endif
