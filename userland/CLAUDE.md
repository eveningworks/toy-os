# userland/ -- widget, layout and drawing rules

Loaded when working under `userland/`, where every widget and app lives.
Moved here from the root CLAUDE.md so they cost nothing elsewhere; the
root file's other rules still apply.

**Before writing a helper or a widget in an app, read `docs/toolkit.md`**
-- every `userland/lib/` and `userland/ui/` header in one generated line.

## Widgets and layout

`docs/conventions/gui.md` carries the reasoning; these fire first.

- **A widget's `ops->hit` is a BOOLEAN, and a widget whose own `_hit()`
  returns a ROW INDEX must convert it.** The router tests
  `!it->ops->hit(...)`, so returning the index makes ROW 0 report "not
  hit". Write `>= 0` -- **UNLESS THE WIDGET HAS A SCROLLBAR**, and then
  answer the whole rect (`uui_hit(x, y, w, h, ...)`), or the bar goes
  dead while the rows work. `tools/check_widget_ops.py` fails the build
  on the conflation.
- **A widget's `natural_size` must not depend on where the widget
  currently IS** -- measuring from the ORIGIN instead of as an extent is
  a feedback loop between layout and measurement.
- **A WIDGET'S OPS TABLE IS THE CONTRACT, AND A MISSING SLOT FAILS
  SILENTLY AND AT A DISTANCE.** No `natural_size`/`set_geometry` means
  never measured or positioned; the router names a widget to its app
  only when it has a `release`. **Fill a new widget's table against
  `uui_widget.h`, never against the widget you copied.** When a layout
  misbehaves, check the ops tables of everything in it before suspecting
  the layout. **AND THE INVERSE HAPPENS -- a slot that is PRESENT and
  read by nobody**; when you add a slot, grep for the code meant to
  consult it.
- **A lone `uui_button` routes its own clicks, and the CLICK arrives as
  `on_action(code)`** -- never `on_widget`, which carries value changes
  only and hears a hover only from a `UUI_TRACK_HOVER` item. Ids are
  unique, or uapp refuses the app. `uui_button_group` is worth keeping
  only for a GRID of them.
- **A DEFAULT MARGIN IS THE WINDOW'S EDGE, NOT EVERY NESTING LEVEL'S.**
  A `uui_layout` inside another container takes NO margin of its own
  unless it names one -- Qt's rule for a sub-layout.
- **A layout CAN grow a child along its stacking axis.** `UUI_FILL_H` in
  a column (and `UUI_FILL_W` in a row) absorbs leftover space.
- **A PAGE THAT CAN OVERFLOW GOES IN A `uui_scrollview`, and the chrome
  stays outside it.** `uui_layout` does not shrink children below their
  natural size -- given too little room it OVERFLOWS, with no scrollbar
  and nothing to say the last children are gone. Keep tabs and status
  bars OUTSIDE; one scroll region per page; a `UUI_FILL` child absorbs a
  SHORTFALL as well as leftover space, and a container with nothing
  stretchable still overflows.

## Anything drawn

**Follows `docs/gui-guidelines.md`**, which is binding. Six things bite
most often:

1. **`gfx_draw_string()` does not clip** -- use
   `gfx_draw_string_clipped()` and `gfx_text_width()` for anything in a
   fixed box (the identical overlap bug has shipped twice).
2. **`on_click` fires on button-DOWN despite its name**, so a control
   that commits there can never be cancelled -- arm in `on_press`, act
   in `on_release`. The menu bar is the documented exception.
3. **Esc closes nothing; Alt+F4 closes a window, and the WM handles
   it.** All three user-facing closes go through one
   `wm_request_close()`, which ASKS a ring-3 client and can be refused
   from `uapp_desc.on_close`. Route a fourth through the same function.
4. **`gfx_set_clip_rect()` with a non-positive w/h sets an EMPTY clip --
   nothing draws -- and only `gfx_clear_clip_rect()` removes a clip.**
5. **Layout is FONT-DERIVED, never in fixed pixels** -- window sizes
   from `default_size()`, chrome from `gfx_char_h()`, the desktop's
   column pitch from `gfx_char_w()`. A hardcoded pixel constant in a
   test tool does not reflow; prefer `DebugConsole.menu_row(label)`.
6. **An app cannot draw outside its own window, and that is enforced.**
   The explicit opt-out is `gfx_clear_clip_rect()`, lasting only for
   that paint.

**Interaction states come from `enum ui_state` / `ui_state_bg()`**,
which derives hover/pressed from the control's own colour: don't
hand-pick tints, and don't assume hover means "lighter" -- on this
near-white theme it has to darken, and `gfx_luminance()` decides.
