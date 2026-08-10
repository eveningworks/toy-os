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
  launches first (see `apps_start()` in `apps.c`).
- **gui** (`gui.c`) -- a one-line wrapper that hands off to the window
  manager (`wm.c`). Launched via `run gui` or the `gui` shortcut command;
  returns to the shell on Esc.

**GUI apps** (registered in `gui_apps.c`, launched from the Start menu
inside the window manager) -- these are event-driven: they never run
their own loop, the window manager calls their `on_open`/`on_draw`/
`on_key`/`on_click` callbacks instead. See "Adding a new GUI app" below.
- **Notepad** (`notepad.c`) -- a small text editor with a toolbar (Save/
  Load, using `fs_write`/`fs_read` from kapi.h under a fixed filename),
  real cursor movement (arrows/Home/End/Delete, since build 377 --
  `widgets.h`'s `text_scrollback` widget gained a cursor). Good example
  of all four callbacks: on_open, on_draw, on_key, and on_click (the
  toolbar buttons).
- **About** (`about.c`) -- a static info window with no input handling at
  all (on_key/on_click both NULL). Good example of the minimum a GUI app
  can be.
- **Calculator** (`calculator.c`) -- a 4-function calculator with a
  button grid, working from both clicks and the keyboard. The arithmetic
  itself lives in `calc_engine.c`/`.h`, kept free of any gfx/wm
  dependency; `calculator.c` is just the adapter that maps button
  clicks/keypresses onto `calc_input()` calls.
- **Terminal** (`terminal.c`) -- a GUI terminal emulator that runs the
  real shell dispatcher (`shell_dispatch()`, see `shell.h`) through a
  `vga_sink` redirecting into a `text_scrollback` widget, rather than
  duplicating shell.c's command handlers. Also hosts the `edit`/`nano`
  full-screen text editor (`editor.c`) as a small non-blocking "sub-mode"
  of its own -- `editor_run()`'s blocking keyboard loop can't run inside
  Terminal's event-driven window the way it does at the physical
  console, so Terminal drives the same `editor_handle_key()` one
  keystroke at a time from its own `on_key` callback instead. See
  `terminal.c`'s top comment and `editor.h` for the full split.

## The window manager (apps/wm/)

The window manager is split across a few files under `apps/wm/`, rather
than one large `wm.c`, purely for readability -- it's still a single,
tightly-coupled event loop, not multiple decoupled components (see
`wm_internal.h`'s comment for why):
- **`wm.h`** -- the public API other apps include (`#include "wm/wm.h"`):
  `struct window`, `window_set_state`/`window_get_state`,
  `window_content_x/y/w/h`, `window_invalidate`, `wm_run()`.
- **`wm.c`** -- shared state, the app-facing helpers behind `wm.h`, window
  lifecycle (`open_app`, `close_window`, `bring_to_front`), and
  `wm_run()`'s main loop. Start reading here.
- **`wm_input.c`** -- mouse click handling and the per-tick drag/resize
  update, called from `wm_run()`.
- **`wm_render.c`** -- everything the window manager draws (window
  chrome, taskbar, Start menu, cursor), plus the shared layout-metric
  helpers (`title_buttons()`, `btn_size()`, etc) that `wm_input.c` also
  needs for hit-testing the exact same regions this file draws.
- **`wm_internal.h`** -- private glue between the three `.c` files
  (`extern` state declarations, cross-file prototypes). Never included
  outside `apps/wm/` -- it's not part of the public API in `wm.h`.

`wm_run()` owns the screen: it keeps a small fixed array of windows in
z-order, and drives everything through one event loop -- mouse clicks,
dragging by the title bar, minimize/maximize/close buttons, a Start menu
that lists `gui_app_registry`, and keyboard input routed to whichever
window is frontmost. Rendering is whole-screen every time rather than
tracking dirty rectangles -- much simpler to get right with overlapping
movable windows. It draws into an off-screen buffer and flips finished
frames with `gfx_present()`, so the repaint isn't visible as flicker --
if you add drawing code, do it in `on_draw` and let the window manager
handle presenting; don't call `gfx_present()` yourself.

There's no process isolation here either -- a GUI app's callbacks run in
the kernel's own context, same as everything else in `apps/`. What the
window manager gives you is a real event-driven API boundary (an app
genuinely can't tell what else is drawn on screen or steal input meant
for another window), just not memory/privilege isolation.

## Shared widgets (widgets.h/widgets.c)

`apps/widgets.h`/`widgets.c` factor out the "clickable rectangle with a
label" pattern that used to be hand-rolled separately in the window
manager's title/taskbar buttons, Calculator's button grid, and Notepad's
toolbar. Two functions, deliberately minimal:
- `int widget_hit(int x, int y, int w, int h, int px, int py)` -- a plain
  bounds check, used for click hit-testing.
- `void widget_button(int x, int y, int w, int h, const char *label, uint32_t bg, uint32_t fg)`
  -- fills the rect with `bg`; if `label` is non-NULL, centers it in `fg`.
  Pass `label = NULL` for icon-only buttons (like the window manager's
  minimize/maximize/close) and draw the icon on top yourself afterward.

This is *not* the start of a general widget toolkit -- no focus
management, no layout engine, no text fields or scrollbars. Add the next
primitive here only once a second real caller needs it, the same
reasoning that produced these two in the first place (three independent
copies of the same button logic was the signal).

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

See `kernel/include/kapi.h` for the exact list -- it just aggregates the
driver headers apps are allowed to use (console output, keyboard, mouse,
timer/RTC, filesystem, graphics primitives, `system_reboot()`). If you're
tempted to `#include` something from `kernel/drivers` or `kernel/core`
directly, or to call `inb`/`outb` yourself, stop -- add the capability to
a driver and expose it through `kapi.h` instead. That's what keeps apps
decoupled from how the hardware side is implemented.

GUI apps additionally use `wm/wm.h` for the window-manager helpers
(`window_content_x/y/w/h`, `window_set_state`/`window_get_state`,
`window_invalidate`) -- that's the GUI-specific equivalent of `kapi.h`.
Apps that draw buttons can also use `widgets.h` (see "Shared widgets"
above), and any app can use `theme.h` (see "Shared theme colors" above)
for the handful of named colors -- both are peer-level apps-internal
headers, not part of `kapi.h`.

## GUI in user space (experimental, separate from everything above)

Everything above -- `wm.c`, `notepad.c`, `about.c`, all of it -- runs in
kernel space at ring 0. `kernel/core/gui_test.c` (shell command
`guitest`) is a first, deliberately narrow step toward something
different: a genuinely isolated ring-3 process drawing directly to the
real screen and reading real input, via two new syscalls
(`SYS_GUI_INIT` maps the linear framebuffer straight into the process's
own address space; `SYS_GUI_POLL_KEY` is a non-blocking keyboard read).
`userland/gui_test.c` is the process -- it fills the screen with a
color and cycles it on each keypress, entirely from ring 3, with no
kernel-space drawing code involved once it's running.

**This is not the window manager moved to user space.** It's *modal*:
since there's no scheduler, the ring-3 process has the real screen
entirely to itself while it runs, the same way `ring3test`/`elftest`/
`syscalltest`/`writetest` each run one process at a time with nothing
else happening concurrently. It doesn't create a window inside `wm.c`,
doesn't coexist with Notepad or About, and can't be dragged, minimized,
or otherwise treated as a window -- there's no `struct window` involved
at all. `wm.c` doesn't know this exists.

Turning this into an actual user-space GUI -- Notepad and About running
as real, isolated ring-3 processes that are *also* proper windows inside
`wm.c` -- needs, roughly:
- A scheduler, so a GUI process can run concurrently with the
  kernel-space compositor's own event loop instead of blocking it
- A real windowing protocol instead of "hand the whole screen to one
  process": syscalls for creating a window (getting back a private
  pixel buffer for just that window's content area, not the whole
  screen), submitting a redraw, and receiving input events scoped to
  that window
- Porting `notepad.c`/`about.c` (or new equivalents) to be freestanding
  userland programs using that protocol, the way `gui_test.c` uses
  `SYS_GUI_INIT`/`SYS_GUI_POLL_KEY` now

`gui_test.c` proves the foundational piece -- a ring-3 process can
genuinely own real pixels and real input -- without yet solving the
concurrency and protocol design that a real multi-window user-space GUI
needs.

