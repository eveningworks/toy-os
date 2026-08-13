# GUI guidelines

How toy-os's GUI is supposed to look and behave. The goal in one line:
**minimalist but modern -- flat, quiet, and never ambiguous about
whether it heard you.**

This is a working document for writing apps and widgets, not a style
manifesto. Where a rule exists because something went wrong, that's
said, because the reason is the useful part.

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
