// timer_bench -- what the timer configuration costs and buys, measured
// from ring 3. tools/timer_bench.py drives it and compares builds.
//
//     timer_bench            all three phases
//     timer_bench hog <ms>   (a child) count work units for <ms>
//
// Three things, each one a question the tick rate might answer:
//
//   LATENCY  how late a sleep ends, at 1/3/7/16 ms, with nothing else
//            running -- a periodic tick is up to a tick late, a
//            one-shot deadline is not.
//   ALONE    how much work ONE busy process gets done in a fixed window
//            -- the tick's own overhead is what separates the rates.
//   LOAD     the same with two busy processes contending, while this
//            one sleeps 1 ms at a time: the sum is the switch overhead,
//            the sleep overshoot is how long a woken process waits for
//            a busy CPU.
//
// Every number goes to the kernel log (fd 2) on a `timer_bench:` line,
// which is where the harness reads it back; nothing here passes or fails.
#include "rt/sys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLES 41
#define say(...) fprintf(stderr, __VA_ARGS__)
#define WINDOW_MS 2000

// A work unit: enough arithmetic that reading the clock once per unit
// is noise, and a result the compiler cannot discard.
static volatile unsigned long long g_sink;
static void work_unit(void) {
    unsigned long long x = g_sink | 1;
    for (int i = 0; i < 20000; i++) x = x * 6364136223846793005ull + 1442695040888963407ull;
    g_sink = x;
}

static unsigned long long hog(unsigned ms) {
    unsigned long long end = sys_monotonic_ns() + (unsigned long long)ms * 1000000ull;
    unsigned long long units = 0;
    while (sys_monotonic_ns() < end) { work_unit(); units++; }
    return units;
}

static void sort(unsigned long long *v, int n) {
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && v[j - 1] > v[j]; j--) {
            unsigned long long t = v[j]; v[j] = v[j - 1]; v[j - 1] = t;
        }
}

// Overshoot of `n` sleeps of `ms`, in microseconds: median, p90, max.
static void sleeps(const char *label, int ms, int n) {
    unsigned long long v[SAMPLES];
    if (n > SAMPLES) n = SAMPLES;
    for (int i = 0; i < n; i++) {
        unsigned long long t0 = sys_monotonic_ns();
        sys_sleep_ms(ms);
        unsigned long long d = sys_monotonic_ns() - t0, want = (unsigned long long)ms * 1000000ull;
        v[i] = d > want ? (d - want) / 1000ull : 0;
    }
    sort(v, n);
    say("timer_bench: %s %dms median_us %llu p90_us %llu max_us %llu n %d\n",
           label, ms, v[n / 2], v[(n * 9) / 10], v[n - 1], n);
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "hog")) {
        unsigned long long u = hog((unsigned)atoi(argv[2]));
        say("timer_bench: hog units %llu\n", u);
        return 0;
    }

    static const int lens[] = { 1, 3, 7, 16 };
    for (unsigned i = 0; i < sizeof lens / sizeof lens[0]; i++) sleeps("idle", lens[i], SAMPLES);

    say("timer_bench: alone units %llu ms %d\n", hog(WINDOW_MS), WINDOW_MS);

    // Two hogs, each reporting its own units. The sleeps run
    // inside their window: a woken sleeper competing with busy ones.
    char arg[24];
    snprintf(arg, sizeof arg, "hog %d", WINDOW_MS);
    int a = sys_spawn("/tests/timer_bench", arg, -1);
    int b = sys_spawn("/tests/timer_bench", arg, -1);
    sys_sleep_ms(50);
    sleeps("load", 1, SAMPLES);
    int code;
    if (a > 0) sys_waitpid(a, &code);
    if (b > 0) sys_waitpid(b, &code);
    say("timer_bench: done\n");
    return 0;
}
