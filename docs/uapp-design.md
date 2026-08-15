# uapp design -- an app model for ring-3 GUI clients

**Status: DESIGN, not implemented.** Written the way
`docs/tfs3-design.md` was: decide the shape and the arguments first,
build it in named stages afterwards. Nothing in `userland/` implements
any of this yet.

**Naming, settled since this was written:** the protocol is **TWP**
(Toy Window Protocol), the server implementing it is **TWS**, and the
client toolkit this document designs for is **Toykit** -- see
`docs/decisions.md`. This file predates those names and mostly says
"the protocol" and "the toolkit"; read them as TWP and Toykit.

**The one-sentence version:** ring-3 clients should describe themselves
and supply callbacks, the way kernel-space apps already do through
`struct gui_app` -- instead of each one hand-writing the window
handshake, the event loop, and the arithmetic that places every widget.

Decisions settled deliberately rather than defaulted, each with its own
section below:

- The loop lives in the library, with an escape hatch (Shapes animates,
  Terminal blocks inside a command).
- Drawing stays **immediate-mode**, but **layout does not** -- rects are
  computed by a layout pass, not by each app. These are separable, and
  conflating them is what made the first draft of this document reject
  layout for the wrong reason.
- Window *behaviour* becomes protocol state instead of a field on a
  kernel-side struct.
- Resize is a **configure/ack handshake**, not a size the server
  imposes.
- **No GUI string type.** UTF-8 is byte-compatible with ASCII by
  design; what breaks later is text *arithmetic*, not `const char *`.
- The widget set gets **normalised before** any of this is built on it,
  because it is currently three inconsistent generations and only one of
  them can participate.

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

## What "easier" actually means

Removing the handshake and the loop is necessary and not sufficient. A
first draft of this design stopped there, and the resulting "Hello
button, World label" example still needed a `hello_size()` computing
`ugfx_text_width("Hello") + 24` and `ugfx_char_h() + 12`, *the same two
expressions again* in `on_open`, and a draw call reading

```c
ugfx_draw_string(s, MARGIN, MARGIN + g_buttons[0].h + GAP,
                  g_text, UTHEME_TEXT, UTHEME_PANEL_BG);
```

-- a y coordinate derived by hand from another widget's height, plus a
foreground and background the app has no real opinion about. That is
still the app doing the toolkit's job. Three distinct causes:

1. **Widgets don't know their own natural size**, so every app computes
   it, with the padding constants copied in.
2. **There is no layout pass**, so every coordinate is arithmetic over
   its neighbours' geometry.
3. **Window size is stated rather than derived**, so a third copy of the
   same numbers appears in `default_size`/`on_size`.

Each is addressed below. The target for that example is fifteen lines
with no coordinates in it at all -- see "What a small app looks like".

## Shape

A client describes itself and supplies callbacks; `uapp_run()` owns the
loop. Everything a window can do is a function on the app handle rather
than a message the app assembles.

## API sketch (`userland/uapp.h`)

Illustrative, not final -- the point is the shape. Types come from
`ugfx.h`, `uui.h` and `win_proto.h`; nothing here is new kernel surface.

