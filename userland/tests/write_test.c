// A freestanding userland test program that exercises the write syscall
// (see kernel/proc/syscall.c) -- the first userland program in this
// project that produces console output *itself*, via a real syscall,
// rather than the kernel narrating on its behalf. Then exits normally.
#include <stdint.h>
#include "rt/sys.h"









// No libc here, so a tiny local strlen.
static uint64_t my_strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

int main(void) {
    const char *msg = "Hello from ring 3, printed via a real write syscall!\n";
    sys_write(1, msg, my_strlen(msg));
    sys_exit(0);
}
