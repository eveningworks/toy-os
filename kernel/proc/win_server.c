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
// How many buffers a window has. Every loop over them takes this
// rather than a range from its caller -- a call site that has to
// remember there are two is how the second one stopped being revoked.
#define WIN_BUFS 2

// ONE BUFFER, WITH ITS OWN SIZE. The dimensions belong to the BUFFER
// rather than to the window, which is what lets a resize rebuild the
// back buffer while the front still holds the last finished frame at
// the size it was drawn at -- Wayland's model, where a wl_buffer
// carries its dimensions and the surface adopts them on commit. The
// window's own w/h below is the CLIENT's current size; the front
// buffer catches up at the next present.
struct win_buf {
    int w, h;         // what the client says it built this one at; 0x0
                       // is a buffer this window does not have, which is
                       // a working window and not an error (see below)

    // WHICH OBJECT IS BEHIND THE NAME. The name identifies the slot and
    // never changes; the client replaces the object under it on a
    // resize and this goes up. A present carries it, and a compositor
    // holding a different one re-opens the name -- see
    // WIN_EV_CLIENT_PRESENT.
    uint32_t gen;
};

struct client_window {
    int used;
    int pid;
    uint32_t id;      // index into the owner's slots, and the slot
                       // the client named its buffer objects after

    // THE TWO BUFFERS, and the kernel takes the client's word that both
    // exist: it holds neither object and cannot look. **NO CLIENT
    // PRODUCES A SINGLE-BUFFERED WINDOW ANY MORE** -- Toykit makes both
    // objects or opens no window -- so `bufs[1].w == 0` is a state
    // nothing reaches today. The flip below still tests for it, because
    // the alternative is a protocol that cannot express a client with
    // one buffer at all.
    //    // A window is double-buffered so the compositor never reads memory
    // a client is drawing into. `front` says which of the two it should
    // read; the client draws into the other, and WIN_REQ_PRESENT flips
    // it.
    struct win_buf bufs[WIN_BUFS];
    int front;        // 0 or 1: which buffer the compositor reads

    // THE CLIENT'S CURRENT SIZE -- what it draws and what the BACK
    // buffer is built to. The front buffer may still be the previous
    // size, so a caller that means "the pixels the compositor is
    // reading" must ask bufs[front], not this.
    int w, h;

    // THE ONLY THING A CLIENT SAYS THAT THE KERNEL STILL KEEPS. The
    // title, the hints and the cursor shape all left for the channel
    // (userland/lib/uwmchan.h); this stayed because it rides
    // WIN_REQ_CREATE, so a window can never exist without one -- and
    // WIN_REQ_ACTIVATE matches on it, where a window briefly nameless is
    // the gap that makes a single-instance app miss its own twin and
    // exit without ever drawing.

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
};

// The registered compositor: the one process raw input is delivered
// to, and the one the framebuffer is granted to. It no longer maps
// anything through the kernel -- see win_server.h.
static int g_comp_pid = 0;



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

// Ends a window. THE PIXELS SURVIVE IT: they are the client's object,
// and whoever still has it mapped -- the compositor, until it drains
// the event below -- holds it alive by reference. Nothing here has a
// mapping to revoke, which is what retired the poison page.
static void destroy_window(struct client_window *cw) {
    if (!cw->used) return;

    if (g_ops && g_ops->window_destroyed) g_ops->window_destroyed(cw->pid, cw->id);
    tell_compositor(WIN_EV_CLIENT_DESTROYED, cw->pid, cw->id, 0, 0);

    for (int b = 0; b < WIN_BUFS; b++) cw->bufs[b].w = cw->bufs[b].h = 0;
    cw->used = 0;
    cw->front = 0;
}

