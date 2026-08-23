// See ui/uapp.h for what this is and why.
#include "rt/sys.h"   // TWP messages, sys_win_request(), sys_wait_event()
#include "ui/uapp.h"
#include "ui/uui_route.h"
#include "ui/uui_focus.h"   // desc.focus -- keyboard focus ring
#include "ui/ulog.h"        // uapp_log_layout()
#include "ui/utheme.h"

struct uapp {
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
    present(a);
}

// --- public: state ----------------------------------------------------

void uapp_redraw(struct uapp *a) { a->dirty = 1; }

// Emits `<prefix>: layout <id> x y w h` for every declared widget that
// has an id and a `bounds` op -- the standard "log my geometry so a test
// can drive me by asking, not by guessing pixels" that ~8 apps
// hand-rolled. Content-relative, exactly as the apps logged it; a test
// adds the window's content origin. Widgets with no id (0) or no bounds
// op are skipped, as are hidden ones.
void uapp_log_layout(struct uapp *a, const char *prefix) {
    for (int i = 0; i < a->router.count; i++) {
        struct uui_item *it = &a->router.items[i];
        if (it->hidden || !it->id || !it->ops || !it->ops->bounds) continue;
        int x, y, w, h;
        it->ops->bounds(it->widget, &x, &y, &w, &h);
        ulogf("%s: layout %d %d %d %d %d\n", prefix, it->id, x, y, w, h);
    }
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
    copy_text(req.text, title);
    return req_send(&req);
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

static void dispatch(struct uapp *a, const struct win_event *ev) {
    const struct uapp_desc *d = a->desc;

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
        if (d->focus && uui_focus_key(d->focus, ev->a, ev->mods)) a->dirty = 1;
        if (d->on_key) d->on_key(a, ev->a, ev->mods);
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
        int primary = (ev->mods & 0x1) != 0;
        if (primary) {
            // Routed FIRST, so a widget that wants this press gets it and
            // takes the pointer grab. The app's on_press still runs: an app
            // may want a press the widgets ignored (a canvas, a text area),
            // or may want to log one they took.
            int changed = 0;
            int id = a->router.count
                         ? uui_router_press(&a->router, ev->a, ev->b, &changed) : 0;
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

    struct win_request_msg req;
    req_clear(&req);
    req.type = WIN_REQ_CREATE;
    req.a = a->w;
    req.b = a->h;
    // 0/0 takes TWS's own cascade rather than stacking every client on
    // the origin -- see win_server.c's on_window_created().
    req.c = desc->x;
    req.d = desc->y;
    copy_text(req.text, desc->app_id);
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
    a->surface = ugfx_surface_for_window(a->window, a->w, a->h);

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
