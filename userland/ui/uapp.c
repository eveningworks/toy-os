// See ui/uapp.h for what this is and why.
#include "ui/uapp_internal.h"

// One process, one TOPLEVEL -- plus its popups, which are surfaces of
// the same app (g_surf[] above) rather than apps of their own. A single
// static instance rather than a heap allocation: there is no allocator
// in libsys, and one toplevel per process makes the multi-instance
// machinery the kernel-space apps need unnecessary here. The handle is
// opaque precisely so a future uapp_window_create() can appear without
// this API changing shape.
struct uapp g_app;

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

int wmchan_send(uint32_t type, uint32_t window,
                       int aa, int bb, int cc, const char *text);
int wmchan_call(uint32_t type, uint32_t window, int aa, int bb, int cc,
                       const char *text, int fail);
// `text` and `dmg` share the message's one payload field: pass AT MOST
// ONE. Both callers do; a string would win.
int wmchan_send_damage(uint32_t type, uint32_t window, int aa, int bb, int cc,
                              const char *text, const struct win_damage *dmg);

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
    a->shown = 1;
    present_popups(a->top.slot);
}

// Draw + present, but only if something actually asked. This is the
// coalescing uapp_redraw() promises: a burst of events costs one round
// trip, not one per event.
static void flush(struct uapp *a) {
    if (!a->top.dirty) return;
    if (a->hold_until_ms) {
        if (sys_monotonic_ns() / 1000000ULL < a->hold_until_ms) return;
        a->hold_until_ms = 0;
    }
    // NO FREE BUFFER, NO FRAME: it stays dirty and is drawn when the
    // compositor hands one back (WIN_EV_BUF_RELEASE wakes the loop).
    if (!a->lease_on) {
        struct ugfx_surface *sf = surf_back(TOPLEVEL);
        if (!sf) return;
        a->surface = *sf;
    }
    a->top.dirty = 0;

    top_paint(&a->top, &a->surface, a->desc->layout, a);
    // The layout block this frame produced, emitted only if it differs
    // from the last -- see uapp_log_layout(). Here rather than in the
    // apps because only the toolkit knows when a frame has ended.
    layout_log_flush(0);
    wmap_sync(a);
    present(a);
}

// --- public: state ----------------------------------------------------

void uapp_redraw(struct uapp *a) { a->top.dirty = 1; }

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
void clamp_to_screen(int *w, int *h) {
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
struct uchan_client g_wmchan;
static int g_wmchan_state;   // 0 untried, 1 open, -1 no compositor channel

static void loop_kick(void) { uchan_client_kick(&g_wmchan); }

// How long a round trip waits. Generous because the cost of being wrong
// is one-sided in both places: giving up early opens a duplicate window
// or an app with none, where waiting only delays a startup. Nothing but
// a wedged compositor spends it.

int wmchan(void) {
    if (g_wmchan_state) return g_wmchan_state > 0;
    g_wmchan_state = uchan_client_open(&g_wmchan, WMCHAN_SERVICE) == 0 ? 1 : -1;
    return g_wmchan_state > 0;
}

// WHO TO GRANT A WINDOW BUFFER TO. The beacon this channel is already
// opened through carries the server's pid, so a client needs no new way
// to learn it -- and a compositor with no beacon is one that could not
// be sent a title either.
int comp_pid(void) {
    return wmchan() ? g_wmchan.beacon->server_pid : 0;
}

// Sends one request over the channel. Returns 1 if it went, 0 if there
// is no compositor channel or its ring is full.
// The payload rides the text union: a string, or a present's damage.
#define WMCHAN_SEND_WAIT_MS 500

int wmchan_send_damage(uint32_t type, uint32_t window, int aa, int bb, int cc,
                              const char *text, const struct win_damage *dmg) {
    if (!wmchan()) return 0;
    struct wmchan_msg m;
    memset(&m, 0, sizeof m);
    m.type = type;
    m.window = window;
    m.a = aa; m.b = bb; m.c = cc;
    if (text) snprintf(m.text, sizeof m.text, "%s", text);
    else if (dmg) m.damage = *dmg;
    // A FULL RING WAITS rather than drops: a window's widget map is a
    // message per named widget, and a page with more than the ring holds
    // lost the PRESENT queued behind them -- a window that opened and
    // never drew (System Settings' Remote Desktop page found it).
    return uchan_send_wait(&g_wmchan, &m, sizeof m, WMCHAN_SEND_WAIT_MS) == 0;
}

int wmchan_send(uint32_t type, uint32_t window,
                       int aa, int bb, int cc, const char *text) {
    return wmchan_send_damage(type, window, aa, bb, cc, text, 0);
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
        // THE LAST PIECE WAITS FOR ITS ANSWER: the client's ring dies
        // with it (wm_client_chan_pump()), so a tool that exits straight
        // after -- every screenshot does -- lost a notice still queued.
        if (i == pieces - 1)
            return wmchan_call(WIN_REQ_NOTICE, 0, i | (pieces << 8), kind, (int)flags, part, -1) == 0;
        if (!wmchan_send(WIN_REQ_NOTICE, 0, i | (pieces << 8), kind, (int)flags, part)) return 0;
    }
    return 1;
}

