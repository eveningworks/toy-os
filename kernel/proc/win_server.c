// **TWS -- the Toy Window Server**, what is left of it in ring 0: the
// compositor role, the framebuffer grant keyed on it, the font mapped
// into a client, and the `gui` diagnostic channel. No window. See
// abi/win_proto.h for TWP, the protocol it serves, and
// userland/wm/wm_client.c for the compositor that owns the windows.
#include "win_server.h"
#include "keyboard.h" // keyboard_suspend_blocking() -- who owns the keyboard follows the compositor role
#include "vmm.h"
#include "pmm.h"
#include "klog.h"
#include "string.h"
#include "font_ttf.h" // the glyph tables WIN_REQ_FONT shares out
#include "font_face.h" // ...or the runtime atlas, when a face is loaded
#include "gfx.h"      // gfx_font_size() -- which variant is active
#include "win_surface.h" // the compositor's framebuffer grant (M41 stage 4a)
#include "timer.h"       // pit_ticks() -- the ring-3 debug leg's deadline
#include "win_events.h"  // WIN_EV_CLOSE to clients when the desktop dies (R7)
#include "vga.h"         // vga_resume() -- hand the screen back (R7)
#include "kfmt.h"        // klog_printf
#include <stddef.h>

#include "scheduler.h" // SCHED_MAX_PROCS
#include "kerrno.h"  // EBUSY -- the diagnostic channel is one slot
#include "mouse.h"    // mouse_get_state() -- park the plane where the pointer is
#include "heap.h"     // kmalloc/kfree -- the DEFINE sprite bounce buffer
#include "initcall.h"
#include "query_abi.h"
#include "query.h"

#define WIN_SERVER_MAX_PIDS SCHED_MAX_PROCS

// **THE KERNEL DOES NOT TOUCH A WINDOW'S PIXELS.** A buffer is a named
// shm object the client creates and grants to the compositor
// (abi/win_proto.h's WIN_BUF_NAME_FMT), and the compositor opens the
// name itself -- so there is nothing here to allocate, adopt, map or
// free. What is left of a buffer in this file is its SIZE and its
// GENERATION, which is what a present has to carry.
// **THERE IS NO WINDOW TABLE.** Stage 6b took it: a window's slot, its
// two buffers, their sizes and their generations were all state the
// kernel kept in order to answer requests it no longer receives. Every
// one of those requests reaches the compositor over its own channel
// now, carrying its payload, and the compositor owns the list.
//
// What is left in this file is what ring 0 must own: the compositor
// ROLE, the framebuffer grant keyed on it, the font it maps into a
// client, and the per-process event queue that carries input outward.
// None of it knows that a window exists.

// The registered compositor: the one process raw input is delivered to,
// and the one the framebuffer is granted to.
static int g_comp_pid = 0;
static uint64_t g_comp_pml4 = 0;

// See win_proto.h's WIN_REQ_FB_CURSOR. Belongs to the ROLE, like the
// framebuffer grant: cleared in win_server_set_compositor(), the one
// place the role changes hands or dies.
static int g_hw_cursor_armed = 0;

int win_server_hw_cursor_armed(void) { return g_hw_cursor_armed; }

