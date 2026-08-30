// The windowing syscalls: the legacy single-window path, the event
// queue, TWP's carriage (SYS_WIN_REQUEST) and its debug channel.
//
// The legacy SYS_GUI_*/SYS_WIN_CREATE/SYS_WIN_PRESENT state below
// predates the window server and is single-process-at-a-time; the real
// windowing path for a ring-3 client is SYS_WIN_REQUEST, which carries
// a typed TWP message (abi/win_proto.h).
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
#include "win_transport.h"
#include "clocksource.h" // clocksource_now_ns() -- the timed wait's deadline
#include <stddef.h>

// SYS_WIN_* state -- like the heap above, single-window/single-process
// at a time (see syscall_abi.h's comment on SYS_WIN_CREATE). Unlike the
// heap, this doesn't need an explicit "arm" call from whoever spawns
// the process: SYS_WIN_CREATE itself sets g_win_pml4 to the CALLING
// process's own CR3, so it's self-arming, and SYS_WIN_PRESENT's
// pml4-mismatch check (same trick as SYS_SBRK's) rejects any process
// that calls it without having created a window first, including a
// later, unrelated process that happens to run after this one exits.
//
// g_win_frames tracks each backing page's PHYSICAL address individually
// rather than assuming the run pmm_alloc_frame() hands back is
// contiguous (it usually is, for a fresh process with nothing else
// allocating concurrently, but pmm.h makes no such promise) -- so
// SYS_WIN_PRESENT can read the buffer directly via each frame's
// identity-mapped physical address without trusting that.
#define WIN_MAX_PAGES ((WIN_MAX_W * WIN_MAX_H * 4 + 4095) / 4096)
static uint64_t g_win_pml4 = 0;
static uint64_t g_win_frames[WIN_MAX_PAGES];
static uint32_t g_win_pages = 0;
static uint32_t g_win_w = 0, g_win_h = 0, g_win_pitch = 0;
static int32_t g_win_x = 0, g_win_y = 0;

#define WIN_TITLEBAR_H (gfx_char_h() + 8)

static int win_close_size(void) {
    int s = WIN_TITLEBAR_H - 6;
    return s < 14 ? 14 : s;
}

// Same hand-drawn diagonal cross as apps/wm.c's draw_close_icon() (see
// the git history for why a font glyph doesn't work in a small button) --
// duplicated rather than shared, since wm.c's version is `static` in a
// completely different translation unit (the GUI app layer), and
// kernel/core has no existing reason to link against apps/.
static void win_draw_close_icon(int x, int y, int size, uint32_t color) {
    int pad = size / 4;
    if (pad < 2) pad = 2;
    int thick = size >= 24 ? 1 : 0;
    for (int i = pad; i < size - pad; i++) {
        for (int t = -thick; t <= thick; t++) {
            gfx_put_pixel(x + i + t, y + i, color);
            gfx_put_pixel(x + i, y + i + t, color);
            gfx_put_pixel(x + i + t, y + (size - 1 - i), color);
            gfx_put_pixel(x + i, y + (size - 1 - i) + t, color);
        }
    }
}

// Reads one already-native-packed pixel (see userland/win_test.c's
// comment on why it can write raw values with no gfx_rgb()-equivalent
// of its own) out of the process's private window buffer.
// byte_offset is always a multiple of 4 (pitch is w*4, no padding), and
// 4096 is itself a multiple of 4, so a 4-byte pixel read here never
// straddles a page boundary -- each one lives entirely in exactly one
// tracked frame.
static uint32_t win_buf_read_pixel(uint64_t byte_offset) {
    uint32_t page = (uint32_t)(byte_offset / 4096);
    uint32_t off = (uint32_t)(byte_offset % 4096);
    if (page >= g_win_pages) return 0;
    return *(volatile uint32_t *)(uintptr_t)(g_win_frames[page] + off);
}

