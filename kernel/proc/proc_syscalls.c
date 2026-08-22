// The process syscalls: exiting, yielding, the heap, spawning, waiting,
// killing, and the two clocks a process can read.
#include "syscalls.h"
#include "heap.h"  // the environment blob, kept off syscall_dispatch()'s frame
#include "syscall.h"   // syscall_reset_heap()'s own declaration
#include "syscall_abi.h"
#include "errno.h"
#include "klog.h"
#include "kfmt.h"      // klog_printf
#include "vmm.h"
#include "pmm.h"
#include "scheduler.h"
#include "process.h"
#include "pipe.h"
#include "signal_abi.h" // PGID_NEW -- SYS_SPAWN's three-way group argument
#include "signal.h"  // signal_send(), signal_send_group() -- SYS_KILL is a signal now
#include "ksignal.h" // signal_name(), for the log line
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
        // sbrk KEEPS RETURNING -1 ON FAILURE, and that is deliberate:
        // it hands back a POINTER, and (void *)-1 is the value every
        // caller here already tests against (userland/lib/heap_os.c,
        // ugfx.c, the WM). A small negative code would be a plausible
        // -- and wrong -- address. libsys turns the sign into errno
        // without touching the value, exactly as POSIX sbrk() does.
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
        // THE BREAK IS A RESERVATION. Nothing is mapped here: the frame
        // for a heap page arrives when something first touches it, via
        // uheap_fault() below. Two consequences a caller must know.
        //
        // sbrk() can no longer report OUT OF MEMORY -- it only refuses
        // a request that runs past UADDR_HEAP_LIMIT. The machine
        // running out is discovered at the page that cannot be given,
        // which kills the process. That is OVERCOMMIT, and it is the
        // only reason a ~2 GiB heap is affordable at all: mapping what
        // it reserves would have meant handing out 2 GiB of frames to a
        // process that asked for address space. Linux makes the same
        // trade and backs it with an OOM killer; here the fault is
        // fatal to the process that took it, which is a smaller blast
        // radius than a kernel that cannot allocate.
        //
        // And the break only ever moves UP. sbrk() with a negative
        // increment is not supported (`inc` is unsigned and the check
        // above refuses anything that would run past the limit), so
        // there is no unmapping path to get wrong.
        uint64_t old_brk = hp->brk;
        hp->brk = old_brk + inc;
        c->regs[14] = old_brk; // classic sbrk() contract: returns the OLD break
    }
    return 0;
}

// Faults in one heap page, for whatever is asking -- the #PF handler,
// or vmm's copy helpers walking to a frame that is not there yet.
//
// Registered with vmm at boot (uheap_fault_init()), rather than called
// from idt.c directly, because the fault handler is NOT the only entry:
// a freshly sbrk'd buffer handed to sys_read() never faults at all --
// the kernel walks to the frame instead of dereferencing the user
// address -- so without the hook that buffer would be reported as
// unmapped and demand paging would silently break every syscall taking
// a pointer. See vmm.h.
//
// Returns 1 only if it mapped something and the access should be
// retried. Everything else is 0 and stays fatal: a wild pointer, the
// stack guard, an address past the break, or a heap page it could not
// find a frame for.
static int uheap_fault(uint64_t pml4_phys, uint64_t vaddr) {
    if (vaddr < UADDR_HEAP_BASE || vaddr >= UADDR_HEAP_LIMIT) return 0;

    // WHOSE heap this is, resolved by address space rather than by "who
    // is running": the copy helpers run inside a syscall made by the
    // owner, but saying so is an assumption, and the legacy loader's
    // single slot is armed per pml4 anyway.
    struct sched_heap *hp = scheduler_heap_for_pml4(pml4_phys);
    if (!hp && g_heap_pml4 && pml4_phys == g_heap_pml4) hp = &g_legacy_heap;
    if (!hp) return 0;

    // Past the break is NOT a heap page. This is the check that keeps
    // the reservation meaningful -- without it the whole ~2 GiB region
    // would fault in on any stray pointer, and a wild write would be
    // answered with memory instead of a fault report.
    uint64_t page = vaddr & ~0xFFFULL;
    if (page < UADDR_HEAP_BASE || page >= hp->brk) return 0;

    uint64_t frame = pmm_alloc_frame();
    if (!frame) {
        klog_printf("heap: no frame for user page %#lx -- the process dies here\n",
                    page);
        return 0;
    }
    // Zeroed, because a fresh heap page carrying somebody else's data
    // is both a surprise to the program and a disclosure between
    // processes. sbrk's eager version zeroed too; nothing changes for a
    // caller.
    for (size_t i = 0; i < 4096; i++) ((uint8_t *)(uintptr_t)frame)[i] = 0;

    if (!vmm_map_user_page(pml4_phys, page, frame)) {
        pmm_free_frame(frame);
        return 0;
    }
    return 1;
}

