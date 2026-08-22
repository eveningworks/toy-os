#ifndef KERNEL_SIGNAL_H
#define KERNEL_SIGNAL_H

#include <stdint.h>

// Signal POLICY: what a signal means, who may send one, and -- the part
// that decides the whole design -- WHEN the kernel is allowed to act on
// it. The per-process state (the pending mask, the ignore mask, the
// group) lives in the process table and is reached through
// api/scheduler.h; the numbers ring 3 passes are abi/signal_abi.h. The
// staged plan is docs/signals-design.md.
//
// THE ONE IDEA HERE: SENDING AND ACTING ARE DIFFERENT MOMENTS.
//
// A signal can be sent from anywhere -- another process's syscall, a
// fault handler, the keyboard IRQ. Terminating a process means freeing
// its page tables and its kernel-side bookkeeping, which calls the heap;
// doing that from an interrupt that landed in the middle of somebody
// else's kmalloc corrupts it. So sending only ever sets a bit (and wakes
// the target if it is parked, so that it gets somewhere useful), and the
// acting happens at exactly one place: signal_deliver_pending(), called
// from isr_dispatch() when the interrupted frame came from RING 3.
//
// That condition is what makes it safe, and it is worth stating plainly:
// if the CPU was executing ring-3 code when the trap arrived, then the
// kernel was not in the middle of anything on anybody's behalf, and the
// process about to be resumed is the one whose state we are about to
// throw away. It is the same point Linux delivers at, arrived at from
// the same constraint.
//
// The cost, stated: delivery is up to ONE TIMER TICK late for a process
// the scheduler happened to switch away from in the same trap. That is
// the ordinary Unix property -- a signal is delivered at the target's
// next return to user mode, not at the sender's convenience.

// Send `sig` to `pid`. Returns 1 if it reached a live process, 0 for a
// pid with no live slot or a signal number that is not one.
//
// SIGKILL IS THE EXCEPTION AND MUST STAY ONE: it terminates the target
// immediately, here, ignoring dispositions -- which is what makes a
// force-quit work against a process wedged in its own loop, and why
// Task Manager can be trusted. Safe because scheduler_kill() is built
// for killing somebody ELSE and never touches the live address space;
// it is NOT safe from an interrupt handler, which is why the INTR key
// sends SIGINT rather than SIGKILL.
int signal_send(int pid, int sig);

// Send `sig` to every live member of `pgid`. Returns how many processes
// it reached; 0 means the group has no live members, which a caller
// should treat as "nothing to signal" rather than as an error.
int signal_send_group(int pgid, int sig);

// Act on anything pending for `pid`, if the moment is right. Called from
// exactly one place -- isr_dispatch(), after the trap has been handled,
// and only when the interrupted frame was ring 3's. Does nothing for a
// pid with nothing pending, which is the overwhelmingly common case and
// is one load and one branch.
//
// May not return in the ordinary sense: terminating the CURRENT process
// hands the CPU to something else through scheduler_on_exit(), exactly
// as SYS_EXIT does.
void signal_deliver_pending(int pid);

// The NAMES are api/ksignal.h, not here: `/bin/kill` parses one off a
// command line and may not include a kernel-internal header, so the
// table is compiled into both rings rather than written down twice.

#endif