// `want_slot` is the slot the CLIENT proposed -- its buffers are named
// after it (abi/win_proto.h), so the kernel cannot choose a different
// one. Negative means "any", which is what the KTEST entry point passes.
static int create_window(int pid, int want_slot,
                         int w, int h, int x, int y,
                          const char *app_id, uint32_t *out_id) {
    if (pid < 1 || pid > WIN_SERVER_MAX_PIDS) return 0;

    if (w <= 0 || h <= 0 || w > WIN_CLIENT_MAX_W || h > WIN_CLIENT_MAX_H) {
        klog_write("win_server: create refused -- bad size\n");
        return 0;
    }

    int slot = -1;
    if (want_slot >= 0) {
        if (want_slot >= WIN_CLIENT_MAX) return 0;
        if (windows[pid - 1][want_slot].used) {
            klog_write("win_server: create refused -- that slot is in use\n");
            return 0;
        }
        slot = want_slot;
    }
    for (int i = 0; slot < 0 && i < WIN_CLIENT_MAX; i++)
        if (!windows[pid - 1][i].used) slot = i;
    if (slot < 0) {
        klog_write("win_server: create refused -- client already holds WIN_CLIENT_MAX windows\n");
        return 0;
    }

    struct client_window *cw = &windows[pid - 1][slot];
    k_memset(cw, 0, sizeof *cw);
    cw->used = 1;
    cw->pid = pid;
    cw->id = (uint32_t)slot;
    // BOTH BUFFERS ARE ASSUMED PRESENT at the created size. The client
    // makes them before it asks, and a window whose second one could not
    // be made says so with WIN_REQ_BUFFER; the kernel cannot look,
    // because looking would mean holding the object.
    for (int b = 0; b < WIN_BUFS; b++) { cw->bufs[b].w = w; cw->bufs[b].h = h; }
    cw->w = w;
    cw->h = h;

    // Everything the kernel was handed and used to forward without
    // keeping. The compositor reads it back with WIN_REQ_WINDOW_APPID.
    k_strlcpy(cw->app_id, app_id ? app_id : "", sizeof cw->app_id);
    // THE IDENTITY, and it comes from the scheduler rather than from
    // anything the client said -- see the field's comment.
    {
        char path[FS_PATH_MAX];
        cw->app_identity = scheduler_exec_path(pid, path, sizeof path)
                            ? app_identity_for(path) : APP_IDENTITY_NONE;
    }

    tell_compositor(WIN_EV_CLIENT_CREATED, pid, cw->id, w, (uint32_t)h);

    if (g_ops && g_ops->window_created) {
        if (!g_ops->window_created(pid, cw->id, w, h, x, y, app_id)) {
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

// Records that the client replaced ONE of its buffers. The object is
// the client's -- it unlinked the old one and created a new one under
// the same name, and the old one stays alive under whatever still maps
// it, which is the promise shm_unlink already makes. All the kernel
// does is bump the GENERATION, which is what tells the compositor on
// the next present to re-open the name.
//
// **UNCONDITIONALLY, because the SIZE IS NOT A PROXY for the object.**
// A client re-created at the size it already had is a new object under
// the same name, and skipping the bump there would leave the compositor
// mapping memory nobody draws into any more -- a window frozen on its
// last frame. It is reachable: a drag proposing the size a window
// already has, or a restored geometry that matches, both send this.
static int rebuild_buffer(struct client_window *cw, int b, int w, int h) {
    struct win_buf *wb = &cw->bufs[b];
    wb->w = w;
    wb->h = h;
    wb->gen++;
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
static int resize_window(struct client_window *cw, int w, int h, int want_buf) {
    if (w <= 0 || h <= 0 || w > WIN_CLIENT_MAX_W || h > WIN_CLIENT_MAX_H) return 0;

    // **THE CLIENT SAYS WHICH BUFFER IT PREPARED.** It replaced that
    // object before asking, and the server must rebuild the SAME one --
    // computing it here from `front` means both sides deriving the
    // answer separately, and they disagree the moment a present lands
    // between the client's replace and this request. The result was a
    // resize that worked once and then silently stopped.
    int back = cw->bufs[1].w ? (cw->front ^ 1) : cw->front;
    if (want_buf >= 0 && want_buf < WIN_BUFS) back = want_buf;
    if (!rebuild_buffer(cw, back, w, h)) return 0;

    cw->w = w;
    cw->h = h;

    tell_compositor(WIN_EV_CLIENT_RESIZED, cw->pid, cw->id, w, (uint32_t)h);
    if (g_ops && g_ops->window_resized) {
        g_ops->window_resized(cw->pid, cw->id, w, h);
    }
    return 1;
}

// --- QUERY_WINDOWS: what the KERNEL thinks a window is ----------------
//
// The compositor's list is reported by `guictl windows`; this is the
// other half, and the pair is what makes a disagreement visible instead
// of inferred.

static int win_q_count(void) {
    int n = 0;
    for (int p = 0; p < WIN_SERVER_MAX_PIDS; p++)
        for (int i = 0; i < WIN_CLIENT_MAX; i++)
            if (windows[p][i].used) n++;
    return n;
}

static int win_q_fill(int index, void *out) {
    int n = 0;
    for (int p = 0; p < WIN_SERVER_MAX_PIDS; p++) {
        for (int i = 0; i < WIN_CLIENT_MAX; i++) {
            struct client_window *cw = &windows[p][i];
            if (!cw->used) continue;
            if (n++ != index) continue;
            struct query_window *r = out;
            k_memset(r, 0, sizeof *r);
            r->pid = cw->pid;
            r->id = cw->id;
            r->w = cw->w;
            r->h = cw->h;
            r->front = (uint32_t)cw->front;
            for (int b = 0; b < WIN_BUFS && b < 2; b++) {
                r->buf_w[b] = cw->bufs[b].w;
                r->buf_h[b] = cw->bufs[b].h;
                r->buf_gen[b] = cw->bufs[b].gen;
            }
            return 1;
        }
    }
    return 0;
}

static const struct query_provider win_q_provider = {
    .cls = QUERY_WINDOWS,
    .name = "windows",
    .record_size = sizeof(struct query_window),
    .flags = QUERY_F_LIST,
    .count = win_q_count,
    .fill = win_q_fill,
    .fields = NULL,
    .field_count = 0,
};

static void win_query_init(void) { query_register(&win_q_provider); }
INITCALL(win_query_init, INIT_QUERY);

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
        // `window` is an OUTPUT on this request and free as an input,
        // which is what carries the slot the client picked -- its
        // buffers are already named after it (abi/win_proto.h).
        if (!create_window(pid, (int)req->window,
                           req->a, req->b, req->c, req->d, app_id, &id))
            return 0;
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
        // **THE KERNEL DOES NOT REBUILD A BUFFER HERE ANY MORE.** It
        // used to resize the stale half at this point, where the pixels
        // it held had just stopped being needed -- opportunistic, and
        // impossible once the memory is the CLIENT's: the object it
        // would have to grow is one only the client can replace. The
        // client replaces it before drawing and says so with
        // WIN_REQ_BUFFER, which is what re-adopts it here.
        if (cw->bufs[1].w) cw->front ^= 1;
        // THE SIZE TRAVELS WITH THE FRAME. `mods` carries the front
        // buffer's own dimensions so the compositor adopts the geometry
        // of the pixels it is about to show, never a size it was
        // promised earlier -- see WIN_EV_CLIENT_PRESENT.
        // `b` CARRIES THE GENERATION TOO: the buffer's name is stable
        // and the object under it is not, so this is how a compositor
        // holding the previous one learns to re-open the name.
        tell_compositor(WIN_EV_CLIENT_PRESENT, pid, cw->id,
                        WIN_PRESENT_B(cw->front, cw->bufs[cw->front].gen),
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
    // WIN_REQ_TITLE, _HINTS and _CURSOR ARE NOT HERE ANY MORE. They go
    // straight to the compositor over a channel (userland/lib/uwmchan.h)
    // carrying their payloads, so the kernel neither stores them nor
    // relays a "something changed" event for them. It never had a use
    // for any of it -- the storage existed because struct win_event is
    // 24 bytes and none of those payloads fits.
    case WIN_REQ_RESIZE: {
        struct client_window *cw = lookup(pid, req->window);
        if (!cw) return 0;
        // c: which buffer the client prepared, -1 for "you choose".
        if (!resize_window(cw, req->a, req->b, req->c)) return 0;
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
    case WIN_REQ_BUFFER: {
        struct client_window *cw = lookup(pid, req->window);
        if (!cw) return 0;
        int b = req->a;
        if (b < 0 || b >= WIN_BUFS) return 0;
        if (req->b <= 0 || req->c <= 0) return 0;
        if (req->b > WIN_CLIENT_MAX_W || req->c > WIN_CLIENT_MAX_H) return 0;
        if (!rebuild_buffer(cw, b, req->b, req->c)) return 0;
        return 1;
    }
    // WIN_REQ_MAP_WINDOW AND _UNMAP_WINDOW ARE RETIRED. The compositor
    // opens a client's buffer by NAME and maps it itself, so there is
    // nothing to ask for and no kernel-held mapping to release.
    // WIN_REQ_WINDOW_INFO IS RETIRED. It answered a window's geometry,
    // and its last caller was the compositor asking for a size the
    // WIN_EV_CLIENT_CREATED event had just carried to it -- a round trip
    // that only existed because the same message used to fetch the title
    // and the hints too, and those left with the state behind them.
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

// --- stand-ins for a client, for the KTESTs ---------------------------
//
// A window's pixels are its client's objects, and these entry points
// have no client. They no longer have to make any: the kernel neither
// maps nor holds a buffer, so a window is exactly its size and its
// generation, and a fixture supplying those is supplying everything.

int win_server_create_raw(int pid, int w, int h, uint32_t *out_id) {
    uint32_t id = 0;
    // No app id: a test window that claimed one could be raised by a
    // real app asking for its twin.
    if (!create_window(pid, -1, w, h, 0, 0, "", &id)) return 0;
    if (out_id) *out_id = id;
    return 1;
}

int win_server_destroy_raw(int pid, uint32_t id) {
    struct client_window *cw = lookup(pid, id);
    if (!cw) return 0;
    destroy_window(cw);
    return 1;
}

// A buffer's own pixel count, for a KTEST asserting which of the two a
// resize touched. 0 for a buffer this window does not have.
int win_server_buf_size(int pid, uint32_t id, int buf) {
    struct client_window *cw = lookup(pid, id);
    if (!cw || buf < 0 || buf >= WIN_BUFS) return 0;
    return cw->bufs[buf].w * cw->bufs[buf].h;
}

// Which OBJECT a buffer is on, as a present would report it.
uint32_t win_server_buf_gen(int pid, uint32_t id, int buf) {
    struct client_window *cw = lookup(pid, id);
    if (!cw || buf < 0 || buf >= WIN_BUFS) return 0;
    return cw->bufs[buf].gen;
}

int win_server_resize_raw(int pid, uint32_t id, int w, int h) {
    struct client_window *cw = lookup(pid, id);
    if (!cw) return 0;
    // -1: let the server pick the back buffer, as a client with nothing
    // in flight would.
    return resize_window(cw, w, h, -1);
}

int win_server_window_count(int pid) {
    if (pid < 1 || pid > WIN_SERVER_MAX_PIDS) return 0;
    int n = 0;
    for (int i = 0; i < WIN_CLIENT_MAX; i++) {
        if (windows[pid - 1][i].used) n++;
    }
    return n;
}
