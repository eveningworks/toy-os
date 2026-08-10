// A freestanding userland test program that exercises the write syscall
// (see kernel/core/syscall.c) -- the first userland program in this
// project that produces console output *itself*, via a real syscall,
// rather than the kernel narrating on its behalf. Then exits normally.
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

static inline int64_t sys_write(const char *buf, uint64_t len) {
    return syscall3(SYS_WRITE, 1, (uint64_t)(uintptr_t)buf, len);
}

static inline void sys_exit(int code) __attribute__((noreturn));
static inline void sys_exit(int code) {
    syscall2(SYS_EXIT, (uint64_t)(int64_t)code, 0);
    for (;;) { } // unreachable
}

// No libc here, so a tiny local strlen.
static uint64_t my_strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

void _start(void) {
    const char *msg = "Hello from ring 3, printed via a real write syscall!\n";
    sys_write(msg, my_strlen(msg));
    sys_exit(0);
}
