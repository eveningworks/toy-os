// See ui/uapp.h for what this is and why.
#include "rt/sys.h"   // TWP messages, sys_win_request(), sys_wait_event()
#include "ui/uapp.h"
#include "ui/utheme.h"

struct uapp {
    const struct uapp_desc *desc;
    uint32_t window;
    int w, h;
    struct ugfx_surface surface;
    int dirty;    // something asked for a repaint since the last present
    int running;
    int status;
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

static void present(struct uapp *a) {
    struct win_request_msg req;
    req_clear(&req);
    req.type = WIN_REQ_PRESENT;
    req.window = a->window;
    req_send(&req);
}

// Draw + present, but only if something actually asked. This is the
// coalescing uapp_redraw() promises: a burst of events costs one round
// trip, not one per event.
static void flush(struct uapp *a) {
    if (!a->dirty) return;
    a->dirty = 0;
    // With a layout: CLEAR, then draw it, then let the app paint on
    // top. The clear is the library's job because it is every app's
    // first line otherwise -- and getting it wrong is silent: an app
    // that clears in its own on_draw paints over the layout that was
    // just drawn, and the window comes up empty with no error anywhere.
    // (Written that way first. The test suite reported three failures
    // that all looked like "clicks do nothing"; a screenshot showed an
    // empty window and explained all three at once.)
    if (a->desc->layout) {
        ugfx_fill(&a->surface, UTHEME_PANEL_BG);
        uui_layout_draw(&a->surface, a->desc->layout);
    }
    if (a->desc->on_draw) {
        struct uapp_draw d = { &a->surface, UTHEME_TEXT, UTHEME_PANEL_BG };
        a->desc->on_draw(a, &d);
    }
    present(a);
}

// --- public: state ----------------------------------------------------

void uapp_redraw(struct uapp *a) { a->dirty = 1; }

void uapp_flush(struct uapp *a) { flush(a); }

void uapp_quit(struct uapp *a, int status) {
    a->running = 0;
    a->status = status;
}

void *uapp_state(struct uapp *a) { return a->desc->state; }
int uapp_width(const struct uapp *a) { return a->w; }
int uapp_height(const struct uapp *a) { return a->h; }

int uapp_resize(struct uapp *a, int w, int h) {
    struct win_request_msg req;
    req_clear(&req);
    req.type = WIN_REQ_RESIZE;
    req.window = a->window;
    req.a = w;
    req.b = h;
    if (req_send(&req) != 1) return 0;

    // The server hands back what it actually granted rather than what
    // was asked for, and the buffer is at the same virtual address as
    // before -- win_buffer_vaddr() derives it from the window id, so
    // the pointer never moves. Rebuilding the surface is only about the
    // new width being the new row stride.
    a->w = req.a;
    a->h = req.b;
    a->surface = ugfx_surface_for_window(a->window, a->w, a->h);
    if (a->desc->layout) uui_layout_run(a->desc->layout, 0, 0, a->w, a->h);
    return 1;
}

int uapp_set_title(struct uapp *a, const char *title) {
    struct win_request_msg req;
    req_clear(&req);
    req.type = WIN_REQ_TITLE;
    req.window = a->window;
    int i = 0;
    for (; title && title[i] && i < WIN_TITLE_LEN - 1; i++) req.text[i] = title[i];
    req.text[i] = '\0';
    return req_send(&req);
}

// --- public: drawing --------------------------------------------------

struct ugfx_surface *uapp_surface(struct uapp_draw *d) { return d->surface; }

// --- event dispatch ---------------------------------------------------

static void dispatch(struct uapp *a, const struct win_event *ev) {
    const struct uapp_desc *d = a->desc;

    switch (ev->type) {
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

    case WIN_EV_KEY:
        if (d->on_key) d->on_key(a, ev->a, ev->mods);
        break;

    case WIN_EV_MOUSE_DOWN:
        if (d->buttons && uui_button_group_press(d->buttons, ev->a, ev->b)) a->dirty = 1;
        if (d->on_press) d->on_press(a, ev->a, ev->b, ev->mods);
        break;

    case WIN_EV_MOUSE_MOVE:
        // With a button held this re-hit-tests the press, so dragging
        // off a control un-presses it; with none held it is hover
        // tracking. Both report "did anything change", so a cursor
        // crossing the window only repaints when it crosses a boundary.
        // This is the arm that was copied verbatim into three apps.
        if (d->buttons) {
            int changed = ev->mods ? uui_button_group_press(d->buttons, ev->a, ev->b)
                                    : uui_button_group_hover(d->buttons, ev->a, ev->b);
            if (changed) a->dirty = 1;
        }
        if (d->on_motion) d->on_motion(a, ev->a, ev->b, ev->mods);
        break;

    case WIN_EV_MOUSE_UP:
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

static int uapp_open(struct uapp **out, const struct uapp_desc *desc) {
    struct uapp *a = &g_app;
    a->desc = desc;
    a->dirty = 1;
    a->running = 1;
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

    struct win_request_msg req;
    req_clear(&req);
    req.type = WIN_REQ_CREATE;
    req.a = a->w;
    req.b = a->h;
    // 0/0 takes TWS's own cascade rather than stacking every client on
    // the origin -- see win_server.c's on_window_created().
    req.c = desc->x;
    req.d = desc->y;
    if (req_send(&req) != 1) return 0;
    a->window = req.window;

    if (desc->title) uapp_set_title(a, desc->title);

    // Declare behaviour. Sent unconditionally, including when the app
    // asked for nothing: "fixed size, no minimum" is a statement, and
    // leaving TWS to assume it would be the inference this protocol
    // deliberately avoids.
    req_clear(&req);
    req.type = WIN_REQ_HINTS;
    req.window = a->window;
    req.a = (int)desc->flags;
    req.b = desc->min_w;
    req.c = desc->min_h;
    req_send(&req);
    a->surface = ugfx_surface_for_window(a->window, a->w, a->h);

    // Now that the content size is settled, place everything in it.
    // Re-run rather than trusting the natural-size pass: the window may
    // have been created at a different size than was asked for.
    if (desc->layout) uui_layout_run(desc->layout, 0, 0, a->w, a->h);

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
    if (!uapp_open(&a, desc)) return 1;

    if (desc->on_tick) {
        // Animating: poll rather than block, so the app keeps moving
        // with no input. The yield is politeness, not a workaround --
        // a client doing real work every frame would otherwise take its
        // whole timeslice and make the desktop feel sticky.
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
