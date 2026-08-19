#ifndef ULIB_CMD_H
#define ULIB_CMD_H

// The three lines every small /bin command writes the same way: say what
// went wrong, say which argument it was about, and say why in the words
// abi/errno.h chose.
//
// A header rather than nine copies for CLAUDE.md's stated reason -- the
// survey that produced kernel/lib/ found the same twenty lines written
// nine times for int-to-string. This is that shape at a smaller scale,
// and the bar (a second real caller, not a plausible one) is met several
// times over: mkdir, rm, mv, ln, stat, truncate, sync and df all print
// exactly this.
//
// Inline rather than a .c file because it is three calls and lives on
// the far side of --gc-sections either way; a command that includes it
// and never fails links nothing.
//
// AND IT WRITES TO STDOUT, NOT STDERR, WHICH IS DELIBERATE HERE.
// fd 2 is the KERNEL LOG in this OS, not a second terminal stream (see
// abi/syscall_abi.h): a shell captures its child's stdout through a
// pipe and never sees fd 2, so a diagnostic written there is perfectly
// recorded in `dmesg` and invisible to the person who typed the
// command. /bin/ls made the same call for the same reason. This flips
// to stderr the day a TTY layer gives fd 2 somewhere a terminal can
// see -- it is one line, in one file, because of this header.
#include "rt/sys.h"

// "<prog>: <subject>: <reason>", where the reason is the errno the last
// failing syscall left. Pass NULL for `subject` when the failure is not
// about a particular argument (sync).
static inline void cmd_fail(const char *prog, const char *subject) {
    sys_print(prog);
    sys_print(": ");
    if (subject) {
        sys_print(subject);
        sys_print(": ");
    }
    sys_print(sys_strerror(sys_errno()));
    sys_print("\n");
}

// "usage: <text>", same stream as cmd_fail() and for the same reason. Its own function only so the word `usage`
// is spelled one way across every command -- a listing where half say
// "usage:" and half say "Usage:" is the kind of drift nobody fixes.
static inline void cmd_usage(const char *text) {
    sys_print("usage: ");
    sys_print(text);
    sys_print("\n");
}

#endif // ULIB_CMD_H
