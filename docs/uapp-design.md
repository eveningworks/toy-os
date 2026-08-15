# uapp design -- an app model for ring-3 GUI clients

**Status: DESIGN, not implemented.** Written the way
`docs/tfs3-design.md` was: decide the shape and the arguments first,
build it in named stages afterwards. Nothing in `userland/` implements
any of this yet.

**The one-sentence version:** ring-3 clients should describe themselves
and supply callbacks, the way kernel-space apps already do through
`struct gui_app` -- instead of each one hand-writing the window
handshake and the event loop, which is why the window manager cannot
gain a feature without every app being edited.

Decisions settled deliberately rather than defaulted, each with its own
section below: the loop lives in the library but an escape hatch exists
(Shapes animates, Terminal blocks inside a command); drawing stays
immediate-mode; window *behaviour* becomes protocol state instead of a
field on a kernel-side struct; and resize is a **configure/ack
handshake**, not a size the server imposes.

## The problem, measured

Every ring-3 client repeats the same three things.

**The handshake.** Create, then title, then present, then destroy on the
way out. Setting the title is a hand-written bounded copy loop into
`req.text` in all five clients. Zeroing the 56-byte request struct is a
byte-at-a-time `for` loop written out in four of them. Three clients
(`winclient.c`, `uiclient.c`, `calculator.c`) additionally define their
own private `win_request()`, `clear_req()` and `wait_event()` -- the
same three functions, copied, even though `userland/sys.c` has carried
`sys_win_request()`/`sys_wait_event()` since it was written. That
divergence is itself the tell: the newer clients found the library, the
older ones never got updated, and nothing made them.

**The loop.** All five write the same `for(;;)` around a
`switch (ev.type)` with the same five or six arms, and the mouse arms
are copied almost verbatim between Calculator, Notepad and Shapes --
including the `if (ev.mods) press else hover` line whose reasoning is
re-explained in a comment in each file.

**The bookkeeping.** Every site that changes something calls `draw()`
and then `present()` by hand, so a burst of events costs one
`WIN_REQ_PRESENT` round trip per event rather than one per settled
state.

In `winclient.c` -- the smallest client, whose entire job is "fill the
window with a colour and cycle on a keypress" -- that machinery is
roughly 55 of 141 lines.

This is exactly the duplication `userland/crt0.asm` and `userland/sys.c`
already deleted one layer down, where twenty programs each hand-rolled
an `int $0x80` stub. The bar `CLAUDE.md` sets for a new abstraction is a
second real caller. There are five.

## The app model already exists -- on the wrong side of the boundary

`apps/gui_apps.h`'s `struct gui_app` is precisely the design proposed
here, and it is already the most carefully documented interface in the
repo: `default_size()` for font-derived sizing, `on_open`, `on_draw`,
`on_key`, `on_press`/`on_release` with the commit-on-release rule
spelled out, `on_hover`, `on_wheel`, `resizable`, `multi_instance`.

The ring-3 migration did not replace that model with a worse one. It
**lost** it. A kernel-space app is a table of callbacks the WM drives; a
ring-3 client got a raw message queue and had to rebuild the driver
itself, once per app.

Two consequences follow directly, and the second is the one that
matters:

1. Every client re-derives the loop, with the copy-drift that implies.
2. **Window behaviour is unreachable from ring 3.** `wm_input.c`'s
   resize test is `if (!(w->app && w->app->resizable)) return -1;`, and
   a client window has `app == NULL` by construction. So no ring-3
   window can ever be resizable -- not by decision, but because
   "resizable" is a field on a struct that only kernel-space apps have.
   `gui windows --json` reports `"resizable": false` for Shapes today
   for exactly this reason. Any behaviour added the same way (always on
   top, no chrome, fixed aspect, a minimum size) inherits the same
   dead end.

`WIN_EV_RESIZE` is the visible scar: it has been defined in
`abi/win_proto.h` since the protocol was written, **nothing sends it and
nothing handles it**, and the reason it stalled is that shipping it
would have meant editing every client's loop by hand.

## Shape

A client describes itself and supplies callbacks; `uapp_run()` owns the
loop. Everything a window can do is a function on the app handle rather
than a message the app assembles.

```c
#include "uapp.h"

static void on_draw(struct uapp *a, struct ugfx_surface *s) { ... }

static void on_release(struct uapp *a, int x, int y, unsigned buttons) {
    (void)buttons;
    int code = uui_button_group_release(&g_group);
    if (code >= 0) calc_input(&g_calc, (char)code);
    uapp_redraw(a);
}

int main(void) {
    struct uapp_desc desc = {
        .title      = "Calculator",
        .on_size    = calc_size,        // font-derived, like default_size()
        .flags      = UAPP_FIXED_SIZE,
        .on_draw    = on_draw,
        .on_release = on_release,
        .on_key     = on_key,
    };
    return uapp_run(&desc);
}
```

