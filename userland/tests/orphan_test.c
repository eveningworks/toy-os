// Makes ORPHANS on purpose, so init's reaping can be measured.
//
// It spawns N short-lived children and exits IMMEDIATELY without
// waiting for any of them. Each child therefore outlives its parent,
// is adopted by init (sched_exit.c's reparent_children()), and must end
// up reaped -- slot freed -- rather than sitting as a zombie for the
// rest of the boot, which is what happened before init existed.
//
// The assertion is not in here: this process is gone before its
// children are. tools/slot_balance.py counts the process table before
// and after and is the thing that passes or fails.
//
// `orphan_test N zombies` WAITS UNTIL EVERY CHILD HAS EXITED -- without
// reaping any -- and only then exits, so init adopts N ZOMBIES at once.
// That adoption wakes init once at most, which is the case where it used
// to reap one child per wake (tools/init_test.py times it).
//
// NOTE it must be started by the SCHEDULER (`gui spawn`, or the shell's
// `spawn`), not by `run`: the legacy loader is not a scheduled process,
// so a child of it has ppid 0 already and there is no orphaning to
// observe. Same reason waitany_test is not in usertest_run.py.
#include "rt/sys.h"
#include <stdio.h>
#include <string.h>

#define CHILD "/tests/exit_test"
#define DEFAULT_N 4

// Whether `pid` is a zombie, by walking the table until it ends -- a pid
// is not assumed to be a slot index.
static int is_zombie(int pid) {
    struct proc_info pi;
    for (int i = 0; sys_proc_info(i, &pi) == 0; i++)
        if (pi.pid == pid) return pi.state == PROC_STATE_ZOMBIE;
    return 0;
}

int main(int argc, char **argv) {
    int n = DEFAULT_N;
    if (argc > 1) {
        int v = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
        if (v > 0 && v <= 16) n = v;
    }

    int zombies = argc > 2 && strcmp(argv[2], "zombies") == 0;
    int pids[16];
    char line[96];
    int made = 0;
    for (int i = 0; i < n; i++) {
        int pid = sys_spawn(CHILD, 0, -1);
        if (pid <= 0) break;
        pids[made++] = pid;
        snprintf(line, sizeof line, "orphan: spawned %d\n", pid);
        sys_eprint(line);
    }

    // Bounded: a child that never exits must not keep this one alive.
    for (int t = 0; zombies && t < 100; t++) {
        int dead = 0;
        for (int i = 0; i < made; i++) dead += is_zombie(pids[i]);
        if (dead == made) break;
        sys_sleep_ms(50);
    }

    snprintf(line, sizeof line, "orphan: abandoning %d children\n", made);
    sys_eprint(line);

    // No wait. That is the whole point of the program.
    return 0;
}
