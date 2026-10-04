// See ui/uapp.h for what this is and why.
#include "rt/sys.h"   // TWP messages, sys_win_request()
#include "ui/uui_anim.h" // a frame while something animates
#include "kpath.h"    // k_path_dirname/_basename -- a drag from another window
#include "ui/uapp.h"
#include <string.h>
#include <stdio.h>
#include "lib/uchan.h"
#include "lib/uwmchan.h"
#include "ui/uui_route.h"
#include "ui/uui_popup.h"   // the popup-surface provider, installed at open
#include "ui/uui_focus.h"   // desc.focus -- keyboard focus ring
#include "ui/uui_button.h"  // a lone button commits through on_action
#include "ui/ulog.h"        // uapp_log_layout()
#include "setting_abi.h" // desktop.layout_log -- the gate below
#include "lib/usetting.h" // ...and the MERGED registry that can see it
#include <stdio.h>      // snprintf/vsnprintf, one layout line at a time
#include <stdarg.h>
#include "ui/utheme.h"
#include "ui/uui_caret.h"
#include "lib/uclip.h"   // clip_poll() -- the clipboard is shared memory now
#include <locale.h>
#include <time.h>       // tzset()

// THE CLIENT'S OWN WINDOW MEMORY. A buffer is a named shm object this
// process creates and GRANTS to the compositor, which opens the name
// itself -- the kernel neither allocates it nor maps it. The name
// identifies the slot; the object in it is replaced on a resize, and
// the old one stays alive under whoever still maps it until they let go
// (docs/winserver-ring3-design.md, stage 5).
#define UAPP_BUFS WIN_CLIENT_BUFS

// ONE SURFACE THE COMPOSITOR SHOWS: the toplevel window in slot 0, a
// popup (ui/uui_popup.h) in any other. Indexed by SLOT, which is also
// the `window` every event names and the number in the buffer's shm
// name -- one number, three uses, nothing to keep in step.
struct uapp_surf {
    int used;
    void *px[UAPP_BUFS];
    uint64_t px_bytes[UAPP_BUFS];
    // WHAT EACH BUFFER IS CURRENTLY SIZED FOR. **NOT derivable from
    // px_bytes**, which is page-rounded: a one-pixel width change usually
    // leaves the rounded length identical, so a comparison on bytes
    // reports a real resize as "nothing to do" -- and the server then
    // holds the old width while the client draws at the new one, which
    // the compositor blits as a diagonal shear.
    int px_w[UAPP_BUFS], px_h[UAPP_BUFS];
    // WHICH OBJECT IS BEHIND EACH BUFFER'S NAME. The name is the slot and
    // never changes; the object under it is replaced on every resize,
    // and this is how the compositor knows to re-open. **THE CREATOR
    // COUNTS IT** -- the kernel used to, from a message the client had
    // to remember to send, which is one record too many for a fact only
    // this side can observe.
    uint32_t px_gen[UAPP_BUFS];
    // WHICH BUFFER THE COMPOSITOR IS READING. The surface handed out
    // always points at the OTHER one -- see present(). 0 until the first
    // present, and 0 forever for a single-buffered surface.
    int front;
    // WHICH BUFFERS THE COMPOSITOR MAY STILL BE READING: presented and
    // not yet handed back by WIN_EV_BUF_RELEASE (wl_buffer.release).
    // Drawing into one of those put half-painted frames on screen. `back`
    // is the one drawn into, chosen when a frame starts (surf_back()),
    // -1 between frames -- choosing at present time would always find
    // the previous front still busy and take the third buffer.
    int busy[UAPP_BUFS];
    unsigned long long busy_seq[UAPP_BUFS];  // present order
    int back;
    unsigned long long stall_ms;  // when no buffer was free; 0 otherwise
    int w, h;
    struct ugfx_surface surface;  // the back buffer, sized w x h
    int dirty;                    // a popup: drawn into since its last present
    // A POPUP'S PLACE relative to the toplevel's content origin, from
    // the compositor's reply -- what turns popup-local input back into
    // the coordinates every widget hit-tests in (ui/uui_popup.h).
    int ox, oy;
    // WHICH WINDOW THIS POPUP BELONGS TO. A dialog window is its own
    // toplevel with its own router (uapp_window_open), so a dropdown
    // inside one anchors its popup to THAT surface and its input must
    // go back to THAT router -- routing it to the main window's would
    // hit-test the press against a different window's widgets.
    uint32_t parent;
    void (*done)(void *owner);    // the widget to tell when the compositor dismisses it
    void *owner;
};

// The window a popup opened right now should hang off: whichever one is
// dispatching, or the toplevel. A widget cannot say -- it does not know
// it is in a dialog -- so uapp tracks it around the one call that can
// re-enter a widget from a non-toplevel window.
static uint32_t g_popup_parent;
static struct uapp_surf g_surf[WIN_CLIENT_MAX];
#define TOPLEVEL (&g_surf[0])

// A PRIMARY PRESS IN FLIGHT ON A POPUP, so a popup closed under it can
// end it. The compositor sends no release to a surface that is gone (nor
// does wl_pointer), and the widget that took the press would otherwise
// hold the router's grab until the next release anywhere -- a value
// committed at the press reached the app only then. The close OWES the
// release, delivered after the event that closed it (owed_release()),
// never from inside the widget call that is closing it.
static uint32_t g_press_slot;           // 0: none, or the press was not on a popup
static struct win_event g_press_up;     // that press, as a release in its parent's coordinates
static int g_release_owed;

static void popup_press_closed(int id) {
    if (id > 0 && (uint32_t)id == g_press_slot) {
        g_press_slot = 0;
        g_release_owed = 1;
    }
}

static int slot_of(const struct uapp_surf *s) { return (int)(s - g_surf); }

static void buf_name(char *out, unsigned cap, int slot, int buf) {
    snprintf(out, cap, WIN_BUF_NAME_FMT, sys_getpid(), slot, buf);
}

// Lets the compositor open this object. **PER OBJECT, NOT PER NAME**: a
// grant lives on the object, and a resize creates a NEW one under the
// same name with an empty grant list -- so this belongs beside every
// create, not once at startup. Without it the first resize is the last
// frame the compositor ever sees of the window.
static int comp_pid(void);

static void buf_grant(const char *nm) {
    int pid = comp_pid();
    if (pid > 0) sys_shm_grant(nm, pid);
}

// Creates (or REPLACES) one buffer at `bytes`. A replace unlinks the old
// name first and creates a new object under it -- the old one stays
// alive for whoever still maps it, which is exactly what stops a resize
// pulling the pixels out from under the compositor.
static int buf_make(struct uapp_surf *s, int buf, int w, int h) {
    int slot = slot_of(s);
    // **PAGE-ALIGNED, because SYS_MUNMAP requires it** (abi/syscall_abi.h)
    // and refuses anything else. A window's pixels are w*h*4, which is
    // almost never a whole number of pages -- so the unrounded length
    // made every unmap fail silently, the old mapping outlive its
    // replacement, and each resize leak a buffer's worth of frames.
    // Twelve objects after three drags, and then the biggest allocation
    // in the run is the one that fails.
    uint64_t bytes = ((uint64_t)w * (uint64_t)h * 4 + 4095) & ~4095ULL;
    char nm[WIN_BUF_NAME_MAX];
    buf_name(nm, sizeof nm, slot, buf);
    if (s->px[buf]) {
        sys_munmap(s->px[buf], s->px_bytes[buf]);
        s->px[buf] = 0;
    }
    sys_shm_unlink(nm);
    int fd = sys_shm_open(nm, bytes, SHM_CREATE | SHM_EXCL);
    if (fd < 0) return 0;
    buf_grant(nm);
    void *p = sys_mmap(0, bytes, SYS_PROT_READ | SYS_PROT_WRITE,
                       SYS_MAP_SHARED, fd, 0);
    sys_close(fd);
    if (p == (void *)-1) return 0;
    s->px[buf] = p;
    s->px_bytes[buf] = bytes;
    s->px_w[buf] = w;
    s->px_h[buf] = h;
    // A NEW OBJECT, WHATEVER ITS SIZE. Bumping only on a size change
    // would leave the compositor mapping an object nobody draws into
    // when a client re-creates a buffer at the size it already had --
    // a window frozen on its last frame, and reachable from a drag that
    // proposes the size the window has.
    s->px_gen[buf]++;
    return 1;
}

static void bufs_release(struct uapp_surf *s);

// Claims slot `slot` for a surface of w x h and makes two of its
// buffers; the third is made the first time both others are busy.
static int bufs_create(struct uapp_surf *s, int w, int h) {
    if (w <= 0 || h <= 0 || s->used) return 0;
    s->used = 1;
    s->front = 0;
    s->back = -1;
    s->stall_ms = 0;
    s->w = w;
    s->h = h;
    s->dirty = 0;
    for (int b = 0; b < UAPP_BUFS; b++) { s->busy[b] = 0; s->busy_seq[b] = 0; }
    s->busy[0] = 1;   // the compositor shows buffer 0 from the create on
    for (int b = 0; b < 2; b++)
        if (!buf_make(s, b, w, h)) { bufs_release(s); return 0; }
    return 1;
}

// Makes sure buffer `buf` is `w` x `h` before the client draws into it.
//
// **THE STALE HALF IS THE CLIENT'S PROBLEM.** A resize replaces only the
// buffer being drawn into; the other one is still the old size and is
// the one the client draws into NEXT.

static int buf_ensure(struct uapp_surf *s, int buf, int w, int h) {
    // **THE DIMENSIONS DECIDE, NOT THE LENGTH.** Comparing page-rounded
    // byte counts here made a one-pixel resize look like no resize at
    // all -- and NOBODY WAS TOLD, because the size lived in a second
    // record the client had to keep in step. It does not any more: the
    // present carries it, so this only has to make the memory right.
    if (s->px[buf] && s->px_w[buf] == w && s->px_h[buf] == h) return 1;

    // The OBJECT is replaced only when the rounded length actually
    // moved; rebuilding on every present would hand the app a freshly
    // zeroed buffer each frame, which shows up as a scene that will not
    // hold still.
    uint64_t want = ((uint64_t)w * (uint64_t)h * 4 + 4095) & ~4095ULL;
    if (!s->px[buf] || s->px_bytes[buf] != want) return buf_make(s, buf, w, h);
    s->px_w[buf] = w;
    s->px_h[buf] = h;
    return 1;
}

static unsigned long long g_present_seq;

// The buffer to draw this frame into, sized and wrapped as s->surface;
// 0 when every one is busy, and the caller leaves its frame dirty for
// the release to wake. Prefers the lowest free index, so with prompt
// releases two buffers alternate and the third is never made.
static struct ugfx_surface *surf_back(struct uapp_surf *s) {
    if (s->back < 0) {
        int pick = -1;
        for (int b = 0; b < UAPP_BUFS && pick < 0; b++)
            if (b != s->front && !s->busy[b]) pick = b;
        if (pick < 0) {
            // A RELEASE THAT NEVER CAME (an overflowed event queue) must
            // not freeze the window: after a quarter second the oldest is
            // long past being read, so it is taken back.
            unsigned long long now = sys_monotonic_ns() / 1000000ULL;
            if (!s->stall_ms) s->stall_ms = now ? now : 1;
            if (now - s->stall_ms < 250) return 0;
            for (int b = 0; b < UAPP_BUFS; b++)
                if (b != s->front && (pick < 0 || s->busy_seq[b] < s->busy_seq[pick]))
                    pick = b;
            s->busy[pick] = 0;
        }
        s->stall_ms = 0;
        if (!buf_ensure(s, pick, s->w, s->h)) return 0;
        s->back = pick;
    }
    s->surface = ugfx_surface_for_pixels(s->px[s->back], s->w, s->h);
    return &s->surface;
}

static void bufs_release(struct uapp_surf *s) {
    for (int b = 0; b < UAPP_BUFS; b++) {
        if (!s->px[b]) continue;
        char nm[WIN_BUF_NAME_MAX];
        buf_name(nm, sizeof nm, slot_of(s), b);
        sys_munmap(s->px[b], s->px_bytes[b]);
        sys_shm_unlink(nm);
        s->px[b] = 0;
    }
    s->used = 0;
    s->done = 0;
    s->owner = 0;
}

struct uapp {
    const struct uapp_desc *desc;
    uint32_t window;
    int w, h;
    // The TOPLEVEL's back buffer -- a copy of TOPLEVEL->surface kept
    // here because every draw callback is handed `&a->surface` and the
    // two are refreshed together in present() and uapp_resize().
    struct ugfx_surface surface;
    int dirty;    // something asked for a repaint since the last present
    int focused;  // keyboard focus, per WIN_EV_FOCUS
    int running;
    int status;
    int timer_armed; // TWS accepted a WIN_REQ_TIMER, so on_tick arrives
    int poll_paused; // uapp_poll_pause(): park in the blocking wait instead
                     // as an event and the loop can block

