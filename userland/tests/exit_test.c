// A freestanding userland test program that exercises the exit syscall
// (see kernel/proc/syscall.c) instead of deliberately faulting like
// userland/hello.c does. Proves the full round-trip: the kernel launches
// this, it runs in ring 3, calls exit, and control returns cleanly to
// whichever kernel code launched it -- no fault, no halt.
#include <stdint.h>
#include "rt/sys.h"



int main(void) {
    sys_exit(42);
}
