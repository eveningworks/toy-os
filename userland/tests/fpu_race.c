// The test that actually proves the scheduler saves and restores FP
// state: two copies of this run CONCURRENTLY, preempted by the 100Hz
// timer, each holding a set of distinct floating-point accumulators in
// registers across thousands of context switches.
//
// fpu_test.c proves floating point *works*. It cannot prove the
// save/restore works, because it never shares the CPU with anything.
// This is the difference between "FP is enabled" and "FP is enabled
// safely", and only the second one is a scheduler feature.
//
// How a bug would show: without FXSAVE/FXRSTOR in scheduler.c, the two
// processes share one set of physical XMM registers. Whichever is
// resumed after a switch continues with the OTHER one's accumulators,
// and both finish with wrong values. The seed argument makes the two
// runs use different numbers, so a leak can't accidentally produce the
// right answer.
//
// Eight accumulators on purpose, not one: a partial save (saving xmm0
// but not the rest, say) would pass a single-accumulator test. Eight
// live doubles pushes the register allocator into using most of the
// XMM file, so the whole 512-byte area has to be genuinely round-tripping.
//
// Exits 0 if every accumulator holds its expected value, or the 1-based
// index of the first one that doesn't.
//
// Usage: fpu_race <seed>   (seed 1 and seed 2 are what `fputest` runs)
#include <stdint.h>
#include "rt/sys.h"







static void puts_(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    sys_call(SYS_WRITE, 1, (uint64_t)(uintptr_t)s, n);
}

#define ACCS 8
// Long enough to span many 10ms slices (so preemption definitely
// happens mid-computation), short enough that `fputest` returns
// promptly. Every partial sum stays exactly representable -- these are
// all dyadic rationals well under 2^53 -- so the final comparison is a
// plain `==` with no epsilon, and a single corrupted register is a hard
// failure rather than a rounding difference.
// Sized so the loop spans HUNDREDS of 10ms slices, not a handful --
// `schedtest`'s counter processes spin 30,000,000 times per iteration
// for the same reason. A loop short enough to finish inside one slice
// would pass with no FPU save/restore at all, which is the one way this
// test could quietly stop testing anything.
#define ITERS 20000000

static double acc[ACCS];
static double expected[ACCS];

// volatile so the loop below can't be constant-folded into a multiply
// by the optimizer -- the point is to hold live values in XMM registers
// across real preemptions, not to compute the answer quickly.
static volatile double step = 0.5;

int main(int argc, char **argv) {
    int seed = 1;
    if (argc > 1 && argv[1] && argv[1][0] >= '0' && argv[1][0] <= '9') {
        seed = argv[1][0] - '0';
    }

    for (int i = 0; i < ACCS; i++) {
        acc[i] = (double)(seed * 1000 + i);
        expected[i] = acc[i] + (double)ITERS * 0.5;
    }

    // Pulled into locals so they live in registers for the whole loop --
    // if they stayed in memory, the test would pass even with no FPU
    // save/restore at all, since memory is per-process anyway. That is
    // the single most important line in this file.
    double a0 = acc[0], a1 = acc[1], a2 = acc[2], a3 = acc[3];
    double a4 = acc[4], a5 = acc[5], a6 = acc[6], a7 = acc[7];

    for (uint32_t n = 0; n < ITERS; n++) {
        double s = step;
        a0 += s; a1 += s; a2 += s; a3 += s;
        a4 += s; a5 += s; a6 += s; a7 += s;
    }

    acc[0] = a0; acc[1] = a1; acc[2] = a2; acc[3] = a3;
    acc[4] = a4; acc[5] = a5; acc[6] = a6; acc[7] = a7;

    for (int i = 0; i < ACCS; i++) {
        if (acc[i] != expected[i]) {
            puts_("fpu_race: FP state corrupted across a context switch\n");
            sys_exit(i + 1);
        }
    }

    puts_("fpu_race: accumulators intact\n");
    sys_exit(0);
}
