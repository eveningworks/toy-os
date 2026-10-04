#ifndef KERNEL_RATELIMIT_H
#define KERNEL_RATELIMIT_H

// A log line that may repeat, at most ONE A SECOND, and saying how many
// were held back -- CLAUDE.md's "stay under a line a second": the klog
// ring holds a few thousand lines, and a fault that logs per event erases
// the evidence it is reporting. Linux's printk_ratelimit() shape, per
// call site:
//
//     static struct ratelimit rl;
//     unsigned held;
//     if (ratelimit_ok(&rl, &held)) klog_printf("...(%u more)\n", held);
//
// Interrupt-safe: no lock, two words, and a lost race costs one line.
#include <stdint.h>
#include "clocksource.h"

struct ratelimit {
    uint64_t next_ns;      // the earliest the next line may go
    unsigned suppressed;   // held back since the last line
};

static inline int ratelimit_ok(struct ratelimit *rl, unsigned *held) {
    uint64_t now = clocksource_now_ns();
    if (rl->next_ns && now < rl->next_ns) {
        rl->suppressed++;
        return 0;
    }
    if (held) *held = rl->suppressed;
    rl->suppressed = 0;
    rl->next_ns = now + 1000000000ull;
    return 1;
}

// Forget the last line: the next one goes at once. For a KTEST that has
// to see the line, never for production code.
static inline void ratelimit_reset(struct ratelimit *rl) {
    rl->next_ns = 0;
    rl->suppressed = 0;
}

#endif
