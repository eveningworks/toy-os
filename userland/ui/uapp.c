// See ui/uapp.h for what this is and why.
#include "rt/sys.h"   // TWP messages, sys_win_request(), sys_wait_event()
#include "ui/uapp.h"
#include <string.h>
#include <stdio.h>
#include "lib/uchan.h"
#include "lib/uwmchan.h"
#include "ui/uui_route.h"
#include "ui/uui_focus.h"   // desc.focus -- keyboard focus ring
#include "ui/ulog.h"        // uapp_log_layout()
#include "setting_abi.h" // desktop.layout_log -- the gate below
#include <stdio.h>      // snprintf/vsnprintf, one layout line at a time
#include <stdarg.h>
#include "ui/utheme.h"
#include "lib/uclip.h"   // clip_poll() -- the clipboard is shared memory now

// THE CLIENT'S OWN WINDOW MEMORY. A buffer is a named shm object this
// process creates and GRANTS to the compositor, which opens the name
// itself -- the kernel neither allocates it nor maps it. The name
// identifies the slot; the object in it is replaced on a resize, and
// the old one stays alive under whoever still maps it until they let go
// (docs/winserver-ring3-design.md, stage 5).
#define UAPP_BUFS 2

static void *g_px[UAPP_BUFS];
static uint64_t g_px_bytes[UAPP_BUFS];
static int g_slot;

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
static int buf_make(int slot, int buf, uint64_t bytes) {
    // **PAGE-ALIGNED, because SYS_MUNMAP requires it** (abi/syscall_abi.h)
    // and refuses anything else. A window's pixels are w*h*4, which is
    // almost never a whole number of pages -- so the unrounded length
    // made every unmap fail silently, the old mapping outlive its
    // replacement, and each resize leak a buffer's worth of frames.
    // Twelve objects after three drags, and then the biggest allocation
    // in the run is the one that fails.
    bytes = (bytes + 4095) & ~4095ULL;
    char nm[WIN_BUF_NAME_MAX];
    buf_name(nm, sizeof nm, slot, buf);
    if (g_px[buf]) {
        sys_munmap(g_px[buf], g_px_bytes[buf]);
        g_px[buf] = 0;
    }
    sys_shm_unlink(nm);
    int fd = sys_shm_open(nm, bytes, SHM_CREATE | SHM_EXCL);
    if (fd < 0) return 0;
    buf_grant(nm);
    void *p = sys_mmap(0, bytes, SYS_PROT_READ | SYS_PROT_WRITE,
                       SYS_MAP_SHARED, fd, 0);
    sys_close(fd);
    if (p == (void *)-1) return 0;
    g_px[buf] = p;
    g_px_bytes[buf] = bytes;
    return 1;
}

static int bufs_create(int slot, int w, int h) {
    uint64_t bytes = (uint64_t)w * (uint64_t)h * 4;
    if (!bytes) return 0;
    g_slot = slot;
    for (int b = 0; b < UAPP_BUFS; b++)
        if (!buf_make(slot, b, bytes)) return 0;
    return 1;
}

// Makes sure buffer `buf` is `w` x `h` before the client draws into it,
// replacing the object and telling the server if it is not.
//
// **THE STALE HALF IS THE CLIENT'S PROBLEM NOW.** A resize replaces only
// the buffer being drawn into; the other one is still the old size and
// is the one the client draws into NEXT. The server used to grow it at
// present time and cannot any more -- the memory belongs to the client.
static void req_clear(struct win_request_msg *r);
static int req_send(struct win_request_msg *r);

static int buf_ensure(uint32_t window, int buf, int w, int h) {
    // ROUNDED THE SAME WAY buf_make() rounds, or the comparison never
    // matches and this replaces the buffer on EVERY present -- handing
    // the app a freshly zeroed one each frame, which shows up as a
    // scene that will not hold still.
    uint64_t want = ((uint64_t)w * (uint64_t)h * 4 + 4095) & ~4095ULL;
    if (g_px[buf] && g_px_bytes[buf] == want) return 1;
    if (!buf_make(g_slot, buf, want)) return 0;

    struct win_request_msg req;
    req_clear(&req);
    req.type = WIN_REQ_BUFFER;
    req.window = window;
    req.a = buf;
    req.b = w;
    req.c = h;
    return req_send(&req) == 1;
}

