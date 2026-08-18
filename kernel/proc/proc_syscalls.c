// The process syscalls: exiting, yielding, the heap, spawning, waiting,
// killing, and the two clocks a process can read.
#include "syscalls.h"
#include "syscall.h"   // syscall_reset_heap()'s own declaration
#include "syscall_abi.h"
#include "klog.h"
#include "kfmt.h"      // klog_printf
#include "vmm.h"
#include "pmm.h"
#include "scheduler.h"
#include "process.h"
#include "pipe.h"
#include "fs.h"
#include "timer.h"       // pit_ticks() -- SYS_TICKS
#include "clocksource.h" // clocksource_now_ns() -- SYS_MONOTONIC_NS
#include "uaddr.h"
#include "string.h"
#include <stddef.h>

// SYS_SBRK state for the LEGACY single process syscall_reset_heap() was
// last armed for -- elf_run.c's blocking loader, which has no scheduler
// slot to keep this in (same reasoning as g_process_ctx above).
//
// A scheduler-spawned process does NOT use this: it carries its own
// `struct sched_heap`, armed when its slot is created. Both are the same
// TYPE and go through the same handler, so the two owners cannot drift
// in behaviour -- which is the whole reason the legacy side is a struct
// here rather than the loose pair of globals it used to be.
static uint64_t g_heap_pml4 = 0;
static struct sched_heap g_legacy_heap;

void syscall_reset_heap(uint64_t pml4_phys, uint64_t heap_base) {
    g_heap_pml4 = pml4_phys;
    g_legacy_heap.brk = heap_base;
    g_legacy_heap.mapped_end = heap_base;
}

SYSCALL_HANDLER sys_do_proc_info(uint64_t *regs, uint64_t rdi, uint64_t rsi) {
    uint64_t pml4 = vmm_current_pml4();
    struct proc_info info;
    if (!vmm_validate_user_range(pml4, rsi, sizeof info)) {
        klog_write("syscall: proc_info() rejected -- invalid user pointer\n");
        regs[14] = 0;
    } else if (!scheduler_proc_info((int)rdi, &info)) {
        regs[14] = 0; // bad index
    } else {
        // Filled in a KERNEL struct and copied out, never written
        // through the user pointer (vmm.h).
        vmm_copy_to_user(pml4, rsi, &info, sizeof info); // validated above
        regs[14] = 1;
    }
}

int sys_exit(struct syscall_ctx *c) {
    int code = (int)c->a0;
    klog_write("syscall: exit() called by ring-3 process\n");
    syscall_process_exit_cleanup(vmm_current_pml4());
    if (scheduler_current_pid()) {
        // Scheduler-managed process (spawned by scheduler_demo_run(),
        // see scheduler.c) -- hand its CPU slot to the next ready
        // process (or back to the shell) instead of the old
        // single-process longjmp-style return below. Returns
        // normally; isr_common's epilogue resumes whatever
        // scheduler_on_exit() picked, via g_next_kernel_rsp.
        scheduler_on_exit(code);
    } else {
        // Legacy path (process.c's process_context_exit()) --
        // doesn't return.
        process_context_exit(code);
    }
    return 0;
}

