// Client windows, kernel side: ids, pixel buffers, per-process
// mappings, ownership and teardown. See win_server.h for why the split
// between this and the registered presentation layer falls where it
// does.
// **TWS -- the Toy Window Server**, memory half. See
// abi/win_proto.h for TWP, the protocol it serves, and
// userland/wm/wm_client.c for the presentation half.
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

#include "scheduler.h" // SCHED_MAX_PROCS, scheduler_exec_path() -- see app_path
#include "fs.h"       // FS_PATH_MAX, the size of that path
#include "kerrno.h"  // EBUSY -- the diagnostic channel is one slot
#include "mouse.h"    // mouse_get_state() -- park the plane where the pointer is
#include "heap.h"     // kmalloc/kfree -- the DEFINE sprite bounce buffer

#define WIN_SERVER_MAX_PIDS SCHED_MAX_PROCS

// The compositor's address region is carved per (pid, window) and its
// pid count is an ABI constant (abi/win_proto.h), so it cannot include
// the scheduler's header. Checked here instead of trusted: a pid past
// the region's end would compute an address overlapping another
// process's window.
_Static_assert(WIN_COMPOSITOR_MAX_PIDS >= SCHED_MAX_PROCS,
               "win_compositor_vaddr() has no room for every process");

// How many buffers a window's compositor slot can hold, and the offset
// of each: buffer `b` lives at `b * WIN_BUFFER_HALF`, which is what
// win_buffer_offset() encodes for the client side. Every slot helper
// below loops over this rather than taking a range from its caller --
// a call site that has to remember there are two is how the second one
// stopped being revoked.
#define COMP_BUFS 2

// ONE BUFFER, WITH ITS OWN SIZE. The dimensions belong to the BUFFER
// rather than to the window, which is what lets a resize rebuild the
// back buffer while the front still holds the last finished frame at
// the size it was drawn at -- Wayland's model, where a wl_buffer
// carries its dimensions and the surface adopts them on commit. The
// window's own w/h below is the CLIENT's current size; the front
// buffer catches up at the next present.
struct win_buf {
    uint32_t *phys;   // kernel-visible (identity-mapped) pixels, NULL if absent
    uint32_t pages;   // frames it spans
    int w, h;         // what was drawn into it
};

struct client_window {
    int used;
    int pid;
    uint32_t id;      // index into the owner's slots, so
                       // win_buffer_vaddr(id) is stable per window
    uint64_t pml4;    // the owner's address space, needed to unmap
    uint64_t vaddr;   // where the client sees buffer 0

    // THE TWO BUFFERS. `bufs[1].phys` is NULL when this window is
    // single-buffered.
    //
    // A window is double-buffered so the compositor never reads memory
    // a client is drawing into (see WIN_BUFFER_HALF in abi/win_proto.h).
    // `front` says which of the two the compositor should read; the
    // client draws into the other. WIN_REQ_PRESENT flips it.
    //
    // **NULL IS A WORKING WINDOW, NOT AN ERROR.** Each buffer is a
    // contiguous run, and that allocation is already what refuses a
    // window when physical memory fragments -- so a second one failing
    // makes the window single-buffered rather than making it not exist.
    // It then tears exactly as every window did before this, which is
    // strictly better than refusing to open. `front` stays 0 and the
    // flip is a no-op.
    struct win_buf bufs[COMP_BUFS];
    int front;        // 0 or 1: which buffer the compositor reads

    // THE CLIENT'S CURRENT SIZE -- what it draws and what the BACK
    // buffer is built to. The front buffer may still be the previous
    // size, so a caller that means "the pixels the compositor is
    // reading" must ask bufs[front], not this.
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
    // Per buffer -- see COMP_BUFS.
    uint32_t comp_poisoned[COMP_BUFS];

    // How many pages of this window's compositor slot currently have a
    // PTE AT ALL -- real frames plus any poison beyond them. It is the
    // HIGH-WATER MARK of every size this window has ever been, and it
    // never shrinks while the window lives.
    //
    // **THIS IS WHAT MAKES A SHRINK SAFE.** comp_poison()'s invariant --
    // a live window's slot is never a hole -- was written for a window
    // being DESTROYED and quietly did not cover one being made SMALLER.
    // A shrink remaps the slot to fewer pages while the compositor is
    // still holding the OLD width and height (it does not learn the new
    // ones until it drains WIN_EV_CLIENT_RESIZED, and it is a process,
    // so that is some frames later). Its very next blit then reads past
    // the new mapping into nothing and the desktop dies -- measured as
    // a page fault in ugfx_blit() at exactly the pixel where the old
    // size ran past the new one.
    //
    // Keeping the tail mapped to the poison page turns that into one
    // frame of black at the bottom of a shrinking window, which the
    // compositor corrects the moment it adopts the new size. Costs only
    // PTEs: every poison page in the system is the same borrowed frame.
    uint32_t comp_span[COMP_BUFS];

