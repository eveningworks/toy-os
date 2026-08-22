#ifndef SIGNAL_ABI_H
#define SIGNAL_ABI_H

// Signals, process groups, and the numbers ring 3 passes for both.
//
// Shared with userland (`/bin/kill`, `/bin/tosh`, the GUI Terminal), so
// it lives in abi/ rather than in a kernel header. The kernel-side API
// is kernel/signal.h; the plan this implements, and what it deliberately
// leaves out, is docs/signals-design.md.
//
// WHY SIGNALS AT ALL, when SYS_KILL could already end a process: because
// `Ctrl-C` cannot be a kill. A key arrives in an interrupt handler, and
// tearing an address space down from there would free page tables under
// whatever the CPU was doing. A signal is precisely the mechanism that
// splits "decide" from "act": the sender sets a bit, and the kernel acts
// on it at the one point where acting is safe -- on the way back to ring
// 3, when the target process is the thing being resumed and holds no
// kernel state. That is what Unix does, for the same reason.
//
// COPY THE SHAPE, NOT THE SIZE. POSIX has 31 standard signals plus 32
// realtime ones, queued delivery, sigaltstack, SA_RESTART and a mask
// per handler. Six signals is enough to make Ctrl-C work and to end a
// process politely, and every one below earns its place. The NUMBERS are
// POSIX's rather than compacted to 1..6 -- they cost nothing, and a
// number meaning something different here from everywhere else is a trap
// for anybody who has used a Unix.

#define SIGINT   2  // what Ctrl-C sends to the foreground group
#define SIGQUIT  3  // the second escape hatch, for when SIGINT is ignored
#define SIGKILL  9  // uncatchable, unignorable -- what a force-quit sends
#define SIGSEGV 11  // a memory fault, given a name
#define SIGTERM 15  // ask politely; `kill` with no signal named
#define SIGCHLD 17  // a child exited. Default action: IGNORE
#define SIGCONT 18  // resume a stopped process. Default action: CONTINUE
#define SIGSTOP 19  // suspend, uncatchable and unignorable
#define SIGTSTP 20  // what Ctrl-Z sends to the foreground group

// The highest signal number this kernel accepts. The pending set is a
// uint32_t bitmask, so 31 is the ceiling the representation allows and
// there is no reason to pick a smaller one.
#define SIGNAL_MAX 31

// 1 if `s` is a signal number this kernel will accept anywhere. Signal 0
// is deliberately NOT valid here: POSIX's `kill(pid, 0)` existence probe
// has no caller in this tree, and accepting it would mean every send
// path carrying a "send nothing" case.
#define SIGNAL_VALID(s) ((s) >= 1 && (s) <= SIGNAL_MAX)

// --- dispositions -----------------------------------------------------
//
// TWO, NOT THREE. A handler is stage 3 of docs/signals-design.md and is
// not built: SYS_SIGACTION refuses anything but these two rather than
// accepting a function pointer it would silently never call.
#define SIG_DFL 0 // the default action -- terminate, except SIGCHLD
#define SIG_IGN 1 // discard it on arrival

// SIGKILL AND SIGQUIT CANNOT BE IGNORED, so there is always something
// that works. SIGKILL is Unix's rule and the reason a force-quit is
// trustworthy; SIGQUIT is this kernel's own second escape hatch, kept
// unignorable because with no handlers a shell that ignores SIGINT (as
// every shell must) would otherwise leave a wedged program with nothing
// pointed at it. POSIX makes SIGQUIT catchable -- a deliberate
// difference, written down rather than assumed.
#define SIGNAL_UNIGNORABLE(s) \
    ((s) == SIGKILL || (s) == SIGQUIT || (s) == SIGSTOP)

// --- stop and continue ------------------------------------------------
//
// **THESE TWO DO NOT GO THROUGH THE PENDING SET, and that is the whole
// reason job control did not break the signal design.** Every other
// signal here means "this process must die", which is what lets
// api/scheduler.h's `pending` be read with no policy lookup at all.
// Suspending is different in kind: it changes nothing about the
// process except whether the scheduler will pick it, it frees no
// memory, and it is therefore safe to apply AT SEND TIME -- including
// from the keyboard IRQ, which is where Ctrl-Z arrives. So a stop is a
// scheduler state flip and never a pending bit, and the invariant
// survives intact.
//
// Linux defers a stop to the target's next signal-check point, which
// this cannot do without handlers; the visible difference is that a
// stopped process here may execute for up to one timer tick more before
// the scheduler stops picking it. Nobody can observe that but the
// process itself.

// 1 if `s` suspends its target rather than terminating it.
#define SIGNAL_STOPS(s) ((s) == SIGSTOP || (s) == SIGTSTP)

// 1 if `s` resumes a stopped target.
#define SIGNAL_CONTINUES(s) ((s) == SIGCONT)

// What a signalled process reports as its exit code: 128 + the signal.
//
// The convention every Unix shell prints, so a Ctrl-C'd program exits
// 130 and a SIGTERM'd one 143 -- distinguishable from an ordinary
// non-zero exit, which is the whole point. This kernel has one plain
// int exit code rather than POSIX's packed wait status, so there is no
// WIFSIGNALED to ask; the number IS the report. A caller that wants the
// signal back subtracts.
#define SIGNAL_EXIT_BASE 128

// What SYS_WAITPID reports for a child that STOPPED rather than exited:
// 256 + the signal that stopped it.
//
// A SECOND BASE RATHER THAN A PACKED STATUS. POSIX answers this with a
// wait status and WIFSTOPPED/WIFEXITED/WIFSIGNALED to take it apart;
// this kernel deliberately chose a plain int above, and changing that
// now would change the meaning of a field init, /bin/tosh, /bin/ps and
// every other waiter already reads. So a stop extends the existing
// convention instead of replacing it: 130 is "died on Ctrl-C", 276 is
// "stopped on Ctrl-Z", and both are one subtraction from the signal.
//
// **THE COST, STATED: a code at or above this base means the child is
// STILL ALIVE.** That is the one thing 128+sig never meant, so a caller
// that treats every waitpid result as an exit is wrong here -- which is
// why the stop is reported ONLY to a waiter that passed SYS_WUNTRACED
// (abi/syscall_abi.h) and asked for it. A caller that does not ask
// cannot see one.
#define SIGNAL_STOP_BASE 256

// 1 if a SYS_WAITPID result says "stopped", rather than any kind of exit.
#define SIGNAL_IS_STOP(code) ((code) >= SIGNAL_STOP_BASE)

// --- process groups ---------------------------------------------------
//
// A process group is an int, and every process is in exactly one. A
// child inherits its spawner's group unless SYS_SPAWN's `pgid` says
// otherwise -- which is how a shell puts a whole pipeline in one group
// with no race, and is where toy-os deliberately differs from POSIX (see
// SYS_SETPGID in syscall_abi.h).
//
// SYS_SETPGID and SYS_TCSETPGRP both read 0 as "the caller's own pid",
// the same shorthand POSIX gives them.
#define PGID_SELF 0

// SYS_SPAWN's `pgid`, as a three-way rather than a number: 0 inherits
// the spawner's group, a positive value JOINS that group, and this means
// the child LEADS A GROUP OF ITS OWN.
//
// It needs a sentinel because the obvious spelling is impossible: a
// shell starting a pipeline wants the first stage to lead, and cannot
// pass that stage's own pid because the pid does not exist until the
// spawn returns. POSIX gets away with `setpgid(0, 0)` from the child
// after fork(); with no fork there is no child to run it.
#define PGID_NEW (-1)

#endif
