// <signal.h>'s implementation: a thin face over rt/sys.h's wrappers.
//
// The conversion between POSIX's `struct sigaction` and the kernel's
// `struct k_sigaction` is the whole of this file's substance. Everything
// else is a rename.
#include <signal.h>
#include <errno.h>
#include "rt/sys.h"

__sighandler_t signal(int sig, __sighandler_t h) {
    // sys_signal() already fills in the restorer and sets SA_RESTART,
    // which is exactly signal()'s contract. Same function underneath,
    // different spelling -- not a second implementation.
    return (__sighandler_t)sys_signal(sig, (sighandler_t)h);
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    struct k_sigaction kact, kold;

    // A NON-EMPTY MASK IS REFUSED, not ignored. There is no syscall to
    // block a signal outside its own handler, so honouring `sa_mask`
    // is impossible -- and a call that returned 0 having quietly not
    // done it would leave the caller believing a critical section is
    // protected when it is not. Refusing names the gap at the call site.
    if (act && act->sa_mask != 0) {
        *__errno_location() = EINVAL;
        return -1;
    }

    if (act) {
        kact.handler  = (uint64_t)act->sa_handler;
        // THE RESTORER IS OURS TO SUPPLY, and only with a real handler:
        // sys_sigaction() answers a handler with no restorer with
        // EINVAL rather than guessing, and SIG_DFL/SIG_IGN never
        // return anywhere so they must not carry one.
        kact.restorer = SIG_IS_HANDLER(kact.handler) ? (uint64_t)__sigrestore : 0;
        kact.flags    = (uint32_t)(act->sa_flags & SA_RESTART);
        kact._pad     = 0;
    }

    if (sys_sigaction(sig, act ? &kact : 0, old ? &kold : 0) < 0) return -1;

    if (old) {
        old->sa_handler = (__sighandler_t)kold.handler;
        old->sa_mask    = 0;   // nothing was ever blocked -- see above
        old->sa_flags   = (int)kold.flags;
    }
    return 0;
}

int raise(int sig) { return sys_kill(sys_getpid(), sig); }

int kill(int pid, int sig) {
    // POSIX's pid <= 0 forms name a process GROUP or every process, and
    // SYS_KILL takes one pid. Refused rather than passed through, since
    // a negative pid reaching the kernel would be ESRCH at best and the
    // wrong process at worst.
    if (pid <= 0) {
        *__errno_location() = EINVAL;
        return -1;
    }
    return sys_kill(pid, sig);
}

// Indexed by signal number, so a gap is a NULL and falls through to the
// default below -- rather than a dispatch chain, which is what
// tools/check_dispatch.py exists to prevent and what this would grow
// into one branch at a time.
static const char *const g_names[SIGNAL_MAX + 1] = {
    [SIGINT]  = "Interrupt",
    [SIGQUIT] = "Quit",
    [SIGILL]  = "Illegal instruction",
    [SIGFPE]  = "Arithmetic exception",
    [SIGKILL] = "Killed",
    [SIGUSR1] = "User defined signal 1",
    [SIGSEGV] = "Segmentation fault",
    [SIGUSR2] = "User defined signal 2",
    [SIGTERM] = "Terminated",
    [SIGCHLD] = "Child exited",
    [SIGCONT] = "Continued",
    [SIGSTOP] = "Stopped (signal)",
    [SIGTSTP] = "Stopped",
    [SIGTTIN] = "Stopped (tty input)",
    [SIGWINCH]= "Window changed",
};

const char *strsignal(int sig) {
    if (SIGNAL_VALID(sig) && g_names[sig]) return g_names[sig];
    return "Unknown signal";
}

// --- the blocked mask -------------------------------------------------
//
// A sigset_t here is a bitmask of the 31 signals in an unsigned long,
// which is what the kernel keeps too -- so these are the syscall with
// errno bookkeeping around it, and no translation that could drift.

int sigprocmask(int how, const sigset_t *set, sigset_t *old) {
    uint64_t s = set ? (uint64_t)*set : 0;
    uint64_t o = 0;
    int rc = sys_sigprocmask(how, set ? &s : 0, old ? &o : 0);
    if (rc < 0) { errno = -rc; return -1; }
    if (old) *old = (sigset_t)o;
    return 0;
}

int sigsuspend(const sigset_t *mask) {
    uint64_t m = mask ? (uint64_t)*mask : 0;
    int rc = sys_sigsuspend(&m);
    // -1/EINTR IS THE NORMAL RETURN, not a failure to report differently.
    errno = rc < 0 ? -rc : EINTR;
    return -1;
}
