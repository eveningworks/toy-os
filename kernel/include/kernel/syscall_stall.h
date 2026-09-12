#ifndef SYSCALL_STALL_H
#define SYSCALL_STALL_H

#include <stdint.h>
#include "query_abi.h" // QUERY_SYSCALL_STALL_BUCKETS -- one bucket layout, not two

// How long each syscall held the CPU.
//
// A syscall handler runs with interrupts off, so its duration is not
// "how long the call took" from the caller's point of view -- it is how
// long NOTHING ELSE ON THE MACHINE RAN. That is the number the desktop
// feels as a late frame, and the one the interruptible-syscall work in
// docs/roadmap.md has to move.
//
// OFF BY DEFAULT, armed by kernel.syscall_stall. Armed it costs two
// rdtsc reads per syscall; disarmed, one global compare -- the bar
// strace_active() already sets in the same function. It times with the
// TSC rather than the clocksource for a reason the .c file states in
// full: the PIT source cannot advance while interrupts are off, which
// is the entire window being measured.

// One slot per syscall number. Sized ABOVE the table rather than equal
// to it, and kept true by a _Static_assert against SYSCALL_TABLE_COUNT
// in kernel/proc/syscall_table.c -- the sibling table here was 64 while
// the syscall table reached 107, so `kstack syscalls` had silently
// reported nothing about the top third of the ABI.
#define SYSCALL_STALL_MAX 128

// The timestamp to hand syscall_stall_end(), or 0 when disarmed.
uint64_t syscall_stall_begin(void);

// `t0` is what begin() returned; 0 means "was not armed", and is ignored.
void syscall_stall_end(int nr, uint64_t t0);

// Arming from OFF also zeroes the counters. Returns 0 REFUSED -- a
// machine with no calibrated TSC frequency cannot convert ticks to
// microseconds, and reporting unconverted ticks as microseconds would be
// worse than reporting nothing.
int  syscall_stall_set(int on);
int  syscall_stall_get(void);
uint32_t syscall_stall_mhz(void); // TSC MHz in use, 0 while never armed

// One syscall's record, for the query provider. Returns 0 for a syscall
// nothing has measured, which is what makes the list "what has actually
// run" rather than a table of zeroes.
struct syscall_stall_info {
    uint64_t n;
    uint64_t sum_us;
    uint64_t max_us;
    uint32_t bucket[QUERY_SYSCALL_STALL_BUCKETS];
};
int syscall_stall_info(int nr, struct syscall_stall_info *out);

// The bucketing, exposed for the KTESTs -- a histogram whose edges are
// only ever checked through the thing that fills it cannot be tested
// without making real syscalls take real milliseconds.
void syscall_stall_record_us(int nr, uint64_t us);
void syscall_stall_reset(void);

#endif
