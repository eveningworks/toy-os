// Proves a pseudo-terminal end to end, from ring 3.
//
// The claim under test is the whole point of the TTY layer: a pty
// behaves like the physical console because it IS a terminal, running
// the same line discipline. So this asserts the discipline's behaviour
// through a pair of file descriptors, with no keyboard and no screen
// anywhere -- and then the thing that actually matters, that a `0x03`
// written to the master reaches a spawned child as SIGINT.
//
// Exits 0 only if every phase worked; each failure has its own code so a
// failing run says WHICH link broke rather than just "no". A raw exit
// code rather than printed output because the caller
// (kernel/tty/pty_test.c) asserts on it.
#include <stdint.h>
#include "rt/sys.h"

#define CHILD "/tests/spin_test"

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

// Drain whatever the terminal has emitted so far. NON-BLOCKING is not
// available here, so this reads exactly once -- every caller below has
// already made sure there is something to read.
static int drain(int master, char *buf, int cap) {
    int n = (int)sys_read(master, buf, (size_t)cap);
    return n < 0 ? 0 : n;
}

int main(void) {
    int master = -1, slave = -1;
    char buf[256];

    if (sys_openpty(&master, &slave) < 0) return 1;
    if (master < 0 || slave < 0) return 2;

    // --- 1. it says it is a terminal ---------------------------------
    //
    // Both ends, because a program asking "am I on a terminal?" holds
    // whichever end it was given, and an answer that depended on which
    // would make isatty() a lie half the time.
    struct sys_stat st;
    if (sys_fstat(slave, &st) < 0 || !(st.flags & SYS_STAT_TTY)) return 3;
    if (sys_fstat(master, &st) < 0 || !(st.flags & SYS_STAT_TTY)) return 4;

    // --- 2. canonical mode, which is the default ---------------------
    //
    // Typed at the master, read at the slave. Nothing must be readable
    // until the newline: that is what ICANON means, and it is the check
    // a discipline that queued as it went would fail while passing
    // everything else here.
    if (sys_write(master, "hi", 2) != 2) return 5;

    // ...and the ECHO of those two characters comes back on the master,
    // which is how a terminal emulator paints what was typed. Reading it
    // now also proves the two directions are separate queues.
    int n = drain(master, buf, sizeof buf);
    if (n != 2 || buf[0] != 'h' || buf[1] != 'i') return 6;

    if (sys_write(master, "\n", 1) != 1) return 7;
    n = (int)sys_read(slave, buf, sizeof buf);
    if (n != 3 || buf[0] != 'h' || buf[1] != 'i' || buf[2] != '\n') return 8;

    // THE NEWLINE IS ECHOED TOO, and draining it here is not tidying --
    // it is an assertion. A terminal that echoed the characters but not
    // the Enter would leave every line of a transcript running into the
    // next, which is what a real one does with ECHO and ONLCR. Missing
    // this was a bug in an earlier version of THIS FILE: the stray byte
    // turned up in the next phase's read and looked exactly like the
    // output path carrying an extra character.
    n = drain(master, buf, sizeof buf);
    if (n != 1 || buf[0] != '\n') return 23;

    // --- 3. raw mode ------------------------------------------------
    struct tty_termios tio;
    if (sys_tcgetattr(slave, &tio) < 0) return 9;
    if (!(tio.lflag & TTY_ICANON)) return 10; // the default must be POSIX's
    if (sys_tty_raw(slave) < 0) return 11;

    if (sys_write(master, "x", 1) != 1) return 12;
    n = (int)sys_read(slave, buf, sizeof buf);
    if (n != 1 || buf[0] != 'x') return 13;
    // ECHO is off now, so nothing came back the other way. Not asserted
    // by reading -- a read of an empty master BLOCKS, and a test that
    // hung would be indistinguishable from a test that failed.

    // --- 4. output flows the other way -------------------------------
    if (sys_write(slave, "out", 3) != 3) return 14;
    n = drain(master, buf, sizeof buf);
    if (n != 3 || !contains(buf, n, "out")) return 15;

    // --- 5. INTR REACHES A CHILD, which is the whole point -----------
    //
    // A spawned process in its own group, put in front of the terminal,
    // then 0x03 written to the master exactly as a terminal emulator
    // would on Ctrl-C. It must die of SIGINT -- 128 + SIGINT, the
    // convention a shell prints.
    //
    // spin_test makes no syscalls while it spins, so the ONLY way it can
    // be interrupted is signal delivery on a timer tick. A child that
    // polled would be killed by the other path and would say nothing
    // about this one.
    if (sys_tcsetattr(slave, &tio) < 0) return 16; // ISIG back on with ICANON

    int saved_in = sys_dup(0), saved_out = sys_dup(1);
    sys_dup2(slave, 0);
    sys_dup2(slave, 1);
    int pid = sys_spawn_group(CHILD, "900000", -1, 0, PGID_NEW);
    if (saved_in >= 0)  { sys_dup2(saved_in, 0);  sys_close(saved_in); }
    if (saved_out >= 0) { sys_dup2(saved_out, 1); sys_close(saved_out); }
    if (pid < 0) return 17;

    if (sys_tcsetpgrp(master, pid) < 0) return 18;
    if (sys_write(master, "\x03", 1) != 1) return 19;

    int status = 0;
    if (sys_waitpid(pid, &status) < 0) return 20;
    if (status != 128 + SIGINT) return 21;

    // --- 6. end of file, both directions -----------------------------
    //
    // The rule a shell is written against, and the one a terminal gets
    // wrong: a read with nothing available and the other end still open
    // means "wait", and only a CLOSED other end means end of input.
    // Conflating them is how a terminal decides a running program has
    // finished.
    sys_close(slave);
    n = (int)sys_read(master, buf, sizeof buf);
    if (n != 0) return 22; // no slave left -> EOF, not a block

    sys_close(master);
    return 0;
}