// Maps the desktop's active font read-only into the caller and reports
// its metrics. See WIN_REQ_FONT in abi/win_proto.h for why the server
// hands the font over rather than every client carrying a copy.
//
// Idempotent by construction: re-mapping the same pages over an
// existing identical mapping is a no-op in effect, so a client may ask
// again (after a font-size change, say) without unmapping first.
static int map_font(int pid, struct win_request_msg *req) {
    (void)pid;

    // `window` IS THE WEIGHT on this request, and on no other. See
    // WIN_REQ_FONT in abi/win_proto.h: a font belongs to the session,
    // so this is the one request whose `window` field never named a
    // window and was free to mean something else.
    int weight = (int)req->window;
    if (weight < 0 || weight >= WIN_FONT_WEIGHTS) return 0;

    // WHICHEVER FONT THE DESKTOP IS ACTUALLY DRAWING WITH -- a face
    // rasterized from /usr/share/fonts if one is selected, the baked
    // tables otherwise. Clients get the same glyphs either way, which is
    // the whole reason this request exists (see WIN_REQ_FONT in
    // abi/win_proto.h): a client carrying its own copy would keep
    // rendering the old face after `fontface` changed it.
    const struct font_atlas *atlas =
        font_face_atlas_weight(weight == WIN_FONT_BOLD ? FONT_WEIGHT_BOLD
                                                        : FONT_WEIGHT_REGULAR);
    const struct font_ttf_variant *fv = &font_ttf_variants[gfx_font_size()];

    // THE BAKED FONT HAS ONE WEIGHT, so a bold request against it is
    // REFUSED rather than answered with regular glyphs. A client that
    // got regular back under the name "bold" would draw a heading
    // identical to its body text and have no way to find out; a refusal
    // it can see means it keeps its own regular mapping and knows not
    // to switch. (font_face.c synthesizes bold for a real FACE with no
    // bold file -- that path never reaches here, because the atlas it
    // produces is an ordinary bold atlas.)
    if (weight == WIN_FONT_BOLD && !atlas) return 0;

    uint64_t phys, bytes, adv_off;
    int cw, ch, count, line_h;
    if (atlas) {
        // A runtime atlas is a page-aligned pmm run holding the glyph
        // data and the advance table and NOTHING ELSE -- which is why
        // font_face.c allocates it from the frame allocator rather than
        // from kmalloc. A page-granular mapping of a kmalloc'd atlas
        // would hand every client a read-only window onto whatever else
        // shared its pages.
        phys = atlas->phys;
        bytes = atlas->bytes;
        cw = atlas->cell_w; ch = atlas->cell_h; count = atlas->count;
        line_h = atlas->line_h;
        adv_off = (uint64_t)count * (uint64_t)cw * (uint64_t)ch;
        // The kern matrix follows the advances and the client DERIVES
        // its offset (win_font_kern_offset()) rather than being told --
        // which is only sound because `bytes` covers all three sections.
        // font_face.c sizes the allocation for exactly that.
    } else {
        phys = (uint64_t)(uintptr_t)fv->glyphs;
        cw = fv->w; ch = fv->h; count = FONT_TTF_GLYPH_COUNT;
        // The baked bitmaps were rasterized at build time INTO the
        // squeezed cell, so there is no taller bitmap to report -- the
        // pitch is the cell. That is why `builtin` still clips its
        // descenders and a loaded face does not.
        line_h = fv->h;
        bytes = (uint64_t)count * (uint64_t)cw * (uint64_t)ch;
        adv_off = 0; // the baked tables carry no advances: every cell is cw wide
    }

    // The glyph tables are ordinary kernel .rodata, which this kernel
    // identity-maps -- so their physical address IS the pointer we
    // already hold, and mapping them to a user vaddr is just pointing
    // more PTEs at the same frames. No copy, one instance in memory
    // however many clients ask. The same is true of an atlas, whose
    // frames come from the identity-mapped physical allocator.
    uint64_t page_base = phys & ~0xFFFULL;
    uint64_t offset_in_page = phys - page_base;
    uint64_t pages = (offset_in_page + bytes + 4095) / 4096;

    uint64_t pml4 = vmm_current_pml4();
    for (uint64_t i = 0; i < pages; i++) {
        // writable = 0: these are pages of the kernel image, and a
        // writable mapping would let any client scribble on kernel
        // .rodata. executable = 0 for the same no-surprises reason
        // every other user mapping here is NX.
        // BORROWED: pages of the KERNEL IMAGE, or of an atlas the font
        // cache owns forever. Nothing here ever frees them, and an
        // owning mapping made an exiting client return kernel .rodata to
        // the physical allocator -- measured, two frames per boot, on
        // the first GUI app to close.
        if (!vmm_map_user_borrowed(pml4, win_font_vaddr(weight) + i * 4096,
                                    page_base + i * 4096, 0, 0, VMM_MT_NORMAL)) {
            for (uint64_t j = 0; j < i; j++) {
                vmm_unmap_user_page(pml4, win_font_vaddr(weight) + j * 4096);
            }
            klog_write("win_server: font refused -- mapping failed\n");
            return 0;
        }
    }

    // The client sees the mapping at win_font_vaddr(weight) + the same
    // offset the data has within its first page, so glyph 0 starts
    // exactly there. Reported as `d`'s companion rather than assumed.
    req->a = cw;
    req->b = ch;
    req->c = count;
    req->d = (int32_t)offset_in_page;
    // `window` came in as the weight and goes out as the LINE PITCH --
    // a different number from `b`, which is the bitmap height. See
    // WIN_REQ_FONT in abi/win_proto.h for why confusing them is silent.
    req->window = (uint32_t)line_h;
    // `mods` carries the advance table's offset within the mapping, or 0
    // when there is none. 0 is unambiguous because a table can never
    // START the mapping -- the glyph data does. The KERN table's offset
    // is derived from this one; see win_font_kern_offset().
    req->mods = adv_off ? (uint32_t)(offset_in_page + adv_off) : 0;
    return 1;
}

int win_server_any(void) { return g_comp_pid != 0; }

// Tells the registered compositor that a client did something.
//
// A no-op when no compositor holds the role, which is every boot before
// the desktop starts.
//
// Fire and forget: no reply, no blocking. Nothing here needs an answer
// -- activate, the one request that did, is the compositor's now.
static void tell_compositor(uint32_t type, int pid, uint32_t id,
                             int32_t b, uint32_t mods) {
    if (!g_comp_pid) return;
    struct win_event ev;
    k_memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.window = id;
    ev.a = pid;
    ev.b = b;
    ev.mods = mods;
    win_events_push(g_comp_pid, &ev);
}

