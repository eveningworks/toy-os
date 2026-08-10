// The window manager: owns the screen once gui_main() hands off to it.
// Keeps a small fixed array of windows in z-order (index 0 = back,
// highest index = front/focused) and drives everything through a single
// event loop -- mouse clicks, dragging, keyboard routed to the focused
// window, and a full-screen redraw whenever something changes.
//
// This file holds the shared state, the app-facing helpers (wm.h's
// window_* functions), window lifecycle (open/close/focus), and
// wm_run()'s main loop -- the "core" a reader should start at. Mouse/
// keyboard handling is wm_input.c; all drawing is wm_render.c. See
// wm_internal.h for the private glue between the three -- none of this
// split changes behavior, it's the same single-threaded event loop as
// before, just organized into files by concern.
//
// Rendering draws the whole scene every time a window/taskbar/menu
// actually changes (see wm_render.c) rather than tracking per-window
// dirty rectangles -- much simpler to get right with overlapping movable
// windows, at the cost of being slower than a "real" compositor. It draws
// into an off-screen buffer and flips the finished frame in one pass
// (gfx_set_double_buffered / gfx_present), so the repaint isn't visible
// as flicker. gfx_present() itself only blits the sub-rectangle that
// actually changed (see its own comment in gfx.c) rather than the whole
// screen, which is what makes the other common case -- the mouse moving
// with nothing else changing -- cheap without needing real dirty-rect
// tracking of the scene: see wm_render_cursor_move() in wm_render.c.
#include "wm_internal.h"
#include "kapi.h"

struct window windows[MAX_WINDOWS];
int window_count = 0;

int screen_w, screen_h;
int taskbar_h;

int start_menu_open = 0;

int dragging = -1; // index into windows[], or -1 if not dragging
int drag_off_x, drag_off_y;

int resizing = -1; // index into windows[], or -1 if not resizing
int resize_right = 0, resize_bottom = 0;
int resize_start_mx, resize_start_my;
int resize_start_w, resize_start_h;

int content_dragging = -1; // index into windows[], or -1 -- see wm_internal.h

int redraw_pending = 1;

// ---- app-facing helpers (declared in wm.h) ----

void window_set_state(struct window *win, void *state) { win->app_state = state; }
void *window_get_state(struct window *win) { return win->app_state; }

int window_content_x(const struct window *win) { return win->x + 1; }
int window_content_y(const struct window *win) { return win->y + WM_TITLEBAR_H + 1; }
int window_content_w(const struct window *win) { return win->w - 2; }
int window_content_h(const struct window *win) { return win->h - WM_TITLEBAR_H - 2; }

void window_invalidate(struct window *win) {
    (void)win; // whole-screen redraw, so which window doesn't matter yet
    redraw_pending = 1;
}

// ---- window lifecycle ----

void bring_to_front(int idx) {
    if (idx == window_count - 1) return;
    struct window tmp = windows[idx];
    for (int i = idx; i < window_count - 1; i++) windows[i] = windows[i + 1];
    windows[window_count - 1] = tmp;
}

static int find_window_for_app(const struct gui_app *app) {
    for (int i = 0; i < window_count; i++)
        if (windows[i].app == app) return i;
    return -1;
}

void open_app(const struct gui_app *app) {
    int existing = find_window_for_app(app);
    if (existing >= 0) {
        if (windows[existing].state == WIN_MINIMIZED) windows[existing].state = WIN_NORMAL;
        bring_to_front(existing);
        redraw_pending = 1;
        return;
    }
    if (window_count >= MAX_WINDOWS) return; // no room -- silently ignore

    struct window *win = &windows[window_count];
    k_memset(win, 0, sizeof(*win));

    int cascade = (window_count % 5) * 24;
    win->x = 60 + cascade;
    win->y = 40 + cascade;
    int content_w, content_h;
    app->default_size(&content_w, &content_h);
    win->w = content_w + 2;
    win->h = content_h + WM_TITLEBAR_H + 2;
    win->state = WIN_NORMAL;
    win->app = app;
    win->open = 1;

    int i = 0;
    for (; app->name[i] && i < WIN_TITLE_MAX - 1; i++) win->title[i] = app->name[i];
    win->title[i] = '\0';

    window_count++;
    if (app->on_open) app->on_open(win);
    redraw_pending = 1;
}

