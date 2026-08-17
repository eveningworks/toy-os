# apps/

Everything in here is a "program": one C file, one entry point, registered
in `apps.c`. There's no process isolation yet -- everything still runs in
kernel space, one address space, no separate memory or privilege level --
so this is a *source-code* boundary, not a security one. It exists so you
never have to read or touch the kernel to add something new.

The rule: app code includes `kapi.h` (and `apps.h` if it wants to launch
other apps) and nothing else. It never includes a driver header directly,
never touches ports/hardware registers directly. If an app needs a kernel
capability that isn't in `kapi.h` yet, that's a sign the capability
belongs behind a proper function in `kernel/core` or `kernel/drivers`,
exposed through `kapi.h` -- not a reason to reach around the boundary.

## Adding a new app: concrete steps

Say you want to add a `calc` app -- a tiny calculator.

1. **Write `apps/calc.h`:**
   ```c
   #ifndef CALC_H
   #define CALC_H
   void calc_main(void);
   #endif
   ```

2. **Write `apps/calc.c`:**
   ```c
   #include "calc.h"
   #include "kapi.h"

   void calc_main(void) {
       vga_write("calc: type two numbers and an op, or 'q' to quit\n");
       // ... your logic, using vga_write/keyboard_getchar/etc from kapi.h
   }
   ```

3. **Register it in `apps/apps.c`** -- add one line to the table and one
   include:
   ```c
   #include "calc.h"
   ...
   const struct app_info app_registry[] = {
       { "shell", "Command-line shell",     shell_main },
       { "gui",   "Graphical desktop demo", gui_main   },
       { "calc",  "Basic calculator",       calc_main  }, // <-- new
   };
   ```

4. **Build.** `make run`. Type `run calc` at the shell prompt. Done --
   nothing in `kernel/`, the Makefile, or `shell.c` needed to change. The
   Makefile picks up new files in `apps/*.c` automatically (it globs the
   directory), and `apps` command / `run <name>` in the shell already
   know how to find and launch it via the registry.

If you want a dedicated shortcut command (like `gui` is a shortcut for
`run gui`), add one `else if` branch to `dispatch()` in `apps/shell.c`
that calls `app_run("calc")` -- but that's a convenience, not a
requirement.

## Current apps

Two kinds of app live here now:

**Console apps** (registered in `apps.c`, launched via the shell's `run`
command or a dedicated shortcut) -- these take over the whole screen and
run their own blocking loop:
- **shell** (`shell.c`) -- the command-line shell, also what the kernel
  launches first (see `apps_start()` in `apps.c`). Tab completion is
  shared with the GUI Terminal via `completion.c`/`completion.h`, which
  generates candidates only -- it does no input handling and no drawing,
  because the two shells have completely separate input loops and only
  the candidate logic is genuinely common. See `completion.h`'s top
  comment. *Line editing*, by contrast, IS shared and lives outside
  `apps/` entirely: `kernel/lib/klineedit.c` owns the buffer, cursor,
  kill ring, undo and the whole bash keymap, and each front end only
  paints the result (`repaint_line()` here, `term_repaint_line()` in
  `terminal.c`). The split is deliberate and the opposite of
  completion's for a reason -- candidate generation has no state to
  keep, whereas two copies of an editor drift, and a drifted editor
  means the same keystroke doing different things in the two windows. The shell itself is four files (`shell.c` + `shell_fs.c` +
  `shell_sys.c` + `shell_path.c`, sharing state through
  `shell_internal.h`); `shell_path.c` owns PATH lookup and the single
  "run this name" resolver behind both a bare typed name and `run`.
- **gui** (`gui.c`) -- a one-line wrapper that hands off to the window
  manager (`wm.c`). Launched via `run gui` or the `gui` shortcut command;
  returns to the shell on Esc.

**GUI apps** (registered in `gui_apps.c`, launched from the Start menu
inside the window manager) -- these are event-driven: they never run
their own loop, the window manager calls their `on_open`/`on_draw`/
`on_key`/`on_click` callbacks instead. See "Adding a new GUI app" below.
**Every one of them has now retired**, across Milestone 41's stage 0
and its stage-4 prerequisites (`docs/wm-ring3-design.md`).
Notepad, Calculator and Terminal were
DELETED -- their ring-3 twins under `userland/gui/` had been shipping
alongside them, which is what made that migration verifiable, and
keeping two of each stopped paying once it was. About and UI Demo were
PORTED to `userland/gui/` and are launched from the Start menu through
`exec_path`, exactly as the other ring-3 apps are. Task Manager and
Control Panel followed once the syscalls they needed existed.

