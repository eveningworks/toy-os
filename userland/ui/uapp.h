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
struct uui_focus;   // ui/uui_focus.h -- desc.focus is a pointer, so a
                    // forward decl keeps that header out of every app.

// Behaviour, declared to TWS at open. Absent = the defaults: a
// fixed-size window. Declared rather than inferred -- the server does
// not guess resizability from a window's size (abi/win_proto.h).
#define UAPP_RESIZABLE 0x01

// Only one copy of this app at a time. With this set (and an `app_id`
// given), uapp_run() asks TWS whether a window already carries that id
// BEFORE it opens one: if so it raises that window and this process
// exits 0 without ever appearing on screen, and if not it opens
// normally and registers the id.
//
// The DECISION is here, in the app, on purpose. A launcher launches
// (apps/gui_apps.h's exec_path), because the desktop cannot know
// whether a second copy is meaningful -- two Notepads editing two files
// are useful and two Task Managers are not. An app that leaves this
// unset behaves exactly as every app did before it existed.
//
// Note what this is NOT: a lock. Two copies started in the same instant
// can both find nothing and both open -- see WIN_REQ_ACTIVATE's known
// gap. It is the pattern real desktops use for launching, not a
// mutual-exclusion primitive to build on.
#define UAPP_SINGLE_INSTANCE 0x02

// The drawing context handed to on_draw. Carries the surface AND the
// theme defaults, which is what takes ugfx_draw_string()'s six
// arguments down to three for the common case.
struct uapp_draw {
    struct ugfx_surface *surface;
    uint32_t fg, bg;
};

struct uapp_desc {
    const char *title;

    // This app's own name for what its window IS -- "taskmgr", not a
    // path and not the title. Opaque, matched byte for byte, truncated
    // at WIN_APP_ID_LEN. NULL for the apps that need no identity, which
    // is most of them.
    //
    // Required by UAPP_SINGLE_INSTANCE and otherwise inert today, but
    // worth setting on anything with a lasting identity: this is the
    // field a taskbar groups by and the one a "raise that app" request
    // names, and both want it to have been there all along rather than
    // added later per app.
    const char *app_id;

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

    // How often on_tick should run, in milliseconds. 0 (the default)
    // keeps the old behaviour: on_tick runs as fast as the loop goes,
    // which POLLS -- the loop never blocks, and the process is runnable
    // every scheduling round for as long as it lives.
    //
    // Set it, and the library arms a TWS timer instead and BLOCKS in
    // between, so the app is woken exactly as often as it asked to be.
    // Pick the rate the app actually needs: a clock or a process list
    // wants 500-1000, an animation wants 10-33. Polling to do work
    // twice a second means waking a hundred times a second to decide
    // not to.
    //
    // Ignored without an on_tick, which is the only thing it drives.
    unsigned tick_ms;

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

    // Optional: a keyboard FOCUS ring over this app's focusable widgets.
    // With this set, uapp click-updates it on a press and routes keys
    // through it (Tab moves focus, everything else goes to the focused
    // widget) BEFORE on_key -- exactly as it routes the mouse through
    // `widgets`, so an app hand-rolls no uui_focus_click/uui_focus_key.
    // A SEPARATE list from `widgets` on purpose: the tab order and the
    // z-order are not the same, and a focusable carries a cut-down
    // `_focus_ops`. on_key still fires for any key the ring did not take,
    // and after a key the ring DID take (so an app can re-read the
    // focused widget's new value, the same way it reacts to on_widget).
    struct uui_focus *focus;

    // --- callbacks. ALL optional; NULL is not a special case. --------

    // After the window exists and the first draw is about to happen.
    void (*on_open)(struct uapp *a);

    // A control committed. Only fires when `buttons` is set.
    void (*on_action)(struct uapp *a, int code);

    // Repaint the content. With widgets or a layout declared, this runs
    // BENEATH them -- the library clears first, calls this, then paints
    // the widgets on top. That order is what makes an app's own
    // `ugfx_fill()` harmless: it can wipe its backdrop and never the
    // widgets. (UI Demo shipped completely blank because the opposite
    // order let its on_draw erase everything, and a 35-check suite
    // passed it -- every check read the app's log, and the widgets were
    // live, hit-testable and invisible.)
    //
    // With no widgets and no layout, nothing is cleared for you and this
    // owns the whole surface, exactly as before.
    void (*on_draw)(struct uapp *a, struct uapp_draw *d);