## API sketch (`userland/uapp.h`)

Illustrative, not final -- the point is the shape. Types come from
`ugfx.h` and `win_proto.h`; nothing here is new kernel surface.

```c
struct uapp;   // opaque; one process, one app handle

// --- behaviour flags -------------------------------------------------
#define UAPP_FIXED_SIZE   0x01  // no resize grip (the default is resizable)
#define UAPP_NO_TITLE_BAR 0x02  // reserved -- see "Out of scope"
#define UAPP_CENTER       0x04  // let the server place it centred

struct uapp_desc {
    const char *title;

    // Initial CONTENT size. Either a fixed pair, or -- preferred, and
    // the same reasoning as gui_app::default_size() -- a callback that
    // derives it from the font the server handed over, so a window is
    // sized for the font it opens under. If both are given the callback
    // wins.
    int w, h;
    void (*on_size)(int *w, int *h);

    int min_w, min_h;      // 0 = the library's floor
    unsigned flags;        // UAPP_*
    void *state;           // opaque, handed back via uapp_state()

    // --- callbacks. ALL optional. A NULL callback is not an error and
    // not a special case: the library has a defined default for every
    // event, so an app only writes the ones it cares about. This is the
    // property that lets the WM grow without touching apps.
    void (*on_open)   (struct uapp *a);
    void (*on_draw)   (struct uapp *a, struct ugfx_surface *s);
    void (*on_key)    (struct uapp *a, int key, unsigned mods);
    void (*on_press)  (struct uapp *a, int x, int y, unsigned buttons);
    void (*on_release)(struct uapp *a, int x, int y, unsigned buttons);
    void (*on_motion) (struct uapp *a, int x, int y, unsigned buttons);
    void (*on_resize) (struct uapp *a, int w, int h);
    void (*on_focus)  (struct uapp *a, int focused);
    int  (*on_close)  (struct uapp *a);   // 0 refuses the close; default accepts
    int  (*on_tick)   (struct uapp *a);   // non-NULL = animate; see below
};

// --- the ordinary path -----------------------------------------------
int  uapp_run(const struct uapp_desc *desc);   // -> process exit status

// --- from inside a callback ------------------------------------------
void  uapp_redraw(struct uapp *a);                 // mark dirty; coalesced
void  uapp_quit(struct uapp *a, int status);
int   uapp_set_title(struct uapp *a, const char *title);
int   uapp_resize(struct uapp *a, int w, int h);   // ask; may be refused
void *uapp_state(struct uapp *a);
int   uapp_width(struct uapp *a);
int   uapp_height(struct uapp *a);
struct ugfx_surface *uapp_surface(struct uapp *a);

// --- escape hatch, for an app that owns its own loop ------------------
int uapp_open(struct uapp **out, const struct uapp_desc *desc);
int uapp_pump(struct uapp *a, int block);  // dispatch pending events;
                                            // 0 once the app should exit
void uapp_close(struct uapp *a);
```

### `uapp_redraw()` coalesces, and that is a real change

Today each client calls `draw()` + `present()` at every site that
changes state. `uapp_redraw()` only sets a dirty flag; the loop draws
and presents **once** before it next blocks. A mouse drag crossing three
buttons costs one present instead of three, and an app can call
`uapp_redraw()` from as many places as it likes without thinking about
it. Apps stop containing the phrase "and don't forget to present".

### `on_tick` is how an animating app stays honest

Shapes has no timer event, so it polls, advances an angle, presents and
`sys_yield()`s -- the comment in `gfxdemo.c` explains why blocking would
freeze it. With `on_tick` set, the library polls instead of blocking,
calls `on_tick` once per pass, redraws if it returns 1, and yields.
Shapes keeps its behaviour without keeping its loop.

### Why the escape hatch exists

Terminal is the case that forces it. It blocks inside a command -- the
shell runs to completion in a function call -- and wants to paint output
as it arrives. That is an app driving the loop, not the loop driving the
app, so `uapp_open()`/`uapp_pump()` stay available. Everything else uses
`uapp_run()`, which is implemented in terms of them.

## Protocol additions

