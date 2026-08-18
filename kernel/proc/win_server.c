// Client windows, kernel side: ids, pixel buffers, per-process
// mappings, ownership and teardown. See win_server.h for why the split
// between this and the registered presentation layer falls where it
// does.
// **TWS -- the Toy Window Server**, memory half. See
// abi/win_proto.h for TWP, the protocol it serves, and
// userland/wm/wm_client.c for the presentation half.
#include "win_server.h"
#include "vmm.h"
#include "pmm.h"
#include "klog.h"
#include "string.h"
#include "font_ttf.h" // the glyph tables WIN_REQ_FONT shares out
#include "gfx.h"      // gfx_font_size() -- which variant is active
#include "win_surface.h" // the compositor's framebuffer grant (M41 stage 4a)
#include "timer.h"       // pit_ticks() -- the ring-3 debug leg's deadline
#include "win_events.h"  // WIN_EV_CLOSE to clients when the desktop dies (R7)
#include "vga.h"         // vga_resume() -- hand the screen back (R7)
#include "kfmt.h"        // klog_printf
#include <stddef.h>

#include "scheduler.h" // SCHED_MAX_PROCS -- this table is per process

#define WIN_SERVER_MAX_PIDS SCHED_MAX_PROCS

// The compositor's address region is carved per (pid, window) and its
// pid count is an ABI constant (abi/win_proto.h), so it cannot include
// the scheduler's header. Checked here instead of trusted: a pid past
// the region's end would compute an address overlapping another
// process's window.
_Static_assert(WIN_COMPOSITOR_MAX_PIDS >= SCHED_MAX_PROCS,
               "win_compositor_vaddr() has no room for every process");

struct client_window {
    int used;
    int pid;
    uint32_t id;      // index into the owner's slots, so
                       // win_buffer_vaddr(id) is stable per window
    uint64_t pml4;    // the owner's address space, needed to unmap
    uint32_t *buf;    // kernel-visible (identity-mapped) pixels
    uint64_t vaddr;   // where the client sees it
    uint32_t pages;   // how many frames `buf` spans
    int w, h;
    // Is this window's buffer currently mapped into the compositor's
    // address space? Tracked per window rather than inferred, because
    // the ONLY safe moment to revoke is inside the operation that frees
    // or replaces the frames -- and that operation has a window, not a
    // list of mappings.
    int comp_mapped;

    // How many pages of this window's compositor mapping currently hold
    // the POISON page (see comp_poison()). Non-zero only while the real
    // frames are gone but the compositor may still blit the slot; the
    // count is the OLD window's page span, which is what has to be
    // unmapped again before the slot can be mapped for real.
    uint32_t comp_poisoned;

    // What the kernel used to receive and throw away, passing it
    // straight through to a ring-0 WM. A ring-3 one is TOLD a window
    // changed and reads the detail back (WIN_REQ_WINDOW_INFO), so the
    // detail has to live somewhere -- and the kernel is where it already
    // arrives. Holding it also lets the kernel answer the one question
    // that used to need a round trip into the WM: does a window with
    // this app_id exist? (WIN_REQ_ACTIVATE.)
    char title[WIN_TITLE_LEN];
    char app_id[WIN_APP_ID_LEN];
    unsigned hint_flags;
    int min_w, min_h;
};

// The registered compositor: which process may map other processes'
// windows, and where those mappings go. See win_server.h -- the address
// space is captured here rather than looked up per call.
static int g_comp_pid = 0;
static uint64_t g_comp_pml4 = 0;

// [pid - 1][window id]. A flat table rather than a list: WIN_CLIENT_MAX
// windows across MAX_PROCS processes is 16 entries, and a fixed table
// makes "is this window really that client's?" a bounds check instead
// of a walk -- which matters, because that question is the entire
// access-control story for this protocol.
static struct client_window windows[WIN_SERVER_MAX_PIDS][WIN_CLIENT_MAX];

static const struct win_server_ops *g_ops = NULL;

void win_server_register(const struct win_server_ops *ops) {
    g_ops = ops;
}

const struct win_server_ops *win_server_ops_current(void) {
    return g_ops;
}

int win_server_active(void) {
    return g_ops != NULL;
}

// Tells the registered compositor that a client did something.
//
// A no-op when nothing has registered, which is the ring-0 desktop's
// whole lifetime today -- exactly as the win_server_ops calls beside it
// are skipped when `g_ops` is NULL. So both halves of the inversion can
// be live at once during the migration without either disturbing the
// other, which is the property every stage of this plan has been shaped
// to keep.
//
// Fire and forget: no reply, no blocking. The one caller that needed an
// ANSWER (activate) is answered by the kernel itself now -- see
// WIN_REQ_ACTIVATE.
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

// The one place that answers "does `pid` own `id`?". Everything that
// acts on a client-named window goes through this, so a client cannot
// present, retitle or destroy a window belonging to another process by
// guessing an id.
static struct client_window *lookup(int pid, uint32_t id) {
    if (pid < 1 || pid > WIN_SERVER_MAX_PIDS) return NULL;
    if (id >= WIN_CLIENT_MAX) return NULL;
    struct client_window *cw = &windows[pid - 1][id];
    if (!cw->used || cw->pid != pid) return NULL;
    return cw;
}