    // What the kernel used to receive and throw away, passing it
    // straight through to a ring-0 WM. A ring-3 one is TOLD a window
    // changed and reads the detail back (WIN_REQ_WINDOW_INFO), so the
    // detail has to live somewhere -- and the kernel is where it already
    // arrives. Holding it also lets the kernel answer the one question
    // that used to need a round trip into the WM: does a window with
    // this app_id exist? (WIN_REQ_ACTIVATE.)
    char title[WIN_TITLE_LEN];

    // The client's own NAME for what this window is ("notepad"). A
    // display string and a hint -- **NOT the identity**. See app_path
    // below for why, and abi/win_proto.h's WIN_REQ_CREATE.
    char app_id[WIN_APP_ID_LEN];

    // WHAT THIS WINDOW'S APPLICATION ACTUALLY IS: an index into
    // g_app_paths, interned from the full path its owning process was
    // spawned from -- taken from the SCHEDULER at create time and never
    // from anything the client said.
    //
    // Identity has to be something a client cannot get wrong, because
    // the two things keyed on it both fail silently when it is wrong.
    // Two apps declaring the same app_id would raise each other's
    // windows through WIN_REQ_ACTIVATE -- a single-instance app told
    // "your twin is already up" exits without ever drawing, so the
    // symptom is an app that simply does not start -- and a taskbar
    // grouping by it would merge two unrelated programs into one
    // button. No runtime check can catch that either, because two
    // copies of ONE program legitimately share an identity and look
    // identical to any such check.
    //
    // A path the kernel derives cannot be misdeclared, and every real
    // system anchors identity the same way: Windows falls back to the
    // executable behind an AppUserModelID, macOS to the bundle, and
    // Wayland's app_id is only dependable because a compositor matches
    // it against a .desktop FILE rather than trusting the string.
    // A NUMBER rather than the path itself, for two reasons. The
    // compositor has to compare these, and `struct win_request_msg`'s
    // only string field is WIN_TITLE_LEN (32) while a path is
    // FS_PATH_MAX (64) -- so shipping the path would truncate it, and
    // two long paths sharing a prefix would collide silently, which is
    // the very failure this replaced. And an integer is what a
    // comparison actually wants.
    //
    // APP_IDENTITY_NONE for a process with no scheduler slot (the
    // legacy loader), which therefore matches nothing -- the safe
    // direction.
    int app_identity;

    unsigned hint_flags;
    int min_w, min_h;

    // WIN_CURSOR_*: the shape this window wants under the pointer. Held
    // here because the compositor is a process and has to be told.
    int cursor;
};

// The registered compositor: which process may map other processes'
// windows, and where those mappings go. See win_server.h -- the address
// space is captured here rather than looked up per call.
static int g_comp_pid = 0;

// --- the clipboard ---------------------------------------------------
//
// One buffer for the whole system, holding a COPY of what was last put
// on it (abi/win_proto.h says why a copy and not a promise from the
// source). It is the server's rather than the compositor's so that it
// survives the compositor being killed -- which is a thing that happens
// here on purpose, and a clipboard that a Force Quit could empty would
// be a poor one.
static struct {
    uint32_t op;      // WIN_CLIP_OP_*
    uint32_t count;
    uint32_t len;     // bytes of `data` in use, NULs included
    uint32_t serial;  // bumped per SET, never reused
    char data[WIN_CLIP_BYTES];
} g_clip;


// See win_proto.h's WIN_REQ_FB_CURSOR. Belongs to the ROLE, like the
// framebuffer grant: cleared in win_server_set_compositor(), the one
// place the role changes hands or dies.
static int g_hw_cursor_armed = 0;

int win_server_hw_cursor_armed(void) { return g_hw_cursor_armed; }
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

int win_server_any(void) { return win_server_active() || g_comp_pid != 0; }

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
// --- application identity ---------------------------------------------
//
// Spawn paths, interned so a window can carry a small comparable number
// instead of a string (see struct client_window::app_identity).
//
// Never reclaimed. A bounded, never-shrinking table is the right shape
// here: entries are program paths, of which a running system has a
// handful, and freeing one would need a reference count whose only
// purpose would be to save 64 bytes. Full means the next new program
// gets APP_IDENTITY_NONE -- it groups with nothing and single-instance
// stops working for it, which is a degradation rather than a failure.
#define APP_IDENTITY_NONE (-1)
#define APP_IDENTITY_MAX  32