int sys_sbrk(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    uint64_t inc = c->a0;

    // Refuse unless syscall_reset_heap() armed a heap for exactly
    // this address space -- e.g. a process that never had its heap
    // set up (heap_base defaults to 0, which can't equal a real
    // CR3) or, in principle, a scheduler-managed process (this
    // whole mechanism is legacy-single-process-only, see the header
    // comment) gets a clean -1 instead of silently mapping pages
    // into the wrong address space.
    // WHOSE break this is. A scheduler-spawned process carries its
    // own, armed when its slot is created; the kernel context is the
    // legacy elf_run.c loader, whose single slot syscall_reset_heap()
    // arms. Two OWNERS because the legacy loader has no scheduler
    // slot to hold state in -- but one representation and one
    // handler, so the two cannot drift in behaviour, and everything
    // below writes through `hp` rather than to either directly.
    struct sched_heap *sh = scheduler_current_heap();
    struct sched_heap *hp = sh ? sh : &g_legacy_heap;

    // The pml4 check applies only to the legacy slot, which is armed
    // for one address space at a time and would otherwise hand a
    // later, unrelated process the previous one's break. A scheduled
    // process needs no such check: its heap is reached THROUGH the
    // scheduler's notion of who is running, so it cannot be the
    // wrong one.
    if (!sh && (g_heap_pml4 == 0 || pml4 != g_heap_pml4)) {
        klog_write("syscall: sbrk() rejected -- no heap armed for this process\n");
        c->regs[14] = (uint64_t)-1;
    } else if (inc > UADDR_HEAP_LIMIT - hp->brk) {
        // The heap grows UP toward the stack's guard region, and
        // nothing else stops it: before this check, sbrk() past the
        // ~1 MiB gap happily mapped pages straight over the live
        // stack -- no fault, no message, just a process whose locals
        // started changing under it. Refuse at the guard instead, so
        // the failure is the ordinary out-of-memory answer sbrk()
        // callers already handle.
        //
        // Written as `inc > LIMIT - brk` rather than
        // `brk + inc > LIMIT` on purpose: the sum overflows for a
        // large enough inc and the comparison then passes.
        klog_write("syscall: sbrk() rejected -- would grow into the stack guard\n");
        c->regs[14] = (uint64_t)-1;
    } else {
        uint64_t old_brk = hp->brk;
        uint64_t new_brk = old_brk + inc;
        int ok = 1;

        while (hp->mapped_end < new_brk) {
            uint64_t frame = pmm_alloc_frame();
            if (!frame) { ok = 0; break; }
            for (size_t i = 0; i < 4096; i++) ((uint8_t *)(uintptr_t)frame)[i] = 0;
            if (!vmm_map_user_page(pml4, hp->mapped_end, frame)) {
                pmm_free_frame(frame);
                ok = 0;
                break;
            }
            hp->mapped_end += 4096;
        }

        // Pages already mapped before a failure STAY mapped and
        // stay accounted for -- that is what makes the retry after a
        // partial grow start where this one stopped rather than
        // re-mapping over live pages. Only the break is left where it
        // was, so the process sees the whole request refused.
        if (!ok) {
            klog_write("syscall: sbrk() rejected -- out of physical memory\n");
            c->regs[14] = (uint64_t)-1;
        } else {
            hp->brk = new_brk;
            c->regs[14] = old_brk; // classic sbrk() contract: returns the OLD break
        }
    }
    return 0;
}

int sys_yield(struct syscall_ctx *c) {
    // Reuse the timer's exact rotation instead of inventing a second
    // reschedule path -- `c->regs` is this process's own trapframe,
    // laid out identically to what idt.c hands the timer on a real
    // interrupt. A no-op for non-scheduler-managed processes
    // (scheduler_current_pid() == 0) -- nothing to yield to under
    // the older single-process path.
    //
    // scheduler_yield(), NOT scheduler_tick(): the two differ only
    // in that this one charges no CPU time. It used to call the
    // timer's entry point, on the reasoning that a yield is
    // "indistinguishable from the timer happening to fire right
    // now" -- true for rescheduling and false for ACCOUNTING, since
    // a yield elapses microseconds rather than a whole tick.
    if (scheduler_current_pid()) {
        scheduler_yield(c->regs);
    }
    c->regs[14] = 0;
    return 0;
}

int sys_proc_info(struct syscall_ctx *c) {
    sys_do_proc_info(c->regs, c->a0, c->a1);
    return 0;
}

int sys_ticks(struct syscall_ctx *c) {
    c->regs[14] = pit_ticks();
    return 0;
}

int sys_monotonic_ns(struct syscall_ctx *c) {
    // The clocksource, not the tick counter -- see syscall_abi.h.
    // Safe with interrupts off, which is the state every syscall
    // handler runs in (the int 0x80 gate clears IF).
    c->regs[14] = clocksource_now_ns();
    return 0;
}

int sys_kill(struct syscall_ctx *c) {
    // Unprivileged on purpose -- see SYS_KILL in abi/syscall_abi.h.
    // A process killing ITSELF is legal and behaves like exiting.
    int killed = scheduler_kill((int)c->a0, (int)c->a1);
    if (killed) {
        klog_printf("syscall: kill(pid %d) by pid %d\n",
                    (int)c->a0, scheduler_current_pid());
    }
    c->regs[14] = (uint64_t)killed;
    return 0;
}