    // Pointer routing (ui/uui_route.h). Empty unless the app declared
    // widgets, so an app that does its own hit-testing is untouched.
    struct uui_router router;
    // The last cursor position seen, because a WHEEL event carries
    // notches and no coordinates -- and "which widget is under the
    // cursor" is the only sane answer to where a wheel goes.
    int mouse_x, mouse_y;

    // The WIN_CURSOR_* last named, so uapp_set_cursor() can drop the
    // no-op -- an app calls it on every motion event.
    int cursor;
    int cursor_before_busy;
    // While a motion is being dispatched: the cursor it has settled on so
    // far, sent once when it ends. -1 when not in a motion. Without it the
    // tree's arrow and an app's I-beam over the same spot were BOTH sent
    // on every move.
    int motion_cursor;
    int fullscreen;    // as last asked for -- the compositor's proposal follows
    // The lease (WIN_EV_SCANOUT): the toplevel draws into the display's
    // buffer at WIN_FB_VADDR + lease_back * WIN_FB_BUFFER_STRIDE and
    // presents with WIN_REQ_FB_PRESENT, which answers the next back.
    int lease_on;
    uint32_t lease_pitch;
    int lease_count, lease_back;
};

// One process, one TOPLEVEL -- plus its popups, which are surfaces of
// the same app (g_surf[] above) rather than apps of their own. A single
// static instance rather than a heap allocation: there is no allocator
// in libsys, and one toplevel per process makes the multi-instance
// machinery the kernel-space apps need unnecessary here. The handle is
// opaque precisely so a future uapp_window_create() can appear without
// this API changing shape.
static struct uapp g_app;

// The leased buffer as a surface: as many pixels wide as the PITCH
// holds, clipped to the window, so a padded stride draws right without
// the surface type learning about padding.
static struct ugfx_surface lease_surface(struct uapp *a) {
    void *px = (void *)(uintptr_t)(WIN_FB_VADDR + (uint64_t)a->lease_back * WIN_FB_BUFFER_STRIDE);
    struct ugfx_surface s = ugfx_surface_for_pixels(px, (int)(a->lease_pitch / 4), a->h);
    ugfx_set_clip_rect(&s, 0, 0, a->w, a->h);
    return s;
}

// --- TWP plumbing, in one place instead of once per client ------------

static void req_clear(struct win_request_msg *req) {
    for (unsigned i = 0; i < sizeof(*req); i++) ((uint8_t *)req)[i] = 0;
}

static int req_send(struct win_request_msg *req) {
    return sys_win_request(req);
}

