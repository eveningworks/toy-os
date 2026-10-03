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
// AND IT WRITES TO STDOUT, NOT STDERR -- a choice made when fd 2 was
// the kernel log for every process, so an error written there reached
// `dmesg` and not the person who typed the command. /bin/ls made the
// same call. fd 2 is now the TERMINAL's wherever there is one
// (docs/decisions.md, "stderr is the terminal's, and the kernel log only
// without one"), so flipping this is one line -- but services include it
// too, and their failures would move from `log -u <name>` (stdout) to
// the kernel log (stderr). That is the part still to decide.
#include "rt/sys.h"

// "<prog>: <subject>: <reason>", with the errno given rather than read.
// This is the shape a `ufileop_policy.on_error` callback needs: it is
// HANDED the errno, and reading sys_errno() there would report whatever
// syscall ran last instead of the one that failed. cp, rm, mv and
// install each wrote this out.
// The same line with the reason in words, for a refusal whose errno
// alone would read wrong.
static inline void cmd_fail_msg(const char *prog, const char *subject, const char *reason) {
    sys_print(prog);
    sys_print(": ");
    if (subject) {
        sys_print(subject);
        sys_print(": ");
    }
    sys_print(reason);
    sys_print("\n");
}

static inline void cmd_fail_err(const char *prog, const char *subject, int err) {
    cmd_fail_msg(prog, subject, sys_strerror(err));
}

// The same line, for the errno the last failing syscall left. Pass NULL
// for `subject` when the failure is not about a particular argument
// (sync).
static inline void cmd_fail(const char *prog, const char *subject) {
    cmd_fail_err(prog, subject, sys_errno());
}

// "usage: <text>", same stream as cmd_fail() and for the same reason. Its own function only so the word `usage`
// is spelled one way across every command -- a listing where half say
// "usage:" and half say "Usage:" is the kind of drift nobody fixes.
static inline void cmd_usage(const char *text) {
    sys_print("usage: ");
    sys_print(text);
    sys_print("\n");
}

// A line or byte COUNT as head and tail take it: plain decimal, with
// nothing after it. Returns 1 and *out, or 0 for anything else -- "10k"
// and "-3" are refused rather than read as 10 and 3.
static inline int cmd_parse_count(const char *s, unsigned long long *out) {
    if (!s || !*s) return 0;
    unsigned long long v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return 0;
        if (v > (~0ull - 9) / 10) return 0;
        v = v * 10 + (unsigned long long)(*s - '0');
    }
    *out = v;
    return 1;
}

#endif // ULIB_CMD_H