`uapp` is a **client-side library over the existing messages**. It adds
no syscalls, and must not: the whole bet in `abi/win_proto.h` is that
the server can move to ring 3 as a transport swap
(`docs/roadmap.md` M41). A convenience layer that reached around the
protocol would quietly cash that in.

Stage 1 needs no protocol change at all. Stages 2-3 add three messages
and start sending one event that already exists:

| Message | Fields | Purpose |
|---|---|---|
| `WIN_REQ_HINTS` (6) | `a` = `UAPP_*` behaviour flags, `b`/`c` = min w/h | The client states how its window should behave. Sent once after create. |
| `WIN_REQ_RESIZE` (7) | `a`/`b` = requested content w/h | Reallocate and remap this window's buffer. On success `a`/`b` come back as the size actually granted. |
| `WIN_REQ_MOVE` (8) | `c`/`d` = screen x/y | Reposition. Lowest value of the three; may slip a stage. |
| `WIN_EV_RESIZE` (6, exists) | `a`/`b` = proposed content size | The server proposing a size. **Currently never sent.** |
| `WIN_EV_FOCUS` (7, new) | `a` = 1 gained / 0 lost | A caret should stop blinking when the window isn't focused. |

The WM change that makes hints matter is small and is the real fix for
the dead end above: `wm_find_resize_zone()` stops asking
`w->app->resizable` and asks the *window* instead, with a client's
answer coming from its hints and a kernel-space app's from its
`gui_app`. Behaviour becomes a property of the window, which both kinds
of window have, rather than of one kind's descriptor.

### Resize is a configure/ack handshake, not an imposed size

The obvious implementation -- the WM resizes the window on grip drag and
the client finds out afterwards -- is wrong here, and visibly so: the
server blits `client_w × client_h` pixels into whatever content
rectangle the chrome now describes, so the window would grow with the
content stuck at the old size in the corner. The buffer belongs to the
client; only the client can decide when it changes.

So, following Wayland's `xdg_toplevel` configure/ack (the same problem,
solved the same way, for the same reason):

1. The user drags the grip. The WM tracks a proposed size and draws a
   **rubber-band outline** -- the window itself does not change yet.
2. On release the WM clamps the proposal to the client's hinted minimum
   and sends `WIN_EV_RESIZE(w, h)`.
3. `uapp` receives it, issues `WIN_REQ_RESIZE`, and the server frees the
   old frames, allocates new ones and maps them **at the same virtual
   address** -- which the protocol already guarantees, since
   `win_buffer_vaddr(id)` is derived from the window id and not returned
   by the server. The client's buffer pointer stays valid across a
   resize by construction. This is the fixed-vaddr decision paying off
   in a way it wasn't designed for.
4. `uapp` rebuilds its `ugfx_surface` (stride changes with width), calls
   `on_resize` if the app supplied one, then `on_draw`, then presents.
   The WM adopts the new content size when the present arrives.
5. If the server refuses (out of frames, over `WIN_CLIENT_MAX_W/H`), the
   client keeps the size it had and the window does not change. A
   refusal is a normal outcome, not an error path.

**An app that supplies no `on_resize` still resizes correctly** -- the
library reallocates, rebuilds the surface and repaints. That is the
whole argument for this design in one sentence.

This is also the same shape as the close button, which
`docs/decisions.md` already records as "a handshake, not a seizure".
Two operations, one precedent.

## Decisions settled deliberately

**Drawing stays immediate-mode.** `on_draw` repaints the content; widgets
do not retain damage state. It matches `apps/ui/`, it matches what all
five clients do now, and retained-mode is where toolkits get genuinely
hard. Damage tracking already exists one level up, in the compositor,
which is where it belongs.

**Callbacks are optional, with library defaults.** A new event type ships
as a new optional callback plus a default the library applies. Existing
apps are not edited, not recompiled-with-changes, not even aware. This
is the same optional-slot vtable as `display_driver`'s capability bits
and `ui_focus_ops`, so it is not a new idiom to learn.

**Behaviour is declared, not inferred.** A client says
`UAPP_FIXED_SIZE`; the WM does not guess from the window's size or its
title. Same principle as `fs_ops.caps` and `display_driver`'s honesty
rule -- and worth applying the same way, by having the server refuse a
hint that contradicts itself (a minimum larger than
`WIN_CLIENT_MAX_*`) rather than silently clamping.

**One process, one window, still.** `uapp` handles a single window,
because that is what every client does today and what `WIN_CLIENT_MAX`
being untested at >1 means (M41 lists it). The handle is opaque
specifically so a future `uapp_window_create()` can appear without the
single-window API changing shape.

