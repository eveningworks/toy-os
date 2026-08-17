#ifndef WM_LOG_H
#define WM_LOG_H

// The ring-3 WM's own diagnostics.
//
// NOT a compatibility layer for the kernel's klog: the kernel-side WM
// wrote to the kernel log because it WAS the kernel, and a
// ring-3 program's diagnostics go to **stderr** (`sys_eprint`), which
// the kernel routes into the kernel log and `dmesg`. That is the one
// channel a test can read -- a windowed client's stdout goes nowhere
// useful, and a spawned process's goes into its parent's pipe. See
// CLAUDE.md.
//
// It exists at all only because the variadic form has no one-line
// equivalent: `sys_eprint()` takes a finished string, so every
// formatted line would otherwise be a `snprintf` into a local buffer
// followed by a call, written out thirty-four times. The plain form is
// `sys_eprint()` directly and this header does not wrap it.
//
// **The output grammar is load-bearing.** All 22 GUI test tools read
// `wm: ...` lines out of the kernel log -- the slow-frame watchdog's
// `wm: SLOW FRAME`, the damage verifier's `wm: DAMAGE BUG`, the desktop
// entry counts. Those strings must survive the migration BYTE FOR BYTE
// or the suite goes red for a reason that has nothing to do with what
// broke. Keep the "wm: " prefix and the existing wording.

// Formats and writes one line to stderr. Truncates rather than growing:
// a diagnostic is not worth a heap allocation, and a line long enough to
// overflow this is one nobody was going to read anyway.
void wm_logf(const char *fmt, ...);

#endif
