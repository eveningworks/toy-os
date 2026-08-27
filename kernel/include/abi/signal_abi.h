#ifndef SIGNAL_ABI_H
#define SIGNAL_ABI_H

#include <stdint.h>

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

#define SIGHUP   1  // "your configuration changed" -- the signal a daemon
                    // re-reads on, and what `service` rings init's
                    // doorbell with. Default action terminates, as on
                    // Unix, so a program that has not asked for it is
                    // not quietly immune to a kill
#define SIGINT   2  // what Ctrl-C sends to the foreground group
#define SIGQUIT  3  // the second escape hatch, for when SIGINT is ignored
#define SIGILL   4  // an illegal instruction (#UD)
#define SIGFPE   8  // a division error (#DE). POSIX's name is a
                    // misnomer everywhere -- there is no FPU trap here
#define SIGKILL  9  // uncatchable, unignorable -- what a force-quit sends
#define SIGSEGV 11  // a memory fault (#PF or #GP), given a name
#define SIGTERM 15  // ask politely; `kill` with no signal named
#define SIGCHLD 17  // a child exited. Default action: IGNORE
#define SIGCONT 18  // resume a stopped process. Default action: CONTINUE
#define SIGSTOP 19  // suspend, uncatchable and unignorable
#define SIGTSTP 20  // what Ctrl-Z sends to the foreground group
#define SIGTTIN 21  // a BACKGROUND process tried to read the terminal.
                    // Default action: stop, so it waits its turn
                    // instead of stealing the keyboard from the shell

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
// THREE NOW: the two sentinels below, or ANY OTHER VALUE, which is a
// ring-3 function pointer the kernel will call. 0 and 1 can never be
// real code here (the ring-3 map starts far above them -- see
// kernel/uaddr.h), so a sentinel and an address cannot be confused.
#define SIG_DFL 0 // the default action -- terminate, except SIGCHLD
#define SIG_IGN 1 // discard it on arrival

// 1 if `h` is a user-space function rather than one of the sentinels.
//
// COMPARED AGAINST THE LITERAL 1, not against SIG_IGN, and that is not
// a style choice: <signal.h> redefines SIG_DFL/SIG_IGN as POSIX
// requires -- function POINTERS rather than the plain 0 and 1 here --
// and a pointer on the right of this comparison is a constraint
// violation. 1 is SIG_IGN's value by the definition directly above, so
// the two cannot drift apart without that line changing too.
#define SIG_IS_HANDLER(h) ((uint64_t)(uintptr_t)(h) > 1)

// --- struct k_sigaction, and the RESTORER ------------------------------
//
// POSIX's shape, and POSIX's argument order in SYS_SIGACTION: a signal,
// the new action or NULL, and somewhere to write the old one or NULL.
// The alternative -- (signal, disposition) returning the previous one --
// is what this call was while there were only two dispositions, and it
// runs out of argument registers the moment a handler needs a restorer
// and flags beside it.
//
// **THE RESTORER IS THE ADDRESS THE HANDLER RETURNS TO, and ring 3
// supplies it.** The kernel pushes it as the handler's return address,
// so when the handler does an ordinary `ret` it lands on three
// instructions that call SYS_SIGRETURN, and the kernel unwinds the frame
// from there.
//
// This is x86-64 Linux's SA_RESTORER exactly: the kernel refuses a
// sigaction without one and glibc supplies `__restore_rt`. i386 Linux
// puts the same three instructions in the vDSO instead, which is the
// design not taken here -- it would mean a kernel-owned executable page
// mapped into every address space to serve a runtime that every program
// in this tree already links (userland/rt/sigtramp.asm). What the vDSO
// buys, and what toy-os gives up by choosing the other side: the kernel
// never has to trust a userland pointer for control flow. Mitigated
// rather than ignored -- the restorer is range-checked like any other
// user pointer, and a handler that returns to a bad one faults in ring 3
// where a fault belongs.
struct k_sigaction {
    uint64_t handler;  // SIG_DFL, SIG_IGN, or a `void (*)(int)`
    uint64_t restorer; // where the handler returns to. Required with a
                       // handler, ignored (and reported back as 0) for
                       // the two sentinels
    uint32_t flags;    // SA_* below
    uint32_t _pad;     // explicit, so the struct's layout is the same
                       // in both rings with no packing to argue about
};

