// setpriority()/getpriority() from ring 3, and what a child starts at.
//
// Two bugs this exists for. A negative level read back as -1 -- the raw
// syscall returned it, and the wrapper took it for an errno. And a slot
// reused by a new process kept its LAST tenant's level, so a program
// started after a -10 driver exited ran at -10 without asking. The
// second only shows through a real spawn, which is why this is a
// program and not a KTEST.
//
// `prio_test child` is the child: it exits with 20 - its level, the
// same offset the syscall uses, so every level is a positive code.
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
        // The leak's victim: a child that lowers itself and exits leaves
        // its slot at that level for whoever gets the slot next.
        if (argc > 2 && !strcmp(argv[2], "lower")) setpriority(PRIO_PROCESS, 0, -12);
        return 20 - mine;
    }

    utest_begin("prio_test", "process priority, and what a child inherits",
                UTEST_VERDICT_FILE);

    int start = getpriority(PRIO_PROCESS, 0);
    utest_checkf(setpriority(PRIO_PROCESS, 0, -5) == 0, "set -5");
    int got = getpriority(PRIO_PROCESS, 0);
    utest_checkf(got == -5, "a NEGATIVE level reads back (got %d)", got);
    utest_check(setpriority(PRIO_PROCESS, 0, 25) == -1, "a level outside -20..19 is refused");

    int kid = child_level();
    utest_checkf(kid == -5, "a child starts at its parent's level (got %d)", kid);

    // The reuse: a child lowers itself to -12 and exits, then the next
    // spawn -- which takes the lowest free slot, very likely the same
    // one -- must start at OUR level, not the dead child's.
    setpriority(PRIO_PROCESS, 0, 0);
    int pid = sys_spawn("/tests/prio_test", "child lower", -1);
    int code = -1;
    if (pid > 0) sys_waitpid(pid, &code);
    kid = child_level();
    utest_checkf(kid == 0, "a reused slot does not keep its last tenant's level (got %d)", kid);

    setpriority(PRIO_PROCESS, 0, start);
    return utest_end();
}
