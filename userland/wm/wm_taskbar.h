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
// context-menu clamp and every damage rect all derive from -- and now
// a SETTING, `desktop.taskbar_height`, adopted through
// taskbar_poll_config() and relaid through wm_layout_changed().
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

// The icon column inside a taskbar button: its edge length in pixels.
// Two callers must agree on it -- wm_render.c draws the icon and this
// file's make_label() has to reserve the same width, or a label is
// truncated for a column that is not there, or worse runs under one
// that is. The same "one geometry calculation per widget" rule
// uui_scrollbar.h states.
//
// WHICH icon is not a taskbar question and no longer lives here: a
// title bar asks it too, so it is wm_window_icon_name() in wm.c.
int taskbar_icon_size(void);
#define TASKBAR_ICON_MAX 32

// The strip's height with `desktop.taskbar_height` unset -- the
// registry's constant (api/taskbar_config.h), NOT a font formula: the
// two rings' fonts do not share a line height.
int taskbar_default_h(void);

// HOW THE START BUTTON LOOKS: the word, the mark, or both --
// `desktop.start_button`, an enum the registry owns
// (kernel/lib/start_button_config.c). XFCE's Whisker Menu offers this
// same three-way (Icon / Title / Icon and title) and KDE's launcher the
// same choice against an icon-only default; a boolean cannot express
// `both`, which is what Windows 95 through 7 shipped.
//
// IT LIVES HERE BECAUSE THE STRIP'S GEOMETRY DEPENDS ON IT. The Start
// button's width is derived from what is inside it (start_btn_w() in
// wm_render.c), and everything else on the strip starts to the right of
// that -- so the mode has to be one answer three files agree on, the
// same rule the icon column above states.
enum start_button_mode {
    START_BUTTON_TEXT,   // "Start", as it has always been -- the DEFAULT
    START_BUTTON_ICON,   // the mark alone, KDE Plasma's default
    START_BUTTON_BOTH,   // mark then word, Windows 95's
};
enum start_button_mode taskbar_start_mode(void);

// The mark's edge length inside the Start button, or 0 when this mode
// draws no mark. Same "one geometry calculation" rule as
// taskbar_icon_size(), and the same reason: wm_render.c blits at it and
// start_btn_w() reserves it.
int start_icon_size(void);

// Re-reads `desktop.taskbar_height` and `desktop.start_button` if
// anything on the filesystem has changed. Called once per frame from wm.c, beside desktop_poll_config()
// and for the same reason -- there is no inotify here, so a generation
// counter is what says "ask again". The idle cost is one compare.
void taskbar_poll_config(void);

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
