// The process syscalls: exiting, yielding, the heap, spawning, waiting,
// killing, and the two clocks a process can read.
#include "syscalls.h"
#include "heap.h"  // the environment blob, kept off syscall_dispatch()'s frame
#include "syscall.h"   // syscall_reset_mm()'s own declaration
#include "syscall_abi.h"
#include "errno.h"
#include "klog.h"
#include "kfmt.h"      // klog_printf
#include "mmap.h"      // mmap_fault_in -- the arena's slice of uheap_fault()
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
#include "strace.h"
#include "elf_run.h"  // elf_argv_from_string -- the string form is split here
#include "tty.h"      // tty_set_fg_pgid -- SPAWN_FOREGROUND     // strace_arm_for_current() -- SYS_SPAWN's SPAWN_TRACE
#include <stddef.h>

// The demand-paged memory of the LEGACY single process
// syscall_reset_mm() was last armed for -- elf_run.c's blocking loader,
// which has no scheduler slot to keep this in (same reasoning as
// g_process_ctx above).
//
// A scheduler-spawned process does NOT use this: it carries its own
// `struct sched_mm`, armed when its slot is created. Both are the same
// TYPE and go through the same handler, so the two owners cannot drift
// in behaviour -- which is the whole reason the legacy side is a struct
// here rather than the loose pair of globals it used to be.
static uint64_t g_heap_pml4 = 0;
static struct sched_mm g_legacy_mm;

void syscall_reset_mm(uint64_t pml4_phys, uint64_t image_end) {
    uint64_t heap_base = image_end > UADDR_HEAP_MIN_BASE
                              ? image_end : UADDR_HEAP_MIN_BASE;
    g_heap_pml4 = pml4_phys;
    g_legacy_mm.heap_base    = heap_base;
    g_legacy_mm.brk          = heap_base;
    g_legacy_mm.stack_bottom = UADDR_STACK_INIT_BOTTOM;
}

