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
#include "tty_abi.h" // TTY_ICANON/ECHO/ISIG -- the mode line below

// The name behind a pid, or NULL. A SCAN rather than sys_proc_info(pid
// - 1): pid == slot + 1 is true in the kernel today and is not part of
// the ABI, and a listing that silently named the wrong process would be
// worse than one that named none.
static const char *name_of(int pid, char *buf, int cap) {
    if (pid <= 0) return 0;
    struct proc_info info;
    for (int i = 0; sys_proc_info(i, &info) == 0; i++) {
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

// One terminal. `first` decides whether a blank line separates it from
// the one above -- a listing of several needs the separation and a
// machine with only the console must not start with an empty line.
static void print_tty(const struct query_tty *t, int first) {
    char head[128];
    if (!first) sys_print("\n");
    // NAMED, because with more than one of them "console owner" stops
    // being a complete sentence. tty0 keeps the word "console" beside
    // its number: it is the one every reader already has a name for.
    snprintf(head, sizeof head, "tty%d (%s)%s\n",
             (int)t->index, t->driver,
             t->index == 0 ? " -- the physical console" : "");
    sys_print(head);

    print_who("  owner:            ", "pid",  (int)t->owner_pid,       "nobody");
    print_who("  foreground group: ", "pgid", (int)t->foreground_pgid, "none");

    // WHAT THE DISCIPLINE IS DOING, in the words termios uses. A
    // terminal in canonical mode returns nothing until Enter, which
    // from outside is indistinguishable from one that has stopped
    // working -- so it is stated rather than left to be deduced.
    char modes[96];
    snprintf(modes, sizeof modes, "  mode:             %s%s%s\n",
             (t->lflag & TTY_ICANON) ? "canonical" : "raw",
             (t->lflag & TTY_ECHO)   ? ", echo" : "",
             (t->lflag & TTY_ISIG)   ? ", isig" : "");
    sys_print(modes);

    if (t->flags & QUERY_TTY_BYPASS)
        sys_print("                    (discipline MUTED -- a compositor has the keyboard)\n");
}

int main(int argc, char **argv) {
    (void)argv;
    if (argc > 1) {
        cmd_usage("tty");
        return 1;
    }

    // WALKED UNTIL A RECORD IS REFUSED rather than against a count read
    // first: a list's length is itself a fact and can change between two
    // reads, so a loop bounded by an earlier answer can walk off the end
    // of a shorter list. /bin/parttable does the same.
    struct query_tty t;
    int n = 0;
    for (int i = 0; i < 16; i++) {
        if (sys_query_record(QUERY_TTY, i, &t, sizeof t) < (int)sizeof t) break;
        print_tty(&t, n == 0);
        n++;
    }
    if (n == 0) {
        cmd_fail("tty", 0);
        return 1;
    }

    // The console's own record, re-read for the machine-wide part
    // below: the keyboard and the compositor are the MACHINE's, not any
    // one terminal's, and saying them once under a listing of several is
    // what stops them reading as facts about the last row printed.
    if (sys_query_record(QUERY_TTY, 0, &t, sizeof t) < (int)sizeof t) {
        cmd_fail("tty", 0);
        return 1;
    }
    sys_print("\n");

    // The keyboard line is LAST because it is the one that explains the
    // owners above: an owner of 0 is alarming on a text boot and
    // entirely normal under a desktop, and this is the line that says
    // which.
    if (t.flags & QUERY_TTY_COMPOSITOR) {
        char cname[PROC_NAME_MAX], line[128];
        const char *who = name_of((int)t.compositor_pid, cname, sizeof cname);
        snprintf(line, sizeof line, "keyboard: held by the compositor (pid %d%s%s)\n",
                 (int)t.compositor_pid, who ? ", " : "", who ? who : "");
        sys_print(line);
    } else if (t.flags & QUERY_TTY_CLAIMED) {
        sys_print("keyboard: read by a ring-3 process through fd 0\n");
    } else {
        sys_print("keyboard: the ring-0 console\n");
    }

    // Stated separately from the reason above because BOTH reasons can
    // hold at once (api/keyboard.h keeps two flags for exactly that),
    // and because this is the line that says what it costs: the kernel
    // shell's prompt is not reading anything while it is true.
    if (t.flags & QUERY_TTY_SUSPENDED)
        sys_print("          ring 0's blocking readers are stood down\n");

    // Ctrl-C is the thing people are usually asking about, so say
    // whether it can work rather than leaving it to be inferred from two
    // ids. **ABOUT tty0 SPECIFICALLY** -- it is the terminal the
    // physical keyboard reaches, and the one whose Ctrl-C a person
    // pressing Ctrl-C right now would be using. A window's terminal has
    // its own answer, in its own row above.
    if (t.foreground_pgid == 0)
        sys_print("\nCtrl-C on tty0: nothing to interrupt -- no group is in front of it\n");
    else if (t.foreground_pgid == t.owner_pid)
        sys_print("\nCtrl-C on tty0: cancels the line -- its owner is its own foreground group\n");
    else
        sys_print("\nCtrl-C on tty0: sends SIGINT to the foreground group\n");
    return 0;
}
