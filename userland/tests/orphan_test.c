// Makes ORPHANS on purpose, so init's reaping can be measured.
//
// It spawns N short-lived children and exits IMMEDIATELY without
// waiting for any of them. Each child therefore outlives its parent,
// is adopted by init (scheduler.c's reparent_children()), and must end
// up reaped -- slot freed -- rather than sitting as a zombie for the
// rest of the boot, which is what happened before init existed.
//
// The assertion is not in here: this process is gone before its
// children are. tools/slot_balance.py counts the process table before
// and after and is the thing that passes or fails.
//
// NOTE it must be started by the SCHEDULER (`gui spawn`, or the shell's
// `spawn`), not by `run`: the legacy loader is not a scheduled process,
// so a child of it has ppid 0 already and there is no orphaning to
// observe. Same reason waitany_test is not in usertest_run.py.
#include "rt/sys.h"
#include "lib/stdio.h"
#include "lib/string.h"

#define CHILD "/tests/exit_test"
#define DEFAULT_N 4

int main(int argc, char **argv) {
    int n = DEFAULT_N;
    if (argc > 1) {
        int v = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
        if (v > 0 && v <= 16) n = v;
    }

    char line[96];
    int made = 0;
    for (int i = 0; i < n; i++) {
        int pid = sys_spawn(CHILD, 0, -1);
        if (pid <= 0) break;
        made++;
        snprintf(line, sizeof line, "orphan: spawned %d\n", pid);
        sys_eprint(line);
    }

    snprintf(line, sizeof line, "orphan: abandoning %d children\n", made);
    sys_eprint(line);

    // No wait. That is the whole point of the program.
    return 0;
}
