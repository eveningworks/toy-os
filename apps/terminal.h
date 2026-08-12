#ifndef TERMINAL_H
#define TERMINAL_H

struct window; // full definition in wm/wm.h

// A GUI terminal-emulator app -- runs the real shell dispatcher
// (shell_dispatch(), see shell.h) inside a window, using a
// text_scrollback widget (widgets.h) for display and a struct vga_sink
// (vga.h) to redirect the shell's console output into it. This is the
// payoff of the three prerequisite phases: no duplicated command
// handlers, no separate "GUI shell" implementation to keep in sync with
// the real one.
//
// See apps/GUI_APPS.md for the gui_app callback contract these
// implement, and terminal.c's top comment for what's deliberately NOT
// supported (a short list of commands that don't return or draw
// straight to the physical screen, blocked with an explanatory message
// instead of being handed to shell_dispatch()).
void terminal_default_size(int *w, int *h);
void terminal_open(struct window *win);
void terminal_draw(struct window *win);
void terminal_key(struct window *win, int key);

// Handles clicks on the scrollbar's empty track (page up/down) -- see
// gui_apps.h's on_click. Clicks on the thumb itself never reach this;
// they're claimed by terminal_drag_start() below instead.
void terminal_click(struct window *win, int cx, int cy);

// Scrollbar thumb dragging -- see gui_apps.h's on_drag_start/on_drag
// contract and widgets.h's widget_scrollbar_* functions.
int terminal_drag_start(struct window *win, int cx, int cy);
void terminal_drag(struct window *win, int cx, int cy);

// Mouse scroll wheel -- see gui_apps.h's on_wheel contract and
// mouse.h's mouse_get_wheel_delta().
void terminal_wheel(struct window *win, int delta);

// Called once an async `ls`/`run` started via wm.h's
// window_start_process() exits -- see gui_apps.h's on_process_exit
// contract (Milestone 1 phase 4b, docs/roadmap.md).
void terminal_process_exit(struct window *win, int exit_code);

#endif