// Fill a request's `text` from a NUL-terminated string, truncating to
// fit. Shared by the title and the app id -- both ride that one field
// (never at the same time) and both truncate rather than fail.
static void copy_text(char *dst, const char *src) {
    int i = 0;
    for (; src && src[i] && i < WIN_TITLE_LEN - 1; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static void layout_log_flush(int src);
static void wmap_sync(struct uapp *a);
static int wmchan_send(uint32_t type, uint32_t window,
                       int aa, int bb, int cc, const char *text);

// **THE FRAME NAMES ITSELF.** A present says which buffer holds the
// finished pixels, which OBJECT is behind it, and how big it is -- so
// the compositor adopts the geometry of what it is about to show and
// never a size it was promised earlier. The kernel used to hold that
// per buffer and answer from its copy; nothing does now.
//
// FIRE AND FORGET, which is what lets it run once a frame per client.
// The flip is this side's: the buffer just handed over is the one the
// compositor reads, so the next frame goes into the other.
static int surf_present(struct uapp_surf *s) {
    int shown = s->back;        // the one that has been drawn into
    if (shown < 0 || !s->px[shown]) return 0;

    // A FULL RING MEANS NO FLIP. Dropping the frame is fine -- the next
    // present supersedes it -- but flipping anyway would leave the
    // compositor reading the buffer this process is about to draw into,
    // which is the tearing double buffering exists to remove.
    // THE SEQUENCE TRAVELS, so a late release of an EARLIER present of
    // this buffer cannot free it while the compositor shows this one.
    unsigned long long seq = g_present_seq + 1;
    if ((uint32_t)seq == 0) seq++;   // 0 means "no sequence" on the wire
    if (!wmchan_send(WIN_REQ_PRESENT, (uint32_t)slot_of(s),
                     WIN_PRESENT_B(shown, s->px_gen[shown]),
                     (int)WIN_PRESENT_SIZE(s->w, s->h), (int)(uint32_t)seq, 0)) return 0;

    g_present_seq = seq;
    s->front = shown;
    s->busy[shown] = 1;
    s->busy_seq[shown] = seq;
    s->back = -1;     // the next frame picks one that has come back
    s->dirty = 0;
    return 1;
}

static void present(struct uapp *a) {
    TOPLEVEL->w = a->w;
    TOPLEVEL->h = a->h;
    if (a->lease_on) {
        // The display's buffer: publish the whole screen and take the
        // next back the kernel names (three buffers, a mailbox flip --
        // see win_surface.c). The shm buffers stay as they were for the
        // day the lease ends.
        struct win_request_msg pr;
        for (unsigned i = 0; i < sizeof pr; i++) ((uint8_t *)&pr)[i] = 0;
        pr.type = WIN_REQ_FB_PRESENT;
        pr.a = 0; pr.b = 0; pr.c = a->w; pr.d = a->h;
        if (sys_win_request(&pr) == 0) {
            if ((int)pr.window < a->lease_count) a->lease_back = (int)pr.window;
            a->surface = lease_surface(a);
        }
    } else {
        surf_present(TOPLEVEL);
    }
    // THE POPUPS AFTER THE TOPLEVEL, in slot order -- the order they
    // were opened, which is the order the compositor stacks them. Only
    // those drawn into this frame: a menu nobody hovered is unchanged
    // and re-presenting it would be a wake-up for nothing.
    for (int i = 1; i < WIN_CLIENT_MAX; i++)
        if (g_surf[i].used && g_surf[i].dirty) surf_present(&g_surf[i]);
}

// Draw + present, but only if something actually asked. This is the
// coalescing uapp_redraw() promises: a burst of events costs one round
// trip, not one per event.
static void flush(struct uapp *a) {
    if (!a->dirty) return;
    // NO FREE BUFFER, NO FRAME: it stays dirty and is drawn when the
    // compositor hands one back (WIN_EV_BUF_RELEASE wakes the loop).
    if (!a->lease_on) {
        struct ugfx_surface *sf = surf_back(TOPLEVEL);
        if (!sf) return;
        a->surface = *sf;
    }
    a->dirty = 0;

    // ORDER, and it is load-bearing: clear, then the APP's own painting,
    // then the widgets, then overlays.
    //
    // The app paints UNDER its widgets, which makes the failure that
    // produced this comment structurally impossible. It had already
    // happened twice: an app whose on_draw begins by clearing the
    // surface -- the natural first line, and what every client wrote
    // before the toolkit cleared for them -- wiped everything the
    // toolkit had just drawn, and the window came up completely blank
    // with no error anywhere. UI Demo shipped that way and a 35-check
    // suite passed it, because every check asserted on the app's LOG
    // and the widgets were live, hit-testable and simply invisible.
    //
    // With the app first, a stray clear can only ever wipe its own
    // backdrop. An app that genuinely needs to paint OVER a widget says
    // so with on_draw_over.
    if (a->desc->layout || a->router.count) ugfx_fill(&a->surface, UTHEME_PANEL_BG);

    if (a->desc->on_draw) {
        struct uapp_draw d = { &a->surface, UTHEME_TEXT, UTHEME_PANEL_BG };
        a->desc->on_draw(a, &d);
    }

    if (a->desc->layout) uui_layout_draw(&a->surface, a->desc->layout);
    if (a->router.count) uui_router_draw(&a->router, &a->surface);

    // The escape hatch, deliberately separate and deliberately last: a
    // status line over a canvas, a drag ghost. Rare enough that making
    // it explicit is better than letting every on_draw be ambiguous
    // about whether it runs above or below the widgets.
    if (a->desc->on_draw_over) {
        struct uapp_draw d = { &a->surface, UTHEME_TEXT, UTHEME_PANEL_BG };
        a->desc->on_draw_over(a, &d);
    }
    // The layout block this frame produced, emitted only if it differs
    // from the last -- see uapp_log_layout(). Here rather than in the
    // apps because only the toolkit knows when a frame has ended.
    layout_log_flush(0);
    wmap_sync(a);
    present(a);
}

// --- public: state ----------------------------------------------------

void uapp_redraw(struct uapp *a) { a->dirty = 1; }

// --- posts: the one thing a worker thread may do -----------------------
//
// A private queue in this process, NOT the compositor's inbox: that ring
// has one writer, the compositor, and a second one would race it. The
// worker appends under a spinlock and kicks the loop's wait word;
// the loop drains this before the inbox. Bounded because a worker
// that posts faster than the loop drains is the bug, not the queue.
#define UAPP_POST_MAX 32
static struct { int a0, a1; } g_posts[UAPP_POST_MAX];
static unsigned g_post_head, g_post_tail;   // free-running, like uchan's
static volatile int g_post_lock;
static void loop_kick(void);

static void post_lock(void)   { while (__atomic_exchange_n(&g_post_lock, 1, __ATOMIC_ACQUIRE)) sys_yield(); }
static void post_unlock(void) { __atomic_store_n(&g_post_lock, 0, __ATOMIC_RELEASE); }

static int post_take(int *a0, int *a1) {
    post_lock();
    int have = g_post_head != g_post_tail;
    if (have) {
        *a0 = g_posts[g_post_tail % UAPP_POST_MAX].a0;
        *a1 = g_posts[g_post_tail % UAPP_POST_MAX].a1;
        g_post_tail++;
    }
    post_unlock();
    return have;
}

int uapp_post(struct uapp *a, int a0, int a1) {
    (void)a;
    post_lock();
    int room = g_post_head - g_post_tail < UAPP_POST_MAX;
    if (room) {
        g_posts[g_post_head % UAPP_POST_MAX].a0 = a0;
        g_posts[g_post_head % UAPP_POST_MAX].a1 = a1;
        g_post_head++;
    }
    post_unlock();
    if (room) loop_kick();
    return room;
}


// --- the layout log ---------------------------------------------------
//
// **OFF UNLESS A TEST TURNS IT ON, AND DEDUPED WHEN IT IS.** This is the
// "log my geometry so a test can drive me by asking, not by guessing
// pixels" report, and it used to be written on EVERY FRAME,
// unconditionally, to the kernel log. Nine apps do it. The result was
// that `dmesg` on a machine with a window open was mostly one app
// repeating itself -- and `dmesg -w` was a FEEDBACK LOOP: printing a
// line moved the Terminal's caret, which redrew, which logged the new
// caret, which printed a line.
//
// `desktop.layout_log` gates it, off by default, the same call
// `kernel.kbdtap` makes: the cost of recording is trivial, and the
// default is about what the machine SHOWS.
//
// READ ONCE, at first use. A test sets the setting before launching the
// app it intends to watch, which tools/gui_debug.py's enter_gui() does
// for every tool at once. Re-reading per frame would put a syscall on
// the draw path to answer a question whose answer does not change
// during a run.
static int g_layout_log = -1;   // -1 = not yet asked

static int layout_log_enabled(void) {
    if (g_layout_log < 0) {
        // The qualified name: identity is (namespace, name), and the
        // namespace is the registered name of the file it lives in.
        //
        // **usetting_get(), NOT sys_setting().** This setting is
        // DECLARED by /etc/settings.d/desktop.layout_log, so the
        // syscall -- which answers for the kernel's registry alone --
        // reads it as unset and silently turns layout logging off for
        // every GUI tool that depends on it. Fourteen of them failed at
        // once that way.
        char value[SETTING_ABI_VALUE_MAX];
        g_layout_log = (usetting_get("desktop.layout_log", value, sizeof value)
                        && value[0] == 'o' && value[1] == 'n') ? 1 : 0;
    }
    return g_layout_log;
}

// THE BLOCK A FRAME PRODUCES, held so it can be compared with the last
// one. Deduping LINE BY LINE would not work: an app logs several lines
// per frame, and each differs from the line before it, so nothing would
// ever match. What repeats is the whole block, which is exactly what an
// idle window emits over and over.
//
// **AN OVERFLOW SAYS SO.** Dropping what does not fit is silent, and
// what it drops is whatever an app logs LAST -- so a test asserting on
// those lines reads a working app as broken (it did). The marker's room
// is reserved so it cannot itself be the line that will not fit, and it
// goes INSIDE the block so the dedupe covers it: outside, it would
// print every frame.
#define LAYOUT_BLOCK_MAX 8192
#define LAYOUT_BLOCK_MARK "uapp: layout log TRUNCATED -- raise LAYOUT_BLOCK_MAX\n"
#define LAYOUT_BLOCK_USABLE (LAYOUT_BLOCK_MAX - (int)sizeof(LAYOUT_BLOCK_MARK))
static char g_block[LAYOUT_BLOCK_MAX];
static int  g_block_len;
static int  g_block_over;
// THE DEDUPE IS PER SURFACE. A dialog window draws its own frame after
// the toplevel's, so one shared "previous block" would see two different
// reports alternating and never match -- the dedupe would be off exactly
// when two windows are open. Source 0 is the toplevel, 1 any dialog.
#define LAYOUT_LOG_SOURCES 2
static char g_block_prev[LAYOUT_LOG_SOURCES][LAYOUT_BLOCK_MAX];
static int  g_block_prev_len[LAYOUT_LOG_SOURCES];

// The formatted form, which is what every app's own report uses. It
// exists so those lines go through the SAME gate and the same per-frame
// dedupe as the widget walk -- a line written straight to ulogf() is
// one this cannot suppress, and it was about twenty such lines a frame
// that made `dmesg` useless.
void uapp_logf_layout(const char *fmt, ...) {
    if (!layout_log_enabled()) return;
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    uapp_log_layout_line(line);
}

void uapp_log_layout_line(const char *line) {
    if (!layout_log_enabled()) return;
    for (const char *p = line; *p; p++) {
        if (g_block_len >= LAYOUT_BLOCK_USABLE) { g_block_over = 1; return; }
        g_block[g_block_len++] = *p;
    }
}

// Called by the draw path once the app has finished. Emits the block
// only if it differs from the previous frame's.
static void layout_log_flush(int src) {
    if (!g_layout_log || !g_block_len) { g_block_len = g_block_over = 0; return; }
    char *prev = g_block_prev[src];
    int *prev_len = &g_block_prev_len[src];
    if (g_block_over) {
        for (const char *p = LAYOUT_BLOCK_MARK; *p; p++) g_block[g_block_len++] = *p;
        g_block_over = 0;
    }
    int same = (g_block_len == *prev_len);
    for (int i = 0; same && i < g_block_len; i++)
        if (g_block[i] != prev[i]) same = 0;
    if (!same) {
        g_block[g_block_len] = '\0';
        ulog(g_block);
        for (int i = 0; i < g_block_len; i++) prev[i] = g_block[i];
        *prev_len = g_block_len;
    }
    g_block_len = 0;
}

// The layout walk (ui/uui_describe.h has the vocabulary). Every NAMED
// item with a `bounds` op gets `<prefix>: layout <name> x y w h`, then
// whatever its `describe` op adds; containers are entered through
// `children`, so a widget inside a scroll view or a splitter is reported
// like any other. Content-relative; a test adds the window's content
// origin. Unnamed and hidden items are skipped -- a name is what says
// "a test may want this".
static void describe_line(void *ctx, const char *line) {
    (void)ctx;
    uapp_log_layout_line(line);
}

// --- the widget map the compositor answers `gui probe` from ----------
//
// **THE COMPOSITOR CANNOT SEE INSIDE A WINDOW**, so a test could ask it
// what window and what chrome were under a point and never what CONTROL
// was. The client exports its own map instead -- AT-SPI's shape, which
// is also what uui_describe.h is modelled on (abi/win_proto.h's
// WIN_REQ_WIDGET has the reasoning).
//
// SENT ONLY WHEN IT CHANGES. The walk runs every frame because it is a
// few dozen pointer derefs and no formatting; what it produces is
// compared against the last set and the messages go out only on a
// difference, which in practice is a resize or a layout swap. That is
// what keeps this off the per-frame cost of an app that redraws
// constantly, and out of the LOG -- the string-building layout log is
// still opt-in behind `desktop.layout_log`, and deliberately separate.
struct wmap_entry { char name[WIN_TITLE_LEN]; int x, y, w, h; };
static struct wmap_entry g_wmap[WIN_WIDGET_MAX];
static int g_wmap_n;
static struct wmap_entry g_wmap_sent[WIN_WIDGET_MAX];
static int g_wmap_sent_n = -1;   // -1: nothing sent yet, so the first
                                 // walk always reports

static void wmap_add(const char *name, int x, int y, int w, int h) {
    if (g_wmap_n >= WIN_WIDGET_MAX) return;
    struct wmap_entry *e = &g_wmap[g_wmap_n++];
    copy_text(e->name, name);
    e->x = x; e->y = y; e->w = w; e->h = h;
}

// A widget reachable both through `.layout` and through `.widgets` (the
// usual shape: the tree draws it, the flat list routes it) is reported
// ONCE -- the second sighting is skipped, not re-logged.
#define LOG_SEEN_MAX 128
static const void *g_log_seen[LOG_SEEN_MAX];
static int g_log_seen_n;

static int log_seen(const void *w) {
    for (int i = 0; i < g_log_seen_n; i++) if (g_log_seen[i] == w) return 1;
    if (g_log_seen_n < LOG_SEEN_MAX) g_log_seen[g_log_seen_n++] = w;
    return 0;
}

static void log_items(const char *prefix, struct uui_item *items, int count) {
    for (int i = 0; i < count; i++) {
        struct uui_item *it = &items[i];
        if (it->hidden || !it->ops) continue;
        if (it->name && it->ops->bounds && !log_seen(it->widget)) {
            int x, y, w, h;
            it->ops->bounds(it->widget, &x, &y, &w, &h);
            char line[96];
            snprintf(line, sizeof line, "%s: layout %s %d %d %d %d\n",
                     prefix, it->name, x, y, w, h);
            uapp_log_layout_line(line);
            if (it->ops->describe) {
                struct uui_describe d = { describe_line, 0, prefix, it->name };
                it->ops->describe(it->widget, &d);
            }
        }
        if (it->ops->children) {
            int n = 0;
            struct uui_item *sub = it->ops->children(it->widget, &n);
            if (sub) log_items(prefix, sub, n);
        }
    }
}

static void wmap_items(struct uui_item *items, int count) {
    for (int i = 0; i < count; i++) {
        struct uui_item *it = &items[i];
        if (it->hidden || !it->ops) continue;
        if (it->name && it->ops->bounds && !log_seen(it->widget)) {
            int x, y, w, h;
            it->ops->bounds(it->widget, &x, &y, &w, &h);
            wmap_add(it->name, x, y, w, h);
        }
        if (it->ops->children) {
            int n = 0;
            struct uui_item *sub = it->ops->children(it->widget, &n);
            if (sub) wmap_items(sub, n);
        }
    }
}

// Walk, compare, and report only a change. Called from the draw path.
static void wmap_sync(struct uapp *a) {
    const struct uapp_desc *d = a->desc;
    if (!d) return;
    g_wmap_n = 0;
    g_log_seen_n = 0;
    if (d->layout) {
        struct uui_item root = { .ops = &uui_layout_ops, .widget = (void *)d->layout };
        wmap_items(&root, 1);
    }
    if (d->widgets && d->widget_count) wmap_items(d->widgets, d->widget_count);

    int same = (g_wmap_n == g_wmap_sent_n);
    for (int i = 0; same && i < g_wmap_n; i++) {
        const struct wmap_entry *x = &g_wmap[i], *y = &g_wmap_sent[i];
        if (x->x != y->x || x->y != y->y || x->w != y->w || x->h != y->h ||
            strcmp(x->name, y->name) != 0) same = 0;
    }
    if (same) return;

    wmchan_send(WIN_REQ_WIDGET_RESET, a->window, 0, 0, 0, 0);
    for (int i = 0; i < g_wmap_n; i++) {
        const struct wmap_entry *e = &g_wmap[i];
        wmchan_send(WIN_REQ_WIDGET, a->window, e->x, e->y,
                    WIN_WIDGET_WH(e->w, e->h), e->name);
        g_wmap_sent[i] = *e;
    }
    g_wmap_sent_n = g_wmap_n;
}

void uapp_log_widget(struct uapp *a, const char *prefix, const char *name,
                     const struct uui_widget_ops *ops, const void *widget) {
    (void)a;
    if (!layout_log_enabled() || !ops || !ops->bounds) return;
    int x, y, w, h;
    ops->bounds(widget, &x, &y, &w, &h);
    char line[96];
    snprintf(line, sizeof line, "%s: layout %s %d %d %d %d\n", prefix, name, x, y, w, h);
    uapp_log_layout_line(line);
    if (ops->describe) {
        struct uui_describe d = { describe_line, 0, prefix, name };
        ops->describe(widget, &d);
    }
}

void uapp_log_layout(struct uapp *a, const char *prefix) {
    if (!layout_log_enabled()) return;
    g_log_seen_n = 0;
    if (a->desc->layout) log_items(prefix, a->desc->layout->items, a->desc->layout->count);
    if (a->router.count) log_items(prefix, a->router.items, a->router.count);
}

void uapp_flush(struct uapp *a) { flush(a); }

void uapp_quit(struct uapp *a, int status) {
    a->running = 0;
    a->status = status;
}

void *uapp_state(struct uapp *a) { return a->desc->state; }
int uapp_focused(const struct uapp *a) { return a->focused; }
int uapp_width(const struct uapp *a) { return a->w; }
int uapp_height(const struct uapp *a) { return a->h; }

// THE SCREEN BOUNDS A BUFFER, not a constant: one bigger than the
// screen is memory nobody can see (xdg-shell's configure_bounds), and
// WIN_CLIENT_MAX_W/H is only the ceiling when the screen cannot be
// asked. Asked FRESH every time, never cached: the WM resizes a
// maximized window BEFORE it forwards WIN_EV_SCREEN, so a cached size
// would still be the old screen's when the bigger resize arrives.
static void clamp_to_screen(int *w, int *h) {
    int mw = WIN_CLIENT_MAX_W, mh = WIN_CLIENT_MAX_H;
    struct query_display d;
    if (sys_query_record(QUERY_DISPLAY, 0, &d, sizeof d) >= (int)sizeof d &&
        d.width > 0 && d.height > 0) {
        if ((int)d.width < mw) mw = (int)d.width;
        if ((int)d.height < mh) mh = (int)d.height;
    }
    if (*w > mw) *w = mw;
    if (*h > mh) *h = mh;
}

// **A RESIZE SENDS NOTHING.** It rebuilds the buffer the client is about
// to draw into and re-lays the page out; the new size reaches the
// compositor on the next PRESENT, drawn at it. WIN_REQ_RESIZE existed
// to keep the server's second copy of each buffer's size in step, and
// there is no second copy.
//
// Only the BACK buffer: the front is still showing the last finished
// frame at the old size, and replacing it would be the window of black
// this whole handshake exists to avoid (abi/win_proto.h's
// configure/ack).
//
// **THE CLIENT CLAMPS ITS OWN SIZE NOW.** The kernel refused an
// oversized window because it was allocating the memory; the memory is
// this process's, so the bound is checked where the allocation is.
int uapp_resize(struct uapp *a, int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    clamp_to_screen(&w, &h);

    // The buffer being drawn into, if this frame has one; otherwise the
    // next frame's surf_back() sizes whichever comes back.
    int back = TOPLEVEL->back;
    if (back >= 0 && !buf_make(TOPLEVEL, back, w, h)) return 0;

    a->w = w;
    a->h = h;
    TOPLEVEL->w = w;
    TOPLEVEL->h = h;
    if (back >= 0) TOPLEVEL->surface = ugfx_surface_for_pixels(TOPLEVEL->px[back], w, h);
    a->surface = a->lease_on ? lease_surface(a) : TOPLEVEL->surface;
    if (a->desc->layout) uui_layout_run(a->desc->layout, 0, 0, a->w, a->h);
    return 1;
}


// THE COMPOSITOR'S CHANNEL, opened once and kept. Lazily, because a
// client that never sets a title should not publish a ring, and because
// the compositor may not have its beacon up when an app starts.
static struct uchan_client g_wmchan;
static int g_wmchan_state;   // 0 untried, 1 open, -1 no compositor channel

static void loop_kick(void) { uchan_client_kick(&g_wmchan); }

// How long a round trip waits. Generous because the cost of being wrong
// is one-sided in both places: giving up early opens a duplicate window
// or an app with none, where waiting only delays a startup. Nothing but
// a wedged compositor spends it.
#define UAPP_CALL_TIMEOUT_MS 1000

static int wmchan(void) {
    if (g_wmchan_state) return g_wmchan_state > 0;
    g_wmchan_state = uchan_client_open(&g_wmchan, WMCHAN_SERVICE) == 0 ? 1 : -1;
    return g_wmchan_state > 0;
}

// WHO TO GRANT A WINDOW BUFFER TO. The beacon this channel is already
// opened through carries the server's pid, so a client needs no new way
// to learn it -- and a compositor with no beacon is one that could not
// be sent a title either.
static int comp_pid(void) {
    return wmchan() ? g_wmchan.beacon->server_pid : 0;
}

// Sends one request over the channel. Returns 1 if it went, 0 if there
// is no compositor channel or its ring is full.
static int wmchan_send(uint32_t type, uint32_t window,
                       int aa, int bb, int cc, const char *text) {
    if (!wmchan()) return 0;
    struct wmchan_msg m;
    memset(&m, 0, sizeof m);
    m.type = type;
    m.window = window;
    m.a = aa; m.b = bb; m.c = cc;
    if (text) snprintf(m.text, sizeof m.text, "%s", text);
    return uchan_send(&g_wmchan, &m, sizeof m) == 0;
}

int uapp_notice(struct uapp *a, int kind, unsigned flags, const char *path) {
    (void)a;
    int piece = WIN_TITLE_LEN - 1;
    int len = (int)strlen(path);
    int pieces = (len + piece - 1) / piece;
    if (pieces < 1) pieces = 1;
    if (pieces > WIN_NOTICE_PIECES_MAX) return 0;
    for (int i = 0; i < pieces; i++) {
        char part[WIN_TITLE_LEN];
        int n = len - i * piece;
        if (n > piece) n = piece;
        if (n < 0) n = 0;
        memcpy(part, path + i * piece, (size_t)n);
        part[n] = '\0';
        if (!wmchan_send(WIN_REQ_NOTICE, 0, i | (pieces << 8), kind, (int)flags, part)) return 0;
    }
    return 1;
}

// The two requests that need an answer (lib/uwmchan.h). Returns the
// compositor's `a`, or `fail` when there is no channel or no reply --
// both of which the callers read as a refusal.
static int wmchan_call(uint32_t type, uint32_t window, int aa, int bb,
                       const char *text, int fail) {
    if (!wmchan()) return fail;
    struct wmchan_msg m, r;
    memset(&m, 0, sizeof m);
    m.type = type;
    m.window = window;
    m.a = aa;
    m.b = bb;
    if (text) snprintf(m.text, sizeof m.text, "%s", text);
    if (uchan_call(&g_wmchan, &m, sizeof m, &r, sizeof r,
                   UAPP_CALL_TIMEOUT_MS) < 0) return fail;
    return r.a;
}

// --- popup surfaces: the provider ui/uui_popup.h asks through --------
//
// A popup is a slot above 0 with its own two buffers, opened by the
// third round trip (WIN_REQ_POPUP) and placed by the compositor; the
// reply's offset is what turns its input back into toplevel coordinates
// in dispatch(). Widgets never see a slot: they get an id, which IS the
// slot, and a surface to draw into.

static int popup_open(void *ctx, int ax, int ay, int aw, int ah, int w, int h,
                      int gravity, unsigned flags, void (*done)(void *owner),
                      void *owner, int *out_x, int *out_y) {
    (void)ctx;
    if (w <= 0 || h <= 0 || comp_pid() <= 0) return 0;
    clamp_to_screen(&w, &h);
    struct uapp_surf *s = 0;
    for (int i = 1; i < WIN_CLIENT_MAX; i++)
        if (!g_surf[i].used) { s = &g_surf[i]; break; }
    if (!s) return 0;
    if (!bufs_create(s, w, h)) return 0;

    struct wmchan_msg m, r;
    memset(&m, 0, sizeof m);
    m.type = WIN_REQ_POPUP;
    m.window = (uint32_t)slot_of(s);
    m.a = w;
    m.b = h;
    m.c = (int32_t)(g_popup_parent ? g_popup_parent : g_app.window);
    m.pos.ax = ax; m.pos.ay = ay; m.pos.aw = aw; m.pos.ah = ah;
    m.pos.gravity = gravity == UUI_POPUP_RIGHT ? WIN_POPUP_RIGHT : WIN_POPUP_BELOW;
    _Static_assert(UUI_POPUP_GLASS_PX == WIN_GLASS_PX, "one glass mark, two names");
    m.pos.flags = ((flags & UUI_POPUP_GRAB) ? WIN_POPUP_GRAB : 0) |
                  ((flags & UUI_POPUP_GLASS) ? WIN_POPUP_GLASS : 0);
    if (!wmchan() ||
        uchan_call(&g_wmchan, &m, sizeof m, &r, sizeof r, UAPP_CALL_TIMEOUT_MS) < 0 ||
        r.a < 0) {
        bufs_release(s);
        return 0;
    }
    s->ox = r.b;
    s->oy = r.c;
    s->parent = g_popup_parent ? g_popup_parent : g_app.window;
    s->done = done;
    s->owner = owner;
    surf_back(s);
    if (out_x) *out_x = r.b;
    if (out_y) *out_y = r.c;
    return slot_of(s);
}

static void popup_close(void *ctx, int id) {
    (void)ctx;
    if (id <= 0 || id >= WIN_CLIENT_MAX || !g_surf[id].used) return;
    popup_press_closed(id);
    wmchan_send(WIN_REQ_DESTROY, (uint32_t)id, 0, 0, 0, 0);
    bufs_release(&g_surf[id]);
    g_app.dirty = 1;   // the widget that owned it repaints its own state
}

static struct ugfx_surface *popup_surface(void *ctx, int id) {
    (void)ctx;
    if (id <= 0 || id >= WIN_CLIENT_MAX || !g_surf[id].used) return 0;
    struct uapp_surf *s = &g_surf[id];
    struct ugfx_surface *sf = surf_back(s);
    if (!sf) { g_app.dirty = 1; return 0; }   // drawn once a buffer is back
    s->dirty = 1;   // present() sends it after the toplevel
    return sf;
}

// --- dialog windows: a second toplevel (ui/uapp.h) -------------------
//
// The same per-slot surface machinery a popup uses, with the compositor
// told to give this one chrome instead (WIN_REQ_DIALOG). What is NOT
// shared with the toplevel is the content: its own item array, its own
// router and its own focus ring, because it is a different window --
// a second view of the first one's widgets would be a split view, not
// a dialog.
struct uapp_window {
    int slot;               // 0 = closed; also the `window` its events name
    struct uapp *app;
    struct uapp_window_desc desc;
    char title[WIN_TITLE_LEN];
    struct uui_router router;
    int dirty;
    int cursor;
    int mouse_x, mouse_y;   // a WHEEL carries notches and no position
};
static struct uapp_window g_dlg[WIN_CLIENT_MAX];

static struct uapp_window *dlg_for(uint32_t slot) {
    if (slot == 0 || slot >= WIN_CLIENT_MAX) return 0;
    return g_dlg[slot].slot ? &g_dlg[slot] : 0;
}

// Places the content and paints it. The same order flush() uses for the
// toplevel, minus the app hooks a dialog has no equivalent of.
static void dlg_flush(struct uapp_window *w) {
    if (!w->slot || !w->dirty) return;
    w->dirty = 0;
    struct uapp_surf *s = &g_surf[w->slot];
    if (!surf_back(s)) { w->dirty = 1; return; }   // drawn once a buffer is back
    if (w->desc.layout) uui_layout_run(w->desc.layout, 0, 0, s->w, s->h);
    if (w->desc.log_prefix && layout_log_enabled()) {
        g_log_seen_n = 0;
        if (w->desc.layout) log_items(w->desc.log_prefix, w->desc.layout->items,
                                      w->desc.layout->count);
        if (w->router.count) log_items(w->desc.log_prefix, w->router.items, w->router.count);
        if (w->desc.on_log_layout) w->desc.on_log_layout(w);
    }
    ugfx_fill(&s->surface, UTHEME_PANEL_BG);
    if (w->desc.layout) uui_layout_draw(&s->surface, w->desc.layout);
    if (w->router.count) uui_router_draw(&w->router, &s->surface);
    layout_log_flush(1);
    surf_present(s);
}

static int ids_unique(const struct uui_router *r, const char *who);   // below
static int buttons_heard(const struct uui_router *r, int has_on_action, const char *who);

struct uapp_window *uapp_window_open(struct uapp *a, const struct uapp_window_desc *desc) {
    if (!a || !desc || desc->w <= 0 || desc->h <= 0 || comp_pid() <= 0) return 0;
    int w = desc->w, h = desc->h;
    clamp_to_screen(&w, &h);
    struct uapp_surf *s = 0;
    for (int i = 1; i < WIN_CLIENT_MAX; i++)
        if (!g_surf[i].used) { s = &g_surf[i]; break; }
    if (!s) return 0;
    if (!bufs_create(s, w, h)) return 0;
    int slot = slot_of(s);

    struct uapp_window *d = &g_dlg[slot];
    memset(d, 0, sizeof *d);
    d->desc = *desc;
    strlcpy(d->title, desc->title ? desc->title : "Dialog", sizeof d->title);

    struct wmchan_msg m, r;
    memset(&m, 0, sizeof m);
    m.type = WIN_REQ_DIALOG;
    m.window = (uint32_t)slot;
    m.a = w;
    m.b = h;
    m.c = WIN_DIALOG_C(a->window, (desc->flags & UAPP_WIN_MODAL) ? WIN_DIALOG_MODAL : 0);
    strlcpy(m.text, d->title, sizeof m.text);
    if (!wmchan() ||
        uchan_call(&g_wmchan, &m, sizeof m, &r, sizeof r, UAPP_CALL_TIMEOUT_MS) < 0 ||
        r.a < 0) {
        bufs_release(s);
        d->slot = 0;
        return 0;
    }

    d->slot = slot;
    d->app = a;
    d->cursor = WIN_CURSOR_DEFAULT;
    if (desc->widgets && desc->widget_count > 0) {
        uui_router_init(&d->router, desc->widgets, desc->widget_count);
        // Too late to refuse the app, so the window routes nothing.
        if (!ids_unique(&d->router, desc->title) ||
            !buttons_heard(&d->router, desc->on_action != 0, desc->title))
            d->router.count = 0;
    }
    d->dirty = 1;
    dlg_flush(d);
    return d;
}

void uapp_window_close(struct uapp_window *w) {
    if (!w || !w->slot) return;
    int slot = w->slot;
    wmchan_send(WIN_REQ_DESTROY, (uint32_t)slot, 0, 0, 0, 0);
    bufs_release(&g_surf[slot]);
    w->slot = 0;
    if (w->app) w->app->dirty = 1;   // the owner repaints, now unblocked
}

int   uapp_window_is_open(const struct uapp_window *w) { return w && w->slot != 0; }
void  uapp_window_redraw(struct uapp_window *w)  { if (w && w->slot) w->dirty = 1; }
void *uapp_window_state(struct uapp_window *w)   { return w ? w->desc.state : 0; }
struct uapp *uapp_window_app(struct uapp_window *w) { return w ? w->app : 0; }
int uapp_window_width(const struct uapp_window *w)  { return w && w->slot ? g_surf[w->slot].w : 0; }
int uapp_window_height(const struct uapp_window *w) { return w && w->slot ? g_surf[w->slot].h : 0; }

void uapp_window_set_title(struct uapp_window *w, const char *title) {
    if (!w || !w->slot || !title) return;
    strlcpy(w->title, title, sizeof w->title);
    wmchan_send(WIN_REQ_TITLE, (uint32_t)w->slot, 0, 0, 0, w->title);
}

// One event, in the dialog's OWN coordinate space -- it is a toplevel,
// so nothing is translated. Its widgets are routed exactly as the
// toplevel's are; what a dialog has no equivalent of (scanout, resize,
// drag brokering) is simply not offered.
// --- telling the app what a widget did ------------------------------------
//
// A LONE BUTTON'S COMMIT IS A COMMAND: on_action, with the button's code.
// Its press, hover and a press dragged off reach the app not at all -- the
// button drew them. Every other widget's change is on_widget's. One number
// space for both is how Crash Reports' table (id 1) arrived as its Open
// command (1) on every hover. `committed` is the router's word for it: a
// release while armed, or a key the button took.
static const struct uui_button *lone_button(const struct uui_router *r, int id) {
    const struct uui_item *it = uui_router_item(r, id);
    return it && it->ops == &uui_button_ops ? it->widget : 0;
}

// MOTION reaches the app while a button is held (a drag, a thumb), or
// for an item that asked to hear hovers (UUI_TRACK_HOVER).
static int motion_wanted(const struct uui_router *r, int id, unsigned held) {
    if (held) return 1;
    const struct uui_item *it = uui_router_item(r, id);
    return it && (it->flags & UUI_TRACK_HOVER);
}

static void tell_app(struct uapp *a, int id, int reason, int committed) {
    const struct uapp_desc *d = a->desc;
    if (!id) return;
    const struct uui_button *b = lone_button(&a->router, id);
    if (b) { if (committed && d->on_action) d->on_action(a, b->code); return; }
    if (d->on_widget) d->on_widget(a, id, reason);
}

static void tell_window(struct uapp_window *w, int id, int reason, int committed) {
    const struct uapp_window_desc *d = &w->desc;
    if (!id) return;
    const struct uui_button *b = lone_button(&w->router, id);
    if (b) { if (committed && d->on_action) d->on_action(w, b->code); return; }
    if (d->on_widget) d->on_widget(w, id, reason);
}

// IDS ARE UNIQUE, or the lookup above names the wrong widget. Refused
// loudly at startup rather than misrouted later -- a resource compiler's
// duplicate-id warning, made fatal.
static int ids_unique(const struct uui_router *r, const char *who) {
    int dup = uui_router_duplicate_id(r);
    if (!dup) return 1;
    ulogf("uapp: BUG: %s declares widget id %d twice -- refusing to start\n",
          who ? who : "an app", dup);
    return 0;
}

// A LONE BUTTON WITH NOBODY TO HEAR IT is a button that silently does
// nothing -- the shape check_key_routing.py catches for keys, checked
// here because only the running app knows which files linked together.
static int buttons_heard(const struct uui_router *r, int has_on_action, const char *who) {
    if (has_on_action || !uui_router_has_ops(r, &uui_button_ops)) return 1;
    ulogf("uapp: BUG: %s declares a button and no on_action -- refusing to start\n",
          who ? who : "an app");
    return 0;
}

static void dlg_dispatch(struct uapp_window *w, const struct win_event *ev) {
    const struct uapp_window_desc *d = &w->desc;
    switch (ev->type) {
    case WIN_EV_PING:
        wmchan_send(WIN_REQ_PONG, (uint32_t)w->slot, (int)ev->a, 0, 0, 0);
        return;
    case WIN_EV_CLOSE:
        if (d->on_close) d->on_close(w);
        else uapp_window_close(w);
        return;
    case WIN_EV_KEY: {
        if (uui_key_is_shortcut(ev->a, ev->mods)) {   // as dispatch() does
            if (d->on_key) d->on_key(w, ev->a, ev->mods);
            return;
        }
        if (w->router.count) {
            int changed = 0;
            int id = uui_router_overlay_key(&w->router, ev->a, ev->mods, &changed);
            if (changed) w->dirty = 1;
            if (id) {
                tell_window(w, id, UUI_REASON_KEY, 0);
                return;
            }
        }
        if (d->focus && uui_focus_key(d->focus, ev->a, ev->mods)) {
            w->dirty = 1;
            if (ev->a != '\t' && d->on_widget && d->focus->current >= 0) {
                int id = uui_router_id_of(&w->router,
                                          d->focus->items[d->focus->current].widget);
                tell_window(w, id, UUI_REASON_KEY, 1);
            }
        }
        if (d->on_key) d->on_key(w, ev->a, ev->mods);
        return;
    }
    case WIN_EV_MOUSE_DOWN: {
        w->mouse_x = ev->a;
        w->mouse_y = ev->b;
        unsigned btns = WIN_MOUSE_BUTTONS(ev->mods);
        unsigned kmods = WIN_MOUSE_MODS(ev->mods);
        if (!(btns & 0x1)) return;
        int changed = 0;
        int id = w->router.count
                     ? uui_router_press(&w->router, ev->a, ev->b, kmods, &changed) : 0;
        if (changed) w->dirty = 1;
        tell_window(w, id, UUI_REASON_PRESS, 0);
        if (d->focus && !g_press_slot && uui_focus_click(d->focus, ev->a, ev->b)) w->dirty = 1;
        return;
    }
    case WIN_EV_MOUSE_MOVE: {
        if (ev->a < 0 || ev->b < 0) return;   // a leave carries no position
        w->mouse_x = ev->a;
        w->mouse_y = ev->b;
        unsigned held = ev->mods & 0x1;
        if (!w->router.count) return;
        int changed = 0;
        int id = uui_router_motion(&w->router, ev->a, ev->b, held,
                                   WIN_MOUSE_MODS(ev->mods), &changed);
        if (changed) w->dirty = 1;
        if (motion_wanted(&w->router, id, held)) tell_window(w, id, UUI_REASON_MOTION, 0);
        int want = uui_router_cursor(&w->router, ev->a, ev->b);
        if (want != w->cursor && want >= 0 && want < WIN_CURSOR_COUNT) {
            w->cursor = want;
            wmchan_send(WIN_REQ_CURSOR, (uint32_t)w->slot, want, 0, 0, 0);
        }
        return;
    }
    case WIN_EV_MOUSE_UP: {
        w->mouse_x = ev->a;
        w->mouse_y = ev->b;
        if (!w->router.count) return;
        int changed = 0;
        int id = uui_router_release(&w->router, ev->a, ev->b, &changed);
        if (changed) w->dirty = 1;
        tell_window(w, id, UUI_REASON_RELEASE, changed);
        return;
    }
    case WIN_EV_WHEEL: {
        if (!w->router.count) return;
        int changed = 0;
        int id = uui_router_wheel(&w->router, w->mouse_x, w->mouse_y,
                                  (int)ev->a, &changed);
        if (changed) w->dirty = 1;
        tell_window(w, id, UUI_REASON_WHEEL, 0);
        return;
    }
    default:
        return;
    }
}

static const struct uui_popup_ops g_popup_ops = {
    .open = popup_open,
    .close = popup_close,
    .surface = popup_surface,
};

// The compositor dismissed popup `id` (WIN_EV_POPUP_DONE) -- or asked it
// to close, which for a popup means the same. The window is already
// gone, so no DESTROY goes back; the widget is told and the buffers
// released.
static void popup_dismissed(int id) {
    if (id <= 0 || id >= WIN_CLIENT_MAX || !g_surf[id].used) return;
    struct uapp_surf *s = &g_surf[id];
    void (*done)(void *) = s->done;
    void *owner = s->owner;
    popup_press_closed(id);
    bufs_release(s);
    if (done) done(owner);
    g_app.dirty = 1;
}

int uapp_press_on_popup(struct uapp *a) {
    (void)a;
    return g_press_slot != 0;
}

int uapp_drag_active(struct uapp *a) {
    return uui_router_drag_active(&a->router);
}

const struct uui_drag *uapp_drag(struct uapp *a) {
    if (uui_router_drag_active(&a->router)) return uui_router_drag(&a->router);
    return uui_router_dropped(&a->router);
}

void uapp_set_cursor(struct uapp *a, int cursor) {
    if (cursor < 0 || cursor >= WIN_CURSOR_COUNT) return;
    if (a->motion_cursor >= 0) { a->motion_cursor = cursor; return; }
    if (a->cursor == cursor) return;
    // Either way: a server that refuses this (one built before the
    // request existed) must not be asked again on every motion.
    a->cursor = cursor;
    wmchan_send(WIN_REQ_CURSOR, a->window, cursor, 0, 0, 0);
}

// **ASK THE COMPOSITOR TO STOP EATING GLOBAL SHORTCUTS.** Only a program
// that must RECEIVE them has any business calling this -- in this tree
// that is System Settings' shortcut capture, which cannot record Super+E
// while the compositor is busy launching a file manager with it.
//
// It lapses on its own when this window loses the focus, so a caller
// that forgets to release it cannot leave the desktop without
// shortcuts (abi/win_proto.h). Releasing explicitly is still right:
// the capture control does it the moment it stops listening.
void uapp_inhibit_shortcuts(struct uapp *a, int on) {
    // **SLOT 0 IS A REAL WINDOW.** `a->window` is the compositor's slot
    // number and the first window gets 0, so a `!a->window` guard here
    // refused the only window most apps have -- which presented as the
    // capture control arming and the compositor carrying on eating the
    // very keys it was waiting for.
    if (!a) return;
    wmchan_send(WIN_REQ_INHIBIT_SHORTCUTS, a->window, on ? 1 : 0, 0, 0, 0);
}

void uapp_poll_pause(struct uapp *a, int paused) { a->poll_paused = paused ? 1 : 0; }

void uapp_busy_begin(struct uapp *a) {
    a->cursor_before_busy = a->cursor;
    uapp_set_cursor(a, WIN_CURSOR_WAIT);
    // The shape has to be ON THE WIRE before the caller blocks, and
    // uapp_set_cursor() is a syscall, so it already is -- the compositor
    // reads its queue on its own schedule. Nothing to flush.
}

void uapp_busy_end(struct uapp *a) {
    uapp_set_cursor(a, a->cursor_before_busy);
}

void uapp_set_fullscreen(struct uapp *a, int on) {
    if (!a || !a->running) return;   // the toplevel is window 0, so no id test
    on = on ? 1 : 0;
    if (a->fullscreen == on) return;
    a->fullscreen = on;
    wmchan_send(WIN_REQ_FULLSCREEN, a->window, on, 0, 0, 0);
}

// The toolkit's own channel, for a library that must not open a second
// one: a process may hold only ONE ring per server, because the ring is
// named after its pid. lib/ushot.h is the caller.
struct uchan_client *uapp_wmchan(void) {
    return wmchan() ? &g_wmchan : 0;
}

// SWAP THE WIDGET SET. An app with two modes -- a form, and a
// full-window canvas over it -- otherwise has to draw and hit-test the
// second one itself, because `widgets` is fixed at uapp_run() and
// emptying the LAYOUT leaves the router still drawing every item.
//
// The array is the caller's and must outlive the swap, the same
// ownership rule uapp_desc.widgets follows. A count of 0 is legal and
// means "nothing routed and nothing drawn", which is the point.
void uapp_set_widgets(struct uapp *a, struct uui_item *items, int count) {
    if (!a) return;
    uui_router_init(&a->router, items, count > 0 ? count : 0);
    const char *who = a->desc ? a->desc->title : 0;
    if (!ids_unique(&a->router, who) ||
        !buttons_heard(&a->router, a->desc && a->desc->on_action, who))
        a->router.count = 0;
    a->dirty = 1;
}

int uapp_fullscreen(const struct uapp *a) { return a ? a->fullscreen : 0; }
int uapp_scanout(const struct uapp *a)    { return a ? a->lease_on : 0; }

int uapp_set_title(struct uapp *a, const char *title) {
    // OVER THE CHANNEL, which carries the string itself. The kernel path
    // below can only say "it changed" -- struct win_event is 24 bytes --
    // so the compositor has to read the title back out of the kernel,
    // which is why the kernel stores one at all.
    // THE CHANNEL IS THE ONLY PATH NOW. The kernel used to take this
    // request, store the string and tell the compositor to read it back;
    // it stores nothing of the sort any more.
    return wmchan_send(WIN_REQ_TITLE, a->window, 0, 0, 0, title ? title : "");
}

// Asks the window manager to close every window belonging to `pid` --
// Task Manager's "End Task". POLITE: the target may refuse, exactly as
// it may refuse its own X button, because the WM runs the same
// wm_request_close() for all of them. sys_kill() is the half that
// cannot be refused.
//
// Returns 1 if at least one window was asked, 0 if that pid has none
// (a process with no window is not an error -- it simply cannot be
// asked this way, and the caller should say so rather than appear to
// have done something).
int uapp_request_close_pid(struct uapp *a, int pid) {
    (void)a; // not about this app's own window
    // FIRE AND FORGET, so the 1 here means "asked", not "a window was
    // found" -- the compositor owns the list and this side cannot see
    // it. The caller reports the ask; Task Manager's own list is what
    // tells the user whether anything went.
    return wmchan_send(WIN_REQ_CLOSE_PID, 0, pid, 0, 0, 0);
}

// --- public: drawing --------------------------------------------------

struct ugfx_surface *uapp_surface(struct uapp_draw *d) { return d->surface; }

// --- event dispatch ---------------------------------------------------

// THE CLIPBOARD IS POLLED, NOT BROADCAST, and that is a consequence of
// it living in shared memory rather than in the kernel (lib/uclip.h).
// There is no WIN_EV_CLIPBOARD any more because there is nobody in the
// kernel left to send one -- and asking is a single shared-memory read,
// which is cheaper than the event was. Checked on every event, so an
// app hears about a change the moment anything at all happens to it.
static unsigned g_clip_seen;
static int g_clip_first = 1;

static void clip_poll(struct uapp *a) {
    const struct uapp_desc *d = a->desc;
    if (!d->on_clipboard) return;
    unsigned now = uclip_peek_serial();
    if (g_clip_first) { g_clip_first = 0; g_clip_seen = now; return; }
    if (now == g_clip_seen) return;
    g_clip_seen = now;
    // THE REAL OP, peeked rather than loaded. It was hardcoded to 0
    // while uapp.h promised a UCLIP_*, so the one app that believed the
    // header saw every cut as UCLIP_NONE and never drew one as staged.
    d->on_clipboard(a, uclip_peek_op(), now);
}

// --- children this app launched --------------------------------------
//
// See uapp.h. Sixteen is more programs than any app here launches at
// once, and a full table costs one un-reaped slot rather than a failed
// launch -- which is the honest trade, and the same one the desktop's
// larger table makes.
#define UAPP_CHILDREN 16
static int g_children[UAPP_CHILDREN];

void uapp_track_child(struct uapp *a, int pid) {
    (void)a;
    if (pid <= 0) return;
    for (int i = 0; i < UAPP_CHILDREN; i++)
        if (!g_children[i]) { g_children[i] = pid; return; }
    ulogf("uapp: child table full -- pid %d will not be reaped\n", pid);
}

int uapp_spawn(struct uapp *a, const char *path, const char *arg) {
    char *const argv[] = { (char *)path, (char *)arg, 0 };
    int pid = sys_spawn_argv(path, argv);
    if (pid > 0) uapp_track_child(a, pid);
    return pid;
}

static void reap_children(void) {
    for (int i = 0; i < UAPP_CHILDREN; i++) {
        if (!g_children[i]) continue;
        int code = 0;
        // NOHANG, and that distinction is the whole app: the blocking
        // sys_waitpid() parks until the child exits, so reaping one
        // that is merely RUNNING would freeze the window for as long as
        // somebody had the launched program open. SYS_RETRY means
        // "still running" here rather than "ask again".
        if (sys_waitpid_nohang(g_children[i], &code) != SYS_RETRY)
            g_children[i] = 0;
    }
}

static void dispatch(struct uapp *a, const struct win_event *in);

// The release a closed popup owes its press (g_press_slot above).
static void owed_release(struct uapp *a) {
    if (!g_release_owed) return;
    g_release_owed = 0;
    struct win_event up = g_press_up;
    dispatch(a, &up);
}

static void dispatch(struct uapp *a, const struct win_event *in) {
    const struct uapp_desc *d = a->desc;

    clip_poll(a);
    reap_children();

    // Typing or clicking shows the caret solid and restarts its blink,
    // whichever window or widget the event is for (ui/uui_caret.h).
    if (in->type == WIN_EV_KEY || in->type == WIN_EV_MOUSE_DOWN) uui_caret_reset();

    if (in->type == WIN_EV_MOUSE_DOWN && (WIN_MOUSE_BUTTONS(in->mods) & 0x1)) {
        g_press_slot = 0;
        if (in->window != a->window && in->window < WIN_CLIENT_MAX && g_surf[in->window].used) {
            const struct uapp_surf *ps = &g_surf[in->window];
            g_press_slot = in->window;
            g_press_up = *in;
            g_press_up.type = WIN_EV_MOUSE_UP;
            g_press_up.window = ps->parent ? ps->parent : a->window;
            g_press_up.a = in->a + ps->ox;
            g_press_up.b = in->b + ps->oy;
            g_press_up.mods = WIN_MOUSE_MODS(in->mods) << WIN_MOUSE_MODS_SHIFT;
        }
    } else if (in->type == WIN_EV_MOUSE_UP && !(WIN_MOUSE_BUTTONS(in->mods) & 0x1)) {
        g_press_slot = 0;
    }

    // AN EVENT ON A POPUP IS THE TOPLEVEL'S, TRANSLATED. The widgets
    // hit-test one coordinate space -- the toplevel's content -- and a
    // popup's place in it is known from the compositor's reply, so a
    // pointer event arriving popup-local is moved by that offset and
    // then handled exactly as if it had landed on the toplevel; the
    // menu that is open takes it first (ui/uui_route.h). Keys need no
    // translation. What a popup does NOT share: focus (the toplevel
    // keeps it), resize (a popup has one size) and close (which for a
    // popup is a dismissal).
    // A BUFFER CAME BACK, for any surface: the next frame may draw into
    // it. A stale one -- a generation since replaced, or an earlier
    // present of a buffer shown again since -- is ignored.
    if (in->type == WIN_EV_BUF_RELEASE) {
        if (in->window < WIN_CLIENT_MAX && g_surf[in->window].used &&
            in->a >= 0 && in->a < UAPP_BUFS &&
            g_surf[in->window].px_gen[in->a] == (uint32_t)in->b &&
            (!in->mods || in->mods == (uint32_t)g_surf[in->window].busy_seq[in->a]))
            g_surf[in->window].busy[in->a] = 0;
        return;
    }

    struct win_event copy = *in;
    const struct win_event *ev = in;
    if (in->window != a->window) {
        // A DIALOG WINDOW IS ITS OWN TOPLEVEL, so its event is handled
        // in its own coordinates and goes no further -- the translation
        // below is a POPUP's, and applying it here would move a press
        // by the offset of a surface that has none.
        struct uapp_window *dw = dlg_for(in->window);
        if (dw) {
            uint32_t prev = g_popup_parent;
            g_popup_parent = dw->slot;
            dlg_dispatch(dw, in);
            g_popup_parent = prev;
            return;
        }
        // STALE IF THE SLOT IS NOT IN USE: the compositor answered a
        // popup this side has since closed (a menu switched titles while
        // a press was in flight). Read as the toplevel's, its raw
        // coordinates would land on whatever sits at that point in the
        // window -- which was the menu bar.
        if (in->window >= WIN_CLIENT_MAX || !g_surf[in->window].used) return;
        struct uapp_surf *s = &g_surf[in->window];
        switch (in->type) {
        case WIN_EV_MOUSE_MOVE:
            // A LEAVE CARRIES NO POSITION. The compositor says "the
            // pointer left you" as a move to (-1,-1) in the surface's own
            // coordinates, which for the toplevel hit-tests as nothing;
            // moved by a popup's offset it is a REAL point one pixel
            // above-left of the popup, and a menu bar there switched
            // menus every time a submenu was left. wl_pointer.leave has
            // no coordinates for this reason.
            if (in->a < 0 || in->b < 0 || in->a >= s->w || in->b >= s->h) return;
            /* fallthrough */
        case WIN_EV_MOUSE_DOWN:
        case WIN_EV_MOUSE_UP:
            copy.a += s->ox;
            copy.b += s->oy;
            ev = &copy;
            break;
        case WIN_EV_PING:
            wmchan_send(WIN_REQ_PONG, in->window, (int)in->a, 0, 0, 0);
            return;
        case WIN_EV_CLOSE:
            // ASKED to close, which a dismissal is not: the compositor
            // still holds the window and waits for it to go, so it is
            // destroyed here -- or it stays up with nobody drawing it and
            // the compositor calls it hung.
            if ((int)in->window > 0 && (int)in->window < WIN_CLIENT_MAX &&
                g_surf[in->window].used)
                wmchan_send(WIN_REQ_DESTROY, in->window, 0, 0, 0, 0);
            popup_dismissed((int)in->window);
            return;
        case WIN_EV_POPUP_DONE:
            popup_dismissed((int)in->window);
            return;
        case WIN_EV_FOCUS:
        case WIN_EV_RESIZE:
            return;
        default:
            break;
        }
        // BACK TO THE WINDOW THAT OWNS IT, not always the toplevel. A
        // dialog is its own toplevel with its own router, so a popup
        // anchored to one delivers THERE -- and for KEYS as much as for
        // the pointer, because the compositor sends a grabbing popup's
        // keys to the popup's slot. Routing only the pointer left a
        // dropdown in a dialog seeking with keys that reached the main
        // window's widgets instead.
        if (s->parent && s->parent != a->window) {
            struct uapp_window *ow = dlg_for(s->parent);
            if (ow) {
                struct win_event out = *ev;
                out.window = s->parent;
                uint32_t prev = g_popup_parent;
                g_popup_parent = ow->slot;
                dlg_dispatch(ow, &out);
                g_popup_parent = prev;
                return;
            }
        }
    } else if (in->type == WIN_EV_POPUP_DONE) {
        return;   // never for the toplevel
    }

    switch (ev->type) {
    case WIN_EV_SCANOUT:
        if (ev->a) {
            a->lease_on    = 1;
            a->lease_pitch = (uint32_t)ev->b;
            a->lease_count = (int)(ev->mods & 0xFF);
            a->lease_back  = (int)(ev->mods >> 8);
            if (a->lease_count < 1) a->lease_count = 1;
            if (a->lease_back >= a->lease_count) a->lease_back = 0;
            a->surface = lease_surface(a);
        } else {
            a->lease_on = 0;
            a->surface = TOPLEVEL->surface;
        }
        // Whatever the last frame drew is in the OTHER buffer: repaint.
        a->dirty = 1;
        break;
    case WIN_EV_PING: {
        // Answered HERE, with no app involvement and no callback --
        // deliberately. A liveness check an app could forget to answer
        // would report every app that had not been updated as hung, and
        // one an app could answer from a background thread would report
        // a wedged app as healthy (there are no threads here, but the
        // principle is why xdg_shell puts pong in the toolkit too).
        //
        // Answering from the event loop is exactly the right test: this
        // line only runs if the loop is turning. An app stuck inside its
        // own on_draw or on_key never reaches it, which is precisely
        // what "not responding" should mean.
        wmchan_send(WIN_REQ_PONG, a->window, (int)ev->a, 0, 0, 0);
        break;
    }

    case WIN_EV_DRAG_OVER:
    case WIN_EV_DROP: {
        // A drag from ANOTHER window. The files are in the drag slot;
        // the directory they are in and the count come from it, so the
        // fileview's own refusal ("their own directory") still works.
        if (!a->router.count) break;
        static struct uclip c;   // 64 KiB, lib/uclip.h says why static
        static char dir[80], label[80];
        uclip_drag_load(&c);
        int n = uclip_count(&c);
        if (n <= 0 || uclip_kind(&c) != UCLIP_KIND_FILES) break;
        const char *first = uclip_path(&c, 0);
        k_path_dirname(first, dir, sizeof dir);
        if (n == 1) snprintf(label, sizeof label, "%s", k_path_basename(first));
        else snprintf(label, sizeof label, "%d items", n);
        unsigned mods = WIN_MOUSE_MODS(ev->mods);
        if (ev->type == WIN_EV_DRAG_OVER) {
            uui_router_extern_over(&a->router, ev->a, ev->b, mods, dir, n, label);
        } else {
            int id = uui_router_extern_drop(&a->router, ev->a, ev->b, mods);
            if (id && d->on_widget) d->on_widget(a, id, UUI_REASON_DROP);
        }
        a->dirty = 1;
        break;
    }
    case WIN_EV_DRAG_LEAVE:
        if (a->router.count) { uui_router_extern_leave(&a->router); a->dirty = 1; }
        break;

    case WIN_EV_USER:
        // Posted by this program itself, almost always from a worker
        // thread -- see uapp_post(). Nothing in the toolkit interprets
        // the payload; waking the loop IS the message.
        if (d->on_user && d->on_user(a, (int)ev->a, (int)ev->b)) a->dirty = 1;
        break;

    case WIN_EV_TIMER:
        // The app asked to be woken on a schedule (desc.tick_ms), so
        // this is its on_tick -- reached from the BLOCKING loop, which
        // is the entire point: the same callback, without the process
        // being runnable the whole time in between.
        if (d->on_tick && d->on_tick(a)) a->dirty = 1;
        // THE SESSION FONT MAY HAVE BEEN REPUBLISHED. Asked here rather
        // than only on WIN_EV_FONT because that event fires when the
        // SETTING changes and fontd rebuilds a moment later -- so the
        // event alone re-maps the atlas that is about to be replaced.
        if (ugfx_font_recheck()) {
            if (d->layout) uui_layout_run(d->layout, 0, 0, a->w, a->h);
            if (d->on_font) d->on_font(a);
            a->dirty = 1;
        }
        break;

    case WIN_EV_FONT:
        // The desktop's font changed. Re-map it (the mapping is at a
        // fixed address, so this writes over itself and needs no unmap),
        // re-run the layout because every widget's natural size is
        // measured from the cell, and repaint. An app gets all of this
        // without knowing fonts exist -- the same deal WIN_EV_RESIZE
        // gives it, and for the same reason: an app that has never heard
        // of the thing should still behave correctly when it happens.
        ugfx_font_init();
        if (d->layout) uui_layout_run(d->layout, 0, 0, a->w, a->h);
        // AFTER the re-map and the re-layout, never before: an app's
        // on_font almost always re-measures something, and measuring
        // against the font that just went away is the one mistake this
        // callback exists to prevent.
        if (d->on_font) d->on_font(a);
        a->dirty = 1;
        // AND EVERY DIALOG WINDOW. The event names the toplevel's slot,
        // so marking only `a` would leave a chooser laid out for the
        // font that just went away until something else touched it.
        for (int i = 1; i < WIN_CLIENT_MAX; i++)
            if (g_dlg[i].slot) g_dlg[i].dirty = 1;
        break;

    case WIN_EV_SETTING:
        // A setting changed somewhere (the compositor forwards the
        // kernel's notice). Re-reading the zone and the formats is a few
        // small reads, so it is done for ANY setting rather than asking
        // which -- Windows' WM_SETTINGCHANGE carries no more than this.
        tzset();
        setlocale(LC_ALL, "");
        a->dirty = 1;
        for (int i = 1; i < WIN_CLIENT_MAX; i++)
            if (g_dlg[i].slot) g_dlg[i].dirty = 1;
        break;

    case WIN_EV_CLOSE:
        // The default ACCEPTS. An app that wants to refuse says so;
        // an app that has never heard of closing still closes.
        if (!d->on_close || d->on_close(a)) uapp_quit(a, 0);
        break;

    case WIN_EV_RESIZE:
        // A PROPOSAL from TWS, answered here. Everything an app would
        // otherwise have to remember -- ask the server, rebuild the
        // surface, re-lay-out, repaint -- happens for it. An app that
        // has never heard of resizing gets all of it; one that cares
        // adds on_resize.
        //
        // A refusal is normal (out of memory, over WIN_CLIENT_MAX_*),
        // and the answer is to keep the size we had and repaint
        // nothing: the window is unchanged, so there is nothing to
        // show.
        //
        // **A FIXED-SIZE APP DECLINES**, and the proposal is the WM's to
        // make either way -- a client commits its own size, which is
        // Wayland's rule for a configure. Without this, any single wrong
        // size becomes permanent: geometry is remembered per app and
        // restored as a proposal, so one bad value (a `gui resize`, a
        // font change moving the natural size) reopens the app that way
        // for ever, with the layout adrift in the extra room.
        if (!(d->flags & WIN_HINT_RESIZABLE)) {
            // **REFUSING SILENTLY IS NOT ENOUGH.** The WM has already
            // moved its frame to the size it proposed, so a client that
            // just ignores the event leaves a window drawn at its real
            // size inside a bigger frame -- the whitespace this whole
            // thing is about. Presenting is the answer the protocol
            // already has: the frame carries the front buffer's own
            // dimensions and the compositor adopts THOSE.
            a->dirty = 1;
            break;
        }
        {
            // NOT BELOW THE APP'S OWN MINIMUM: TWS clamps a proposal to
            // the declared minimum, but a remembered geometry is proposed
            // at create, BEFORE the hints that declare it arrive. The
            // client commits its size, so it holds the line itself.
            int rw = ev->a, rh = ev->b;
            if (d->min_w > 0 && rw < d->min_w) rw = d->min_w;
            if (d->min_h > 0 && rh < d->min_h) rh = d->min_h;
            if (uapp_resize(a, rw, rh)) {
                if (d->on_resize) d->on_resize(a, a->w, a->h);
                a->dirty = 1;
            }
        }
        break;

    case WIN_EV_WHEEL: {
        // To the widget UNDER THE CURSOR, not down a fixed chain -- see
        // uui_route.h. The app's own on_wheel still fires for anything
        // the widgets did not take.
        int changed = 0;
        int id = a->router.count
                     ? uui_router_wheel(&a->router, a->mouse_x, a->mouse_y,
                                        ev->a, &changed)
                     : 0;
        if (changed) a->dirty = 1;
        tell_app(a, id, UUI_REASON_WHEEL, 0);
        if (!id && d->on_wheel) { d->on_wheel(a, ev->a); a->dirty = 1; }
        break;
    }

    case WIN_EV_FOCUS:
        // Recorded and repainted for the app, so the common case --
        // "stop drawing my caret when I am not focused" -- needs no
        // callback, just uapp_focused() in on_draw.
        // Logged on a CHANGE only: what the client was told is the oracle
        // for the compositor's focus events (tools/focus_state_test.py).
        if (a->focused != (ev->a ? 1 : 0))
            ulogf("uapp: pid %d focus %d\n", (int)sys_getpid(), ev->a ? 1 : 0);
        a->focused = ev->a ? 1 : 0;
        if (d->on_focus) d->on_focus(a, a->focused);
        a->dirty = 1;
        break;

    case WIN_EV_KEY:
        // The focus ring gets the key first: Tab/Shift-Tab move focus,
        // everything else goes to the focused widget. on_key still fires
        // afterwards -- for a key the ring did not take, and so an app
        // can re-read a focused widget whose value the ring just changed.
        // AN OPEN POPUP OUTRANKS BOTH. It is drawn over everything and
        // is what the user is looking at, so it takes the key before
        // the focus ring or the app sees it -- the keyboard's half of
        // the overlay rule the pointer already follows, and what makes
        // typing in a dropdown work in an app with no focus ring at
        // all. See ui/uui_route.h.
        // ESC CANCELS A DRAG before anything else sees the key: the
        // pointer is still held, so nothing else can be meant by it.
        if (a->router.count && ev->a == 0x1B && uui_router_drag_active(&a->router)) {
            uui_router_drag_cancel(&a->router);
            wmchan_send(WIN_REQ_DRAG_END, 0, 0, 0, 0, 0);
            a->dirty = 1;
            break;
        }
        // A SHORTCUT SKIPS THE WIDGETS (uui_widget.h): Ctrl+1 goes to the
        // app, and a focused field does not type a 1.
        if (uui_key_is_shortcut(ev->a, ev->mods)) {
            if (d->on_key) d->on_key(a, ev->a, ev->mods);
            break;
        }
        if (a->router.count) {
            int changed = 0;
            int id = uui_router_overlay_key(&a->router, ev->a, ev->mods, &changed);
            if (changed) a->dirty = 1;
            if (id) {
                tell_app(a, id, UUI_REASON_KEY, 0);
                break;
            }
        }
        if (d->focus && uui_focus_key(d->focus, ev->a, ev->mods)) {
            a->dirty = 1;
            // AND THE APP IS TOLD, by the same id a click on that widget
            // reports. A focused control whose value the keyboard just
            // changed is exactly as much a change as a clicked one, and
            // an app hearing only about clicks silently drops it. Tab
            // is excluded: it moved focus and changed no value.
            if (ev->a != '\t' && d->on_widget && d->focus->current >= 0) {
                int id = uui_router_id_of(&a->router,
                                          d->focus->items[d->focus->current].widget);
                tell_app(a, id, UUI_REASON_KEY, 1);
            }
        }
        if (d->on_key) d->on_key(a, ev->a, ev->mods);
        break;

    case WIN_EV_KEY_PHYS:
        if (d->on_phys_key) d->on_phys_key(a, ev->a, ev->b, ev->mods);
        break;

    case WIN_EV_KEY_UP:
        // NOT offered to the focus ring. Tab moves focus on the way DOWN
        // and doing it again on the way up would move it twice per press;
        // no widget here acts on a key release at all. An app that wants
        // releases is doing something the widgets are not -- tracking a
        // held key -- so this goes straight to it.
        if (d->on_key_up) d->on_key_up(a, ev->a, ev->mods);
        break;

    case WIN_EV_MOUSE_DOWN: {
        a->mouse_x = ev->a;
        a->mouse_y = ev->b;
        // ONLY THE PRIMARY BUTTON ACTIVATES A WIDGET. `mods` carries the
        // button bits (abi/win_proto.h), and TWS delivers a secondary
        // click inside a client's content area like any other press --
        // so without this test a right-click would arm buttons, move the
        // focus and commit menu items, which no toolkit does. Qt and GTK
        // both hand every button to the app and act on button 1 alone.
        //
        // on_press still fires for EVERY button, because the app is the
        // only thing that can know what a secondary click means to it
        // (Minesweeper flags a cell; Calculator ignores it).
        // The event's `mods` is TWO fields (WIN_MOUSE_MODS_SHIFT): the
        // buttons now down, and the keyboard modifiers held.
        unsigned btns = WIN_MOUSE_BUTTONS(ev->mods);
        unsigned kmods = WIN_MOUSE_MODS(ev->mods);
        int primary = (btns & 0x1) != 0;
        if (primary) {
            // Routed FIRST, so a widget that wants this press gets it and
            // takes the pointer grab. The app's on_press still runs: an app
            // may want a press the widgets ignored (a canvas, a text area),
            // or may want to log one they took.
            int changed = 0;
            int id = a->router.count
                         ? uui_router_press(&a->router, ev->a, ev->b, kmods,
                                            &changed) : 0;
            if (changed) a->dirty = 1;
            tell_app(a, id, UUI_REASON_PRESS, 0);
            // Keyboard focus follows the click, after the widgets have had
            // the press (a widget takes the pointer grab; this only moves
            // which one keys go to). See uui_focus_click(). NOT for a
            // press on a popup: it is its owner's, and whatever lies
            // under it in the window took nothing -- moving focus there
            // closed a dropdown under the press choosing its row.
            if (d->focus && !g_press_slot && uui_focus_click(d->focus, ev->a, ev->b)) a->dirty = 1;
            if (d->buttons && uui_button_group_press(d->buttons, ev->a, ev->b)) a->dirty = 1;
        }
        if (d->on_press) d->on_press(a, ev->a, ev->b, ev->mods);
        break;
    }

    case WIN_EV_MOUSE_MOVE:
        // With a button held this re-hit-tests the press, so dragging
        // off a control un-presses it; with none held it is hover
        // tracking. Both report "did anything change", so a cursor
        // crossing the window only repaints when it crosses a boundary.
        // This is the arm that was copied verbatim into three apps.
        a->mouse_x = ev->a;
        a->mouse_y = ev->b;
        // Masked to the PRIMARY button for the same reason as the press
        // above: a drag with only the secondary button held is not a
        // drag as far as a widget is concerned, it is hover with
        // something else held down.
        unsigned held = ev->mods & 0x1;
        if (a->router.count) {
            int changed = 0;
            // The GRAB lives here: while a button is held this goes to
            // whoever took the press, wherever the cursor now is, which
            // is what makes a drag work with no app state at all.
            int was_dragging = uui_router_drag_active(&a->router);
            int id = uui_router_motion(&a->router, ev->a, ev->b, held,
                                       WIN_MOUSE_MODS(ev->mods), &changed);
            if (changed) a->dirty = 1;
            // A HOVER IS NOT AN EVENT FOR THE APP unless it asked
            // (motion_wanted()). The widget has already redrawn its hover;
            // an app that read every call as "act" opened Notepad per row
            // crossed (Crash Reports).
            if (motion_wanted(&a->router, id, held)) tell_app(a, id, UUI_REASON_MOTION, 0);
            // A drag just began: tell the compositor, so it can offer it
            // to whatever the pointer leaves this window for. The source
            // widget has filled the drag slot (lib/uclip.h) in drag_start.
            if (!was_dragging && uui_router_drag_active(&a->router)) {
                const struct uui_drag *dg = uui_router_drag(&a->router);
                wmchan_send(WIN_REQ_DRAG_START, 0, dg ? dg->count : 1, 0, 0, 0);
            }
        }
        if (d->buttons) {
            int changed = held ? uui_button_group_press(d->buttons, ev->a, ev->b)
                                : uui_button_group_hover(d->buttons, ev->a, ev->b);
            if (changed) a->dirty = 1;
        }
        // The widget tree answers first; on_motion below overrides, and
        // only the final answer is sent (motion_cursor).
        a->motion_cursor = a->cursor;
        if (a->router.count) {
            uapp_set_cursor(a, uui_router_cursor(&a->router, ev->a, ev->b));
        }
        if (d->on_motion) d->on_motion(a, ev->a, ev->b, ev->mods);
        {
            int want = a->motion_cursor;
            a->motion_cursor = -1;
            uapp_set_cursor(a, want);
        }
        break;

    case WIN_EV_MOUSE_UP:
        a->mouse_x = ev->a;
        a->mouse_y = ev->b;
        if (a->router.count) {
            int changed = 0;
            int was_dragging = uui_router_drag_active(&a->router);
            int id = uui_router_release(&a->router, ev->a, ev->b, &changed);
            if (changed) a->dirty = 1;
            tell_app(a, id, UUI_REASON_RELEASE, changed);
            // The slot stays: a target in another window reads it AFTER
            // this release reaches the compositor (wm_dnd.c says why).
            if (was_dragging) wmchan_send(WIN_REQ_DRAG_END, 0, 0, 0, 0, 0);
            // A release that DROPPED names the target instead. The
            // payload is still readable through uapp_drag() here and
            // nowhere later -- the router has already let go of it.
            int drop = uui_router_take_drop(&a->router);
            if (drop && d->on_widget) d->on_widget(a, drop, UUI_REASON_DROP);
        }
        if (d->buttons) {
            // The commit point. A press dragged off its button was
            // already cleared by the moves above, so this returns -1
            // and correctly does nothing.
            int code = uui_button_group_release(d->buttons);
            a->dirty = 1;
            if (code >= 0 && d->on_action) d->on_action(a, code);
        }
        if (d->on_release) d->on_release(a, ev->a, ev->b, ev->mods);
        break;

    default:
        // An event type this build does not know about. Ignoring it is
        // the whole point: TWS can start sending something new without
        // every app being edited.
        break;
    }
}

// --- lifecycle --------------------------------------------------------


// For UAPP_SINGLE_INSTANCE: is a copy of this app already on screen? If
// so the compositor raises its window and this returns 1, meaning "you
// are the second copy, go away quietly".
//
// Asked BEFORE the window is created, and before the font is mapped,
// so the redundant copy costs one round trip and never appears -- a
// window that flashes up and vanishes is worse than no single-instance
// support at all.
static int activate_existing(const struct uapp_desc *desc) {
    if (!(desc->flags & UAPP_SINGLE_INSTANCE)) return 0;
    // **ASKED OF THE COMPOSITOR, AND THE ONLY CHANNEL MESSAGE THAT
    // WAITS.** It carries no name: the compositor asks the kernel what
    // program the ASKING pid is (QUERY_PROCPATH) and compares that
    // against what it recorded per window, so the answer cannot depend
    // on a string an app declares about itself.
    //
    // NO CHANNEL, NO TWIN -- and a timeout says the same. The wrong
    // direction here is a false "yes", which makes a single-instance
    // app exit without ever drawing; a false "no" opens a second
    // window, which the user can see and close.
    return wmchan_call(WIN_REQ_ACTIVATE, 0, 0, 0, 0, 0) == 1;
}

static int uapp_open(struct uapp **out, const struct uapp_desc *desc) {
    struct uapp *a = &g_app;
    // EVERY GUI APP SPEAKS THE SYSTEM'S REGION, and follows a change to
    // it (WIN_EV_SETTING below) -- what the person chose in System
    // Settings is not something an app should have to opt into.
    setlocale(LC_ALL, "");
    a->desc = desc;
    a->dirty = 1;
    a->running = 1;
    // A window is frontmost the moment it is created, so TWS sends no
    // event to say so -- see wm_client.c's on_window_created().
    a->focused = 1;
    a->status = 0;
    a->motion_cursor = -1;   // not in a motion; 0 is WIN_CURSOR_DEFAULT

    // The font FIRST: on_size derives the window size from the metrics,
    // and WIN_REQ_FONT needs a registered server rather than a window,
    // so the order is legal. (Calculator worked this out the hard way --
    // see its comment on why it cannot create-then-resize.)
    if (!ugfx_font_init()) return 0;

    // Precedence: an explicit on_size wins, then the layout's natural
    // size, then a fixed w/h. The layout case is the one that makes
    // "the window is exactly big enough for its content" free rather
    // than a third copy of the same arithmetic in every app.
    a->w = desc->w;
    a->h = desc->h;
    if (desc->on_size)     desc->on_size(&a->w, &a->h);
    else if (desc->layout) uui_layout_natural_size(desc->layout, &a->w, &a->h);
    if (a->w <= 0 || a->h <= 0) return 0;
    clamp_to_screen(&a->w, &a->h);

    // **NO COMPOSITOR CHANNEL, NO WINDOW.** The pixels are this
    // process's own memory and the compositor gets at them only because
    // this grants it -- so without the beacon that names it, a window
    // would open, draw, present and never appear. Refusing here is the
    // loud version of that, and it is what stage 6 does anyway: no
    // compositor, no window.
    if (comp_pid() <= 0) return 0;

    // THE CLIENT ALLOCATES, AND PROPOSES ITS OWN SLOT -- which is what
    // lets the buffers be NAMED, granted and mapped before anyone has
    // answered. One toplevel per process, so the proposal is always 0;
    // its popups take the slots above (popup_open()).
    if (!bufs_create(TOPLEVEL, a->w, a->h)) return 0;

    // The x/y an app used to ask for are GONE from this message: the
    // compositor has always placed windows itself (a cascade), and a
    // field nobody reads is a field that eventually gets believed.
    int slot = wmchan_call(WIN_REQ_CREATE, 0, a->w, a->h, desc->app_id, -1);
    if (slot < 0) { bufs_release(TOPLEVEL); return 0; }
    a->window = (uint32_t)slot;
    uui_popup_set_provider(&g_popup_ops, a);

    if (desc->title) uapp_set_title(a, desc->title);

    // Declare behaviour. Sent unconditionally, including when the app
    // asked for nothing: "fixed size, no minimum" is a statement, and
    // leaving TWS to assume it would be the inference this protocol
    // deliberately avoids.
    wmchan_send(WIN_REQ_HINTS, a->window,
                (int)(desc->flags | (desc->on_phys_key ? WIN_HINT_PHYS_KEYS : 0)),
                desc->min_w, desc->min_h, 0);

    // Arm the repeating timer, if the app asked for one. Only useful
    // alongside an on_tick, which is the only thing it drives -- arming
    // it without one would wake the process to do nothing, which is a
    // slower version of the problem it exists to solve.
    if (desc->tick_ms && desc->on_tick) {
        // A refusal is survivable and deliberately not fatal: uapp_run()
        // checks the same condition and falls back to polling, so an
        // older server that has never heard of this simply gets the old
        // behaviour instead of an app that never ticks.
        a->timer_armed = wmchan_send(WIN_REQ_TIMER, a->window,
                                     (int)desc->tick_ms, 0, 0, 0);
    }
    // THE BACK BUFFER, never buffer 0: the compositor shows that one
    // from the create on, so painting it reads as a window that opens
    // and draws nothing (surf_back() skips the busy front).
    if (surf_back(TOPLEVEL)) a->surface = TOPLEVEL->surface;

    // Now that the content size is settled, place everything in it.
    // Re-run rather than trusting the natural-size pass: the window may
    // have been created at a different size than was asked for.
    if (desc->layout) uui_layout_run(desc->layout, 0, 0, a->w, a->h);
    uui_router_init(&a->router, desc->widgets, desc->widget_count);

    if (desc->on_open) desc->on_open(a);
    flush(a); // the first frame, from the dirty flag set above

    *out = a;
    return 1;
}

// How long one park lasts before the loop looks around. Not a cadence
// anything is delivered on -- an event wakes it at once -- but the
// interval at which a client with nothing to do notices the compositor
// has died (uchan_client_server_alive), which is what a WIN_EV_CLOSE
// from the kernel used to say.
#define UAPP_WAIT_MS 500

// The next event: a post from this process's own worker first, then the
// compositor's inbox. Returns 1 with `ev` filled, 0 when both are empty.
static int next_event(struct win_event *ev) {
    int a0, a1;
    if (post_take(&a0, &a1)) {
        memset(ev, 0, sizeof *ev);
        ev->type = WIN_EV_USER;
        ev->window = g_app.window;
        ev->a = a0;
        ev->b = a1;
        return 1;
    }
    return uchan_client_recv(&g_wmchan, ev, sizeof *ev);
}

static int uapp_pump(struct uapp *a, int block) {
    struct win_event ev;
    static uint32_t dropped_seen;
    static int asked_to_close;   // the compositor's death is asked about ONCE

    if (block) {
        while (!uchan_client_pending(&g_wmchan) && g_post_head == g_post_tail) {
            // ONE FRAME while something animates (ui/uui_anim.h), the
            // long park otherwise -- and the frame's wake is not "a whole
            // wait with nobody there", so the liveness check below stays
            // on the long one.
            int animating = uui_anim_pending();
            int wait = animating ? UUI_ANIM_FRAME_MS : UAPP_WAIT_MS;
            // A drawn caret's next flip (ui/uui_caret.h) shortens the
            // park like a frame does; it asks for nothing once solid.
            int caret = uui_caret_wait_ms();
            if (caret == 0) {
                // Which surface drew the caret is not recorded, so all
                // of them repaint -- two frames a second, for ten seconds.
                a->dirty = 1;
                for (int i = 0; i < WIN_CLIENT_MAX; i++)
                    if (g_dlg[i].slot) g_dlg[i].dirty = 1;
                break;
            }
            if (caret > 0 && caret < wait) wait = caret;
            uchan_client_wait(&g_wmchan, wait);
            if (uchan_client_pending(&g_wmchan) || g_post_head != g_post_tail) break;
            if (animating) break;
            if (wait == caret) continue;   // the flip, not a silent server
            // Nothing arrived in a whole wait: is anyone still there to
            // send? A dead compositor's beacon is unlinked with it, so
            // this is the close the desktop can no longer ask for.
            if (!asked_to_close && !uchan_client_server_alive(&g_wmchan)) {
                // ASKED, not destroyed -- the same courtesy the desktop
                // extends when it is alive. An app that refuses lingers
                // with no window, which is a leak and not a fault, and
                // is what it asked for; it is not asked again.
                asked_to_close = 1;
                ulog("uapp: the compositor is gone -- asked to close\n");
                memset(&ev, 0, sizeof ev);
                ev.type = WIN_EV_CLOSE;
                ev.window = a->window;
                dispatch(a, &ev);
                if (!a->running) return 0;
            }
        }
    }
    // Drain everything queued, blocking or not: one wake often carries
    // several events, and handling them together is what makes the
    // single coalesced present below correct rather than laggy.
    while (a->running && next_event(&ev)) {
        dispatch(a, &ev);
        owed_release(a);
    }

    // A widget asked for another frame from its last draw: paint every
    // surface, since nothing says which one it was on. Only while
    // something moves, so the over-paint is bounded by the motion.
    if (uui_anim_take()) {
        a->dirty = 1;
        for (int i = 0; i < WIN_CLIENT_MAX; i++)
            if (g_dlg[i].slot) g_dlg[i].dirty = 1;
    }

    // Input the inbox could not hold went missing, and that is worth
    // one line per rise: a client that sees this is not keeping up.
    uint32_t dropped = uchan_client_dropped(&g_wmchan);
    if (dropped != dropped_seen) {
        ulogf("uapp: %u event(s) dropped -- the loop is behind\n", (unsigned)(dropped - dropped_seen));
        dropped_seen = dropped;
    }

    flush(a);
    // AND THE DIALOG WINDOWS, in slot order. Separate from the popup
    // pass inside present(): a popup is drawn by the widget that owns it
    // during the toplevel's own frame, while a dialog has its own
    // content and paints itself.
    for (int i = 1; i < WIN_CLIENT_MAX; i++)
        if (g_dlg[i].slot) dlg_flush(&g_dlg[i]);
    return a->running;
}

static void uapp_close(struct uapp *a) {
    for (int i = 1; i < WIN_CLIENT_MAX; i++)
        if (g_surf[i].used) popup_close(0, i);
    wmchan_send(WIN_REQ_DESTROY, a->window, 0, 0, 0, 0);
}

int uapp_run(const struct uapp_desc *desc) {
    struct uapp *a;

    // Already running? Its window has been raised; this copy's whole
    // job is done. Exit 0 -- the user asked for the app and got it, so
    // the launch SUCCEEDED, and reporting a failure here would put an
    // error in the log for the case that works.
    if (activate_existing(desc)) return 0;

    // Before any window exists: a refusal leaves nothing on screen.
    if (desc->widgets && desc->widget_count > 0) {
        struct uui_router probe;
        uui_router_init(&probe, desc->widgets, desc->widget_count);
        if (!ids_unique(&probe, desc->title)) return 1;
        if (!buttons_heard(&probe, desc->on_action != 0, desc->title)) return 1;
    }

    if (!uapp_open(&a, desc)) return 1;

    if (desc->on_tick && !a->timer_armed) {
        // No timer -- either the app named no interval or the server
        // declined one. Poll rather than block, so the app still keeps
        // moving with no input. The yield is politeness, not a
        // workaround: a client doing real work every frame would
        // otherwise take its whole timeslice and make the desktop feel
        // sticky.
        //
        // This is the OLD path, kept for apps that have not named a
        // tick_ms. It costs a wake-up per tick whatever the app
        // actually needed, which is why anything with a real cadence
        // should set one -- see uapp.h's tick_ms.
        while (a->running) {
            if (a->poll_paused) {   // nothing to animate: wait like any app
                uapp_pump(a, 1);
                continue;
            }
            if (desc->on_tick(a)) a->dirty = 1;
            uapp_pump(a, 0);
            sys_yield();
        }
    } else {
        while (uapp_pump(a, 1)) { }
    }

    uapp_close(a);
    return a->status;
}
