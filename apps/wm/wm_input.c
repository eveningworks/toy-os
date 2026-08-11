// Mouse click handling and the per-tick drag/resize update -- called
// from wm.c's wm_run() loop. See wm_internal.h for the shared state
// this reads and mutates (windows[], dragging/resizing, etc.) and
// wm.c's top comment for why that sharing is fine here.
#include "wm_internal.h"
#include "widgets.h"
#include "kapi.h"

// How long a clicked Start menu row keeps showing its flash before the
// menu actually closes -- pit_ticks() increments 100/sec (see wm.c's
// once-a-second redraw tick), so 10 ticks is ~100ms: long enough to
// register as a deliberate flash, short enough not to feel like the
// click is laggy.
#define START_MENU_FLASH_TICKS 10

int wm_find_resize_zone(int mx, int my, int *out_right, int *out_bottom) {
    for (int i = window_count - 1; i >= 0; i--) {
        struct window *w = &windows[i];
        if (w->state == WIN_MINIMIZED) continue;
        if (!widget_hit(w->x, w->y, w->w, w->h, mx, my)) continue;

        if (w->state == WIN_MAXIMIZED) return -1; // maximized windows aren't resizable
        if (!(w->app && w->app->resizable)) return -1; // fixed-size app -- see gui_apps.h
        if (my < w->y + WM_TITLEBAR_H) return -1; // over the title bar, not the resize border

        int on_right = (mx >= w->x + w->w - RESIZE_MARGIN);
        int on_bottom = (my >= w->y + w->h - RESIZE_MARGIN);
        if (!on_right && !on_bottom) return -1;

        *out_right = on_right;
        *out_bottom = on_bottom;
        return i;
    }
    return -1;
}

void wm_handle_left_click(int mx, int my) {
    if (start_menu_open) {
        int item_h = gfx_char_h() + 6;
        int menu_w = start_menu_w();
        int menu_x = 4;
        int total_items = gui_app_registry_count + wm_system_action_count;
        int menu_y = (screen_h - taskbar_h) - item_h * total_items;
        if (widget_hit(menu_x, menu_y, menu_w, item_h * total_items, mx, my)) {
            int idx = (my - menu_y) / item_h;
            if (idx >= 0 && idx < gui_app_registry_count) {
                open_app(&gui_app_registry[idx]);
            } else if (idx >= gui_app_registry_count && idx < total_items) {
                wm_system_actions[idx - gui_app_registry_count].on_select();
            }
            // The row's action already ran above (same as before) --
            // only closing the menu is deferred, so the click gets a
            // brief visible flash instead of vanishing in the same
            // frame it landed. wm_update_start_menu_flash() (wm.c's
            // loop) closes the menu once the deadline passes.
            start_menu_flash_index = idx;
            start_menu_flash_until = pit_ticks() + START_MENU_FLASH_TICKS;
        } else {
            // Clicked elsewhere while the menu was open (the desktop, a
            // window) -- no row was selected, so there's nothing to
            // flash; close immediately, same as before.
            start_menu_open = 0;
        }
        redraw_pending = 1;
        return;
    }

    if (my >= screen_h - taskbar_h) {
        int ty = screen_h - taskbar_h;
        int sbw = start_btn_w(), wbw = win_btn_w();
        if (widget_hit(4, ty, sbw, taskbar_h, mx, my)) {
            start_menu_open = 1;
            redraw_pending = 1;
            return;
        }
        int bx = 4 + sbw + 8;
        for (int i = 0; i < window_count; i++) {
            if (widget_hit(bx, ty, wbw, taskbar_h, mx, my)) {
                if (windows[i].state == WIN_MINIMIZED) {
                    windows[i].state = WIN_NORMAL;
                    bring_to_front(i);
                } else if (i == window_count - 1) {
                    windows[i].state = WIN_MINIMIZED;
                } else {
                    bring_to_front(i);
                }
                redraw_pending = 1;
                return;
            }
            bx += wbw + 4;
        }
        return;
    }

    {
        int on_right = 0, on_bottom = 0;
        int ri = wm_find_resize_zone(mx, my, &on_right, &on_bottom);
        if (ri >= 0) {
            bring_to_front(ri);
            dragging = -1;
            resizing = window_count - 1;
            resize_right = on_right;
            resize_bottom = on_bottom;
            resize_start_mx = mx;
            resize_start_my = my;
            resize_start_w = windows[resizing].w;
            resize_start_h = windows[resizing].h;
            redraw_pending = 1;
            return;
        }
    }

    for (int i = window_count - 1; i >= 0; i--) {
        struct window *w = &windows[i];
        if (w->state == WIN_MINIMIZED) continue;
        if (!widget_hit(w->x, w->y, w->w, w->h, mx, my)) continue;

        if (my < w->y + WM_TITLEBAR_H) {
            struct btn_rects r = title_buttons(w);
            if (widget_hit(r.min_x, r.y, r.size, r.size, mx, my)) {
                w->state = WIN_MINIMIZED;
                redraw_pending = 1;
                return;
            }
            if (widget_hit(r.max_x, r.y, r.size, r.size, mx, my)) {
                // Fixed-size apps (Calculator -- see gui_apps.h) get a
                // disabled maximize button: focus the window like any
                // other click on it, but don't touch its geometry.
                if (w->app && w->app->resizable) {
                    if (w->state == WIN_MAXIMIZED) {
                        w->x = w->saved_x; w->y = w->saved_y;
                        w->w = w->saved_w; w->h = w->saved_h;
                        w->state = WIN_NORMAL;
                    } else {
                        w->saved_x = w->x; w->saved_y = w->y;
                        w->saved_w = w->w; w->saved_h = w->h;
                        w->x = 0; w->y = 0;
                        w->w = screen_w; w->h = screen_h - taskbar_h;
                        w->state = WIN_MAXIMIZED;
                    }
                }
                bring_to_front(i);
                redraw_pending = 1;
                return;
            }
            if (widget_hit(r.close_x, r.y, r.size, r.size, mx, my)) {
                close_window(i);
                return;
            }
            if (w->state != WIN_MAXIMIZED) {
                bring_to_front(i);
                resizing = -1;
                dragging = window_count - 1;
                drag_off_x = mx - windows[dragging].x;
                drag_off_y = my - windows[dragging].y;
            } else {
                bring_to_front(i);
            }
            redraw_pending = 1;
            return;
        }

        bring_to_front(i);
        int fi = window_count - 1;
        int ccx = mx - window_content_x(&windows[fi]);
        int ccy = my - window_content_y(&windows[fi]);
        int claimed_drag = 0;
        if (windows[fi].app && windows[fi].app->on_drag_start) {
            claimed_drag = windows[fi].app->on_drag_start(&windows[fi], ccx, ccy);
        }
        if (claimed_drag) {
            content_dragging = fi;
        } else {
            if (windows[fi].app && windows[fi].app->on_click) {
                windows[fi].app->on_click(&windows[fi], ccx, ccy);
            }
            // Independent of on_click above -- a press-feedback app
            // (e.g. calculator.c) wants both: on_click to actually act,
            // on_press to show which button is currently held. Fired
            // on this same button-down tick too, not just subsequent
            // ones, so the pressed look appears immediately.
            if (windows[fi].app && windows[fi].app->on_press) {
                content_pressed = fi;
                windows[fi].app->on_press(&windows[fi], ccx, ccy);
            }
        }
        redraw_pending = 1;
        return;
    }
    // clicked empty desktop -- nothing to do
}

