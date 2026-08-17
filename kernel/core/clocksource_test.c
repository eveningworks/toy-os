// Clocksource tests. The mult/shift arithmetic and the registration
// rules are the two things here that fail SILENTLY -- a wrong shift
// makes every duration wrong by a constant factor, and a refused-or-not
// decision made wrongly installs a stopped clock over a working one.
// Neither shows up as a crash.
#include "ktest.h"
#include "clocksource.h"

// Converts as clocksource_now_ns() does, so a test can check the pair
// without reaching into the accumulator.
static uint64_t convert(uint64_t delta, uint32_t mult, uint32_t shift) {
    return (delta * mult) >> shift;
}

KTEST("clocksource", "mult/shift converts a second to a billion ns") {
    // Three frequencies spanning the plausible range: the PIT's 100Hz,
    // a 1MHz HPET, and a 3.4GHz TSC. One second of each must come back
    // as 1e9 ns, within the rounding the pair can express.
    const uint64_t freqs[] = { 100ULL, 1000000ULL, 3400000000ULL };

    for (unsigned i = 0; i < sizeof freqs / sizeof freqs[0]; i++) {
        uint32_t mult = 0, shift = 0;
        clocksource_calc_mult_shift(&mult, &shift, freqs[i], 3600);
        KTEST_ASSERT(mult != 0);

        uint64_t ns = convert(freqs[i], mult, shift);
        // Within 0.1% of a second. Not exact on purpose: mult/shift is a
        // fixed-point ratio, so demanding equality would be asserting
        // that the rounding happens to vanish for these three numbers.
        uint64_t err = ns > 1000000000ULL ? ns - 1000000000ULL : 1000000000ULL - ns;
        KTEST_ASSERT(err < 1000000ULL);
    }
}

KTEST("clocksource", "the worst-case delta does not overflow the multiply") {
    // The bound calc_mult_shift() is asked for is what stops
    // clocksource_now_ns()'s multiply wrapping -- and a wrap makes time
    // jump BACKWARDS, which is the one failure every caller's arithmetic
    // assumes cannot happen.
    uint32_t mult = 0, shift = 0;
    clocksource_calc_mult_shift(&mult, &shift, 3400000000ULL, 3600);
    KTEST_ASSERT(mult != 0);

    uint64_t maxcycles = 3600ULL * 3400000000ULL;
    // The product must still fit -- i.e. dividing it back by mult has to
    // return what went in.
    uint64_t product = maxcycles * mult;
    KTEST_ASSERT_EQ(product / mult, maxcycles);

    // And an hour really does convert to about an hour.
    uint64_t ns = convert(maxcycles, mult, shift);
    uint64_t expect = 3600ULL * 1000000000ULL;
    uint64_t err = ns > expect ? ns - expect : expect - ns;
    KTEST_ASSERT(err < expect / 1000); // within 0.1%
}

KTEST("clocksource", "a self-contradictory source is refused") {
    // The honesty check, same shape as display_probe() refusing a driver
    // whose capability bits and function pointers disagree. Each of
    // these would otherwise install a clock that never advances, over
    // one that works.
    struct clocksource broken = {
        .name = "test-broken", .read = 0,
        .mask = CLOCKSOURCE_MASK(64), .mult = 1, .shift = 0,
        .rating = 9999, // high enough to displace anything real
    };
    KTEST_ASSERT_EQ(clocksource_register(&broken), 0);

    broken.read = clocksource_current() ? clocksource_current()->read : 0;
    broken.mult = 0; // converts every delta to zero -- a stopped clock
    KTEST_ASSERT_EQ(clocksource_register(&broken), 0);

    // The live source must be untouched by either attempt: a refused
    // registration that still swapped the pointer would be worse than
    // accepting it.
    KTEST_ASSERT(clocksource_current() != 0);
    KTEST_ASSERT(clocksource_current()->mult != 0);
}

KTEST("clocksource", "time moves forward and never backward") {
    // Monotonicity, read repeatedly. This is the property the whole
    // accumulate-a-delta design exists to provide, and the one a
    // wraparound bug breaks.
    uint64_t prev = clocksource_now_ns();
    for (int i = 0; i < 200; i++) {
        uint64_t now = clocksource_now_ns();
        KTEST_ASSERT(now >= prev);
        prev = now;
    }
}

KTEST("clocksource", "a source is registered and it is the best-rated one") {
    const struct clocksource *cs = clocksource_current();
    KTEST_ASSERT(cs != 0);
    KTEST_ASSERT(cs->name != 0);
    // Whichever won, it must at least be usable as a clock. Naming the
    // expected WINNER here would be wrong: which source is live depends
    // on the CPU (the TSC needs an invariant one, absent under plain
    // TCG) and on the `notsc` boot flag.
    KTEST_ASSERT(cs->read != 0);
    KTEST_ASSERT(cs->mult != 0);
    KTEST_ASSERT(cs->rating >= CLOCKSOURCE_RATING_PIT);
}