// **QUERY_WINDOWS IS RETIRED WITH THE TABLE.** It reported what
// win_server.c believed about each window, beside `guictl windows`'s
// report of what the compositor believed -- a pair that made a
// disagreement visible instead of inferred. There is nothing left to
// disagree: one process holds the list, and `guictl windows` is it.

// --- the diagnostic channel (Milestone 41, stage 3) -------------------
//
// One reply at a time, buffered here between the WM that formats it and
// the transport that carries it. See win_server.h on why the chunking
// lives on this side of the boundary rather than in the WM.
//
// A single static buffer, and a second DEBUG_CMD simply discards
// whatever the previous one had left: the console is the only client of
// this channel and it drains a reply before sending the next command.
// Sized for the longest reply any subcommand produces (`gui help`, ~1.8
// KB) with room to grow -- an over-long one is truncated with a marker
// rather than silently cut, matching kernel/lib's rule that a formatter
// which does not fit says so.
static char g_dbg_reply[WIN_DEBUG_REPLY_MAX];
static int  g_dbg_len = 0;  // bytes of reply held
static int  g_dbg_sent = 0; // how many of them have gone out

// ONE DIAGNOSTIC AT A TIME, AND THE SECOND CALLER IS TOLD SO.
//
// g_dbg_reply/g_dbg_sent above and the ring-3 leg below are ONE SLOT,
// which was safe while the serial console was the only client. It is
// not any more: `/bin/guictl` issues the same WIN_REQ_DEBUG_CMD from
// ring 3, so two callers can interleave a command with somebody else's
// chunk drain and read each other's bytes. A command arriving while
// another is in flight is REFUSED with -EBUSY rather than served --
// the reject-rather-than-guess rule this file already applies to the
// clipboard 60 lines down.
//
// THE CLAIM EXPIRES, and that is not belt-and-braces. A client killed
// between its command and its last chunk would otherwise hold the
// channel until reboot, and a diagnostic nobody can run is a worse
// failure than one that can be raced. Three seconds; a real drain is
// milliseconds.
static int g_dbg_owner;              // 0 = free. WIN_PID_KERNEL is the console
static uint64_t g_dbg_owner_until;   // pit_ticks() past which the claim lapses
#define DBG_OWNER_TICKS 300

// A ring-3 caller's command that has been posted to the compositor and
// not yet answered. It polls with WIN_REQ_DEBUG_MORE and gets
// WIN_DEBUG_F_PENDING until the answer lands -- see that flag.
static int g_dbg_awaiting;
static uint64_t g_dbg_await_until;

// Where a ring-3 compositor's answer lands before it is handed to the
// waiting caller. Separate from g_dbg_reply, which is the CHUNKING
// buffer the console reads out of -- writing straight into that would
// mean the compositor's reply racing the chunk being sent.
static char g_dbg_ring3[WIN_DEBUG_REPLY_MAX];
static uint32_t g_dbg_ring3_len;
static int g_dbg_reply_ready;
static unsigned g_dbg_reply_flags;

// Fills `msg` with the next chunk of the held reply.
static void dbg_take_chunk(struct win_debug_msg *msg) {
    int left = g_dbg_len - g_dbg_sent;
    if (left < 0) left = 0;

    int n = left > WIN_DEBUG_CHUNK ? WIN_DEBUG_CHUNK : left;
    for (int i = 0; i < n; i++) msg->text[i] = g_dbg_reply[g_dbg_sent + i];
    msg->text[n] = '\0';

    g_dbg_sent += n;
    msg->type = WIN_EV_DEBUG_OUT;
    msg->len = (uint32_t)n;
    // The flag is what makes the reply self-delimiting -- a chunk that
    // exactly fills the buffer is otherwise indistinguishable from a
    // truncated one. See WIN_DEBUG_F_MORE.
    if (g_dbg_sent < g_dbg_len) msg->flags |= WIN_DEBUG_F_MORE;
    else g_dbg_owner = 0;   // fully drained -- the channel is free again

    // WHAT THE ANSWERING COMPOSITOR SAID ABOUT THE COMMAND, not about
    // this chunk. g_dbg_reply_flags was accumulated and read by nobody,
    // so `gui nosuchthing` printed NOTHING from the moment the desktop
    // became a process -- an unknown subcommand looked exactly like one
    // that legitimately had no output, which is the distinction
    // WIN_DEBUG_F_UNKNOWN exists to make.
    msg->flags |= (g_dbg_reply_flags & WIN_DEBUG_F_UNKNOWN);
}


// --- the ring-3 debug leg (M41) --------------------------------------
//
// The command waiting for a ring-3 compositor to run it, and the reply
// coming back. One slot, guarded by the claim above.
static char g_dbg_pending[WIN_DEBUG_CMD_LEN];
static int g_dbg_pending_valid;

// How long the console waits for the compositor. Two seconds: long
// enough that a desktop busy with a slow frame still answers, short
// enough that a WEDGED one does not hang the console -- which would take
// the whole test harness down with it, since every tool arrives this
// way.
//
// A timeout is reported as an empty reply, NOT as an unknown command:
// those are different facts, and the tools distinguish them.
#define DBG_RING3_TIMEOUT_TICKS 200u

