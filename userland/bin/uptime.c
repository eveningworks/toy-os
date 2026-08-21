// uptime -- how long this machine has been running.
//
// SYS_MONOTONIC_NS, not the RTC: uptime is an INTERVAL, and wall clock
// is not an implementation of monotonic time (CLAUDE.md's rule -- the
// RTC can step, and an interval measured across a step is wrong).
//
// It prints a duration in units a person reads, not a tick count. The
// kernel shell's builtin printed raw ticks, which answers "is the timer
// running" and not "how long has this been up"; ticks are still
// available to anything that wants them, through the same syscall.
#include "rt/sys.h"
#include <stdio.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    unsigned long long ns = sys_monotonic_ns();
    unsigned long long secs = ns / 1000000000ull;
    // The tenth comes out of the remainder -- there is no floating
    // point in ring 3 either (-mno-sse).
    unsigned long long tenths = (ns % 1000000000ull) / 100000000ull;

    unsigned long long d = secs / 86400;
    unsigned long long h = (secs % 86400) / 3600;
    unsigned long long m = (secs % 3600) / 60;
    unsigned long long s = secs % 60;

    char line[96];
    if (d) {
        snprintf(line, sizeof line, "up %llu day%s, %02llu:%02llu:%02llu\n",
                 d, d == 1 ? "" : "s", h, m, s);
    } else if (h) {
        snprintf(line, sizeof line, "up %llu:%02llu:%02llu\n", h, m, s);
    } else if (m) {
        snprintf(line, sizeof line, "up %llu min %llu sec\n", m, s);
    } else {
        // Under a minute the tenth is the interesting digit -- this is
        // what a boot-time measurement reads.
        snprintf(line, sizeof line, "up %llu.%llu sec\n", s, tenths);
    }
    sys_print(line);
    return 0;
}