// One shared, zero-filled frame, mapped READ-ONLY wherever a
// compositor mapping has to survive the frames behind it going away.
// Allocated on first use and never freed -- there is exactly one, and
// the whole point is that it is always available at the moment a window
// dies.
static uint64_t g_poison_frame;

static uint64_t poison_frame(void) {
    if (!g_poison_frame) {
        uint64_t f = pmm_alloc_frame();
        if (!f) return 0;
        k_memset((void *)(uintptr_t)f, 0, 4096);
        g_poison_frame = f;
    }
    return g_poison_frame;
}

// Takes a poisoned slot's zero-page mappings back out. Must run before
// anything maps real frames there, and before the compositor's address
// space goes away.
static void comp_unpoison(struct client_window *cw) {
    if (!cw->comp_poisoned || !g_comp_pml4) { cw->comp_poisoned = 0; return; }
    uint64_t vaddr = win_compositor_vaddr(cw->pid, cw->id);
    for (uint32_t i = 0; i < cw->comp_poisoned; i++) {
        vmm_unmap_user_page(g_comp_pml4, vaddr + (uint64_t)i * 4096);
    }
    cw->comp_poisoned = 0;
}

// Maps a window's frames into the compositor at its derived address.
// Returns 1 on success (including "already mapped"), 0 if the mapping
// could not be built -- in which case nothing is left half-mapped.
static int comp_map(struct client_window *cw) {
    if (!g_comp_pid || !g_comp_pml4) return 0;
    uint64_t vaddr = win_compositor_vaddr(cw->pid, cw->id);
    // A poisoned slot has PRESENT page-table entries pointing at the
    // zero page, and mapping over a present entry does NOT invalidate
    // the TLB (vmm_map_user_page_type() only counts it) -- so the
    // compositor would go on reading zeros from a live window. Unmap
    // first, which does invalidate.
    comp_unpoison(cw);
    for (uint32_t i = 0; i < cw->pages; i++) {
        uint64_t phys = (uint64_t)(uintptr_t)cw->buf + (uint64_t)i * 4096;
        if (!vmm_map_user_page(g_comp_pml4, vaddr + (uint64_t)i * 4096, phys)) {
            for (uint32_t j = 0; j < i; j++) {
                vmm_unmap_user_page(g_comp_pml4, vaddr + (uint64_t)j * 4096);
            }
            klog_write("win_server: compositor mapping failed\n");
            return 0;
        }
    }
    cw->comp_mapped = 1;
    return 1;
}

// Revokes it. Safe to call unconditionally; that is the point, since
// every path that frees or replaces frames has to call it and none of
// them should have to know whether a mapping exists.
static void comp_unmap(struct client_window *cw) {
    if (!cw->comp_mapped) return;
    uint64_t vaddr = win_compositor_vaddr(cw->pid, cw->id);
    for (uint32_t i = 0; i < cw->pages; i++) {
        vmm_unmap_user_page(g_comp_pml4, vaddr + (uint64_t)i * 4096);
    }
    cw->comp_mapped = 0;
}

// THE INVARIANT: while a compositor is registered, a window buffer's
// slot in its address space is never a HOLE. Frames that go away are
// replaced by the read-only zero page rather than unmapped, because the
// compositor is a process and cannot be stopped mid-frame: it learns a
// window died from a QUEUED event (WIN_EV_CLIENT_DESTROYED), and until
// it drains that event its window list still names the slot. A hole
// there is a page fault in the compositor, i.e. the desktop dying --
// which is exactly what Force Quit did, since a ring-3 WM triggers the
// teardown from inside its own sys_kill() and blits the dead window on
// the very next frame.
//
// Poison reads as black for at most one frame, and the frames really
// are freed, so this is not a use-after-free -- the alternative that
// keeps the mapping live IS one.
//
// Read-only on purpose: a compositor composites OUT of a client buffer
// and never writes one, so a write here is a bug worth faulting on
// rather than silently absorbing into a page every dead window shares.
static void comp_poison(struct client_window *cw) {
    if (!cw->comp_mapped) return;
    uint32_t pages = cw->pages;
    comp_unmap(cw);                    // also invalidates the TLB
    uint64_t phys = poison_frame();
    if (!phys) return;                 // out of memory: a hole, as before
    uint64_t vaddr = win_compositor_vaddr(cw->pid, cw->id);
    for (uint32_t i = 0; i < pages; i++) {
        if (!vmm_map_user_page_flags(g_comp_pml4, vaddr + (uint64_t)i * 4096,
                                     phys, 0, 0)) {
            break;
        }
        cw->comp_poisoned = i + 1;
    }
}