static void bufs_release(void) {
    for (int b = 0; b < UAPP_BUFS; b++) {
        if (!g_px[b]) continue;
        char nm[WIN_BUF_NAME_MAX];
        buf_name(nm, sizeof nm, g_slot, b);
        sys_munmap(g_px[b], g_px_bytes[b]);
        sys_shm_unlink(nm);
        g_px[b] = 0;
    }
}

struct uapp {
    // WHICH OF THE WINDOW'S TWO BUFFERS THE COMPOSITOR IS READING. The
    // surface always points at the OTHER one -- see present(). 0 until
    // the first present, and 0 forever for a single-buffered window,
    // so nothing here needs to know which kind it has.
    int front;
    const struct uapp_desc *desc;
    uint32_t window;
    int w, h;
    struct ugfx_surface surface;
    int dirty;    // something asked for a repaint since the last present
    int focused;  // keyboard focus, per WIN_EV_FOCUS
    int running;
    int status;
    int timer_armed; // TWS accepted a WIN_REQ_TIMER, so on_tick arrives
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
};

// One process, one window -- which is what every client does today, and
// what WIN_CLIENT_MAX being untested above 1 means (docs/roadmap.md's
// M41). A single static instance rather than a heap allocation: there
// is no allocator in libsys, and one window per process makes the
// multi-instance machinery the kernel-space apps need unnecessary here.
// The handle is opaque precisely so a future uapp_window_create() can
// appear without this API changing shape.
static struct uapp g_app;

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

static void layout_log_flush(void);

static void present(struct uapp *a) {
    struct win_request_msg req;
    req_clear(&req);
    req.type = WIN_REQ_PRESENT;
    req.window = a->window;
    int rc = req_send(&req);
    // THE SERVER ANSWERS WITH THE NEW FRONT INDEX, biased by one so 0
    // can still mean "refused" (abi/win_proto.h). The next frame must
    // go into the OTHER buffer -- drawing into the one just handed to
    // the compositor is precisely the tearing this exists to remove.
    //
    // A refusal leaves the surface where it was, which is correct: if
    // the present did not happen, neither did the flip.
    if (rc > 0) {
        a->front = rc - 1;
        // The BACK buffer for the new front: the other of the two. It
        // may still be the pre-resize size, so make it current before
        // handing it over as a surface.
        int back = a->front ^ 1;
        if (!buf_ensure(a->window, back, a->w, a->h)) return;
        a->surface = ugfx_surface_for_pixels(g_px[back], a->w, a->h);
    }
}

// Draw + present, but only if something actually asked. This is the
// coalescing uapp_redraw() promises: a burst of events costs one round
// trip, not one per event.
static void flush(struct uapp *a) {
    if (!a->dirty) return;
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
    layout_log_flush();
    present(a);
}

// --- public: state ----------------------------------------------------

void uapp_redraw(struct uapp *a) { a->dirty = 1; }

