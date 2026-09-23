#ifndef ABI_PROC_INFO_H
#define ABI_PROC_INFO_H

#include <stdint.h>

// What one process looks like from outside it -- the kernel<->userland
// contract behind SYS_PROC_INFO, and therefore behind Task Manager.
//
// **Why this exists at all.** `struct sched_process` held state, page
// tables, a kernel stack and FPU state and nothing else, so the
// questions a task manager asks -- what is this, what is it costing --
// had no answer anywhere in the kernel. The three fields below are
// exactly the bookkeeping that was added to answer them, and no more.
//
// In abi/ rather than api/ because a ring-3 program reads it: Task
// Manager is a userland/gui/ app, so these bytes cross the syscall
// boundary and their layout is a contract, not an implementation
// detail. Fixed-layout and pointer-free for the same reason every TWP
// message is (see abi/win_proto.h).

// Longest program name, including the NUL. The name is the spawn path's
// LAST COMPONENT ("uidemo", not "/bin/wm/demos/uidemo") -- a task
// manager column is a few characters wide and a full path would be
// truncated to uselessness in it. The full path is not kept: nothing
// asks for it, and it would be the largest field here by far.
#define PROC_NAME_MAX 24

// Process states, as reported. Deliberately a SEPARATE enumeration from
// the scheduler's own `enum sched_state`: that one is kernel-internal
// and free to gain states (a blocked process's wait reason, say)
// without changing what userland sees.
#define PROC_STATE_UNUSED  0
#define PROC_STATE_READY   1
#define PROC_STATE_RUNNING 2
#define PROC_STATE_BLOCKED 3
#define PROC_STATE_ZOMBIE  4 // exited, not yet reaped -- still holds a slot
#define PROC_STATE_STOPPED 5 // suspended by SIGSTOP/SIGTSTP -- alive, holds
                             // everything it held, and will not be picked
                             // until SIGCONT. Reported AHEAD of whatever
                             // the process was doing when it stopped: a
                             // process suspended mid-read is stopped
                             // first and blocked second, because the
                             // block is no longer why it is not running

// What a PROC_STATE_BLOCKED process is waiting for, as a LABEL. Read
// `ps`'s state column: `block(pipe)`.
//
// A SECOND ENUMERATION FOR THE SAME REASON THE STATES ARE ONE: the
// scheduler's SCHED_WAIT_* (api/scheduler.h) are kernel-internal and
// free to gain a reason without that reaching a ring-3 binary, and this
// header's own note above anticipated exactly this field. The mapping
// lives in scheduler_proc_info(), and a KTEST asserts it is total --
// every scheduler reason must arrive here as a distinct non-zero value,
// so a sixth one added kernel-side cannot silently report as "none".
//
// MEANINGLESS IN ANY OTHER STATE, and reported as PROC_WAIT_NONE there
// rather than left at whatever the slot last waited on -- a runnable
// process that still named a channel would read as blocked on it.
#define PROC_WAIT_NONE   0
#define PROC_WAIT_EVENT  1 // a window/input event for this process
#define PROC_WAIT_PIPE   2 // data (or EOF) on a pipe it reads
#define PROC_WAIT_CHILD  3 // a spawned child of it exited
#define PROC_WAIT_TIMER  4 // a deadline it asked to sleep until
#define PROC_WAIT_KEY    5 // a keystroke on a terminal it reads
#define PROC_WAIT_TTY    6 // a terminal's output side -- a pty master
                           // waiting for its shell to print, or a shell
                           // waiting for a master that has fallen behind
#define PROC_WAIT_THREAD 7 // a thread of the same process, being joined
#define PROC_WAIT_NET    8 // a datagram on a socket it reads
#define PROC_WAIT_FUTEX  9 // a word in memory somebody will change
#define PROC_WAIT_SIGNAL 10 // sigsuspend: any signal its mask lets through
#define PROC_WAIT_LOCK   11 // a kernel lock another context holds
#define PROC_WAIT_DISK   12 // a disk transfer it started -- Linux's D state

