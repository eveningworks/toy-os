// Named shared memory, end to end: SYS_SHM_OPEN, MAP_SHARED, and two
// processes writing to each other through one object.
//
// **THE CROSS-PROCESS CHECK IS THE POINT, and it cannot be done in one
// process.** A single process mapping its own object twice would see
// its own writes whether or not the frames were shared -- so this
// spawns /tests/shm_child, which is handed no descriptor, no address
// and no mapping, only the NAME. Both directions are checked: the
// parent's pattern must reach the child, and the child's reply must
// come back.
//
// The patterns are ADDRESS-DERIVED. A constant fill reads back correctly
// whether the two mappings share frames or merely both exist, which is
// exactly the failure this is here to see.
//
// IT IS SPAWNED, NOT `run`: the parent blocks in waitpid, which the
// legacy loader has no scheduler slot for. The verdict goes to a file
// for the same reason env_test's does.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "rt/sys.h"
#include "syscall_abi.h"
#include "lib/utest.h"

#define SHM_PAGES 2
#define SHM_BYTES (SHM_PAGES * 4096)

static uint8_t want(int i) { return (uint8_t)(i * 31 + 7); }
static uint8_t reply(int i) { return (uint8_t)(i * 17 + 3); }

int main(void) {
    utest_begin("shm_test", "named shared memory across two processes",
                UTEST_VERDICT_FILE);

    sys_shm_unlink("t-shm"); // a previous run that died mid-way
    sys_shm_unlink("t-private");

    // --- the object ---------------------------------------------------
    int fd = sys_shm_open("t-shm", SHM_BYTES, SHM_CREATE | SHM_EXCL);
    utest_checkf(fd >= 0, "created a %d-byte object (fd %d)", SHM_BYTES, fd);
    if (fd < 0) return utest_end();

    utest_check(sys_shm_open("t-shm", SHM_BYTES, SHM_CREATE | SHM_EXCL) < 0 &&
                sys_errno() == EEXIST, "SHM_EXCL over an existing name is EEXIST");
    utest_check(sys_shm_open("t-nothing", 0, 0) < 0 && sys_errno() == ENOENT,
                "opening a name that does not exist is ENOENT");

    // A NAME IS NOT A CAPABILITY ANY MORE. Checked from the one process
    // that can ask without a second one: granting to a pid that is not
    // this one and never was, and confirming the object still refuses
    // everybody else -- the deny itself needs the child below, which
    // cannot open this until it is granted.
    utest_check(sys_shm_grant("t-nothing", 2) < 0 && sys_errno() == ENOENT,
                "granting an object that does not exist is ENOENT");

    // --- the mapping --------------------------------------------------
    volatile uint8_t *p = sys_mmap(0, SHM_BYTES, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, fd, 0);
    utest_check(p != (void *)-1, "mapped it MAP_SHARED");
    if (p == (void *)-1) return utest_end();

    utest_check(sys_mmap(0, SHM_BYTES * 8, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0) == (void *)-1,
                "a mapping longer than the object is refused");
    utest_check(sys_mmap(0, 4096, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0) == (void *)-1,
                "MAP_SHARED with MAP_ANONYMOUS is refused");

    int zero = 1;
    for (int i = 0; i < SHM_BYTES; i++) if (p[i]) { zero = 0; break; }
    utest_check(zero, "a new object reads back zeroed");

    for (int i = 0; i < 4096; i++) p[i] = want(i);

    // --- the other process ---------------------------------------------
    // AN OBJECT THE CHILD IS NEVER GRANTED, so the refusal below is
    // deterministic rather than a race with the grant above. The child
    // tries it once and fails the run if it gets in.
    int priv = sys_shm_open("t-private", 4096, SHM_CREATE | SHM_EXCL);
    utest_check(priv >= 0, "created a private object the child cannot have");

    int pid = sys_spawn("/tests/shm_child", "", -1);
    // THE GRANT IS WHAT LETS IT IN. Before objects had an owner any
    // process could open this by reading `lsshm`; now the creator names
    // who may, and the child retries while it waits to be named.
    if (pid > 0)
        utest_check(sys_shm_grant("t-shm", pid) == 0, "the child is granted");
    utest_checkf(pid > 0, "spawned /tests/shm_child (pid %d)", pid);
    if (pid > 0) {
        int code = -1;
        sys_waitpid(pid, &code);
        utest_checkf(code == 0, "the child read this process's pattern "
                                "(exit %d)", code);

        int ok = 1;
        for (int i = 0; i < 4096; i++)
            if (p[4096 + i] != reply(i)) { ok = 0; break; }
        utest_check(ok, "the child's reply came back through the same frames");
    }

    // --- lifetime -------------------------------------------------------
    utest_check(sys_shm_unlink("t-shm") == 0, "unlinked it");
    utest_check(sys_shm_open("t-shm", 0, 0) < 0 && sys_errno() == ENOENT,
                "an unlinked name has no new openers");
    // The mapping is still live and still readable -- the half of POSIX's
    // lifetime rule that a refcount on descriptors alone would break.
    utest_check(p[0] == want(0), "a live mapping survives the unlink");

    utest_check(sys_munmap((void *)p, SHM_BYTES) == 0, "unmapped it");
    sys_close(fd);
    return utest_end();
}