static char g_app_paths[APP_IDENTITY_MAX][FS_PATH_MAX];
static int  g_app_count;

static int app_identity_for(const char *path) {
    if (!path || !path[0]) return APP_IDENTITY_NONE;
    for (int i = 0; i < g_app_count; i++) {
        if (k_strcmp(g_app_paths[i], path) == 0) return i;
    }
    if (g_app_count >= APP_IDENTITY_MAX) {
        klog_write("win_server: app identity table full -- window ungrouped\n");
        return APP_IDENTITY_NONE;
    }
    k_strlcpy(g_app_paths[g_app_count], path, FS_PATH_MAX);
    return g_app_count++;
}

static uint64_t g_poison_frame;

static uint64_t poison_frame(void) {
    if (!g_poison_frame) {
        uint64_t f = pmm_alloc_frame(PMM_ZONE_ANY);
        if (!f) return 0;
        k_memset((void *)(uintptr_t)f, 0, 4096);
        g_poison_frame = f;
    }
    return g_poison_frame;
}

// Takes a poisoned slot's zero-page mappings back out. Must run before
// anything maps real frames there, and before the compositor's address
// space goes away.
// Takes EVERY page of the slot back out -- real frames and poison alike
// -- leaving it with no PTEs at all. The only caller that may leave it
// that way is the teardown of the compositor's address space itself;
// every other one maps something back immediately.
//
// It walks `comp_span`, not `pages`, because the two differ exactly
// when this matters: after a shrink the slot spans more pages than the
// window now needs, and unmapping only `pages` of them would strand the
// tail's PTEs pointing at the poison page forever.
static void comp_clear(struct client_window *cw) {
    uint64_t vaddr = win_compositor_vaddr(cw->pid, cw->id);
    for (int b = 0; b < COMP_BUFS; b++) {
        if (g_comp_pml4) {
            uint64_t base = vaddr + (uint64_t)b * WIN_BUFFER_HALF;
            for (uint32_t i = 0; i < cw->comp_span[b]; i++) {
                vmm_unmap_user_page(g_comp_pml4, base + (uint64_t)i * 4096);
            }
        }
        cw->comp_span[b] = 0;
        cw->comp_poisoned[b] = 0;
    }
    cw->comp_mapped = 0;
}

// Maps the poison page over [from, to) of BUFFER `b`'s half of the
// slot. Returns how many pages it managed; the caller decides whether a
// short result matters.
static uint32_t comp_poison_range(struct client_window *cw, int b,
                                  uint32_t from, uint32_t to) {
    if (to <= from) return 0;
    uint64_t phys = poison_frame();
    if (!phys) return 0;               // out of memory: a hole, as before
    uint64_t vaddr = win_compositor_vaddr(cw->pid, cw->id)
                   + (uint64_t)b * WIN_BUFFER_HALF;
    uint32_t n = 0;
    for (uint32_t i = from; i < to; i++) {
        // BORROWED, and doubly so: one frame mapped at every page of the
        // slot, so an owning teardown would free the same frame `pages`
        // times -- and it is a permanent singleton nothing ever frees.
        if (!vmm_map_user_borrowed(g_comp_pml4, vaddr + (uint64_t)i * 4096,
                                   phys, 0, 0, VMM_MT_NORMAL)) {
            break;
        }
        n++;
    }
    return n;
}

// The physical base of buffer `b`, or 0 when the window does not have
// one. Buffer 1 is absent on a single-buffered window, which is a
// working window and not an error (see the struct).
static uint64_t comp_buf_phys(const struct client_window *cw, int b) {
    return (uint64_t)(uintptr_t)cw->bufs[b].phys;
}

