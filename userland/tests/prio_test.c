// Nice and the scheduling class from ring 3: who may change what, and
// what a child starts at.
//
// A slot reused by a new process kept its LAST tenant's level, so a
// program started after a driver exited ran at the driver's level
// without asking -- which only shows through a real spawn, so this is a
// program and not a KTEST. (A negative nice reading back as -1, the
// other bug it was written for, needs init to set one now.)
//
// `prio_test child` is the child: it exits with 20 - its nice, the
// same offset the syscall uses, so every level is a positive code.
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include "rt/sys.h"

#include "lib/utest.h"

// What a child spawned now starts at, or -99 if it could not be run.
static int child_level(void) {
    int pid = sys_spawn("/tests/prio_test", "child", -1);
    if (pid <= 0) return -99;
    int code = -1;
    sys_waitpid(pid, &code);
    return code < 0 ? -99 : 20 - code;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "child")) {
        int mine = getpriority(PRIO_PROCESS, 0);
        // The leak's victim: a child that changes itself and exits leaves
        // its slot at that level for whoever gets the slot next.
        if (argc > 2 && !strcmp(argv[2], "lower")) setpriority(PRIO_PROCESS, 0, 12);
        return 20 - mine;
    }

    utest_begin("prio_test", "nice and the scheduling class, and what a child inherits",
                UTEST_VERDICT_FILE);

    int start = getpriority(PRIO_PROCESS, 0);
    utest_checkf(start == 0, "a test starts at nice 0 (got %d)", start);

    // ONLY INIT MAY MAKE ANYTHING MORE IMPORTANT. Every refusal below is
    // checked by its errno, and the state after it read back, since a
    // call that failed half-way is the bug this would miss.
    errno = 0;
    utest_check(setpriority(PRIO_PROCESS, 0, -5) == -1 && errno == EPERM,
                "lowering our own nice is refused with EPERM");
    utest_check(getpriority(PRIO_PROCESS, 0) == 0, "and left it alone");
    errno = 0;
    utest_check(setpriority(PRIO_PROCESS, 1, 5) == -1 && errno == EPERM,
                "renicing another process (init) is refused with EPERM");
    struct sched_param sp = { 10 };
    errno = 0;
    utest_check(sched_setscheduler(0, SCHED_FIFO, &sp) == -1 && errno == EPERM,
                "asking for SCHED_FIFO is refused with EPERM");
    utest_check(sched_getscheduler(0) == SCHED_OTHER, "and left us SCHED_OTHER");
    sp.sched_priority = 0;
    utest_check(sched_setscheduler(0, SCHED_OTHER, &sp) == 0,
                "dropping ourselves to SCHED_OTHER is allowed");
    utest_check(setpriority(PRIO_PROCESS, 0, 25) == -1, "a nice outside -20..19 is refused");

    // The reuse: a child raises itself to 12 and exits, then the next
    // spawn -- which takes the lowest free slot, very likely the same
    // one -- must start at OUR level, not the dead child's.
    int pid = sys_spawn("/tests/prio_test", "child lower", -1);
    int code = -1;
    if (pid > 0) sys_waitpid(pid, &code);
    int kid = child_level();
    utest_checkf(kid == 0, "a reused slot does not keep its last tenant's level (got %d)", kid);

    // Anyone may make itself LESS important, and its children follow.
    utest_checkf(setpriority(PRIO_PROCESS, 0, 5) == 0, "raising our own nice to 5");
    int got = getpriority(PRIO_PROCESS, 0);
    utest_checkf(got == 5, "it reads back (got %d)", got);
    kid = child_level();
    utest_checkf(kid == 5, "a child starts at its parent's nice (got %d)", kid);
    errno = 0;
    utest_check(setpriority(PRIO_PROCESS, 0, 0) == -1 && errno == EPERM,
                "and cannot take it back");

    // Reading is not changing: anyone may ask about anyone.
    sp.sched_priority = -1;
    utest_check(sched_getparam(1, &sp) == 0 && sp.sched_priority == 0,
                "init itself is SCHED_OTHER, read through sched_getparam");
    return utest_end();
}
