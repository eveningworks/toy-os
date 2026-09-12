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

static inline uint64_t uclock_granularity_ns(void) {
    uint64_t best = 0;
    for (int i = 0; i < 64; i++) {
        uint64_t a = sys_monotonic_ns();
        uint64_t b = sys_monotonic_ns();
        if (b > a && (!best || b - a < best)) best = b - a;
    }
    return best;
}

#endif