// Maps a window's frames into the compositor at its derived address.
// Returns 1 on success (including "already mapped"), 0 if the mapping
// could not be built -- in which case nothing is left half-mapped.
//
// ONE LOOP OVER EVERY BUFFER, and that is the point rather than a
// tidying: the slot holds two, and a version that mapped both here
// while the teardown paths described one left the compositor holding
// writable PTEs into buffer 1's freed frames after every window close.
static int comp_map(struct client_window *cw) {
    if (!g_comp_pid || !g_comp_pml4) return 0;
    uint64_t vaddr = win_compositor_vaddr(cw->pid, cw->id);

    // The slot's extent never shrinks while the window lives -- see
    // comp_span. Remember it before clearing, because the tail beyond
    // the new frames has to be poisoned back over, not left as a hole.
    // A buffer this window does not have keeps its high-water mark and
    // gains nothing, so a window that has never been double-buffered
    // spends no PTEs on the half it never uses -- while one that just
    // LOST its second buffer still gets that half poisoned, for the
    // same reason a shrink does.
    uint32_t span[COMP_BUFS];
    for (int b = 0; b < COMP_BUFS; b++) {
        span[b] = cw->comp_span[b];
        if (comp_buf_phys(cw, b) && span[b] < cw->bufs[b].pages)
            span[b] = cw->bufs[b].pages;
    }

    // A poisoned slot has PRESENT page-table entries pointing at the
    // zero page, and mapping over a present entry does NOT invalidate
    // the TLB (vmm_map_user_page_type() only counts it) -- so the
    // compositor would go on reading zeros from a live window. Unmap
    // first, which does invalidate.
    comp_clear(cw);

    for (int b = 0; b < COMP_BUFS; b++) {
        uint64_t phys = comp_buf_phys(cw, b);
        uint64_t base = vaddr + (uint64_t)b * WIN_BUFFER_HALF;
        uint32_t done = 0;
        // BORROWED: these frames are the window server's (allocated in
        // create_window(), freed in destroy_window()). The compositor
        // only gets to look at them.
        for (; phys && done < cw->bufs[b].pages; done++) {
            if (!vmm_map_user_borrowed(g_comp_pml4, base + (uint64_t)done * 4096,
                                        phys + (uint64_t)done * 4096,
                                        1, 0, VMM_MT_NORMAL))
                break;
        }
        if (phys && done < cw->bufs[b].pages) {
            // Never left half-mapped. Buffer 0 failing is fatal to the
            // whole mapping; buffer 1 failing only makes the window
            // single-buffered, which is what it would have been if the
            // second allocation had failed in the first place.
            for (uint32_t j = 0; j < done; j++)
                vmm_unmap_user_page(g_comp_pml4, base + (uint64_t)j * 4096);
            done = 0;
            if (b == 0) {
                klog_write("win_server: compositor mapping failed\n");
                return 0;   // comp_clear() above already zeroed the spans
            }
            klog_write("win_server: compositor second-buffer mapping failed -- "
                       "window reads as single-buffered\n");
            cw->front = 0;
        }
        cw->comp_span[b] = done;

        // THE TAIL. Everything this half of the slot used to cover and
        // no longer does reads as black rather than faulting, for as
        // long as it takes the compositor to notice.
        cw->comp_poisoned[b] = comp_poison_range(cw, b, done, span[b]);
        cw->comp_span[b] += cw->comp_poisoned[b];
    }

    cw->comp_mapped = 1;
    return 1;
}

