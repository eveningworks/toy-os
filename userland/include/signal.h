#ifndef ULIB_SIGNAL_H
#define ULIB_SIGNAL_H

// C's <signal.h> plus the POSIX half that toy-os can actually honour.
//
// THIS HEADER WAS ON THE "NOT" LIST AND ITS REASON EXPIRED.
// docs/libc-design.md ruled it out with "a libc that ships a signal()
// which cannot deliver anything is worse than one that does not declare
// it" -- correct when written, and signals were built afterwards
// (sigaction with SA_RESTORER, SA_RESTART, SIGCHLD on a child's death,
// process groups, job control). The capability arrived; the header
// follows it.
//
// THE NUMBERS ARE THE KERNEL'S -- abi/signal_abi.h, one list, so a
// number means the same thing on both sides of the syscall boundary.
// That header also declares `struct k_sigaction`, the KERNEL's shape;
// POSIX's `struct sigaction` below is a different structure with
// different field names and is converted at the call. glibc makes the
// same split for the same reason (its `struct kernel_sigaction`) -- the
// two cannot be one struct, because the kernel's carries a restorer the
// caller must not have to know about and POSIX's carries a mask the
// kernel has no syscall for.
#include <signal_abi.h>
#include <stddef.h>

typedef void (*__sighandler_t)(int);

// An integer an asynchronous handler may read and write. Plain `int`
// here: this is a single-core, non-preemptive-within-a-handler world
// and every access to an int is already atomic on x86-64.
typedef int sig_atomic_t;

// SIG_DFL and SIG_IGN are signal_abi.h's, as plain 0 and 1. Cast here to
// the handler type C requires them to have.
#undef SIG_DFL
#undef SIG_IGN
#define SIG_DFL ((__sighandler_t)0)
#define SIG_IGN ((__sighandler_t)1)
// rt/sys.h defines SIG_ERR too, as the same value under its own handler
// typedef -- guarded rather than undef'd, since the two are the same
// pointer and a redefinition warning would be noise.
#ifndef SIG_ERR
#define SIG_ERR ((__sighandler_t)-1)
#endif

// --- signal sets ------------------------------------------------------
//
// A BITMASK OF THE 31 SIGNALS. The five set operations are pure bit
// arithmetic and need nothing from the kernel; sigprocmask() and
// sigsuspend() below are real syscalls now (SYS_SIGPROCMASK,
// SYS_SIGSUSPEND) over a per-process mask the kernel already kept for
// handler re-entry.
typedef unsigned long sigset_t;

static inline int sigemptyset(sigset_t *s) { if (!s) return -1; *s = 0; return 0; }
static inline int sigfillset(sigset_t *s)  { if (!s) return -1; *s = ~0UL; return 0; }
static inline int sigaddset(sigset_t *s, int n) {
    if (!s || !SIGNAL_VALID(n)) return -1;
    *s |= 1UL << n;
    return 0;
}
static inline int sigdelset(sigset_t *s, int n) {
    if (!s || !SIGNAL_VALID(n)) return -1;
    *s &= ~(1UL << n);
    return 0;
}
static inline int sigismember(const sigset_t *s, int n) {
    if (!s || !SIGNAL_VALID(n)) return -1;
    return (*s & (1UL << n)) != 0;
}

// --- dispositions -----------------------------------------------------

// POSIX's action. `sa_mask` is DECLARED and REFUSED rather than
// silently ignored: sigaction() returns -1/EINVAL for a non-empty one
// (see the .c file). A parser rejects rather than guesses; so does this.
struct sigaction {
    __sighandler_t sa_handler;
    sigset_t       sa_mask;
    int            sa_flags;
};

// SA_RESTART is the kernel's, from signal_abi.h, and is the only flag
// there is. SA_SIGINFO is deliberately absent: no sender pid is
// recorded and no fault address is passed through, so a siginfo_t here
// would be a struct of zeroes.
#define SA_NOCLDSTOP 0   // accepted and inert: a stop is never reported
                         // to a parent as SIGCHLD in the first place

// Install `h` and return the previous disposition, or SIG_ERR.
// BSD/glibc semantics: the handler STAYS installed across deliveries and
// interrupted syscalls restart. SIGKILL, SIGQUIT and SIGSTOP are
// refused with EPERM, so there is always something that works.
__sighandler_t signal(int sig, __sighandler_t h);

// Install and/or read back an action. Either pointer may be NULL.
// Returns 0, or -1 with errno -- EINVAL for an unknown signal or a
// non-empty `sa_mask`, EPERM for the three that cannot be caught.
int sigaction(int sig, const struct sigaction *act, struct sigaction *old);

// SIGKILL and SIGSTOP are DROPPED from whatever you ask for rather than
// refused, which is POSIX's rule and is enforced in the kernel -- so a
// sigfillset() mask is accepted and those two still arrive.
// 0, or -1 with errno.
int sigprocmask(int how, const sigset_t *set, sigset_t *old);

// Install `mask`, wait until a signal it does not block is delivered,
// restore the previous mask. **ALWAYS returns -1 with errno EINTR** --
// there is no success return, which is why POSIX gives it none.
//
// The pair below is NOT the same thing and has a race this does not:
//
//     sigprocmask(SIG_SETMASK, &mask, &old);   // a signal arriving
//     pause();                                 // HERE is lost
//
// One syscall, with interrupts off across the check and the park, is
// what closes it.
//
// **ONE DIVERGENCE FROM POSIX, stated because it is invisible:** the
// handler runs under the mask that was in force BEFORE the sigsuspend,
// plus the signal being delivered -- not under `mask`. Re-entry of the
// handler's own signal is still prevented (the kernel blocks it for the
// length of the handler either way), so what differs is only whether
// OTHER signals named in `mask` can interrupt the handler. Linux defers
// the restore to the sigreturn to get this exactly right; that needs a
// second saved mask this kernel does not carry.
int sigsuspend(const sigset_t *mask);

// Send `sig` to this process. C's own, and the only part of this header
// ISO C requires.
int raise(int sig);

// Send `sig` to `pid`. POSIX's negative-pid forms (a process GROUP, or
// every process) are not accepted -- SYS_KILL takes one pid, and a
// silently-ignored sign would kill the wrong thing.
int kill(int pid, int sig);

// A short description of `sig` -- "Interrupt", "Segmentation fault".
// Returns a static string, never NULL: an unknown number gives
// "Unknown signal".
const char *strsignal(int sig);


// ONE PAST THE HIGHEST SIGNAL NUMBER, which is what every `for (i = 1;
// i < NSIG; i++)` loop over dispositions expects. SIGNAL_MAX is the
// kernel's name for the highest; this is the C one for the bound.
#ifndef NSIG
#define NSIG (SIGNAL_MAX + 1)
#endif

#endif