// Frees a window's frames and unmaps them from its owner's address
// space. The presentation layer is told FIRST (while `buf` is still
// valid), so it can drop the window from its list before the memory
// behind it goes away.
static void destroy_window(struct client_window *cw) {
    if (!cw->used) return;

    if (g_ops && g_ops->window_destroyed) g_ops->window_destroyed(cw->pid, cw->id);
    // AFTER the ring-0 callback, and note the asymmetry documented in
    // win_proto.h: that callback runs while `buf` is still valid, and
    // this event is only queued -- by the time the compositor reads it
    // the buffer is gone. There is no way to hold a ring-3 process
    // inside a kernel teardown, so the protocol says "already freed"
    // rather than pretending otherwise.
    tell_compositor(WIN_EV_CLIENT_DESTROYED, cw->pid, cw->id, 0, 0);

    // Before the frames go back to the allocator. A compositor left
    // holding a mapping of freed frames reads whatever is allocated
    // there next, which looks like a drawing bug rather than a
    // use-after-free -- see win_server.h. Poisoned rather than unmapped
    // because the compositor may blit this slot again before it drains
    // the event above: see comp_poison().
    comp_poison(cw);

    for (uint32_t i = 0; i < cw->pages; i++) {
        vmm_unmap_user_page(cw->pml4, cw->vaddr + (uint64_t)i * 4096);
    }
    // pmm_free_contiguous(), not a pmm_free_frame() loop -- the buffer
    // came from pmm_alloc_contiguous() and the two allocators are not
    // interchangeable (see api/pmm.h).
    pmm_free_contiguous((uint64_t)(uintptr_t)cw->buf, cw->pages);
    cw->used = 0;
    cw->buf = NULL;
    cw->pages = 0;
}

// `pml4` is passed in rather than read from vmm_current_pml4() here.
// The protocol path passes the caller's own address space (a syscall
// does not switch CR3, so that is the client's); win_server_create_raw()
// passes one a KTEST made. Making it a parameter is also the honest
// shape: this function maps into an address space, and which one should
// not depend on when it happens to be called.
static int create_window(int pid, uint64_t pml4, int w, int h, int x, int y,
                          const char *app_id, uint32_t *out_id) {
    if (pid < 1 || pid > WIN_SERVER_MAX_PIDS) return 0;

    if (w <= 0 || h <= 0 || w > WIN_CLIENT_MAX_W || h > WIN_CLIENT_MAX_H) {
        klog_write("win_server: create refused -- bad size\n");
        return 0;
    }

    int slot = -1;
    for (int i = 0; i < WIN_CLIENT_MAX; i++) {
        if (!windows[pid - 1][i].used) { slot = i; break; }
    }
    if (slot < 0) {
        klog_write("win_server: create refused -- client already holds WIN_CLIENT_MAX windows\n");
        return 0;
    }

    struct client_window *cw = &windows[pid - 1][slot];
    uint64_t vaddr = win_buffer_vaddr((uint32_t)slot);
    uint32_t bytes = (uint32_t)w * (uint32_t)h * 4;
    uint32_t pages = (bytes + 4095) / 4096;

    // Contiguous frames, so the kernel-visible pointer can be a plain
    // uint32_t* over the whole buffer instead of a per-page walk on
    // every composite. pmm_alloc_contiguous() already exists for the
    // same reason drivers need it.
    uint64_t phys = pmm_alloc_contiguous(pages);
    if (!phys) {
        klog_write("win_server: create refused -- out of contiguous memory\n");
        return 0;
    }

    // Zero it before the client ever sees it: a fresh window must not
    // show whatever the previous owner of these frames left behind.
    // k_memset() rather than the byte loop this used to be -- a
    // full-screen window is 3.5 MiB, and the same loop runs again on
    // every resize.
    k_memset((void *)(uintptr_t)phys, 0, (size_t)pages * 4096);

    for (uint32_t i = 0; i < pages; i++) {
        if (!vmm_map_user_page(pml4, vaddr + (uint64_t)i * 4096, phys + (uint64_t)i * 4096)) {
            // Unwind the pages already mapped, then the frames.
            for (uint32_t j = 0; j < i; j++) vmm_unmap_user_page(pml4, vaddr + (uint64_t)j * 4096);
            pmm_free_contiguous(phys, pages);
            klog_write("win_server: create refused -- mapping failed\n");
            return 0;
        }
    }

    cw->used = 1;
    cw->pid = pid;
    cw->id = (uint32_t)slot;
    cw->pml4 = pml4;
    cw->buf = (uint32_t *)(uintptr_t)phys;
    cw->vaddr = vaddr;
    cw->pages = pages;
    cw->w = w;
    cw->h = h;
    cw->comp_mapped = 0;

    // The presentation layer gets the last word: if it has no room in
    // its window list, the whole create fails and the memory goes back
    // rather than leaving a buffer nothing will ever draw.
    // Everything the kernel was handed and used to forward without
    // keeping. WIN_REQ_WINDOW_INFO reads it back.
    k_strlcpy(cw->title, "", sizeof cw->title);
    k_strlcpy(cw->app_id, app_id ? app_id : "", sizeof cw->app_id);
    cw->hint_flags = 0;
    cw->min_w = 0;
    cw->min_h = 0;

    tell_compositor(WIN_EV_CLIENT_CREATED, pid, cw->id, w, (uint32_t)h);

    if (g_ops && g_ops->window_created) {
        if (!g_ops->window_created(pid, cw->id, cw->buf, w, h, x, y, app_id)) {
            // Not destroy_window() -- that would call window_destroyed()
            // for a window the presentation layer just refused and never
            // recorded.
            for (uint32_t i = 0; i < pages; i++) {
                vmm_unmap_user_page(pml4, vaddr + (uint64_t)i * 4096);
            }
            pmm_free_contiguous(phys, pages);
            cw->used = 0;
            klog_write("win_server: create refused -- no room in the window list\n");
            return 0;
        }
    }

    *out_id = cw->id;
    return 1;
}

