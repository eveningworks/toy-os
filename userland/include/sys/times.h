#ifndef ULIB_SYS_TIMES_H
#define ULIB_SYS_TIMES_H

// Process CPU accounting, in clock ticks. <time.h>'s clock() answers
// the same question more simply; this exists because POSIX specifies it
// and because the shell's `times` builtin is written against it.

#include <sys/types.h>
#include <time.h>   // clock_t -- the SAME type, a different unit

// **THE UNIT IS sysconf(_SC_CLK_TCK), NOT CLOCKS_PER_SEC.** Both are
// clock_t and C and POSIX really do reuse one type for two units --
// clock() divides by CLOCKS_PER_SEC, times() by _SC_CLK_TCK. Here they
// are deliberately the SAME number, 1000000, so the trap costs nothing
// on this system; <unistd.h> says why that value and not glibc's 100.

struct tms {
    clock_t tms_utime;   // this process, user time
    clock_t tms_stime;   // this process, system time
    clock_t tms_cutime;  // reaped children, user time
    clock_t tms_cstime;  // reaped children, system time
};

// **TWO OF THE FOUR FIELDS ARE ALWAYS ZERO, AND SAYING SO IS THE POINT.**
// This kernel measures one number per process -- cpu_ns, time spent
// RUNNING (abi/proc_info.h) -- and does not split it into user and
// system, because a syscall's time is not billed separately from the
// code that made it. So tms_utime carries the whole of it and
// tms_stime is 0, rather than both carrying a guess.
//
// tms_cutime/tms_cstime are 0 because nothing accumulates a reaped
// child's CPU time into its parent; waitpid() discards it with the
// slot. A shell's `times` therefore reports its own time truthfully and
// its children's as zero -- which is visibly wrong in the right
// direction, unlike a fabricated number.
//
// Returns elapsed real time in ticks since an arbitrary point in the
// past, or (clock_t)-1 with errno set. Callers are required to use
// the DIFFERENCE of two such values, never one on its own.
clock_t times(struct tms *buf);

#endif
