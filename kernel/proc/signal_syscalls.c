// The ring-3 surface of signals and process groups: SYS_SETPGID,
// SYS_GETPGID, SYS_SIGACTION, SYS_TCSETPGRP, SYS_TCGETPGRP.
//
// SYS_KILL is deliberately NOT here -- it lives with the other process
// syscalls in proc_syscalls.c, where it always has. Moving it would make
// this file look like "everything signal-shaped" when what it actually
// is, is the calls that had nowhere else to go.
//
// Each of these is a thin wrapper: the rules live in scheduler.c (the
// state) and tty.c (the console's foreground group), so a caller reaching
// them through the shell or through a syscall cannot get different
// answers. All five report -errno on failure, per abi/errno.h.
#include "syscalls.h"
#include "syscall_abi.h"
#include "signal_abi.h"
#include "scheduler.h"
#include "tty.h"
#include "errno.h"

// PGID_SELF (0) means "me", in both arguments of both group calls -- the
// same shorthand POSIX gives setpgid() and getpgid(). Resolved once,
// here, rather than in each handler.
static int this_pid_if_zero(int pid) {
    return pid == PGID_SELF ? scheduler_current_pid() : pid;
}

int sys_setpgid(struct syscall_ctx *c) {
    int pid  = this_pid_if_zero((int)(int32_t)c->a0);
    int pgid = (int)(int32_t)c->a1;
    // pgid 0 means "the same value as pid" -- lead a new group. POSIX's
    // rule, and the only way a process can name a group that does not
    // exist yet.
    if (pgid == PGID_SELF) pgid = pid;

    int64_t r;
    if (pid < 1 || !scheduler_pid_alive(pid)) {
        r = -ESRCH;
    } else if (!scheduler_setpgid(pid, pgid)) {
        // The remaining refusal is "that group has no live member and is
        // not `pid` itself" -- EPERM rather than ESRCH, matching POSIX,
        // where joining a group you may not join is a permission answer.
        r = -EPERM;
    } else {
        r = 0;
    }
    c->regs[14] = (uint64_t)r;
    return 0;
}

int sys_getpgid(struct syscall_ctx *c) {
    int pid = this_pid_if_zero((int)(int32_t)c->a0);
    int pgid = scheduler_pgid(pid);
    c->regs[14] = (uint64_t)(int64_t)(pgid > 0 ? pgid : -ESRCH);
    return 0;
}

int sys_sigaction(struct syscall_ctx *c) {
    int sig  = (int)(int32_t)c->a0;
    int disp = (int)(int32_t)c->a1;

    int64_t r;
    if (!SIGNAL_VALID(sig) || (disp != SIG_DFL && disp != SIG_IGN)) {
        // A HANDLER POINTER LANDS HERE, and that is the intended
        // outcome: refusing one outright beats accepting it and never
        // calling it, which is what a userland program would otherwise
        // have to discover by watching itself die. Stage 3 of
        // docs/signals-design.md is what makes this legal.
        r = -EINVAL;
    } else if (SIGNAL_UNIGNORABLE(sig)) {
        // SIGKILL and SIGQUIT, so there is always something that works.
        // Refused even for SIG_DFL -- setting a signal to the
        // disposition it already has would succeed and teach a caller
        // that the call sometimes works on it.
        r = -EPERM;
    } else {
        int was = scheduler_signal_set_ignored(scheduler_current_pid(), sig,
                                                disp == SIG_IGN);
        // -1 means the caller has no scheduler slot: the legacy
        // process_run_ring3() loader, which has no signal state to set.
        r = was < 0 ? -EPERM : (was ? SIG_IGN : SIG_DFL);
    }
    c->regs[14] = (uint64_t)r;
    return 0;
}

// **BOTH TAKE AN fd NOW.** They took none while "the console" was a
// complete answer; it stopped being one when a Terminal window got a
// terminal of its own, and a shell that could only ever move the
// console's foreground group would be aiming the physical keyboard's
// Ctrl-C at its own child.
//
// Every rule is tty.c's, including who is allowed to call this -- the
// terminal's owner, and only it. A second copy of that check here is the
// kind of duplication that drifts (CLAUDE.md's note on the context menu
// re-implementing wm_request_close()).
int sys_tcsetpgrp(struct syscall_ctx *c) {
    struct tty *t = fd_tty(c->pml4, (int)(int32_t)c->a0);
    if (!t) { c->regs[14] = (uint64_t)(int64_t)-ENOTTY; return 0; }
    c->regs[14] = (uint64_t)(int64_t)tty_set_fg_pgid(t, (int)(int32_t)c->a1);
    return 0;
}

int sys_tcgetpgrp(struct syscall_ctx *c) {
    struct tty *t = fd_tty(c->pml4, (int)(int32_t)c->a0);
    if (!t) { c->regs[14] = (uint64_t)(int64_t)-ENOTTY; return 0; }
    int pgid = tty_fg_pgid(t);
    c->regs[14] = (uint64_t)(int64_t)(pgid > 0 ? pgid : -ENODEV);
    return 0;
}
