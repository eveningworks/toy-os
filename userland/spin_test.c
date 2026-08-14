// A deliberately SILENT, long-running ring-3 process: spins across many
// 100Hz timer slices, writes nothing at all, then exits 0. An optional
// argument sets how long (see parse_rounds() below).
//
// Exists for one test that couldn't be written with counter_a/counter_b
// (the other long-running binaries here): kernel/proc/sched_test.c's
// check that the KERNEL context keeps getting CPU while a ring-3
// process is ready. That test runs under `ktest`, whose report is
// parsed off the serial console by tools/ktest_run.py -- so a test
// process that printed anything (counter_a writes 20 'A's) would be
// interleaving junk into the very stream the harness is parsing. This
// one is the same shape minus the output.
//
// The spin is a volatile loop rather than a sleep syscall because there
// is no sleep syscall -- and deliberately so for this test: a process
// that blocked would prove nothing about preemption, since the whole
// question is whether the kernel gets a turn against a process that
// never yields.
#include <stdint.h>
#include "syscall_abi.h"

static inline int64_t syscall2(uint64_t num, uint64_t arg1, uint64_t arg2) {
    int64_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2)
        : "memory"
    );
    return ret;
}

static inline void sys_exit(int code) __attribute__((noreturn));
static inline void sys_exit(int code) {
    syscall2(SYS_EXIT, (uint64_t)(int64_t)code, 0);
    for (;;) { } // unreachable
}

// Default outer rounds: enough to span comfortably more timer slices
// than sched_test.c's minimum observation count needs, while still
// finishing well inside that test's own tick timeout -- both ends
// matter, since the test fails either if the kernel observes too few
// ticks OR if the process never exits. Each round is the same order of
// magnitude as counter_a.c's per-iteration spin, documented there as
// "several 10ms slices".
#define OUTER_DEFAULT 6
#define OUTER_MAX 600 // ~100x the default; a bound so a typo'd argument
                       // can't wedge a test VM for minutes
#define SPIN_ITERS 30000000u

// Why the duration is an ARGUMENT rather than one baked-in constant:
// the two tests that drive this program need very different lengths.
// sched_test.c samples from a tight kernel loop, so it collects plenty
// of evidence in the default duration and wants the suite to stay fast.
// tools/sched_gui_test.py samples over a SERIAL round trip per sample
// (hundreds of ms each), so it needs a process that lives for seconds
// to overlap enough of them. Baking in the longer figure would have
// slowed `make test` for no gain; baking in the shorter one made the
// GUI test flaky at exactly 3 samples against a required 6.
static int parse_rounds(const char *s) {
    if (!s || !*s) return OUTER_DEFAULT;
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return OUTER_DEFAULT; // reject, don't guess
        v = v * 10 + (*s - '0');
        if (v > OUTER_MAX) return OUTER_MAX;
    }
    return v > 0 ? v : OUTER_DEFAULT;
}

// argc/argv per the kernel's process ABI (elf_build_argv_on_stack(),
// kernel/proc/elf_run.c) -- argv[0] is the path, so the optional round
// count is argv[1].
void _start(int argc, char **argv) {
    int rounds = argc > 1 ? parse_rounds(argv[1]) : OUTER_DEFAULT;
    for (int i = 0; i < rounds; i++) {
        for (volatile uint32_t j = 0; j < SPIN_ITERS; j++) { }
    }
    sys_exit(0);
}
