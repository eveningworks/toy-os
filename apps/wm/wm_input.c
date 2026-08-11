// Mouse click handling and the per-tick drag/resize update -- called
// from wm.c's wm_run() loop. See wm_internal.h for the shared state
// this reads and mutates (windows[], dragging/resizing, etc.) and
// wm.c's top comment for why that sharing is fine here.
#include "wm_internal.h"
#include "start_menu.h"
#include "context_menu.h"
#include "desktop.h"
#include "ui/ui.h"
#include "kapi.h"

// Which window (if any) has a title-bar button under (mx, my), and
// which one -- mirrors the same top-to-bottom z-order search
// wm_handle_left_click() already does when hit-testing a window body,
// kept deliberately in sync with it (both walk windows[] back to front
// and stop at the first window whose bounding box contains the point).
// Returns the window index with *out_kind set to 0/1/2
// (minimize/maximize/close) on a button hit; returns -1 (out_kind
// untouched) if the point isn't over any window's title-bar button --
// including when it's over a window's title bar but not a button, or
// over the window body below the title bar entirely.
static int title_btn_hit_test(int mx, int my, int *out_kind) {
    for (int i = window_count - 1; i >= 0; i--) {
        struct window *w = &windows[i];
        if (w->state == WIN_MINIMIZED) continue;
        if (!widget_hit(w->x, w->y, w->w, w->h, mx, my)) continue;
        if (my >= w->y + WM_TITLEBAR_H) return -1; // topmost window here, but below its title bar
        struct btn_rects r = title_buttons(w);
        if (widget_hit(r.min_x, r.y, r.size, r.size, mx, my)) { *out_kind = 0; return i; }
        if (widget_hit(r.max_x, r.y, r.size, r.size, mx, my)) { *out_kind = 1; return i; }
        if (widget_hit(r.close_x, r.y, r.size, r.size, mx, my)) { *out_kind = 2; return i; }
        return -1; // over the title bar, but not a button
    }
    return -1;
}

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
    if (context_menu_handle_click(mx, my)) return;
    if (start_menu_handle_click(mx, my)) return;

    if (my >= screen_h - taskbar_h) {
        int ty = screen_h - taskbar_h;
        int sbw = start_btn_w(), wbw = win_btn_w();
        if (widget_hit(4, ty, sbw, taskbar_h, mx, my)) {
            start_menu_open_now();
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
            // Windows/KDE-style delayed commit: mouse-down here only
            // ARMS the button (shows a pressed visual) -- the actual
            // minimize/maximize/close only happens on release, and only
            // if the cursor's still over this same button then (see
            // wm_update_title_btn_press()). Deliberately doesn't
            // bring_to_front() here -- that still only happens as part
            // of the committed action (maximize) or not at all
            // (minimize/close), same as before this change, so a
            // press-then-drag-off-then-release cancel has no visible
            // side effect at all, not even a restack.
            if (widget_hit(r.min_x, r.y, r.size, r.size, mx, my)) {
                title_btn_armed_win = i;
                title_btn_armed_kind = 0;
                title_btn_pressed_active = 1;
                title_hover_win = -1;
                title_hover_kind = -1;
                redraw_pending = 1;
                return;
            }
            if (widget_hit(r.max_x, r.y, r.size, r.size, mx, my)) {
                title_btn_armed_win = i;
                title_btn_armed_kind = 1;
                title_btn_pressed_active = 1;
                title_hover_win = -1;
                title_hover_kind = -1;
                redraw_pending = 1;
                return;
            }
            if (widget_hit(r.close_x, r.y, r.size, r.size, mx, my)) {
                title_btn_armed_win = i;
                title_btn_armed_kind = 2;
                title_btn_pressed_active = 1;
                title_hover_win = -1;
                title_hover_kind = -1;
                redraw_pending = 1;
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
    // Nothing else claimed it -- the click landed on the desktop
    // background (or one of its icons). See desktop.h.
    desktop_handle_click(mx, my);
}

// ---- right-click dispatch (see context_menu.h / wm_internal.h) ----

// Small static scratch used as a context-menu item's `ctx` -- set right
// before the menu it belongs to opens, read back when a row is later
// selected. Safe as a single shared static: only one context menu is
// ever open at a time (opening a new one always closes the last), and
// nothing else in this single-threaded event loop can touch it in
// between open and select.
static int g_ctx_window_target;

static void ctx_close_window(void *ctx) { close_window(*(int *)ctx); }

static void ctx_minimize_window(void *ctx) {
    int i = *(int *)ctx;
    windows[i].state = WIN_MINIMIZED;
    redraw_pending = 1;
}

static void ctx_toggle_maximize_window(void *ctx) {
    int i = *(int *)ctx;
    if (!(windows[i].app && windows[i].app->resizable)) return; // fixed-size app -- same rule the title-bar button follows
    if (windows[i].state == WIN_MAXIMIZED) {
        windows[i].x = windows[i].saved_x; windows[i].y = windows[i].saved_y;
        windows[i].w = windows[i].saved_w; windows[i].h = windows[i].saved_h;
        windows[i].state = WIN_NORMAL;
    } else {
        windows[i].saved_x = windows[i].x; windows[i].saved_y = windows[i].y;
        windows[i].saved_w = windows[i].w; windows[i].saved_h = windows[i].h;
        windows[i].x = 0; windows[i].y = 0;
        windows[i].w = screen_w; windows[i].h = screen_h - taskbar_h;
        windows[i].state = WIN_MAXIMIZED;
    }
    redraw_pending = 1;
}

static void ctx_open_app(void *ctx) { open_app((const struct gui_app *)ctx); }

void wm_handle_right_click(int mx, int my) {
    // A right-click always resolves to at most one popup -- close
    // whatever's already open before deciding what (if anything) the
    // new click should show, so right-clicks never stack menus.
    if (start_menu_open) {
        // Route into the Start menu's own row only if the click actually
        // landed on one; this duplicates start_menu.c's small geometry()
        // formula rather than exporting an internal-only helper for the
        // sake of one caller -- revisit if a third caller ever needs it.
        int item_h = gfx_char_h() + 6;
        int menu_w = start_menu_w();
        int menu_x = 4;
        int total_items = gui_app_registry_count + wm_system_action_count;
        int menu_y = (screen_h - taskbar_h) - item_h * total_items;
        int hit_row = widget_hit(menu_x, menu_y, menu_w, item_h * total_items, mx, my)
                      ? (my - menu_y) / item_h : -1;
        start_menu_open = 0;
        redraw_pending = 1;
        if (hit_row >= 0 && hit_row < gui_app_registry_count) {
            static struct context_menu_item item[1];
            item[0].label = "Open";
            item[0].on_select = ctx_open_app;
            item[0].ctx = (void *)&gui_app_registry[hit_row];
            context_menu_open_at(mx, my, item, 1);
        }
        return;
    }

    if (context_menu_open) context_menu_close();

    if (my >= screen_h - taskbar_h) {
        int ty = screen_h - taskbar_h;
        int sbw = start_btn_w(), wbw = win_btn_w();
        int bx = 4 + sbw + 8;
        for (int i = 0; i < window_count; i++) {
            if (widget_hit(bx, ty, wbw, taskbar_h, mx, my)) {
                g_ctx_window_target = i;
                static struct context_menu_item item[1];
                item[0].label = "Close window";
                item[0].on_select = ctx_close_window;
                item[0].ctx = &g_ctx_window_target;
                context_menu_open_at(mx, my, item, 1);
                return;
            }
            bx += wbw + 4;
        }
        return; // taskbar area, but not over an app button (or the Start button -- no menu there)
    }

    for (int i = window_count - 1; i >= 0; i--) {
        struct window *w = &windows[i];
        if (w->state == WIN_MINIMIZED) continue;
        if (!widget_hit(w->x, w->y, w->w, w->h, mx, my)) continue;

        // One menu for the whole window (title bar OR content area) --
        // mirrors the title-bar buttons' actions rather than requiring
        // the right-click to land exactly on one of those small
        // buttons, which is the whole point of offering it as a menu.
        g_ctx_window_target = i;
        static struct context_menu_item items[3];
        int n = 0;
        items[n].label = "Minimize"; items[n].on_select = ctx_minimize_window; items[n].ctx = &g_ctx_window_target; n++;
        if (w->app && w->app->resizable) {
            items[n].label = (w->state == WIN_MAXIMIZED) ? "Restore" : "Maximize";
            items[n].on_select = ctx_toggle_maximize_window; items[n].ctx = &g_ctx_window_target; n++;
        }
        items[n].label = "Close"; items[n].on_select = ctx_close_window; items[n].ctx = &g_ctx_window_target; n++;
        context_menu_open_at(mx, my, items, n);
        return;
    }

    // Nothing else claimed it -- the desktop background (see desktop.h;
    // it doesn't distinguish an icon from empty space this round, see
    // its own top comment).
    desktop_handle_right_click(mx, my);
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

// Drives title_btn_pressed_active while a title-bar button is armed
// (title_btn_armed_win >= 0), and fires its action on release -- see
// wm_internal.h's comment on title_btn_armed_win for the full contract.
// Mirrors wm_update_drag_resize()'s content_pressed handling above:
// every tick while held, recompute whether the cursor is still over the
// armed button (only redraw when that actually changes); on release,
// commit the action if it's still over the button, cancel silently if
// not.
void wm_update_title_btn_press(int mx, int my, uint8_t buttons) {
    if (title_btn_armed_win < 0) return;
    struct window *w = &windows[title_btn_armed_win];
    struct btn_rects r = title_buttons(w);
    int bx = (title_btn_armed_kind == 0) ? r.min_x
           : (title_btn_armed_kind == 1) ? r.max_x
           : r.close_x;
    int now_over = widget_hit(bx, r.y, r.size, r.size, mx, my);

    if (buttons & 0x1) {
        if (now_over != title_btn_pressed_active) {
            title_btn_pressed_active = now_over;
            redraw_pending = 1;
        }
        return;
    }

    // Released -- commit if still over the button, otherwise this was a
    // press-then-drag-off cancel and nothing happens.
    if (now_over) {
        int idx = title_btn_armed_win;
        if (title_btn_armed_kind == 0) {
            windows[idx].state = WIN_MINIMIZED;
        } else if (title_btn_armed_kind == 1) {
            // Fixed-size apps (Calculator -- see gui_apps.h) get a
            // disabled maximize button: focus the window like any other
            // click on it, but don't touch its geometry.
            if (windows[idx].app && windows[idx].app->resizable) {
                if (windows[idx].state == WIN_MAXIMIZED) {
                    windows[idx].x = windows[idx].saved_x; windows[idx].y = windows[idx].saved_y;
                    windows[idx].w = windows[idx].saved_w; windows[idx].h = windows[idx].saved_h;
                    windows[idx].state = WIN_NORMAL;
                } else {
                    windows[idx].saved_x = windows[idx].x; windows[idx].saved_y = windows[idx].y;
                    windows[idx].saved_w = windows[idx].w; windows[idx].saved_h = windows[idx].h;
                    windows[idx].x = 0; windows[idx].y = 0;
                    windows[idx].w = screen_w; windows[idx].h = screen_h - taskbar_h;
                    windows[idx].state = WIN_MAXIMIZED;
                }
            }
            bring_to_front(idx);
        } else {
            close_window(idx); // shifts windows[] -- nothing below may touch windows[idx] again
        }
    }

    title_btn_armed_win = -1;
    title_btn_armed_kind = -1;
    title_btn_pressed_active = 0;
    redraw_pending = 1;
}

// Recomputes title_hover_win/kind from the live mouse position -- see
// wm_internal.h's comment on title_hover_win. A no-op while a button's
// armed (the press visual owns the drawing then, not hover).
void wm_update_title_hover(int mx, int my) {
    if (title_btn_armed_win >= 0) return;
    int kind = -1;
    int win = title_btn_hit_test(mx, my, &kind);
    if (win != title_hover_win || kind != title_hover_kind) {
        title_hover_win = win;
        title_hover_kind = kind;
        redraw_pending = 1;
    }
}