The one deliberate loss: the ring-3 About no longer reports the
filesystem backend and whether it persists. Those are kernel calls with
no syscall behind them, and stage 0 is the stage that adds no kernel
capability; `df` and `fsck` report the same two facts meanwhile.

**Nothing is left. As of 2026-08-17 this directory contains no
applications at all** -- only the window manager, the shell and their
support code.

Task Manager went to `userland/gui/system/taskmgr.c` once
`SYS_PROC_INFO`/`SYS_KILL` existed, and Control Panel to
`userland/gui/system/cpanel.c` once the settings registry did
(`kernel/include/api/setting.h` + `SYS_SETTING`, plus `SYS_SYSINFO` for
its System Info page). Both were on stage 4's prerequisite list anyway,
so building what they needed is what closed that list.

Two consequences worth knowing:

- **`Exec=builtin:` is gone**, along with the table, the lookup and the
  struct behind it in `gui_apps.c`. Every `.desktop` entry now names a
  real binary. An entry still carrying the retired form is refused
  loudly rather than shown as a menu row that does nothing.
- **The WM's live-`.desktop`-reload deferral is now unreachable.** It
  defers a reload while a window holds a `gui_app_registry[]` pointer,
  and only a kernel-space app ever held one (a ring-3 client's is 0).
  The guard is kept and still correct; `desktop_entries_test.py`
  asserts the property that makes it unreachable, so a kernel-space app
  coming back turns that check red instead of producing a mystery
  rebinding bug later.

## The window manager (apps/wm/)

The window manager is split across a few files under `apps/wm/`, rather
than one large `wm.c`, purely for readability -- it's still a single,
tightly-coupled event loop, not multiple decoupled components (see
`wm_internal.h`'s comment for why):
- **`wm.h`** -- the public API other apps include (`#include "wm/wm.h"`):
  `struct window`, `window_set_state`/`window_get_state`,
  `window_content_x/y/w/h`, `window_invalidate`, `wm_window_count()`/
  `wm_get_window()` (read-only window introspection -- Task Manager's
  data source), `window_start_write()`/`window_write_pending()`
  (registers a `fs_write_range_begin()` handle for `wm_run()` to poll
  once per frame instead of blocking -- Milestone 1 phase 3, see
  `docs/roadmap.md`; completion is delivered via `gui_apps.h`'s
  `on_write_complete` callback), `window_start_read()`/
  `window_read_pending()` (same shape, for `fs_read_range_begin()` --
  Milestone 1 phase 4; completion via `gui_apps.h`'s `on_read_complete`
  callback, which also carries the actual byte count read),
  `window_start_process()`/`window_process_pending()` (same shape again,
  for a pid from `scheduler.h`'s `scheduler_spawn()` -- Milestone 1
  phase 4b; completion via `gui_apps.h`'s `on_process_exit` callback,
  which carries the process's real exit code -- the process's own
  console output needs no help from this polling at all, since it
  already streams straight into whatever `vga_sink` is active the
  instant each `SYS_WRITE` syscall runs), `tray_register()`/
  `tray_set_text()`/`tray_unregister()` (the taskbar notification area
  -- register a small text item, update it whenever it changes, no
  polling needed since a tray item's text is push-only; the taskbar
  clock is itself tray item 0, see `wm_tray.c`), `wm_run()`.
- **`wm.c`** -- shared state, the app-facing helpers behind `wm.h`, window
  lifecycle (`open_app`, `close_window`, `bring_to_front`), and
  `wm_run()`'s main loop. Start reading here.
- **`wm_input.c`** -- mouse click handling (left AND right button) and
  the per-tick drag/resize update, called from `wm_run()`.
- **`wm_render.c`** -- everything the window manager draws (window
  chrome, taskbar, cursor), plus the shared layout-metric helpers
  (`title_buttons()`, `btn_size()`, etc) that `wm_input.c` also needs
  for hit-testing the exact same regions this file draws.
- **`wm_internal.h`** -- private glue between all of the above
  (`extern` state declarations, cross-file prototypes). Never included
  outside `apps/wm/` -- it's not part of the public API in `wm.h`.
- **`wm_tray.c`/`.h`** -- the taskbar notification area: a small fixed
  array of text items drawn right-to-left from the taskbar's right
  edge. `tray_init()`/`tray_update_clock()` (called from `wm.c`) and
  `draw_tray()` (called from `wm_render.c`'s `draw_taskbar()`) are the
  WM-internal half; `tray_register()`/`tray_set_text()`/
  `tray_unregister()` in `wm.h` are the app-facing half. The clock is
  tray item 0, registered through this same API rather than drawn as
  hardcoded chrome.
- **`desktop.c`/`.h`** -- the desktop background + icon grid behind
  every window (one icon per `gui_app_registry` entry; single-click
  selects, double-click launches, drag repositions to any grid cell --
  positions persist across reboot in `/etc/desktop.conf`, built on
  `apps/ui/ui_icon_grid.h`'s reusable geometry/drag-session helper).
- **`start_menu.c`/`.h`** -- the Start menu popup (app list + system
  actions -- "Exit to shell" and "Shutdown", the latter confirm-gated
  then `system_poweroff()`, see `kernel/include/api/power.h`), with hover/
  click-flash feedback.
- **`context_menu.c`/`.h`** -- a small, generic reusable right-click
  popup (label + callback + caller-supplied context pointer per row),
  wired into the desktop, window chrome, taskbar app buttons, and Start
  menu rows -- see its own top comment for the full list and
  `docs/decisions.md` for why its callback shape differs from
  `start_menu.h`'s.
- **`confirm_dialog.c`/`.h`** -- a reusable screen-absolute Yes/No modal
  (message + confirm callback + optional cancel callback), same
  overlay pattern as the two above. Callers so far: "Exit to shell"
  and "Shutdown".
- **`file_picker.c`/`.h`** -- a reusable Open/Save file-picker dialog,
  the fourth screen-absolute overlay in this same family: full
  directory navigation (double-click a row to enter it, `../` to go
  up, directories sorted first, a scrollbar), backed by `fs_list()`/
  `fs_is_dir()`/`fs_exists()` directly. First caller: Notepad's
  Open.../Save As... toolbar buttons, replacing its old always-visible
  inline filename field. See `docs/decisions.md` for why this is a
  WM-level overlay rather than an `apps/ui/` widget.

`wm_run()` owns the screen: it keeps a small fixed array of windows in
z-order, and drives everything through one event loop -- mouse clicks
(left AND right button), dragging by the title bar, minimize/maximize/
close buttons, a Start menu that lists `gui_app_registry`, right-click
context menus, desktop icons, and keyboard input routed to whichever
window is frontmost. `wm_render.c` now tracks a SCENE-level damage
region each frame -- a computed bounding box of what actually needs
repainting (window move/resize/open/close/minimize/restore/z-order,
desktop icon drag) -- and clips a `draw_window_chrome()`/`on_draw()`
call to it, or skips the call entirely for a window whose rect doesn't
intersect it at all; everything within the damaged box still redraws
back-to-front (desktop, then windows in z-order, then taskbar/menus)
rather than computing exact exposed sub-rectangles, which is what
makes this correct for overlapping windows without separate occlusion
tracking. Menu/taskbar-content-click/dialog redraws still fall back to
a full-screen repaint (imprecise but safe) -- see `docs/decisions.md`'s
compositor entry for the full design and `docs/roadmap.md`'s
Milestone 19 entry for what's still open. Below that scene-level layer,
`gfx_present()` (`kernel/drivers/gfx.c`) still separately tracks
dirty PIXELS for the final blit to the real framebuffer -- only the
touched bounding box gets copied out, not the whole screen, plus a
cheap cursor-only fast path for plain mouse movement. If you add
drawing code, do it in `on_draw` and let the window manager handle
presenting; don't call `gfx_present()` yourself, and don't call
`gfx_set_clip_rect()` from app code either -- that's the compositor's
own mechanism, not a per-app one. (Its contract, for the record: a
non-positive w/h sets an EMPTY clip -- nothing draws -- and only
`gfx_clear_clip_rect()` removes a clip. The two used to be conflated,
and the compositor painting an app's whole `on_draw()` unclipped was
the result.)

There's no process isolation here either -- a GUI app's callbacks run in
the kernel's own context, same as everything else in `apps/`. What the
window manager gives you is a real event-driven API boundary (an app
genuinely can't tell what else is drawn on screen or steal input meant
for another window), just not memory/privilege isolation.

## Shared widgets (apps/ui/)

`apps/ui/` holds every reusable GUI primitive, one widget per file, all
pulled in together via the umbrella include `apps/ui/ui.h`
(`#include "ui/ui.h"`). This used to be a single flat `apps/widgets.h`/
`widgets.c` pair; it was split out file-per-widget once the pair grew
past "two small functions" (see `docs/decisions.md`). `apps/widgets.h`/
`.c` no longer exist.

**This directory is now SHRINKING, on purpose.** Milestone 41's stage 0
deleted the checkbox, dropdown, listbox and text view from it: with the
GUI apps in ring 3, nothing kernel-side called them, and their ring-3
twins under `userland/ui/` are the copies that survive the milestone.
What is left is what the WINDOW MANAGER itself still draws with, plus
`ui_focus`, which has no user of its own but is what `ui_button_group`
and `ui_textbox` export their focus tables into. All of it retires with
the WM in stage 4 -- so **add a widget here only if the WM needs it**;
anything an app needs belongs in `userland/ui/`.

**The standing rule for anything here: the WIDGET owns behaviour, the
APP owns configuration.** Input handling, hit-testing, geometry and
state transitions belong in `apps/ui/`; what an app gets to decide is
exposed as fields or setters (visibility policy, colours, step sizes,
who owns an ambiguous event). An app forwarding
`if (ui_thing_click(&t, cx, cy)) window_invalidate(win);` is the shape
to aim for. Where two apps want different behaviour, add a flag to the
widget rather than a second copy. `apps/ui/ui_textview.h` is the worked
example and `docs/gui-guidelines.md` has the full rule, including the
cases where not following it is the right call.

- **`ui_primitives.h`/`.c`** -- the original two functions, still the
  base every other widget in this directory builds on:
  - `int widget_hit(int x, int y, int w, int h, int px, int py)` -- a
    plain bounds check, used for click hit-testing.
  - `void widget_button(int x, int y, int w, int h, const char *label, uint32_t bg, uint32_t fg)`
    -- fills the rect with `bg`; if `label` is non-NULL, centers it in
    `fg`. Pass `label = NULL` for icon-only buttons (like the window
    manager's minimize/maximize/close) and draw the icon on top
    yourself afterward.
- **`ui_button.h`/`.c`** and **`ui_button_group.h`/`.c`** -- an owned
  x/y/w/h button object built on `widget_button`/`widget_hit`, plus a
  small group helper for laying out a row of them (used by the window
  manager's title-bar buttons and, per the widgets migration, available
  to any app that wants a row of buttons without hand-rolling the
  layout math). `ui_button_set_disabled()` marks one non-interactive and
  drawn dimmed -- `ui_button_group_press()`/`_click()` skip it. First
  user: Notepad's Save As... button while a steppable write is in
  flight (Milestone 1 phase 3, see `docs/roadmap.md`).
- **`ui_textbox.h`/`.c`** -- `struct text_field` (single-line input,
  cursor position, `TEXTFIELD_MAX` 48 chars) and
  `widget_textfield_init/set_active/key/draw`, wrapped in an owned-
  geometry `struct ui_textbox` (`ui_textbox_init/set_geometry/draw/
  hit/set_active/key`).
(Note the *physical* console has its own, unrelated scrollback now --
a character ring inside `kernel/drivers/vga.c`, scrolled with
PageUp/PageDown, covering the boot log and any command's output. The
widget below is the in-window one, used by GUI apps; the two share
nothing but the idea. See `docs/decisions.md`.)

- **`ui_scrollback.h`/`.c`** -- `struct text_scrollback`
  (`SCROLLBACK_CAP` 8192 chars), the multi-line scrolling text buffer
  Notepad, Terminal, and the editor all use, with cursor movement
  (`widget_scrollback_cursor_left/right/up/down/home/end`) and in-place
  editing (`insert_at_cursor`/`delete_at_cursor`/`backspace_at_cursor`).
  Deliberately has no owned-geometry wrapper -- every real caller
  already recomputes its content rect live each frame for resize
  support, so a wrapper wouldn't save any work (see
  `docs/decisions.md`).
- **`ui_scrollbar.h`/`.c`** -- draws and hit-tests a vertical scrollbar
  (`widget_scrollbar_draw/hit/thumb_rect/offset_for_drag`) for anything
  using `ui_scrollback`. Same "no owned-geometry wrapper" reasoning as
  `ui_scrollback`.
- **`ui_checkbox.h`/`.c`** -- `widget_checkbox_width/draw/hit`, a small
  labeled checkbox.
- **`ui_icon_grid.h`/`.c`** -- icon-grid cell geometry
  (`icon_grid_cell_rect`/`icon_grid_nearest_cell`) and a drag-to-
  reposition session (`struct icon_drag`,
  `icon_drag_start/update/end`), mirroring `wm_input.c`'s window-drag
  shape. First (and currently only) user: the desktop icon grid
  (`apps/wm/desktop.c`) -- built as its own widget rather than
  desktop.c-local, by explicit request, ahead of the second real
  caller a future file manager's icon view (`docs/roadmap.md`
  Milestone 22) is expected to be. Deliberately an exception to this
  section's own "wait for a second caller" rule below, not a change to
  it. (A second caller did arrive and has since left: the kernel-space
  Control Panel's applet chooser used the same cell geometry, and that
  file was deleted when Control Panel moved to ring 3. The desktop icon
  grid is the only caller again -- which is worth noting rather than
  quietly deleting, because it means this widget is back to being the
  documented exception to the second-caller rule rather than a
  vindication of it.)
- **`ui_radio_list.h`/`.c`** -- a single-select list: one row per
  option, the active one marked, laid out in one or more columns
  (`ui_radio_list_size/draw/hit`). The mutual exclusivity
  `ui_checkbox.h` deliberately leaves out. Caller-owned selection and
  labels -- this module knows geometry, drawing and hit-testing only,
  so the selection can live where it actually belongs. The worked
  example used to be the kernel-space Control Panel's timezone applet,
  whose selection lived in `tz.c`'s own current index rather than a copy
  needing to be kept in sync; that app is in ring 3 now and its ring-3
  twin `uui_radio_list` makes the same split against the settings
  registry. Hit areas are the full row, not just the marker.
  Added ahead of a second caller by explicit request, the same
  acknowledged exception `ui_checkbox` and `ui_icon_grid` above are --
  see `docs/decisions.md`.
- **`ui_listbox.h`/`.c`** -- a scrollable single-select list of text
  items: rows, selection, hover, keyboard navigation (arrows/Home/End/
  PageUp/PageDown) and a scrollbar that appears only when the items
  overflow, as one control. Where `ui_radio_list` above stops -- it has
  no viewport, so no scrolling, hover or keys -- this begins; all four
  arrive together the moment a list is longer than its box. Windows'
  listbox is the behaviour model: the wheel scrolls the view without
  moving the selection, and the armed row follows the cursor while
  held, but nothing commits until release, so a press dragged off is
  still cancellable. **Scroll offset here is 0 = top**, the opposite of
  `ui_scrollbar`'s convention; the conversion happens in two helpers in
  the `.c` and nowhere else (see `docs/decisions.md`).
- **`ui_dropdown.h`/`.c`** -- a combo box: a closed box showing the
  current value, which opens a popup list to change it. **Composes
  `ui_listbox`** for that popup rather than reimplementing a list, the
  same layering `ui_textview` uses over scrollback + scrollbar. The one
  rule an app must follow is that `ui_dropdown_draw_popup()` is a
  SEPARATE call made after every other widget -- drawing is
  immediate-mode, so z-order is call order -- and input is forwarded to
  the dropdown *before* the widgets underneath, since the popup is on
  top. The popup cannot leave the window (the WM clips `on_draw` to the
  content rect), so it flips above the box when there's no room below.
  See `docs/decisions.md` for why it isn't a WM overlay.

- **`ui_focus.h`/`.c`** -- keyboard focus for a window's widgets: which
  one gets keys, Tab/Shift-Tab to cycle, and the focus ring. A widget
  joins by exporting one `const struct ui_focus_ops`
  (`key`/`hit`/`draw_ring`/`accepts_focus`/`set_focused`), so nothing
  central lists them -- the same "adding one is adding a row" property
  `gui_app_registry[]` has, and the reason this is a small vtable rather
  than a switch over a widget-kind enum. `ui_textbox`, `ui_dropdown`,
  `ui_listbox` and `ui_button_group` all export one; a group is ONE stop
  with arrows moving between its buttons. Tab order is array order.
  Built because routing keys by trying each widget in turn breaks with
  two keyboard-taking widgets -- see `docs/decisions.md`.

This is *not* a general-purpose widget toolkit -- there's a focus ring
now, but no layout engine beyond `ui_button_group`, and no view tree.

Add the next primitive here
only once a second real caller needs it, the same reasoning that
produced the original two functions in the first place (three
independent copies of the same button logic was the signal).

## Shared theme colors (theme.h)

`apps/theme.h` names the handful of `gfx_rgb(...)` values that had
already converged identically across two or more files by hand --
`THEME_WHITE`, `THEME_TEXT`, `THEME_BORDER`, `THEME_BUTTON_BG`,
`THEME_WINDOW_BG`, `THEME_PANEL_BG`. These are function-like macros that
expand to the exact same `gfx_rgb()` call as before, not precomputed
constants -- `gfx_rgb()` still runs at each call site, converting to
whatever pixel format the active framebuffer uses. Using a `THEME_*`
name changes nothing about what gets drawn; it just means the next app
that wants "the same light gray Calculator's buttons use" has a name to
reach for instead of re-typing `gfx_rgb(225, 225, 230)` and hoping it
matches.

Like `widget_hit`/`widget_button`, this only names values that already
repeated -- a color used once in a single file (the close button's red,
the taskbar's dark background) stays a plain `gfx_rgb()` call at its
call site. Add a new `THEME_*` name only when a value starts repeating,
not preemptively.

## Adding a new GUI app

Say you want to add a `clock` app -- a window that just shows the time.

1. **Write `apps/clock.h`:**
   ```c
   #ifndef CLOCK_H
   #define CLOCK_H
   struct window;
   void clock_open(struct window *win);
   void clock_draw(struct window *win);
   #endif
   ```

2. **Write `apps/clock.c`:**
   ```c
   #include "clock.h"
   #include "wm/wm.h"
   #include "kapi.h"

   void clock_open(struct window *win) { (void)win; }

   void clock_draw(struct window *win) {
       int cx = window_content_x(win), cy = window_content_y(win);
       int cw = window_content_w(win), ch = window_content_h(win);
       gfx_fill_rect(cx, cy, cw, ch, gfx_rgb(255,255,255));
       struct rtc_time t;
       rtc_read(&t);
       // ... draw t.hour / t.minute / t.second with gfx_draw_string ...
   }
   ```

3. **Register it in `apps/gui_apps.c`** -- one include, one line:
   ```c
   #include "clock.h"
   ...
   const struct gui_app gui_app_registry[] = {
       { "Notepad", 360, 220, notepad_open, notepad_draw, notepad_key, 0 },
       { "About",   520, 340, about_open,   about_draw,   0,           0 },
       { "Clock",   200, 100, clock_open,   clock_draw,   0,           0 }, // <-- new
   };
   ```

4. **Build, `make run`, `gui`, Start -> Clock.** Done -- it's movable,
   minimizable, maximizable, and closable for free; the window manager
   provides all of that. Nothing in `wm.c` needed to change.

If you want your app to react to typing, add an `on_key` callback (see
`notepad.c`); if you want it to react to mouse clicks inside its content
area, add `on_click` (neither `notepad.c` nor `about.c` currently uses
that one, so there's no example yet -- the signature is
`void (*)(struct window *win, int content_relative_x, int content_relative_y)`).

## What's available via kapi.h

See `kernel/include/api/kapi.h` for the exact list -- it just aggregates the
driver headers apps are allowed to use (console output, keyboard, mouse,
timer/RTC, filesystem -- including the backend-capability surface:
`fs_has(FS_CAP_*)` to ask what the active filesystem supports,
`fs_backend_name()` for display, `fs_link()` for hardlinks, and
`fs_stat()`'s `struct fs_stat_info` with an inode number and
epoch-second timestamps -- graphics primitives, `system_reboot()`). If you're
tempted to `#include` something from `kernel/drivers` or `kernel/core`
directly, or to call `inb`/`outb` yourself, stop -- add the capability to
a driver and expose it through `kapi.h` instead. That's what keeps apps
decoupled from how the hardware side is implemented.

GUI apps additionally use `wm/wm.h` for the window-manager helpers
(`window_content_x/y/w/h`, `window_set_state`/`window_get_state`,
`window_invalidate`) -- that's the GUI-specific equivalent of `kapi.h`.
Apps that draw buttons can also use `ui/ui.h` (see "Shared widgets"
above), and any app can use `theme.h` (see "Shared theme colors" above)
for the handful of named colors -- both are peer-level apps-internal
headers, not part of `kapi.h`.

## GUI in user space (experimental, separate from everything above)

Everything above -- `wm.c`, `notepad.c`, `about.c`, all of it -- runs in
kernel space at ring 0. `userland/tests/gui_test.c` (a real disk-hosted
binary at `/bin/gui_test`, run via `run gui_test` -- see
`docs/decisions.md`) is a first, deliberately narrow step toward
something different: a genuinely isolated ring-3 process drawing
directly to the real screen and reading real input, via two new
syscalls (`SYS_GUI_INIT` maps the linear framebuffer straight into the
process's own address space; `SYS_GUI_POLL_KEY` is a non-blocking
keyboard read). It fills the screen with a color and cycles it on each
keypress, entirely from ring 3, with no kernel-space drawing code
involved once it's running.

**This is not the window manager moved to user space.** It's *modal*:
the physical shell's `run <name>` is still the legacy blocking
`elf_run_from_fs()` path with no scheduler backing it, so `gui_test`
has the real screen entirely to itself while it runs, the same way
`ring3test`/`hello`/`exit_test`/`write_test` each run one process at a
time with nothing else happening concurrently (`schedtest`, and now
Terminal's own async `run`/`ls` for its allowlist -- see this file's
Terminal entry above -- are scheduler-backed, but `gui_test` is
deliberately excluded from that allowlist for exactly this
whole-screen-takeover reason). It doesn't create a window inside `wm.c`,
doesn't coexist with Notepad or About, and can't be dragged, minimized,
or otherwise treated as a window -- there's no `struct window` involved
at all. `wm.c` doesn't know this exists.

**That real user-space GUI has since been built** -- `gui_test.c` is
kept as the foundational proof it always was, not as the current state
of the art. Everything the list below used to describe as missing now
exists:

- The scheduler lets a ring-3 process run concurrently with the
  compositor's event loop (the kernel context is itself a rotation
  participant, and blocking syscalls deschedule rather than spin --
  see `docs/decisions.md`).
- There is a real windowing protocol: one syscall carrying typed
  messages (`SYS_WIN_REQUEST` / `struct win_request_msg`), giving a
  client a private surface for its own content area, a present call,
  and input events scoped to its window. `apps/wm/wm_client.c` is the
  WM side.
- Real apps run on it. Calculator, Notepad and Terminal are ring-3
  processes in `userland/`, indistinguishable from the kernel-space
  windows beside them -- which is why the Task Manager labels every row
  `[r0]` or `[r3]`.
- Ring 3 has a drawing runtime and the widget toolkit, **Toykit**:
  `userland/ui/ugfx.c` (surfaces, text, the desktop font mapped
  read-only), `utext.c` (the wrapped editable buffer), `uapp.c` (the
  app layer -- describe your app, the library owns the handshake and the
  loop) and one file per widget, `uui_*.c`, mirroring `apps/ui/`'s
  layout so a widget's two versions sit at matching paths. (It was
  `uui.c` + a grab-bag called `uwidgets.c` until that split; neither
  file exists now.)
  Two widgets have no kernel-side twin and were written here first:
  `uui_menubar` (nested pull-down menus) and `uui_statusbar`. Ring-3
  windows also answer a liveness ping, which is what lets the WM tell a
  wedged client from a busy one -- see `docs/decisions.md`.

So the honest current division is: `apps/` holds the window manager,
the desktop and the kernel-space apps; `userland/` holds the ring-3
ones. New GUI apps belong in `userland/` unless they need something
only the kernel can reach.