void uheap_fault_init(void) { vmm_set_fault_handler(uheap_fault); }

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

int sys_getpid(struct syscall_ctx *c) {
    // scheduler_current_pid() answers 0 for the kernel context and for
    // the legacy loader's unscheduled path. Reported as -1 rather than
    // passed through, because 0 is not a pid a caller can do anything
    // with and -1 is the value every other "no answer" here uses.
    int pid = scheduler_current_pid();
    c->regs[14] = (uint64_t)(int64_t)(pid > 0 ? pid : -1);
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
    // A process signalling ITSELF is legal, and for a fatal signal
    // behaves like exiting (delivery happens a few instructions later,
    // on the way back to ring 3).
    int target = (int)(int32_t)c->a0;
    int sig    = (int)(int32_t)c->a1;

    // A NEGATIVE TARGET IS A GROUP, POSIX's `kill(-pgid, sig)`. Pids are
    // 1-based, so this cannot collide with one -- which is exactly why
    // POSIX could spell it this way and why copying the spelling costs
    // nothing.
    int reached = target < 0 ? signal_send_group(-target, sig)
                              : signal_send(target, sig);
    if (reached) {
        klog_printf("syscall: kill(%s %d, SIG%s) by pid %d\n",
                    target < 0 ? "pgid" : "pid",
                    target < 0 ? -target : target,
                    signal_name(sig), scheduler_current_pid());
    }
    // 1/0, not a count and not an errno: SYS_KILL's failure value has
    // always been 0, and a negative code is TRUTHY -- flipping it would
    // silently turn every `if (!sys_kill(...))` caller inside out. See
    // abi/errno.h's note on the calls deliberately not converted.
    c->regs[14] = (uint64_t)(reached ? 1 : 0);
    return 0;
}

// Copies the environment blob out of user memory. It is NOT a C string
// -- it is a run of them ending in an empty one -- so it cannot go
// through vmm_copy_string_from_user(), which would stop at the first
// entry's terminator and hand back one variable out of ten.
//
// Copied a byte at a time through the validated-range helper rather
// than in one block, because the total length is not known until the
// double NUL is found, and validating a range this has not measured
// would be validating a guess.
//
// Returns the number of bytes written (including the terminator), or 0
// for a malformed or oversized blob -- REFUSED, never truncated.
static size_t copy_env_from_user(uint64_t pml4, uint64_t uptr, char *out, size_t cap) {
    if (!uptr) return 0;
    size_t n = 0;
    int prev_nul = 0;
    while (n < cap) {
        char ch;
        if (!vmm_copy_from_user(pml4, &ch, uptr + n, 1)) return 0;
        out[n++] = ch;
        if (ch == '\0') {
            if (prev_nul || n == 1) return n;   // the empty string that ends it
            prev_nul = 1;
        } else {
            prev_nul = 0;
        }
    }
    return 0; // ran past the cap without finding the end
}