// Runs a `gui` command on a RING-3 compositor and waits for the answer.
// Returns the reply length, or -1 if nothing could be asked.
//
// **This blocks the caller**, which is the kernel context running the
// serial console -- not a syscall handler, so the hazard CLAUDE.md warns
// about (a nested IRQ clobbering the saved trapframe) does not apply
// here. The console already blocks this way on the filesystem for `sh
// cat big`, so a bounded wait is the behaviour it already has.
//
// It waits with interrupts ON and `hlt`: the timer has to keep firing,
// because it is what schedules the compositor that owes us the answer.
// Spinning with them off would deadlock against the very process being
// waited for -- the failure that looks like a hung machine rather than a
// slow one.
// Hands the command to the compositor and returns at once. -1 if there
// is no compositor to hand it to.
static int debug_post_to_compositor(const char *line) {
    if (!g_comp_pid) return -1;

    k_strlcpy(g_dbg_pending, line, sizeof g_dbg_pending);
    g_dbg_pending_valid = 1;
    g_dbg_reply_ready = 0;
    g_dbg_reply_flags = 0;
    g_dbg_ring3_len = 0;

    tell_compositor(WIN_EV_CLIENT_DEBUG, g_comp_pid, 0, 0, 0);
    return 0;
}

// The answer, once it is there. -1 while it is not.
static int debug_collect(char *out, int cap) {
    if (!g_dbg_reply_ready) return -1;
    g_dbg_pending_valid = 0;
    int n = (int)g_dbg_ring3_len;
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) out[i] = g_dbg_ring3[i];
    return n;
}

// THE CONSOLE'S round trip, and ONLY the console's. It waits in place
// with `sti; hlt`, which is legal here for one reason: the serial debug
// console is not a scheduled process, so there is no trapframe to
// corrupt and nothing to switch away from. A SYSCALL may not do this --
// api/scheduler.h says so in as many words, and handing a ring-3 caller
// this path faulted inside isr_common on the first try. Ring 3 gets the
// post/collect pair above and polls from its own side instead.
static int debug_via_compositor(const char *line, char *out, int cap) {
    if (debug_post_to_compositor(line) < 0) return -1;

    uint64_t deadline = pit_ticks() + DBG_RING3_TIMEOUT_TICKS;
    while (!g_dbg_reply_ready && pit_ticks() < deadline) {
        __asm__ volatile ("sti; hlt");
    }

    g_dbg_pending_valid = 0;
    if (!g_dbg_reply_ready) {
        klog_write("win: compositor did not answer a gui command in time\n");
        return 0; // empty, and deliberately NOT "unknown" -- see above
    }
    return debug_collect(out, cap);
}

