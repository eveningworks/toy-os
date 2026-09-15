// THE COMPOSITOR ROLE -- DRM master's shape: which one process owns the
// display. Everything ring 0 keeps for the window system keys off it:
// the framebuffer grant (win_surface.c), the input queue (win_input.c),
// the font mapped into a client, and the `gui` diagnostic channel. No
// window, no client state: a window is the compositor's
// (userland/wm/wm_client.c) and a client's events ride its own channel
// ring (lib/uwmchan.h). abi/win_proto.h is TWP, the protocol.
#include "win_role.h"
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
#include "win_input.h"   // the compositor's queue -- tell_compositor(), and reset with the role
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
// **THERE IS NO WINDOW TABLE AND NO CLIENT TABLE.** A window's slot,
// buffers and geometry are the compositor's (stage 6b); a client's
// events are written by the compositor into the client's own ring
// (stage 8). What is left is what one process must be granted by the
// kernel: the role, the screen, the devices, the font.

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
    // **THIS HANDS OUT THE BAKED TABLES, and that is all it can.** A
    // runtime face is /bin/fontd's now, published as shared memory a
    // client maps itself (abi/font_shm.h) -- ring 0 parses no font. What
    // is left here is the FALLBACK a client uses before fontd has
    // published, so a window that opens early still draws text; it
    // upgrades itself when the atlas appears (ugfx_font_recheck).
    const struct font_atlas *atlas = 0;
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
            klog_write(KLOG_ERR "win_server: font refused -- mapping failed\n");
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
    win_input_push(&ev);
}

// **QUERY_WINDOWS IS RETIRED WITH THE TABLE.** It reported what
// win_server.c believed about each window, beside `guictl windows`'s
// report of what the compositor believed -- a pair that made a
// disagreement visible instead of inferred. There is nothing left to
// disagree: one process holds the list, and `guictl windows` is it.

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
        // The compositor, or the one client it leased the grant to;
        // win_surface_present() refuses whichever of the two is not
        // presenting right now.
        if (!g_comp_pid || (pid != g_comp_pid && pid != win_surface_lessee())) return -1;
        int back = 0;
        if (!win_surface_present(pid, req->a, req->b, req->c, req->d, &back)) return -1;
        req->window = (uint32_t)back;
        return 0;
    }
    if (req->type == WIN_REQ_FB_LEASE) {
        if (pid != g_comp_pid || !g_comp_pid) return -1;
        int back = 0;
        if (req->a == 0) {
            win_surface_lease_end(&back);
            req->window = (uint32_t)back;
            return 0;
        }
        int lessee = req->a;
        if (req->b == 1) { win_surface_lease_unmap(lessee); return 0; }
        uint64_t pml4 = scheduler_pid_pml4(lessee);
        if (lessee == pid || !pml4) return -1;
        uint32_t w = 0, h = 0, pitch = 0, bpp = 0;
        int count = 1;
        if (!win_surface_lease(lessee, pml4, &w, &h, &pitch, &bpp, &count, &back)) return -1;
        req->a = (int32_t)w;
        req->b = (int32_t)h;
        req->c = (int32_t)pitch;
        req->d = (int32_t)bpp;
        req->mods = (uint32_t)count;
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

    // **NO REQUEST BELOW NAMES A WINDOW OR A CLIENT.** Every request a
    // client makes reaches the compositor over its own channel, and
    // every event the compositor sends comes back the same way. What
    // survives here is what the kernel genuinely owns: the compositor's
    // input queue, the font it maps, and the framebuffer grant above.

    switch (req->type) {
    case WIN_REQ_EVENT_STATS: {
        // The compositor's OWN queue -- the only one there is.
        if (!g_comp_pid || pid != g_comp_pid) return -1;
        req->a = win_input_pending();
        req->b = win_input_dropped();
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

// A session fact changed. THE COMPOSITOR IS TOLD, AND IT TELLS ITS
// CLIENTS -- the kernel has no list of them, the same way a DRM
// hotplug uevent reaches the compositor and not every X client.
static void broadcast(uint32_t type, int32_t a, int32_t b) {
    if (!g_comp_pid) return;
    struct win_event ev;
    k_memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.a = a;
    ev.b = b;
    win_input_push(&ev);
}

// The font moved. See WIN_EV_FONT in abi/win_proto.h for why this is a
// notification rather than the kernel doing anything about it;
// font_config.c is the one caller.
void win_server_font_changed(void) { broadcast(WIN_EV_FONT, 0, 0); }

// The screen changed size. screen_set_mode() is the one caller.
void win_server_screen_changed(int w, int h) { broadcast(WIN_EV_SCREEN, w, h); }

void win_server_client_gone(int pid) {
    if (pid < 1 || pid > WIN_SERVER_MAX_PIDS) return;

    // The COMPOSITOR dying is not the same event as a client dying, and
    // it has to be handled first: its address space is about to be torn
    // down, so every mapping into it becomes meaningless. Clearing the
    // registration here also drops the per-window flags, so nothing
    // later tries to unmap out of an address space that no longer
    // exists.
    if (pid == g_comp_pid) win_server_set_compositor(0, 0);
    win_surface_client_gone(pid);

    // **NOTHING ELSE TO TEAR DOWN.** A client's windows were entries in
    // a table here; they are the compositor's now, and it learns of the
    // death from its own channel scan -- the client's ring is an shm
    // object the kernel unlinks with every other one this process owned
    // (docs/winserver-ring3-design.md, stage 6b).
}

// --- cross-process buffer sharing (see win_role.h) --------------------

int win_server_compositor_pid(void) { return g_comp_pid; }

// What happens when the desktop goes away.
//
// Reached from ONE place -- the role being cleared below -- so a clean
// deregistration, a `kill`, and the compositor faulting are the same
// path. Its clients are NOT told from here: a client parked on its own
// ring notices the compositor's beacon has gone (uchan_client_server_
// alive) and closes itself, the way a Wayland client sees its socket
// close. The kernel keeps no list to tell.
static void compositor_gone(void) {
    // Hand the screen back. Reaching here means nobody else is drawing
    // it, so the last frame the dead desktop left is all the user would
    // otherwise have -- indistinguishable from a hang. The console owns
    // its own double buffering, so this repaints rather than inheriting
    // whatever state the compositor left.
    vga_resume();
    klog_write("win: compositor gone -- console restored\n");
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
    // The queue belongs to the ROLE: a successor must not inherit a
    // predecessor's keystrokes, or a `gui` command meant for it.
    win_input_reset();

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
