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
    char     name[PROC_NAME_MAX];
};

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
