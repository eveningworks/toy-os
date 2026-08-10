// A freestanding userland test program that deliberately passes an
// INVALID pointer to the write syscall -- proving the kernel's pointer
// validation (vmm_validate_user_range(), see vmm.h / syscall.c) actually
// rejects it, rather than just assuming it would.
//
// 0x1000 is a real address, and it IS present in this process's own page
// tables (PML4 entry 0 -- the low identity-mapped range -- is shared
// with the kernel, see vmm.h), but it was never mapped with the USER
// bit: it's kernel-only memory. This process could never legally read
// it itself; the point of this test is confirming the kernel won't read
// it on the process's behalf either.
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

void _start(void) {
    // fd=1 (stdout, valid) as RDI, 0x1000 (deliberately-bad kernel-only
    // pointer) as RSI -- still proves vmm_validate_user_range() rejects
    // the buffer, now against the 3-arg ABI.
    int64_t ret = syscall3(SYS_WRITE, 1, 0x1000, 50);
    // exit(1) if the kernel correctly rejected the pointer (ret == -1,
    // our simplified error indicator -- no errno yet), exit(0) if it
    // didn't (a real bug: the kernel would have read kernel memory on
    // this process's behalf).
    sys_exit(ret == -1 ? 1 : 0);
}