int win_server_debug(int pid, struct win_debug_msg *msg) {
    (void)pid; // the console is the only client; kept for the ops shape
    if (!msg) return 0;

    // THE INCOMING FLAGS, TAKEN BEFORE THEY ARE CLEARED. `flags` is an
    // out-parameter on every path but WIN_REQ_DEBUG_REPLY, where it is
    // the compositor telling us what its answer IS -- and the reset
    // below ran first, so that handler read zero every time. Two things
    // were silently lost: WIN_DEBUG_F_UNKNOWN, so `gui nosuchthing`
    // printed nothing and looked like a command with no output; and
    // WIN_DEBUG_F_MORE, so g_dbg_reply_ready was set on the FIRST chunk
    // of a multi-chunk reply. The second has never bitten because the
    // compositor sends its chunks back to back without yielding -- it
    // is a race that has not been lost yet, not a race that is absent.
    unsigned in_flags = msg->flags;

    msg->flags = 0;
    msg->reserved = 0;

    if (msg->type == WIN_REQ_DEBUG_MORE) {
        // A stranger asking for more gets the SAME empty final chunk a
        // client one call too late gets -- not an error and not
        // somebody else's bytes, so their loop ends instead of
        // consuming a reply they never asked for.
        if (g_dbg_owner && pid != g_dbg_owner) {
            msg->type = WIN_EV_DEBUG_OUT;
            msg->len = 0;
            msg->text[0] = '\0';
            return 1;
        }
        // Still waiting on the compositor: say so rather than handing
        // back an empty final chunk, which the caller cannot tell from
        // a command that legitimately printed nothing.
        if (g_dbg_awaiting) {
            int n = debug_collect(g_dbg_reply, (int)sizeof g_dbg_reply);
            if (n < 0) {
                if (pit_ticks() >= g_dbg_await_until) {
                    klog_write("win: compositor did not answer a gui command in time\n");
                    g_dbg_awaiting = 0;
                    g_dbg_pending_valid = 0;
                    g_dbg_owner = 0;
                    msg->type = WIN_EV_DEBUG_OUT;
                    msg->len = 0;
                    msg->text[0] = '\0';
                    return 1;
                }
                msg->type = WIN_EV_DEBUG_OUT;
                msg->flags = WIN_DEBUG_F_PENDING;
                msg->len = 0;
                msg->text[0] = '\0';
                return 1;
            }
            g_dbg_awaiting = 0;
            g_dbg_len = n;
            g_dbg_sent = 0;
        }
        // No live reply is not an error -- it is an empty final chunk,
        // so a client that asks one time too many terminates cleanly
        // instead of looping.
        dbg_take_chunk(msg);
        return 1;
    }

    // The compositor fetching the command it was told about.
    if (msg->type == WIN_REQ_DEBUG_TAKE) {
        if (!g_comp_pid || pid != g_comp_pid) return 0;
        if (!g_dbg_pending_valid) { msg->len = 0; msg->text[0] = '\0'; return 0; }
        k_strlcpy(msg->text, g_dbg_pending, WIN_DEBUG_CMD_LEN);
        msg->len = (uint32_t)k_strlen(msg->text);
        // Cleared on TAKE, not on reply: a second TAKE must get nothing
        // rather than run the same command twice.
        g_dbg_pending_valid = 0;
        return 1;
    }

    // ...and answering it.
    if (msg->type == WIN_REQ_DEBUG_REPLY) {
        if (!g_comp_pid || pid != g_comp_pid) return 0;
        // APPENDED, not assigned: one message carries WIN_DEBUG_CHUNK
        // bytes and a reply may be longer, so a compositor sends several
        // with WIN_DEBUG_F_MORE set on every piece but the last. The
        // waiter is only released by that last one -- otherwise the
        // console would print the first 512 bytes of a `gui windows`
        // and call it the whole answer.
        uint32_t n = msg->len;
        uint32_t room = (uint32_t)sizeof g_dbg_ring3 - g_dbg_ring3_len;
        if (n > room) n = room;
        for (uint32_t i = 0; i < n; i++) g_dbg_ring3[g_dbg_ring3_len + i] = msg->text[i];
        g_dbg_ring3_len += n;
        g_dbg_reply_flags |= in_flags;
        if (!(in_flags & WIN_DEBUG_F_MORE)) g_dbg_reply_ready = 1;
        return 1;
    }

    if (msg->type != WIN_REQ_DEBUG_CMD) return 0;

    if (g_dbg_owner && g_dbg_owner != pid && pit_ticks() < g_dbg_owner_until)
        return -EBUSY;
    g_dbg_owner = pid;
    g_dbg_owner_until = pit_ticks() + DBG_OWNER_TICKS;

    g_dbg_len = 0;
    g_dbg_sent = 0;
    g_dbg_reply_flags = 0;   // a previous command's verdict is not this one's

    msg->text[WIN_DEBUG_CMD_LEN - 1] = '\0'; // the command is client data

    // The compositor is asked and waited for. Which of the two branches
    // below applies is about the CALLER's ring, not about the WM.
    int n;
    if (g_comp_pid && pid > 0) {
        // pid > 0 is A PROCESS. The serial console passes
        // WIN_PID_KERNEL (-1) and sys_win_debug() refuses 0, so this is
        // exactly "somebody who can be scheduled" -- tested that way
        // rather than by including the transport's header, which sits
        // ABOVE this file.
        // Post it and hand back PENDING: the wait happens in the
        // caller's own ring. See WIN_DEBUG_F_PENDING.
        if (debug_post_to_compositor(msg->text) < 0) {
            g_dbg_owner = 0;
            msg->type = WIN_EV_DEBUG_OUT;
            msg->len = 0;
            msg->text[0] = '\0';
            return 0;
        }
        g_dbg_awaiting = 1;
        g_dbg_await_until = pit_ticks() + DBG_RING3_TIMEOUT_TICKS;
        msg->type = WIN_EV_DEBUG_OUT;
        msg->flags = WIN_DEBUG_F_PENDING;
        msg->len = 0;
        msg->text[0] = '\0';
        return 1;
    } else if (g_comp_pid) {
        n = debug_via_compositor(msg->text, g_dbg_reply, sizeof g_dbg_reply);
    } else {
        msg->type = WIN_EV_DEBUG_OUT;
        msg->len = 0;
        msg->text[0] = '\0';
        return 0;
    }
    if (n < 0) {
        // Unrecognised, which the caller must be able to tell from a
        // command that legitimately printed nothing.
        msg->type = WIN_EV_DEBUG_OUT;
        msg->flags = WIN_DEBUG_F_UNKNOWN;
        msg->len = 0;
        msg->text[0] = '\0';
        return 1;
    }

    if (n > (int)sizeof g_dbg_reply) n = (int)sizeof g_dbg_reply;
    g_dbg_len = n;
    dbg_take_chunk(msg);
    return 1;
}

