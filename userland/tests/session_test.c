// A terminal belongs to a SESSION, and a child in that session may take
// its foreground group.
//
// **THE BUG THIS EXISTS FOR**, found on real hardware: `dash` typed into
// a Terminal window exited at once with "Cannot set tty process group
// (operation not permitted)". The terminal was owned by the /bin/tosh
// that Terminal had started, and the rule was "only the owning PID may
// move the foreground group" -- so a second shell, being a different
// process, could never take job control of the terminal it was running
// on. POSIX keys that on the SESSION for exactly this reason.
//
// It has to run from ring 3: the permission asks who the CURRENT
// process is, and a KTEST runs in the kernel context, which is no
// process. kernel/tty/tty_test.c drives this by exit code.
#include <stdint.h>
#include "rt/sys.h"

int main(void) {
    int master = -1, slave = -1;
    char buf[64];

    if (sys_openpty(&master, &slave) < 0) return 1;

    // **OWNERSHIP IS CLAIMED ON THE FIRST READ OF THE SLAVE**, not at
    // open -- see kernel/tty/tty_fd.c, where the comment explains that the
    // opener is a terminal emulator and the reader is the shell. So
    // write something first and read it back, and THIS process is the
    // owner from here on.
    if (sys_write(master, "x\n", 2) != 2) return 2;
    if (sys_read(slave, buf, sizeof buf) <= 0) return 3;

    // The owner itself may move the foreground group -- true before this
    // change too, and here so a failure says which half broke.
    if (sys_tcsetpgrp(slave, sys_getpgid(0)) < 0) return 4;

    // THE CLAIM: a CHILD -- a different process, same session -- may do
    // it too. fork() rather than spawn: the child needs this fd and this
    // session, which is exactly what a fork gives it.
    int pid = sys_fork();
    if (pid < 0) return 9;
    if (pid == 0) {
        // In its own group, as a shell puts a job -- then take the
        // terminal. Before the session rule this was -EPERM, which is
        // the error dash printed.
        if (sys_setpgid(0, 0) < 0) sys_exit(6);
        if (sys_tcsetpgrp(slave, sys_getpgid(0)) < 0) sys_exit(5);
        sys_exit(0);
    }

    int status = 0;
    if (sys_waitpid(pid, &status) < 0) return 7;
    if (status != 0) return status;

    // And the session is INHERITED, which is what makes the above work
    // rather than it being a special case for children.
    if (sys_getsid(0) != sys_getsid(sys_getpid())) return 8;

    sys_close(master);
    sys_close(slave);
    return 0;
}