```c
struct uapp;   // opaque; one process, one window (for now)

// --- behaviour flags -------------------------------------------------
#define UAPP_FIXED_SIZE   0x01  // no resize grip (the default is resizable)
#define UAPP_NO_TITLE_BAR 0x02  // reserved -- see "Out of scope"
#define UAPP_CENTER       0x04  // let the server place it centred

struct uapp_desc {
    const char *title;

    // --- content: pick ONE of these three ---------------------------
    // A layout the library places, sizes the window from, and draws by
    // default. The ordinary case.
    struct uui_layout *layout;
    // Or: a button group the library routes the mouse to, for an app
    // that draws its own content but wants standard controls.
    struct uui_button_group *buttons;
    // Or: nothing, and the app draws and hit-tests everything itself.

    // Only needed to override what the layout implies.
    int w, h;
    void (*on_size)(int *w, int *h);
    int min_w, min_h;

    unsigned flags;        // UAPP_*
    void *state;           // opaque, handed back via uapp_state()

    // --- callbacks. ALL optional. A NULL callback is not an error and
    // not a special case: the library has a defined default for every
    // event, so an app only writes the ones it cares about. This is the
    // property that lets the WM grow without touching apps.
    void (*on_open)   (struct uapp *a);
    void (*on_action) (struct uapp *a, int code);  // a control committed
    void (*on_draw)   (struct uapp *a, struct uapp_draw *d);
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

// --- drawing, for an app with its own content --------------------------
// The context carries the surface AND the theme defaults, which is what
// takes ugfx_draw_string()'s six arguments down to three. The styled
// forms stay for the cases that genuinely differ.
struct ugfx_surface *uapp_surface(struct uapp_draw *d);
void uapp_text(struct uapp_draw *d, int x, int y, const char *s);
void uapp_text_styled(struct uapp_draw *d, int x, int y, const char *s,
                       uint32_t fg, uint32_t bg);
void uapp_fill(struct uapp_draw *d, int x, int y, int w, int h, uint32_t c);

// --- escape hatch, for an app that owns its own loop ------------------
int  uapp_open(struct uapp **out, const struct uapp_desc *desc);
int  uapp_pump(struct uapp *a, int block);  // dispatch pending events;
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

### `on_action` is where the commit rule lives

`docs/gui-guidelines.md`'s press-then-commit-on-release rule is
currently re-implemented in every app, as three mouse callbacks that
forward to `uui_button_group_press/_hover/_release` and then check the
return. With `.layout` or `.buttons` set, the library does that routing
and calls `on_action(a, code)` when a control actually commits. The rule
lives in one place instead of once per app, which is the same argument
`ui_textview.h` makes about scrolling having been copy-pasted into three
apps until the third copy shipped a scrollbar that drew and did nothing.

The raw `on_press`/`on_motion`/`on_release` callbacks stay for apps
doing their own hit-testing -- Shapes' canvas, Notepad's text area.

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

## Layout: natural sizes, one pass, two-pass draw

Three pieces, in dependency order.

**Natural size.** Every widget type answers "how big do you want to be"
-- a button is `text_width(label) + UUI_BTN_PAD_X*2` by
`char_h + UUI_BTN_PAD_Y*2`, with the padding a `uui` constant rather
than a number copied into each app.

This is not a new concept, it is an existing one nobody named. It is
already implemented three times, in three incompatible spellings:

| Existing | Widget |
|---|---|
| `ui_radio_list_size(list, *w, *h)` | radio list |
| `uui_checkbox_width(size, label)` | checkbox (width only) |
| `ui_listbox_height_for_rows(lb, rows)` | listbox (height only, parameterised) |

Three real callers, no shared signature. Formalising it is consolidation,
not speculation -- the same finding that produced `kernel/lib/`.

**One layout pass.** Widgets are declared in order and a
column/row/grid container places them, with margins and gaps derived
from `char_h` so the result reflows at any font size (`CLAUDE.md`'s
font-derived-layout rule, which app-computed coordinates satisfy only by
each app remembering to). The pass runs at open and on resize, and
writes through the `set_geometry()` that generation-A widgets already
have -- and whose comment already says geometry is font-dependent and
gets recomputed. Nothing about existing widgets needs to change for the
pass to have somewhere to write.

**Window size derived from it.** Once natural sizes exist, the initial
window size is the layout's natural size plus margins, so `on_size`
disappears for anything that doesn't want a specific size. This also
means a window is correctly sized at every font size for free, rather
than by each app remembering to derive it.

**Drawing is still immediate-mode, and stays two passes.** The default
`on_draw` walks the layout and draws each widget; nothing retains damage
state, and the compositor one level up keeps doing that job. But it
cannot be a single loop: `ui_dropdown_draw_popup()` must be called
**after every other widget**, because drawing is immediate-mode and
z-order is call order. So the widget descriptor carries an `overlay`
flag and the default draw is a content pass followed by an overlay pass.
Missing this would reintroduce exactly the bug `ui_dropdown.h` warns
about.

**Layout is not retained mode.** The first draft of this document
rejected layout by citing "immediate-mode stays" and "a layout engine is
M20", which conflated two separable things. Computing rects once and
drawing them immediately is not retained mode; it is what every app here
already does by hand. The M20 relationship is real but narrower than it
looked: this takes the first bite of it on the ring-3 side only, and
does not commit the kernel-space GUI to anything.

## Text, and Unicode later

**Decision: no GUI string type.** Widgets keep taking `const char *`.

UTF-8 is byte-compatible with ASCII by design -- that is the entire
point of the encoding -- so the parameter type survives the migration
untouched. What does not survive is **arithmetic on the bytes**, and a
wrapper struct does not fix arithmetic. Concretely, the two things that
will actually hurt at `docs/roadmap.md` M37 are both already in the
tree, and neither is a string type:

- **`userland/ugfx.c`'s `int idx = (int)c - WIN_FONT_FIRST_CHAR;`** --
  glyph lookup is ASCII-contiguous by construction, and
  `abi/win_proto.h` bakes that into the protocol ("glyph 0 is ASCII 32
  and they run contiguously from there"). This needs a real glyph map,
  and it is a protocol change as well as a code change.
- **`struct scrollback_cell { char ch; uint8_t fg; }`** -- 8192 fixed
  one-byte cells. The terminal's *storage* assumes one byte is one
  character and one column. This is the expensive one.

What generalises instead is a small set of **text chokepoints**, which
already half-exist: `ugfx_text_width()` is the single measuring
function and its comment already anticipates a proportional face, and
`utext.c` already routes measure / draw / index-at-point through one
shared wrap accounting *specifically* so the three cannot disagree.
Finish that pattern:

| Chokepoint | Replaces, in widget code |
|---|---|
| `width(s)` | `len * char_w` |
| `index_at_x(s, x)` | scanning with `i * char_w` |
| `next(s, i)` / `prev(s, i)` | `i + 1` / `i - 1` for a cursor |
| `truncate_to_width(s, w)` | `s + n`, and hand-rolled ellipsis logic |

with the standing rule that **no widget does character arithmetic
itself**. Then UTF-8 becomes a change inside four functions plus the
font map, rather than a change in every widget.

The alternative considered and rejected: `typedef const char *uui_text;`
in widget signatures now, so they don't change at M37. It costs almost
nothing, but it buys almost nothing -- no compile-time safety, no
behaviour -- while making every example noisier. Per `CLAUDE.md`'s bar,
the type gets added when a caller genuinely needs it; the chokepoint
functions have several callers today.

## The widget audit: three generations

Asked directly -- do the *other* UI components work under this model?
Today, no. The set is three inconsistent generations, and the split
falls exactly where layout needs it.

| Generation | Widgets | Shape | Fits layout? |
|---|---|---|---|
| **A. Retained geometry** | `ui_button`, `ui_dropdown`, `ui_listbox`, `ui_textbox`, `ui_textview`; `uui_listbox`, `uui_dropdown` | `_init(x,y,w,h)`, `_set_geometry()`, `_draw(origin)`, `_hit`/`_hover`/`_press` | **Yes.** Only needs `natural_size()`. |
| **B. State, no stored rect** | `ui_radio_list`, `uui_radio_list`, `uui_field`, `text_field` | struct exists, but `_draw(w, x, y, …)`/`_hit(w, x, y, px, py)` take coordinates every call | **Not yet.** Needs `set_geometry()` -- small and mechanical. |
| **C. Free functions, no state** | `widget_checkbox_*`, `uui_checkbox_*`, `widget_scrollbar_*`, `uui_scrollbar_*`, `widget_button()` | no struct at all; the caller passes everything, every call | **No.** Nothing to lay out, nowhere to hold hover/pressed, cannot join `ui_focus`. Needs a struct. |

A checkbox cannot be a layout child today because a checkbox is not an
object. That is the finding that adds a stage to the plan: **normalise
first, build on it second.** Skipping it produces a layout layer that
works for buttons and quietly doesn't for half the toolkit -- which is
worse than no layout layer, because the gap is invisible until an app
hits it.

One more constraint the audit turned up: **`ui_focus_ops` is already a
per-widget vtable.** The layout descriptor should extend that table
rather than introduce a second one, or a widget ends up declaring itself
twice in two places that can drift -- the same failure `fs_ops.caps` and
`display_driver` guard against by refusing a driver whose two statements
disagree.

## Protocol additions

`uapp` is a **client-side library over the existing messages**. It adds
no syscalls, and must not: the whole bet in `abi/win_proto.h` is that
the server can move to ring 3 as a transport swap
(`docs/roadmap.md` M41). A convenience layer that reached around the
protocol would quietly cash that in.

Stages 1a-1c need no protocol change at all. Stages 2-3 add three
messages and start sending one event that already exists:

| Message | Fields | Purpose |
|---|---|---|
| `WIN_REQ_HINTS` (6) | `a` = behaviour flags, `b`/`c` = min w/h | The client states how its window should behave. Sent once after create. |
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
4. `uapp` rebuilds its surface (stride changes with width), **re-runs
   the layout pass**, calls `on_resize` if the app supplied one, then
   `on_draw`, then presents. The WM adopts the new content size when the
   present arrives.
5. If the server refuses (out of frames, over `WIN_CLIENT_MAX_W/H`), the
   client keeps the size it had and the window does not change. A
   refusal is a normal outcome, not an error path.

**An app that supplies no `on_resize` still resizes correctly, and with
layout its widgets move to the right places** -- the library
reallocates, re-lays-out, repaints. That is the whole argument for this
design in one sentence.

This is also the same shape as the close button, which
`docs/decisions.md` already records as "a handshake, not a seizure".
Two operations, one precedent.

## Decisions settled deliberately

**Drawing stays immediate-mode; layout does not.** See "Layout" above
for why these are separable and why the first draft got it wrong.

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

**`uapp` is ring-3 only; `gui_app` stays as it is.** Confirmed with the
maintainer. No attempt to unify the two toolkits. `apps/ui/` and
`userland/uui.c` are already separate ports and M41 has the
kernel-space side retiring eventually; the right move is to make the
ring-3 side the good one and let porting flow in that direction, not to
build a shared abstraction over a layer that is scheduled to disappear.
Normalisation (stage 1a) is worth doing on both sides anyway, since
`uidemo` is the test app for the kernel-space set and is how the
behaviour gets verified at all.

**`uapp` composes with `uui`, it does not absorb it.** A client that
wants no app framework can still use the widgets directly, and a client
that wants no widgets can still use `uapp`. `.layout` and `.buttons` are
opt-in fields, not a required content model.

## What a small app looks like

A button labelled "Hello" and "World" underneath, complete:

```c
#include "uapp.h"