int win_server_request(int pid, struct win_request_msg *req) {
    if (!req) return -1;

    // Deliberately ABOVE the "is there a compositor" guard: claiming the
    // role is what a window server does before it is one, so gating it
    // on the role would make it unreachable. See WIN_REQ_SET_COMPOSITOR.
    if (req->type == WIN_REQ_SET_COMPOSITOR) {
        if (req->a) return win_server_set_compositor(pid, vmm_current_pml4());
        // Releasing is only yours to do. Without this any process could
        // evict the compositor and silently take the input stream and
        // every buffer mapping down with it.
        if (pid != g_comp_pid) return 0;
        return win_server_set_compositor(0, 0);
    }

    // The framebuffer grant, gated on the same role and handled beside
    // it.
    //
    // The access control lives HERE rather than in win_surface.c, the
    // same way it lives here for the per-window compositor mappings --
    // one file decides who may act as the compositor.
    if (req->type == WIN_REQ_FB_MAP) {
        if (pid != g_comp_pid || !g_comp_pid) return -1;
        uint32_t w = 0, h = 0, pitch = 0, bpp = 0;
        int count = 1, back = 0;
        if (!win_surface_grant(pid, vmm_current_pml4(), &w, &h, &pitch, &bpp,
                               &count, &back))
            return -1;
        req->a = (int32_t)w;
        req->b = (int32_t)h;
        req->c = (int32_t)pitch;
        req->d = (int32_t)bpp;
        req->mods = (uint32_t)count;
        req->window = (uint32_t)back;
        return 0;
    }
    if (req->type == WIN_REQ_FB_PRESENT) {
        if (pid != g_comp_pid || !g_comp_pid) return -1;
        int back = 0;
        if (!win_surface_present(pid, req->a, req->b, req->c, req->d, &back)) return -1;
        req->window = (uint32_t)back;
        return 0;
    }
    // The hardware cursor plane, same role gate. See win_proto.h for
    // the op encoding; the MOVE half deliberately has no request at
    // all -- win_input.c moves the plane where it already holds the
    // screen coordinates, so pointer motion costs zero syscalls.
    if (req->type == WIN_REQ_FB_CURSOR) {
        if (pid != g_comp_pid || !g_comp_pid) return -1;
        switch (req->a) {
        case WIN_FB_CURSOR_QUERY:
            return gfx_hw_cursor_available() ? 1 : 0;
        case WIN_FB_CURSOR_HIDE:
            g_hw_cursor_armed = 0;
            gfx_hw_cursor_show(0);
            return 0;
        case WIN_FB_CURSOR_SHOW: {
            if (!gfx_hw_cursor_available()) return -1;
            gfx_hw_cursor_show(1);
            // Armed LAST, and the plane is put where the pointer
            // already is -- waiting for the next event would show the
            // sprite at its previous position for one human-visible
            // moment.
            int mx, my;
            mouse_get_state(&mx, &my, 0);
            gfx_hw_cursor_move(mx, my);
            g_hw_cursor_armed = 1;
            return 0;
        }
        case WIN_FB_CURSOR_DEFINE: {
            if (!gfx_hw_cursor_available()) return -1;
            int w     = (int)(((uint32_t)req->d >> 24) & 0xFF);
            int h     = (int)(((uint32_t)req->d >> 16) & 0xFF);
            int hot_x = (int)(((uint32_t)req->d >> 8) & 0xFF);
            int hot_y = (int)((uint32_t)req->d & 0xFF);
            if (w < 1 || h < 1 || w > 64 || h > 64) return -1;
            if (hot_x >= w || hot_y >= h) return -1;
            uint64_t uaddr = (uint32_t)req->b |
                              ((uint64_t)(uint32_t)req->c << 32);
            uint32_t *px = kmalloc((size_t)w * h * 4);
            if (!px) return -1;
            // 1 on success -- vmm.h's convention, not errno's.
            if (!vmm_copy_from_user(vmm_current_pml4(), px, uaddr,
                                     (uint64_t)w * h * 4)) {
                kfree(px);
                return -1;
            }
            int ok = gfx_hw_cursor_define(px, w, h, hot_x, hot_y);
            kfree(px);
            return ok ? 0 : -1;
        }
        default:
            return -1;
        }
    }

    // A window server is a registered ring-3 compositor, and there is no
    // longer a second kind. The refusal is a bare -1 that nothing logs,
    // so the calls below are each guarded on their own rather than
    // leaning on this one.
    if (!g_comp_pid) return -1;

    // **NO REQUEST BELOW NAMES A WINDOW**, and that is stage 6b in one
    // sentence. Create, present, destroy, resize, buffer, title, hints,
    // cursor, timer, pong, close-pid and activate all reach the
    // compositor over its own channel now, carrying their payloads; the
    // kernel has no window table to look one up in and nothing left to
    // check ownership against. What survives here is what the kernel
    // genuinely owns -- a client's event QUEUE, the font it maps, and
    // the framebuffer grant above.

    switch (req->type) {
    case WIN_REQ_EVENT_PUSH: {
        // Only the compositor may put events on ANOTHER process's
        // queue. Without this any client could synthesise a keystroke
        // into any other -- the protocol's whole access-control story is
        // that a window belongs to a process, and this is the one
        // request that reaches ACROSS processes.
        //
        // **A CLIENT MAY POST TO ITSELF**, which is the one exception
        // and is bounded twice: the target must be 0 ("me") and the
        // type must be WIN_EV_USER. A process can already do anything
        // it likes to its own state, so posting itself an event grants
        // nothing new -- what it buys is the ability to WAKE ITSELF,
        // which a worker thread has no other way to do. Restricting the
        // type as well as the target is what keeps "a client cannot
        // synthesise input" true of its own queue too, so a stray post
        // can never be mistaken for a keystroke.
        int self_post = (req->a == 0 && (uint32_t)req->b == WIN_EV_USER);
        if (!self_post && (!g_comp_pid || pid != g_comp_pid)) return -1;
        if (self_post) req->a = pid;
        if (req->a < 1 || req->a > WIN_SERVER_MAX_PIDS) return -1;

        struct win_event ev;
        k_memset(&ev, 0, sizeof ev);
        ev.type = (uint32_t)req->b;
        ev.window = req->window;
        ev.a = req->c;
        ev.b = req->d;
        ev.mods = req->mods;
        return win_events_push(req->a, &ev) ? 0 : -1;
    }
    case WIN_REQ_EVENT_STATS: {
        if (!g_comp_pid || pid != g_comp_pid) return -1;
        // 0 means "me". A compositor has no way to learn its own pid
        // otherwise (there is no getpid), and it is the only pid it can
        // reasonably ask about without being told one.
        int target = req->a ? req->a : pid;
        if (target < 1 || target > WIN_SERVER_MAX_PIDS) return -1;
        req->a = win_events_pending(target);
        req->b = win_events_dropped(target);
        req->c = g_comp_pid;
        return 0;
    }
    // WIN_REQ_BUFFER AND WIN_REQ_WINDOW_APPID ARE RETIRED WITH THE
    // TABLE. The first told the kernel a buffer's new size, which only
    // mattered because a present answered from that record; a present
    // carries its own geometry now. The second read back an app_id the
    // kernel was holding for the compositor, which receives it with the
    // create.
    case WIN_REQ_FONT:
        return map_font(pid, req);
    default:
        return -1;
    }
}