// The requests that need an answer (lib/uwmchan.h). Returns the
// compositor's `a`, or `fail` when there is no channel or no reply --
// both of which the callers read as a refusal.
int wmchan_call(uint32_t type, uint32_t window, int aa, int bb, int cc,
                       const char *text, int fail) {
    if (!wmchan()) return fail;
    struct wmchan_msg m, r;
    memset(&m, 0, sizeof m);
    m.type = type;
    m.window = window;
    m.a = aa;
    m.b = bb;
    m.c = cc;
    if (text) snprintf(m.text, sizeof m.text, "%s", text);
    if (uchan_call(&g_wmchan, &m, sizeof m, &r, sizeof r,
                   UAPP_CALL_TIMEOUT_MS) < 0) return fail;
    return r.a;
}

// Set while uapp dispatches the close IT made up because the compositor
// is gone: nothing may be asked of a compositor that is not there.
static int g_close_synth;

int uapp_press_on_popup(struct uapp *a) {
    (void)a;
    return g_press_slot != 0;
}

int uapp_drag_active(struct uapp *a) {
    return uui_router_drag_active(&a->top.router);
}

const struct uui_drag *uapp_drag(struct uapp *a) {
    if (uui_router_drag_active(&a->top.router)) return uui_router_drag(&a->top.router);
    return uui_router_dropped(&a->top.router);
}

