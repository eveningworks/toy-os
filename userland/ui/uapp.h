#ifndef UAPP_H
#define UAPP_H

#include <stdint.h>
#include "ui/ugfx.h"
#include "ui/uui_button_group.h"
#include "ui/uui_layout.h"

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

// Behaviour, declared to TWS at open. Absent = the defaults: a
// fixed-size window. Declared rather than inferred -- the server does
// not guess resizability from a window's size (abi/win_proto.h).
#define UAPP_RESIZABLE 0x01

// The drawing context handed to on_draw. Carries the surface AND the
// theme defaults, which is what takes ugfx_draw_string()'s six
// arguments down to three for the common case.
struct uapp_draw {
    struct ugfx_surface *surface;
    uint32_t fg, bg;
};

struct uapp_desc {
    const char *title;

    // The content, laid out. With this set the library sizes the window
    // from the layout's natural size, re-runs the layout whenever the
    // window's size is known, and draws it -- so an app with nothing
    // custom to paint needs no on_size and no on_draw at all.
    //
    // The library clears the window to the theme background, draws the
    // layout, and only then calls on_draw -- so app-specific painting
    // goes on top of the widgets rather than under them, and an app
    // never has to remember to clear.
    struct uui_layout *layout;

    // Initial CONTENT size, for an app not using a layout. Either a
    // fixed pair, or -- preferred, and the same reasoning as the
    // kernel's gui_app::default_size() -- a callback that derives it
    // from the font, so a window is sized for the font it opens under.
    // Precedence is on_size, then layout, then w/h; all three run AFTER
    // the font is available.
    int w, h;
    void (*on_size)(int *w, int *h);

    // Where on screen. 0/0 -- the default -- takes TWS's own cascade
    // rather than stacking every client on the origin, which is
    // WIN_REQ_CREATE's own meaning for those fields, not an invention
    // here.
    int x, y;

    // UAPP_* behaviour flags, plus the smallest content size this app
    // will accept (0 = no opinion). Sent to TWS as hints once the
    // window exists.
    unsigned flags;
    int min_w, min_h;

    // Handed back to every callback via uapp_state(). The library never
    // looks inside it.
    void *state;

    // The widgets this app declares, and the ONLY thing it has to do to
    // get working mouse input. The library hit-tests them, delivers
    // press/motion/release/wheel, and holds a pointer GRAB so a drag
    // keeps reaching the widget that started it (ui/uui_route.h).
    //
    // **An app with these writes no input code.** Before they existed
    // the toolkit routed exactly one widget type -- see `buttons` below
    // -- so Calculator wrote none while UI Demo hand-dispatched
    // twenty-one calls, and a widget an app forgot to forward was
    // simply dead with nothing to notice it.
    //
    // The same `struct uui_item` array a uui_layout takes, so an app
    // that lays out with the toolkit passes the array it already has,
    // and one that places widgets itself (UI Demo, so its coordinates
    // stay documented for tests) declares the array and no layout.
    struct uui_item *widgets;
    int widget_count;

    // A widget changed. `id` is the one the app put on that item, and
    // `reason` is the input that caused it (enum uui_reason: press,
    // motion, release, wheel). The app reads the new VALUE from the
    // widget itself -- uui_dropdown_selected(), cb.checked,
    // list.selected -- so this stays one callback and a switch rather
    // than a callback pointer stapled to every widget struct.
    void (*on_widget)(struct uapp *a, int id, int reason);

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

    // Wheel notches: + is up/away, - is down/toward. An app with
    // scrollable content wants this; one without can ignore it.
    void (*on_wheel)(struct uapp *a, int notches);

    // Keyboard focus arrived (1) or left (0). Most apps do not need
    // this: the library records it and repaints, so an app that draws
    // its caret with uapp_focused() needs no callback at all.
    void (*on_focus)(struct uapp *a, int focused);

    // The window was resized -- the library has already reallocated the
    // buffer, rebuilt the surface and re-run the layout before calling
    // this. Most apps need nothing here, which is the point: **an app
    // with a layout and no on_resize resizes correctly**, because the
    // layout was re-run for it.
    void (*on_resize)(struct uapp *a, int w, int h);

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

// Draw and present NOW, if anything is dirty, instead of waiting for
// the loop to come round. For a handler that is about to BLOCK and
// wants what it has already produced on screen first -- Terminal echoes
// the command line, flushes, and only then runs the shell, which can
// take seconds. Without this the echo would appear after the command it
// echoed.
//
// Not the normal way to paint: uapp_redraw() and let the loop coalesce.
// Reach for this only when something long is about to happen inside a
// callback.
void uapp_flush(struct uapp *a);

void uapp_quit(struct uapp *a, int status);
int uapp_set_title(struct uapp *a, const char *title);

// Ask TWS for a different content size. Returns 0 if it refused, in
// which case nothing changed. Called for you when TWS proposes one;
// public because an app may also want to resize itself.
int uapp_resize(struct uapp *a, int w, int h);
void *uapp_state(struct uapp *a);

// Does this window have keyboard focus? An app drawing a caret should
// ask -- an unfocused window showing one claims to be taking input that
// is going somewhere else. Starts true: a window is frontmost the
// moment it is created.
int uapp_focused(const struct uapp *a);
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

// --- no escape hatch, and why ----------------------------------------
//
// There was one: uapp_open()/uapp_pump()/uapp_close(), so an app could
// drive the loop itself. The design predicted Terminal would need it,
// because Terminal BLOCKS inside a command -- the shell runs to
// completion in a function call -- and wants what it has already
// produced on screen first.
//
// Porting Terminal showed that is not a loop problem at all. It needs
// to PAINT at a moment of its choosing, which is uapp_flush(), one
// line. With that, Terminal uses uapp_run() like everything else, and
// the hatch had no caller in the tree.
//
// So it was removed rather than kept for a case that turned out not to
// exist. It comes back the day something genuinely owns its own loop --
// and the shape is already known, since it was written once.

#endif