int uapp_post(struct uapp *a, int a0, int a1) {
    // NOT `a->window`, and not a->anything: this runs on a WORKER
    // THREAD, where reading toolkit state is the exact thing uapp.h
    // says not to do. The request needs no window -- the compositor
    // routes a self-post by pid -- so the parameter is here for symmetry
    // with every other call in this header and is deliberately unread.
    (void)a;
    struct win_request_msg req;
    for (unsigned i = 0; i < sizeof req; i++) ((char *)&req)[i] = 0;
    req.type = WIN_REQ_EVENT_PUSH;
    req.a = 0;              // 0 = "me", the only target a client may name
    req.b = WIN_EV_USER;    // and the only type it may send
    req.c = a0;
    req.d = a1;
    return req_send(&req);
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
        struct setting_msg msg;
        for (unsigned i = 0; i < sizeof msg; i++) ((uint8_t *)&msg)[i] = 0;
        msg.op = SETTING_OP_GET;
        // The qualified name: identity is (namespace, name), and the
        // namespace is the registered name of the file it lives in.
        const char *n = "desktop.layout_log";
        unsigned k = 0;
        while (n[k] && k < sizeof msg.name - 1) { msg.name[k] = n[k]; k++; }
        msg.name[k] = '\0';
        g_layout_log = (sys_setting(&msg) == 0 && msg.value[0] == 'o'
                        && msg.value[1] == 'n') ? 1 : 0;
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
#define LAYOUT_BLOCK_MAX 2048
#define LAYOUT_BLOCK_MARK "uapp: layout log TRUNCATED -- raise LAYOUT_BLOCK_MAX\n"
#define LAYOUT_BLOCK_USABLE (LAYOUT_BLOCK_MAX - (int)sizeof(LAYOUT_BLOCK_MARK))
static char g_block[LAYOUT_BLOCK_MAX];
static int  g_block_len;
static int  g_block_over;
static char g_block_prev[LAYOUT_BLOCK_MAX];
static int  g_block_prev_len;

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
static void layout_log_flush(void) {
    if (!g_layout_log || !g_block_len) { g_block_len = g_block_over = 0; return; }
    if (g_block_over) {
        for (const char *p = LAYOUT_BLOCK_MARK; *p; p++) g_block[g_block_len++] = *p;
        g_block_over = 0;
    }
    int same = (g_block_len == g_block_prev_len);
    for (int i = 0; same && i < g_block_len; i++)
        if (g_block[i] != g_block_prev[i]) same = 0;
    if (!same) {
        g_block[g_block_len] = '\0';
        ulog(g_block);
        for (int i = 0; i < g_block_len; i++) g_block_prev[i] = g_block[i];
        g_block_prev_len = g_block_len;
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

int uapp_resize(struct uapp *a, int w, int h) {
    struct win_request_msg req;
    req_clear(&req);
    // THE BUFFER IS REPLACED BEFORE THE REQUEST, because the kernel
    // re-adopts by name and has to find the NEW object there. Only the
    // BACK one: the front is still showing the last finished frame at
    // the old size, and replacing it would be the window of black this
    // whole handshake exists to avoid (abi/win_proto.h).
    // NO WIN_REQ_BUFFER HERE, and that is not an oversight: the resize
    // request below re-adopts the buffer by name itself, so telling the
    // server twice costs a second syscall on a path a live drag takes
    // per mouse move -- enough for the WM's own lag measurement to
    // decide the client cannot keep up and fall back to an outline.
    int back = a->front ^ 1;
    if (!buf_make(g_slot, back, (uint64_t)w * (uint64_t)h * 4)) return 0;

    req.type = WIN_REQ_RESIZE;
    req.window = a->window;
    req.a = w;
    req.b = h;
    // WHICH BUFFER WAS PREPARED. Both sides deriving it from `front`
    // separately is a disagreement waiting for a present to land in
    // between -- and it did.
    req.c = back;
    if (req_send(&req) != 1) return 0;

    // The server hands back what it actually granted rather than what
    // was asked for. The surface is rebuilt because buf_make() mapped a
    // NEW object, at a new address and a new row stride.    //
    // STILL THE BACK BUFFER FOR THE CURRENT FRONT: only that one was
    // rebuilt at the new size, and the front is still showing the last
    // frame at the old one (abi/win_proto.h's configure/ack).
    a->w = req.a;
    a->h = req.b;
    a->surface = ugfx_surface_for_pixels(g_px[a->front ^ 1], a->w, a->h);
    if (a->desc->layout) uui_layout_run(a->desc->layout, 0, 0, a->w, a->h);
    return 1;
}

// THE COMPOSITOR'S CHANNEL, opened once and kept. Lazily, because a
// client that never sets a title should not publish a ring, and because
// the compositor may not have its beacon up when an app starts.
static struct uchan_client g_wmchan;
static int g_wmchan_state;   // 0 untried, 1 open, -1 no compositor channel

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

// Sends one request over the channel. Returns 1 if it went, 0 if the
// caller should take the kernel path -- there is no compositor channel,
// or its ring is full, and neither is a reason to drop the request.
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

void uapp_set_cursor(struct uapp *a, int cursor) {
    if (cursor < 0 || cursor >= WIN_CURSOR_COUNT) return;
    if (a->cursor == cursor) return;
    // Either way: a server that refuses this (one built before the
    // request existed) must not be asked again on every motion.
    a->cursor = cursor;
    wmchan_send(WIN_REQ_CURSOR, a->window, cursor, 0, 0, 0);
}

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
    struct win_request_msg req;
    req_clear(&req);
    req.type = WIN_REQ_CLOSE_PID;
    req.a = pid;
    return req_send(&req) == 1;
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
    // The op is not carried here: an app that cares reads the
    // clipboard, which is what it had to do for the payload anyway.
    d->on_clipboard(a, 0, now);
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

int uapp_spawn(struct uapp *a, const char *path, const char *args) {
    int pid = sys_spawn(path, args, -1);
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

static void dispatch(struct uapp *a, const struct win_event *ev) {
    const struct uapp_desc *d = a->desc;

    clip_poll(a);
    reap_children();

    switch (ev->type) {
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
        struct win_request_msg req;
        req_clear(&req);
        req.type = WIN_REQ_PONG;
        req.window = a->window;
        req.a = ev->a; // the serial, echoed unchanged
        req_send(&req);
        break;
    }

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
        if (uapp_resize(a, ev->a, ev->b)) {
            if (d->on_resize) d->on_resize(a, a->w, a->h);
            a->dirty = 1;
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
        if (id && d->on_widget) d->on_widget(a, id, UUI_REASON_WHEEL);
        if (!id && d->on_wheel) { d->on_wheel(a, ev->a); a->dirty = 1; }
        break;
    }

    case WIN_EV_FOCUS:
        // Recorded and repainted for the app, so the common case --
        // "stop drawing my caret when I am not focused" -- needs no
        // callback, just uapp_focused() in on_draw.
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
        if (a->router.count) {
            int changed = 0;
            int id = uui_router_overlay_key(&a->router, ev->a, ev->mods, &changed);
            if (changed) a->dirty = 1;
            if (id) {
                if (d->on_widget) d->on_widget(a, id, UUI_REASON_KEY);
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
                if (id) d->on_widget(a, id, UUI_REASON_KEY);
            }
        }
        if (d->on_key) d->on_key(a, ev->a, ev->mods);
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
            if (id && d->on_widget) d->on_widget(a, id, UUI_REASON_PRESS);
            // Keyboard focus follows the click, after the widgets have had
            // the press (a widget takes the pointer grab; this only moves
            // which one keys go to). See uui_focus_click().
            if (d->focus && uui_focus_click(d->focus, ev->a, ev->b)) a->dirty = 1;
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
            int id = uui_router_motion(&a->router, ev->a, ev->b, held, &changed);
            if (changed) a->dirty = 1;
            if (id && d->on_widget) d->on_widget(a, id, UUI_REASON_MOTION);
        }
        if (d->buttons) {
            int changed = held ? uui_button_group_press(d->buttons, ev->a, ev->b)
                                : uui_button_group_hover(d->buttons, ev->a, ev->b);
            if (changed) a->dirty = 1;
        }
        // The widget tree answers first; on_motion below overrides.
        if (a->router.count) {
            uapp_set_cursor(a, uui_router_cursor(&a->router, ev->a, ev->b));
        }
        if (d->on_motion) d->on_motion(a, ev->a, ev->b, ev->mods);
        break;

    case WIN_EV_MOUSE_UP:
        a->mouse_x = ev->a;
        a->mouse_y = ev->b;
        if (a->router.count) {
            int changed = 0;
            int id = uui_router_release(&a->router, ev->a, ev->b, &changed);
            if (changed) a->dirty = 1;
            if (id && d->on_widget) d->on_widget(a, id, UUI_REASON_RELEASE);
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
// so TWS raises its window and this returns 1, meaning "you are the
// second copy, go away quietly".
//
// Asked BEFORE the window is created, and before the font is mapped,
// so the redundant copy costs one round trip and never appears -- a
// window that flashes up and vanishes is worse than no single-instance
// support at all.
//
// An app with the flag but no id gets 0: nothing to match on, so it
// opens normally rather than silently refusing to start. That is the
// safer direction of the two.
static int activate_existing(const struct uapp_desc *desc) {
    if (!(desc->flags & UAPP_SINGLE_INSTANCE)) return 0;
    // NOTHING IS SENT. The server answers from this process's own spawn
    // path, so the flag alone is the whole declaration -- an app_id is
    // no longer required, and cannot be got wrong. See
    // WIN_REQ_ACTIVATE.
    struct win_request_msg req;
    req_clear(&req);
    req.type = WIN_REQ_ACTIVATE;
    return req_send(&req) == 1;
}

static int uapp_open(struct uapp **out, const struct uapp_desc *desc) {
    struct uapp *a = &g_app;
    a->desc = desc;
    a->dirty = 1;
    a->running = 1;
    // A window is frontmost the moment it is created, so TWS sends no
    // event to say so -- see wm_client.c's on_window_created().
    a->focused = 1;
    a->status = 0;

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

    // **NO COMPOSITOR CHANNEL, NO WINDOW.** The pixels are this
    // process's own memory and the compositor gets at them only because
    // this grants it -- so without the beacon that names it, a window
    // would open, draw, present and never appear. Refusing here is the
    // loud version of that, and it is what stage 6 does anyway: no
    // compositor, no window.
    if (comp_pid() <= 0) return 0;

    struct win_request_msg req;
    req_clear(&req);
    // THE CLIENT ALLOCATES, AND PROPOSES ITS OWN SLOT. `window` is an
    // output on CREATE and free as an input, which is what lets the
    // buffers be NAMED, granted and mapped before the kernel has
    // answered.
    if (!bufs_create(0, a->w, a->h)) return 0;

    req.type = WIN_REQ_CREATE;
    req.window = 0;
    req.a = a->w;
    req.b = a->h;
    // 0/0 takes TWS's own cascade rather than stacking every client on
    // the origin -- see win_server.c's on_window_created().
    req.c = desc->x;
    req.d = desc->y;
    copy_text(req.text, desc->app_id);
    if (req_send(&req) != 1) { bufs_release(); return 0; }
    a->window = req.window;

    if (desc->title) uapp_set_title(a, desc->title);

    // Declare behaviour. Sent unconditionally, including when the app
    // asked for nothing: "fixed size, no minimum" is a statement, and
    // leaving TWS to assume it would be the inference this protocol
    // deliberately avoids.
    wmchan_send(WIN_REQ_HINTS, a->window, (int)desc->flags,
                desc->min_w, desc->min_h, 0);

    // Arm the repeating timer, if the app asked for one. Only useful
    // alongside an on_tick, which is the only thing it drives -- arming
    // it without one would wake the process to do nothing, which is a
    // slower version of the problem it exists to solve.
    if (desc->tick_ms && desc->on_tick) {
        req_clear(&req);
        req.type = WIN_REQ_TIMER;
        req.window = a->window;
        req.a = (int)desc->tick_ms;
        // A refusal is survivable and deliberately not fatal: uapp_run()
        // checks the same condition and falls back to polling, so an
        // older server that has never heard of this simply gets the old
        // behaviour instead of an app that never ticks.
        a->timer_armed = (req_send(&req) == 1);
    }
    // THE BACK BUFFER, which with front 0 is buffer 1. Starting on
    // buffer 0 paints into the one the compositor IS showing, which
    // reads as a window that opens and draws nothing.
    a->surface = ugfx_surface_for_pixels(g_px[a->front ^ 1], a->w, a->h);

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

static int uapp_pump(struct uapp *a, int block) {
    struct win_event ev;

    if (block) {
        // sys_wait_event() already absorbs the SYS_RETRY sentinel, so a
        // 0 here is a real refusal rather than "ask again".
        if (sys_wait_event(&ev) != 1) return 0;
        dispatch(a, &ev);
    }
    // Drain whatever else is queued, blocking or not: one wake often
    // carries several events, and handling them together is what makes
    // the single coalesced present below correct rather than laggy.
    while (a->running && sys_poll_event(&ev) == 1) dispatch(a, &ev);

    flush(a);
    return a->running;
}

static void uapp_close(struct uapp *a) {
    struct win_request_msg req;
    req_clear(&req);
    req.type = WIN_REQ_DESTROY;
    req.window = a->window;
    req_send(&req);
}

int uapp_run(const struct uapp_desc *desc) {
    struct uapp *a;

    // Already running? Its window has been raised; this copy's whole
    // job is done. Exit 0 -- the user asked for the app and got it, so
    // the launch SUCCEEDED, and reporting a failure here would put an
    // error in the log for the case that works.
    if (activate_existing(desc)) return 0;

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
