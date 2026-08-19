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
#include "rt/sys.h"







int main(void) {
    // fd=1 (stdout, valid) as RDI, 0x1000 (deliberately-bad kernel-only
    // pointer) as RSI -- still proves vmm_validate_user_range() rejects
    // the buffer, now against the 3-arg ABI.
    int64_t ret = sys_call(SYS_WRITE, 1, 0x1000, 50);
    // exit(1) if the kernel correctly rejected the pointer, exit(0) if
    // it didn't (a real bug: the kernel would have read kernel memory on
    // this process's behalf).
    //
    // -EFAULT, not -1. This is the RAW hatch, so it sees what the
    // handler actually returns rather than the -1 libsys converts that
    // into (rt/sys.h) -- and asserting the specific code is a stronger
    // check than "some failure", which is what this used to be.
    sys_exit(ret == -EFAULT ? 1 : 0);
}