void uapp_set_cursor(struct uapp *a, int cursor) {
    if (cursor < 0 || cursor >= WIN_CURSOR_COUNT) return;
    if (a->motion_cursor >= 0) { a->motion_cursor = cursor; return; }
    if (a->top.cursor == cursor) return;
    // Either way: a server that refuses this (one built before the
    // request existed) must not be asked again on every motion.
    a->top.cursor = cursor;
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

int uapp_set_tick(struct uapp *a, unsigned ms) {
    if (!a->timer_armed || !ms) return 0;
    return wmchan_send(WIN_REQ_TIMER, a->window, (int)ms, 0, 0, 0) ? 1 : 0;
}

void uapp_busy_begin(struct uapp *a) {
    a->cursor_before_busy = a->top.cursor;
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
    if (on && !a->shown) a->hold_until_ms = sys_monotonic_ns() / 1000000ULL + 300;
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
    uui_router_init(&a->top.router, items, count > 0 ? count : 0);
    const char *who = a->desc ? a->desc->title : 0;
    if (!ids_unique(&a->top.router, who) ||
        !buttons_heard(&a->top.router, a->desc && a->desc->on_action, who))
        a->top.router.count = 0;
    a->top.dirty = 1;
}

int uapp_fullscreen(const struct uapp *a) { return a ? a->fullscreen : 0; }
int uapp_scanout(const struct uapp *a)    { return a ? a->lease_on : 0; }

void uapp_display_name(const struct uapp *a, char *out, int cap) {
    struct uappentry e;
    char path[96];
    const char *id = a ? a->desc->app_id : 0;
    if (id && snprintf(path, sizeof path, "/usr/wm/applications/%s.desktop", id) < (int)sizeof path &&
        uappentry_read(path, &e) && e.name[0])
        strlcpy(out, e.name, (size_t)cap);
    else
        strlcpy(out, a && a->desc->title ? a->desc->title : "", (size_t)cap);
}

int uapp_set_tabs(struct uapp *a, const char *const *labels, int n, int active) {
    if (n < 0) n = 0;
    if (n > WIN_TABS_MAX) n = WIN_TABS_MAX;
    int ok = wmchan_send(WIN_REQ_TABS, a->window, n, active, 0, 0);
    for (int i = 0; i < n && ok; i++)
        ok = wmchan_send(WIN_REQ_TAB, a->window, i, 0, 0, labels[i] ? labels[i] : "");
    return ok;
}

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
        // A DIALOG IS A TOPLEVEL, NOT A POPUP, though it holds a g_surf
        // slot too: its press owes nothing, and recording it made
        // dlg_dispatch() skip the click's focus move -- a dialog's text
        // field could not be clicked into.
        if (in->window != a->window && in->window < WIN_CLIENT_MAX && g_surf[in->window].used &&
            !dlg_for(in->window)) {
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
            g_popup_parent = dw->top.slot;
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
                g_popup_parent = ow->top.slot;
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
        a->top.dirty = 1;
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
        if (!a->top.router.count) break;
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
            uui_router_extern_over(&a->top.router, ev->a, ev->b, mods, dir, n, label);
        } else {
            int id = uui_router_extern_drop(&a->top.router, ev->a, ev->b, mods);
            if (id && d->on_widget) d->on_widget(a, id, UUI_REASON_DROP);
        }
        a->top.dirty = 1;
        break;
    }
    case WIN_EV_DRAG_LEAVE:
        if (a->top.router.count) { uui_router_extern_leave(&a->top.router); a->top.dirty = 1; }
        break;

    case WIN_EV_TAB:
        if (d->on_tab) { d->on_tab(a, (int)ev->a, (int)ev->b); a->top.dirty = 1; }
        break;

    case WIN_EV_USER:
        // Posted by this program itself, almost always from a worker
        // thread -- see uapp_post(). Nothing in the toolkit interprets
        // the payload; waking the loop IS the message.
        if (d->on_user && d->on_user(a, (int)ev->a, (int)ev->b)) a->top.dirty = 1;
        break;

    case WIN_EV_TIMER:
        // The app asked to be woken on a schedule (desc.tick_ms), so
        // this is its on_tick -- reached from the BLOCKING loop, which
        // is the entire point: the same callback, without the process
        // being runnable the whole time in between.
        if (d->on_tick && d->on_tick(a)) a->top.dirty = 1;
        // THE SESSION FONT MAY HAVE BEEN REPUBLISHED. Asked here rather
        // than only on WIN_EV_FONT because that event fires when the
        // SETTING changes and fontd rebuilds a moment later -- so the
        // event alone re-maps the atlas that is about to be replaced.
        if (ugfx_font_recheck()) {
            if (d->layout) uui_layout_run(d->layout, 0, 0, a->w, a->h);
            if (d->on_font) d->on_font(a);
            a->top.dirty = 1;
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
        a->top.dirty = 1;
        // AND EVERY DIALOG WINDOW. The event names the toplevel's slot,
        // so marking only `a` would leave a chooser laid out for the
        // font that just went away until something else touched it.
        for (int i = 1; i < WIN_CLIENT_MAX; i++)
            if (g_dlg[i].top.slot) g_dlg[i].top.dirty = 1;
        break;

    case WIN_EV_SETTING:
        // A setting changed somewhere (the compositor forwards the
        // kernel's notice). Re-reading the zone and the formats is a few
        // small reads, so it is done for ANY setting rather than asking
        // which -- Windows' WM_SETTINGCHANGE carries no more than this.
        tzset();
        setlocale(LC_ALL, "");
        a->top.dirty = 1;
        for (int i = 1; i < WIN_CLIENT_MAX; i++)
            if (g_dlg[i].top.slot) g_dlg[i].top.dirty = 1;
        break;

    case WIN_EV_CLOSE:
        // The default ACCEPTS -- except while a question is open IN the
        // window ("File already exists", "Disable device"), whose answer
        // decides. A separate chooser window guards no data, so it does
        // not hold the close (uapp_desc.on_close).
    {
        // ONE walk for the question, taken where the answer is needed:
        // before the default decides, or after an on_close that refused
        // (it may just have OPENED its prompt, Notepad's "Save changes?").
        int asking;
        if (!d->on_close) {
            asking = uapp_inwindow_question_open(a);
            if (!asking) { uapp_quit(a, 0); break; }
            ulogf("uapp: close refused -- a question is open\n");
        } else {
            if (d->on_close(a)) { uapp_quit(a, 0); break; }
            asking = uapp_inwindow_question_open(a);
        }
        // A REFUSED close's question has to be SEEN, and the compositor
        // cannot see one drawn in here: ask for THIS window to come
        // forward. One-way; the compositor grants it only in answer to
        // a single close it just sent (win_proto.h). Never for the close
        // uapp makes up when the compositor is gone -- nobody to ask.
        if (asking && !g_close_synth) {
            wmchan_send(WIN_REQ_ACTIVATE_OWN, a->window, 0, 0, 0, 0);
            ulogf("uapp: asked to be brought forward for its question\n");
        }
        break;
    }

    case WIN_EV_RESIZE:
        a->hold_until_ms = 0;
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
            a->top.dirty = 1;
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
                a->top.dirty = 1;
            }
        }
        break;

    case WIN_EV_WHEEL:
        // The app's own on_wheel fires for anything the widgets did not take.
        if (!top_wheel(&a->top, ev->a) && d->on_wheel) { d->on_wheel(a, ev->a); a->top.dirty = 1; }
        break;

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
        a->top.dirty = 1;
        break;

    case WIN_EV_KEY:
        // ESC CANCELS A DRAG before anything else sees the key: the
        // pointer is still held, so nothing else can be meant by it.
        if (a->top.router.count && ev->a == 0x1B && uui_router_drag_active(&a->top.router)) {
            uui_router_drag_cancel(&a->top.router);
            wmchan_send(WIN_REQ_DRAG_END, 0, 0, 0, 0, 0);
            a->top.dirty = 1;
            break;
        }
        top_key(&a->top, ev);
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

    case WIN_EV_MOUSE_DOWN:
        a->top.mouse_x = ev->a;
        a->top.mouse_y = ev->b;
        // ONLY THE PRIMARY BUTTON ACTIVATES A WIDGET. TWS delivers a
        // secondary click inside a client's content like any other press,
        // and no toolkit lets a right-click arm buttons, move the focus or
        // commit menu items -- Qt and GTK both hand every button to the
        // app and act on button 1 alone. on_press still fires for EVERY
        // button: only the app knows what a secondary click means to it
        // (Minesweeper flags a cell). `mods` is TWO fields
        // (WIN_MOUSE_MODS_SHIFT): the buttons now down, and the keys held.
        if (WIN_MOUSE_BUTTONS(ev->mods) & 0x1) {
            top_press(&a->top, ev->a, ev->b, WIN_MOUSE_MODS(ev->mods));
            if (d->buttons && uui_button_group_press(d->buttons, ev->a, ev->b)) a->top.dirty = 1;
        }
        if (d->on_press) d->on_press(a, ev->a, ev->b, ev->mods);
        break;

    case WIN_EV_MOUSE_MOVE: {
        a->top.mouse_x = ev->a;
        a->top.mouse_y = ev->b;
        // Masked to the PRIMARY button, as the press is: a drag with only
        // the secondary held is hover with something else held down.
        unsigned held = ev->mods & 0x1;
        int was_dragging = a->top.router.count && uui_router_drag_active(&a->top.router);
        top_motion(&a->top, ev->a, ev->b, held, WIN_MOUSE_MODS(ev->mods));
        // A drag just began: tell the compositor, so it can offer it to
        // whatever the pointer leaves this window for. The source widget
        // has filled the drag slot (lib/uclip.h) in drag_start.
        if (a->top.router.count && !was_dragging && uui_router_drag_active(&a->top.router)) {
            const struct uui_drag *dg = uui_router_drag(&a->top.router);
            wmchan_send(WIN_REQ_DRAG_START, 0, dg ? dg->count : 1, 0, 0, 0);
        }
        if (d->buttons) {
            int changed = held ? uui_button_group_press(d->buttons, ev->a, ev->b)
                                : uui_button_group_hover(d->buttons, ev->a, ev->b);
            if (changed) a->top.dirty = 1;
        }
        // The widget tree answers first; on_motion below overrides, and
        // only the final answer is sent (motion_cursor).
        a->motion_cursor = a->top.cursor;
        if (a->top.router.count) uapp_set_cursor(a, uui_router_cursor(&a->top.router, ev->a, ev->b));
        if (d->on_motion) d->on_motion(a, ev->a, ev->b, ev->mods);
        int want = a->motion_cursor;
        a->motion_cursor = -1;
        uapp_set_cursor(a, want);
        break;
    }

    case WIN_EV_MOUSE_UP:
        a->top.mouse_x = ev->a;
        a->top.mouse_y = ev->b;
        if (a->top.router.count) {
            int was_dragging = uui_router_drag_active(&a->top.router);
            top_release(&a->top, ev->a, ev->b);
            // The slot stays: a target in another window reads it AFTER
            // this release reaches the compositor (wm_dnd.c says why).
            if (was_dragging) wmchan_send(WIN_REQ_DRAG_END, 0, 0, 0, 0, 0);
            // A release that DROPPED names the target instead. The
            // payload is still readable through uapp_drag() here and
            // nowhere later -- the router has already let go of it.
            int drop = uui_router_take_drop(&a->top.router);
            if (drop && d->on_widget) d->on_widget(a, drop, UUI_REASON_DROP);
        }
        if (d->buttons) {
            // The commit point. A press dragged off its button was
            // already cleared by the moves above, so this returns -1
            // and correctly does nothing.
            int code = uui_button_group_release(d->buttons);
            a->top.dirty = 1;
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
    return wmchan_call(WIN_REQ_ACTIVATE, 0, 0, 0, 0, 0, 0) == 1;
}

