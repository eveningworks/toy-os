// Proves that a traced child's trace reaches the TRACER'S TERMINAL.
//
// **THE ONLY CHECK THAT CAN SEE THE THING THAT CHANGED.** Tracing
// itself has KTESTs (kernel/proc/strace_test.c) over the formatter, and
// a trace has always been visible in `dmesg` -- so every existing check
// passes whether the lines go to the physical console, to a window, or
// nowhere a person is looking. What moved when `strace` became a ring-3
// program is WHERE the lines come out: the kernel resolves the tracer's
// fd 1 to a terminal at the spawn and writes there. A pty is how that
// becomes assertable with no screen anywhere -- point fd 1 at one, ask
// for a traced child, and read the master.
//
// Exits 0 only if every phase worked; each failure has its own code so a
// failing run says WHICH link broke. A raw exit code rather than printed
// output for the reason pty_test gives: fd 1 is a pty here, so anything
// printed would go into the very buffer being asserted on.
#include <stdint.h>
#include "rt/sys.h"

// A child that exits at once and makes a handful of syscalls on the way
// -- enough to trace, few enough that the whole trace fits the master's
// buffer without draining mid-run.
#define CHILD "/bin/hello"

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int contains(const char *hay, int hay_len, const char *needle) {
    int nl = slen(needle);
    if (nl == 0) return 1;
    for (int i = 0; i + nl <= hay_len; i++) {
        int j = 0;
        while (j < nl && hay[i + j] == needle[j]) j++;
        if (j == nl) return 1;
    }
    return 0;
}

int main(void) {
    int master = -1, slave = -1;
    static char buf[2048];

    if (sys_openpty(&master, &slave) < 0) return 1;

    // NON-BLOCKING FIRST, AND IT IS NOT A CONVENIENCE. A read of an
    // empty master BLOCKS, and every drain below is asking "is there
    // anything yet" -- so without this a phase that fails by producing
    // nothing would hang the test instead of failing it, which is the
    // worse of the two outcomes by a long way.
    if (sys_set_nonblock(master, 1) < 0) return 2;

    // fd 1 IS WHAT THE KERNEL RESOLVES, so this is the whole setup: from
    // here on, this process's stdout names a terminal that is not the
    // console, and anything that lands on the master got there by the
    // kernel having looked fd 1 up.
    if (sys_dup2(slave, 1) < 0) return 3;

    // --- 1. an UNTRACED spawn puts no trace on the terminal ----------
    //
    // THE CONTROL, AND IT RUNS FIRST. Without it "the master has trace
    // text on it" is also what a kernel that traced every process would
    // produce, and this test would pass against a tracing switch that
    // ignored the flag entirely.
    int plain = sys_spawn_flags(CHILD, 0, -1, 0, 0, 0);
    if (plain <= 0) return 4;
    sys_waitpid(plain, 0);

    int n = 0;
    for (int i = 0; i < 8; i++) {
        int r = (int)sys_read(master, buf + n, sizeof buf - 1 - (unsigned)n);
        if (r > 0) n += r;
        if (n >= (int)sizeof buf - 1) break;
    }
    buf[n > 0 ? n : 0] = '\0';
    // The child's own output is expected here; a trace line is not.
    if (contains(buf, n, "syscalls traced")) return 5;
    if (contains(buf, n, "exit(")) return 6;

    // --- 2. a TRACED spawn does ---------------------------------------
    int traced = sys_spawn_flags(CHILD, 0, -1, 0, 0, SPAWN_TRACE);
    if (traced <= 0) return 7;
    sys_waitpid(traced, 0);

    n = 0;
    for (int i = 0; i < 16; i++) {
        int r = (int)sys_read(master, buf + n, sizeof buf - 1 - (unsigned)n);
        if (r > 0) n += r;
        if (n >= (int)sizeof buf - 1) break;
    }
    buf[n > 0 ? n : 0] = '\0';

    // A DECODED LINE, not merely "some bytes arrived". The child prints
    // its own output down this same terminal, so a check for ink would
    // pass with no tracing at all; `exit(` is text only the tracer
    // writes.
    if (!contains(buf, n, "exit(")) return 8;
    // ...and the summary, which is the kernel's and comes out at
    // release. It is the half that proves the sink is still known when
    // the traced process is being torn down.
    if (!contains(buf, n, "syscalls traced")) return 9;

    // --- 3. an unknown flag is REFUSED, not ignored -------------------
    //
    // The ABI's own promise (abi/syscall_abi.h). A flag word that drops
    // what it does not recognise can never be extended safely, and the
    // failure is silent, so this is checked rather than assumed.
    if (sys_spawn_flags(CHILD, 0, -1, 0, 0, 0x8000u) > 0) return 10;

    return 0;
}