void close_window(int idx) {
    for (int i = idx; i < window_count - 1; i++) windows[i] = windows[i + 1];
    window_count--;
    redraw_pending = 1;
}

// ---- main loop ----

void wm_run(void) {
    if (!gfx_init()) {
        vga_write("gui: no linear RGB framebuffer available from GRUB\n");
        return;
    }

    screen_w = gfx_width();
    screen_h = gfx_height();
    taskbar_h = WM_TITLEBAR_H;

    // Draw off-screen and flip completed frames -- without this the
    // full-screen repaint below is visible as flicker while it happens.
    // If the mode is too large to buffer we just draw directly; it still
    // works, it just flickers (see gfx_set_double_buffered).
    gfx_set_double_buffered(1);

    mouse_set_bounds(screen_w, screen_h);
    mouse_init();

    window_count = 0;
    start_menu_open = 0;
    dragging = -1;
    resizing = -1;
    content_dragging = -1;
    redraw_pending = 1;

    int mx, my;
    uint8_t buttons;
    mouse_get_state(&mx, &my, &buttons);
    int prev_mx = mx, prev_my = my;
    uint8_t prev_buttons = buttons;

    for (;;) {
        __asm__ volatile ("hlt");

        mouse_get_state(&mx, &my, &buttons);
        int mouse_moved = (mx != prev_mx || my != prev_my);

        int left_edge_down = (buttons & 0x1) && !(prev_buttons & 0x1);
        if (left_edge_down) wm_handle_left_click(mx, my);

        wm_update_drag_resize(mx, my, buttons);

        static uint64_t last_second = (uint64_t)-1;
        uint64_t ticks = pit_ticks();
        uint64_t this_second = ticks / 100;
        if (this_second != last_second) {
            last_second = this_second;
            redraw_pending = 1;
        }

        int key = keyboard_try_getchar();
        if (key == 27) { // Esc always exits the window manager
            gfx_set_double_buffered(0); // console draws straight to screen
            return;
        }

        int wheel = mouse_get_wheel_delta();

        if (key != -1 || wheel != 0) {
            int f = -1;
            for (int i = window_count - 1; i >= 0; i--) {
                if (windows[i].state != WIN_MINIMIZED) { f = i; break; }
            }
            if (f >= 0 && key != -1 && windows[f].app && windows[f].app->on_key) {
                windows[f].app->on_key(&windows[f], key);
            }
            if (f >= 0 && wheel != 0 && windows[f].app && windows[f].app->on_wheel) {
                windows[f].app->on_wheel(&windows[f], wheel);
            }
            redraw_pending = 1;
        }

        prev_mx = mx; prev_my = my; prev_buttons = buttons;

        // With double buffering there's no flicker to hide, so redraw as
        // soon as anything changes -- throttling here only added cursor lag.
        //
        // Two tiers: anything that actually changed the scene (a click,
        // a drag/resize in progress, a window opening/closing, a key or
        // wheel event delivered to an app, the once-a-second clock tick)
        // sets redraw_pending and gets a full repaint. Mouse movement
        // alone -- by far the most common event this loop sees -- takes
        // wm_render_cursor_move()'s cheap path instead: it doesn't touch
        // redraw_pending at all, so a plain full redraw still happens
        // exactly when it used to. See wm_render.c's dirty-rectangle
        // comments for why this split is worth having.
        if (redraw_pending) {
            redraw_pending = 0;
            wm_render_frame(mx, my);
        } else if (mouse_moved) {
            wm_render_cursor_move(mx, my);
        }
    }
}
