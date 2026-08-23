// Signal a process that is parked in a blocking read, then feed it.
//
// The other half of signal_test's SA_RESTART checks, and it exists
// because THE ORDER IS THE TEST: the parent must be genuinely blocked
// when the signal lands, or a restarted read and a never-interrupted
// one are the same observation. So this sleeps first, signals, sleeps
// again, and only then writes the byte the parent is waiting for.
//
// The sleeps are the honest weakness and are worth naming rather than
// hiding: they make the ordering overwhelmingly likely, not certain.
// What turns that into a real test is on signal_test's side -- it runs
// the same sequence twice, once with SA_RESTART and once without, and
// asserts the two differ. A run where the parent was not parked gives
// the SAME answer both times and fails, so a lost race is visible as a
// failure rather than as a pass.
//
// Its stdout IS the pipe the parent reads (sys_spawn_group's stdout_fd),
// which is why there is no fd argument.
#include "rt/sys.h"
#include <stdlib.h>
#include <string.h>

// Long enough that the parent has certainly reached its read() on a TCG
// guest under parallel load, short enough not to matter to a suite.
#define SETTLE_MS 120

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    int pid = atoi(argv[1]);
    int sig = atoi(argv[2]);

    sys_sleep_ms(SETTLE_MS);
    sys_kill(pid, sig);
    sys_sleep_ms(SETTLE_MS);
    // ONE BYTE, and its value is checked: a parent that reads back
    // something else has restarted into a read of the wrong thing.
    sys_write(1, "R", 1);
    return 0;
}