    // Painting that must land ON TOP of the widgets -- a readout over a
    // canvas, a drag ghost. Separate from on_draw rather than a flag on
    // it, so "above or below the widgets" is never ambiguous at the call
    // site.
    void (*on_draw_over)(struct uapp *a, struct uapp_draw *d);

    // THE SESSION FONT CHANGED (`fontface`, `fontsize`). By the time
    // this runs the toolkit has already re-mapped both weights and
    // re-run the layout, so an app needs this only if it CACHED
    // something measured from the font -- a column width, a wrapped
    // line count, its own window size. Most apps do not, which is why
    // this is optional and why the toolkit handles the event whether or
    // not an app implements it (ui/uapp.h's usual deal: an app that has
    // never heard of fonts still behaves correctly when one changes).
    void (*on_font)(struct uapp *a);

    // THE CLIPBOARD WAS REPLACED, by this app or any other (`op` is a
    // WIN_CLIP_OP_*, `serial` changes on every set). Same deal as
    // on_font: an app that has never heard of it behaves correctly
    // anyway, and one that draws a pending cut needs to stop drawing it
    // the moment somebody else copies. The payload is NOT here -- ask
    // with lib/uclip.h if you want it, which most apps never will.
    void (*on_clipboard)(struct uapp *a, int op, unsigned serial);

    void (*on_key)(struct uapp *a, int key, unsigned mods);

    // A key came UP. `key` is the code its PRESS produced, so a client
    // that recorded `key` on the way down can clear the same one --
    // pressing W, then Shift, then releasing W reports 'w', not 'W'
    // (abi/win_proto.h's WIN_EV_KEY_UP).
    //
    // **MOST APPS MUST NOT WANT THIS.** Text entry, menu shortcuts and
    // every widget in the toolkit act on the press; an app that also
    // acted on the release would do everything twice. This is for an app
    // holding a MODEL of what is currently held down -- a game, a
    // drag-modifier, a push-to-talk -- which is why it is a separate
    // callback an app opts into rather than a flag on on_key that every
    // existing handler would suddenly start seeing.
    //
    // The four modifier keys arrive here and in on_key as KEY_SHIFT,
    // KEY_CTRL, KEY_ALT and KEY_ALTGR (api/keyboard.h); they produce no
    // character, so they reach an app no other way.
    //
    // An unmatched release is POSSIBLE and must be tolerated: the WM
    // claims some presses as shortcuts (Super, Alt+F4) and delivers the
    // release regardless, the same shape an X11 grab produces. Ignoring
    // an up you have no down for is the correct handling.
    void (*on_key_up)(struct uapp *a, int key, unsigned mods);

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

    // Called on a cadence; repaints if it returns 1. WITH `tick_ms`
    // set the loop still BLOCKS and TWS wakes it on a timer, which is
    // what an app with a rate (a clock, a game's second hand) wants.
    // WITHOUT it the loop POLLS -- calls this once per pass and yields
    // -- which is what an animation with no natural rate needs, and
    // which costs a scheduling slot for as long as the app lives.
    int (*on_tick)(struct uapp *a);

    // Something this program posted to ITSELF arrived -- see
    // uapp_post(). `a0`/`a1` are whatever the poster put there;
    // repaints if it returns 1.
    //
    // **THE POSTER IS USUALLY A WORKER THREAD**, and this callback runs
    // on the MAIN one, which is the entire point: the worker produces a
    // result and posts, and the widget tree is only ever touched here.
    int (*on_user)(struct uapp *a, int a0, int a1);
};

// The ordinary path: open, run until closed, clean up. Returns the
// process exit status -- pass it straight back from main().
int uapp_run(const struct uapp_desc *desc);

// --- from inside a callback ------------------------------------------

