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
void calculator_draw(struct window *win);
void calculator_key(struct window *win, int key);
void calculator_click(struct window *win, int cx, int cy);

// on_press/on_release (gui_apps.h) -- real press/release visual
// feedback, see calculator.c's g_calc.pressed_index.
int calculator_press(struct window *win, int cx, int cy);
void calculator_release(struct window *win);

#endif
