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

// --- where a `gui` command's output goes ------------------------------
//
// Milestone 41, stage 3. These used to write straight to sys_eprint(),
// i.e. to whatever the serial debug console was connected to, which made
// the reply unavailable to anything but that console. It has to become a
// PAYLOAD now: the console reaches the WM over the transport (see
// kernel/win_transport.h), and a message carries bytes, not side effects
// on a serial port.
//
// A caller-supplied sink rather than a klog capture/redirect, which was
// the cheaper option and is the wrong one: a redirect is global, so
// kernel log lines emitted DURING a command -- the ELF loader's during
// `gui spawn`, the WM's during `gui open`, a damage-verify report --
// would be swallowed into the reply instead of reaching the serial log,
// silently breaking every test that reads them (DebugConsole.logs(),
// damage_bugs()). With a sink, only this file's own output is captured
// and the kernel log is untouched.
struct dbg_out {
    char *buf;
    int   cap;      // buffer size INCLUDING room for the NUL
    int   len;      // bytes written so far, never >= cap
    int   overflow; // set once something did not fit; see dbg_out_write
    int   reserve;  // bytes at the end nothing may write into; see
                     // dbg_out_reserve()
};

// Append to the sink. A write that does not fit is TRUNCATED and sets
// `overflow` -- deliberately unlike kernel/lib's formatters, which write
// nothing rather than a wrong value. The difference is what the value
// is: a number that is half-written is wrong, whereas a diagnostic
// transcript that stops early is simply shorter, and the flag is what
// makes that visible instead of silent.
void dbg_out_write(struct dbg_out *o, const char *s);
void dbg_out_printf(struct dbg_out *o, const char *fmt, ...);

// Holds `bytes` at the end of the sink back, so a caller that MUST be
// able to finish -- a JSON emitter that owes a `]}` -- can guarantee
// room for the ending before it starts the middle.
//
// It exists because a truncated transcript is merely short while a
// truncated JSON document is UNPARSEABLE: `gui windows --json` on a
// desktop with twenty-five windows overran WIN_DEBUG_REPLY_MAX and every
// tool asking for the window list got a ValueError rather than a partial
// answer. Reserve, fill until `overflow`, then release and close.
void dbg_out_reserve(struct dbg_out *o, int bytes);

// A rollback point, so an ARRAY ELEMENT is all-or-nothing.
//
// Reserving room for the ending is only half of it: dbg_out_write()
// stops mid-string when it runs out, which would leave a half-written
// `{"z":5,"tit` in front of the closing bracket -- still unparseable.
// Mark before each element, and roll back to that mark if the element
// did not fit, so what survives is a shorter VALID document rather than
// a longer broken one.
int  dbg_out_mark(const struct dbg_out *o);
void dbg_out_rollback(struct dbg_out *o, int mark);

// Handles a `gui ...` command line: `line` is everything AFTER the word
// `gui`, already trimmed (empty string for a bare `gui`). Output goes to
// `out`. Returns 1 if the subcommand was recognised, 0 if not (the
// caller prints usage).
//
// `line` is tokenised IN PLACE, so it must be writable.
int wm_debug_dispatch_out(char *line, struct dbg_out *out);

// The same, writing straight to the kernel log. For in-kernel callers
// that are already inside the WM and want the old behaviour -- the demo
// tour (apps/demo.c) is the only one. NOT the path the serial console
// takes any more: that one goes over the transport, which is the whole
// point of stage 3.
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

// ...and the KEY_MOD_* bits `gui key <c> [shift|ctrl|alt]` attached to
// it, so a test can send Shift-Tab -- which is not expressible any other
// way, since Tab has no shifted character (see api/keyboard.h).
// `out_mods` may be NULL, which makes this exactly wm_debug_next_key().
int wm_debug_next_key_mods(uint8_t *out_mods);

// ...and for the wheel: returns the next injected notch delta, or 0.
// Present because apps DO handle the wheel (a scrollback's scrollbar
// is not fully exercised without it), so leaving it out would make the
// injection set quietly incomplete -- which is how UI Demo's scrolling
// shipped untested the first time.
int wm_debug_next_wheel(void);

// Undelivered injected events of every kind, so a test can wait for its
// own input to drain rather than sleep a guessed interval -- reported by
// `gui state` as `pending`. See the function's comment in wm_debug.c for
// why a fixed sleep was the wrong shape (the WM loop's frame rate is not
// a constant, and `gui damage verify on` in particular slows it by
// roughly an order of magnitude).
int wm_debug_input_pending(void);

#endif
