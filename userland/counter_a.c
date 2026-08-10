// M16 scheduler demo (see kernel/core/scheduler.c). A tiny freestanding
// ring-3 program that proves preemptive, non-cooperative scheduling:
// prints 'A' a fixed number of times, each followed by a long busy-spin
// (spanning several 100Hz timer ticks), then exits. Run alongside
// counter_b.c (which does the same with 'B') via the shell's `schedtest`
// command. There is no yield syscall anywhere in this project -- if the
// output interleaves on screen instead of printing all 20 As followed
// by all 20 Bs, the ONLY possible explanation is the timer preempting
// one process mid-spin and handing the CPU to the other.
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

static inline int64_t syscall3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    int64_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3)
        : "memory"
    );
    return ret;
}

static inline void sys_exit(int code) __attribute__((noreturn));
static inline void sys_exit(int code) {
    syscall2(SYS_EXIT, (uint64_t)(int64_t)code, 0);
    for (;;) { } // unreachable
}

// A global (not a stack local) so it's definitely inside a PT_LOAD
// segment elf.c mapped -- same reasoning as write_test.c's approach.
static char out_char = 'A';

#define ITERATIONS  20
#define SPIN_ITERS  30000000u // long enough to span several 10ms slices

void _start(void) {
    for (int i = 0; i < ITERATIONS; i++) {
        syscall3(SYS_WRITE, 1, (uint64_t)(uintptr_t)&out_char, 1);
        for (volatile uint32_t j = 0; j < SPIN_ITERS; j++) { }
    }
    sys_exit(0);
}
