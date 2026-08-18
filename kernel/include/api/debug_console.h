#ifndef DEBUG_CONSOLE_H
#define DEBUG_CONSOLE_H

// A tiny interactive command console over the serial port (COM1),
// independent of the VGA/keyboard physical shell and the GUI's
// keyboard focus -- so kernel state (memory, the filesystem, PCI
// devices) is inspectable over a second connection (e.g. QEMU's
// `-serial pty`/`-serial tcp:...` or a real COM1 cable) even while the
// screen is showing the GUI desktop, a ring-3 process is running, or
// the physical shell's own prompt is otherwise occupied. See
// docs/decisions.md for the full design writeup, including the honest
// limitation on when this actually gets polled.
//
// It WAS read-only inspection only (`meminfo`, `lsfs`, `lsdev`, `help`).
// It isn't any more: `sh <command>` runs any shell command through the
// real shell_dispatch() and sends the output back down the wire, and
// `ktest` runs the in-kernel test suite. See docs/decisions.md for why
// that trade was made -- in short, verifying kernel behaviour from a
// host script previously meant emulating keystrokes over QMP and
// reading the answer out of a screenshot, which is layout-dependent,
// drops keys, and can't be asserted on.
//
// A few commands are still refused (`gui`, `ring3test`, `schedtest`,
// `edit`/`nano`) because they take over the screen, never return, or
// need keyboard input this console can't deliver -- see
// debug_console.c's DBG_BLOCKED_CMDS, which mirrors apps/terminal.c's
// list for the same reasons.

void debug_console_init(void);

// Non-blocking -- drains whatever's arrived on COM1 since the last
// call (via serial_try_getc()), echoing/buffering/dispatching as
// needed, and returns immediately either way. Meant to be called from
// an existing idle-wait loop that already wakes on every interrupt
// (see keyboard_getchar()'s hlt loop and userland/wm/wm.c's own event
// loop, the two call sites this is actually wired into) -- never spins
// or blocks waiting for more input itself.
void debug_console_poll(void);

#endif
