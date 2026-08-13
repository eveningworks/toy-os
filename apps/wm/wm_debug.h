#ifndef WM_DEBUG_H
#define WM_DEBUG_H

#include <stdint.h>

// The `gui` command family for the serial debug console -- ask the
// window manager what it's actually doing, and drive it, without
// touching a pixel.
//
// **Why this exists.** GUI testing here meant QMP: synthesize a mouse
// move, click, screenshot, read the picture. That works, but every
// coordinate in it was derived by hand from a screenshot and then
// hardcoded -- tools/gui_flow.py carries MENU_TOP_Y = 475 and
// ITEM_H = 27 with a comment recording that they had already drifted
// once and been re-measured. Asking the kernel for the number it
// actually used deletes that whole class of problem, and turns
// "does this look right" into an assertion on real values.
//
// This is reachable while the desktop is up because
// debug_console_poll() is called from wm_run()'s idle loop (see
// apps/wm/wm.c), the same piggyback keyboard_getchar() does for the
// physical shell.
//
// **Injected input is NOT a replacement for QMP.** It enters at the WM
// loop, below the PS/2 driver, so it exercises WM and app logic but
// proves nothing about the mouse driver or the keyboard layout. Use it
// for "does this control do the right thing"; keep QMP for "does input
// arrive at all", and for anything where the answer is genuinely a
// picture.
//
// Lives in apps/wm/ rather than kernel/core/ because it reads
// wm_internal.h's window table -- WM-private state. kernel/core's
// debug_console.c only routes the word `gui` here, the same way it
// reaches apps/ for `sh` (see kernel/core/debug_console.c, and the
// Makefile's -Iapps for why a kernel file may include this at all).

// Handles a `gui ...` command line: `line` is everything AFTER the
// word `gui`, already trimmed (empty string for a bare `gui`). Writes
// its output with klog_write()/klog_printf(), so it lands on whatever
// the debug console is connected to. Returns 1 if the subcommand was
// recognised, 0 if not (the caller prints usage).
int wm_debug_dispatch(char *line);

// --- synthetic input, drained by wm_run() ---------------------------
//
// One queued (x, y, buttons) triple per call, consumed at most one per
// loop iteration so each becomes its own tick -- which is what makes a
// press and a release land on separate frames, the way the arm-on-press
// /commit-on-release contract requires.
//
// Returns 1 and fills the outs if a synthetic event was waiting, 0 if
// the loop should use the real mouse. Nothing here blocks: enqueue and
// return is the ONLY safe shape, because these commands are dispatched
// from inside wm_run() itself -- a `gui click` that waited for its own
// events to drain would be waiting on the loop it is currently
// blocking. (The same trap that made a lazy CPU-clock calibration hang
// inside a syscall; see api/cpuinfo.h.)
int wm_debug_next_input(int *out_x, int *out_y, uint8_t *out_buttons);

// Same idea for keys: returns the next injected key code, or 0 if none.
int wm_debug_next_key(void);

// ...and for the wheel: returns the next injected notch delta, or 0.
// Present because apps DO handle the wheel (a scrollback's scrollbar
// is not fully exercised without it), so leaving it out would make the
// injection set quietly incomplete -- which is how UI Demo's scrolling
// shipped untested the first time.
int wm_debug_next_wheel(void);

#endif
