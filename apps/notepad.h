#ifndef NOTEPAD_H
#define NOTEPAD_H

struct window;

// Computes Notepad's content-area size in pixels from the current font
// (see gui_apps.h's default_size).
void notepad_default_size(int *w, int *h);
void notepad_open(struct window *win);
void notepad_draw(struct window *win);
void notepad_key(struct window *win, int key);
void notepad_click(struct window *win, int cx, int cy);

#endif