// Maps the desktop's active font read-only into the caller and reports
// its metrics. See WIN_REQ_FONT in abi/win_proto.h for why the server
// hands the font over rather than every client carrying a copy.
//
// Idempotent by construction: re-mapping the same pages over an
// existing identical mapping is a no-op in effect, so a client may ask
// again (after a font-size change, say) without unmapping first.
static int map_font(int pid, struct win_request_msg *req) {
    (void)pid;

    const struct font_ttf_variant *fv = &font_ttf_variants[gfx_font_size()];
    uint64_t phys = (uint64_t)(uintptr_t)fv->glyphs;
    uint64_t bytes = (uint64_t)FONT_TTF_GLYPH_COUNT * (uint64_t)fv->w * (uint64_t)fv->h;

    // The glyph tables are ordinary kernel .rodata, which this kernel
    // identity-maps -- so their physical address IS the pointer we
    // already hold, and mapping them to a user vaddr is just pointing
    // more PTEs at the same frames. No copy, one instance in memory
    // however many clients ask.
    uint64_t page_base = phys & ~0xFFFULL;
    uint64_t offset_in_page = phys - page_base;
    uint64_t pages = (offset_in_page + bytes + 4095) / 4096;

    uint64_t pml4 = vmm_current_pml4();
    for (uint64_t i = 0; i < pages; i++) {
        // writable = 0: these are pages of the kernel image, and a
        // writable mapping would let any client scribble on kernel
        // .rodata. executable = 0 for the same no-surprises reason
        // every other user mapping here is NX.
        if (!vmm_map_user_page_flags(pml4, WIN_FONT_VADDR + i * 4096,
                                      page_base + i * 4096, 0, 0)) {
            for (uint64_t j = 0; j < i; j++) {
                vmm_unmap_user_page(pml4, WIN_FONT_VADDR + j * 4096);
            }
            klog_write("win_server: font refused -- mapping failed\n");
            return 0;
        }
    }

    // The client sees the mapping at WIN_FONT_VADDR + the same offset
    // the data has within its first page, so glyph 0 starts exactly
    // there. Reported as `d`'s companion rather than assumed.
    req->a = fv->w;
    req->b = fv->h;
    req->c = FONT_TTF_GLYPH_COUNT;
    req->d = (int32_t)offset_in_page;
    return 1;
}

// Reallocate a window's buffer, mapped AT THE SAME VIRTUAL ADDRESS.
//
// That last part is what makes a client-driven resize simple rather
// than a lifetime problem: win_buffer_vaddr() derives the address from
// the window id, so the client's pointer is unchanged and it never has
// to be told where its pixels moved. The fixed-vaddr decision was made
// for a different reason (a client can compute its own buffer address)
// and pays off here.
//
// Ordering is chosen so a FAILURE leaves the window exactly as it was:
// the new frames are allocated and zeroed BEFORE anything is unmapped,
// so running out of memory means the old buffer is still mapped and
// still correct. A refusal is a normal outcome of this call, not an
// error path -- see abi/win_proto.h.
static int resize_window(struct client_window *cw, int w, int h) {
    if (w <= 0 || h <= 0 || w > WIN_CLIENT_MAX_W || h > WIN_CLIENT_MAX_H) return 0;

    uint32_t bytes = (uint32_t)w * (uint32_t)h * 4;
    uint32_t pages = (bytes + 4095) / 4096;

    uint64_t phys = pmm_alloc_contiguous(pages);
    if (!phys) {
        klog_write("win_server: resize refused -- out of contiguous memory\n");
        return 0;
    }
    k_memset((void *)(uintptr_t)phys, 0, (size_t)pages * 4096);

    // Only now is the old mapping disturbed.
    for (uint32_t i = 0; i < cw->pages; i++) {
        vmm_unmap_user_page(cw->pml4, cw->vaddr + (uint64_t)i * 4096);
    }
    for (uint32_t i = 0; i < pages; i++) {
        if (!vmm_map_user_page(cw->pml4, cw->vaddr + (uint64_t)i * 4096,
                                phys + (uint64_t)i * 4096)) {
            // Half-mapped and the old frames are already unmapped: put
            // the ORIGINAL buffer back rather than leaving the client
            // with an address that faults. The old frames are still
            // allocated -- nothing has freed them yet -- so this can
            // always succeed with the memory it had a moment ago.
            for (uint32_t j = 0; j < i; j++) {
                vmm_unmap_user_page(cw->pml4, cw->vaddr + (uint64_t)j * 4096);
            }
            for (uint32_t j = 0; j < cw->pages; j++) {
                vmm_map_user_page(cw->pml4, cw->vaddr + (uint64_t)j * 4096,
                                   (uint64_t)(uintptr_t)cw->buf + (uint64_t)j * 4096);
            }
            pmm_free_contiguous(phys, pages);
            klog_write("win_server: resize refused -- mapping failed\n");
            return 0;
        }
    }

    // The compositor's mapping still points at the OLD frames, which are
    // about to be freed. Revoke before the free, then rebuild against
    // the new ones -- at the same virtual address, so the compositor's
    // pointer is unchanged and it need not be told anything (see
    // win_compositor_vaddr()).
    //
    // Order matters twice over: the unmap has to happen before
    // pmm_free_contiguous(), and the remap has to use the new frames,
    // so this cannot be collapsed into one call either side of the
    // free. If the remap fails, the window is left UNMAPPED rather than
    // stale -- a compositor that finds its mapping gone can ask again,
    // whereas one reading freed frames cannot tell anything is wrong.
    int was_comp_mapped = cw->comp_mapped;
    comp_poison(cw);   // never a hole while the window lives -- see comp_poison()

    pmm_free_contiguous((uint64_t)(uintptr_t)cw->buf, cw->pages);
    cw->buf = (uint32_t *)(uintptr_t)phys;
    cw->pages = pages;
    cw->w = w;
    cw->h = h;

    if (was_comp_mapped) comp_map(cw);

    tell_compositor(WIN_EV_CLIENT_RESIZED, cw->pid, cw->id, w, (uint32_t)h);
    if (g_ops && g_ops->window_resized) {
        g_ops->window_resized(cw->pid, cw->id, cw->buf, w, h);
    }
    return 1;
}

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
}

