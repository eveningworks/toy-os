# GUI guidelines

How toy-os's GUI is supposed to look and behave. The goal in one line:
**minimalist but modern -- flat, quiet, and never ambiguous about
whether it heard you.**

This is a working document for writing apps and widgets, not a style
manifesto. Where a rule exists because something went wrong, that's
said, because the reason is the useful part.

## Behaviour belongs to the component, not to the app

**A widget owns how it behaves. An app owns whether and how it looks.**
Unless there's a concrete reason otherwise, new interaction logic goes
in `apps/ui/`, and the app configures it and forwards events -- it does
not reimplement it.

Concretely, when you add a control:

- Input handling, hit-testing, geometry and state transitions live in
  the widget. The app's callback should read like
  `if (ui_thing_click(&t, cx, cy)) window_invalidate(win);`
- What the app gets to decide is exposed as **fields or setters**:
  visibility policy, colours, step sizes, and who owns an ambiguous
  event. Never by copying the widget's internals into the app.
- Colours come *from* the caller, so `apps/ui/` stays theme-agnostic
  (see `ui_primitives.c`). "Configurable" does not mean the widget
  reaches into `theme.h`.
- If two apps need slightly different behaviour, that's a **flag on the
  widget**, not a second copy. `ui_textview`'s `UI_TEXTVIEW_BODY_APP`
  exists exactly so Notepad can keep click-to-position while UI Demo
  pans.

The worked example is `ui_textview` (`apps/ui/ui_textview.h`). Scrolling
used to be every app's own job: Notepad, Terminal and UI Demo each
carried the same ~20 lines of overflow check, strip reservation, track
paging, thumb-grab bookkeeping and wheel arithmetic. Three copies meant
three chances to get it wrong, and the newest of them shipped a
scrollbar that drew perfectly and did nothing at all, because it had
been given the layout half and not the input half. After the migration
those three apps call zero `widget_scrollbar_*` functions between them.

**The reason this is a rule and not a preference:** duplicated
behaviour doesn't fail loudly. It rots one copy at a time, and the copy
that rots is the one nobody is looking at. A control that owns its own
behaviour cannot be half-implemented by its next caller.

