// M16 scheduler demo -- see counter_a.c for the full explanation. Same
// program, prints 'B' instead of 'A', so `schedtest`'s two processes are
// visibly distinguishable in the interleaved output.
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

static char out_char = 'B';

#define ITERATIONS  20
#define SPIN_ITERS  30000000u

void _start(void) {
    for (int i = 0; i < ITERATIONS; i++) {
        syscall3(SYS_WRITE, 1, (uint64_t)(uintptr_t)&out_char, 1);
        for (volatile uint32_t j = 0; j < SPIN_ITERS; j++) { }
    }
    sys_exit(0);
}
