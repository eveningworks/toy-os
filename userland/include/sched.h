#ifndef ULIB_SCHED_H
#define ULIB_SCHED_H

// The scheduling class, POSIX's <sched.h>. SCHED_OTHER is the fair
// class every process starts in, where nice (<sys/resource.h>) is a
// weight; SCHED_FIFO and SCHED_RR at 1..99 run before ALL of it, for a
// process that blocks promptly -- abi/syscall_abi.h has the guards.
//
// ONLY INIT MAY ASK FOR RT (a service's `CPUSchedulingPolicy=`); anyone
// may drop itself to SCHED_OTHER, and anything else is EPERM. What an
// RT process creates starts SCHED_OTHER: SCHED_RESET_ON_FORK, always.

#include <sys/types.h>
#include <syscall_abi.h>   // SCHED_OTHER, SCHED_FIFO, SCHED_RR

struct sched_param {
    int sched_priority;
};

// 0, or -1 with errno (EINVAL, ESRCH, EPERM). `pid` 0 is the caller.
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param);
// The policy, or -1 with errno.
int sched_getscheduler(pid_t pid);
int sched_getparam(pid_t pid, struct sched_param *param);
int sched_get_priority_min(int policy);
int sched_get_priority_max(int policy);
// To the back of the caller's queue: its priority if RT, its share if not.
int sched_yield(void);

#endif
