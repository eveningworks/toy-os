// Console ownership and the foreground process group -- see
// kernel/tty.h, which states what this is NOT (a line discipline) and
// why the INTR recognition is here for now.
#include "tty.h"
#include "signal.h"
#include "signal_abi.h"
#include "scheduler.h"
#include "errno.h"

// Who claimed fd 0, and what is in front of it. Two variables rather
// than one because they answer different questions and move at
// different times: ownership changes when a process starts or stops
// reading the console, and the foreground group changes every time that
// process runs a job.
static int g_owner_pid;
static int g_fg_pgid;

int tty_console_owner(void) { return g_owner_pid; }
int tty_foreground_pgid(void) { return g_fg_pgid; }

void tty_set_console_owner(int pid) {
    g_owner_pid = pid;
    // THE OWNER'S OWN GROUP GOES IN FRONT, so the console is never in
    // the state "somebody owns it and nothing is in front of it" -- in
    // which a Ctrl-C would have nowhere to go and would be silently
    // dropped rather than falling back to the line editor. A shell moves
    // it from here per job; one that never does simply keeps this.
    g_fg_pgid = pid ? scheduler_pgid(pid) : 0;
}

int tty_set_foreground_pgid(int pgid) {
    if (!g_owner_pid) return -ENODEV;
    if (scheduler_current_pid() != g_owner_pid) return -EPERM;
    if (pgid < 1 || !scheduler_pgid_live(pgid)) return -ESRCH;
    g_fg_pgid = pgid;
    return 0;
}

int tty_intr(void) {
    // Nobody reading the console: the kernel shell is at the prompt, or
    // a compositor holds the keyboard. Ctrl-C is a keystroke.
    if (!g_owner_pid || !g_fg_pgid) return 0;

    // THE SHELL'S OWN GROUP IS IN FRONT, so there is no job to
    // interrupt. Deliver the byte, and send nothing: the line editor's
    // KLINE_CANCEL is the right and only response, and a SIGINT here
    // would rely on the shell having remembered to ignore it.
    //
    // Compared against the OWNER'S group rather than tracking a
    // "foreground job or not" flag, because the flag would be a second
    // record of the same fact -- and the one that goes stale when a
    // shell exits without restoring the foreground group.
    if (g_fg_pgid == scheduler_pgid(g_owner_pid)) return 0;

    // A job is running. Signal the whole group -- which is why groups
    // exist here at all: a pipeline is several processes, and killing
    // only one leaves the shell waiting on the rest.
    //
    // CONSUMED EVEN IF THE GROUP HAS ALREADY DIED. The alternative is to
    // fall through and push the byte, which would hand a stray 0x03 to
    // whoever reads next -- and the shell, one instruction from putting
    // its own group back in front, is exactly who that is.
    signal_send_group(g_fg_pgid, SIGINT);
    return 1;
}