static int uapp_open(struct uapp **out, const struct uapp_desc *desc) {
    struct uapp *a = &g_app;
    // EVERY GUI APP SPEAKS THE SYSTEM'S REGION, and follows a change to
    // it (WIN_EV_SETTING below) -- what the person chose in System
    // Settings is not something an app should have to opt into.
    setlocale(LC_ALL, "");
    a->desc = desc;
    a->top.dirty = 1;
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
    int slot = wmchan_call(WIN_REQ_CREATE, 0, a->w, a->h, 0, desc->app_id, -1);
    if (slot < 0) { bufs_release(TOPLEVEL); return 0; }
    a->window = (uint32_t)slot;
    a->top.slot = a->window;
    a->top.ops = &APP_OPS;
    a->top.focus = desc->focus;
    a->top.hears = desc->on_widget != 0;
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
    uui_router_init(&a->top.router, desc->widgets, desc->widget_count);

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
                a->top.dirty = 1;
                for (int i = 0; i < WIN_CLIENT_MAX; i++)
                    if (g_dlg[i].top.slot) g_dlg[i].top.dirty = 1;
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
                g_close_synth = 1;
                dispatch(a, &ev);
                g_close_synth = 0;
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
        a->top.dirty = 1;
        for (int i = 0; i < WIN_CLIENT_MAX; i++)
            if (g_dlg[i].top.slot) g_dlg[i].top.dirty = 1;
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
        if (g_dlg[i].top.slot) dlg_flush(&g_dlg[i]);
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
            if (desc->on_tick(a)) a->top.dirty = 1;
            uapp_pump(a, 0);
            sys_yield();
        }
    } else {
        while (uapp_pump(a, 1)) { }
    }

    uapp_close(a);
    return a->status;
}
