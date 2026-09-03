// Wall-clock tests. What fails silently here is the ANCHOR: a clock
// that reads plausibly at one instant and drifts, or one that comes back
// from a set() a few seconds off, looks right in a screenshot and is
// wrong by the time anything compares two timestamps.
//
// THESE RUN IN THE LIVE KERNEL, so they move the machine's real clock.
// Every one of them puts it back -- see the restore in each -- because
// leaving a test's date behind would give every file written afterwards
// a wrong mtime, and a `ktest` run must not be an event in the
// filesystem's history.
#include "ktest.h"
#include "ktime.h"
#include "caltime.h"
#include "timer.h"
#include "clocksource.h"

// 2026-09-03 12:34:56 UTC. A real date rather than a round number: a
// month and day that are both plausible as either ordering is exactly
// the fixture that cannot catch a swapped pair.
#define FIXTURE_EPOCH 1788438896ull

KTEST("ktime", "the clock advances with monotonic time, not in jumps") {
    uint64_t mono_a = clocksource_now_ns();
    uint64_t wall_a = ktime_now_ns();

    // SPIN UNTIL THE CLOCKSOURCE HAS MOVED, rather than for a fixed
    // number of iterations. A fixed count was the first version and it
    // is a race: on a PIT-backed clocksource the resolution is 10 ms,
    // and a busy loop that usually outruns that sometimes does not --
    // which failed as "the clock does not advance" on a clock that was
    // advancing perfectly well.
    uint64_t mono_b = mono_a;
    for (volatile int i = 0; i < 100000000; i++) {
        mono_b = clocksource_now_ns();
        if (mono_b - mono_a >= 20000000ull) break; // 20 ms
    }
    uint64_t wall_b = ktime_now_ns();

    KTEST_ASSERT(mono_b > mono_a);
    KTEST_ASSERT(wall_b > wall_a);

    // **WALL TIME TRACKS MONOTONIC TIME**, which is the entire design in
    // one assertion: the clock is an epoch plus a clocksource delta, so
    // the two must move together. A wall clock still reading the CMOS
    // would move in whole seconds and fail this by ~980 ms.
    uint64_t wall_delta = wall_b - wall_a;
    uint64_t mono_delta = mono_b - mono_a;
    uint64_t skew = wall_delta > mono_delta ? wall_delta - mono_delta
                                            : mono_delta - wall_delta;
    // THREE PIT TICKS, and the bound has to be more than one. Both
    // numbers come from the same accumulator, but they are read at four
    // different instants, so on a 100 Hz PIT source the skew is the
    // difference of two sub-tick gaps and can be a whole 10 ms tick --
    // which failed here at a bound of exactly one. The assertion keeps
    // all its power at 30 ms: a wall clock still reading the CMOS would
    // move in whole SECONDS and miss by up to 1000.
    KTEST_ASSERT(skew < 30000000ull);
}

KTEST("ktime", "set and read round-trip to the same second") {
    uint64_t saved = ktime_now_sec();

    KTEST_ASSERT(ktime_set(FIXTURE_EPOCH) == 1);
    uint64_t got = ktime_now_sec();
    // Not equality: real time passes between the set and the read, and
    // demanding an exact match would make this fail on a slow host for
    // the one reason that is not a bug.
    KTEST_ASSERT(got >= FIXTURE_EPOCH && got < FIXTURE_EPOCH + 5);

    // The broken-down form has to agree with the epoch it came from.
    struct rtc_time t;
    ktime_read(&t);
    KTEST_ASSERT_EQ(t.year, 2026);
    KTEST_ASSERT_EQ(t.month, 9);
    KTEST_ASSERT_EQ(t.day, 3);
    KTEST_ASSERT_EQ(t.hour, 12);
    KTEST_ASSERT_EQ(t.minute, 34);

    ktime_set(saved);
}

KTEST("ktime", "a step is reported, and its size is the difference") {
    uint64_t saved = ktime_now_sec();
    uint32_t before = ktime_step_count();

    // Forward an hour from wherever the clock is, so the expected step
    // is known without depending on what the RTC happens to say.
    uint64_t target = ktime_now_sec() + 3600;
    KTEST_ASSERT(ktime_set(target) == 1);
    KTEST_ASSERT_EQ(ktime_step_count(), before + 1);

    int64_t step = ktime_last_step();
    KTEST_ASSERT(step >= 3595 && step <= 3600);

    ktime_set(saved);
}

KTEST("ktime", "an out-of-range epoch is refused and moves nothing") {
    uint64_t saved = ktime_now_sec();
    uint32_t before = ktime_step_count();

    // Year 10000 and beyond: the RTC's two-digit year plus an assumed
    // century cannot hold it, so accepting it would store a time that
    // reads back as something else entirely.
    KTEST_ASSERT_EQ(ktime_set(253402300800ull), 0);
    KTEST_ASSERT_EQ(ktime_step_count(), before);

    uint64_t now = ktime_now_sec();
    KTEST_ASSERT(now >= saved && now < saved + 5);
}

KTEST("ktime", "the RTC takes what was written to it") {
    // THE HARDWARE, not the software clock -- this is what makes a
    // correction survive a reboot, and it is the half a read of
    // ktime_now_sec() cannot see. The two are compared through the
    // calendar arithmetic rather than field by field so a BCD or
    // 12-hour-mode bug in rtc_write() shows up as a wrong SECOND count
    // rather than being tested against the same conversion that wrote it.
    uint64_t saved = ktime_now_sec();

    KTEST_ASSERT(ktime_set(FIXTURE_EPOCH) == 1);

    struct rtc_time hw;
    rtc_read(&hw);
    int64_t days = cal_days_from_civil(hw.year, hw.month, hw.day);
    KTEST_ASSERT(days >= 0);
    uint64_t hw_epoch = (uint64_t)days * 86400ull +
                        hw.hour * 3600ull + hw.minute * 60ull + hw.second;

    KTEST_ASSERT(hw_epoch >= FIXTURE_EPOCH && hw_epoch < FIXTURE_EPOCH + 5);

    ktime_set(saved);
}
