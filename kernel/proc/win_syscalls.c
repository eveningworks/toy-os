// The windowing syscalls: the event queue, TWP's carriage
// (SYS_WIN_REQUEST) and its debug channel.
//
// **THE KERNEL DRAWS NO WINDOW.** SYS_WIN_CREATE and SYS_WIN_PRESENT
// used to composite one here -- a title bar, a close button and a
// per-pixel blit, all through gfx_* from inside a syscall -- and were
// the last GUI DRAWING in ring 0. They predated the window server,
// served one process at a time, and had no caller but their own test.
// A ring-3 client's windowing path is SYS_WIN_REQUEST, which carries a
// typed TWP message (abi/win_proto.h).
//
// SYS_GUI_INIT survives and is a different thing: it maps the real
// framebuffer to a ring-3 caller and draws nothing itself.
#include "syscalls.h"
#include "syscall_abi.h"
#include "errno.h"
#include "klog.h"
#include "vmm.h"
#include "pmm.h"
#include "gfx.h"
#include "keyboard.h"
#include "scheduler.h"
#include "win_events.h"
#include "win_server.h"
#include "clocksource.h" // clocksource_now_ns() -- the timed wait's deadline
#include <stddef.h>

int sys_gui_init(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;

    struct gui_info info;
    info.width = (uint32_t)gfx_width();
    info.height = (uint32_t)gfx_height();
    info.pitch = gfx_framebuffer_pitch();
    info.bpp = gfx_framebuffer_bpp();
    if (!vmm_copy_to_user(pml4, c->a0, &info, sizeof info)) {
        klog_write("syscall: gui_init() rejected -- invalid info pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else {

        uint64_t fb_phys = gfx_framebuffer_phys();
        uint64_t fb_size = (uint64_t)info.pitch * info.height;
        uint64_t pages = (fb_size + 4095) / 4096;

        int ok = 1;
        for (uint64_t i = 0; i < pages; i++) {
            if (!vmm_map_user_page(pml4, GUI_FB_VADDR + i * 4096, fb_phys + i * 4096)) {
                ok = 0;
                break;
            }
        }
        klog_write(ok ? "syscall: gui_init() mapped the framebuffer\n"
                         : "syscall: gui_init() failed to map the framebuffer\n");
        c->regs[14] = ok ? 0 : (uint64_t)(int64_t)-ENOMEM;
    }
    return 0;
}

int sys_gui_poll_key(struct syscall_ctx *c) {
    int key = keyboard_try_getchar(); // already non-blocking
    c->regs[14] = (uint64_t)(int64_t)key;
    return 0;
}

int sys_read_key(struct syscall_ctx *c) {
    // Non-blocking, same as SYS_GUI_POLL_KEY above (echo.c spins,
    // calling this again if it gets -1) -- NOT a design choice,
    // a hard requirement. A genuinely blocking version was tried
    // first: `sti` then keyboard_getchar()'s `hlt` loop, so a real
    // keyboard IRQ could land while this syscall was still on the
    // stack. It worked for exactly one keystroke and then hung --
    // g_next_kernel_rsp (idt.c) is a single global "where to resume"
    // pointer, correct for the scheduler's use (see scheduler.c's
    // design comment) but never meant to be reentrant: the nested
    // IRQ1 handler overwrites it while the outer int-0x80 handler
    // is still executing, so by the time THIS handler's own
    // isr_common epilogue runs, it resumes into a stale frame
    // instead of back into ring 3. Never make a syscall handler
    // block-with-interrupts-on in this codebase without fixing that
    // global first.
    int key = keyboard_try_getchar();
    c->regs[14] = (uint64_t)(int64_t)key;
    return 0;
}

int sys_win_request(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    // THE PROCESS, not the calling thread: a window belongs to the
    // program, so a second thread of it must find the same windows and
    // the same event queue rather than a fresh, empty client.
    int pid = scheduler_current_tgid();

    struct win_request_msg req;
    if (!vmm_copy_from_user(pml4, &req, c->a0, sizeof req)) {
        klog_write("syscall: win_request() rejected -- invalid user pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else if (pid == 0) {
        klog_write("syscall: win_request() rejected -- caller isn't a scheduled process\n");
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
            klog_write("syscall: win_request() rejected -- no window server registered\n");
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

int sys_win_debug(struct syscall_ctx *c) {
    // TWP's diagnostic channel, reachable from ring 3 so a ring-3
    // compositor can answer `gui` commands. Same copy-in / act /
    // copy-back discipline as SYS_WIN_REQUEST above, and for the
    // same reason: the client shares the page and could otherwise
    // change a field between validation and use.
    uint64_t pml4 = c->pml4;
    int pid = scheduler_current_tgid(); // the process -- see sys_win_request()
    struct win_debug_msg msg;
    if (!vmm_copy_from_user(pml4, &msg, c->a0, sizeof msg)) {
        klog_write("syscall: win_debug() rejected -- invalid user pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else if (pid == 0) {
        c->regs[14] = (uint64_t)(int64_t)-EPERM;
    } else {
        int rc = win_server_debug(pid, &msg);
        if (!vmm_copy_to_user(pml4, c->a0, &msg, sizeof msg)) rc = -EFAULT;
        c->regs[14] = (uint64_t)(int64_t)rc;
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
        klog_write("syscall: event() rejected -- invalid user pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else if (pid == 0) {
        // No scheduler slot, so no event queue and nowhere to park.
        // Refused rather than silently degraded to a never-blocking
        // call, which would turn the documented `while (... != 1)`
        // client loop into a busy spin.
        klog_write("syscall: event() rejected -- caller has no event queue\n");
        c->regs[14] = (uint64_t)(int64_t)-EPERM;
    } else {
        // **ASKING FOR A WINDOW EVENT IS THE DECLARATION.** Nothing but
        // a windowing client calls either of these, so this is where
        // the kernel learns which processes to reach with a broadcast
        // -- the question the window table used to answer. Marked on
        // the POLLING path too: an app with a tick and no timer never
        // blocks, and would otherwise never hear the font change.
        win_events_mark_client(pid);

        struct win_event ev;
        if (win_events_pop(pid, &ev)) {
            // Validated above BEFORE the pop, so a bad pointer cannot
            // lose an event; a copy that still fails says so.
            c->regs[14] = vmm_copy_to_user(pml4, c->a0, &ev, sizeof ev) ? 1 : (uint64_t)(int64_t)-EFAULT;
        } else if (!blocking) {
            c->regs[14] = 0; // empty, and this one never blocks
        } else {
            // Park until something is queued. Interrupts are OFF
            // for this whole handler, so the "queue was empty" test
            // above and this park are atomic with respect to an IRQ
            // pushing an event -- there is no window in which an
            // event arrives after the check and is missed by the
            // block, which is the classic lost-wakeup bug.
            //
            // On success this MUST NOT set c->regs[14]: the process is
            // no longer the one running, and scheduler_wake() will
            // write the return value (0, "ask again") straight into
            // the trapframe saved here. Writing it now would
            // clobber that.
            if (!scheduler_block_current(c->regs, win_events_wait_chan(pid), SCHED_WAIT_EVENT)) {
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
// The queue test and the park are atomic with respect to an IRQ pushing
// an event, because this whole handler runs with interrupts off; that is
// the same lost-wakeup argument event_get() makes, and it is the reason
// the check cannot be hoisted into a helper that returns first.
int sys_wait_ready(struct syscall_ctx *c) {
    int pid = scheduler_current_tgid();
    if (pid == 0) {
        klog_write("syscall: wait_ready() rejected -- caller has no event queue\n");
        c->regs[14] = (uint64_t)(int64_t)-EPERM;
        return 0;
    }
    if (win_events_pending(pid)) { c->regs[14] = 1; return 0; }

    uint64_t ms = c->a0;
    if (!ms) { c->regs[14] = 0; return 0; }
    if (ms > SYS_SLEEP_MAX_MS) ms = SYS_SLEEP_MAX_MS;

    uint64_t deadline = clocksource_now_ns() + ms * 1000000ull;
    if (!scheduler_block_current_until(c->regs, win_events_wait_chan(pid),
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
