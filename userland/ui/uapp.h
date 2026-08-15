#ifndef UAPP_H
#define UAPP_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_button_group.h"

// uapp -- Toykit's application layer: describe your app, supply
// callbacks, and let the library own the window handshake and the event
// loop.
//
// WHAT THIS REPLACES, AND WHY
// ---------------------------
// Every TWP client used to hand-write the same three things: the
// create/title/present/destroy handshake (with a byte-at-a-time struct
// zero and a bounded copy loop for the title), a `for(;;)` around a
// `switch (ev.type)` with the same five arms, and a `draw(); present();`
// pair at every site that changed anything. In winclient.c that was
// roughly 55 of 141 lines before the app did anything of its own, and
// the mouse arms were near-identical between Calculator, Notepad and
// Shapes -- including the comment explaining them, re-typed each time.
//
// That is the same duplication crt0.asm and libsys already removed one
// layer down, where twenty programs each hand-rolled an `int $0x80`.
//
// THE PROPERTY WORTH THE ABSTRACTION
// ----------------------------------
// **Every callback is optional and the library has a defined default
// for every event.** That is what lets TWS gain a feature without every
// app being edited: a new event type arrives as a new optional callback
// plus a default, and an app that has never heard of it keeps working.
// The alternative -- what exists today -- is that shipping an event
// means editing every client's switch by hand, which is precisely why
// WIN_EV_RESIZE has been defined in TWP since the protocol was written
// and is still never sent.
//
// It is the same optional-slot vtable as the kernel's `display_driver`
// capabilities and `ui_focus_ops`, so it is not a new idiom to learn.
//
// WHAT THIS IS NOT
// ----------------
// A client-side library over TWP messages, and nothing more. It adds no
// syscalls and must not: the whole bet in abi/win_proto.h is that TWS
// can move to ring 3 as a transport swap, and a convenience layer that
// reached around the protocol would quietly cash that in.
//
// It also does not absorb the widgets. `on_draw` hands you a surface
// and the mouse callbacks hand you content-relative coordinates --
// exactly what uui_* already takes -- so the two compose without either
// knowing about the other, and a client that wants widgets without this
// framework (or this framework without widgets) can have either.

struct uapp;

// The drawing context handed to on_draw. Carries the surface AND the
// theme defaults, which is what takes ugfx_draw_string()'s six
// arguments down to three for the common case.
struct uapp_draw {
    struct ugfx_surface *surface;
    uint32_t fg, bg;
};

struct uapp_desc {
    const char *title;

    // Initial CONTENT size. Either a fixed pair, or -- preferred, and
    // the same reasoning as the kernel's gui_app::default_size() -- a
    // callback that derives it from the font, so a window is sized for
    // the font it opens under. The callback wins if both are given, and
    // it runs AFTER the font is available.
    int w, h;
    void (*on_size)(int *w, int *h);

    // Where on screen. 0/0 -- the default -- takes TWS's own cascade
    // rather than stacking every client on the origin, which is
    // WIN_REQ_CREATE's own meaning for those fields, not an invention
    // here.
    int x, y;

    // Handed back to every callback via uapp_state(). The library never
    // looks inside it.
    void *state;

    // Optional: a button group the library routes the mouse to. With
    // this set, press/motion/release are handled for you and a control
    // that COMMITS (pressed and released on the same button) arrives as
    // on_action. That keeps docs/gui-guidelines.md's
    // press-then-commit-on-release rule in one place instead of
    // re-implemented per app -- the same argument ui_textview.h makes
    // about scrolling having been copy-pasted into three apps until the
    // third copy shipped a scrollbar that drew and did nothing.
    struct uui_button_group *buttons;

    // --- callbacks. ALL optional; NULL is not a special case. --------

    // After the window exists and the first draw is about to happen.
    void (*on_open)(struct uapp *a);

    // A control committed. Only fires when `buttons` is set.
    void (*on_action)(struct uapp *a, int code);

    // Repaint the content. The library clears nothing first -- an app
    // owns its whole surface.
    void (*on_draw)(struct uapp *a, struct uapp_draw *d);

    void (*on_key)(struct uapp *a, int key, unsigned mods);

    // Content-relative, in pixels. `buttons` is offered these first;
    // these still fire, so an app can mix routed controls with its own
    // hit-testing (Shapes' checkbox, Notepad's text area).
    void (*on_press)(struct uapp *a, int x, int y, unsigned buttons);
    void (*on_release)(struct uapp *a, int x, int y, unsigned buttons);
    void (*on_motion)(struct uapp *a, int x, int y, unsigned buttons);

    // Return 0 to REFUSE the close; the default accepts. Refusing
    // politely is not the same problem as a client that never answers,
    // which is TWS's to solve (see docs/roadmap.md's M41).
    int (*on_close)(struct uapp *a);

    // Non-NULL turns the loop into an animating one: it polls instead
    // of blocking, calls this once per pass, repaints if it returns 1,
    // and yields. Shapes needs this -- there is no timer event, so its
    // animation is driven by its own loop's pace, and blocking would
    // freeze it.
    int (*on_tick)(struct uapp *a);
};

// The ordinary path: open, run until closed, clean up. Returns the
// process exit status -- pass it straight back from main().
int uapp_run(const struct uapp_desc *desc);

// --- from inside a callback ------------------------------------------

// Marks the content dirty. The loop draws and presents ONCE before it
// next blocks, so a mouse drag crossing three buttons costs one present
// rather than three, and an app can call this from as many places as it
// likes without thinking about it. Apps stop containing the phrase "and
// don't forget to present".
void uapp_redraw(struct uapp *a);

void uapp_quit(struct uapp *a, int status);
int uapp_set_title(struct uapp *a, const char *title);
void *uapp_state(struct uapp *a);
int uapp_width(const struct uapp *a);
int uapp_height(const struct uapp *a);

// --- drawing ----------------------------------------------------------
//
// Just the surface, for now. A `uapp_text(d, x, y, s)` taking the
// context's theme defaults -- the thing that would cut
// ugfx_draw_string()'s six arguments to three -- was written and then
// removed before landing: every client today draws with its own
// palette rather than the theme's, so it had no callers. It comes back
// with stage 1c's default on_draw, which is the caller that wants it.
// Same bar that had gfx_text_index_at_x() written, unused and deleted
// (see kernel/include/api/gfx.h).
struct ugfx_surface *uapp_surface(struct uapp_draw *d);

// --- escape hatch, for an app that owns its own loop -------------------
//
// Terminal is the case that forces this to exist: it blocks inside a
// command (the shell runs to completion in a function call) and wants
// to paint output as it arrives. That is an app driving the loop rather
// than the loop driving the app. uapp_run() is implemented in terms of
// these three.
int uapp_open(struct uapp **out, const struct uapp_desc *desc);

// Dispatches whatever has arrived, then repaints if anything asked for
// it. `block` waits for an event; otherwise it drains what is already
// queued and returns. Returns 0 once the app should exit.
int uapp_pump(struct uapp *a, int block);

void uapp_close(struct uapp *a);

#endif