When NOT to follow it -- state the reason in a comment if you don't:
behaviour genuinely specific to one app (Calculator's key-to-operator
mapping isn't a widget), a single caller where the abstraction would be
invented rather than extracted (this codebase's standing rule is that a
mechanism arrives with its second real caller), or a case where hiding
the logic would make the app's own code harder to follow than the
duplication.

## The three interaction states

Every clickable control has them, and `enum ui_state`
(`apps/ui/ui_primitives.h`) is this vocabulary in code so the two can't
drift apart.

| State | When | Looks like |
|---|---|---|
| `UI_STATE_REST` | Idle | The control's own colours, no decoration |
| `UI_STATE_HOVER` | Cursor over it, nothing held | Whole control washed one step |
| `UI_STATE_PRESSED` | Held **and** cursor still over it | Washed two steps, content nudged 1px down-right |
| `UI_STATE_DISABLED` | Present but inert | Muted toward the background |

`ui_state_bg(base, state)` produces the wash from the control's *own*
colour. Don't hand-pick tints: the title bar used to carry a
specially-chosen lighter red for its close button purely because a
caller had no way to shift an arbitrary packed colour, and that whole
constant disappeared when this function arrived.

**The wash direction follows the control's brightness, not a fixed
"lighter on hover" rule.** The first version always lightened, which is
the textbook description and wrong here: this theme's window background
is already 235/255, so hover moved the pixels by *two* and was invisible.
`gfx_luminance()` decides the direction, so a light control darkens and
a dark one lightens. That bug was found by reading pixel values off a
screenshot, not by looking at one -- see "Verifying" below.

**Flat, not bevelled.** No inset borders, no 3D edges. Pressed is a
darker fill plus a 1px nudge; the nudge is what makes it feel physical,
and the fill alone reads as a colour change rather than a press.

## Press, then commit on release

**A control must not act until the button is released over it.** Press
it, drag away, release: nothing happens. This is how every real button
behaves, and it's the user's last chance to change their mind.

Concretely:

1. `on_press` arms the control and records which one. It's called
   **every tick while held**, with live coordinates, so it also tracks
   whether the cursor is still over the armed control.
2. `on_release` commits -- but only if the cursor is still on the armed
   control.
3. Dragging off an armed control drops it to **REST, not hover.**
   Something about to be cancelled must not look like it's still being
   interacted with.

**`on_click` is a trap here.** Despite the name, the WM fires it on
button-*down* (`wm_input.c`), so nothing armed by `on_press` exists yet.
A control that commits there fires on press and can never be cancelled.
This was found by building it wrong: the Control Panel's applets used
`on_click`, and the drag-away-and-release test simply opened the applet
anyway. Use `on_click` only for things that genuinely act on contact --
placing a text cursor, focusing a field.

The reference implementation is the title bar
(`wm_update_title_btn_press()`), which has worked this way all along;
`apps/control_panel.c` is the app-side version of the same shape.

For a control in a `ui_button_group`, all of that is already done:
**`ui_button_group_release()` returns the released button's code, or
-1 if the press had been dragged off it**, so committing correctly is
`if (code >= 0) act(code)` in `on_release` and nothing else. Calculator
and Notepad both acted from `on_click` until that return value existed,
and both were uncancellable the whole time -- which is also why there
is deliberately no `ui_button_group_click()` any more (see
`docs/decisions.md`).

## Scrollbars: what a real one does

A scrollbar looks trivial and has more behaviour than any other control
here. This section is the specification, written down because the
implementation has been rebuilt under it twice and each time something
in this list quietly stopped being true. Every point below is how
Windows, KDE/Breeze, macOS and GTK all behave -- they differ on looks,
not on any of this. If a change makes one of these false, the change is
wrong even if it builds and even if the bar still moves.

**1. The thumb must not jump when you grab it.** Pressing anywhere on
the thumb and moving by N pixels moves the thumb by exactly N pixels,
from wherever it was. This means recording HOW FAR DOWN THE THUMB the
press landed, and subtracting it on every motion event
(`grab_offset_in_thumb`). Passing 0 there is the bug this section exists
for: the thumb then leaps so its TOP sits under the cursor, and the
control is usable only by catching its top edge exactly. It shipped that
way in the ring-3 Notepad, and the symptom reads as "the scrollbar is
broken" rather than as an off-by-a-grab-offset, which is why it survived.

**2. Dragging is absolute, not relative to the content.** The thumb
follows the cursor 1:1 in TRACK space; the content position is derived
from the thumb, never the other way round. A drag past either end
clamps, and dragging back returns along the same path -- no accumulated
drift.

**3. The trough pages, the thumb drags, the arrows step.** A press on
the track above the thumb goes back one visible page, below it forward
one page, and a stepper arrow (where the app asks for them) moves one
line. Three zones, three different amounts. A trough click that jumps
straight to the clicked position instead of paging is a different
convention -- macOS offers it as an option, KDE as `scrollbarLeftClick`
-- and this toolkit deliberately pages, so don't "fix" it to jump.

**4. The thumb has a minimum size** (`UUI_SCROLLBAR_MIN_THUMB_H`).
Proportional sizing alone makes the thumb vanish on long content, and a
0px thumb is a control you cannot grab at all.

**5. The hit area and the drawn thumb are the same rectangle.** They
must come from one geometry function -- `sb_geometry()` here, which
draw, hit-test and the drag maths all call. Two derivations is the
classic way a scrollbar ends up drawing in one place and responding in
another, and the copy Notepad used to carry did exactly that.

**6. It must be wide enough to hit with a mouse.** The strip is
FONT-DERIVED (`uui_scrollbar_natural_size()` /
`widget_scrollbar_natural_size()`, `char_w + 6`), which is also this
document's size-everything-from-the-font rule -- but the floor is
ergonomic, not aesthetic. An app that hardcodes a pixel width gets a bar
that is right at exactly one font size and, in Notepad's case, an 8px
strip with a 6px thumb that was genuinely hard to click. **Take the
width from the widget.** Passing your own is supported -- it is a
parameter of every scrollbar call -- but the DEFAULT must come from the
widget or the desktop's bars stop matching each other.

**7. The wheel scrolls the CONTENT, and never needs the bar.** A
control with `UI_SCROLLBAR_NEVER` still scrolls. The bar is an
indicator and a handle, not the mechanism.

**8. It is an indicator even when it cannot scroll.** The track is drawn
whether or not there is a thumb, so you can see the control is
scrollable before you go looking. (The near-invisible modern style needs
hover-to-expand to compensate, and these widgets deliberately keep no
hover state.)

`tools/scrollbar_test.py` asserts points 1, 2, 3 and 6 against the
ring-3 Notepad. Run it after touching either scrollbar port.

## Menus: the one control that opens on press

`uui_menubar` (`userland/ui/uui_menubar.h`) is the exception to the
press-then-commit rule above, and it is an exception every real desktop
makes. **A menu bar title opens its menu on button-DOWN.** Press "File"
and the menu is there immediately; you may then release and click an
item, or keep the button held, slide down and release over the item you
want. Both gestures work, and both are how Windows, KDE, GTK and macOS
behave.

The half of the rule that protects the user is kept: **the item still
commits on RELEASE.** Press "Exit", drag off it, release -- nothing
happens. `tools/menubar_test.py` asserts exactly that, and it is the
only check in that file that can tell the two apart: every other one
presses and releases in the same place, so a menu that acted on press
would pass all of them.

The rest of the behaviour, all of which real toolkits implement
identically:

- **Sliding along the bar with a menu open SWITCHES menus.** That is
  what makes a menu bar browsable in one gesture.
- **A submenu opens on hover**, after a short travel onto its parent
  row, and does not close when the cursor briefly leaves the parent on
  the way to it. Re-opening an already-open submenu on every motion
  event repaints continuously -- same contract as `on_hover`.
- **A click anywhere else DISMISSES, and the menu owns that click.** It
  must not also reach whatever is underneath. This has its own check
  because the obvious one ("the menu closed") stays green when the click
  falls through; what catches it is measuring the caret.
- **Esc closes ONE level**, not the whole chain.
- **Separators and disabled rows are skipped by the arrows** and commit
  nothing when released on.

**Per-item state is asked for, never stored in the menu.** The tree is
`const`; an app sets `item_flags` and the widget queries it per item on
every draw and hit test. There is deliberately no "refresh the menu"
call to forget.

**Where a popup is allowed to go is a parameter.** Windows constrains a
menu against the monitor work area and KDE lets the compositor resolve
an `xdg_popup` positioner; a TWP client can draw only inside its own
window, so the widget resolves the same flip/slide/clamp against a
bounds rectangle the app supplies. Pass it the content rect. See
`docs/decisions.md` for why that divergence is the whole difference and
what changes when TWP grows a popup surface.

**Alt+letter mnemonics do not exist here and should not be added while
Alt is an ESC prefix** (`api/keyboard.h`): Alt-F arrives as ESC then
'f', which cannot be told from the Esc that closes the menu. `KEY_F10`
focuses the bar instead -- Windows' own second binding.

## Closing a window: Esc doesn't, Alt+F4 does

**Esc is app-local and closes nothing.** It cancels a dialog, closes a
menu, clears a selection. It used to quit six apps, which is not what
Esc does on any desktop and which put unsaved work one stray press away
once Esc became the menu-close key too.

**Alt+F4 closes the focused window, and the window manager handles it**
-- it never reaches the app, exactly as on Windows (routed through
`DefWindowProc` to `WM_SYSCOMMAND`/`SC_CLOSE`) and KDE (a KWin global
shortcut). An app that could swallow the shortcut would defeat its
purpose.

**What the app still decides is whether it closes.** All three routes --
the title bar's X, the context menu's Close, and Alt+F4 -- go through
one `wm_request_close()`, which ASKS a ring-3 client with
`WIN_EV_CLOSE`; returning 0 from `uapp_desc.on_close` refuses. Do your
cleanup there and return 1, or refuse and stay open.

Two things this rule is worth stating for. A window operation must not
have one route that asks and another that seizes: the context menu's
Close used to call `close_window()` straight, so right-clicking closed
a client that had explicitly declined. And when you add a fourth route,
route it through the same function rather than repeating the client
check -- that check being repeated is what let the third one drift.

## When an app stops answering

A window that will not close is either **declining** or **wedged**, and
those want opposite responses. Never treat a timeout as evidence of
either: ask the client whether it is alive (`WIN_EV_PING`, answered by
Toykit's loop) and let the answer decide. `docs/decisions.md` has the
full reasoning.

Three rules the WM follows here, worth matching in anything similar:

- **Mark, don't interrupt.** A hung window gets "(Not Responding)" in
  its title bar. The modal only appears once the user has actually asked
  it to close -- a dialog that appears on its own, over whatever they
  were doing, for a window they never touched, is worse than the hang.
- **Offer verbs, not Yes/No.** The buttons are "Force Quit" and "Wait",
  because the user is choosing between two actions rather than answering
  a question. `confirm_dialog_open_labelled()` exists for this.
- **"Wait" must not be permanent.** It re-arms the check rather than
  suppressing it, so an app that was merely slow gets another chance and
  one that is truly stuck can be offered again. Getting this wrong is
  invisible until someone presses Wait twice.

## Status bars: panes, not a string

`uui_statusbar` carries a MESSAGE that stretches and INDICATORS that do
not, which is the shape Win32's `SB_SETPARTS` (a `-1` last part) and
Qt's `addWidget` vs `addPermanentWidget` both arrived at. An indicator
that slides around as the message changes is unreadable at a glance.

Two rules: pane widths are in **characters**, not pixels, so the strip
reflows with the font like everything else here; and the text is the
**app's own buffers**, not copied, so updating the status is assigning a
string and there is no set/get pair to keep in sync.

Real toolkits sink each part behind a bevel. This one does not bevel
anything, so parts are separated by a hairline and the strip by a
hairline along its top.

When testing one, pair the halves: "the indicator changed when the
cursor moved" is satisfied by a bar that repaints everything, and "the
message did not change" is satisfied by a dead indicator. Assert both.

## Keyboard: focus, and what a control owes it

A window's widgets share one `ui_focus` ring (`apps/ui/ui_focus.h`).
Tab and Shift-Tab cycle it, clicking a widget focuses it, and the
focused widget gets every key that isn't Tab. **Route keys by focus, not
by trying each widget in turn** -- the second arrangement works with one
keyboard-taking widget and silently breaks with two, because the first
one tried swallows everything it recognises. That is not hypothetical:
`ui_dropdown` handles arrows even while closed, which left a `ui_listbox`
beside it completely unreachable from the keyboard.

A widget joins the ring by exporting a `ui_focus_ops` table. What it
owes:

- **Draw a focus ring** (`ui_focus_ring_rect()` for the standard one), or
  show focus some other way it can justify -- `ui_textbox` shows a caret
  instead, so its `draw_ring` still draws the ring but its `set_focused`
  is what turns the caret on.
- **Decline focus when it can't use it.** `accepts_focus` returning 0
  makes Tab skip straight over it. A disabled control that can still be
  tabbed to is a dead stop the user has to press Tab twice to escape.
- **Not check focus itself.** By the time `ops->key` is called the
  manager has already decided; a widget re-checking is a second source
  of truth.

Two bindings every focusable control should honour, because every
toolkit has them: **Space and Enter activate**, and arrows navigate
*within* a control (a button group's buttons, a list's rows) rather than
between controls.

**Shift-Tab needs the modifier bits.** Tab is 0x09 with or without
Shift, so `on_key`'s `mods` parameter is the only thing separating them
(see `api/keyboard.h`). Most other bindings should ignore `mods`
entirely: Ctrl and Alt are already folded into the key itself, so
`key == 'a' && (mods & KEY_MOD_CTRL)` is never true -- match 0x01.

## When feedback is required -- and when it isn't

Feedback is required when the user **did something and the result isn't
otherwise visible**:

- Any control that can be pressed shows hover and pressed states.
- An action with no visible outcome must produce one (a message, a
  changed value, a redraw). Silence reads as breakage.
- An action that is refused says so. `ata nodma` returning "refused --
  a transfer is in flight" is the shell equivalent of this rule.

Feedback is **not** wanted when the result is already the feedback:

- Selecting a timezone changes the clock; it doesn't also need a
  confirmation.
- Typing shows the character. Don't flash anything.
- Passive or continuous states (a clock ticking, a progress bar
  advancing) aren't "feedback" and shouldn't be decorated as if they
  were.

The failure mode to avoid is a UI that congratulates itself. If a
reasonable person would say "yes, obviously, I just did that", the
feedback is noise.

## Hover

`on_hover(win, cx, cy)` (`apps/gui_apps.h`) is delivered every tick
while the cursor is over a window's content with no button held, and
once with `(-1, -1)` when it leaves so the app can clear its highlight.

Two rules that keep it cheap and honest:

- **Return 1 only when the hovered item actually changed.** Returning 1
  every tick repaints the window continuously. Same contract as
  `on_press`, for the same reason.
- **It reaches unfocused windows too** -- the only app callback that
  does. A control that stays inert until you've clicked its window
  first feels dead in exactly the moment hover exists to prevent.

Hover is suppressed entirely while anything is held, armed or being
dragged: the press visual owns the feedback then.

**A control built on `ui_button_group` gets all of this without
writing any of it.** `ui_button_group_hover()` maintains each button's
`hovered` flag and returns the changed/not-changed answer this contract
wants, so the app's `on_hover` is a forward into it -- exactly as its
`on_press` already forwards into `ui_button_group_press()`. Hand-rolling
a `hover_index` next to a group is a sign of not knowing the function
exists; the Control Panel tracks its own only because its applet grid
isn't a button group.

## Layout and text

- **Never draw text into a fixed box with `gfx_draw_string()`.** It
  draws past any boundary you had in mind. Use
  `gfx_draw_string_clipped()`, and `gfx_text_width()` to measure. This
  has caused the same bug twice in different files -- see
  `docs/decisions.md`.
- **`gfx_draw_string_clipped()` bounds WIDTH ONLY. Budget the height
  yourself.** The name reads like it handles both; it takes a `max_w`
  and has no notion of a row limit, so a line drawn below the bottom of
  its box lands wherever the caller put it. A panel that emits rows in a
  loop must stop at the last one that fully fits (`if (ly +
  gfx_char_h() > y + h) break;`) -- otherwise it keeps drawing into
  whatever is underneath. This is the *other* half of the rule above and
  it went unnoticed until the Control Panel was resized small: System
  Info's lower rows carried on down the desktop, outside any window.
  Nothing reaches the screen now (the WM clips app drawing to its
  window, below), but stopping cleanly still beats a final row sliced
  through the middle of its glyphs.
- **An app cannot draw outside its own window, and must not rely on
  that.** `wm_render_frame()` narrows the clip to each window's content
  rect for the duration of its `on_draw()`. That's a containment
  boundary, not a licence to overdraw: clipped-away drawing still costs
  the CPU that produced it, and a control the user can't see is still a
  control that mis-reports its layout to hit-testing.
- **Size boxes from font metrics, not constants.** `gfx_char_w()`/
  `gfx_char_h()` change with the font size setting; a hardcoded cell
  width is correct at exactly one font.
- **Budget both axes.** A row sized to exactly `gfx_char_h()` has the
  glyph's own background painting over its border.
- **One hit-test per control, shared by drawing and input.** The
  Control Panel's first version computed cell geometry separately in
  the draw and press paths and tested clicks in the wrong coordinate
  space entirely -- a grid that rendered perfectly and opened nothing.

## Colours

`apps/theme.h` holds the named colours; `apps/ui/` widgets take colours
from their caller and stay theme-agnostic. Add a `THEME_*` entry only
when a second caller needs the same colour -- the standing rule for this
directory (see `apps/README.md`).

Semantic colour is separate from decoration: the close button is red
because closing is destructive, not because it looks nice.

## Size everything from the font, never in fixed pixels

Window sizes come from each app's `default_size()`, called at open time
with whatever font is active. Chrome heights come from `gfx_char_h()`,
the desktop's column pitch from `gfx_char_w()`, the Start menu's row
height and origin from both. Nothing lays itself out in absolute pixels.

That is not style, it is what makes the default font size a one-line
change: it dropped from 18pt to 14pt in 2026-08-14 and the whole UI
reflowed correctly, with all 82 GUI regression checks passing unchanged.
A single hardcoded height would have been a visible break at that
moment and nowhere else.

Two corollaries:

- **A label in a fixed box must be clipped**, because the box is
  font-sized and the text may not fit at every font. Use
  `gfx_draw_string_clipped()` and mark the cut (the desktop appends
  ".."), so a truncated label reads as truncated rather than as a
  different, shorter name.
- **A test tool's pixel constants are the exception that doesn't
  reflow.** `tools/gui_flow.py`'s calibrated numbers have needed
  re-measuring three times, once per font change, and a stale one fails
  silently by clicking the wrong row. Ask the kernel instead:
  `DebugConsole.menu_row(label)`, `gui menu --json`, `gui windows
  --json`.

## Shapes: use the wrappers, and pick anti-aliasing deliberately

Lines, curves and rotation come from `kernel/lib/geom.c` (see
`api/geom.h`). Don't call it directly if a wrapper fits:

- **In the kernel**, use `gfx_draw_line()`, `gfx_draw_polyline()`,
  `gfx_draw_circle()`, `gfx_draw_ellipse()`, `gfx_fill_circle()`,
  `gfx_fill_ellipse()`. They install the plot callback that routes
  opaque pixels to `gfx_put_pixel()` and partial ones to
  `gfx_blend_pixel()` -- get that wrong by hand and anti-aliased edges
  come out as hard pixels against the wrong background.
- **In a ring-3 app**, use `uui_canvas` (`userland/ui/uwidgets.h`). It
  owns the drawing rect, converts local to surface coordinates, and
  CLIPS. A shape drawn without clipping does not fail visibly at first
  -- it fails the day the shape grows, by painting over the app's own
  controls.

**Anti-aliasing is a choice per call, and neither answer is the
default.** `GEOM_AA` costs roughly 2-3x the pixels of `GEOM_ALIASED`
and is what curves and diagonals should use in anything a person looks
at closely. `GEOM_ALIASED` is right for large, fast-changing or
throwaway drawing, and for anything axis-aligned, where AA does nothing
but cost. If a shape ROTATES, use AA: aliased edges crawl visibly as
the angle changes, which is the one artefact a still screenshot will
never show you. ("Shapes" toggles between them live, for exactly this
reason -- `run shapes`, press `A`.)

**Angles are in TURNS**, not radians -- `FX_ONE` is a full rotation.
See `docs/decisions.md`.

## An app cannot draw outside its own window

**This is enforced, not asked for.** The window manager narrows the clip
rect to a window's content area around every `on_draw()`
(`wm_render.c`'s `clip_to_window_content()`), so a kernel-space app that
paints out of bounds has those pixels dropped rather than landing on the
desktop or on the window behind it. It is a containment boundary, and it
exists because the Control Panel's System Info rows once carried on down
the desktop, perfectly legible, well outside the frame.

A ring-3 client cannot do it at all: it draws into its own buffer, which
IS its window, and it has no mapping of the framebuffer or of any other
window. `ugfx` clips every primitive to the surface besides, so even a
bounds mistake cannot reach the pages after its own buffer. Paging
enforces the rest.

**Deliberately drawing outside is possible, and is `gfx_clear_clip_rect()`.**
That is the explicit opt-out: it removes the active clip for the rest of
the current `on_draw()`, and the WM re-establishes the boundary for
every window on every frame, so the escape cannot outlive one paint.
Nothing in the tree needs it today. If you reach for it, say why in a
comment -- the WM's own chrome and overlay drawing happen outside any
app's `on_draw()` and do not need it either.

**The boundary is tested continuously.** UI Demo paints two magenta
squares outside its own content rect on every frame, and
`tools/uidemo_test.py` asserts that colour appears nowhere on screen.
That check exists because the boundary is one call in one function, it
has silently regressed once already (an empty clip rect used to CLEAR
the clip rather than reject every write, handing an app a full-screen
`on_draw()`), and a regression is otherwise invisible until some app
happens to overdraw. Validated by removing the clip and watching that
one check -- and only that one -- go red.

## The damage invariant, and how to check it

The WM only repaints the region declared as damage. That makes it
correct **only if everything that changes on screen is inside that
region** -- and nothing enforces it: damage is declared by hand from
eight sites across three files. A missed declaration is stale pixels:
no crash, no wrong return value, no failing assertion, and often
visible only in one specific interaction. Every rendering bug this
project has had is that shape.

So when you change anything that draws:

- **Whatever you change, damage it.** Moving, resizing, opening,
  closing, focusing, a menu opening, a clock ticking -- if it alters
  pixels, it declares them via `wm_damage_rect()`.
- **Remember the things that change without moving.** A window losing
  focus repaints its title bar. A taskbar button changes tint when the
  frontmost window changes. Those bit us; both were "nothing moved, so
  nothing was damaged".
- **Anything alpha-blended must have the scene under it redrawn.** The
  cursor blends, so compositing it over a region that wasn't repainted
  blends it over its own previous frame and the edges darken. It is a
  damage source for exactly this reason.
- **Remember the overlays.** A menu, a picker or a dialog draws outside
  every window's rect. They declare no damage and rely on the frame
  being a full repaint -- which is now *enforced* (`wm_render_frame()`
  discards the damage box while one is open) rather than assumed. It
  used to be assumed, and the assumption held only while nothing else
  declared damage in the same frame.
- **Then check it, don't reason about it:** `gui damage verify on`
  (see `apps/wm/wm_debug.c`) renders every frame twice, once
  damage-limited and once unrestricted, and reports any pixel that
  differs. Run it while exercising whatever you changed. It found four
  real bugs in its first minute, four more once a systematic sweep
  (`tools/damage_sweep.py`) drove it -- and then a whole different
  CLASS of bug: its long-standing "20 px" violation turned out to be
  `gfx_set_clip_rect()` treating an empty rectangle as NO clip, which
  handed an app's entire `on_draw()` an unclipped screen on frames
  whose damage grazed only the window's border. The contract since:
  a non-positive w/h is an EMPTY clip (nothing draws);
  `gfx_clear_clip_rect()` is the only way to remove one.

  `python3 tools/damage_sweep.py` is that sweep: it turns the checker on,
  walks the interactions that historically break the invariant, and
  exits non-zero on a violation. Run it after touching anything that
  draws, damages, focuses or changes window chrome. Add `--random 60`
  to cover interaction *orders* the fixed walk doesn't -- three of the
  four bugs it has found needed one specific window arrangement that no
  scripted sequence would have visited.

  **A clean run only means the interactions it ran found nothing.**
  When you change drawing code, add the interaction you changed to its
  sequence rather than trusting the existing walk to cover it -- and if
  you are ever unsure the checker is really running, `--positive-control`
  expects a violation, so you can prove it against a deliberately broken
  build instead of trusting a green result.

## A GUI test asks the app where things are

Every app built to be tested against reports its own geometry -- one
`<app>: layout <what> x y w h` line per widget, content-relative, on
stderr (which reaches `dmesg`; a client's stdout goes to its owning
Terminal's scrollback, where no test can read it). UI Demo, Shapes,
Calculator, Terminal and Notepad all do it, and their tools parse those
lines.

**Do not re-derive geometry in Python.** Four tools have now learned
this the same way. `calculator_client_test.py` inverted the app's sizing
formula, including a literal `char_h = (ch - 150) // 7`, and when the
app moved to `uui_layout` it computed 18 against a real 17 and clicked
several pixels off centre -- inside the buttons, so the suite stayed
GREEN while measuring something it no longer understood. The arrow check
in `notepad_client_test.py` computed a stepper's position, hit the track
instead, paged rather than stepped, and could not step back.

A derived copy drifts silently the moment the layout changes. Add the
log line to the app instead; it is a dozen lines and it cannot drift.

## Three ways a GUI test passes without testing anything

All three of these shipped a real bug past a green suite. They are
listed together because they share a shape: the assertion was true, and
it was not the assertion anyone wanted.

**1. "It responds" is not "it is drawn."** The ring-3 Calculator shipped
with NO VISIBLE BUTTONS. The layout placed them, hit-testing worked, and
every check passed -- because every check asserted that clicking a
button CHANGED THE DISPLAY, which it did. Nothing asserted the buttons
were painted. If a control can be interacted with, assert separately
that it can be SEEN: sample its face, away from its label, against a
control point that is not the control.

**2. Moving identical content is pixel-identical.** A scrolling test
typed forty copies of the same line, scrolled, and compared the text
area -- which is unchanged whether scrolling works or not. It reported a
working feature as broken, and would just as happily have reported a
broken one as working. **Test content must be distinguishable**: number
the rows.

**3. A test must not assume the thing it is testing.** The same test
then tried to reach a known starting position by scrolling to the top --
using the scroll it was there to verify. Pick a fixed point you can
reach unconditionally (the bottom, after typing) or set the state
directly.

The general form is the rule this file already states: **ask what a
broken version would still pass.** These are three specific ways to
answer it wrong.

## Verifying a GUI change

A screenshot proves it drew *something*. It does not prove it drew the
right thing:

- **Read pixel values, don't eyeball.** The invisible hover state
  described above looked plausible in a screenshot and was two units
  from the background. `Image.open(p).convert("RGB").getpixel(...)` on
  the control and on a neighbour that shouldn't have changed.
- **Test the cancel paths, not just the happy path.** Press-drag-off-
  release must do nothing; that's a separate test from press-release.
- **Check the neighbour.** A hovered control changing is half the
  assertion; the one next to it *not* changing is the other half.
- **Park the REAL cursor to test hover, and confirm it arrived.** `gui
  move` holds for one WM iteration only -- injected input overrides the
  mouse for that tick and then the real pointer takes over, so the hover
  is recomputed away before a screenshot can see it. Use
  `DebugConsole.warp_cursor()`, which drives the real PS/2 cursor and
  checks `gui state` for where it actually landed; `QMPSession.goto()`
  alone is open-loop and a large jump was measured landing about a third
  of the way.
- **Don't sample the pixel under the cursor.** The sprite draws
  down-and-right from its hotspot with a black outline, so probing the
  hover point measures the cursor, not the control. A hover check
  written that way "passed" by reading pure black.
- **A moving shape needs a different assertion than a static control.**
  A fixed sample point says nothing about a curve, because the curve
  moves off it. Two that do work: compare whole regions between frames
  (and always pair "it changed" with a case that must NOT change --
  `tools/gfxdemo_test.py` pairs "it rotates" with "it stops dead at
  speed 0", since either alone is satisfied by a bug), and COUNT
  DISTINCT COLOURS to tell anti-aliased drawing from aliased -- 468 vs
  5 in the canvas, because partial coverage is exactly what AA emits.
- `tools/dialog_test.py`, `tools/uidemo_test.py` and
  `tools/gfxdemo_test.py` do all of the above for the confirm dialog,
  UI Demo's widgets and the geometry; extend those rather than starting
  a fresh script. `tools/gui_regress.py` runs the whole set in one go.
