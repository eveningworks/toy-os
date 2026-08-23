#ifndef WM_TASKBAR_H
#define WM_TASKBAR_H

// The taskbar strip's LAYOUT, computed in one place and read by both
// wm_render.c (drawing the buttons) and wm_input.c (hit-testing them).
//
// It used to be neither: `win_btn_w()` returned a constant and each of
// the three call sites walked the windows itself, marching `bx` right by
// a fixed step. With enough windows open the last buttons ran off the
// screen edge and under the clock -- the reported bug -- and any fix
// applied to one of the three walks would have silently disagreed with
// the other two. A layout a caller cannot recompute differently is the
// point of this file.
//
// WHAT IT DOES, and it is Windows' behaviour rather than an invention:
// buttons take their natural width while they fit, SHRINK toward a
// font-derived floor as more windows open, and once even the floor will
// not fit, windows of the same application COLLAPSE into one button
// carrying a count. Clicking a collapsed button opens a list of its
// windows (Windows' jump list; KDE Plasma's grouped-task popup). The
// deliberate difference from Windows is what happens past that: it never
// runs out, because it scrolls. This drops the buttons that do not fit
// and says so through `taskbar_hidden()` rather than drawing off-screen,
// which is the failure this file exists to end. A second row is the
// obvious next step and is deliberately not taken -- `taskbar_h` is a
// constant that the desktop icon area, the Start menu's anchor, the
// context-menu clamp and every damage rect all derive from.
//
// GROUPING KEY: `struct window.app_id` -- the client's own name for what
// its window IS ("notepad"), which is why every uapp now sets one. A
// window with no app_id (a kernel-space app window, or a client that
// declared none) groups by its client pid instead, so it can never be
// merged with an unrelated window that also said nothing.

#include "wm.h"

// One button on the strip. `first` is the index into windows[] of the
// group's frontmost member -- the one a plain click acts on -- and
// `count` is how many windows the button stands for (1 for an
// ungrouped button).
struct taskbar_button {
    // The icon NAME to draw in this button, or NULL. Borrowed from the
    // app registry, which outlives one frame's layout.
    const char *icon;
    int x, w;
    int first;
    int count;
    char label[32];
};

// Fills `out` with the strip's current buttons, left to right, and
// returns how many were written. Never writes more than `max`, and
// never places a button that would cross into the tray.
//
// RECOMPUTED ON EVERY CALL rather than cached: it depends on the window
// list, the live font metrics and the tray's width, and a cache would be
// a fourth thing that can disagree with the other three.
int taskbar_layout(struct taskbar_button *out, int max);

// The icon column inside a taskbar button: its edge length in pixels,
// and the icon NAME for the window at index `win` (or NULL when that
// window's app has no artwork, or is not a launcher's app at all).
//
// BOTH LIVE HERE because two callers must agree: wm_render.c draws the
// icon and this file's make_label() has to reserve the same width, or a
// label is truncated for a column that is not there -- or worse, runs
// under one that is. The same "one geometry calculation per widget"
// rule uui_scrollbar.h states.
int taskbar_icon_size(void);
const char *taskbar_icon_name(int win);

// How many windows the last taskbar_layout() could not place at all.
// Nonzero only when even collapsed, floor-width buttons overflow the
// strip. Read by the debug console so a test can assert the strip
// stopped overflowing rather than assert on pixels.
int taskbar_hidden(void);

// Handles a left click at (mx, my) inside the taskbar's window-button
// area. Returns 1 if a button claimed it. A grouped button opens its
// window list; an ungrouped one raises/minimizes as it always did.
int taskbar_handle_click(int mx, int my);

// Same, for a right click: opens the per-window context menu, or the
// group list for a collapsed button.
int taskbar_handle_right_click(int mx, int my);

#endif
