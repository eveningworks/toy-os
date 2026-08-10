#ifndef ABOUT_H
#define ABOUT_H

struct window;

// Computes About's content-area size in pixels from the current font
// (see gui_apps.h's default_size).
void about_default_size(int *w, int *h);
void about_open(struct window *win);
void about_draw(struct window *win);

#endif
