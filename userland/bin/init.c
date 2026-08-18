// init -- pid 1, the process every other ring-3 process ends up under.
//
// Stage 1 of docs/init-design.md, and deliberately the minimum that
// earns its keep: it REAPS. A zombie is only ever cleared by somebody
// waiting for it, so before this an orphan -- a process whose parent
// died first -- held its slot for the rest of the boot with nothing in
// the system able to free it.
//
// The kernel does the adopting (scheduler.c's reparent_children() hands
// a dying process's children to whatever pid this holds); this side is
// just the loop that then collects them.
//
// TWO STATES, AND THE SECOND IS WHY SYS_SLEEP EXISTS:
//
//   - With children, it blocks in waitpid(-1) and consumes NOTHING
//     until one dies. That is the whole loop on a busy system.
//   - With NO children, waitpid(-1) answers -1, which is permanent (see
//     its ABI comment: "no children at all" is not "not yet"). There is
//     nothing to block on, so it sleeps. A yield-loop here would burn a
//     core forever and make every CPU figure in the system meaningless.
//
// What it deliberately does NOT do yet: start anything. The desktop is
// still started by `gui` at the shell, so init is nobody's parent until
// a process dies leaving children behind. Stage 2 gives it a target to
// start and supervise, at which point the first branch above becomes
// the common one.
#include "rt/sys.h"
#include "lib/stdio.h"

// Long enough that an idle machine wakes ~4 times a second, short
// enough that a freshly adopted orphan is reaped promptly even if the
// adoption's wake is missed. The kernel wakes child-waiters on adoption
// (reparent_children()), so this is a backstop rather than the
// mechanism -- which is the right way round: a poll that is load-
// bearing is a poll whose interval is a correctness constant.
#define IDLE_SLEEP_MS 250

int main(void) {
    // No pid of its own to report -- there is no getpid() in this ABI,
    // and the kernel names the pid it spawned init as anyway.
    char msg[96];
    sys_eprint("init: reaping orphans\n");

    for (;;) {
        int code = 0;
        int pid = sys_waitpid(-1, &code);

        if (pid > 0) {
            // Report every reap on stderr, which reaches the kernel log
            // and `dmesg`. An init that silently absorbs corpses is one
            // nobody can tell apart from an init that has died.
            snprintf(msg, sizeof msg, "init: reaped orphan pid %d (code %d)\n",
                     pid, code);
            sys_eprint(msg);
            continue;
        }

        // -1: no children at all. Nothing to wait on, so idle.
        sys_sleep_ms(IDLE_SLEEP_MS);
    }
}
