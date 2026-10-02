// The scheduler for the kernel debugger -- see sched_debug.h. Read-only
// and lock-free on purpose: it runs with the machine stopped at an
// arbitrary instruction, possibly inside the scheduler itself.
#include "sched_debug.h"
#include "sched_internal.h"
#include "string.h"
#include "vmm.h"   // vmm_kernel_pml4_phys()

static void from_slot(int i, struct sched_debug_thread *out) {
    const struct sched_process *p = &procs[i];
    out->tid = p->pid;
    out->running = i == current_index;
    out->state = (int)p->state;
    out->stopped = p->stopped;
    k_strlcpy(out->name, p->name[0] ? p->name : "?", sizeof out->name);
    out->kctx = out->running ? 0 : &p->kctx;
    out->pml4 = p->pml4_phys;
}

static void kernel_thread(struct sched_debug_thread *out) {
    const struct kernel_context *k = scheduler_kernel_kctx();
    out->tid = SCHED_DEBUG_KERNEL_TID;
    out->running = current_index < 0;
    out->state = -1;
    out->stopped = 0;
    k_strlcpy(out->name, "kernel", sizeof out->name);
    out->kctx = out->running || !k->rip ? 0 : k;
    out->pml4 = vmm_kernel_pml4_phys();
}

static int live(int i) {
    return procs[i].state != SCHED_UNUSED && procs[i].state != SCHED_ZOMBIE;
}

int sched_debug_thread(int n, struct sched_debug_thread *out) {
    if (n == 0) { kernel_thread(out); return 1; }
    for (int i = 0; i < MAX_PROCS; i++) {
        if (!live(i)) continue;
        if (--n == 0) { from_slot(i, out); return 1; }
    }
    return 0;
}

int sched_debug_find(int tid, struct sched_debug_thread *out) {
    if (tid == SCHED_DEBUG_KERNEL_TID) { kernel_thread(out); return 1; }
    int s = pid_slot(tid);
    if (s < 0 || !live(s)) return 0;
    from_slot(s, out);
    return 1;
}