// The "server" half of the protocol: composites the current window
// buffer plus kernel-drawn chrome onto the real screen. Runs entirely
// in kernel space (this is a syscall handler), so -- unlike the
// process that owns the buffer -- it can call gfx_* directly with no
// syscall of its own needed to reach the real framebuffer.
static void win_present(void) {
    int titlebar_h = WIN_TITLEBAR_H;
    int close_size = win_close_size();
    uint32_t border = gfx_rgb(60, 60, 60);
    uint32_t titlebar = gfx_rgb(70, 110, 180);
    uint32_t titletext = gfx_rgb(255, 255, 255);
    uint32_t winbg = gfx_rgb(235, 235, 235);

    int win_w = (int)g_win_w;
    int win_h = (int)g_win_h;
    int total_h = titlebar_h + win_h;

    gfx_fill_rect(g_win_x, g_win_y, win_w, total_h, winbg);
    gfx_draw_rect(g_win_x, g_win_y, win_w, total_h, border);
    gfx_fill_rect(g_win_x + 1, g_win_y + 1, win_w - 2, titlebar_h, titlebar);
    gfx_draw_string(g_win_x + 6, g_win_y + (titlebar_h - gfx_char_h()) / 2,
                     "App", titletext, titlebar);

    int close_x = g_win_x + win_w - 6 - close_size;
    int close_y = g_win_y + (titlebar_h - close_size) / 2;
    gfx_fill_rect(close_x, close_y, close_size, close_size, gfx_rgb(190, 60, 60));
    win_draw_close_icon(close_x, close_y, close_size, gfx_rgb(255, 255, 255));

    int content_y = g_win_y + titlebar_h;
    for (int y = 0; y < win_h; y++) {
        for (int x = 0; x < win_w; x++) {
            uint32_t pixel = win_buf_read_pixel((uint64_t)y * g_win_pitch + (uint64_t)x * 4);
            gfx_put_pixel(g_win_x + x, content_y + y, pixel);
        }
    }
}

