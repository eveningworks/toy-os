// SYS_WAITPID(-1) -- reaping ANY child -- from ring 3.
//
// The scheduler-side bookkeeping has KTESTs (kernel/proc/proctree_test.c);
// this is the syscall on top of it, which is the half a KTEST cannot
// reach. It matters because wait-any is what an init's whole main loop
// is (docs/init-design.md), and because a caller has to tell the two
// negative answers apart: -1 means "no children at all" and is
// permanent, where a still-running child blocks (or answers SYS_RETRY
// under WNOHANG).
//
// NOTE it must be started by the SCHEDULER (`gui spawn`, or a KTEST),
// not by the shell's `run`: the legacy loader is not a scheduled
// process, so it has no pid, so nothing it spawns has a parent and
// wait-any has nobody to ask about. That is why this is not in
// tools/usertest_run.py, which drives everything through `run`.
#include "rt/sys.h"
#include "lib/stdio.h"

#define CHILD "/tests/exit_test"

static int fail(const char *why) {
    sys_eprint("waitany: FAIL -- ");
    sys_eprint(why);
    sys_eprint("\n");
    return 1;
}

int main(void) {
    // No children yet: -1 is the permanent answer, and confusing it
    // with "not yet" is how an init spins forever.
    if (sys_waitpid(-1, 0) != -1) return fail("wait-any with no children did not report -1");
    sys_eprint("waitany: no children -> -1, as expected\n");

    int a = sys_spawn(CHILD, 0, -1);
    int b = sys_spawn(CHILD, 0, -1);
    if (a <= 0 || b <= 0) return fail("could not spawn two children");

    char line[96];
    snprintf(line, sizeof line, "waitany: spawned %d and %d\n", a, b);
    sys_eprint(line);

    // Reap both WITHOUT naming either. Each pid must come back exactly
    // once; a reap that returned the same corpse twice, or invented a
    // pid that was never a child, is the failure this looks for.
    int seen_a = 0, seen_b = 0;
    for (int i = 0; i < 2; i++) {
        int code = -1;
        int pid = sys_waitpid(-1, &code);
        if (pid == a && !seen_a) seen_a = 1;
        else if (pid == b && !seen_b) seen_b = 1;
        else return fail("wait-any returned a pid that was not an unreaped child");

        snprintf(line, sizeof line, "waitany: reaped %d (exit %d)\n", pid, code);
        sys_eprint(line);
    }
    if (!seen_a || !seen_b) return fail("did not reap both children");

    // Drained: back to the permanent answer, not a third corpse.
    if (sys_waitpid(-1, 0) != -1) return fail("wait-any kept returning after both were reaped");

    sys_eprint("waitany: PASSED\n");
    return 0;
}
