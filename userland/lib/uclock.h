#ifndef ULIB_UCLOCK_H
#define ULIB_UCLOCK_H

// The smallest non-zero gap the monotonic clock will actually show.
//
// A header rather than a second copy. /bin/diskbench had this written
// out to decide whether to believe its own microsecond columns, and the
// WM's latency counters need the same answer for the same reason -- two
// REAL callers, which is the bar (CLAUDE.md: a second real caller, not a
// plausible one).
//
// SAMPLED, NOT ASKED FOR: nothing reports a clocksource's resolution,
// and what a reader needs is what can be OBSERVED. The answer is ~10 ms
// on any default QEMU, because an invariant TSC is not offered to a
// guest unless the CPU model says `+invtsc` (it blocks migration), so
// the PIT wins -- see clocksource.h's ratings. A granularity in the
// millions means every microsecond figure beside it is floor-zero noise
// rather than a measurement, which is the most misleading answer a
// profiler can give.
#include "rt/sys.h"
#include <stdint.h>

// A CLOCK THAT NEVER MOVED IS UCLOCK_NEVER_MOVED, NOT 0 -- 0 would read
// as perfect precision. The adjacent-read pass sees no increase on a
// tick clock (64 reads fit inside one PIT tick), so the fallback times
// edge to edge instead, which is the tick itself.
#define UCLOCK_NEVER_MOVED UINT64_MAX

static inline uint64_t uclock_next_edge(uint64_t from) {
    // Bounded by reads, not time: the clock is what is in doubt. A
    // million syscalls is ~100x a 10 ms tick even under TCG.
    for (uint32_t i = 0; i < 1000000u; i++) {
        uint64_t t = sys_monotonic_ns();
        if (t != from) return t;
    }
    return 0;
}

static inline uint64_t uclock_granularity_ns(void) {
    uint64_t best = 0;
    for (int i = 0; i < 64; i++) {
        uint64_t a = sys_monotonic_ns();
        uint64_t b = sys_monotonic_ns();
        if (b > a && (!best || b - a < best)) best = b - a;
    }
    if (best) return best;
    uint64_t e = uclock_next_edge(sys_monotonic_ns());
    for (int i = 0; e && i < 2; i++) {
        uint64_t n = uclock_next_edge(e);
        if (n > e && (!best || n - e < best)) best = n - e;
        e = n;
    }
    return best ? best : UCLOCK_NEVER_MOVED;
}

#endif
