#ifndef NOTEPAD_H
#define NOTEPAD_H

#include <stdint.h> // uint32_t, for notepad_read_complete() below

struct window;

// Computes Notepad's content-area size in pixels from the current font
// (see gui_apps.h's default_size).
void notepad_default_size(int *w, int *h);
void notepad_open(struct window *win);
void notepad_draw(struct window *win);
void notepad_key(struct window *win, int key);

// Toolbar (Save/Load) clicks, and scrollbar track clicks (page up/down)
// that aren't on the thumb -- see terminal.h's terminal_click for the
// same split, which this mirrors now that Notepad shares the same
// text_scrollback + scrollbar widgets.
void notepad_click(struct window *win, int cx, int cy);

// Save/Load press-feedback -- see gui_apps.h's on_press/on_release
// contract and calculator.c's identically-shaped wrappers around the
// same ui_button_group_press()/_release() calls.
int notepad_press(struct window *win, int cx, int cy);
void notepad_release(struct window *win);

// on_hover (gui_apps.h) -- lights the toolbar button under the cursor
// before it's clicked. Returns 1 only when which button that is changed.
int notepad_hover(struct window *win, int cx, int cy);

// Scrollbar thumb dragging, and (new) click-to-position/drag-select in
// the text body -- see gui_apps.h's on_drag_start/on_drag contract,
// widgets.h's widget_scrollbar_* functions, and ui_scrollback.h's
// selection API.
int notepad_drag_start(struct window *win, int cx, int cy);
void notepad_drag(struct window *win, int cx, int cy);

// Mouse scroll wheel -- see gui_apps.h's on_wheel contract and
// mouse.h's mouse_get_wheel_delta().
void notepad_wheel(struct window *win, int delta);

// Called once Save As...'s steppable write reaches a terminal result --
// see gui_apps.h's on_write_complete contract and wm.h's
// window_start_write().
void notepad_write_complete(struct window *win, int success);

// Called once Open...'s steppable read reaches a terminal result --
// see gui_apps.h's on_read_complete contract and wm.h's
// window_start_read().
void notepad_read_complete(struct window *win, int success, uint32_t total);

#endif
