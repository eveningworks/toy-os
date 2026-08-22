#ifndef KERNEL_TTY_H
#define KERNEL_TTY_H

// The beginnings of a TTY layer: who owns the physical console, which
// process group is in FRONT of it, and what the INTR key does.
//
// **THIS IS NOT A LINE DISCIPLINE, AND THE INTR RECOGNITION BELOW IS
// TEMPORARY** -- written down now rather than discovered later as a
// layering mistake. On Unix, `Ctrl-C` is not a kernel feature at all: a
// terminal's line discipline recognises the INTR character and signals
// the terminal's foreground process group. toy-os has no line discipline
// (fd 0 is RAW -- no echo control, no cooked mode; see
// docs/roadmap.md's TTY track), so the recognition lives in the keyboard
// driver, which is the one place a key arrives. When a discipline
// exists, the INTR decision moves into it and this header keeps only the
// ownership half.
//
// WHY THERE IS ONE OF THESE AND NOT ONE PER TERMINAL: there is one
// physical keyboard and one console. Multiple virtual terminals are a
// roadmap item, and when they land this state becomes per terminal --
// which is bookkeeping, because everything below is already asked
// through functions rather than read as globals.

// The process that owns the console -- the first one to read fd 0 -- or
// 0 when nobody does (the kernel shell is at the prompt, or a compositor
// holds the keyboard).
//
// CLAIMED ON THE FIRST READ rather than at spawn, which is
// syscall_fd.c's existing rule and the reason it is that file that sets
// this: a process that never reads the console must not silence the
// kernel shell, and there is no other moment the kernel could learn the
// difference.
int  tty_console_owner(void);
// Set by syscall_fd.c when the console is claimed and released. Setting
// an owner also puts that owner's own group in the foreground, so there
// is never a window where the console has an owner and no foreground
// group; releasing clears both.
void tty_set_console_owner(int pid);

// The group in FRONT of the console, or 0 when nobody owns it.
//
// A GROUP, not a pid, and that is the difference that makes `Ctrl-C`
// work on a pipeline: `cat big | grep x | less` is three processes, and
// interrupting only the last one leaves two running with the shell still
// waiting on them. This is `tcgetpgrp()`.
int  tty_foreground_pgid(void);

// Put `pgid` in front. Returns 0, or a negative errno: -EPERM if the
// caller does not own the console, -ENODEV if nobody does, -ESRCH for a
// group with no live member. This is `tcsetpgrp()`, and SYS_TCSETPGRP is
// a thin wrapper over it.
//
// **ONLY THE OWNER MAY CALL IT.** Not a privilege check in the sense
// this kernel does not have (see SYS_KILL) -- it is an ownership one: a
// process that has never read the console has no business deciding what
// the keyboard interrupts, and letting it would be a way to point
// somebody else's Ctrl-C at a process of your choosing.
int  tty_set_foreground_pgid(int pgid);

// The INTR key (Ctrl-C) arrived. Returns 1 if it was CONSUMED -- a
// foreground job existed and has been sent SIGINT -- and 0 if the caller
// should deliver the keystroke as an ordinary byte instead.
//
// THE TWO OUTCOMES ARE THE TWO STATES A SHELL IS IN, and both are right:
//
//   - A JOB IS RUNNING (the foreground group is not the console owner's
//     own): the group gets SIGINT and the byte is DISCARDED, which is
//     what a real line discipline does with INTR. Delivering it too
//     would leave a stray 0x03 in the input queue for whoever reads
//     next.
//   - NO JOB IS RUNNING (the shell's own group is in front, or nobody
//     owns the console at all): nothing is signalled and the byte goes
//     through, so `Ctrl-C` at a prompt still abandons the line exactly
//     as it does today -- KLINE_CANCEL, in the one line editor both
//     shells share.
//
// Called from the keyboard IRQ, so it may only set bits and flip
// scheduler state; signal_send() keeps to that for everything except
// SIGKILL, which is why the INTR key sends SIGINT.
int  tty_intr(void);

#endif