**`uapp` is ring-3 only; `gui_app` stays as it is.** No attempt to
unify the two toolkits now. `apps/ui/` and `userland/uui.c` are already
separate ports and M41 has the kernel-space side retiring eventually;
the right move is to make the ring-3 side the good one and let porting
flow in that direction, not to build a shared abstraction over a layer
that is scheduled to disappear.

**Widgets stay where they are.** `uapp` does not absorb `uui`. It hands
`on_draw` a surface and hands the mouse callbacks content-relative
coordinates -- exactly what `uui_button_group_*` already takes. The two
compose without either knowing about the other, which is what keeps
`uui` usable from a client that wants no app framework at all.

## What each client becomes

Estimates, listed so they can be checked against reality afterwards
rather than quietly forgotten.

| Client | Today | After | What goes |
|---|---:|---:|---|
| `winclient.c` | 141 | ~50 | the whole handshake and loop; it becomes a paint function and a keypress |
| `uiclient.c` | 189 | ~95 | ditto |
| `calculator.c` | 282 | ~205 | 3 private syscall helpers, the create/title dance, the 35-line switch |
| `gfxdemo.c` | 298 | ~235 | the poll/yield loop becomes `on_tick` |
| `terminal.c` | 191 | ~165 | handshake only -- it keeps its own loop via the hatch |
| `notepad.c` | 565 | ~490 | handshake + switch; gains resize for free |

The absolute saving is not the point -- roughly 300 lines. The point is
that the ~55 lines removed from each are the *same* 55 lines, and that
the next client written pays none of it.

## Staging

Each stage is a commit that builds, tests and stands on its own -- the
A-E pattern TFS3 used.

**Stage 1 -- the library, no kernel changes.** `userland/uapp.c`/`.h`
over today's five messages. Port `winclient`, `uiclient` and
`calculator`. Their existing test tools must pass **unchanged**: they
assert externally observable behaviour, which makes them an unusually
good regression net for a refactor whose entire claim is that nothing
observable changed.

**Stage 2 -- behaviour becomes protocol state.** `WIN_REQ_HINTS`;
`wm_find_resize_zone()` and friends read the window rather than
`w->app`; `gui windows --json` starts reporting a client's real
`resizable`. No visible change yet for anything that doesn't opt in.

**Stage 3 -- resize, end to end.** The rubber band, `WIN_EV_RESIZE`
actually sent, `WIN_REQ_RESIZE` and the server-side realloc/remap,
`uapp`'s default handling. Port `notepad` and `terminal` and let them
declare themselves resizable. **The acceptance test for the whole design
is that Stage 3 touches zero lines in `calculator.c`, `winclient.c`,
`uiclient.c` or `gfxdemo.c`** and they keep passing.

**Stage 4 -- optional.** `WIN_EV_FOCUS`, `WIN_REQ_MOVE`, and a second
window per process if M41 wants it.

## Testing

- `tools/gui_regress.py` after every stage. Six of its seven tools drive
  the clients being ported, and all of them must pass without edits --
  an edit to a test tool during Stage 1 is a signal that behaviour
  changed, not that the tool was wrong.
- `tools/damage_sweep.py` after Stage 2 and 3: both touch what the WM
  draws, and the rubber band is new drawing.
- A new `tools/uapp_test.py` for the resize handshake specifically, with
  the assertions this repo has learned to demand: the window's reported
  content size and the client's own painted extent must agree **after**
  the ack (a client that resized its buffer but not its drawing passes
  any check that looks at only one of them); a refused resize must leave
  the window pixel-identical; and a positive control -- break the ack
  path deliberately and confirm the test goes red -- before the clean run
  is believed.
- `make test` throughout: `win_server.c`'s realloc path wants KTESTs
  with `fault_inject.h` failing the frame allocation, since "the server
  refuses" is a path no interactive test will hit by accident.

## Out of scope

- **A layout engine.** Apps keep computing
  `MARGIN + col * (BTN_W + GAP)`. That is roadmap M20 and a separate
  argument.
- **Retained-mode widgets**, for the reasons above.
- **Client-side decorations** (`UAPP_NO_TITLE_BAR` is reserved, not
  planned). The WM owns chrome; a client drawing its own title bar is a
  much larger conversation about who owns the desktop's look.
- **Clipboard and drag-and-drop** -- M23, and they want a protocol of
  their own rather than another `win_request` opcode.
- **Force-closing an unresponsive client.** Already listed under M41 and
  unaffected by any of this: `on_close` returning 0 is a client
  *refusing* politely, which is not the same problem as one that never
  answers.
