// Mouse click handling and the per-tick drag/resize update -- called
// from wm.c's wm_run() loop. See wm_internal.h for the shared state
// this reads and mutates (windows[], dragging/resizing, etc.) and
// wm.c's top comment for why that sharing is fine here.
#include "wm_internal.h"
#include "start_menu.h"
#include "context_menu.h"
#include "confirm_dialog.h"
#include "file_picker.h"
#include "desktop.h"
#include "ui/uui.h"
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
        if (!uui_hit(w->x, w->y, w->w, w->h, mx, my)) continue;
        if (my >= w->y + WM_TITLEBAR_H) return -1; // topmost window here, but below its title bar
        struct btn_rects r = title_buttons(w);
        if (uui_hit(r.min_x, r.y, r.size, r.size, mx, my)) { *out_kind = 0; return i; }
        if (uui_hit(r.max_x, r.y, r.size, r.size, mx, my)) { *out_kind = 1; return i; }
        if (uui_hit(r.close_x, r.y, r.size, r.size, mx, my)) { *out_kind = 2; return i; }
        return -1; // over the title bar, but not a button
    }
    return -1;
}

int wm_find_resize_zone(int mx, int my, int *out_right, int *out_bottom) {
    for (int i = window_count - 1; i >= 0; i--) {
        struct window *w = &windows[i];
        if (w->state == WIN_MINIMIZED) continue;
        if (!uui_hit(w->x, w->y, w->w, w->h, mx, my)) continue;

        if (w->state == WIN_MAXIMIZED) return -1; // maximized windows aren't resizable
        if (!w->resizable) return -1; // fixed-size window -- see wm.h
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
    if (confirm_dialog_handle_click(mx, my)) return; // most modal -- checked first, see confirm_dialog.h
    if (file_picker_handle_click(mx, my)) return; // also modal (an app-opened dialog, e.g. Notepad's Save As...) -- see file_picker.h
    if (context_menu_handle_click(mx, my)) return;
    if (start_menu_handle_click(mx, my)) return;

    if (my >= screen_h - taskbar_h) {
        int ty = screen_h - taskbar_h;
        int sbw = start_btn_w(), wbw = win_btn_w();
        if (uui_hit(4, ty, sbw, taskbar_h, mx, my)) {
            start_menu_open_now();
            redraw_pending = 1;
            return;
        }
        int bx = 4 + sbw + 8;
        for (int i = 0; i < window_count; i++) {
            if (uui_hit(bx, ty, wbw, taskbar_h, mx, my)) {
                if (windows[i].state == WIN_MINIMIZED) {
                    windows[i].state = WIN_NORMAL;
                    wm_ensure_reachable(i);
                    bring_to_front(i);
                } else if (wm_ensure_reachable(i)) {
                    // It was somewhere it could not be grabbed -- off an
                    // edge, or behind the taskbar. RECOVERING it is the
                    // action, taking priority over the minimize toggle
                    // below: minimizing something the user cannot see
                    // does nothing they can perceive, and this button is
                    // the only handle such a window has left.
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
        if (!uui_hit(w->x, w->y, w->w, w->h, mx, my)) continue;

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
            if (uui_hit(r.min_x, r.y, r.size, r.size, mx, my)) {
                title_btn_armed_win = i;
                title_btn_armed_kind = 0;
                title_btn_pressed_active = 1;
                title_hover_win = -1;
                title_hover_kind = -1;
                redraw_pending = 1;
                return;
            }
            if (uui_hit(r.max_x, r.y, r.size, r.size, mx, my)) {
                title_btn_armed_win = i;
                title_btn_armed_kind = 1;
                title_btn_pressed_active = 1;
                title_hover_win = -1;
                title_hover_kind = -1;
                redraw_pending = 1;
                return;
            }
            if (uui_hit(r.close_x, r.y, r.size, r.size, mx, my)) {
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
            } else if (wm_client_is_client_window(&windows[fi])) {
                // A client has no on_press callback -- it does its own
                // press logic from the message. But content_pressed is
                // what routes the subsequent drag-tracking moves and
                // the RELEASE to this window, so it still has to be
                // set, or the client would receive a MOUSE_DOWN that
                // is never followed by a MOUSE_UP and could never
                // commit a click. That is exactly the bug this line
                // fixes: keyboard input worked and mouse input did
                // not.
                content_pressed = fi;
            }
            // A ring-3 client gets the same event as a message instead
            // of a callback. It has no on_click/on_press distinction --
            // that split exists so a kernel app can paint press
            // feedback synchronously, which a client does for itself by
            // drawing into its own buffer and presenting.
            wm_client_send_mouse(&windows[fi], WIN_EV_MOUSE_DOWN, mx, my, 1);
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

// Goes through wm_request_close(), not close_window(). It used to call
// the latter, which for a ring-3 client destroyed its window without
// ever sending WIN_EV_CLOSE -- so right-click > Close skipped the
// handshake the X button had always honoured, and an app refusing to
// close was closed anyway. A WM feature that treats one window kind
// correctly and the other not; see wm_internal.h.
static void ctx_close_window(void *ctx) { wm_request_close(*(int *)ctx); }

static void ctx_minimize_window(void *ctx) {
    int i = *(int *)ctx;
    windows[i].state = WIN_MINIMIZED;
    redraw_pending = 1;
}

// Maximize, or restore a maximized window. One implementation, called
// by both the title-bar button and the context menu -- they used to hold
// a copy each, and a CLIENT window is exactly the case where two copies
// of a rule stop agreeing.
//
// The client half is the part worth reading. A kernel-space app draws
// into whatever rect the WM gives it, so setting w/h here is the whole
// operation. A CLIENT owns its own pixel buffer, so imposing a size
// produces precisely the failure abi/win_proto.h describes for the grip:
// full-screen chrome around a buffer still the old size, with undrawn
// desktop filling the difference. It did exactly that until this was
// fixed. So a client is PROPOSED the new content size, the same
// configure/ack the grip uses, and wm_client.c's on_window_resized()
// adopts w/h when the client answers.
static void wm_toggle_maximize(int i) {
    if (!windows[i].resizable) return; // same rule for both entry points
    int is_client = wm_client_is_client_window(&windows[i]);

    // Damage the rect the window is leaving, in both directions: the
    // window is about to move and resize, and nothing else repaints the
    // desktop it uncovers.
    wm_damage_rect(windows[i].x, windows[i].y, windows[i].w, windows[i].h);

    if (windows[i].state == WIN_MAXIMIZED) {
        windows[i].x = windows[i].saved_x; windows[i].y = windows[i].saved_y;
        windows[i].state = WIN_NORMAL;
        if (is_client) {
            wm_client_send_resize(&windows[i], windows[i].saved_w - 2,
                                   windows[i].saved_h - WM_TITLEBAR_H - 2);
        } else {
            windows[i].w = windows[i].saved_w; windows[i].h = windows[i].saved_h;
        }
    } else {
        windows[i].saved_x = windows[i].x; windows[i].saved_y = windows[i].y;
        windows[i].saved_w = windows[i].w; windows[i].saved_h = windows[i].h;
        windows[i].x = 0; windows[i].y = 0;
        windows[i].state = WIN_MAXIMIZED;
        if (is_client) {
            wm_client_send_resize(&windows[i], screen_w - 2,
                                   screen_h - taskbar_h - WM_TITLEBAR_H - 2);
        } else {
            windows[i].w = screen_w; windows[i].h = screen_h - taskbar_h;
        }
    }
    redraw_pending = 1;
}

static void ctx_toggle_maximize_window(void *ctx) {
    wm_toggle_maximize(*(int *)ctx);
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
        int item_h = ugfx_char_h() + 6;
        int menu_w = start_menu_w();
        int menu_x = 4;
        int total_items = gui_app_registry_count + wm_system_action_count;
        int menu_y = (screen_h - taskbar_h) - item_h * total_items;
        int hit_row = uui_hit(menu_x, menu_y, menu_w, item_h * total_items, mx, my)
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
            if (uui_hit(bx, ty, wbw, taskbar_h, mx, my)) {
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
        if (!uui_hit(w->x, w->y, w->w, w->h, mx, my)) continue;

        // One menu for the whole window (title bar OR content area) --
        // mirrors the title-bar buttons' actions rather than requiring
        // the right-click to land exactly on one of those small
        // buttons, which is the whole point of offering it as a menu.
        g_ctx_window_target = i;
        static struct context_menu_item items[3];
        int n = 0;
        items[n].label = "Minimize"; items[n].on_select = ctx_minimize_window; items[n].ctx = &g_ctx_window_target; n++;
        if (w->resizable) {
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

// Pulls a window back to where its title bar can be grabbed again.
//
// Dragging may leave a window with its title bar off the side or
// entirely BEHIND the taskbar (both deliberate -- see the clamp in
// wm_update_drag_resize()). Windows and KDE allow the same, and both
// have keyboard escapes for it (Alt+Space then Move, Win+arrow). This
// desktop has neither, so without this a window pushed under the
// taskbar would be visible only as a taskbar button and impossible to
// move ever again.
//
// Called when a taskbar button activates a window -- that button is the
// one handle such a window still has. A window that is already
// reachable is left exactly where it is, including one deliberately
// hanging off an edge: the clamps below only bite past the point where
// nothing grabbable is left.
int wm_ensure_reachable(int idx) {
    if (idx < 0 || idx >= window_count) return 0;
    struct window *w = &windows[idx];

    int keep = ugfx_char_w() * 8;
    if (keep > w->w) keep = w->w;

    int min_x = keep - w->w;
    int max_x = screen_w - keep;
    int max_y = screen_h - taskbar_h - WM_TITLEBAR_H; // clear of the taskbar

    int nx = w->x, ny = w->y;
    if (nx < min_x) nx = min_x;
    if (nx > max_x) nx = max_x;
    if (ny < 0) ny = 0;
    if (ny > max_y) ny = max_y;

    if (nx == w->x && ny == w->y) return 0; // already reachable, leave it

    wm_damage_rect(w->x, w->y, w->w, w->h); // vacated
    w->x = nx;
    w->y = ny;
    wm_damage_rect(w->x, w->y, w->w, w->h); // arrived
    return 1;
}

void wm_update_drag_resize(int mx, int my, uint8_t buttons) {
    if (dragging >= 0) {
        if (buttons & 0x1) {
            struct window *w = &windows[dragging];
            w->x = mx - drag_off_x;
            w->y = my - drag_off_y;

            // A window may hang off the left, right and bottom edges,
            // the way it can on Windows and KDE -- partially hiding a
            // window is a normal thing to want, and the old clamp
            // (fully on screen, always) made it impossible. What is
            // still enforced is that the window stays REACHABLE:
            //
            //  - enough of the title bar stays on screen to grab it
            //    back, horizontally;
            //  - the title bar never goes above the top edge, so it can
            //    never be dragged somewhere the pointer cannot follow.
            //    Windows enforces the same asymmetry, and for the same
            //    reason: every other edge can be recovered by dragging
            //    the title bar, but the title bar cannot recover
            //    itself.
            //  - dragged DOWN, the window slides UNDER the taskbar --
            //    the taskbar is drawn last, so it is already on top;
            //    what used to stop this was purely this clamp. The
            //    window stays reachable from its taskbar button even
            //    when the title bar itself is behind the taskbar, which
            //    is exactly the bargain Windows and KDE make.
            //
            // Font-derived rather than a pixel constant, per
            // docs/gui-guidelines.md -- at 14pt this is ~64px of
            // grabbable title bar.
            int keep = ugfx_char_w() * 8;
            if (keep > w->w) keep = w->w; // a very narrow window stays whole

            int min_x = keep - w->w;             // mostly off the LEFT
            int max_x = screen_w - keep;         // mostly off the RIGHT
            int max_y = screen_h - WM_TITLEBAR_H; // down BEHIND the taskbar

            if (w->x < min_x) w->x = min_x;
            if (w->x > max_x) w->x = max_x;
            if (w->y < 0) w->y = 0;
            if (w->y > max_y) w->y = max_y;
            redraw_pending = 1;
        } else {
            dragging = -1;
        }
    }

    if (resizing >= 0) {
        struct window *w = &windows[resizing];
        int is_client = wm_client_is_client_window(w);

        if (buttons & 0x1) {
            int dx = mx - resize_start_mx;
            int dy = my - resize_start_my;
            int neww = w->w, newh = w->h;
            if (resize_right) {
                neww = resize_start_w + dx;
                if (neww < MIN_CONTENT_W + 2) neww = MIN_CONTENT_W + 2;
                if (w->x + neww > screen_w) neww = screen_w - w->x;
            }
            if (resize_bottom) {
                newh = resize_start_h + dy;
                int min_h = MIN_CONTENT_H + WM_TITLEBAR_H + 2;
                if (newh < min_h) newh = min_h;
                if (w->y + newh > screen_h - taskbar_h) newh = screen_h - taskbar_h - w->y;
            }

            if (is_client) {
                // Propose only -- an outline, drawn by wm_render.c. The
                // window keeps its real size until the client answers.
                if (resize_prop_w != neww || resize_prop_h != newh) {
                    // The old outline has to be erased as well as the
                    // new one drawn, so damage both.
                    if (resize_prop_w > 0) wm_damage_rect(w->x, w->y, resize_prop_w, resize_prop_h);
                    resize_prop_w = neww;
                    resize_prop_h = newh;
                    wm_damage_rect(w->x, w->y, neww, newh);
                    redraw_pending = 1;
                }
            } else {
                w->w = neww;
                w->h = newh;
                redraw_pending = 1;
            }
        } else {
            if (is_client && resize_prop_w > 0) {
                // Released: ask. The CONTENT size, not the frame's --
                // the client knows nothing about chrome.
                wm_damage_rect(w->x, w->y, resize_prop_w, resize_prop_h);
                wm_client_send_resize(w, resize_prop_w - 2,
                                       resize_prop_h - WM_TITLEBAR_H - 2);
                redraw_pending = 1;
            }
            resize_prop_w = resize_prop_h = -1;
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
            // A client gets the drag as MOUSE_MOVE with the button
            // held, which is what lets its widgets re-hit-test and
            // "un-press" when the cursor slides off a control -- the
            // same behaviour on_press gives a kernel app.
            wm_client_send_mouse(w, WIN_EV_MOUSE_MOVE, mx, my, 1);
        } else {
            if (w->app && w->app->on_release) w->app->on_release(w);
            // The release is what COMMITS a click, for a client exactly
            // as for an app: a press dragged off its control has
            // already been cleared by the moves above, so this makes it
            // correctly do nothing (docs/gui-guidelines.md).
            wm_client_send_mouse(w, WIN_EV_MOUSE_UP, mx, my, 0);
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
    int now_over = uui_hit(bx, r.y, r.size, r.size, mx, my);

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
            // wm_toggle_maximize() enforces that itself, and is shared
            // with the context menu's Maximize/Restore item.
            wm_toggle_maximize(idx);
            bring_to_front(idx);
        } else {
            // A client's window is the CLIENT's to close: it may have
            // unsaved state, and the WM tearing it down behind the
            // process's back would leave that process drawing into a
            // buffer that is no longer on screen. So the X is a
            // request (WIN_EV_CLOSE), and the client answers with
            // WIN_REQ_DESTROY -- the same polite-close handshake every
            // real windowing system uses.
            //
            // A client that ignores it keeps its window, which is the
            // honest consequence of the handshake. Force-closing an
            // unresponsive client needs a "not responding" timeout and
            // a way to kill the process, neither of which exists yet --
            // see docs/roadmap.md's Milestone 41.
            //
            // That whole decision now lives in wm_request_close() rather
            // than here, because the X button was not the only way a
            // user closes a window and the other ways were not honouring
            // it. May shift windows[] -- nothing below may touch
            // windows[idx] again.
            wm_request_close(idx);
        }
    }

    title_btn_armed_win = -1;
    title_btn_armed_kind = -1;
    title_btn_pressed_active = 0;
    redraw_pending = 1;
}

int content_hover_win = -1;

// Tells the window under the cursor that it's hovered, and the one the
// cursor just left that it isn't. See gui_apps.h's on_hover contract.
//
// Deliberately delivered to the topmost window under the cursor whether
// or not it has focus -- the only app callback that reaches an
// unfocused window. A control that stays inert until you've clicked its
// window first is exactly the deadness hover exists to remove.
void wm_update_content_hover(int mx, int my, uint8_t buttons) {
    // While anything is held or armed, the press visual owns the
    // feedback and hover must not fight it -- same deference
    // wm_update_title_hover() shows title_btn_armed_win.
    int suppressed = (buttons & 0x1) || content_pressed >= 0 ||
                      content_dragging >= 0 || dragging >= 0 || resizing >= 0 ||
                      title_btn_armed_win >= 0;

    int now = -1;
    if (!suppressed) {
        for (int i = window_count - 1; i >= 0; i--) {
            struct window *w = &windows[i];
            if (w->state == WIN_MINIMIZED) continue;
            if (!uui_hit(w->x, w->y, w->w, w->h, mx, my)) continue;
            // Over this window, but the title bar isn't app content.
            if (my >= w->y + WM_TITLEBAR_H) now = i;
            break; // topmost hit wins either way -- windows below are covered
        }
    }

    if (now != content_hover_win) {
        // The window being left hears (-1,-1) so it can clear its own
        // hover state; without this a control stays lit after the
        // cursor has moved on, which looks like a stuck highlight.
        if (content_hover_win >= 0 && content_hover_win < window_count) {
            struct window *prev = &windows[content_hover_win];
            if (prev->app && prev->app->on_hover) {
                if (prev->app->on_hover(prev, -1, -1)) redraw_pending = 1;
            }
            // Same "you are being left" signal for a client. Sent as a
            // move to (-1,-1) in its own coordinates, which its widgets
            // hit-test as "nothing" and clear their highlight from --
            // no special case needed on either side.
            if (wm_client_is_client_window(prev)) {
                wm_client_send_mouse(prev, WIN_EV_MOUSE_MOVE,
                                      window_content_x(prev) - 1,
                                      window_content_y(prev) - 1, 0);
            }
        }
        content_hover_win = now;
    }

    if (now < 0) return;
    struct window *w = &windows[now];
    wm_client_send_mouse(w, WIN_EV_MOUSE_MOVE, mx, my, 0); // no-op for an app window
    if (!w->app || !w->app->on_hover) return;
    int ccx = mx - window_content_x(w);
    int ccy = my - window_content_y(w);
    if (w->app->on_hover(w, ccx, ccy)) redraw_pending = 1;
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