// --- the ring-3 debug leg (M41) --------------------------------------
//
// The command waiting for a ring-3 compositor to run it, and the reply
// coming back. One slot: `gui` commands are issued one at a time by a
// console that blocks on each, so a queue would be state with no second
// user.
static char g_dbg_pending[WIN_DEBUG_CMD_LEN];
static int g_dbg_pending_valid;

// Where a ring-3 compositor's answer lands before it is handed to the
// waiting caller. Separate from g_dbg_reply, which is the CHUNKING
// buffer the console reads out of -- writing straight into that would
// mean the compositor's reply racing the chunk being sent.
static char g_dbg_ring3[WIN_DEBUG_REPLY_MAX];
static uint32_t g_dbg_ring3_len;
static int g_dbg_reply_ready;
static unsigned g_dbg_reply_flags;

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
static int debug_via_compositor(const char *line, char *out, int cap) {
    if (!g_comp_pid) return -1;

    k_strlcpy(g_dbg_pending, line, sizeof g_dbg_pending);
    g_dbg_pending_valid = 1;
    g_dbg_reply_ready = 0;
    g_dbg_reply_flags = 0;
    g_dbg_ring3_len = 0;

    tell_compositor(WIN_EV_CLIENT_DEBUG, g_comp_pid, 0, 0, 0);

    uint64_t deadline = pit_ticks() + DBG_RING3_TIMEOUT_TICKS;
    while (!g_dbg_reply_ready && pit_ticks() < deadline) {
        __asm__ volatile ("sti; hlt");
    }

    g_dbg_pending_valid = 0;
    if (!g_dbg_reply_ready) {
        klog_write("win: compositor did not answer a gui command in time\n");
        return 0; // empty, and deliberately NOT "unknown" -- see above
    }

    int n = (int)g_dbg_ring3_len;
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) out[i] = g_dbg_ring3[i];
    return n;
}

