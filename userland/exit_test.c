// A freestanding userland test program that exercises the exit syscall
// (see kernel/proc/syscall.c) instead of deliberately faulting like
// userland/hello.c does. Proves the full round-trip: the kernel launches
// this, it runs in ring 3, calls exit, and control returns cleanly to
// whichever kernel code launched it -- no fault, no halt.
#include <stdint.h>
#include "syscall_abi.h"

static inline void sys_exit(int code) __attribute__((noreturn));
static inline void sys_exit(int code) {
    __asm__ volatile (
        "int $0x80\n\t"
        :
        : "a"((uint64_t)SYS_EXIT), "D"((uint64_t)(int64_t)code)
        : "memory"
    );
    for (;;) { } // unreachable -- exit doesn't return
}

void _start(void) {
    sys_exit(42);
}