// Wake this program's own event loop, carrying two numbers.
// `desc.on_user` receives them on the MAIN thread. Returns 0, or -1 if
// the queue is full.
//
// **THIS IS THE ONE CALL IN TOYKIT A WORKER THREAD MAY MAKE**, and the
// rule around it is the same one every real toolkit has: AppKit is
// main-thread-only, Qt widgets are main-thread-only, GTK the same. A
// worker computes into memory it owns and then posts; it must never
// touch a widget, `ugfx_*`, or the window buffer, because the main
// thread may be drawing from either at that instant.
//
// Two numbers rather than a pointer because the event is a MESSAGE --
// fixed-layout and readable in a log, the same reason the protocol
// spells events out field by field. A worker with a result bigger than
// that puts it somewhere both threads agreed on and posts an index.
int uapp_post(struct uapp *a, int a0, int a1);

// Marks the content dirty. The loop draws and presents ONCE before it
// next blocks, so a mouse drag crossing three buttons costs one present
// rather than three, and an app can call this from as many places as it
// likes without thinking about it. Apps stop containing the phrase "and
// don't forget to present".
void uapp_redraw(struct uapp *a);

// Logs `<prefix>: layout <name> x y w h` for each NAMED item (uui_item
// .name) with a `bounds` op, then whatever the widget's own `describe`
// op adds -- the geometry a test drives the app by, in the vocabulary
// ui/uui_describe.h states. Walks `.layout` and `.widgets`, entering
// containers. Call it from on_draw, after the app has placed anything
// it places by hand. Content-relative. An app adds only what no widget
// knows (a board it draws itself) through uapp_logf_layout().
void uapp_log_layout(struct uapp *a, const char *prefix);

// The same report for ONE widget the app owns outside its arrays -- a
// menu bar an app with hand-drawn chrome routes itself (Minesweeper).
// Same vocabulary, so a test helper written for the walk reads it.
void uapp_log_widget(struct uapp *a, const char *prefix, const char *name,
                     const struct uui_widget_ops *ops, const void *widget);

// One extra layout line, for an app whose report says something the
// widget walk above cannot -- a cursor cell, a pane rect it draws
// itself. Subject to the SAME gate and the same per-frame dedupe, which
// is the whole reason it exists rather than each app calling ulog()
// directly: a line written straight to the log is one this cannot
// suppress, and it was ~20 such lines a frame that made `dmesg` useless
// and `dmesg -w` a feedback loop.
//
// Include the trailing newline; the block is emitted verbatim.
void uapp_log_layout_line(const char *line);

// The formatted form of the same thing, and what an app's own
// `log_layout()` should call instead of ulogf(). Same gate, same
// per-frame dedupe.
void uapp_logf_layout(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

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

// Name the WIN_CURSOR_* for this window's content area. THE ESCAPE
// HATCH: a widget declares its own shape (uui_widget_ops.cursor) and
// the toolkit sets it for you. This is for surfaces that are not
// widgets -- Notepad's document, the Terminal's grid.
//
// A STATE, not an event: call it on every motion INCLUDING to set it
// back, or an app that only sets TEXT on the way in keeps the I-beam
// over its own toolbar. Repeats send nothing. Runs after the widget
// tree, so an app has the last word over its own window.
void uapp_set_cursor(struct uapp *a, int cursor);

// Bracket a blocking stretch: the busy pointer, then whatever was there
// before. The toolkit remembers, because "restore to what" is a
// question every app would otherwise answer differently and wrongly.
//
// Say so BEFORE going quiet. The request reaches the compositor through
// the kernel, so it lands even while this process is not pumping its
// queue -- but only if it was sent first. DOES NOT NEST.
//
// The WM raises the busy pointer by itself once a window stops
// answering pings, so this is for work that is slow ON PURPOSE and
// finishes: it says "working", where the WM's says "not responding".
void uapp_busy_begin(struct uapp *a);
void uapp_busy_end(struct uapp *a);

// Asks the WM to close every window owned by `pid` -- the POLITE way to
// end another process, which it may refuse. Returns 1 if at least one
// window was asked, 0 if that pid has none. See sys_kill() for the half
// that cannot be refused.
int uapp_request_close_pid(struct uapp *a, int pid);

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
