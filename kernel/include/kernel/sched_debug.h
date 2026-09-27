#ifndef SCHED_DEBUG_H
#define SCHED_DEBUG_H

// The scheduler, as the kernel DEBUGGER sees it: read with the machine
// stopped, so no lock is taken and nothing is changed. kernel/debug/
// shows each process as a GDB thread (thread id = pid) and the kernel
// context as one more, SCHED_DEBUG_KERNEL_TID.

#include <stdint.h>
#include "context_switch.h"
#include "proc_info.h"   // PROC_NAME_MAX

#define SCHED_DEBUG_KERNEL_TID 1000   // above every pid, and not 0 (GDB's "any")

struct sched_debug_thread {
    int tid;
    int running;                         // this one was on the CPU when it stopped
    int state;                           // enum sched_state, or -1 for the kernel context
    int stopped;                         // job-control stopped
    char name[PROC_NAME_MAX];
    const struct kernel_context *kctx;   // where it is parked; NULL while running,
                                         // or before the kernel context was ever left
    uint64_t pml4;                       // its address space's page-table root
};

// The Nth live thread (N from 0), or 0 past the end. Zombies are skipped:
// their saved context describes nothing that will run again.
int sched_debug_thread(int n, struct sched_debug_thread *out);
// The same, by thread id.
int sched_debug_find(int tid, struct sched_debug_thread *out);

// scheduler.c's own: the kernel context's parking place.
const struct kernel_context *scheduler_kernel_kctx(void);

#endif