// One event to EVERY windowing client, and to the compositor.
//
// **THE COMPOSITOR FIRST, AND SEPARATELY.** It does not block in
// SYS_WAIT_EVENT -- it parks on its channel's futex, so that it wakes
// for a client's message and a kernel event alike -- which means it is
// not in the set below. A broadcast that only walked that set reached
// every client and missed the one process that draws the chrome, the
// taskbar and the icons, and the font change looked like it was never
// delivered at all. That is the SECOND time this has happened: the
// walk used to be over the window table, which it is also not in.
//
// **WHO ELSE IS A CLIENT IS THE EVENT QUEUE'S ANSWER NOW**
// (win_events_is_client), not the window table's. This never cared
// which WINDOW anything had, only which PROCESS to wake, and a process
// that waits for window events is exactly that.
//
// `window` is 0 on every copy. It used to name each client's window,
// which meant a client with two would get two; nothing that rides this
// (WIN_EV_FONT, WIN_EV_SCREEN) is about a particular window.
void win_server_broadcast(uint32_t type, int32_t a, int32_t b, uint32_t mods) {
    tell_compositor(type, 0, 0, b, mods);

    for (int pid = 1; pid <= WIN_SERVER_MAX_PIDS; pid++) {
        if (pid == g_comp_pid || !win_events_is_client(pid)) continue;
        struct win_event ev;
        k_memset(&ev, 0, sizeof ev);
        ev.type = type;
        ev.a = a;
        ev.b = b;
        ev.mods = mods;
        win_events_push(pid, &ev);
    }
}

// Tells every window the font moved. See WIN_EV_FONT in
// abi/win_proto.h for why this is a notification rather than the server
// doing anything about it, and font_config.c for the one place that
// calls it.
void win_server_font_changed(void) {
    win_server_broadcast(WIN_EV_FONT, 0, 0, 0);
}

// The screen changed size. Same discipline: screen_set_mode() is the
// one caller.
void win_server_screen_changed(int w, int h) {
    win_server_broadcast(WIN_EV_SCREEN, w, h, 0);
}

void win_server_client_gone(int pid) {
    if (pid < 1 || pid > WIN_SERVER_MAX_PIDS) return;

    // The COMPOSITOR dying is not the same event as a client dying, and
    // it has to be handled first: its address space is about to be torn
    // down, so every mapping into it becomes meaningless. Clearing the
    // registration here also drops the per-window flags, so nothing
    // later tries to unmap out of an address space that no longer
    // exists.
    if (pid == g_comp_pid) win_server_set_compositor(0, 0);

    // **NOTHING ELSE TO TEAR DOWN.** A client's windows were entries in
    // a table here; they are the compositor's now, and it learns of the
    // death from its own channel scan -- the client's ring is an shm
    // object the kernel unlinks with every other one this process owned
    // (docs/winserver-ring3-design.md, stage 6b).
}

// --- cross-process buffer sharing (see win_server.h) -----------------