void wm_update_start_menu_flash(void) {
    if (start_menu_flash_index < 0) return;
    if (pit_ticks() >= start_menu_flash_until) {
        start_menu_flash_index = -1;
        start_menu_open = 0;
        redraw_pending = 1;
    }
}

void wm_update_drag_resize(int mx, int my, uint8_t buttons) {
    if (dragging >= 0) {
        if (buttons & 0x1) {
            struct window *w = &windows[dragging];
            w->x = mx - drag_off_x;
            w->y = my - drag_off_y;
            if (w->x < 0) w->x = 0;
            if (w->y < 0) w->y = 0;
            if (w->x + w->w > screen_w) w->x = screen_w - w->w;
            if (w->y + w->h > screen_h - taskbar_h) w->y = screen_h - taskbar_h - w->h;
            redraw_pending = 1;
        } else {
            dragging = -1;
        }
    }

    if (resizing >= 0) {
        if (buttons & 0x1) {
            struct window *w = &windows[resizing];
            int dx = mx - resize_start_mx;
            int dy = my - resize_start_my;
            if (resize_right) {
                int neww = resize_start_w + dx;
                if (neww < MIN_CONTENT_W + 2) neww = MIN_CONTENT_W + 2;
                if (w->x + neww > screen_w) neww = screen_w - w->x;
                w->w = neww;
            }
            if (resize_bottom) {
                int newh = resize_start_h + dy;
                int min_h = MIN_CONTENT_H + WM_TITLEBAR_H + 2;
                if (newh < min_h) newh = min_h;
                if (w->y + newh > screen_h - taskbar_h) newh = screen_h - taskbar_h - w->y;
                w->h = newh;
            }
            redraw_pending = 1;
        } else {
            resizing = -1;
        }
    }

    if (content_dragging >= 0) {
        struct window *w = &windows[content_dragging];
        if (buttons & 0x1) {
            if (w->app && w->app->on_drag) {
                int ccx = mx - window_content_x(w);
                int ccy = my - window_content_y(w);
                w->app->on_drag(w, ccx, ccy);
            }
            redraw_pending = 1;
        } else {
            content_dragging = -1;
        }
    }

    if (content_pressed >= 0) {
        struct window *w = &windows[content_pressed];
        if (buttons & 0x1) {
            // Every tick, not just on change -- on_press decides for
            // itself whether the "hot" button moved (e.g. the mouse
            // slid onto a different button, or off all of them) and
            // returns 1 only when that changed, so holding still over
            // the same button doesn't force a redraw every single tick.
            if (w->app && w->app->on_press) {
                int ccx = mx - window_content_x(w);
                int ccy = my - window_content_y(w);
                if (w->app->on_press(w, ccx, ccy)) redraw_pending = 1;
            }
        } else {
            if (w->app && w->app->on_release) w->app->on_release(w);
            content_pressed = -1;
            redraw_pending = 1;
        }
    }
}