struct proc_info {
    int32_t  pid;         // 0 means "this slot is empty"; see SYS_PROC_INFO
    uint32_t state;       // PROC_STATE_*
    uint64_t cpu_ns;      // NANOSECONDS spent RUNNING, cumulative
    uint64_t mem_bytes;   // user memory currently mapped into it
    int32_t  exit_code;   // meaningful only in PROC_STATE_ZOMBIE
    // Who spawned this process. 0 means "the kernel did" -- the shell's
    // `spawn`, `gui`, or a KTEST -- and is also what a process's
    // children are set to when it dies, so a ppid never names a slot
    // that has since been handed to somebody else.
    //
    // Took `reserved`'s place rather than growing the struct: it was
    // only ever written as 0 and never read, and this keeps the 8-byte
    // alignment that field existed for.
    int32_t  ppid;
    // This process's GROUP (abi/signal_abi.h). Never 0 for a live slot:
    // a child inherits its spawner's, and one the kernel started leads
    // its own.
    //
    // Reported because a signal's whole behaviour is "which group did it
    // reach" -- `kill -TERM -<pgid>` and Ctrl-C both act on a group, and
    // without this a person debugging either has no way to see the
    // grouping they are acting on. `ps` prints it.
    int32_t  pgid;
    char     name[PROC_NAME_MAX];
    // PROC_WAIT_*, and only while PROC_STATE_BLOCKED.
    //
    // **IT COSTS NOTHING AND MOVES NOTHING.** `name` ends four bytes
    // short of the struct's 8-byte alignment, so this fills a hole that
    // was already there: every existing field keeps its offset and
    // sizeof(struct proc_info) is unchanged, which is the same
    // append-without-disturbing rule `ppid` followed when it took
    // `reserved`'s place. A _Static_assert below holds that true.
    uint32_t wait_reason;
    // Has this process announced that it finished starting up
    // (SYS_NOTIFY_READY)? 0 for every process that never calls it,
    // which is almost all of them -- only a service init supervises has
    // any reason to.
    //
    // **THE FIRST FIELD THAT GREW THE STRUCT.** `ppid` took `reserved`'s
    // place and `wait_reason` filled the hole after `name`; there is no
    // hole left, so this adds 8 bytes (4 for the field, 4 of tail
    // padding the next one can have). That is the append rule working
    // as intended rather than a break of it: every existing offset is
    // unchanged, so a ring-3 binary reading the older layout still
    // finds every field it knows where it left it.
    //
    // A BIT, NOT A TIMESTAMP. "When did it become ready" has no reader
    // -- init logs the interval itself, from the clock it already
    // sampled at the spawn -- and this project's bar for a field is a
    // second real caller.
    uint32_t ready;
    // The THREAD GROUP this slot belongs to: the pid of its leader,
    // which for an ordinary process is its own pid. `tgid != pid` is
    // what makes a slot a thread rather than a process, and it is the
    // only way a reader can tell -- everything else about a thread
    // (name, ppid, memory) is deliberately its leader's.
    //
    // Free: it lands in the tail padding `ready` left behind, so every
    // offset and the size are unchanged.
    int32_t  tgid;
};

// `wait_reason` went into the hole after `name`; `ready` then grew the
// struct, which is fine -- but a field must GROW it rather than
// reshuffle, because every offset here is a contract with a ring-3
// binary. The assert is the thing that notices, so update it
// deliberately when a field is added and never to make a build pass.
_Static_assert(sizeof(struct proc_info) == 72,
               "struct proc_info must grow append-only -- existing fields never move");
_Static_assert(__builtin_offsetof(struct proc_info, name) == 36,
               "struct proc_info's name must stay where ring-3 binaries expect it");

// **cpu_ns is cumulative, and that is deliberate.** A percentage is the
// difference between two reads divided by the time elapsed between
// them, which only the consumer can compute -- it is the one that knows
// how often it refreshes. Reporting a percentage here would bake a
// sampling interval into the kernel, and a task manager that refreshes
// at a different rate would then be reading a number that means
// something else. The same reasoning Linux applies to /proc/stat, which
// also reports totals and leaves the arithmetic to `top`.
//
// Pair it with SYS_MONOTONIC_NS -- the SAME clock, which is what makes
// the ratio a real percentage rather than one counter over an unrelated
// other one.
//
// **NANOSECONDS, not ticks, and the rename is the point.** This was
// `cpu_ticks`, incremented once per 100Hz timer interrupt, and a tick
// count is not a duration: a yield billed a whole tick for microseconds
// of work (every polling app read a fake 100%, several at once), and
// charging only from the timer replaced that with the opposite error
// (anything finishing inside a tick read 0%). The scheduler measures
// elapsed time against a clocksource now -- exact to the nanosecond
// wherever the TSC is usable, no worse than before where it is not.
// See docs/decisions.md.

#endif
