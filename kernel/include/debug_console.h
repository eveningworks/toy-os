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
// Not a shell replacement and not trying to be one: no filesystem
// mutation commands, no process control, just read-only inspection
// (`meminfo`, `lsfs`, `lsdev`, `help`) -- see debug_console.c's own top
// comment for why the command set stops there for now.

void debug_console_init(void);

// Non-blocking -- drains whatever's arrived on COM1 since the last
// call (via serial_try_getc()), echoing/buffering/dispatching as
// needed, and returns immediately either way. Meant to be called from
// an existing idle-wait loop that already wakes on every interrupt
// (see keyboard_getchar()'s hlt loop and apps/wm/wm.c's own event
// loop, the two call sites this is actually wired into) -- never spins
// or blocks waiting for more input itself.
void debug_console_poll(void);

#endif