// Revokes it. Safe to call unconditionally; that is the point, since
// every path that frees or replaces frames has to call it and none of
// them should have to know whether a mapping exists.
static void comp_unmap(struct client_window *cw) {
    if (!cw->comp_mapped) return;
    comp_clear(cw);
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
    // The WHOLE span of EVERY buffer, not just the live frames: a slot
    // that has been shrunk already has a poisoned tail, and the
    // compositor may still blit out to the largest size this window
    // ever was.
    uint32_t span[COMP_BUFS];
    for (int b = 0; b < COMP_BUFS; b++) span[b] = cw->comp_span[b];
    comp_clear(cw);                    // also invalidates the TLB
    for (int b = 0; b < COMP_BUFS; b++) {
        cw->comp_poisoned[b] = comp_poison_range(cw, b, 0, span[b]);
        cw->comp_span[b] = cw->comp_poisoned[b];
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

    // Both buffers, each at its OWN page count -- they can differ while
    // a resize is in flight. Unmapped before freeing for the same reason
    // throughout: a mapping outliving its frames points at whatever the
    // allocator hands out next.
    for (int b = 0; b < COMP_BUFS; b++) {
        struct win_buf *wb = &cw->bufs[b];
        if (!wb->phys) continue;
        uint64_t base = cw->vaddr + (uint64_t)b * WIN_BUFFER_HALF;
        for (uint32_t i = 0; i < wb->pages; i++)
            vmm_unmap_user_page(cw->pml4, base + (uint64_t)i * 4096);
        // pmm_free_contiguous(), not a pmm_free_frame() loop -- the
        // buffer came from pmm_alloc_contiguous() and the two allocators
        // are not interchangeable (see api/pmm.h).
        pmm_free_contiguous((uint64_t)(uintptr_t)wb->phys, wb->pages);
        wb->phys = NULL;
        wb->pages = 0;
        wb->w = wb->h = 0;
    }
    cw->used = 0;
    cw->front = 0;
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
    uint64_t phys = pmm_alloc_contiguous(pages, PMM_ZONE_ANY);
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
        // BORROWED: pmm_alloc_contiguous() above made these the window
        // server's, and destroy_window() frees them. A client's teardown
        // freeing them too would hand live frames back to the allocator.
        if (!vmm_map_user_borrowed(pml4, vaddr + (uint64_t)i * 4096,
                                    phys + (uint64_t)i * 4096, 1, 0, VMM_MT_NORMAL)) {
            // Unwind the pages already mapped, then the frames.
            for (uint32_t j = 0; j < i; j++) vmm_unmap_user_page(pml4, vaddr + (uint64_t)j * 4096);
            pmm_free_contiguous(phys, pages);
            klog_write("win_server: create refused -- mapping failed\n");
            return 0;
        }
    }

    // THE SECOND BUFFER. Allocated and mapped exactly like the first,
    // at `vaddr + WIN_BUFFER_HALF`, and its failure is NOT fatal: the
    // window is created single-buffered and tears the way every window
    // did before double buffering existed. Refusing to open a window
    // because the tear-free path is unavailable would be trading a
    // cosmetic problem for a functional one.
    uint64_t phys2 = pmm_alloc_contiguous(pages, PMM_ZONE_ANY);
    if (phys2) {
        k_memset((void *)(uintptr_t)phys2, 0, (size_t)pages * 4096);
        uint64_t v2 = vaddr + WIN_BUFFER_HALF;
        uint32_t done = 0;
        for (; done < pages; done++) {
            if (!vmm_map_user_borrowed(pml4, v2 + (uint64_t)done * 4096,
                                        phys2 + (uint64_t)done * 4096, 1, 0,
                                        VMM_MT_NORMAL))
                break;
        }
        if (done < pages) {
            for (uint32_t j = 0; j < done; j++)
                vmm_unmap_user_page(pml4, v2 + (uint64_t)j * 4096);
            pmm_free_contiguous(phys2, pages);
            phys2 = 0;
            klog_write("win_server: second buffer unmapped -- window is "
                       "single-buffered\n");
        }
    } else {
        klog_write("win_server: no contiguous memory for a second buffer -- "
                   "window is single-buffered\n");
    }

    cw->used = 1;
    cw->pid = pid;
    cw->id = (uint32_t)slot;
    cw->pml4 = pml4;
    cw->bufs[0].phys = (uint32_t *)(uintptr_t)phys;
    cw->bufs[0].pages = pages;
    cw->bufs[0].w = w;
    cw->bufs[0].h = h;
    cw->bufs[1].phys = (uint32_t *)(uintptr_t)phys2;
    cw->bufs[1].pages = phys2 ? pages : 0;
    cw->bufs[1].w = phys2 ? w : 0;
    cw->bufs[1].h = phys2 ? h : 0;
    cw->front = 0;
    cw->vaddr = vaddr;
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
    // THE IDENTITY, and it comes from the scheduler rather than from
    // anything the client said -- see the field's comment.
    {
        char path[FS_PATH_MAX];
        cw->app_identity = scheduler_exec_path(pid, path, sizeof path)
                            ? app_identity_for(path) : APP_IDENTITY_NONE;
    }
    cw->hint_flags = 0;
    cw->min_w = 0;
    cw->min_h = 0;
    cw->cursor = WIN_CURSOR_DEFAULT; // slots are reused; don't inherit

    tell_compositor(WIN_EV_CLIENT_CREATED, pid, cw->id, w, (uint32_t)h);

    if (g_ops && g_ops->window_created) {
        if (!g_ops->window_created(pid, cw->id, cw->bufs[0].phys, w, h, x, y, app_id)) {
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

// Rebuild ONE of a window's buffers at a new size, at the same virtual
// address it already has -- win_buffer_vaddr() derives that from the
// window id plus the buffer index, so the client's pointer never moves
// and it is never told its pixels went anywhere.
//
// Ordering is chosen so a FAILURE leaves the buffer exactly as it was:
// the new frames are allocated and zeroed BEFORE anything is unmapped,
// and a half-built mapping puts the original frames back (they are
// still allocated -- nothing has freed them yet). A refusal is a normal
// outcome of this call, not an error path -- see abi/win_proto.h.
//
// The COMPOSITOR's view is deliberately NOT touched here: the caller
// remaps the whole slot once, because poisoning the front buffer to
// rebuild the back one is exactly the black frame this file is shaped
// to avoid.
static int rebuild_buffer(struct client_window *cw, int b, int w, int h) {
    struct win_buf *wb = &cw->bufs[b];
    uint32_t bytes = (uint32_t)w * (uint32_t)h * 4;
    uint32_t pages = (bytes + 4095) / 4096;
    uint64_t base = cw->vaddr + (uint64_t)b * WIN_BUFFER_HALF;

    uint64_t phys = pmm_alloc_contiguous(pages, PMM_ZONE_ANY);
    if (!phys) {
        klog_write("win_server: buffer rebuild refused -- out of contiguous memory\n");
        return 0;
    }
    // Zeroed before the client ever sees it: these frames may have been
    // another process's a moment ago.
    k_memset((void *)(uintptr_t)phys, 0, (size_t)pages * 4096);

    uint32_t old_pages = wb->pages;
    for (uint32_t i = 0; i < old_pages; i++)
        vmm_unmap_user_page(cw->pml4, base + (uint64_t)i * 4096);

    for (uint32_t i = 0; i < pages; i++) {
        // BORROWED: these frames are the window server's, freed by
        // destroy_window() and by this function. A mapping that owned
        // them would have the client's teardown free them too.
        if (vmm_map_user_borrowed(cw->pml4, base + (uint64_t)i * 4096,
                                   phys + (uint64_t)i * 4096, 1, 0, VMM_MT_NORMAL))
            continue;
        // Half-mapped and the old frames are already unmapped: put the
        // ORIGINAL buffer back rather than leaving the client with an
        // address that faults. Nothing has been freed yet, so this can
        // always succeed with the memory it had a moment ago.
        for (uint32_t j = 0; j < i; j++)
            vmm_unmap_user_page(cw->pml4, base + (uint64_t)j * 4096);
        for (uint32_t j = 0; j < old_pages; j++)
            vmm_map_user_borrowed(cw->pml4, base + (uint64_t)j * 4096,
                                   (uint64_t)(uintptr_t)wb->phys + (uint64_t)j * 4096,
                                   1, 0, VMM_MT_NORMAL);
        pmm_free_contiguous(phys, pages);
        klog_write("win_server: buffer rebuild refused -- mapping failed\n");
        return 0;
    }

    if (wb->phys) pmm_free_contiguous((uint64_t)(uintptr_t)wb->phys, old_pages);
    wb->phys = (uint32_t *)(uintptr_t)phys;
    wb->pages = pages;
    wb->w = w;
    wb->h = h;
    return 1;
}

// A client accepted a proposed size: rebuild the buffer it is about to
// draw into, and LEAVE THE FRONT ONE ALONE.
//
// That asymmetry is the whole of it. The front buffer still holds the
// last finished frame, at the size it was drawn at, so the compositor
// goes on showing real pixels for the rest of the round trip instead of
// a freshly zeroed buffer -- which is a whole window of black for as
// long as the client takes to repaint (measured at 100-240 ms under
// TCG). The new size arrives with the frame drawn at it, at the next
// present. That is Wayland's rule that a buffer carries its own
// dimensions; X11's server-resizes-then-app-repaints is the shape this
// used to have, flicker included.
//
// A SINGLE-BUFFERED window has nowhere to hide the change and rebuilds
// the one buffer it has, blank frame and all -- the same degradation it
// already accepts for tearing.
static int resize_window(struct client_window *cw, int w, int h) {
    if (w <= 0 || h <= 0 || w > WIN_CLIENT_MAX_W || h > WIN_CLIENT_MAX_H) return 0;

    int back = cw->bufs[1].phys ? (cw->front ^ 1) : cw->front;
    if (!rebuild_buffer(cw, back, w, h)) return 0;

    cw->w = w;
    cw->h = h;

    // The compositor's mapping still describes the old frames of the
    // buffer that just changed. Revoke and rebuild the whole slot --
    // both halves, because comp_map() is the only thing that knows how
    // to poison a shrunken tail. This runs inside the client's syscall,
    // so the compositor cannot composite between the two.
    if (cw->comp_mapped) {
        comp_poison(cw);  // never a hole while the window lives
        comp_map(cw);
    }

    tell_compositor(WIN_EV_CLIENT_RESIZED, cw->pid, cw->id, w, (uint32_t)h);
    if (g_ops && g_ops->window_resized) {
        g_ops->window_resized(cw->pid, cw->id, cw->bufs[0].phys, w, h);
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

// The clipboard, set and read. ANY client may do either: a clipboard
// whose reads were privileged would be one nothing could paste from,
// and every windowing system takes the same view.
//
// **SPLIT INTO A HEADER AND A BUFFER, on purpose.** The obvious shape
// -- one `struct win_clip_msg` copied in, acted on, copied back, as
// SYS_WIN_DEBUG does -- puts a kilobyte on the kernel stack, and
// `-Wframe-larger-than=1024` says no. A static scratch buffer would be
// worse: a ring-3 process is preemptible inside a syscall, so two
// clients would overwrite each other's payload, which is the exact
// re-entrancy bug tfs3.c carries a preemption guard for. So the caller
// copies the payload straight in and out of the one buffer that has to
// exist anyway, and only the 24-byte header rides the stack.
void win_server_clip_get(uint32_t *op, uint32_t *count, uint32_t *len,
                          uint32_t *serial) {
    if (op) *op = g_clip.op;
    if (count) *count = g_clip.count;
    if (len) *len = g_clip.len;
    if (serial) *serial = g_clip.serial;
}

char *win_server_clip_buf(void) { return g_clip.data; }

// Checked BEFORE the caller copies anything in, so a refusal cannot
// leave half a payload in the buffer.
int win_server_clip_would_fit(uint32_t op, uint32_t count, uint32_t len) {
    if (len > WIN_CLIP_BYTES || count > WIN_CLIP_MAX) {
        // REFUSED, not truncated. Half a cut set pasted is files
        // silently left behind -- the same rule as a formatter that
        // will not fit writing nothing.
        klog_write("win: clipboard SET refused -- payload too large\n");
        return 0;
    }
    return op == WIN_CLIP_OP_NONE || op == WIN_CLIP_OP_COPY ||
           op == WIN_CLIP_OP_CUT;
}

// Called once the payload is in the buffer. Returns the new serial.
uint32_t win_server_clip_commit(uint32_t op, uint32_t count, uint32_t len) {
    g_clip.op = op;
    g_clip.count = count;
    g_clip.len = len;
    // Bumped per SET and never reused: it is how a client notices that
    // somebody else replaced the clipboard under it. Comparing payloads
    // would be slower and wrong -- copying the same file twice is a
    // real change to the cut/copy mode.
    g_clip.serial++;

    // Every client hears, so a File Manager showing a pending cut stops
    // showing it the moment another one copies.
    win_server_broadcast(WIN_EV_CLIPBOARD, (int32_t)op,
                          (int32_t)g_clip.serial, 0);
    return g_clip.serial;
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

    // A ring-0 WM answers directly; a ring-3 one is asked and waited for.
    // Both are live during the migration, and the ring-0 one wins while
    // it exists -- it is still the desktop being driven.
    int n;
    if (g_ops && g_ops->debug_command) {
        n = g_ops->debug_command(msg->text, g_dbg_reply, sizeof g_dbg_reply);
    } else if (g_comp_pid && pid > 0) {
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
        // **THE CALLER NAMES NOTHING.** This asks "is a window of MY
        // program already open?", and the kernel answers from the
        // caller's own spawn path -- see struct client_window::app_path.
        //
        // It used to take an app id in `text`, which made the answer
        // depend on a string each app declared about itself: two apps
        // declaring the same one raised each other's windows, and the
        // single-instance caller reads a "yes" as "my twin is up, exit
        // now" -- so the app simply never appeared. Nothing could check
        // for that either, because two copies of one program are
        // SUPPOSED to match. A question whose answer the asker cannot
        // influence has no such failure mode.
        char self_path[FS_PATH_MAX];
        if (!scheduler_exec_path(pid, self_path, sizeof self_path)) return 0;
        int self_id = app_identity_for(self_path);
        if (self_id == APP_IDENTITY_NONE) return 0;   // matches nothing

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
        // window per program by construction (that is what single
        // instance MEANS) there is never a second.
        //
        // The caller's OWN windows are skipped. It is asking whether a
        // twin exists, and matching itself would make every
        // single-instance app refuse its own first window.
        for (int p = 0; p < WIN_SERVER_MAX_PIDS; p++) {
            for (int i = 0; i < WIN_CLIENT_MAX; i++) {
                struct client_window *cw = &windows[p][i];
                if (!cw->used) continue;
                if (cw->pid == pid) continue;
                if (cw->app_identity != self_id) continue;
                tell_compositor(WIN_EV_CLIENT_ACTIVATE, cw->pid, cw->id, 0, 0);
                // The ring-0 WM still raises it through its own callback
                // while it exists; both run, neither disturbs the other.
                if (g_ops && g_ops->window_activate) g_ops->window_activate(cw->app_id);
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
        // THE FLIP HAPPENS HERE, before anybody is told. A client must
        // know which buffer is safe to draw into the moment this
        // returns, and the compositor must never be pointed at a buffer
        // the client has already started on -- doing it in the other
        // order leaves a window where both are true at once.
        //
        // A single-buffered window (no second allocation) stays at 0,
        // so the client keeps drawing where it was and the compositor
        // keeps reading the same place. That is the old behaviour, and
        // the caller needs no special case for it.
        if (cw->bufs[1].phys) {
            // THE BUFFER ABOUT TO BECOME THE BACK ONE IS THE STALE ONE.
            // A resize rebuilt only the buffer the client drew this
            // frame into (see resize_window()), so the other half is
            // still the previous size -- and the client draws into it
            // next. Rebuild it here, where the pixels it holds have
            // just stopped being needed.
            //
            // A failure means NO FLIP: the compositor keeps the frame
            // it has and the client redraws the same buffer, so the
            // window stops updating until memory frees up rather than
            // overrunning a too-small one. The next present retries.
            struct win_buf *stale = &cw->bufs[cw->front];
            if (stale->w != cw->w || stale->h != cw->h) {
                if (!rebuild_buffer(cw, cw->front, cw->w, cw->h)) {
                    klog_write("win_server: present deferred -- no memory for "
                               "the back buffer\n");
                    return cw->front + 1;
                }
                // Only when frames actually moved: comp_map() is a
                // page-table rebuild, and doing it per present would put
                // one on every client frame.
                if (cw->comp_mapped) { comp_poison(cw); comp_map(cw); }
            }
            cw->front ^= 1;
        }
        // THE SIZE TRAVELS WITH THE FRAME. `mods` carries the front
        // buffer's own dimensions so the compositor adopts the geometry
        // of the pixels it is about to show, never a size it was
        // promised earlier -- see WIN_EV_CLIENT_PRESENT.
        tell_compositor(WIN_EV_CLIENT_PRESENT, pid, cw->id,
                        (uint32_t)cw->front,
                        WIN_PRESENT_SIZE(cw->bufs[cw->front].w,
                                         cw->bufs[cw->front].h));
        if (g_ops && g_ops->window_present) g_ops->window_present(pid, cw->id);
        // The new FRONT index: what the compositor will read, and
        // therefore what the client must NOT draw into.
        return cw->front + 1;   // 1 or 2, so 0 stays "refused"
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
    case WIN_REQ_CURSOR: {
        struct client_window *cw = lookup(pid, req->window);
        if (!cw) return 0;
        // Refuse, don't clamp: a client built against a later
        // WIN_CURSOR_* should find out.
        if (req->a < 0 || req->a >= WIN_CURSOR_COUNT) return 0;
        if (cw->cursor == req->a) return 1; // no-op, and no event
        cw->cursor = req->a;
        tell_compositor(WIN_EV_CLIENT_CURSOR, pid, cw->id, req->a, 0);
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
    case WIN_REQ_WINDOW_APPID: {
        // Same access rule and same "gone is not an error" contract as
        // WIN_REQ_WINDOW_INFO above -- see win_proto.h for why this
        // needs its own request rather than a field on that one.
        //
        // Answers with the window's IDENTITY (its owner's spawn path),
        // not with the app_id the client declared: the compositor groups
        // taskbar buttons by this, and grouping must not be something an
        // app can get wrong. `text` is WIN_TITLE_LEN and a path is
        // FS_PATH_MAX -- both 64 today, and the copy is bounded by the
        // smaller either way.
        if (!g_comp_pid || pid != g_comp_pid) return -1;
        struct client_window *cw = lookup(req->a, req->window);
        if (!cw) return -1;
        req->a = cw->app_identity;
        copy_text(req->text, cw->app_id, WIN_APP_ID_LEN);
        return 0;
    }
    case WIN_REQ_FONT:
        return map_font(pid, req);
    default:
        return -1;
    }
}

// One event to EVERY window, and to the compositor.
//
// THE COMPOSITOR FIRST, AND SEPARATELY -- it is not in windows[][]. It
// owns no window of its own (it draws the screen), so a broadcast that
// only walked the window table reached every client and missed the one
// process that draws the chrome, the taskbar and the icons. That is
// exactly how the font broadcast first looked like it was never
// delivered at all.
void win_server_broadcast(uint32_t type, int32_t a, int32_t b, uint32_t mods) {
    // The compositor's own copy names its window as 0 -- it has none,
    // which is the whole point of telling it separately.
    tell_compositor(type, 0, 0, b, mods);

    for (int p = 0; p < WIN_SERVER_MAX_PIDS; p++) {
        for (int i = 0; i < WIN_CLIENT_MAX; i++) {
            struct client_window *cw = &windows[p][i];
            if (!cw->used) continue;
            struct win_event ev;
            k_memset(&ev, 0, sizeof ev);
            ev.type = type;
            ev.window = cw->id;
            ev.a = a;
            ev.b = b;
            ev.mods = mods;
            win_events_push(cw->pid, &ev);
        }
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
                // comp_clear() takes real frames AND poison in one
                // pass, which is what this needs: an already-destroyed
                // window's slot holds only poison, and a shrunk one
                // holds both. Both live in the OLD compositor's address
                // space, and the record of them must not outlive it.
                comp_clear(&windows[p][i]);
            }
        }
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