int sys_spawn(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    int64_t spawn_rc = -ENOENT; // no such program, unless something below says otherwise
    char path[FS_PATH_MAX];
    // The argument string gets the same budget as the path: it is
    // handed to elf_build_argv_on_stack(), which enforces the real
    // limit (one stack page) and rejects anything longer.
    char argbuf[FS_PATH_MAX];

    // The message struct, copied whole before anything in it is
    // trusted -- see abi/syscall_abi.h for why spawn outgrew three
    // registers.
    struct spawn_msg msg;
    if (!vmm_copy_from_user(pml4, &msg, c->a0, sizeof msg)) {
        klog_write("syscall: spawn() rejected -- invalid message pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    // `pgid` took the `reserved` field's place -- both are 0 for every
    // caller that predates it, so the "reserved must be zero" check
    // becomes a range check on a real value. THREE-WAY: PGID_NEW (-1)
    // leads a new group, 0 inherits, positive joins. Anything below
    // PGID_NEW is refused rather than clamped -- SYS_KILL uses a negative
    // number to MEAN a group, and accepting one here would be taking a
    // value from the wrong vocabulary.
    if (msg.pgid < PGID_NEW) {
        klog_write("syscall: spawn() rejected -- pgid below PGID_NEW\n");
        c->regs[14] = (uint64_t)(int64_t)-EINVAL;
        return 0;
    }

    // The environment lives on the HEAP, not this frame: SYS_ENV_MAX is
    // 2 KiB and syscall_dispatch()'s frame is already the one this
    // kernel measures (see CLAUDE.md's note on -fstack-usage).
    char *envbuf = 0;
    const char *env = 0;
    if (msg.env) {
        envbuf = kmalloc(SYS_ENV_MAX);
        if (!envbuf) {
            c->regs[14] = (uint64_t)(int64_t)-ENOMEM;
            return 0;
        }
        if (!copy_env_from_user(pml4, (uint64_t)(uintptr_t)msg.env, envbuf, SYS_ENV_MAX)) {
            klog_write("syscall: spawn() rejected -- bad or oversized environment\n");
            kfree(envbuf);
            c->regs[14] = (uint64_t)(int64_t)-EINVAL;
            return 0;
        }
        env = envbuf;
    }

    if (!vmm_copy_string_from_user(pml4, path, (uint64_t)(uintptr_t)msg.path, FS_PATH_MAX)) {
        klog_write("syscall: spawn() rejected -- invalid path pointer\n");
        spawn_rc = -EFAULT;
    } else {
        const char *args = 0;
        if (msg.args && vmm_copy_string_from_user(pml4, argbuf, (uint64_t)(uintptr_t)msg.args, FS_PATH_MAX)) args = argbuf;

        // The child's stdout override, as a DESCRIPTION index rather
        // than a pipe index: the child's fd 1 will simply name this
        // same open file, which is the general mechanism now and not a
        // pipe special case. An fd that isn't this process's own write
        // end is REFUSED rather than quietly ignored -- spawning with
        // console output instead would leave the parent blocked on a
        // pipe nothing will ever write to.
        int stdout_desc = -1;
        int ok = 1;
        int64_t wfd = msg.stdout_fd;
        if (wfd >= 0) {
            struct open_file *f = fd_get(pml4, (int)wfd);
            if (!f || f->kind != FD_KIND_PIPE_W) {
                klog_write("syscall: spawn() rejected -- stdout fd isn't this process's pipe write end\n");
                spawn_rc = -EBADF;
                ok = 0;
            } else {
                stdout_desc = fd_desc_index(pml4, (int)wfd);
            }
        }
        if (ok) {
            // No pipe_add_writer() here any more: the child taking a
            // reference to the DESCRIPTION is what makes it a second
            // writer, and fd_set_desc() does that. Doing both counted
            // the child twice, so the pipe never reached EOF.
            // scheduler_spawn_piped() reports one failure value for
            // "no such file", "not an ELF" and "no free slot" alike, so
            // the code stays the default ENOENT rather than inventing a
            // distinction the layer below does not make. Splitting it
            // means giving that function a reason to return first.
            // 0 = inherit the caller's group, which is what every
            // spawn that predates process groups passes.
            int pid = scheduler_spawn_group(path, args, stdout_desc, env, msg.pgid);
            if (pid > 0) spawn_rc = pid;
        }
    }
    if (envbuf) kfree(envbuf);
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
    // pid -1 is "any child of mine", the same convention POSIX gives
    // wait(): it cannot collide with a real pid, which is 1-based.
    // Handled before the validity check below, which asks about a
    // specific process and has nothing to say about this.
    if (!bad && pid == -1) {
        int child = 0, code = 0;
        enum sched_poll_result r =
            scheduler_poll_any(scheduler_current_pid(), &child, &code);
        if (r == SCHED_POLL_EXITED) {
            if (out) vmm_copy_to_user(pml4, out, &code, sizeof code);
            c->regs[14] = (uint64_t)(int64_t)child;
        } else if (r == SCHED_POLL_INVALID) {
            // No children AT ALL -- a permanent answer, not "not yet",
            // so parking here would be a wait nothing could ever end.
            // ECHILD is what makes that permanence readable: an init
            // loop must not confuse it with "none have exited yet",
            // which is SYS_RETRY.
            c->regs[14] = (uint64_t)(int64_t)-ECHILD;
        } else if (c->a2 & SYS_WNOHANG) {
            c->regs[14] = (uint64_t)(int64_t)SYS_RETRY;
        } else if (!scheduler_block_current(c->regs, scheduler_wait_chan_pid(scheduler_current_pid()), SCHED_WAIT_CHILD)) {
            c->regs[14] = (uint64_t)(int64_t)-EPERM; // nowhere to park -- not a scheduled process
        } else {
            return 1; // parked -- the wake writes the return value
        }
        return 0;
    }

    if (bad) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else if (!scheduler_pid_valid(pid)) {
        c->regs[14] = (uint64_t)(int64_t)-ECHILD;
    } else {
        int code = 0;
        enum sched_poll_result r = scheduler_poll(pid, &code);
        if (r == SCHED_POLL_EXITED) {
            if (out) vmm_copy_to_user(pml4, out, &code, sizeof code);
            c->regs[14] = (uint64_t)(int64_t)pid;
        } else if (r == SCHED_POLL_INVALID) {
            c->regs[14] = (uint64_t)(int64_t)-ECHILD;
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
            if (!scheduler_block_current(c->regs, scheduler_wait_chan_pid(scheduler_current_pid()), SCHED_WAIT_CHILD)) {
                c->regs[14] = (uint64_t)(int64_t)-EPERM; // nowhere to park
            } else {
                return 1;
            }
        }
    }
    return 0;
}

// SYS_SLEEP -- park for `ms` milliseconds.
//
// The deadline is computed HERE, from the clock, rather than being
// counted down per tick: a sleeper that is not running cannot decrement
// anything, and a duration counted in ticks drifts with whatever else
// the machine is doing. Same reasoning as uapp's timers computing the
// next firing from now.
//
// A caller with no scheduler slot is REFUSED rather than given an
// instant return. Returning 0 would say "you slept", which is a lie a
// polling loop would then spin on. EPERM names the reason: the legacy
// loader has no slot to park, so `run <prog>` cannot sleep and `spawn`
// must be used instead.
int sys_sleep(struct syscall_ctx *c) {
    int64_t ms = (int64_t)c->a0;
    if (ms < 0) ms = 0;
    if (ms > SYS_SLEEP_MAX_MS) ms = SYS_SLEEP_MAX_MS;

    uint64_t deadline = clocksource_now_ns() + (uint64_t)ms * 1000000ull;
    if (!scheduler_sleep_current(c->regs, deadline)) {
        c->regs[14] = (uint64_t)(int64_t)-EPERM;
        return 0;
    }
    return 1; // parked -- scheduler_wake_timers() writes the 0 return
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
    }
}
