// The windowing syscalls: the event queue and TWP's carriage
// (SYS_WIN_REQUEST). The kernel draws no window and maps no
// framebuffer here -- a client's path is a typed TWP message
// (abi/win_proto.h), and the compositor's grant is WIN_REQ_FB_MAP.
#include "syscalls.h"
#include "syscall_abi.h"
#include "errno.h"
#include "klog.h"
#include "vmm.h"
#include "scheduler.h"
#include "win_input.h"
#include "win_role.h"
#include "clocksource.h" // clocksource_now_ns() -- the timed wait's deadline
#include <stddef.h>

int sys_win_request(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    // THE PROCESS, not the calling thread: a window belongs to the
    // program, so a second thread of it must find the same windows and
    // the same event queue rather than a fresh, empty client.
    int pid = scheduler_current_tgid();

    struct win_request_msg req;
    if (!vmm_copy_from_user(pml4, &req, c->a0, sizeof req)) {
        klog_write(KLOG_ERR "syscall: win_request() rejected -- invalid user pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else if (pid == 0) {
        klog_write(KLOG_ERR "syscall: win_request() rejected -- caller isn't a scheduled process\n");
        c->regs[14] = (uint64_t)(int64_t)-EPERM;
    } else {
        // Copy in, act, copy back: the request is handled against a
        // KERNEL copy, never against the user page directly. The
        // client shares that page and could otherwise change a
        // field after it was validated but before it was used --
        // and `window` in particular is used to index the server's
        // own tables.
        //
        // The copy happens BEFORE the "is there a window server"
        // gate below, because that gate now has a typed exception
        // and the type is only knowable from the copy.

        // "Is there a window server?" means "does a compositor hold the
        // role?" -- the only kind there is.
        if (!win_server_any() && req.type != WIN_REQ_SET_COMPOSITOR) {
            // No desktop running. Refused rather than silently
            // succeeding, so a client started outside GUI mode finds
            // out immediately instead of drawing into a buffer nothing
            // will ever composite.
            //
            // SET_COMPOSITOR is exempt: claiming the role is what a
            // window server does before it is one. See abi/win_proto.h.
            klog_write(KLOG_ERR "syscall: win_request() rejected -- no window server registered\n");
            c->regs[14] = (uint64_t)(int64_t)-ENODEV;
        } else {
            int rc = win_server_request(pid, &req);

            // Only copy back a request the server actually looked at
            // -- a malformed one leaves the client's buffer as sent.
            // The SERVER's own return value, passed through unchanged.
            // TWP has its own failure vocabulary inside `req` and this
            // is not translated into an error code -- doing so would
            // put two error systems on one return value, and the
            // protocol is the one that knows what went wrong. See
            // abi/win_proto.h.
            if (rc >= 0) vmm_copy_to_user(pml4, c->a0, &req, sizeof req);
            c->regs[14] = (uint64_t)(int64_t)rc;
        }
    }
    return 0;
}


// **THE QUEUE IS THE COMPOSITOR'S.** Anyone else asking is refused
// with -EPERM -- a client's events are on its own channel ring, and a
// stale binary still asking here should find out at once rather than
// park forever on a queue nothing feeds. Logged once, not per call.
static int compositor_only(int pid, const char *what) {
    if (pid && pid == win_server_compositor_pid()) return 1;
    static int said;
    if (!said++) {
        klog_write(KLOG_ERR "syscall: ");
        klog_write(what);
        klog_write("() rejected -- only the compositor has an event queue\n");
    }
    return 0;
}

// SYS_POLL_EVENT and SYS_WAIT_EVENT share everything except what
// happens when the queue is empty, so they share a body rather than
// duplicating the validation and the copy-out. Whether to block is a
// PARAMETER, for the reason send_recv() gives.
static int event_get(struct syscall_ctx *c, int blocking) {
    uint64_t pml4 = c->pml4;
    int pid = scheduler_current_tgid(); // the process -- see sys_win_request()

    if (!vmm_validate_user_range(pml4, c->a0, sizeof(struct win_event))) {
        klog_write(KLOG_ERR "syscall: event() rejected -- invalid user pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else if (!compositor_only(pid, "event")) {
        c->regs[14] = (uint64_t)(int64_t)-EPERM;
    } else {
        // **ARMED BEFORE THE QUEUE IS TESTED.** A push landing between
        // the pop below and the park would otherwise find this process
        // still RUNNING, and scheduler_wake() only sees blocked ones --
        // the lost wakeup that made a preemptible syscall unsafe. The
        // arm gives that wake something to land on; the park then
        // declines and answers instead. Disarmed on every path that
        // does not go on to park.
        scheduler_wait_arm(win_input_wait_chan());
        struct win_event ev;
        if (win_input_pop(&ev)) {
            scheduler_wait_disarm();
            // Validated above BEFORE the pop, so a bad pointer cannot
            // lose an event; a copy that still fails says so.
            c->regs[14] = vmm_copy_to_user(pml4, c->a0, &ev, sizeof ev) ? 1 : (uint64_t)(int64_t)-EFAULT;
        } else if (!blocking) {
            scheduler_wait_disarm();
            c->regs[14] = 0; // empty, and this one never blocks
        } else {
            // Park until something is queued. The "queue was empty"
            // test above and this park are made atomic by the ARM
            // above, not by interrupts being off -- they are not, once
            // the syscall gate is a trap gate. See scheduler.h's
            // prepare_to_wait section for the window and what closes
            // it.
            //
            // On success this MUST NOT set c->regs[14]: the process is
            // no longer the one running, and scheduler_wake() will
            // write the return value (0, "ask again") straight into
            // the trapframe saved here. Writing it now would
            // clobber that.
            if (!scheduler_block_current(c->regs, win_input_wait_chan(), SCHED_WAIT_EVENT)) {
                c->regs[14] = (uint64_t)(int64_t)-EPERM; // couldn't park -- see above
            } else {
                // Parked. c->regs[14] is NOT this call's return value
                // (it still holds the syscall number), so the
                // normal strace_end() below would print a bogus
                // one. Close the line as "= ?" instead, the same
                // way SYS_EXIT does -- this handler isn't returning
                // a value either. When the process is woken it
                // re-enters the syscall and gets its own trace
                // line, so a blocking wait reads as a "= ?" per
                // park followed by the real result.
                return 1;
            }
        }
    }
    return 0;
}

int sys_poll_event(struct syscall_ctx *c) { return event_get(c, 0); }
int sys_wait_event(struct syscall_ctx *c) { return event_get(c, 1); }

// Readiness with a deadline, consuming nothing -- see SYS_WAIT_READY.
// The queue test and the park are made atomic by scheduler_wait_arm(),
// NOT by interrupts being off -- they are not, once the syscall gate is
// a trap gate. Same reasoning as event_get() above, and it is still the
// reason the check cannot be hoisted into a helper that returns first.
int sys_wait_ready(struct syscall_ctx *c) {
    int pid = scheduler_current_tgid();
    if (!compositor_only(pid, "wait_ready")) {
        c->regs[14] = (uint64_t)(int64_t)-EPERM;
        return 0;
    }
    // Armed before the queue is tested, for the reason event_get()
    // above records.
    scheduler_wait_arm(win_input_wait_chan());
    if (win_input_pending()) { scheduler_wait_disarm(); c->regs[14] = 1; return 0; }

    uint64_t ms = c->a0;
    if (!ms) { scheduler_wait_disarm(); c->regs[14] = 0; return 0; }
    if (ms > SYS_SLEEP_MAX_MS) ms = SYS_SLEEP_MAX_MS;

    uint64_t deadline = clocksource_now_ns() + ms * 1000000ull;
    if (!scheduler_block_current_until(c->regs, win_input_wait_chan(),
                                       SCHED_WAIT_EVENT, deadline)) {
        c->regs[14] = (uint64_t)(int64_t)-EPERM;
        return 0;
    }
    return 1; // parked -- the waker writes the return value, as in event_get()
}

// The legacy single-window state, for the same reason
// proc_syscall_release() exists: it is a global here, not a mapping, so
// tearing the address space down does not clear it. The pixel buffer's
// pages ARE part of the address space and are freed with it.
void win_syscall_release(uint64_t pml4_phys) {
    // NOTHING LEFT TO RELEASE. It held the legacy single-window path's
    // globals, which went with that path; the hook stays because
    // syscall.c calls it on every address-space teardown and a future
    // per-process window global would want exactly this seam.
    (void)pml4_phys;
}
