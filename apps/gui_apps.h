#ifndef GUI_APPS_H
#define GUI_APPS_H

struct window; // full definition in wm.h

// A GUI app is launched into its own window by the window manager (see
// wm.h). Unlike apps.h apps (which take over the whole screen and run
// their own blocking loop), GUI apps are event-driven: the window
// manager keeps control of the main loop at all times and calls these
// callbacks in response to input. Only one window per app is supported --
// opening an already-open app from the Start menu just focuses/restores
// its existing window rather than creating a second one.
//
// See apps/notepad.c for the simplest possible example, or
// apps/GUI_APPS.md for a full walkthrough of adding a new one.
struct gui_app {
    const char *name; // shown in the Start menu and the title bar

    // Computes this app's initial content-area size in pixels, from
    // whatever font size is currently active (gfx_char_w()/gfx_char_h()
    // -- see gfx_set_font_size()). Called once, at window-open time, so
    // a window is always sized for the font it's opened under rather
    // than a fixed size that's either cramped at large fonts or full of
    // wasted space at small ones. Font size can only change from the
    // shell before `gui` runs (there's no live in-GUI font picker), so
    // this is the only place size needs to be computed -- no live
    // resize-on-fontsize-change plumbing needed.
    void (*default_size)(int *w, int *h);

    // Called once, the first time this app's window is opened. There's
    // no heap, so app state lives in a static struct inside the app's
    // .c file -- this is typically just window_set_state(win, &g_state).
    void (*on_open)(struct window *win);

    // Called whenever the window's content area needs (re)painting --
    // after open, after resize/restore, or after anything the app did
    // that changes what should be on screen. Use window_content_x/y/w/h()
    // to find where and how big the content area is.
    void (*on_draw)(struct window *win);

    // Called when this window has keyboard focus (it's the frontmost,
    // non-minimized window) and a key arrives. `key` is an ASCII char or
    // a KEY_* code from keyboard.h. May be NULL if the app takes no input.
    void (*on_key)(struct window *win, int key);

    // Called on a left-click inside the content area. (cx, cy) are
    // content-relative (0,0 = top-left of the content area). May be NULL.
    void (*on_click)(struct window *win, int cx, int cy);

    // 1 (the common case) if the user can drag-resize and maximize this
    // app's window; 0 to fix it at its default_size() forever -- no
    // resize grip, hovering an edge doesn't show a resize cursor, and
    // the maximize button is drawn disabled and does nothing. First
    // used by Calculator, whose button grid has no sensible way to fill
    // extra space (see calculator.c) -- a per-app flag rather than
    // something the window manager decides on its own, since whether a
    // fixed size makes sense is an app-content question, not a WM one.
    int resizable;
};

extern const struct gui_app gui_app_registry[];
extern const int gui_app_registry_count;

#endif
