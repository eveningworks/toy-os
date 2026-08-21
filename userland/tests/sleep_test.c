// SYS_SLEEP -- does a sleep actually last as long as it was asked for,
// and does it come back?
//
// Both halves matter and the second is the one that bites: a sleep is a
// park, so a bug in the wake path is a process that never runs again --
// which from outside looks exactly like a hang, not like a wrong
// number. Every check here therefore has to COMPLETE to pass.
//
// Measured against SYS_MONOTONIC_NS, which is the clock the kernel
// computes the deadline from, so this is not comparing two unrelated
// counters.
#include "rt/sys.h"
#include <stdio.h>

static int fails = 0;

static void check(const char *what, int ok, const char *detail) {
    char line[160];
    snprintf(line, sizeof line, "sleep: %s %s%s%s\n", ok ? "ok  " : "FAIL",
             what, detail ? " -- " : "", detail ? detail : "");
    sys_eprint(line);
    if (!ok) fails++;
}

// One tick is 10 ms at the PIT's 100 Hz, and a sleep is rounded UP to
// the next one -- so a request may legitimately overshoot by nearly a
// tick, plus however long the scheduler takes to rotate back to us.
#define SLACK_MS 60

static unsigned long long slept_ms(int ms) {
    unsigned long long t0 = sys_monotonic_ns();
    if (sys_sleep_ms(ms) != 0) return ~0ull;
    unsigned long long t1 = sys_monotonic_ns();
    return (t1 - t0) / 1000000ull;
}

int main(void) {
    char detail[64];

    // Never EARLY. This is the assertion a broken deadline fails: a
    // sleep woken on the first tick regardless of its deadline still
    // returns, still returns 0, and only this notices.
    unsigned long long got = slept_ms(200);
    snprintf(detail, sizeof detail, "asked 200ms, slept %ums", (unsigned)got);
    check("a 200ms sleep is not short", got != ~0ull && got >= 200, detail);
    check("a 200ms sleep is not wildly long", got != ~0ull && got <= 200 + SLACK_MS, detail);

    // A short one, to catch a deadline computed in the wrong unit: 20
    // seconds instead of 20 ms would still be "not short".
    got = slept_ms(20);
    snprintf(detail, sizeof detail, "asked 20ms, slept %ums", (unsigned)got);
    check("a 20ms sleep is bounded", got != ~0ull && got >= 20 && got <= 20 + SLACK_MS, detail);

    // Zero is a valid request meaning "until the next tick", not an
    // error and not an infinite park.
    got = slept_ms(0);
    snprintf(detail, sizeof detail, "slept %ums", (unsigned)got);
    check("a 0ms sleep returns", got != ~0ull && got <= SLACK_MS, detail);

    // Repeated sleeps must keep working -- a wake that leaves the slot
    // in a bad state passes the first check and hangs on the second.
    int ok = 1;
    for (int i = 0; i < 5; i++) if (sys_sleep_ms(10) != 0) ok = 0;
    check("five sleeps in a row all return", ok, 0);

    if (fails) {
        sys_eprint("sleep: FAILED\n");
        return 1;
    }
    sys_eprint("sleep: all checks passed\n");
    return 0;
}