int win_server_compositor_pid(void) { return g_comp_pid; }

// What happens when the desktop goes away (M41's R7).
//
// Reached from ONE place -- the role being cleared below -- so a clean
// deregistration, a `kill`, and the compositor faulting are the same
// path. That is the property this whole stage is built on: the exit
// criterion of the milestone is that killing the WM is SURVIVABLE, and
// three teardown paths that could drift is how one of them ends up not
// being.
//
// **Client windows are ASKED to close, not destroyed.** R7 originally
// said "drops every client window", which is what a reader expects until
// you notice that destroy_window() unmaps and frees the CLIENT's own
// buffer pages -- so a client that happened to be mid-draw would take a
// page fault, and the compositor dying would cascade into every app
// dying with it. That is the opposite of survivable. WIN_EV_CLOSE is
// already the protocol's "the server wants this window gone"; a
// well-behaved client exits and its windows are freed through the
// ordinary win_server_client_gone() path a moment later.
//
// The cost, stated rather than hidden: a client that IGNORES the event
// lingers as a process holding its own buffer, with no window on screen
// and nothing compositing it. That is a leak, not a crash, and it is the
// right way round -- the alternative trades a leak for a fault.
static void compositor_gone(void) {
    // EVERY WINDOWING CLIENT, asked through the event queue that makes
    // it one. The window table used to be this list; a process that
    // waits for window events is the same set without any window state
    // behind it (win_events_is_client).
    //
    // `window` is 0, where it used to name each window. A client with
    // one window -- which is all any of them has -- sees no difference,
    // and Toykit acts on the CLOSE rather than on which window it
    // names.
    int asked = 0;
    for (int pid = 1; pid <= WIN_SERVER_MAX_PIDS; pid++) {
        if (pid == g_comp_pid || !win_events_is_client(pid)) continue;
        struct win_event ev;
        k_memset(&ev, 0, sizeof ev);
        ev.type = WIN_EV_CLOSE;
        win_events_push(pid, &ev);
        asked++;
    }

    // Hand the screen back. Reaching here means nobody else is drawing
    // it (see the guard at the top), so the last frame the dead desktop
    // left is all the user would otherwise have -- indistinguishable
    // from a hang. The console owns its own double buffering, so this
    // repaints rather than inheriting whatever state the compositor left.
    vga_resume();

    klog_printf("win: compositor gone -- %d client window(s) asked to close, "
                "console restored\n", asked);
}

int win_server_set_compositor(int pid, uint64_t pml4) {
    if (pid < 0 || pid > WIN_SERVER_MAX_PIDS) return 0;

    // NO MAPPINGS TO DROP: a compositor's view of a window is its own
    // mmap of the client's object, and it goes when that process's
    // address space does.
    if (g_comp_pid && g_comp_pml4) {
        // The framebuffer grant goes the same way and for the same
        // reason: it belongs to the ROLE, not to the process. This is
        // the one place the role is cleared -- deregistration, a kill
        // and a fault all arrive here -- so it is the one place the
        // grant needs revoking, and there is no second bookkeeping to
        // fall out of step with it.
        win_surface_revoke(g_comp_pid);
        // The plane goes with the grant: a dead compositor must not
        // leave its sprite parked on screen, moving with a pointer
        // nothing is watching.
        if (g_hw_cursor_armed) {
            g_hw_cursor_armed = 0;
            gfx_hw_cursor_show(0);
        }
    }

    // Was the role HELD, and is it being given up entirely? Only that
    // transition is a desktop dying. Handing the role from one
    // compositor to another is not -- the screen keeps an owner, so
    // asking every client to close and repainting the text console over
    // the top of the new desktop would be actively wrong.
    int was_held = (g_comp_pid != 0);

    g_comp_pid = pid;
    g_comp_pml4 = pid ? pml4 : 0;

    // WHO OWNS THE KEYBOARD follows the role, and this is the one place
    // the role changes -- so registering, deregistering, a kill and a
    // fault all arrive here, the same argument the framebuffer revoke
    // above makes. While a compositor holds it, ring 0's BLOCKING
    // readers stop consuming: the physical shell sits at a prompt behind
    // the desktop and was draining the same ring, so keys typed at the
    // desktop were being run by an invisible shell (see keyboard.h).
    keyboard_suspend_blocking(pid != 0);

    // AFTER the registration is cleared, not before: compositor_gone()
    // pushes events and repaints, and anything it reaches must already
    // see "there is no compositor" rather than a half-cleared one.
    if (was_held && pid == 0) compositor_gone();
    return 1;
}

// **THE KTEST STAND-INS ARE GONE WITH THE TABLE.** They existed so a
// kernel test could make a window with no client behind it; there is no
// window in ring 0 to make. What they covered -- slot allocation, the
// buffer flip, a resize touching the back buffer only, a dead client's
// windows closing -- is now the compositor's, and is covered from ring
// 3 by tools/single_instance_test.py, tools/uapp_test.py,
// tools/winclient_test.py and tools/resize_stride_test.py.
