#ifndef CALCULATOR_H
#define CALCULATOR_H

struct window;

// calculator_open/draw/key/click: the gui_app callbacks (see
// gui_apps.h/gui_app_registry) that adapt calc_engine.h's pure
// arithmetic core to a real on-screen window -- button layout, click
// hit-testing, and keyboard shortcuts live here; none of the actual math
// does (see apps/calc_engine.h for that, and for how to add a new
// operator or button).
// Computes Calculator's content-area size in pixels from the current
// font (see gui_apps.h's default_size).
void calculator_default_size(int *w, int *h);
void calculator_open(struct window *win);

// Releases what calculator_open() kzalloc'd for this window -- see
// gui_apps.h's `multi_instance`/on_close and calculator.c's top comment
// for why Calculator's state can't be a single static struct anymore.
void calculator_close(struct window *win);

void calculator_draw(struct window *win);
void calculator_key(struct window *win, int key);

// on_press/on_release (gui_apps.h) -- the press/hover visual feedback
// AND where a button actually acts. There's deliberately no on_click
// here: that fires on button-DOWN (see gui_apps.h), which made every
// key uncancellable until calculator_release() took the commit over.
int calculator_press(struct window *win, int cx, int cy);
void calculator_release(struct window *win);

// on_hover (gui_apps.h) -- lights the button under the cursor before
// it's clicked. Returns 1 only when which button that is changed.
int calculator_hover(struct window *win, int cx, int cy);

#endif
