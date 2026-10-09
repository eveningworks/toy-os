// Proves the whole trace path, end to end: /bin/strace's lines reach
// ITS STDERR -- the kernel recorded, strace decoded -- and nowhere else.
//
// **THE ONLY CHECK THAT SEES WHERE THE TEXT COMES OUT.** The decoder has
// /tests/utrace_test and the ring /tests/tracering_test; both pass
// whether or not a person would ever see a line. A pty is how "it is on
// the terminal" becomes assertable with no screen: strace's fd 2 is the
// slave, and the master is read.
//
// Exits 0 only if every phase worked; each failure has its own code so a
// failing run says WHICH link broke. A raw exit code rather than printed
// output, as pty_test does: anything printed would land in the buffer
// being asserted on. Driven by kernel/proc/strace_test.c.
#include <stdint.h>
#include "rt/sys.h"
#include "syscall_abi.h"

// A child that exits at once and makes a handful of syscalls on the way.
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

static char g_buf[8192];

static int drain(int master, int tries) {
    int n = 0;
    for (int i = 0; i < tries; i++) {
        int r = (int)sys_read(master, g_buf + n, sizeof g_buf - 1 - (unsigned)n);
        if (r > 0) n += r;
        if (n >= (int)sizeof g_buf - 1) break;
    }
    g_buf[n > 0 ? n : 0] = '\0';
    return n;
}

int main(void) {
    int master = -1, slave = -1;
    if (sys_openpty(&master, &slave) < 0) return 1;
    if (sys_set_nonblock(master, 1) < 0) return 2;

    // THE CONTROL: the child untraced, its stderr on the pty. No trace
    // text may appear -- a pass on the real run means nothing otherwise.
    struct sys_spawn_opts o;
    sys_spawn_opts_init(&o);
    o.stderr_fd = slave;
    int plain = sys_spawn_opts(CHILD, &o);
    if (plain <= 0) return 4;
    sys_waitpid(plain, 0);
    int n = drain(master, 8);
    if (contains(g_buf, n, "+++ exited")) return 5;
    if (contains(g_buf, n, "exit(")) return 6;

    // THE REAL RUN: /bin/strace with its stderr on the pty.
    sys_spawn_opts_init(&o);
    o.args = CHILD;
    o.stderr_fd = slave;
    int traced = sys_spawn_opts("/bin/strace", &o);
    if (traced <= 0) return 7;
    int code = -1;
    sys_waitpid(traced, &code);
    n = drain(master, 32);
    if (!contains(g_buf, n, "exit(0) = ?")) return 8;
    if (!contains(g_buf, n, "+++ exited with 0 +++")) return 9;
    if (code != 0) return 11;

    // A trace with nowhere to go is refused, and so is a flag nobody
    // defined -- an old kernel accepting an unknown flag would do nothing.
    if (sys_spawn_flags(CHILD, 0, -1, 0, 0, SPAWN_TRACE) > 0) return 12;
    if (sys_spawn_flags(CHILD, 0, -1, 0, 0, 0x8000u) > 0) return 10;
    return 0;
}
