#ifndef UIDEMO_H
#define UIDEMO_H

struct window;

// UI Demo -- a reference app whose entire purpose is to be TESTED
// AGAINST. Every widget in apps/ui/ appears exactly once, at a
// documented, deterministic position, and every interaction is logged
// to the kernel log as one parseable line. See uidemo.c's top comment
// for the layout table and the log-line grammar.
void uidemo_default_size(int *w, int *h);
void uidemo_open(struct window *win);
void uidemo_draw(struct window *win);
int  uidemo_press(struct window *win, int cx, int cy);
void uidemo_release(struct window *win);
int  uidemo_hover(struct window *win, int cx, int cy);
void uidemo_click(struct window *win, int cx, int cy);
void uidemo_key(struct window *win, int key);

// Scrolling: the wheel, plus thumb dragging via on_drag_start/on_drag.
// Track clicks page from uidemo_click(), the same split notepad.c uses.
void uidemo_wheel(struct window *win, int delta);
int  uidemo_drag_start(struct window *win, int cx, int cy);
void uidemo_drag(struct window *win, int cx, int cy);

#endif