// A SIGNAL THAT INTERRUPTS A BLOCKING SYSCALL RESTARTS IT rather than
// making it fail with -EINTR.
//
// Linux sets this for most signals through `signal()` and BSD made it
// the default, because the alternative is every caller of read() growing
// an EINTR loop. Without it the interrupted call returns -EINTR, which
// is the POSIX-without-SA_RESTART behaviour and is what abi/errno.h has
// always promised.
//
// **WHAT IT CANNOT DO: restart a call that already finished.** The
// restart rewinds RIP over the `int $0x80` and re-runs the syscall, so
// it is only ever applied where the signal was taken BEFORE the call ran
// -- which is exactly the case that matters, because a process parked in
// a blocking syscall is rewound to its own `int` when the signal wakes
// it (api/scheduler.h). A signal that becomes pending while a syscall is
// mid-flight is delivered on the way out, with the call's real result
// intact.
#define SA_RESTART 1

// --- the signal frame -------------------------------------------------
//
// What the kernel pushes onto the user stack before entering a handler,
// and reads back on SYS_SIGRETURN. Shared so that it is DOCUMENTED
// rather than reverse-engineered; nothing in ring 3 has to build one.
//
// Laid out so that `restorer_ret` is at the LOWEST address, because that
// is where a SysV `call` would have left a return address -- the handler
// is entered as an ordinary function and returns as one.
//
// The whole trapframe is saved, not a curated subset, because the one
// thing this has to get exactly right is putting the process back the
// way it was. What is NOT restored verbatim is CS, SS and the unsafe
// bits of RFLAGS: those come from the kernel on the way out, so a
// program that scribbles its own frame gets a fault in ring 3 rather
// than a ring transition.
#define SIGFRAME_TF_SLOTS 22

// "SIGFRAM1" -- a frame that does not carry this was not built by the
// kernel, and SYS_SIGRETURN kills rather than guesses.
#define SIGFRAME_MAGIC 0x5349474652414d31ULL

struct sigframe {
    uint64_t restorer_ret;                 // the handler's return address
    uint64_t regs[SIGFRAME_TF_SLOTS];      // the interrupted trapframe
    uint32_t blocked;                      // the mask to put back
    int32_t  sig;                          // which signal this frame is for
    uint64_t magic;                        // SIGFRAME_MAGIC
};

// THE RED ZONE IS REAL AND MUST BE SKIPPED. SysV reserves the 128 bytes
// below RSP for a leaf function to use without adjusting the stack, so a
// frame built AT RSP overwrites live data in any function that took
// advantage of it. Linux skips the same 128 bytes for the same reason.
#define SIGFRAME_RED_ZONE 128

// SIGKILL AND SIGQUIT CANNOT BE IGNORED **OR CAUGHT**, so there is
// always something that works. SIGKILL is Unix's rule and the reason a
// force-quit is trustworthy; SIGQUIT is this kernel's own second escape
// hatch. POSIX makes SIGQUIT catchable -- a deliberate difference,
// written down rather than assumed.
//
// **THE ARGUMENT FOR SIGQUIT GOT WEAKER WHEN HANDLERS LANDED, and it is
// worth saying so rather than leaving the old reasoning standing.** It
// was unignorable because with no handlers a shell that ignores SIGINT
// (as every shell must) left a wedged program with nothing pointed at
// it but SIGKILL, which gives the program no chance to clean up. A
// catchable SIGQUIT is now a coherent thing to want. It stays
// uncatchable because the cheap escape hatch is worth more here than
// POSIX fidelity: this kernel has no `kill -9` habit built up in its
// users, and one signal that provably reaches every process without
// running its code is what makes a hung program recoverable from a
// shell. Revisit if a program ever has a real reason to catch it.
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
#define SIGNAL_STOPS(s) \
    ((s) == SIGSTOP || (s) == SIGTSTP || (s) == SIGTTIN)

// THERE IS NO SIGTTOU, and that is a decision rather than an omission.
// It exists on Unix to stop a background process WRITING to the
// terminal -- but only when `TOSTOP` is set, which it is not by default
// on Linux or anywhere else, so in practice a background job's output
// interleaves with the shell's and everyone has learned to live with
// it. toy-os has no TOSTOP and no reason to grow one, which would leave
// a signal number with no sender. The read side is different and is why
// SIGTTIN is here: two readers of one keyboard is not untidy output, it
// is keystrokes going to the wrong process.

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