int win_server_debug(int pid, struct win_debug_msg *msg) {
    (void)pid; // the console is the only client; kept for the ops shape
    if (!msg) return 0;

    msg->flags = 0;
    msg->reserved = 0;

    if (msg->type == WIN_REQ_DEBUG_MORE) {
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
        g_dbg_reply_flags |= msg->flags;
        if (!(msg->flags & WIN_DEBUG_F_MORE)) g_dbg_reply_ready = 1;
        return 1;
    }

    if (msg->type != WIN_REQ_DEBUG_CMD) return 0;

    g_dbg_len = 0;
    g_dbg_sent = 0;

    msg->text[WIN_DEBUG_CMD_LEN - 1] = '\0'; // the command is client data

    // A ring-0 WM answers directly; a ring-3 one is asked and waited for.
    // Both are live during the migration, and the ring-0 one wins while
    // it exists -- it is still the desktop being driven.
    int n;
    if (g_ops && g_ops->debug_command) {
        n = g_ops->debug_command(msg->text, g_dbg_reply, sizeof g_dbg_reply);
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

// Copy one of `text`'s NUL-terminated strings out of a request into a
// kernel buffer of `cap` bytes, truncating to fit. `req->text` is a
// fixed array with no guarantee of a terminator, so this bounds on BOTH
// ends -- a client that fills all 32 bytes with no NUL gets a truncated
// string rather than a read past the field.
static void copy_text(char *dst, const char *src, int cap) {
    int i = 0;
    for (; i < cap - 1 && i < WIN_TITLE_LEN && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

int win_server_request(int pid, struct win_request_msg *req) {
    if (!req) return -1;

    // Deliberately ABOVE the !g_ops guard: claiming the compositor role
    // must not require a registered presentation layer, because in
    // stage 4 the ring-3 WM is both and there is no kernel-side one left
    // to register first. Gating it here would make it unreachable at
    // exactly the point of the milestone. See WIN_REQ_SET_COMPOSITOR.
    if (req->type == WIN_REQ_SET_COMPOSITOR) {
        if (req->a) return win_server_set_compositor(pid, vmm_current_pml4());
        // Releasing is only yours to do. Without this any process could
        // evict the compositor and silently take the input stream and
        // every buffer mapping down with it.
        if (pid != g_comp_pid) return 0;
        return win_server_set_compositor(0, 0);
    }

    // The framebuffer grant, gated on the same role and handled beside
    // it -- also above the !g_ops guard, for the reason given there: in
    // stage 4 the ring-3 WM IS the presentation layer, so requiring one
    // to already be registered would make the grant unreachable at
    // exactly the point of the milestone.
    //
    // The access control lives HERE rather than in win_surface.c, the
    // same way it lives here for the per-window compositor mappings --
    // one file decides who may act as the compositor.
    if (req->type == WIN_REQ_FB_MAP) {
        if (pid != g_comp_pid || !g_comp_pid) return -1;
        uint32_t w = 0, h = 0, pitch = 0, bpp = 0;
        if (!win_surface_grant(pid, vmm_current_pml4(), &w, &h, &pitch, &bpp))
            return -1;
        req->a = (int32_t)w;
        req->b = (int32_t)h;
        req->c = (int32_t)pitch;
        req->d = (int32_t)bpp;
        return 0;
    }
    if (req->type == WIN_REQ_FB_PRESENT) {
        if (pid != g_comp_pid || !g_comp_pid) return -1;
        return win_surface_present(pid, req->a, req->b, req->c, req->d) ? 0 : -1;
    }

    // A window server is EITHER a registered ring-0 presentation layer
    // or a registered ring-3 compositor. This used to demand the first,
    // which refused every request below the moment the WM moved out of
    // the kernel -- the font (so the desktop drew no text, and every
    // font-derived measurement collapsed with it) and window creation
    // (so no client could ever get a window). Silently, because the
    // refusal is a bare -1 that nothing logs.
    //
    // Everything past here therefore has to tolerate g_ops being NULL;
    // the calls below are all guarded individually rather than by this
    // one, which is what that used to buy.
    if (!g_ops && !g_comp_pid) return -1;

    // Not addressed to one of the CALLER's own windows -- it names
    // another process entirely, so it skips the lookup() ownership
    // check every other request below goes through. That is the whole
    // point of it (a task manager acts on other processes) and is why
    // the header documents it as unprivileged rather than leaving the
    // missing check to be discovered.
    if (req->type == WIN_REQ_CLOSE_PID) {
        // The compositor is TOLD; the kernel answers from its own
        // table, because "does that pid own any windows" is a fact it
        // already holds and does not need to ask for.
        int owned = 0;
        if (req->a >= 1 && req->a <= WIN_SERVER_MAX_PIDS) {
            for (int i = 0; i < WIN_CLIENT_MAX; i++) {
                if (windows[req->a - 1][i].used) owned++;
            }
        }
        if (owned) tell_compositor(WIN_EV_CLIENT_CLOSE, req->a, 0, 0, 0);
        if (g_ops && g_ops->close_pid) return g_ops->close_pid(req->a) > 0 ? 1 : 0;
        return owned > 0;
    }

    // Also not addressed to one of the caller's own windows -- it names
    // an app id, and the window carrying it belongs to somebody else by
    // definition (a client asking about its OWN window learns nothing).
    // Same unprivileged reasoning as CLOSE_PID above, and weaker still:
    // the worst outcome is raising a window the user can already see.
    if (req->type == WIN_REQ_ACTIVATE) {
        char app_id[WIN_APP_ID_LEN];
        copy_text(app_id, req->text, WIN_APP_ID_LEN);
        // An empty id matches nothing, rather than matching every
        // window that never set one. Without this, one app opting in
        // would start raising unrelated windows.
        if (!app_id[0]) return 0;

        // **THE KERNEL ANSWERS THIS ONE ITSELF**, and that is what
        // removes the last thing needing a round trip into ring 3.
        //
        // The answer is load-bearing: a second copy of a single-instance
        // app exits 0 only if told its twin was raised, so getting a
        // "no" wrong opens a duplicate window and getting a "yes" wrong
        // makes the app vanish. In ring 0 the WM answered because it
        // owned the window list. But the kernel RECEIVES every app_id at
        // create time and now keeps it, so "does a twin exist?" is a
        // fact it already holds -- and the compositor is left with the
        // ACTION, raising the window, which needs no answer at all.
        //
        // Search order is deliberate: the first match wins, and with one
        // window per app_id by construction (that is what single
        // instance MEANS) there is never a second.
        for (int p = 0; p < WIN_SERVER_MAX_PIDS; p++) {
            for (int i = 0; i < WIN_CLIENT_MAX; i++) {
                struct client_window *cw = &windows[p][i];
                if (!cw->used) continue;
                if (k_strcmp(cw->app_id, app_id) != 0) continue;
                tell_compositor(WIN_EV_CLIENT_ACTIVATE, cw->pid, cw->id, 0, 0);
                // The ring-0 WM still raises it through its own callback
                // while it exists; both run, neither disturbs the other.
                if (g_ops && g_ops->window_activate) g_ops->window_activate(app_id);
                return 1;
            }
        }
        return 0;
    }

    switch (req->type) {
    case WIN_REQ_CREATE: {
        uint32_t id = 0;
        // Truncate rather than refuse, exactly as WIN_REQ_TITLE does:
        // an over-long id is the client's mistake to notice, and
        // failing the create over it would turn a cosmetic slip into a
        // window that never opens.
        char app_id[WIN_APP_ID_LEN];
        copy_text(app_id, req->text, WIN_APP_ID_LEN);
        // vmm_current_pml4() IS the calling client's address space: a
        // syscall does not switch CR3 on entry (see vmm.h).
        if (!create_window(pid, vmm_current_pml4(), req->a, req->b,
                           req->c, req->d, app_id, &id)) return 0;
        req->window = id;
        return 1;
    }
    case WIN_REQ_PRESENT: {
        struct client_window *cw = lookup(pid, req->window);
        if (!cw) return 0;
        tell_compositor(WIN_EV_CLIENT_PRESENT, pid, cw->id, 0, 0);
        if (g_ops && g_ops->window_present) g_ops->window_present(pid, cw->id);
        return 1;
    }
    case WIN_REQ_DESTROY: {
        struct client_window *cw = lookup(pid, req->window);
        if (!cw) return 0;
        destroy_window(cw);
        return 1;
    }
    case WIN_REQ_TITLE: {
        struct client_window *cw = lookup(pid, req->window);
        if (!cw) return 0;
        // Truncate rather than refuse -- a too-long title is cosmetic.
        char title[WIN_TITLE_LEN];
        copy_text(title, req->text, WIN_TITLE_LEN);
        k_strlcpy(cw->title, title, sizeof cw->title);
        tell_compositor(WIN_EV_CLIENT_TITLE, pid, cw->id, 0, 0);
        if (g_ops && g_ops->window_title) g_ops->window_title(pid, cw->id, title);
        return 1;
    }
    case WIN_REQ_HINTS: {
        struct client_window *cw = lookup(pid, req->window);
        if (!cw) return 0;
        cw->hint_flags = (unsigned)req->a;
        cw->min_w = req->b;
        cw->min_h = req->c;
        tell_compositor(WIN_EV_CLIENT_HINTS, pid, cw->id, 0, 0);
        if (g_ops && g_ops->window_hints) {
            g_ops->window_hints(pid, cw->id, (unsigned)req->a, req->b, req->c);
        }
        return 1;
    }
    case WIN_REQ_RESIZE: {
        struct client_window *cw = lookup(pid, req->window);
        if (!cw) return 0;
        if (!resize_window(cw, req->a, req->b)) return 0;
        // Hand back what was actually granted, so a client never has to
        // assume it got what it asked for.
        req->a = cw->w;
        req->b = cw->h;
        return 1;
    }
    case WIN_REQ_TIMER: {
        struct client_window *cw = lookup(pid, req->window);
        if (!cw) return 0;
        // A timer with nobody to service it is refused, as before --
        // but "nobody" now means neither a ring-0 layer NOR a ring-3
        // compositor, and the compositor is told through the event
        // beside this call rather than through a slot.
        if (!g_comp_pid && (!g_ops || !g_ops->window_timer)) return 0;
        // Negative is nonsense rather than "cancel" -- 0 already means
        // that, and silently reinterpreting a bad value hides the bug.
        if (req->a < 0) return 0;
        tell_compositor(WIN_EV_CLIENT_TIMER, pid, cw->id, req->a, 0);
        if (g_ops && g_ops->window_timer) g_ops->window_timer(pid, cw->id, (unsigned)req->a);
        return 1;
    }
    case WIN_REQ_PONG: {
        struct client_window *cw = lookup(pid, req->window);
        if (!cw) return 0;
        // Relayed, not interpreted. Whether a late pong or a missing
        // one means anything is the WM's call -- see win_server.h.
        tell_compositor(WIN_EV_CLIENT_PONG, pid, req->window, req->a, 0);
    if (g_ops && g_ops->window_pong) g_ops->window_pong(pid, req->window, req->a);
        return 1;
    }
    case WIN_REQ_EVENT_PUSH: {
        // Only the compositor may put events on another process's
        // queue. Without this any client could synthesise a keystroke
        // into any other -- the protocol's whole access-control story is
        // that a window belongs to a process, and this is the one
        // request that reaches ACROSS processes.
        if (!g_comp_pid || pid != g_comp_pid) return -1;
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
    case WIN_REQ_MAP_WINDOW: {
        if (!g_comp_pid || pid != g_comp_pid) return -1;
        uint64_t vaddr = 0;
        // The access control lives inside that call rather than here: it
        // checks the requester IS the compositor and that the window
        // really belongs to the pid named. See win_server.h.
        if (!win_server_map_to_compositor(pid, req->a, req->window, &vaddr)) return -1;
        return 0;
    }
    case WIN_REQ_WINDOW_INFO: {
        // Another process's window's details, so: compositor only.
        if (!g_comp_pid || pid != g_comp_pid) return -1;
        struct client_window *cw = lookup(req->a, req->window);
        // Not an error the compositor can avoid -- a client may destroy
        // a window between the event and this call. Dropping the window
        // is the right response, not retrying.
        if (!cw) return -1;
        req->a = cw->w;
        req->b = cw->h;
        req->c = (int32_t)cw->hint_flags;
        // Two 16-bit values in one field rather than widening the
        // message: a minimum size larger than 65535 is not a thing.
        req->d = (int32_t)(((uint32_t)cw->min_h << 16) | ((uint32_t)cw->min_w & 0xFFFF));
        copy_text(req->text, cw->title, WIN_TITLE_LEN);
        return 0;
    }
    case WIN_REQ_FONT:
        return map_font(pid, req);
    default:
        return -1;
    }
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

    for (int i = 0; i < WIN_CLIENT_MAX; i++) {
        struct client_window *cw = &windows[pid - 1][i];
        if (cw->used) destroy_window(cw);
    }
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
    // IS THIS COMPOSITOR THE DESKTOP? That is the question the whole
    // function is conditional on, and getting it wrong is not subtle.
    //
    // `win_server_active()` is true while a presentation layer is
    // registered, which today means the RING-0 WM owns the screen and
    // the window list. In that world a compositor releasing the role is
    // a SECOND consumer leaving (stage 2's design -- compclient and
    // screenclient come and go routinely), not the desktop dying. Asking
    // every client to close there tears down live windows the WM is
    // still drawing: `compositor_test.py` caught exactly that, as UI Demo
    // going silent the moment the test compositor released the role.
    //
    // When the WM itself IS the ring-3 compositor there is no registered
    // presentation layer, so this proceeds and the user lands at a text
    // shell -- the milestone's exit criterion. The guard costs nothing
    // then and can go with the ring-0 WM.
    if (win_server_active()) {
        klog_write("win: compositor left, but the ring-0 WM still owns the "
                   "screen -- clients and console untouched\n");
        return;
    }

    int asked = 0;
    for (int p = 0; p < WIN_SERVER_MAX_PIDS; p++) {
        for (int i = 0; i < WIN_CLIENT_MAX; i++) {
            struct client_window *cw = &windows[p][i];
            if (!cw->used) continue;
            struct win_event ev;
            k_memset(&ev, 0, sizeof ev);
            ev.type = WIN_EV_CLOSE;
            ev.window = cw->id;
            win_events_push(cw->pid, &ev);
            asked++;
        }
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

    // Changing or clearing the compositor drops every mapping the old
    // one held. Skipping this would leave bookkeeping claiming a
    // mapping exists in an address space nobody composites from -- and
    // if that pml4 is later destroyed and its frames reused, a stale
    // `comp_mapped` makes the next unmap walk whatever now lives there.
    if (g_comp_pid && g_comp_pml4) {
        for (int p = 0; p < WIN_SERVER_MAX_PIDS; p++) {
            for (int i = 0; i < WIN_CLIENT_MAX; i++) {
                comp_unmap(&windows[p][i]);
                // The poison mappings of already-destroyed windows go
                // the same way and for the same reason -- they live in
                // the OLD compositor's address space, and the record of
                // them must not outlive it.
                comp_unpoison(&windows[p][i]);
            }
        }
        // The framebuffer grant goes the same way and for the same
        // reason: it belongs to the ROLE, not to the process. This is
        // the one place the role is cleared -- deregistration, a kill
        // and a fault all arrive here -- so it is the one place the
        // grant needs revoking, and there is no second bookkeeping to
        // fall out of step with it.
        win_surface_revoke(g_comp_pid);
    }

    // Was the role HELD, and is it being given up entirely? Only that
    // transition is a desktop dying. Handing the role from one
    // compositor to another is not -- the screen keeps an owner, so
    // asking every client to close and repainting the text console over
    // the top of the new desktop would be actively wrong.
    int was_held = (g_comp_pid != 0);

    g_comp_pid = pid;
    g_comp_pml4 = pid ? pml4 : 0;

    // AFTER the registration is cleared, not before: compositor_gone()
    // pushes events and repaints, and anything it reaches must already
    // see "there is no compositor" rather than a half-cleared one.
    if (was_held && pid == 0) compositor_gone();
    return 1;
}

int win_server_map_to_compositor(int requester_pid, int owner_pid, uint32_t id,
                                  uint64_t *out_vaddr) {
    // The access control, in one place: only the registered compositor,
    // and only for a window that really exists and really belongs to
    // the pid named. A window buffer is a client's private memory.
    if (!g_comp_pid || requester_pid != g_comp_pid) return 0;
    if (owner_pid < 1 || owner_pid > WIN_COMPOSITOR_MAX_PIDS) return 0;

    struct client_window *cw = lookup(owner_pid, id);
    if (!cw) return 0;

    if (!cw->comp_mapped && !comp_map(cw)) return 0;
    if (out_vaddr) *out_vaddr = win_compositor_vaddr(owner_pid, id);
    return 1;
}

int win_server_unmap_from_compositor(int requester_pid, int owner_pid, uint32_t id) {
    if (!g_comp_pid || requester_pid != g_comp_pid) return 0;
    struct client_window *cw = lookup(owner_pid, id);
    if (!cw || !cw->comp_mapped) return 0;
    comp_unmap(cw);
    return 1;
}

int win_server_is_mapped_to_compositor(int owner_pid, uint32_t id) {
    struct client_window *cw = lookup(owner_pid, id);
    return cw && cw->comp_mapped;
}

int win_server_create_raw(int pid, uint64_t pml4, int w, int h, uint32_t *out_id) {
    uint32_t id = 0;
    // No app id: this is the KTEST/compositor entry point, and a test
    // window that claimed one could be raised by a real app asking for
    // its twin.
    if (!create_window(pid, pml4, w, h, 0, 0, "", &id)) return 0;
    if (out_id) *out_id = id;
    return 1;
}

int win_server_destroy_raw(int pid, uint32_t id) {
    struct client_window *cw = lookup(pid, id);
    if (!cw) return 0;
    destroy_window(cw);
    return 1;
}

int win_server_resize_raw(int pid, uint32_t id, int w, int h) {
    struct client_window *cw = lookup(pid, id);
    if (!cw) return 0;
    return resize_window(cw, w, h);
}

int win_server_window_count(int pid) {
    if (pid < 1 || pid > WIN_SERVER_MAX_PIDS) return 0;
    int n = 0;
    for (int i = 0; i < WIN_CLIENT_MAX; i++) {
        if (windows[pid - 1][i].used) n++;
    }
    return n;
}
