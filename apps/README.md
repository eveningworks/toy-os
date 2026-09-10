# apps/

Everything in here is a "program": one C file, one entry point, registered
in `apps.c`. These run in KERNEL SPACE -- one address space, no separate
memory or privilege level -- so this is a *source-code* boundary, not a
security one. It exists so you never have to read or touch the kernel to
add something new.

**This is the small half of the system now.** Real programs are ring-3
processes under `userland/`, with their own address spaces, and that is
where anything new should go unless it genuinely has to run inside the
kernel. What is left here is what does: the shell (it is what the kernel
starts), the `edit` command, and the one-line launcher that spawns the
desktop.

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

ONE kind now: console apps, registered in `apps.c` and launched via the
shell's `run` command or a dedicated shortcut. They take over the whole
screen and run their own blocking loop. (There used to be a second kind
-- event-driven GUI apps the window manager called back. They are all
ring-3 processes now; see the next section.)
- **shell** (`shell.c`) -- the command-line shell, also what the kernel
  launches first (see `apps_start()` in `apps.c`). Tab completion is
  shared with the ring-3 Terminal via `completion.c`/`completion.h`, which
  generates candidates only -- it does no input handling and no drawing,
  because the two shells have completely separate input loops and only
  the candidate logic is genuinely common. See `completion.h`'s top
  comment. *Line editing*, by contrast, IS shared and lives outside
  `apps/` entirely: `kernel/lib/klineedit.c` owns the buffer, cursor,
  kill ring, undo and the whole bash keymap, and each of the THREE front
  ends only paints the result -- `repaint_line()` here, `redraw()` in
  `/bin/tosh`, and the prompt row in the ring-3 Terminal. The two ring-3
  ones got it on 2026-08-19, when klineedit.c started being compiled a
  second time into `libuapp.a`; until then each carried its own
  append-only loop. The split is deliberate and the opposite of
  completion's for a reason -- candidate generation has no state to
  keep, whereas two copies of an editor drift, and a drifted editor
  means the same keystroke doing different things in the two windows. The shell itself is five files (`shell.c` + `shell_fs.c` +
  `shell_sys.c` + `shell_path.c` + `shell_rescue.c`, sharing state
  through `shell_internal.h`); `shell_path.c` owns PATH lookup and the
  single "run this name" resolver behind both a bare typed name and
  `run`. Most everyday file commands are NOT builtins -- `cat`, `rm`,
  `touch`, `mkdir`, `mv`, `ln`, `stat`, `truncate`, `sync`, `echo` and
  `uptime` are `/bin` programs found by that resolver, and
  `shell_rescue.c` holds the kernel's own copies behind `rescue`, a
  name that cannot shadow them. See `docs/conventions/shell.md`.
- **gui** (`gui3.c`) -- starts the DESKTOP, which is a ring-3 process:
  it spawns `/bin/wm/system/toywm` and waits for it. Registered as both
  `gui` and `gui3`, the second an alias kept so notes and scripts that
  ask for the ring-3 desktop by name keep selecting what they meant.


## The GUI is not here any more (userland/wm/, userland/ui/)

`apps/wm/` -- the window manager, ~5,300 lines -- and `apps/gui_apps.c`
were DELETED on 2026-08-18, when Milestone 41 finished and the desktop
became an ordinary ring-3 process. `apps/ui/`'s widget set went with
them: the window manager was its only caller.

Where each thing lives now:

| was | is |
|---|---|
| `apps/wm/*` (event loop, chrome, compositing, the `gui` debug commands) | `userland/wm/*`, built as `/bin/wm/system/toywm` |
| `apps/gui_apps.c` (the app registry) | `userland/wm/gui_apps.c`, still reading `/usr/wm/applications/*.desktop` |
| `apps/ui/*` (buttons, primitives, focus ring, scrollbar, textbox, radio list, icon grid) | `userland/ui/uui_*` -- Toykit, which is what a client programs against |
| `apps/ui/ui_scrollback.*` and `apps/editor.*` (the `edit` command) | `/bin/edit`, over `userland/ui/utext.c` -- the model Notepad already used |
| the GUI apps themselves | `userland/gui/{system,apps,demos}/` |

**`apps/ui/` IS GONE ENTIRELY** as of 2026-08-22. The last thing in it,
`ui_scrollback.{c,h}`, survived only because the kernel's own `edit`
drew with it; `edit` is `/bin/edit` now, over the same `utext` model
Notepad uses, so **the kernel image contains no widget code at all.** A
widget belongs in `userland/ui/`, and there is no longer any such thing
as a kernel-side one.

`apps/gui3.c` is all that connects this directory to the desktop: it
spawns `/bin/wm/system/toywm` and waits for it. When the desktop exits
-- normally, killed, or faulting -- that call returns and the shell
redraws the console.

To write a GUI app, see `docs/uapp-design.md` and `userland/gui/`: an app
is a `.c` file there with a `struct uapp_desc`, and needs no Makefile
edit.


## Shared theme colors (theme.h)

`apps/theme.h` names the handful of `gfx_rgb(...)` values that had
converged identically across more than one file. It survived the GUI's
departure because `apps/completion.c` colours the shell's tab-completion
with it -- the ring-3 toolkit has its own palette in
`userland/ui/utheme.h`, and the two are deliberately separate now that
nothing draws in both places.

Add a `THEME_*` name only when a value starts repeating, not
preemptively.


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

`wm/wm.h` used to be a second, peer-level boundary here for GUI apps.
It went with the window manager: the equivalent for a ring-3 client is
`userland/ui/uapp.h` and the rest of Toykit, which is a different
contract entirely -- a client talks to the server over TWP rather than
being called back by it.

What remains peer-level in this directory is `theme.h` (above) and
`ui/ui_scrollback.h`, both apps-internal and neither part of `kapi.h`.
