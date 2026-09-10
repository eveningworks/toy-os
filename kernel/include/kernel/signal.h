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
// idt.c, after the trap has been handled, and only when the interrupted
// frame was ring 3's. Does nothing for a pid with nothing pending, which
// is the overwhelmingly common case and is one load and one branch.
//
// `regs` is the trapframe about to be resumed -- which a handler
// REWRITES, since entering one means pointing the process at different
// code with a different stack.
//
// **`at_syscall_entry` IS THE WHOLE OF SA_RESTART**, and getting it
// wrong is silent. It means "the interrupted trap is an `int $0x80`
// whose handler has not run yet", which is true at exactly one of the
// two call sites (idt.c's pre-dispatch check) and false at the other
// (the tail of a trap, where any syscall has already produced its
// result). Passing 1 from the tail would rewind RIP over a call that
// already happened and run it a second time.
//
// **RETURNS WHETHER IT ACTED**, and the syscall-entry caller must obey
// it. That call site delivers INSTEAD OF running the syscall, so a
// delivery that decides to do nothing has to say so or the syscall is
// silently swallowed -- which is exactly the bug that shipped for an
// hour: `pending` includes signals BLOCKED by a running handler, so a
// handler's own SYS_SIGRETURN was eaten and the restorer fell through
// to its `ud2`. Asking "is anything pending" and then finding nothing
// deliverable is a legal outcome, not an impossible one.
//
// May not return in the ordinary sense: terminating the CURRENT process
// hands the CPU to something else through scheduler_on_exit(), exactly
// as SYS_EXIT does.
// WHERE THE TRAP STANDS relative to the interrupted syscall -- the
// third argument. TRAP_DONE: whatever it was has produced its result.
// AT_SYSCALL_ENTRY: the syscall has not run, and the frame can be
// rewound over the `int $0x80` (SA_RESTART) or given -EINTR.
// BEFORE_REISSUE: a signal wake already rewound the frame onto the
// `int $0x80` and a hardware interrupt landed before it executed --
// the syscall has not run either, but the frame must be stepped
// FORWARD for -EINTR and left alone for SA_RESTART.
#define SIG_TRAP_DONE        0
#define SIG_AT_SYSCALL_ENTRY 1
#define SIG_BEFORE_REISSUE   2
int signal_deliver_pending(int pid, uint64_t *regs, int at_syscall_entry);

// A SYNCHRONOUS fault, offered to the process that caused it. Returns 1
// if `regs` now enters a ring-3 handler -- the caller resumes normally
// and must NOT tear the process down -- and 0 if there is no handler, no
// room for a frame, or the fault happened inside this signal's own
// handler, in which case the caller's existing teardown is the answer.
//
// THE ASYMMETRY WITH THE PENDING PATH IS DELIBERATE: this acts
// immediately rather than setting a bit, because a fault is not
// something that arrived from elsewhere -- the process is standing on
// the instruction that caused it, and there is nowhere to defer it TO.
// That is the same reason it cannot be restarted (signal.c).
int signal_deliver_fault(int pid, int sig, uint64_t *regs);

// Restores the frame a handler is returning through. SYS_SIGRETURN's
// implementation, and the only caller. Returns 0 if the frame is not one
// the kernel built, which is a program that corrupted its own stack.
int signal_restore_frame(int pid, uint64_t *regs);

// The NAMES are api/ksignal.h, not here: `/bin/kill` parses one off a
// command line and may not include a kernel-internal header, so the
// table is compiled into both rings rather than written down twice.

#endif
