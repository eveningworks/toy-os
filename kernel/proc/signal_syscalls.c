// The ring-3 surface of signals and process groups: SYS_SETPGID,
// SYS_GETPGID, SYS_SIGACTION, SYS_SIGRETURN, SYS_TCSETPGRP,
// SYS_TCGETPGRP.
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
#include "signal.h" // signal_restore_frame() -- SYS_SIGRETURN's whole body
#include "vmm.h"
#include "klog.h"
#include "kfmt.h"   // klog_printf()
#include "syscall.h" // syscall_process_exit_cleanup()

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
    int sig = (int)(int32_t)c->a0;
    uint64_t uact = c->a1, uold = c->a2;

    if (!SIGNAL_VALID(sig)) { c->regs[14] = (uint64_t)(int64_t)-EINVAL; return 0; }

    // SIGKILL, SIGQUIT AND SIGSTOP, so there is always something that
    // works. Refused even for a plain read of the old action -- not
    // because reading would hurt, but because a caller that can read one
    // will try to write one, and a call that succeeds on the query and
    // fails on the install teaches the wrong thing about which signals
    // are off limits. abi/signal_abi.h has why SIGQUIT is on this list
    // and why the argument for it got weaker when handlers landed.
    if (SIGNAL_UNIGNORABLE(sig)) { c->regs[14] = (uint64_t)(int64_t)-EPERM; return 0; }

    struct sigaction act, old;
    if (uact) {
        if (!vmm_copy_from_user(c->pml4, &act, uact, sizeof act)) {
            c->regs[14] = (uint64_t)(int64_t)-EFAULT;
            return 0;
        }
        // A HANDLER WITHOUT A RESTORER IS REFUSED, not silently given
        // one. There is nothing sensible the kernel could substitute:
        // the restorer is ring-3 code, and a handler that returns to a
        // guessed address faults on the way out of something that
        // otherwise worked -- the hardest possible place to debug it.
        // x86-64 Linux refuses the same call for the same reason.
        if (SIG_IS_HANDLER(act.handler) && !act.restorer) {
            c->regs[14] = (uint64_t)(int64_t)-EINVAL;
            return 0;
        }
        // Every OTHER field is the caller's business, but an unknown
        // flag is not: accepting one means a program can ask for
        // behaviour it will not get and have no way to find out.
        if (act.flags & ~(uint32_t)SA_RESTART) {
            c->regs[14] = (uint64_t)(int64_t)-EINVAL;
            return 0;
        }
    }

    // -1 means the caller has no scheduler slot: the legacy
    // process_run_ring3() loader, which has no signal state to set.
    if (scheduler_signal_set_action(scheduler_current_pid(), sig,
                                    uact ? &act : 0, &old) < 0) {
        c->regs[14] = (uint64_t)(int64_t)-EPERM;
        return 0;
    }

    // THE INSTALL HAS ALREADY HAPPENED IF THIS COPY FAILS, and that is
    // the honest ordering rather than the tidy one: the alternative is
    // validating the out pointer first, which is a second walk of the
    // page tables to make an error path prettier. A caller that passes a
    // bad `old` gets -EFAULT and an installed action, which POSIX allows
    // and which is strictly less surprising than an install that
    // silently did not happen.
    if (uold && !vmm_copy_to_user(c->pml4, uold, &old, sizeof old)) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }

    c->regs[14] = 0;
    return 0;
}

// THE ONE SYSCALL THAT DOES NOT RETURN TO ITS CALLER. It rewrites the
// trapframe it was called through, so the `int $0x80` in the restorer
// resumes as whatever the handler interrupted -- which is why nothing
// below writes regs[14] on the success path: RAX is part of the restored
// state, and overwriting it with a return value would corrupt the very
// register the interrupted code was using.
int sys_sigreturn(struct syscall_ctx *c) {
    int pid = scheduler_current_pid();
    if (signal_restore_frame(pid, c->regs)) return 0;

    // NO FRAME MEANS THE PROCESS IS ALREADY LOST. It either called this
    // by hand -- there is nothing to return to -- or corrupted the stack
    // the frame was on, in which case resuming it would resume garbage.
    // Killed with SIGSEGV rather than handed an error code, because
    // there is no register left that a caller could read one out of.
    klog_printf("signal: pid %d called sigreturn with no valid frame\n", pid);
    syscall_process_exit_cleanup(vmm_current_pml4());
    scheduler_on_exit(SIGNAL_EXIT_BASE + SIGSEGV);
    return 1; // never reached; keeps the dispatcher from writing a result
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