enum { BTN_HELLO = 1 };
static const char *g_text = "World";

static struct uui_widget WIDGETS[] = {
    UUI_BUTTON("Hello", BTN_HELLO),
    UUI_LABEL(&g_text),
};

static void on_action(struct uapp *a, int code) {
    if (code == BTN_HELLO) { g_text = "World!"; uapp_redraw(a); }
}

int main(void) {
    struct uapp_desc desc = {
        .title     = "Hello",
        .layout    = UAPP_COLUMN(WIDGETS),
        .on_action = on_action,
    };
    return uapp_run(&desc);
}
```

Plus one Makefile line (`EXTRA_OBJS_hello = uui uapp $(UGFX_OBJS)`) and
one `SEED_PROGRAMS` entry. No coordinates, no padding constants, no
colours, no `on_size`, no `on_draw`, no handshake, no event loop. The
same program written against today's API is about 120 lines, roughly 60
of them machinery.

`UUI_LABEL(&g_text)` binds to a pointer rather than taking a setter, so
changing the text is an assignment plus `uapp_redraw()`. **Open
question, to settle before stage 1c:** pointer-binding for text vs.
opaque widgets with `uui_label_set_text()`. Pointer-binding is what makes
the example fifteen lines; setters are more conventional and survive a
widget wanting to cache measured metrics. Current lean: pointer-binding
for plain text, setters for anything with internal state -- and note
that `utext`/`ui_textview` deliberately cache nothing, which weakens the
main argument for setters.

## What each client becomes

Estimates, listed so they can be checked against reality afterwards
rather than quietly forgotten.

| Client | Today | After | What goes |
|---|---:|---:|---|
| `winclient.c` | 141 | ~45 | the whole handshake and loop |
| `uiclient.c` | 189 | ~85 | ditto, plus its own button placement |
| `calculator.c` | 282 | ~180 | 3 private syscall helpers, the create/title dance, the 35-line switch, and `button_rect()`/`metrics_init()` once the grid is a layout |
| `gfxdemo.c` | 298 | ~230 | the poll/yield loop becomes `on_tick`; the canvas keeps its own drawing |
| `terminal.c` | 191 | ~160 | handshake only -- it keeps its own loop via the hatch |
| `notepad.c` | 565 | ~470 | handshake + switch; gains resize for free |

The absolute saving is not the point -- roughly 400 lines. The point is
that the removed lines are the *same* lines in each file, and that the
next client written pays none of it.

## What this buys, and what it doesn't

The whole point is that a change to the WM, the widgets or the drawing
layer stops meaning a change to every app. That is a claim worth stating
precisely, because stated loosely it is false.

**Free -- no app changes:**

| Change | Why |
|---|---|
| New event types (focus, wheel, drag-and-drop, timer, font-changed) | optional callback plus a library default; an app that ignores it still behaves correctly |
| New window behaviours (snap, always-on-top, tiling) | WM-side; apps opt in with a hint or ignore it |
| Chrome -- title bar, close button, shadows | apps never draw chrome (already true) |
| Theme and palette | apps use theme constants and the draw context's defaults |
| Font size | already free via font-derived layout; more so once the window auto-sizes |
| Widget internals -- a listbox gaining keyboard nav, a button gaining a focus ring | behaviour belongs to the widget, the rule `ui_textview.h` already states |
| Resize | non-opt-in apps untouched; opt-in apps get re-layout for free |
| **The transport** -- one-message-per-syscall to a shared-memory ring | apps talk to `uapp`, not to the protocol |
| **The window server moving to ring 3** | same argument |

The last two are the biggest practical win and the easiest to overlook:
both are remaining M41 items, and today either would touch every
client's event loop.

**Still touches apps, correctly:**

- An app wanting to *use* a new capability. New feature, new app code.
- Apps that draw their own content (Shapes' canvas, Notepad's text
  area). A change to text semantics reaches them *if* they do character
  arithmetic -- which is what the chokepoint rule is for, and it holds
  only as far as the discipline does.
- Apps carrying hardcoded values. `calculator.c` hardcodes
  `ugfx_rgb(210, 218, 235)` for operator buttons; `winclient.c`
  hardcodes `WIN_W`/`WIN_H`. An abstraction only protects code written
  against it.

**Never free, correctly:** anything that changes what the app *means*.

### Five rules, and making one of them structural

Each rule removes one reason an app would need editing: an app never
touches the protocol, never computes chrome or geometry, never does
character arithmetic, never picks interaction colours, and never owns
the event loop.

Rules erode. Two things make these hold rather than be hoped for:

1. **Stage 3's acceptance test measures the property directly** -- zero
   lines changed in the clients that don't opt into resize.
2. **Include-path enforcement for the most important rule.** Once the
   restructure separates `userland/gui/` from `userland/ui/`, build the
   GUI apps *without* `kernel/include/abi` on their include path, so
   `win_proto.h` and `syscall_abi.h` are reachable only from
   `userland/ui/` (i.e. from `uapp` and libsys) and from
   `userland/tests/`, which pokes the raw ABI on purpose. An app
   reaching for the protocol then fails to compile instead of being
   caught in review -- exactly the mechanism `kernel/include/kernel/`
   already uses to keep `apps/` out of kernel internals, and the
   reason that boundary has held where a documented convention would
   not have. Cost is one `-I` line, and it is only expressible *after*
   stage 0 puts apps and toolkit in different directories.

### The precedent that says this is achievable here

The experiment has already been run once in this repo: dropping the
default font from 18 to 14 was a **one-line change** and all 82 GUI
regression checks passed unchanged, because layout is font-derived. The
counter-example is in the same changelog entry -- the hardcoded pixel
constants in `tools/gui_flow.py` did not reflow and needed re-measuring
for the third time. The property holds exactly as far as nothing
hardcodes what the layer above should own, which is why stage 1a and the
include-path rule matter more than they look.

## File structure

Asked separately, and the answer is yes -- `userland/` needs the
treatment `kernel/` and `apps/` already got, and it needs it *before*
this design lands rather than after.

**Today it is flat: 44 source files in one directory**, mixing four
unrelated things -- the C runtime (`crt0.asm`, `sys.c`, `stack_chk.c`,
`link.ld`), the toolkit (`ugfx`, `uui`, `uwidgets`, `utext`, `utheme`),
six real GUI apps plus the shell, and about twenty single-mechanism test
diagnostics (`nx_test.c`, `fpu_race.c`, `write_bad_test.c`, ...). The
diagnostics outnumber everything else and bury it.

Three specific problems, each of which this design makes worse:

- **The toolkit split is arbitrary.** `uui.c` is 125 lines and holds
  buttons; `uwidgets.c` is 650 and holds scrollbar, field, checkbox,
  radio list, listbox and dropdown. Meanwhile `apps/ui/` -- the same
  widgets, on the kernel side -- is one file per widget. Stage 1a adds
  a `natural_size()` and a `set_geometry()` to *every one of those
  widgets*, which is exactly when a 650-line grab bag stops being
  tolerable. `CLAUDE.md`'s rule applies squarely: split when a file
  mixes more than one real concern, and follow the existing pattern
  rather than inventing one.
- **Source discovery never became recursive here.** The Makefile finds
  kernel and apps sources with `$(shell find kernel apps -name '*.c')`,
  but userland is served by flat pattern rules
  (`$(BUILD)/userland/%.o: userland/%.c`). So `userland/` is the one
  place left where adding a directory means editing the Makefile --
  the tax the recursive change removed everywhere else.
- **Build artifacts live in the source tree.** `userland/*.elf` are
  written next to their `.c` files and hidden with a `.gitignore` line,
  while every other object in the project lands under `build/`.

### Proposed layout

```
userland/
  rt/      crt0.asm, sys.c/h, stack_chk.c, link.ld
  ui/      ugfx, utheme, utext, uapp, and one file per widget
           (uui_button, uui_listbox, uui_dropdown, uui_checkbox,
            uui_scrollbar, uui_field, uui_radio_list, uui_layout)
  gui/     calculator, notepad, terminal, gfxdemo, uiclient, winclient
  bin/     ls, lspci, lscpu, hello, ush
  tests/   exit_test, nx_test, fpu_test, write_bad_test, ... (~20)
```

with ELFs built to `build/userland/**` rather than into the source
directory.

**`ui/` deliberately mirrors `apps/ui/`**, one file per widget, so the
kernel-side and ring-3 versions of a widget are findable at the same
relative path -- which matters while both exist and porting flows one
way.

**`gui/` is separate from `bin/`** because a windowed client and a
command-line program are different kinds of thing to write, test and
read, even though both seed to `/bin`.

**The directories imply the seed destination.**
`docs/filesystem-layout.md` already makes `/bin` versus `/tests` a real,
enforced distinction (`tools/check_layout.py` fails CI in both
directions), and the Makefile currently restates that split by hand as
`SEED_PROGRAMS` and `SEED_TESTS`. With the tree above the rule is two
lines -- `gui/` and `bin/` to `/bin`, `tests/` to `/tests` -- plus the
handful of genuine renames (`echo.c` seeds as `echo_test`), instead of
two per-file lists to keep in step. The drift that prevents is real:
every ring-3 client landed in `/tests` originally because the first one
did, and nobody moved them until the Start menu forced it.

### Why not a top-level `gui/` tree

Asked directly: should the whole GUI -- the WM, the widgets, the apps,
both rings -- live under one top-level directory instead of being split
across `apps/` and `userland/`? No, for three reasons.

**It would cross-cut the boundary the build enforces.** `apps/` and
`userland/` are not two flavours of the same thing: they compile with
different code models (`-mcmodel=kernel` against `-mcmodel=large`), link
at different addresses, and see different include paths --
`kernel/include/` is deliberately off `apps/`'s path so reaching for an
internal header is a compile error rather than a review catch (see
`kernel/include/README.md`). A directory holding both rings puts files
with incompatible compile flags side by side, and the enforcement
quietly stops meaning anything.

**`apps/` is a shrinking tree.** M41 retires the kernel-space desktop;
`userland/` is where GUI code lives permanently. Restructuring around
the tree that is scheduled to disappear is investment in the wrong
place.

**`apps/` is really two things, and only one of them is the GUI.** It
holds the desktop (`wm/`, `ui/`, `gui.c`, `gui_apps.c`, `theme.h`, and
the GUI apps) *and* the physical shell (`shell*.c`, `completion.c`,
`editor.c`, `apps.c`) -- which is ring-0 by nature, is what `Esc` drops
back to, and does not retire with the GUI. So the end state is not "one
gui tree" but:

```
userland/   ... rt/ ui/ gui/ bin/ tests/, plus wm/ once M41
                moves the window server out of the kernel
apps/       ... empties out; what remains is the physical shell,
                and should be named shell/ at that point
```

`userland/` *becomes* the whole userland, GUI included. That reaches the
same place a top-level `gui/` was reaching for -- the GUI in one
coherent home -- by following the privilege boundary instead of cutting
across it, in increments, with no third tree. Renaming `apps/` is
explicitly **not** part of this work: it is churn on a tree whose
contents are about to move anyway, and it should happen once, at the
end, when what is left is genuinely just the shell.

### Do it first, and prove it changed nothing

This belongs **before** stage 1a. Moving files while adding functions to
them makes both halves harder to review, and the move has an unusually
strong correctness check available: a pure `git mv` plus Makefile
rework should leave every userland ELF identical, the same check that
verified this session's `EXTRA_OBJS` change. A move that changes a
binary is a move that changed something.

**One correction, from running it:** "byte-for-byte identical" is the
wrong bar, and it fails for a reason that is not a bug. `USERLAND_CFLAGS`
carries `-g`, so every ELF embeds its source path in DWARF -- and the
whole point of the move is that those paths change. All 29 differed on
the first check. The right bar, and the one the move actually met, is
**identical after `--strip-debug`**: every loadable byte the same, with
the debug info correctly naming the new locations. Worth recording
because the naive check looks like a catastrophic failure (29 of 29
binaries changed) when nothing executable moved at all.

`apps/` needs nothing. `apps/wm/` and `apps/ui/` are already split by
concern, and M41 retires the kernel-space app set eventually -- so the
right amount of restructuring there is none.

## Staging

Each stage is a commit that builds, tests and stands on its own -- the
A-E pattern TFS3 used.

**Stage 0 -- restructure `userland/`.** Two commits. First the move:
`git mv` into `rt/`/`ui/`/`lib/`/`gui/`/`bin/`/`tests/`, path-qualified
includes off one `-Iuserland`, ELFs to `build/`, and `SEED_*` derived
from the directories. Then the split of `uwidgets.c` one-file-per-widget
to match `apps/ui/`. No behaviour change in either; the acceptance test
is ELFs identical after `--strip-debug` (see above).
**Status: DONE.** The move landed first (29/29 ELFs identical after
`--strip-debug`), then `libuapp.a` with `--gc-sections` -- brought
forward from its deferral because the split was about to turn each
app's object list from three entries into eight, which is exactly the
second real caller the deferral was waiting for. A program now names no
objects at all, and the 29 binaries lost 171 KB between them. Then the
split itself, verified as pure motion by an identical
`nm --print-size` dump across all 29. All 82 GUI regression checks pass
throughout, with no test tool edited. **Stage 1a is next.**

**Stage 1a -- normalise the widget set.** One `natural_size()` spelling
replacing the three that exist; `set_geometry()` on generation B;
structs for generation C (`uui_checkbox`, `uui_scrollbar`); the text
chokepoints. No new concepts and no app changes. `tools/uidemo_test.py`
drives every one of these widgets and its 27 checks must pass unchanged
-- which makes it an unusually good net for a refactor whose entire
claim is that nothing observable changed.

**Stage 1b -- the library. DONE.** `userland/ui/uapp.c`/`.h` over
today's five messages: the descriptor, the loop, `on_action`, the
escape hatch. Ported `winclient` (141 -> 85 lines), `uiclient`
(189 -> 148) and `gfxdemo` -- the third added beyond the plan because
it is what exercises `on_tick` and the button routing, and shipping
those unexercised would have repeated the mistake this document keeps
warning about. All test tools passed unchanged. The draw-context
helpers were cut for having no callers; `on_resize`/`on_focus`/hints
wait for stages 2-3.

**Stage 1c -- layout. DONE.** Containers (column/row/grid), nesting, a
custom item for an app's own drawing, auto-sized windows, and the
library clearing + drawing the layout before `on_draw`. Calculator was
the proof and passed it: 282 -> 230 lines, every coordinate gone
(`button_rect`, `metrics_init`, `content_w`, `content_h` and their four
spacing constants), its `on_draw` gone entirely, and
`calculator_client_test.py` passing unedited. `uui_focus_ops` was
folded into one `uui_widget_ops`. The two-pass overlay draw is NOT
built -- no layout contains a dropdown yet, and it is internal to
`uui_layout_draw()` when one does.

**Stage 2 -- behaviour becomes a property of the window. DONE.**
`struct window` carries `resizable`, and the seven readers of
`w->app->resizable` ask the window. No behaviour change; a client's
stays 0 because nothing can resize one yet.

`WIN_REQ_HINTS` was planned for this stage and deliberately moved to
stage 3. A hint saying "resizable" with no handshake behind it either
does nothing -- dead API, which this document has argued against three
times -- or enables a grip that drags the chrome while the buffer stays
put, which is exactly the wrong behaviour described under "Resize is a
configure/ack handshake". The hint and the thing that honours it should
land together.

**Stage 3 -- resize, end to end. DONE.** `WIN_REQ_HINTS` and
`WIN_REQ_RESIZE` in TWP, the server-side realloc/remap at the same
virtual address, the rubber band, `WIN_EV_RESIZE` finally sent, and
`uapp` answering it by default. **The acceptance test passed**: zero
lines changed in `calculator.c`, `uiclient.c` or `gfxdemo.c`, all 83
existing checks green. `winclient` opted in afterwards with one field
and no resize code. `tools/uapp_test.py` covers the handshake.

Notepad and Terminal are NOT yet ported -- they are still hand-rolled
TWP clients, so they cannot opt in until they move onto uapp. That is
the next piece of work rather than part of this stage.

**Stage 4 -- optional.** `WIN_EV_FOCUS`, `WIN_REQ_MOVE`, and a second
window per process if M41 wants it.

## Testing

- `tools/gui_regress.py` after every stage. Six of its seven tools drive
  the clients being ported, and all of them must pass without edits --
  an edit to a test tool during stages 1a-1c is a signal that behaviour
  changed, not that the tool was wrong.
- `tools/uidemo_test.py` specifically gates stage 1a, since it is the
  only thing that exercises the generation B and C widgets at all.
- `tools/damage_sweep.py` after stages 2 and 3: both touch what the WM
  draws, and the rubber band is new drawing.
- A new `tools/uapp_test.py` for the resize handshake, with the
  assertions this repo has learned to demand: the window's reported
  content size and the client's own painted extent must agree **after**
  the ack (a client that resized its buffer but not its drawing passes
  any check that looks at only one of them); a refused resize must leave
  the window pixel-identical; and a positive control -- break the ack
  path deliberately and confirm the test goes red -- before a clean run
  is believed.
- `make test` throughout: `win_server.c`'s realloc path wants KTESTs
  with `fault_inject.h` failing the frame allocation, since "the server
  refuses" is a path no interactive test will hit by accident.

## Out of scope

- **A general layout engine for the kernel-space GUI.** Stage 1c is the
  ring-3 toolkit only. M20 remains its own item; this informs it rather
  than replacing it.
- **Retained-mode widgets.** Layout is computed once and drawn
  immediately; nothing retains damage state below the compositor.
- **A GUI string type.** See "Text, and Unicode later" -- and note that
  M37's real cost is the glyph map and `struct scrollback_cell`, neither
  of which this design changes or blocks.
- **Client-side decorations** (`UAPP_NO_TITLE_BAR` is reserved, not
  planned). The WM owns chrome; a client drawing its own title bar is a
  much larger conversation about who owns the desktop's look.
- **Clipboard and drag-and-drop** -- M23, and they want a protocol of
  their own rather than another `win_request` opcode.
- **Force-closing an unresponsive client.** Already listed under M41 and
  unaffected by any of this: `on_close` returning 0 is a client
  *refusing* politely, which is not the same problem as one that never
  answers.

## Revision history

- **First draft.** The library, the callbacks, the protocol additions,
  resize as a handshake. Explicitly rejected a layout engine and said
  "`uapp` does not absorb `uui`".
- **This revision**, after review (and its own follow-up, which added
  the "File structure" section and stage 0): the rejection of layout was wrong and
  is reversed -- it conflated layout with retained-mode rendering, and
  the resulting example still had the app computing button padding
  twice and deriving a label's y coordinate by hand. Adds natural sizes,
  the layout pass, the draw context and `on_action`; adds the widget
  audit that found the three generations and made stage 1a necessary;
  and settles the GUI-string question as "no type, formalise the
  chokepoints". The composition rule survives in a narrower form:
  `uapp` composes with `uui` rather than absorbing it.
