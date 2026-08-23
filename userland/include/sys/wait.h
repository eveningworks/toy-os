#ifndef ULIB_SYS_WAIT_H
#define ULIB_SYS_WAIT_H

// POSIX's <sys/wait.h>: waiting for a child, and DECODING what came
// back.
//
// THE MACROS ARE THE POINT OF THIS HEADER. SYS_WAITPID already existed
// and already reported all three outcomes -- it encodes them as a plain
// exit code, `SIGNAL_EXIT_BASE + sig` (128) for a death by signal, and
// `SIGNAL_STOP_BASE + sig` (256) for a stop. Every caller decoded that
// by hand against those two constants, which is a rule copied into each
// program rather than stated once. These are the standard spelling of
// it, and they sit on the same constants (abi/signal_abi.h) so there is
// still one encoding.
//
// THE ENCODING IS NOT LINUX'S, deliberately. Linux packs the status into
// bit fields (`(sig << 8) | 0x7f` and so on) for historical reasons no
// new system has to inherit; toy-os uses three disjoint ranges, which is
// readable in a log and needs no shifting. Portable code never looks --
// it asks these macros, which is exactly why POSIX specifies the macros
// and not the layout.
#include <signal_abi.h>
#include "rt/sys.h"

// waitpid() options. Bit values so they can be OR-ed, matching the two
// distinct kernel entry points below.
#define WNOHANG    1  // do not block if no child has anything to report
#define WUNTRACED  2  // report a child that STOPPED, as well as one that
                      // died -- which means a status can name a child
                      // that is still alive

// EXITED NORMALLY? Then WEXITSTATUS is what it passed to exit().
#define WIFEXITED(s)    ((s) >= 0 && (s) < SIGNAL_EXIT_BASE)
#define WEXITSTATUS(s)  (s)

// KILLED BY A SIGNAL? Then WTERMSIG names it.
#define WIFSIGNALED(s)  ((s) >= SIGNAL_EXIT_BASE && (s) < SIGNAL_STOP_BASE)
#define WTERMSIG(s)     ((s) - SIGNAL_EXIT_BASE)

// STOPPED, and still alive? Then WSTOPSIG names the signal that did it.
// Only ever seen with WUNTRACED -- without it the kernel keeps waiting.
#define WIFSTOPPED(s)   ((s) >= SIGNAL_STOP_BASE)
#define WSTOPSIG(s)     ((s) - SIGNAL_STOP_BASE)

// CONTINUED is deliberately absent. Nothing reports it: SIGCONT acts at
// send time and never enters the pending set, so there is no moment at
// which a waiter could observe it. A WIFCONTINUED() that always
// returned 0 would be a question with a wrong answer rather than no
// question. See docs/decisions/kernel.md on stop and continue.

// Wait for `pid` (or -1 for any child). Returns the pid that reported,
// 0 with WNOHANG when nothing has, or -1 with errno. `status` may be
// NULL.
int waitpid(int pid, int *status, int options);

// Block for any child at all. POSIX's wait(), which is waitpid(-1, s, 0).
static inline int wait(int *status) { return waitpid(-1, status, 0); }

#endif
