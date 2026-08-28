// mmap()/munmap() -- thin over rt/sys.c's stubs, which already speak
// POSIX's argument shape and MAP_FAILED. See <sys/mman.h> for what
// this mmap deliberately does not do.
#include <sys/mman.h>
#include "rt/sys.h"

void *mmap(void *addr, size_t length, int prot, int flags,
           int fd, long offset) {
    return sys_mmap(addr, length, prot, flags, fd, (uint64_t)offset);
}

int munmap(void *addr, size_t length) {
    return sys_munmap(addr, length);
}
