// tty -- who owns the physical console, and who is holding the keyboard.
//
// **THIS IS NOT POSIX'S `tty`.** That one prints the device name of the
// terminal on stdin (`/dev/pts/3`) and this OS has no device nodes to
// name, so printing a path here would be inventing one. What it answers
// instead is the question that actually comes up on this machine, and
// which nothing could answer before: I typed something and nothing
// happened -- who is getting my keystrokes? On Linux that takes
// `ps -o tpgid` plus `fuser /dev/tty`; there is no single command,
// because there is a filesystem to ask instead.
//
// THREE PARTIES, and they fail in ways that look identical from a dead
// keyboard: nobody owns the console, somebody owns it but no job is in
// front, or a compositor took the keyboard away from ring 0 entirely.
// That last one is the ordinary graphical boot -- a blank console with
// a live desktop above it is correct, not broken.
//
// It reports and never changes anything. Ownership is claimed by the
// first read of fd 0 (kernel/tty.h) and the foreground group is set by
// a shell through SYS_TCSETPGRP; a command that could hand either one
// around would be a way to point somebody else's Ctrl-C at a process of
// your choosing, which tty_set_foreground_pgid() exists to refuse.
#include "rt/sys.h"
#include "lib/cmd.h"
#include <stdio.h>
#include <string.h>
#include "proc_info.h"

// The name behind a pid, or NULL. A SCAN rather than sys_proc_info(pid
// - 1): pid == slot + 1 is true in the kernel today and is not part of
// the ABI, and a listing that silently named the wrong process would be
// worse than one that named none.
static const char *name_of(int pid, char *buf, int cap) {
    if (pid <= 0) return 0;
    struct proc_info info;
    for (int i = 0; i < SYS_PROC_MAX; i++) {
        if (!sys_proc_info(i, &info)) continue;
        if (info.pid != pid) continue;
        snprintf(buf, cap, "%s", info.name);
        return buf;
    }
    return 0;
}

static void print_who(const char *label, const char *unit, int id, const char *empty) {
    char line[128], name[PROC_NAME_MAX];
    const char *n = name_of(id, name, sizeof name);
    if (id <= 0) {
        snprintf(line, sizeof line, "%s%s\n", label, empty);
    } else if (n) {
        snprintf(line, sizeof line, "%s%s %d (%s)\n", label, unit, id, n);
    } else {
        // A live id with no row in the table is the kernel context: the
        // physical shell runs a /bin binary through the legacy loader,
        // which has no procs[] slot at all (see /bin/ps's own note).
        snprintf(line, sizeof line, "%s%s %d\n", label, unit, id);
    }
    sys_print(line);
}

int main(int argc, char **argv) {
    (void)argv;
    if (argc > 1) {
        cmd_usage("tty");
        return 1;
    }

    struct query_tty t;
    if (sys_query_record(QUERY_TTY, 0, &t, sizeof t) < (int)sizeof t) {
        cmd_fail("tty", 0);
        return 1;
    }

    print_who("console owner:    ", "pid",  (int)t.owner_pid,       "nobody");
    print_who("foreground group: ", "pgid", (int)t.foreground_pgid, "none");

    // The keyboard line is LAST because it is the one that explains the
    // two above: an owner of 0 is alarming on a text boot and entirely
    // normal under a desktop, and this is the line that says which.
    if (t.flags & QUERY_TTY_COMPOSITOR) {
        char name[PROC_NAME_MAX], line[128];
        const char *n = name_of((int)t.compositor_pid, name, sizeof name);
        snprintf(line, sizeof line, "keyboard:         held by the compositor (pid %d%s%s)\n",
                 (int)t.compositor_pid, n ? ", " : "", n ? n : "");
        sys_print(line);
    } else if (t.flags & QUERY_TTY_CLAIMED) {
        sys_print("keyboard:         read by a ring-3 process through fd 0\n");
    } else {
        sys_print("keyboard:         the ring-0 console\n");
    }

    // Stated separately from the reason above because BOTH reasons can
    // hold at once (api/keyboard.h keeps two flags for exactly that),
    // and because this is the line that says what it costs: the kernel
    // shell's prompt is not reading anything while it is true.
    if (t.flags & QUERY_TTY_SUSPENDED)
        sys_print("                  ring 0's blocking readers are stood down\n");

    // Ctrl-C is the thing people are usually asking about, so say
    // whether it can work rather than leaving it to be inferred from
    // two ids. A group in front that IS the owner is a shell at its own
    // prompt -- the key cancels the line and signals nothing, which is
    // the one case tty_intr() deliberately does not consume.
    if (t.foreground_pgid == 0)
        sys_print("\nCtrl-C: nothing to interrupt -- no group is in front of the console\n");
    else if (t.foreground_pgid == t.owner_pid)
        sys_print("\nCtrl-C: cancels the line -- the console's owner is its own foreground group\n");
    else
        sys_print("\nCtrl-C: sends SIGINT to the foreground group\n");
    return 0;
}
