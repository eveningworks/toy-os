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
  real bugs in its first minute, and four more once a systematic sweep
  drove it.

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
