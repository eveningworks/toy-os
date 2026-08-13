#ifndef CONTROL_PANEL_H
#define CONTROL_PANEL_H

#include "wm/wm.h"

// See control_panel.c for the applet-registry design.
void control_panel_default_size(int *w, int *h);
void control_panel_open(struct window *win);
void control_panel_draw(struct window *win);
int  control_panel_hover(struct window *win, int cx, int cy);
int  control_panel_press(struct window *win, int cx, int cy);
void control_panel_release(struct window *win);

#endif