int sys_gui_init(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;

    if (!vmm_validate_user_range(pml4, c->a0, sizeof(struct gui_info))) {
        klog_write("syscall: gui_init() rejected -- invalid info pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else {
        struct gui_info info;
        info.width = (uint32_t)gfx_width();
        info.height = (uint32_t)gfx_height();
        info.pitch = gfx_framebuffer_pitch();
        info.bpp = gfx_framebuffer_bpp();
        vmm_copy_to_user(pml4, c->a0, &info, sizeof info); // range validated just above

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

int sys_win_create(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;

    if (!vmm_validate_user_range(pml4, c->a0, sizeof(struct win_request))) {
        klog_write("syscall: win_create() rejected -- invalid request pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else {
        struct win_request req;
        vmm_copy_from_user(pml4, &req, c->a0, sizeof req); // range validated just above
        int bad_size = (req.w == 0 || req.h == 0 || req.w > WIN_MAX_W || req.h > WIN_MAX_H);

        if (bad_size) {
            klog_write("syscall: win_create() rejected -- bad size\n");
            c->regs[14] = (uint64_t)(int64_t)-EINVAL;
        } else {
            uint64_t size = (uint64_t)req.w * 4 * req.h;
            uint32_t pages_needed = (uint32_t)((size + 4095) / 4096);
            uint32_t i;
            int ok = 1;

            for (i = 0; i < pages_needed; i++) {
                uint64_t frame = pmm_alloc_frame();
                if (!frame) { ok = 0; break; }
                for (size_t b = 0; b < 4096; b++) ((uint8_t *)(uintptr_t)frame)[b] = 0;
                if (!vmm_map_user_page(pml4, WIN_BUF_VADDR + (uint64_t)i * 4096, frame)) {
                    pmm_free_frame(frame);
                    ok = 0;
                    break;
                }
                g_win_frames[i] = frame;
            }

            if (!ok) {
                for (uint32_t j = 0; j < i; j++) pmm_free_frame(g_win_frames[j]);
                klog_write("syscall: win_create() rejected -- out of physical memory\n");
                c->regs[14] = (uint64_t)(int64_t)-ENOMEM;
            } else {
                g_win_pml4 = pml4;
                g_win_pages = pages_needed;
                g_win_w = req.w;
                g_win_h = req.h;
                g_win_pitch = req.w * 4;
                g_win_x = req.x;
                g_win_y = req.y;

                req.pitch = g_win_pitch;
                req.bpp = 32;
                vmm_copy_to_user(pml4, c->a0, &req, sizeof req);
                c->regs[14] = 0;
            }
        }
    }
    return 0;
}

int sys_win_present(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    if (g_win_pml4 == 0 || pml4 != g_win_pml4) {
        c->regs[14] = (uint64_t)(int64_t)-EPERM; // not this process's window
    } else {
        win_present();
        c->regs[14] = 0;
    }
    return 0;
}

int sys_win_request(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    // THE PROCESS, not the calling thread: a window belongs to the
    // program, so a second thread of it must find the same windows and
    // the same event queue rather than a fresh, empty client.
    int pid = scheduler_current_tgid();

    if (!vmm_validate_user_range(pml4, c->a0, sizeof(struct win_request_msg))) {
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
        struct win_request_msg req;
        vmm_copy_from_user(pml4, &req, c->a0, sizeof req); // range validated above

        // "Is there a window server?" has two answers now. A
        // registered ring-0 presentation layer is one; a REGISTERED
        // COMPOSITOR is the other, and with the WM in ring 3 it is
        // the only one there will ever be -- there is no kernel-side
        // layer to register at all.
        //
        // Gating on win_server_active() alone rejected every request
        // a ring-3 WM made after claiming the role, starting with
        // the framebuffer grant, so the desktop exited before
        // drawing a pixel. SET_COMPOSITOR was already exempt for the
        // same reason; that exemption was just one request short.
        if (!win_server_any() && req.type != WIN_REQ_SET_COMPOSITOR) {
            // No desktop running. Refused rather than silently
            // succeeding, so a client started outside GUI mode finds
            // out immediately instead of drawing into a buffer nothing
            // will ever composite.
            //
            // SET_COMPOSITOR is exempt: claiming the role is what a
            // ring-3 window server does BEFORE there is a
            // presentation layer, and in stage 4 there is never a
            // kernel-side one. See abi/win_proto.h.
            klog_write("syscall: win_request() rejected -- no window server registered\n");
            c->regs[14] = (uint64_t)(int64_t)-ENODEV;
        } else {
            // Over the transport rather than straight into the
            // server: SYS_WIN_REQUEST is now ONE carriage for TWP,
            // not the only one (Milestone 41, stage 3). See
            // kernel/win_transport.h.
            int rc = win_transport_request(pid, &req);

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
    if (!vmm_validate_user_range(pml4, c->a0, sizeof(struct win_debug_msg))) {
        klog_write("syscall: win_debug() rejected -- invalid user pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
    } else if (pid == 0) {
        c->regs[14] = (uint64_t)(int64_t)-EPERM;
    } else {
        struct win_debug_msg msg;
        vmm_copy_from_user(pml4, &msg, c->a0, sizeof msg);
        // Straight to the server, not over the transport: the
        // transport carries messages TO the window server, and this
        // is the server's own client answering it. Routing it back
        // out through the transport would be a loop.
        int rc = win_server_debug(pid, &msg);
        vmm_copy_to_user(pml4, c->a0, &msg, sizeof msg);
        c->regs[14] = (uint64_t)(int64_t)rc;
    }
    return 0;
}

// The clipboard. The message is a kilobyte, so the HEADER rides the
// stack and the payload is copied straight in and out of the server's
// own buffer -- see win_server_clip_get() for why neither a stack copy
// nor a static scratch buffer would do.
#define CLIP_HDR_BYTES ((uint64_t)__builtin_offsetof(struct win_clip_msg, data))

int sys_win_clip(struct syscall_ctx *c) {
    uint64_t pml4 = c->pml4;
    int pid = scheduler_current_tgid(); // the process -- see sys_win_request()
    if (!vmm_validate_user_range(pml4, c->a0, sizeof(struct win_clip_msg))) {
        klog_write("syscall: win_clip() rejected -- invalid user pointer\n");
        c->regs[14] = (uint64_t)(int64_t)-EFAULT;
        return 0;
    }
    if (pid == 0) {
        c->regs[14] = (uint64_t)(int64_t)-EPERM;
        return 0;
    }

    struct { uint32_t type, op, count, len, serial, reserved; } hdr;
    vmm_copy_from_user(pml4, &hdr, c->a0, sizeof hdr);

    int rc = 0;
    if (hdr.type == WIN_REQ_CLIP_GET) {
        win_server_clip_get(&hdr.op, &hdr.count, &hdr.len, &hdr.serial);
        hdr.reserved = 0;
        vmm_copy_to_user(pml4, c->a0, &hdr, sizeof hdr);
        if (hdr.len)
            vmm_copy_to_user(pml4, c->a0 + CLIP_HDR_BYTES,
                              win_server_clip_buf(), hdr.len);
        rc = 1;
    } else if (hdr.type == WIN_REQ_CLIP_SET) {
        // Validated BEFORE anything is copied, so a refusal cannot
        // leave half a payload in the buffer.
        if (win_server_clip_would_fit(hdr.op, hdr.count, hdr.len)) {
            vmm_copy_from_user(pml4, win_server_clip_buf(),
                                c->a0 + CLIP_HDR_BYTES, hdr.len);
            hdr.serial = win_server_clip_commit(hdr.op, hdr.count, hdr.len);
            vmm_copy_to_user(pml4, c->a0, &hdr, sizeof hdr);
            rc = 1;
        }
    }
    c->regs[14] = (uint64_t)(int64_t)rc;
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
        struct win_event ev;
        if (win_events_pop(pid, &ev)) {
            vmm_copy_to_user(pml4, c->a0, &ev, sizeof ev); // range validated above
            c->regs[14] = 1;
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
    if (g_win_pml4 == pml4_phys) {
        g_win_pml4 = 0;
        g_win_pages = 0;
        g_win_w = 0;
        g_win_h = 0;
        g_win_pitch = 0;
        g_win_x = 0;
        g_win_y = 0;
    }
}
