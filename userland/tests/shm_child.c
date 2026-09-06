// The second half of /tests/shm_test: a process that shares nothing
// with the parent except a NAME.
//
// That is the whole point of being a separate program. It was never
// handed a descriptor, it does not inherit a mapping, and it was not
// told an address -- it opens a string and gets the same frames. Its
// exit code is the number of failed checks, which is what the parent
// reads.
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "rt/sys.h"

#define SHM_PAGES 2
#define SHM_BYTES (SHM_PAGES * 4096)

// Address-derived, never constant: a constant fill cannot tell two
// mappings of one object from two mappings of two objects, which is
// the bug this test exists to catch.
static uint8_t want(int i) { return (uint8_t)(i * 31 + 7); }
static uint8_t reply(int i) { return (uint8_t)(i * 17 + 3); }

int main(void) {
    int fails = 0;

    int fd = sys_shm_open("t-shm", 0, 0); // no SHM_CREATE: it must exist
    if (fd < 0) return 1;

    volatile uint8_t *p = sys_mmap(0, SHM_BYTES, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, fd, 0);
    if (p == (void *)-1) return 2;

    for (int i = 0; i < 4096; i++)
        if (p[i] != want(i)) { fails++; break; }

    for (int i = 0; i < 4096; i++) p[4096 + i] = reply(i);

    sys_munmap((void *)p, SHM_BYTES);
    sys_close(fd);
    return fails;
}