int sys_spawn(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    int spawn_rc = -1;
    char path[FS_PATH_MAX];
    // The argument string gets the same budget as the path: it is
    // handed to elf_build_argv_on_stack(), which enforces the real
    // limit (one stack page) and rejects anything longer.
    char argbuf[FS_PATH_MAX];
    if (!vmm_copy_string_from_user(pml4, path, c->a0, FS_PATH_MAX)) {
        klog_write("syscall: spawn() rejected -- invalid path pointer\n");
    } else {
        const char *args = 0;
        if (c->a1 && vmm_copy_string_from_user(pml4, argbuf, c->a1, FS_PATH_MAX)) args = argbuf;

        // Resolve the caller's write-end fd to a pipe index. An fd
        // that isn't this process's own write end is REFUSED rather
        // than quietly ignored: spawning with console output
        // instead would leave the parent blocked on a pipe nothing
        // will ever write to.
        int stdout_pipe = -1;
        int ok = 1;
        int64_t wfd = (int64_t)c->a2;
        if (wfd >= 0) {
            struct open_file *f = fd_lookup((int)wfd, pml4);
            if (!f || f->kind != FD_KIND_PIPE_W) {
                klog_write("syscall: spawn() rejected -- stdout fd isn't this process's pipe write end\n");
                ok = 0;
            } else {
                stdout_pipe = f->pipe.idx;
                // The child becomes a SECOND writer; the parent
                // keeps its own. Without this the parent closing
                // its copy would signal EOF while the child is
                // still producing output.
                pipe_add_writer(stdout_pipe);
            }
        }
        if (ok) {
            int pid = scheduler_spawn_piped(path, args, stdout_pipe);
            if (pid == 0 && stdout_pipe >= 0) pipe_close_writer(stdout_pipe); // undo
            spawn_rc = pid > 0 ? pid : -1;
        }
    }
    c->regs[14] = (uint64_t)(int64_t)spawn_rc;
    return 0;
}

int sys_waitpid(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    int pid = (int)c->a0;
    // `out` is a USER address (0 = the caller doesn't want the exit
    // code), never dereferenced -- see the copy below.
    uint64_t out = 0;
    int bad = 0;
    if (c->a1) {
        if (!vmm_validate_user_range(pml4, c->a1, sizeof(int))) {
            klog_write("syscall: waitpid() rejected -- invalid out pointer\n");
            bad = 1;
        } else {
            out = c->a1;
        }
    }
    if (bad || !scheduler_pid_valid(pid)) {
        c->regs[14] = (uint64_t)-1;
    } else {
        int code = 0;
        enum sched_poll_result r = scheduler_poll(pid, &code);
        if (r == SCHED_POLL_EXITED) {
            if (out) vmm_copy_to_user(pml4, out, &code, sizeof code);
            c->regs[14] = (uint64_t)(int64_t)pid;
        } else if (r == SCHED_POLL_INVALID) {
            c->regs[14] = (uint64_t)-1;
        } else if (c->a2 & SYS_WNOHANG) {
            // Asked, not waited. SYS_RETRY is the right answer
            // rather than a distinct "still running" code: it
            // already means "no result yet, ask again" everywhere
            // else in this ABI, and reusing it means a caller that
            // loops on SYS_RETRY works unchanged whether or not it
            // passed the flag.
            c->regs[14] = (uint64_t)(int64_t)SYS_RETRY;
        } else {
            // Still running: park. Interrupts are off for the whole
            // handler, so "still running" and "park" are atomic
            // against the exit that would wake us -- the same
            // lost-wakeup argument as SYS_WAIT_EVENT.
            if (!scheduler_block_current(c->regs, SCHED_WAIT_CHILD)) {
                c->regs[14] = (uint64_t)-1; // nowhere to park
            } else {
                return 1;
            }
        }
    }
    return 0;
}

// The legacy heap's "armed for this pml4" bookkeeping. Not part of the
// address space itself, even though the PAGES it describes are and get
// freed with everything else.
//
// Only the LEGACY slot is cleared. A scheduled process's heap lives in
// its scheduler slot and is reset when that slot is next handed out,
// which is the only moment it could matter -- clearing it here would
// need this function to know which slot died, and the slot already
// knows.
void proc_syscall_release(uint64_t pml4_phys) {
    if (g_heap_pml4 == pml4_phys) {
        g_heap_pml4 = 0;
        g_legacy_heap.brk = 0;
        g_legacy_heap.mapped_end = 0;
    }
}