SYSCALL_HANDLER sys_do_proc_info(uint64_t *regs, uint64_t rdi, uint64_t rsi) {
    uint64_t pml4 = vmm_current_pml4();
    struct proc_info info;
    if (!scheduler_proc_info((int)rdi, &info)) {
        // -EINVAL for a bad index -- which is also every enumerator's
        // TERMINATOR, so a caller loops on `== 0` now and still skips
        // empty slots (pid 0) itself: an empty slot is a SUCCESS.
        regs[14] = (uint64_t)(int64_t)-EINVAL;
    } else if (!vmm_copy_to_user(pml4, rsi, &info, sizeof info)) {
        klog_write("syscall: proc_info() rejected -- invalid user pointer\n");
        regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else {
        // Filled in a KERNEL struct and copied out, never written
        // through the user pointer (vmm.h).
        regs[14] = 0;
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

// --- threads ---------------------------------------------------------
//
// The kernel half is deliberately four small calls: create a thread on
// a stack ring 3 supplies, end one, collect one, and forget one. The
// library on top of them -- pthread_t, return values, destructors,
// stack allocation -- is userland/libc/pthread.c, because none of it
// needs a privilege the kernel has and all of it needs a malloc.

int sys_thread_create(struct syscall_ctx *c) {
    struct thread_create_msg msg;
    if (!vmm_copy_from_user(c->pml4, &msg, c->a0, sizeof msg)) {
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    if (msg.reserved != 0) { // an unset field is the only legal value
        c->regs[14] = (uint64_t)(int64_t)-EINVAL;
        return 0;
    }
    int64_t rc = scheduler_thread_create(msg.entry, msg.stack_top, msg.arg,
                                          msg.tls, msg.detached);
    c->regs[14] = (uint64_t)rc;
    return 0;
}

int sys_thread_exit(struct syscall_ctx *c) {
    int code = (int)c->a0;
    // NO syscall_process_exit_cleanup() HERE, and that is the whole
    // difference from sys_exit(): the address space, the fd table and
    // the window state belong to the PROCESS, and its other threads are
    // still using every one of them. A thread that tore them down would
    // leave its siblings running with no memory.
    //
    // Called from the group leader this exits the process instead, and
    // that path does need the cleanup -- so it goes through sys_exit()'s
    // code rather than around it.
    if (scheduler_current_pid() && scheduler_current_tgid() != scheduler_current_pid()) {
        scheduler_on_thread_exit(code);
        return 0;
    }
    return sys_exit(c);
}

int sys_thread_join(struct syscall_ctx *c) {
    int tid = (int)c->a0;
    int code = 0;
    enum sched_poll_result r = scheduler_thread_poll(tid, &code);
    if (r == SCHED_POLL_EXITED) {
        c->regs[14] = (uint64_t)(int64_t)code;
        return 0;
    }
    if (r == SCHED_POLL_INVALID) {
        c->regs[14] = (uint64_t)(int64_t)-ESRCH;
        return 0;
    }
    // Still running: park on the thread being joined. Interrupts are
    // off for the whole handler, so "still running" and "park" are
    // atomic against the exit that would wake us -- the same
    // lost-wakeup argument SYS_WAITPID makes.
    if (!scheduler_block_current(c->regs, scheduler_wait_chan_pid(tid),
                                  SCHED_WAIT_THREAD)) {
        c->regs[14] = (uint64_t)(int64_t)-EPERM; // nowhere to park
        return 0;
    }
    return 1; // parked -- the wake writes SYS_RETRY and ring 3 asks again
}

int sys_thread_detach(struct syscall_ctx *c) {
    c->regs[14] = (uint64_t)(int64_t)scheduler_thread_detach((int)c->a0);
    return 0;
}

int sys_gettid(struct syscall_ctx *c) {
    int tid = scheduler_current_pid();
    c->regs[14] = (uint64_t)(int64_t)(tid > 0 ? tid : -1);
    return 0;
}

int sys_set_tls(struct syscall_ctx *c) {
    c->regs[14] = (uint64_t)(int64_t)scheduler_set_tls(c->a0);
    return 0;
}

int sys_sbrk(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    uint64_t inc = c->a0;

    // Refuse unless syscall_reset_mm() armed a heap for exactly
    // this address space -- e.g. a process that never had its heap
    // set up (heap_base defaults to 0, which can't equal a real
    // CR3) or, in principle, a scheduler-managed process (this
    // whole mechanism is legacy-single-process-only, see the header
    // comment) gets a clean -1 instead of silently mapping pages
    // into the wrong address space.
    // WHOSE break this is. A scheduler-spawned process carries its
    // own, armed when its slot is created; the kernel context is the
    // legacy elf_run.c loader, whose single slot syscall_reset_mm()
    // arms. Two OWNERS because the legacy loader has no scheduler
    // slot to hold state in -- but one representation and one
    // handler, so the two cannot drift in behaviour, and everything
    // below writes through `hp` rather than to either directly.
    struct sched_mm *sh = scheduler_current_mm();
    struct sched_mm *hp = sh ? sh : &g_legacy_mm;

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

// Faults in one page of a process's demand-paged memory, for whatever
// is asking -- the #PF handler, or vmm's copy helpers walking to a frame
// that is not there yet.
//
// Registered with vmm at boot (uheap_fault_init()), rather than called
// from idt.c directly, because the fault handler is NOT the only entry:
// a freshly sbrk'd buffer handed to sys_read() never faults at all --
// the kernel walks to the frame instead of dereferencing the user
// address -- so without the hook that buffer would be reported as
// unmapped and demand paging would silently break every syscall taking
// a pointer. See vmm.h.
//
// **THE STACK GOES THROUGH THE SAME HOOK, AND IT HAS TO.** A syscall
// whose output lands in a not-yet-grown stack page reaches it through
// the copy helpers, exactly as an sbrk'd buffer does -- so a growth path
// wired only into the #PF handler would work for ordinary code and fail
// for `read(fd, buf, n)` with `buf` a deep local. That is the same bug
// the heap had before this hook existed, and it would have come back
// with the opposite symptom.
//
// Returns 1 only if it mapped something and the access should be
// retried. Everything else is 0 and stays fatal: a wild pointer, the
// stack guard, an address past the break, a growth leap too deep to be
// a stack frame, or a page it could not find a frame for.

// Zeroed, because a fresh page carrying somebody else's data is both a
// surprise to the program and a disclosure between processes. sbrk's
// eager version zeroed too; nothing changes for a caller.
static int map_zeroed_user_page(uint64_t pml4_phys, uint64_t page) {
    uint64_t frame = pmm_alloc_frame(PMM_ZONE_ANY);
    if (!frame) {
        klog_printf("mm: no frame for user page %#lx -- the process dies here\n",
                    page);
        return 0;
    }
    for (size_t i = 0; i < 4096; i++) ((uint8_t *)(uintptr_t)frame)[i] = 0;

    if (!vmm_map_user_page(pml4_phys, page, frame)) {
        pmm_free_frame(frame);
        return 0;
    }
    return 1;
}

// Grows the stack down to cover `page`, which the caller has already
// established lies inside the reservation and below the current bottom.
//
// EVERY PAGE FROM THE OLD BOTTOM DOWN TO THE FAULTING ONE IS MAPPED, not
// just the faulting one, and `stack_bottom` is what says so: the field
// means "every page from here up is mapped", and leaving a hole under a
// lowered bottom would make that a lie in the direction that never
// faults -- the next access into the hole would be answered by this same
// function deciding there was nothing to do.
static int grow_stack(struct sched_mm *mm, uint64_t pml4_phys, uint64_t page) {
    while (mm->stack_bottom > page) {
        uint64_t next = mm->stack_bottom - 4096;
        if (!map_zeroed_user_page(pml4_phys, next)) return 0;
        // Committed one page at a time, so a failure part way down
        // leaves a SHORTER stack that is still entirely mapped rather
        // than a bottom pointing at a page that was never allocated.
        mm->stack_bottom = next;
    }
    return 1;
}

static int uheap_fault(uint64_t pml4_phys, uint64_t vaddr) {
    // WHOSE memory this is, resolved by address space rather than by
    // "who is running": the copy helpers run inside a syscall made by
    // the owner, but saying so is an assumption, and the legacy loader's
    // single slot is armed per pml4 anyway.
    struct sched_mm *hp = scheduler_mm_for_pml4(pml4_phys);
    if (!hp && g_heap_pml4 && pml4_phys == g_heap_pml4) hp = &g_legacy_mm;
    if (!hp) return 0;

    uint64_t page = vaddr & ~0xFFFULL;

    // --- the stack, growing DOWN ---------------------------------------
    if (uaddr_is_stack_range(vaddr)) {
        // Already mapped, or above the bottom: not ours to answer. The
        // access faulted for some other reason (a write to a read-only
        // page, say), and reporting it is the classifier's job.
        if (page >= hp->stack_bottom) return 0;

        // HOW FAR BELOW THE BOTTOM DECIDES WHETHER THIS IS A STACK AT
        // ALL. Within the gap it is a function opening a frame; deeper
        // than that it is a wild pointer that happens to land in the
        // reservation, or a frame so large it leapt the growable region,
        // and both are worth a fault report rather than memory. See
        // UADDR_STACK_GROW_GAP.
        if (hp->stack_bottom - page > UADDR_STACK_GROW_GAP) {
            // LOGGED, because the fault report cannot say this. idt.c
            // names a fault in the GUARD as a stack overflow, and this
            // address is not in the guard -- it is inside the
            // reservation, which from the outside looks like an
            // ordinary wild pointer. Without this line the two most
            // interesting failures here (a frame that leapt the gap,
            // and a pointer aimed into unmapped stack) are both a bare
            // "Page fault" with nothing to distinguish them.
            klog_printf("mm: refused to grow the stack to %#lx -- %lu KiB "
                        "below the bottom (%#lx), further than one frame\n",
                        page, (unsigned long)((hp->stack_bottom - page) / 1024),
                        hp->stack_bottom);
            return 0;
        }

        if (!grow_stack(hp, pml4_phys, page)) return 0;
        return 1;
    }

    // --- the mmap arena ------------------------------------------------
    if (uaddr_is_mmap_range(vaddr))
        return mmap_fault_in(hp, pml4_phys, vaddr);

    // --- the heap, growing UP ------------------------------------------
    //
    // Past the break is NOT a heap page. This is the check that keeps
    // the reservation meaningful -- without it the whole ~2 GiB region
    // would fault in on any stray pointer, and a wild write would be
    // answered with memory instead of a fault report.
    if (page < hp->heap_base || page >= hp->brk) return 0;

    return map_zeroed_user_page(pml4_phys, page);
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

int sys_fork(struct syscall_ctx *c) {
    int r = scheduler_fork(c->regs);
    c->regs[14] = (uint64_t)(int64_t)r;
    return 0;
}

int sys_getpid(struct syscall_ctx *c) {
    // THE PROCESS, not the thread: every thread of one program answers
    // the same pid, which is what getpid() means and what SYS_GETTID is
    // for when a caller wants them told apart.
    //
    // scheduler_current_tgid() answers 0 for the kernel context and for
    // the legacy loader's unscheduled path. Reported as -1 rather than
    // passed through, because 0 is not a pid a caller can do anything
    // with and -1 is the value every other "no answer" here uses.
    int pid = scheduler_current_tgid();
    c->regs[14] = (uint64_t)(int64_t)(pid > 0 ? pid : -1);
    return 0;
}

int sys_notify_ready(struct syscall_ctx *c) {
    // No arguments to validate and nothing to copy: the caller's
    // identity IS the message, which is the whole reason this is a
    // syscall rather than a byte on a channel somebody could lie on.
    int64_t rc = scheduler_mark_current_ready() ? 0 : -ESRCH;
    c->regs[14] = (uint64_t)rc;
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
    // 0 success / -ESRCH: converted with every caller in one commit
    // (docs/errno-design.md's leftover bucket). ESRCH covers both an
    // absent pid and an empty group, which is POSIX's answer too.
    c->regs[14] = reached ? 0 : (uint64_t)(int64_t)-ESRCH;
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

// One of the child's standard streams, resolved to a description index
// the spawn can install. `want` is the pipe end that direction accepts;
// a connected SOCKET is accepted for either, which is what lets a
// handler spawned per connection be an ordinary filter. Returns -1 for
// anything else, including an fd this process does not hold.
static int spawn_std_desc(uint64_t pml4, int fd, enum fd_kind want) {
    // The sentinel is not an fd and is not looked up: it asks for a
    // FRESH log description, which the child then owns. Only for the
    // child's stdout -- there is nothing to read back from a log, so
    // accepting it for stdin would be a descriptor that answers every
    // read with failure.
    if (fd == SPAWN_FD_LOG && want == FD_KIND_PIPE_W)
        return fd_desc_alloc(FD_KIND_LOG, -1);
    struct open_file *f = fd_get(pml4, fd);
    if (!f) return -1;
    if (f->kind != want && f->kind != FD_KIND_SOCKET) return -1;
    return fd_desc_index(pml4, fd);
}

// The three things a program is started with -- path, argument vector,
// environment -- copied out of the caller's address space into memory
// the kernel owns, for a spawn and for an exec alike (an exec's copies
// outlive the address space they came from). `envbuf`/`argbuf` are
// kmalloc'd, on the heap for the reason SYS_ENV_MAX and SPAWN_ARGS_MAX
// give: neither belongs on a 16 KiB kernel stack, and a STATIC buffer
// held across an ELF load would be overwritten by a concurrent spawn.
// 0 or -errno; on an error nothing is held.
struct spawn_args {
    char        path[FS_PATH_MAX];
    char       *envbuf;
    const char *env;
    char       *argbuf;
    const char *args;
    size_t      args_len;
};

static void spawn_args_free(struct spawn_args *a) {
    if (a->envbuf) kfree(a->envbuf);
    if (a->argbuf) kfree(a->argbuf);
    a->envbuf = a->argbuf = 0;
}

static int spawn_args_collect(uint64_t pml4, const struct spawn_msg *msg,
                              struct spawn_args *a, const char *who) {
    k_memset(a, 0, sizeof *a);
    if (msg->env) {
        a->envbuf = kmalloc(SYS_ENV_MAX);
        if (!a->envbuf) return -ENOMEM;
        if (!copy_env_from_user(pml4, (uint64_t)(uintptr_t)msg->env, a->envbuf, SYS_ENV_MAX)) {
            klog_printf("syscall: %s() rejected -- bad or oversized environment\n", who);
            spawn_args_free(a);
            return -EINVAL;
        }
        a->env = a->envbuf;
    }
    if (!vmm_copy_string_from_user(pml4, a->path, (uint64_t)(uintptr_t)msg->path, FS_PATH_MAX)) {
        klog_printf("syscall: %s() rejected -- invalid path pointer\n", who);
        spawn_args_free(a);
        return -EFAULT;
    }
    // `args` becomes the VECTOR the loader carries. With SPAWN_ARGV it
    // arrives as one (copied like the environment, and refused past the
    // cap or without its final NUL); otherwise the string form is split
    // here -- the ring-3 edge is the one place that happens. NOTE the
    // string form TRUNCATES rather than rejects an over-long argument
    // (vmm_copy_string_from_user() terminates at max-1).
    // `args_len` is READ ONLY WITH THE FLAG: a binary built before the
    // field existed passes a shorter struct.
    int bad = 0;
    if ((msg->flags & SPAWN_ARGV) &&
        (!msg->args || msg->args_len == 0 || msg->args_len > SPAWN_ARGS_MAX)) {
        bad = 1;
    } else if (msg->args) {
        a->argbuf = kmalloc(SPAWN_ARGS_MAX + FS_PATH_MAX);
        if (!a->argbuf) {
            bad = 1;
        } else if (msg->flags & SPAWN_ARGV) {
            if (vmm_copy_from_user(pml4, a->argbuf, (uint64_t)(uintptr_t)msg->args, msg->args_len) &&
                a->argbuf[msg->args_len - 1] == '\0') {
                a->args = a->argbuf;
                a->args_len = msg->args_len;
            } else {
                bad = 1;
            }
        } else {
            char *str = kmalloc(SPAWN_ARGS_MAX);
            if (str && vmm_copy_string_from_user(pml4, str, (uint64_t)(uintptr_t)msg->args,
                                                 SPAWN_ARGS_MAX) &&
                elf_argv_from_string(a->path, str, a->argbuf, SPAWN_ARGS_MAX + FS_PATH_MAX,
                                     &a->args_len))
                a->args = a->argbuf;
            else
                bad = 1;
            if (str) kfree(str);
        }
    }
    if (bad) {
        klog_printf("syscall: %s() rejected -- bad or oversized arguments\n", who);
        spawn_args_free(a);
        return -EINVAL;
    }
    return 0;
}

int sys_spawn(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    int64_t spawn_rc = -ENOENT; // no such program, unless something below says otherwise

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
    // AN UNKNOWN FLAG IS REFUSED, NOT IGNORED. `flags` took the tail of
    // this struct, so every caller that predates it passes 0 -- and a
    // word that silently drops bits it does not recognise can never be
    // extended safely, because an old kernel would accept a new flag and
    // do nothing. Same reasoning as the "reserved must be zero" check
    // `pgid` replaced.
    if (msg.flags & ~(uint32_t)SPAWN_FLAGS_ALL) {
        klog_write("syscall: spawn() rejected -- unknown flag\n");
        c->regs[14] = (uint64_t)(int64_t)-EINVAL;
        return 0;
    }

    struct spawn_args a;
    int rc = spawn_args_collect(pml4, &msg, &a, "spawn");
    if (rc < 0) {
        c->regs[14] = (uint64_t)(int64_t)rc;
        return 0;
    }

    // The child's stdin/stdout overrides, as DESCRIPTION indices
    // rather than pipe indices: the child's fd will simply name the
    // same open file, which is the general mechanism and not a pipe
    // special case. An fd that isn't one of the kinds below is
    // REFUSED rather than quietly ignored -- spawning with console
    // output instead would leave the parent blocked on a pipe
    // nothing will ever write to.
    //
    // THE TEST IS `!= -1`, NOT `>= 0`: -1 is the only value meaning
    // "the console", and every other negative is a sentinel or a
    // mistake. Written as `>= 0` it silently swallowed
    // SPAWN_FD_LOG, so a service spawned onto the log printed to the
    // console with nothing refused and nothing logged.
    int stdout_desc = -1, stdin_desc = -1;
    int ok = 1;
    if (msg.stdout_fd != -1) {
        stdout_desc = spawn_std_desc(pml4, (int)msg.stdout_fd, FD_KIND_PIPE_W);
        if (stdout_desc < 0) {
            klog_write("syscall: spawn() rejected -- stdout fd isn't this process's pipe write end or socket\n");
            spawn_rc = -EBADF;
            ok = 0;
        }
    }
    if (ok && msg.stdin_fd != -1) {
        stdin_desc = spawn_std_desc(pml4, (int)msg.stdin_fd, FD_KIND_PIPE_R);
        if (stdin_desc < 0) {
            klog_write("syscall: spawn() rejected -- stdin fd isn't this process's pipe read end or socket\n");
            spawn_rc = -EBADF;
            ok = 0;
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
        // TRACING IS ARMED HERE AND CONSUMED BY THE SPAWN ITSELF.
        // The arm records THIS process (kernel/strace.h), so the
        // window between these two lines is not a race: nobody
        // else's spawn can collect it. The disarm covers the spawn
        // having failed before an address space existed.
        if (msg.flags & SPAWN_TRACE) strace_arm_for_current();
        int pid = scheduler_spawn_group(a.path, a.args, a.args_len, stdout_desc,
                                         stdin_desc, a.env, msg.pgid, c->pml4);
        strace_disarm();
        if (pid > 0) spawn_rc = pid;
        // SPAWN_FOREGROUND: the child's group in front of OUR fd 0,
        // before the child can possibly read -- it is this syscall
        // that creates it, so there is no window. Failure is a
        // no-op by contract (not a terminal, not the owner): the
        // after-the-fact tcsetpgrp this replaces behaved the same.
        // BEFORE the foreground line below, which reads the child's
        // group: a session leader leads a group of its own, so the two
        // flags together would otherwise put the OLD group in front.
        if (pid > 0 && (msg.flags & SPAWN_SETSID)) {
            scheduler_make_session_leader(pid);
            // **AND THE TERMINAL BECOMES THE NEW SESSION'S**, which is
            // POSIX acquiring a controlling terminal when a session
            // leader gets one. Ownership is otherwise claimed on the
            // first READ of a pty slave (syscall_fd.c) -- fine for a
            // shell that reads before it asks, and wrong for one that
            // asks first: dash calls tcgetpgrp() during startup, got
            // -ENODEV because nobody owned the terminal yet, and
            // printed "can't access tty; job control turned off". It
            // then needed /dev/null for a background job's stdin, which
            // this system does not have -- one cause, two symptoms.
            //
            // **NEVER THE PHYSICAL CONSOLE, and that guard is the whole
            // safety of this.** tty0's owner is whoever READS it and
            // must stay that way: in a graphical boot nothing owns it,
            // so without this the first session leader to come along --
            // a Terminal window's shell, or a telnet login -- takes the
            // console and its foreground group, and the machine's own
            // keyboard stops working while the on-screen one still
            // does. That happened, on hardware, and it does not
            // reproduce under a text boot because tosh already owns
            // tty0 there and the claim is skipped.
            uint64_t child_as = scheduler_pid_pml4(pid);
            struct tty *ct = child_as ? fd_tty(child_as, 0) : 0;
            if (ct && ct != tty_console() && !tty_owner(ct))
                tty_set_owner(ct, pid);
        }
        if (pid > 0 && (msg.flags & SPAWN_FOREGROUND))
            tty_set_fg_pgid(fd_tty(pml4, 0), scheduler_pgid(pid));
    }
    spawn_args_free(&a);
    c->regs[14] = (uint64_t)(int64_t)spawn_rc;
    return 0;
}

// An exec is a spawn into the caller's own slot, and takes the same
// message so there is one shape (abi/syscall_abi.h). The fields that
// only mean something for a CHILD are refused rather than ignored.
int sys_exec(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    struct spawn_msg msg;
    if (!vmm_copy_from_user(pml4, &msg, c->a0, sizeof msg)) {
        klog_write("syscall: exec() rejected -- invalid message pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    if (msg.stdin_fd != -1 || msg.stdout_fd != -1 || msg.pgid != 0 ||
        (msg.flags & ~(uint32_t)SPAWN_ARGV)) {
        klog_write("syscall: exec() rejected -- a stream, group or flag that only a child could take\n");
        c->regs[14] = (uint64_t)(int64_t)-EINVAL;
        return 0;
    }
    struct spawn_args a;
    int rc = spawn_args_collect(pml4, &msg, &a, "exec");
    if (rc == 0) rc = scheduler_exec(a.path, a.args, a.args_len, a.env, c->regs);
    spawn_args_free(&a);
    // On success the trapframe already holds the new image's entry and
    // RAX is one of the registers it clears -- there is nobody to
    // return a value to.
    if (rc < 0) c->regs[14] = (uint64_t)(int64_t)rc;
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
    // A STOP IS CHECKED BEFORE AN EXIT, for both forms below, and the
    // order is deliberate: a stopped child is still alive, so the poll
    // that follows would report it as "still running" and park the
    // caller on a wake that has already happened. Only a caller that
    // asked (SYS_WUNTRACED) can see one at all.
    if (!bad && (c->a2 & SYS_WUNTRACED)) {
        int stopped_pid = pid, sig = 0;
        if (pid == -1)
            sig = scheduler_stop_report_any(scheduler_current_tgid(), &stopped_pid);
        else if (scheduler_pid_valid(pid))
            sig = scheduler_stop_report(pid);
        if (sig) {
            int code = SIGNAL_STOP_BASE + sig;
            if (out) vmm_copy_to_user(pml4, out, &code, sizeof code);
            c->regs[14] = (uint64_t)(int64_t)stopped_pid;
            return 0;
        }
    }

    if (!bad && pid == -1) {
        int child = 0, code = 0;
        // **ARMED BEFORE THE POLL.** A child that exits between "is one
        // dead?" and the park below wakes a parent that is not blocked
        // yet, and scheduler_wake() only finds one that is -- so the
        // wake is dropped and the parent sleeps with a zombie child it
        // asked about. An interrupt gate hid this by construction:
        // nothing else could run inside the syscall. Measured under a
        // trap gate as `shm_test` in block(child) beside `shm_child` in
        // zombie, which is the whole bug in two lines of `ps`.
        scheduler_wait_arm(scheduler_wait_chan_pid(scheduler_current_tgid()));
        enum sched_poll_result r =
            scheduler_poll_any(scheduler_current_tgid(), &child, &code);
        if (r != SCHED_POLL_RUNNING || (c->a2 & SYS_WNOHANG))
            scheduler_wait_disarm();   // every path that does not park
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
        } else if (!scheduler_block_current(c->regs, scheduler_wait_chan_pid(scheduler_current_tgid()), SCHED_WAIT_CHILD)) {
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
        // Armed before the poll -- see the pid == -1 branch above.
        scheduler_wait_arm(scheduler_wait_chan_pid(scheduler_current_tgid()));
        enum sched_poll_result r = scheduler_poll(pid, &code);
        if (r != SCHED_POLL_RUNNING || (c->a2 & SYS_WNOHANG))
            scheduler_wait_disarm();
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
            if (!scheduler_block_current(c->regs, scheduler_wait_chan_pid(scheduler_current_tgid()), SCHED_WAIT_CHILD)) {
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
        g_legacy_mm.brk = 0;
    }
}
