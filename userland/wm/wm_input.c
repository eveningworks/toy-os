// Mouse click handling and the per-tick drag/resize update -- called
// from wm.c's wm_run() loop. See wm_internal.h for the shared state
// this reads and mutates (windows[], dragging/resizing, etc.) and
// wm.c's top comment for why that sharing is fine here.
#include "wm_internal.h"
#include "wm_shadow.h"
#include "wm_anim.h"   // wm_damage_window_rect(): a window's rect plus its shadow
#include "start_menu.h"
#include "start_store.h"
#include "context_menu.h"
#include "calendar_popup.h"
#include "volume_popup.h"
#include "brightness_popup.h"
#include "wm_overlay.h"
#include "wm_taskbar.h"
#include "wm_tray.h"
#include "confirm_dialog.h"
#include "desktop.h"
#include "ui/uui.h"
#include "rt/sys.h"   // sys_ticks(), for the title-bar double-click
#include "lib/usetting.h" // desktop.move_mode / desktop.resize_mode
#include "wm_log.h"
#include "kapi.h"

// How long a window may take to BE the size that was asked for before
// `auto` stops asking it to keep up: a tenth of a second, the threshold
// below which a response reads as instantaneous and above which it
// reads as the machine lagging (Card, Robertson and Newell; Nielsen's
// 0.1 s).
#define RESIZE_AUTO_LAG_MS 100

// --- what a drag SHOWS: the window, or an outline ---------------------
//
// `desktop.move_mode` and `desktop.resize_mode` (kernel/lib/
// window_drag_config.c). Read ONCE when a drag begins rather than
// polled: a drag is short, a setting cannot usefully change during one,
// and one syscall per drag needs no generation tracking at all.

// The outline rect is thrown away and rebuilt as the pointer moves, so
// both the old and the new one have to be damaged. One helper, because
// a move and a resize differ only in which corner is pinned.
static void drag_outline_set(int idx, int x, int y, int w, int h) {
    if (drag_outline_win >= 0) {
        wm_damage_rect(drag_outline_x, drag_outline_y,
                       drag_outline_w, drag_outline_h);
    }
    drag_outline_win = idx;
    drag_outline_x = x; drag_outline_y = y;
    drag_outline_w = w; drag_outline_h = h;
    if (idx >= 0) wm_damage_rect(x, y, w, h);
    redraw_pending = 1;
}

static void drag_outline_clear(void) { drag_outline_set(-1, 0, 0, 0, 0); }

// "outline" from a setting; anything else -- including a value nothing
// recognises -- means show the window, which is the behaviour a machine
// with no /etc/desktop.conf gets.
static int mode_is_outline(const char *name) {
    char v[SETTING_ABI_VALUE_MAX];
    if (!usetting_get(name, v, sizeof v)) return 0;
    return k_strcmp(v, "outline") == 0;
}

static int mode_is_auto(const char *name) {
    char v[SETTING_ABI_VALUE_MAX];
    if (!usetting_get(name, v, sizeof v)) return 1;  // auto is the default
    return v[0] == '\0' || k_strcmp(v, "auto") == 0;
}


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
static void damage_title_buttons(int win);

static int title_btn_hit_test(int mx, int my, int *out_kind) {
    for (int i = window_count - 1; i >= 0; i--) {
        struct window *w = &windows[i];
        if (w->state == WIN_MINIMIZED) continue;
        if (!uui_hit(w->x, w->y, w->w, w->h, mx, my)) continue;
        if (!window_has_chrome(w)) return -1;      // fullscreen: nothing to press
        if (my >= w->y + WM_TITLEBAR_H) return -1; // topmost window here, but below its title bar
        struct btn_rects r = title_buttons(w);
        if (uui_hit(r.min_x, r.y, r.size, r.size, mx, my)) { *out_kind = 0; return i; }
        if (uui_hit(r.max_x, r.y, r.size, r.size, mx, my)) { *out_kind = 1; return i; }
        if (uui_hit(r.close_x, r.y, r.size, r.size, mx, my)) { *out_kind = 2; return i; }
        return -1; // over the title bar, but not a button
    }
    return -1;
}

int wm_find_resize_zone(int mx, int my, int *out_edges) {
    for (int i = window_count - 1; i >= 0; i--) {
        struct window *w = &windows[i];
        if (w->state == WIN_MINIMIZED) continue;

        int inside = uui_hit(w->x, w->y, w->w, w->h, mx, my);
        int near = uui_hit(w->x - RESIZE_OUTSIDE, w->y - RESIZE_OUTSIDE,
                           w->w + 2 * RESIZE_OUTSIDE, w->h + 2 * RESIZE_OUTSIDE,
                           mx, my);
        if (!near) continue;

        // **A WINDOW THAT CANNOT BE RESIZED HAS NO OUTSIDE BAND.** It
        // still owns what is genuinely inside it -- so the search stops
        // there, as it always did -- but it must not reach past its own
        // frame, or a fixed-size popup would silently swallow the resize
        // border of the window underneath it.
        int no_resize = (w->state == WIN_MAXIMIZED || w->fullscreen ||
                         !w->resizable || wm_dialog_blocker(i) >= 0);
        if (no_resize) {
            if (inside) return -1;
            continue;
        }

        int edges = 0;
        if (mx < w->x + RESIZE_INSIDE)              edges |= WM_EDGE_LEFT;
        if (mx >= w->x + w->w - RESIZE_INSIDE)      edges |= WM_EDGE_RIGHT;
        if (my < w->y + RESIZE_INSIDE)              edges |= WM_EDGE_TOP;
        if (my >= w->y + w->h - RESIZE_INSIDE)      edges |= WM_EDGE_BOTTOM;

        // A CORNER IS BIGGER THAN ITS EDGES. Within RESIZE_CORNER of
        // one, the perpendicular edge joins in even though the pointer
        // is not in its strip -- so the diagonal drag has a square to
        // aim at rather than the point the two strips intersect in.
        // Every desktop does this; Windows names the zones HTTOPLEFT
        // and friends for the same reason.
        if (edges & (WM_EDGE_LEFT | WM_EDGE_RIGHT)) {
            if (my < w->y + RESIZE_CORNER)                edges |= WM_EDGE_TOP;
            else if (my >= w->y + w->h - RESIZE_CORNER)   edges |= WM_EDGE_BOTTOM;
        }
        if (edges & (WM_EDGE_TOP | WM_EDGE_BOTTOM)) {
            if (mx < w->x + RESIZE_CORNER)                edges |= WM_EDGE_LEFT;
            else if (mx >= w->x + w->w - RESIZE_CORNER)   edges |= WM_EDGE_RIGHT;
        }

        // Inside the frame and on no edge: the window body, and the
        // search stops here rather than falling through to a window
        // below. Outside it, `near` guarantees at least one edge, so
        // this cannot swallow a point the band was meant to catch.
        if (!edges) {
            if (inside) return -1;
            continue;
        }

        // THE TITLE BAR'S TOP STRIP RESIZES, THE REST OF IT DRAGS, and
        // that split is why this test comes after the edges rather than
        // before them: the top edge IS the title bar on every window
        // here, so refusing the whole bar (as this did) made the top
        // edge and both top corners unreachable. Windows and KWin carve
        // the same strip out of their own title bars.
        if (inside && my < w->y + WM_TITLEBAR_H && !(edges & WM_EDGE_TOP) &&
            !(edges & (WM_EDGE_LEFT | WM_EDGE_RIGHT)))
            return -1;

        *out_edges = edges;
        return i;
    }
    return -1;
}

static void wm_toggle_maximize(int i);

// The last title-bar press, for the double-click below.
#define TITLE_DOUBLE_CLICK_TICKS 30   // ~300 ms at 100 Hz, desktop.c's threshold
static int title_click_pid = 0;
static uint32_t title_click_win = 0;
static unsigned long title_click_tick = 0;

void wm_handle_left_click(int mx, int my) {
    // EVERY OVERLAY FIRST, most modal first, from the table in
    // wm_overlay.h -- a modal dialog takes a click before a menu does,
    // and both take one before a window. The two tray popups are asked
    // BEFORE the taskbar below, which is what makes a second click on
    // the clock or the speaker CLOSE the popup instead of reopening it:
    // the click is outside the panel, so the popup closes and stops,
    // and the tray hit-test never runs.
    if (wm_overlay_click(mx, my)) return;

    // A desktop caption being edited: its field takes the click, and any
    // other click commits it first and then does its own job.
    if (desktop_rename_click(mx, my)) return;

    // THE POPUP GRAB, before the taskbar and the windows: a press
    // anywhere but one of the owning client's own surfaces dismisses its
    // popups and goes no further -- Wayland's and Win32's rule both, and
    // what makes a click on the desktop behind an open menu close the
    // menu instead of also selecting an icon. A press inside the client's
    // other windows falls through and is delivered; the client decides
    // what it means, as it always has (ui/uui_menubar.c's press).
    {
        int owner = wm_client_popup_owner();
        if (owner && wm_client_popup_route(owner, mx, my) == 0) {
            wm_client_popups_dismiss(owner);
            return;
        }
    }

    if (my >= screen_h - taskbar_h && !wm_top_covers_screen()) {
        int sx, sy, sw, sh;
        taskbar_start_hit_rect(&sx, &sy, &sw, &sh);
        if (uui_hit(sx, sy, sw, sh, mx, my)) {
            start_menu_open_now();   // closes the other popups itself
            redraw_pending = 1;
            return;
        }
        // THE CLOCK OPENS THE CALENDAR, and the rect comes from the tray
        // itself (wm_tray.h) rather than from "the right end of the
        // strip" -- an app-registered tray item moves the clock left,
        // and a hit-test phrased as a screen edge would then open the
        // popup from the wrong control.
        {
            int cx, cy, cw, ch;
            if (tray_clock_rect(&cx, &cy, &cw, &ch) &&
                uui_hit(cx, cy, cw, ch, mx, my)) {
                calendar_open_now();
                return;
            }
        }
        // The window buttons, their layout and what a click on one does
        // all live in wm_taskbar.c -- this used to walk the windows with
        // a fixed button width, as did the right-click path below and
        // draw_taskbar(), and three walks of one strip is how buttons
        // came to be drawn where nothing would hit-test them.
        taskbar_handle_click(mx, my);
        return;
    }

    {
        int edges = 0;
        int ri = wm_find_resize_zone(mx, my, &edges);
        if (ri >= 0) {
            bring_to_front(ri);
            dragging = -1;
            resizing = window_count - 1;
            resize_edges = edges;
            resize_start_mx = mx;
            resize_start_my = my;
            resize_start_w = windows[resizing].w;
            resize_start_h = windows[resizing].h;
            resize_start_x = windows[resizing].x;
            resize_start_y = windows[resizing].y;
            // WHAT THIS DRAG SHOWS, decided once, here.
            //
            // `auto` ASKS WHAT HAPPENED LAST TIME. A window already
            // measured as slow is outlined from the first pixel; one
            // that has never been measured starts live and is watched
            // (see wm_resize_shown()), which is the only way a window
            // nobody has dragged yet can be judged at all. Both
            // directions self-correct, because even an outlined drag
            // measures its one resize on release.
            resize_outline_mode = mode_is_outline("desktop.resize_mode");
            resize_auto = !resize_outline_mode && mode_is_auto("desktop.resize_mode");
            if (resize_auto && windows[resizing].resize_lag_ms > RESIZE_AUTO_LAG_MS)
                resize_outline_mode = 1;
            // A NEW DRAG FORGETS THE LAST ONE'S ASK. The ask outlives
            // the drag that made it (see resize_pump()), and anything
            // that resized this window since -- maximize, a restored
            // geometry -- did not go through it, so the remembered
            // "already sent" size can no longer be trusted.
            resize_sent_w = resize_sent_h = -1;
            resize_inflight = 0;
            redraw_pending = 1;
            return;
        }
    }

    for (int i = window_count - 1; i >= 0; i--) {
        struct window *w = &windows[i];
        if (w->state == WIN_MINIMIZED) continue;
        if (!uui_hit(w->x, w->y, w->w, w->h, mx, my)) continue;

        // A WINDOW WITH A MODAL DIALOG UP TAKES NO PRESS -- the dialog
        // is raised and focused instead, and the press goes no further.
        // Win32 disables the owner HWND to the same end; KDE raises and
        // flashes. Not even the title bar: dragging a window whose
        // dialog stays put is how the two end up on opposite sides of
        // the screen with nothing to say they are related.
        int blocker = wm_dialog_blocker(i);
        if (blocker >= 0) {
            bring_to_front(blocker);
            redraw_pending = 1;
            return;
        }

        if (window_has_chrome(w) && my < w->y + WM_TITLEBAR_H) {
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
                damage_title_buttons(i);   // the pressed look arrives
                title_btn_armed_kind = 0;
                title_btn_pressed_active = 1;
                title_hover_win = -1;
                title_hover_kind = -1;
                redraw_pending = 1;
                return;
            }
            if (uui_hit(r.max_x, r.y, r.size, r.size, mx, my)) {
                title_btn_armed_win = i;
                damage_title_buttons(i);   // the pressed look arrives
                title_btn_armed_kind = 1;
                title_btn_pressed_active = 1;
                title_hover_win = -1;
                title_hover_kind = -1;
                redraw_pending = 1;
                return;
            }
            if (uui_hit(r.close_x, r.y, r.size, r.size, mx, my)) {
                title_btn_armed_win = i;
                damage_title_buttons(i);   // the pressed look arrives
                title_btn_armed_kind = 2;
                title_btn_pressed_active = 1;
                title_hover_win = -1;
                title_hover_kind = -1;
                redraw_pending = 1;
                return;
            }
            // THE APP ICON OPENS THE WINDOW MENU, on PRESS -- a menu
            // opens on button-down on every real desktop, and
            // docs/gui-guidelines.md names that as the documented
            // exception to arm-then-commit-on-release. Its items still
            // commit on release.
            //
            // Raised first, like any other title-bar click, so the
            // index the menu targets is the POST-raise one --
            // bring_to_front() moves the window to the end of the
            // array and every index above it shifts down.
            int ix, iy, isz;
            if (title_icon(i, &ix, &iy, &isz) && uui_hit(ix, iy, isz, isz, mx, my)) {
                bring_to_front(i);
                // Anchored under the icon rather than at the cursor,
                // which is what a menu attached to a fixed piece of
                // chrome does -- the same thing the menu bar does with
                // its own titles.
                wm_open_window_menu(window_count - 1, ix, iy + isz);
                redraw_pending = 1;
                return;
            }

            // DOUBLE-CLICK ON THE TITLE BAR TOGGLES MAXIMIZE, as on
            // Windows and KDE. The window is named by its client ids
            // rather than its index, which bring_to_front() moves.
            // The same threshold the desktop's icons use.
            unsigned long now = sys_ticks();
            int same = w->client_pid == title_click_pid && w->client_win == title_click_win;
            if (same && now - title_click_tick <= TITLE_DOUBLE_CLICK_TICKS) {
                title_click_pid = 0;   // a third click is a fresh first one
                bring_to_front(i);
                wm_toggle_maximize(window_count - 1);
                redraw_pending = 1;
                return;
            }
            title_click_pid = w->client_pid;
            title_click_win = w->client_win;
            title_click_tick = now;

            if (w->state != WIN_MAXIMIZED) {
                bring_to_front(i);
                resizing = -1;
                dragging = window_count - 1;
                drag_off_x = mx - windows[dragging].x;
                drag_off_y = my - windows[dragging].y;
                move_outline_mode = mode_is_outline("desktop.move_mode");
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
                content_pressed_btn = 0x1;
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
                content_pressed_btn = 0x1;
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

// The window menu's window, as its open_seq -- the menu stays up while
// windows open, close and raise, and an index would by then name a
// different window (Close on one window closed another). Every row
// resolves it through ctx_target(), and a window that has gone does
// nothing.
static uint32_t g_ctx_window_seq;
static struct context_menu_item g_window_menu[5];

static int ctx_target(void *ctx) { return wm_window_by_seq(*(uint32_t *)ctx); }

// Goes through wm_request_close(), not close_window(). It used to call
// the latter, which for a ring-3 client destroyed its window without
// ever sending WIN_EV_CLOSE -- so right-click > Close skipped the
// handshake the X button had always honoured, and an app refusing to
// close was closed anyway. A WM feature that treats one window kind
// correctly and the other not; see wm_internal.h.
static void ctx_close_window(void *ctx) {
    int i = ctx_target(ctx);
    if (i >= 0) wm_request_close(i);
}

void wm_window_minimize(int i) {
    if (windows[i].state == WIN_MINIMIZED) return;
    wm_anim_minimize(i);
    windows[i].min_prev = windows[i].state;
    windows[i].state = WIN_MINIMIZED;
    redraw_pending = 1;
}

void wm_window_unminimize(int i) {
    if (windows[i].state != WIN_MINIMIZED) return;
    wm_anim_restore(i);
    windows[i].state = windows[i].min_prev == WIN_MAXIMIZED ? WIN_MAXIMIZED : WIN_NORMAL;
    redraw_pending = 1;
}

int wm_window_maximized(const struct window *w) {
    return w->state == WIN_MAXIMIZED ||
           (w->state == WIN_MINIMIZED && w->min_prev == WIN_MAXIMIZED);
}

static void ctx_minimize_window(void *ctx) {
    int i = ctx_target(ctx);
    if (i >= 0) wm_window_minimize(i);
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
    wm_damage_window_rect(windows[i].x, windows[i].y, windows[i].w, windows[i].h);

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
    int i = ctx_target(ctx);
    if (i >= 0) wm_toggle_maximize(i);
}

// Fullscreen: the whole screen, no chrome, no taskbar. The window's
// state stays what it was -- fs_prev remembers it -- so leaving lands
// back on the maximized or the saved rect. A client is PROPOSED the
// size, as for maximize; the lease (wm_scanout.c) waits until it has
// adopted it.
void wm_set_fullscreen(int i, int on) {
    if (i < 0 || i >= window_count) return;
    struct window *w = &windows[i];
    if (!w->resizable || w->popup) return;
    on = on ? 1 : 0;
    if (w->fullscreen == on) return;
    int is_client = wm_client_is_client_window(w);

    wm_damage_window_rect(w->x, w->y, w->w, w->h);
    if (on) {
        w->fs_prev = w->state;
        if (w->state != WIN_MAXIMIZED) {
            w->saved_x = w->x; w->saved_y = w->y;
            w->saved_w = w->w; w->saved_h = w->h;
        }
        w->fullscreen = 1;
        w->x = 0; w->y = 0;
        if (is_client) wm_client_send_resize(w, screen_w, screen_h);
        else { w->w = screen_w; w->h = screen_h; }
        bring_to_front(i);
        w = &windows[window_count - 1];   // the raise moved it
    } else {
        w->fullscreen = 0;
        if (w->fs_prev == WIN_MAXIMIZED) {
            w->state = WIN_MAXIMIZED;
            w->x = 0; w->y = 0;
            if (is_client)
                wm_client_send_resize(w, screen_w - 2, screen_h - taskbar_h - WM_TITLEBAR_H - 2);
            else { w->w = screen_w; w->h = screen_h - taskbar_h; }
        } else {
            w->state = WIN_NORMAL;
            w->x = w->saved_x; w->y = w->saved_y;
            if (is_client)
                wm_client_send_resize(w, w->saved_w - 2, w->saved_h - WM_TITLEBAR_H - 2);
            else { w->w = w->saved_w; w->h = w->saved_h; }
        }
    }
    wm_damage_window_rect(w->x, w->y, w->w, w->h);
    redraw_pending = 1;
}

static void ctx_toggle_fullscreen_window(void *ctx) {
    int i = ctx_target(ctx);
    if (i >= 0) wm_set_fullscreen(i, !windows[i].fullscreen);
}

// Launching DISMISSES the Start menu, which is what a left-click on the
// same row does and what every desktop does when a window opens. "Add to
// desktop" deliberately does not: it is a non-launching verb, and
// Windows leaves Start up after one so you can do another.
static void ctx_open_app(void *ctx) {
    start_menu_close();
    open_app((const struct gui_app *)ctx);
}
static void ctx_add_to_desktop(void *ctx) { desktop_add_launcher((const struct gui_app *)ctx); }

// THE WINDOW MENU: one menu for the whole window, mirroring the
// title-bar buttons' actions rather than requiring a click to land
// exactly on one of those small squares -- which is the whole point of
// offering it as a menu at all.
//
// TWO WAYS IN, ONE IMPLEMENTATION. A right-click anywhere on the window
// opens it, and so does a LEFT-click on the title bar's app icon, which
// is what Windows' system menu and Plasma's window-menu button do. They
// used to be one call site; a second one is exactly where two copies of
// a rule stop agreeing -- Close in particular has to stay
// wm_request_close() (ASK the client) rather than close_window() (seize
// it), which is a bug this file has already shipped once.
static void ctx_restore_window(void *ctx) {
    int i = ctx_target(ctx);
    if (i < 0 || windows[i].state != WIN_MINIMIZED) return;
    wm_window_unminimize(i);
    wm_ensure_reachable(i);
    raise_with_dialogs(i);
    redraw_pending = 1;
}

void wm_open_window_menu(int idx, int mx, int my) {
    if (idx < 0 || idx >= window_count) return;
    const struct window *w = &windows[idx];
    if (!windows[idx].open_seq) windows[idx].open_seq = wm_next_open_seq();
    g_ctx_window_seq = w->open_seq;
    struct context_menu_item *items = g_window_menu;
    int n = 0;
    // A MINIMIZED window -- reachable only from its taskbar button --
    // offers the way back instead of a Minimize that would do nothing.
    // The glyphs are the caption buttons', so a row and the button it
    // stands for look alike.
    if (w->state == WIN_MINIMIZED)
        items[n++] = (struct context_menu_item){ .label = "Restore", .on_select = ctx_restore_window,
                                                 .ctx = &g_ctx_window_seq, .icon = "tb-restore" };
    else
        items[n++] = (struct context_menu_item){ .label = "Minimize", .on_select = ctx_minimize_window,
                                                 .ctx = &g_ctx_window_seq, .icon = "tb-minimize" };
    // Nothing that sizes a window it cannot see: maximizing or going
    // fullscreen from minimized would leave it resized and still hidden.
    if (w->resizable && w->state != WIN_MINIMIZED) {
        int max = w->state == WIN_MAXIMIZED;
        items[n++] = (struct context_menu_item){ .label = max ? "Restore" : "Maximize",
                                                 .on_select = ctx_toggle_maximize_window,
                                                 .ctx = &g_ctx_window_seq,
                                                 .icon = max ? "tb-restore" : "tb-maximize" };
        items[n++] = (struct context_menu_item){ .label = w->fullscreen ? "Exit Fullscreen" : "Fullscreen",
                                                 .on_select = ctx_toggle_fullscreen_window,
                                                 .ctx = &g_ctx_window_seq,
                                                 .icon = w->fullscreen ? "tb-unfullscreen" : "tb-fullscreen",
                                                 .tint = UTHEME_ACT_VIEW };
    }
    // Close apart, as Windows' window menu sets it, with the key that does it.
    items[n++] = (struct context_menu_item){ .separator = 1 };
    items[n++] = (struct context_menu_item){ .label = "Close", .on_select = ctx_close_window,
                                             .ctx = &g_ctx_window_seq, .icon = "tb-close",
                                             .tint = UTHEME_ACT_DANGER, .accel = "Alt+F4" };
    context_menu_open_at(mx, my, items, n);
}

void wm_window_menu_forget(int idx) {
    if (windows[idx].open_seq == g_ctx_window_seq && context_menu_showing(g_window_menu))
        context_menu_close();
}

// Pin or unpin, and leave the Start menu UP: this changes what the menu
// shows rather than launching anything, and dismissing it would hide
// the result of the click. The sidebar gains or loses its Favourites
// folder on the next frame, which is the feedback.
static void ctx_toggle_pin(void *ctx) {
    const struct gui_app *app = (const struct gui_app *)ctx;
    if (!app || !app->app_id) return;
    start_menu_toggle_pin(app->app_id);
}

// A THUMB BUTTON GOES STRAIGHT TO THE CLIENT UNDER THE POINTER, and
// does no window management on the way: it does not raise, focus, drag,
// or touch the chrome. That is Wayland's rule for a button the
// compositor has no policy for -- and it is also what makes the button
// usable, since navigating back in a window you are already looking at
// should not reorder the desktop.
//
// DELIVERED BY POSITION, NOT BY FOCUS, which is what both Windows
// (mouse messages go to the window under the cursor) and Wayland
// (pointer focus IS the surface under the pointer) do.
void wm_handle_thumb_button(int mx, int my, unsigned btn, int down) {
    for (int i = window_count - 1; i >= 0; i--) {
        struct window *w = &windows[i];
        if (w->state == WIN_MINIMIZED) continue;
        if (!uui_hit(w->x, w->y, w->w, w->h, mx, my)) continue;
        if (wm_dialog_blocker(i) >= 0) return;   // a modal owner takes nothing
        // CONTENT ONLY. A press on the title bar or a border is chrome,
        // and the client has no business hearing about it.
        int cx = mx - window_content_x(w), cy = my - window_content_y(w);
        if (cx < 0 || cy < 0 || cx >= w->w || cy >= w->h) return;
        if (wm_client_is_client_window(w))
            wm_client_send_mouse(w, down ? WIN_EV_MOUSE_DOWN : WIN_EV_MOUSE_UP,
                                 mx, my, down ? btn : 0);
        return;   // the topmost window here takes it, hit or not
    }
}

void wm_handle_right_click(int mx, int my) {
    // A right-click always resolves to at most one popup -- close
    // whatever's already open before deciding what (if anything) the
    // new click should show, so right-clicks never stack menus.
    if (start_menu_open) {
        // Which app, asked of start_menu.c -- this used to re-derive the
        // row formula, and its copy counted gui_app_registry_count rows
        // where the menu draws only the ones that show in it.
        struct gui_app *app = start_menu_app_at(mx, my);
        // THE START MENU STAYS UP UNDER ITS OWN ROW'S MENU, as on
        // Windows: naming it the popup's parent is what stops
        // context_menu_open_at()'s close_others() taking it down. A row
        // that LAUNCHES still dismisses it (ctx_open_app), because
        // opening a window is what closes Start everywhere else.
        if (!app) { start_menu_close(); return; }
        wm_overlay_set_parent("start");
        {
            static struct context_menu_item item[3];
            item[0] = (struct context_menu_item){ .label = "Open", .on_select = ctx_open_app,
                                                  .ctx = app, .icon = "tb-open",
                                                  .tint = UTHEME_ACT_NAV };
            // PIN IS A TOGGLE WITH TWO NAMES, not a checkmark: the row
            // says what the click will DO, which is how Windows and
            // KDE both word this ("Pin to Start" / "Unpin from
            // Start"). A ticked "Pinned" would need a second click to
            // discover what it means.
            item[1] = (struct context_menu_item){
                .label = start_store_is_pinned(app->app_id) ? "Unpin from Start" : "Pin to Start",
                .on_select = ctx_toggle_pin, .ctx = app, .icon = "tb-pin" };
            item[2] = (struct context_menu_item){ .label = "Add to desktop",
                                                  .on_select = ctx_add_to_desktop, .ctx = app,
                                                  .icon = "tb-desktop-add", .tint = UTHEME_ACT_CREATE };
            context_menu_open_at(mx, my, item, 3);
        }
        return;
    }

    wm_overlay_close_others(0); // a right-click anywhere dismisses a popup, as a menu does

    // And a client's popup, by the same rule as the primary button --
    // see wm_handle_left_click().
    {
        int owner = wm_client_popup_owner();
        if (owner && wm_client_popup_route(owner, mx, my) == 0) {
            wm_client_popups_dismiss(owner);
            return;
        }
    }

    if (my >= screen_h - taskbar_h && !wm_top_covers_screen()) {
        taskbar_handle_right_click(mx, my);
        return; // taskbar area, but not over an app button (or the Start button -- no menu there)
    }

    for (int i = window_count - 1; i >= 0; i--) {
        struct window *w = &windows[i];
        if (w->state == WIN_MINIMIZED) continue;
        if (!uui_hit(w->x, w->y, w->w, w->h, mx, my)) continue;

        // BLOCKED BY A MODAL DIALOG, exactly as the primary button is
        // (wm_handle_left_click()). A secondary click that still opened
        // the app's context menu would be a live control on a window
        // whose every other control is refused.
        int blocker = wm_dialog_blocker(i);
        if (blocker >= 0) {
            bring_to_front(blocker);
            redraw_pending = 1;
            return;
        }

        // THE CONTENT AREA BELONGS TO THE CLIENT, THE FRAME BELONGS TO
        // THE WM. A secondary click inside a client's own pixels is
        // delivered to it as WIN_EV_MOUSE_DOWN with button bit 0x2,
        // exactly as the primary one is; the window menu stays on the
        // title bar, the app icon, the border, the taskbar button and
        // Alt+F4. That is the split Windows, X11 and Wayland all make --
        // a right-click in Notepad's text area opens Notepad's menu, not
        // the system menu -- and this used to be the one place toy-os
        // was the outlier, seizing the button over the whole window and
        // leaving a client with no secondary click at all.
        //
        // What it COSTS, stated plainly: the window menu is no longer
        // reachable from the middle of an app's window. Four other ways
        // in remain (above), which is the same bargain every real
        // desktop makes.
        if (wm_client_is_client_window(w) &&
            uui_hit(window_content_x(w), window_content_y(w),
                     window_content_w(w), window_content_h(w), mx, my)) {
            bring_to_front(i);
            int fi = window_count - 1;
            // Armed exactly like the left button's press, or the
            // MOUSE_UP below would never be sent -- see the same
            // reasoning in wm_handle_left_click().
            content_pressed = fi;
            content_pressed_btn = 0x2;
            wm_client_send_mouse(&windows[fi], WIN_EV_MOUSE_DOWN, mx, my, 2);
            redraw_pending = 1;
            return;
        }

        wm_open_window_menu(i, mx, my);
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

    wm_damage_window_rect(w->x, w->y, w->w, w->h); // vacated
    w->x = nx;
    w->y = ny;
    wm_damage_window_rect(w->x, w->y, w->w, w->h); // arrived
    return 1;
}

// --- the interactive resize's ask ------------------------------------
//
// A client owns its buffer, so the WM cannot simply widen its window:
// it proposes a size and the client answers (abi/win_proto.h). What
// makes that feel like Windows' and KDE's live resize rather than a
// rubber-band outline is doing it DURING the drag -- and what keeps it
// from drowning a slow app is that only one proposal is outstanding.

// A proposal is with the client until it acks, and ignoring one is
// legal, so an unanswered proposal cannot be allowed to take the grip
// with it.
#define RESIZE_ACK_TIMEOUT_TICKS (USER_HZ / 2)

// The window the ask names, or -1. The pid/id pair is checked because
// closing a window compacts windows[] -- a bare index would then name
// somebody else and resize the wrong app.
static int resize_ask_target(void) {
    if (resize_ask_idx < 0 || resize_ask_idx >= window_count) return -1;
    const struct window *w = &windows[resize_ask_idx];
    if (w->client_pid != resize_ask_pid || w->client_win != resize_ask_win)
        return -1;
    return resize_ask_idx;
}

// Send the next proposal, if the drag has asked for a size the client
// has not been told about and nothing is outstanding.
static void resize_pump(void) {
    int idx = resize_ask_target();
    if (idx < 0 || resize_want_w <= 0) return;
    if (resize_want_w == resize_sent_w && resize_want_h == resize_sent_h) return;
    // A PROPOSAL FOR THE SIZE IT ALREADY IS IS NOT A RESIZE. The press
    // that begins a drag is itself a motion, with a delta of zero, so
    // this is the first thing a drag would otherwise ask for -- and the
    // answer is a present that changes nothing, which is the one thing
    // the pacing below cannot use.
    if (resize_want_w == windows[idx].client_w &&
        resize_want_h == windows[idx].client_h) {
        resize_sent_w = resize_want_w;
        resize_sent_h = resize_want_h;
        return;
    }
    if (resize_inflight && sys_ticks() - resize_sent_tick < RESIZE_ACK_TIMEOUT_TICKS)
        return;

    wm_client_send_resize(&windows[idx], resize_want_w, resize_want_h);
    resize_asks++;
    resize_sent_w = resize_want_w;
    resize_sent_h = resize_want_h;
    resize_inflight = 1;
    resize_sent_tick = sys_ticks();
}

// THE CLIENT HAS SHOWN THE SIZE IT WAS ASKED FOR. Called from the
// PRESENT that adopts it, not from the ack -- and that distinction is
// the whole measurement. A client acks a proposal the moment it gets
// one, from inside uapp's event handler, and only then draws; so the
// ack times a syscall round trip (a few ms for anything) while the
// present times what the user actually waits for. Pacing on it is also
// what Wayland does: a compositor waits for the COMMIT, not for the
// ack_configure.
void wm_resize_shown(int idx, int size_changed) {
    if (idx != resize_ask_target()) return;

    // CLEARED BY ANY PRESENT, not only by one that changed the size. A
    // client may present for its own reasons, and it may answer a
    // proposal with a size the server clamped to what it already had --
    // and a rule that waited for a CHANGE would then wait forever, with
    // the drag's remaining sizes queued behind it. Sending the next
    // proposal a frame early is harmless; never sending it is a wedged
    // drag.
    resize_inflight = 0;

    // The MEASUREMENT still needs a real one, or an unrelated repaint
    // arriving late would read as the client lagging.
    if (!size_changed) { resize_pump(); return; }
    resize_lag_ms = (unsigned)((sys_ticks() - resize_sent_tick) * (1000 / USER_HZ));
    windows[idx].resize_lag_ms = resize_lag_ms;

    // ONE STRIKE, AND NO WAY BACK WITHIN THE DRAG. A client that missed
    // one deadline will miss the next -- the cost is its own repaint --
    // and a window alternating between following the pointer and being
    // an outline mid-drag would be worse than either. The next drag
    // re-reads the setting and starts live again.
    if (resize_auto && resizing >= 0 && resize_lag_ms > RESIZE_AUTO_LAG_MS) {
        resize_auto = 0;
        resize_outline_mode = 1;
        drag_outline_set(resizing, windows[resizing].x, windows[resizing].y,
                         windows[resizing].w, windows[resizing].h);
        wm_logf("wm: resize fell back to an outline -- the window took %ums "
                "to become the size it was asked for\n", resize_lag_ms);
    }
    resize_pump();
}

// Point the ask at a window and give it a size. `w`/`h` are CONTENT
// pixels -- a client knows nothing about chrome.
void wm_resize_client(int idx, int w, int h);

static void resize_ask(int idx, int w, int h) {
    if (idx != resize_ask_target()) {
        resize_ask_idx = idx;
        resize_ask_pid = windows[idx].client_pid;
        resize_ask_win = windows[idx].client_win;
        resize_sent_w = resize_sent_h = -1;
        resize_inflight = 0;
    }
    resize_want_w = w;
    resize_want_h = h;
    resize_pump();
}

// **A WINDOW CAN BE RESIZED WITHOUT A POINTER.** `gui resize` reaches
// this; nothing else does. It exists because a test had no way to
// resize a window at all -- dragging the grip needs a real pointer the
// compositor tracks across frames, which is exactly the case gui_debug's
// injected input cannot drive -- so every resize behaviour on the client
// side was untestable, which is how a broken one shipped.
//
// The SAME resize_ask() the grip uses, so this drives the real path
// (propose, the client answers, adopt) rather than a second one.
void wm_resize_client(int idx, int w, int h) {
    if (idx < 0 || idx >= window_count) return;
    if (!wm_client_is_client_window(&windows[idx])) return;
    resize_ask(idx, w, h);
}

void wm_update_drag_resize(int mx, int my, uint8_t buttons) {
    if (dragging >= 0) {
        if (buttons & 0x1) {
            struct window *w = &windows[dragging];
            int nx = mx - drag_off_x;
            int ny = my - drag_off_y;

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

            if (nx < min_x) nx = min_x;
            if (nx > max_x) nx = max_x;
            if (ny < 0) ny = 0;
            if (ny > max_y) ny = max_y;

            // CLAMPED FIRST, whichever is shown. An outline that
            // wandered somewhere the window may not go would be lying
            // about where the window will land.
            if (move_outline_mode) {
                drag_outline_set(dragging, nx, ny, w->w, w->h);
            } else {
                w->x = nx;
                w->y = ny;
                redraw_pending = 1;
            }
        } else {
            // RELEASED: the window goes where the outline was.
            if (move_outline_mode && drag_outline_win == dragging) {
                struct window *w = &windows[dragging];
                wm_damage_window_rect(w->x, w->y, w->w, w->h);   // vacated
                w->x = drag_outline_x;
                w->y = drag_outline_y;
                wm_damage_window_rect(w->x, w->y, w->w, w->h);   // arrived
                drag_outline_clear();
            }
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
            int newx = w->x, newy = w->y;
            int min_w = MIN_CONTENT_W + 2;
            int min_h = MIN_CONTENT_H + WM_TITLEBAR_H + 2;

            if (resize_edges & WM_EDGE_RIGHT) {
                neww = resize_start_w + dx;
                if (neww < min_w) neww = min_w;
                if (w->x + neww > screen_w) neww = screen_w - w->x;
            }
            // THE OPPOSITE EDGE IS THE ANCHOR, so the minimum size and
            // the screen edge both clamp the ORIGIN, not the extent: a
            // left drag that has hit the minimum must stop moving x, or
            // the window walks away while staying the same size.
            if (resize_edges & WM_EDGE_LEFT) {
                int right = resize_start_x + resize_start_w;
                newx = resize_start_x + dx;
                if (newx < 0) newx = 0;
                if (right - newx < min_w) newx = right - min_w;
                neww = right - newx;
            }
            if (resize_edges & WM_EDGE_BOTTOM) {
                newh = resize_start_h + dy;
                if (newh < min_h) newh = min_h;
                if (w->y + newh > screen_h - taskbar_h) newh = screen_h - taskbar_h - w->y;
            }
            if (resize_edges & WM_EDGE_TOP) {
                int bottom = resize_start_y + resize_start_h;
                newy = resize_start_y + dy;
                // Never above the top of the screen -- the same rule the
                // title-bar drag above enforces, and for the same
                // reason: a title bar the pointer cannot reach cannot
                // recover itself.
                if (newy < 0) newy = 0;
                if (bottom - newy < min_h) newy = bottom - min_h;
                newh = bottom - newy;
            }

            // The window MOVES as it sizes, and a client sizes itself a
            // frame or two later (resize_pump()), so the anchored edge
            // can visibly lag on a slow client. `desktop.resize_mode =
            // outline` is the answer for anyone it bothers.
            if (!resize_outline_mode && (newx != w->x || newy != w->y)) {
                wm_damage_window_rect(w->x, w->y, w->w, w->h);
                w->x = newx;
                w->y = newy;
                wm_damage_window_rect(w->x, w->y, w->w, w->h);
                redraw_pending = 1;
            }

            if (resize_outline_mode) {
                // The window keeps its size; the outline says what it
                // will become. A non-client window is outlined too --
                // the setting is about what a DRAG looks like, and the
                // WM owning those pixels is no reason to answer
                // differently.
                drag_outline_set(resizing, newx, newy, neww, newh);
            } else if (is_client) {
                // ASK, every step of the drag. The window changes size
                // when the client presents a frame at the new one, so it
                // follows the pointer at the client's own latency
                // instead of jumping on release.
                resize_ask(resizing, neww - 2, newh - WM_TITLEBAR_H - 2);
            } else {
                w->w = neww;
                w->h = newh;
                redraw_pending = 1;
            }
        } else {
            // RELEASED. In outline mode this is the only size anyone is
            // told about; otherwise the last size asked for is already
            // queued, and the ask outlives the drag on purpose -- a
            // client still answering the previous proposal gets the
            // final one when it does.
            if (resize_outline_mode && drag_outline_win == resizing) {
                int fw = drag_outline_w, fh = drag_outline_h;
                int fx = drag_outline_x, fy = drag_outline_y;
                drag_outline_clear();
                // THE ORIGIN LANDS FIRST, and for a client it lands even
                // though the size arrives later -- a left/top drag moved
                // the outline, and leaving x behind would put the window
                // back where the drag started.
                if (fx != w->x || fy != w->y) {
                    wm_damage_window_rect(w->x, w->y, w->w, w->h);
                    w->x = fx;
                    w->y = fy;
                    wm_damage_window_rect(w->x, w->y, w->w, w->h);
                    redraw_pending = 1;
                }
                if (is_client) {
                    resize_ask(resizing, fw - 2, fh - WM_TITLEBAR_H - 2);
                } else {
                    wm_damage_window_rect(w->x, w->y, w->w, w->h);
                    w->w = fw;
                    w->h = fh;
                    wm_damage_window_rect(w->x, w->y, w->w, w->h);
                    redraw_pending = 1;
                }
            } else if (is_client) {
                resize_pump();
            }
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
        // The button that ARMED this press is the one whose release
        // ends it -- see wm_internal.h's content_pressed_btn.
        if (buttons & content_pressed_btn) {
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
            wm_client_send_mouse(w, WIN_EV_MOUSE_MOVE, mx, my,
                                  (unsigned)content_pressed_btn);
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
            damage_title_buttons(title_btn_armed_win);
            redraw_pending = 1;
        }
        return;
    }

    // Released -- commit if still over the button, otherwise this was a
    // press-then-drag-off cancel and nothing happens.
    if (now_over) {
        int idx = title_btn_armed_win;
        if (title_btn_armed_kind == 0) {
            wm_window_minimize(idx);
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

    damage_title_buttons(title_btn_armed_win);   // the pressed look leaves
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
// Is the bare desktop under the point -- no overlay, taskbar or window
// on top of it? The desktop's hover wash asks, and nothing else must
// light an icon through a window.
int wm_point_on_desktop(int mx, int my) {
    if (wm_overlay_under(mx, my)) return 0;
    if (my >= screen_h - taskbar_h) return 0;
    for (int i = window_count - 1; i >= 0; i--) {
        const struct window *w = &windows[i];
        if (w->state == WIN_MINIMIZED) continue;
        if (uui_hit(w->x, w->y, w->w, w->h, mx, my)) return 0;
    }
    return 1;
}

void wm_update_content_hover(int mx, int my, uint8_t buttons) {
    // While anything is held or armed, the press visual owns the
    // feedback and hover must not fight it -- same deference
    // wm_update_title_hover() shows title_btn_armed_win.
    //
    // A HELD BUTTON FREEZES THE HOVERED WINDOW -- no leave, no enter --
    // Wayland's implicit grab. A leave mid-press reaches a client as a
    // move to (-1,-1) with NO button held, which reads as a release and
    // ends any drag it was tracking. The release re-runs this.
    if ((buttons & 0x1) || content_pressed >= 0 || content_dragging >= 0 ||
        dragging >= 0 || resizing >= 0 || title_btn_armed_win >= 0)
        return;

    int now = -1;
    // UNDER AN OVERLAY, NO WINDOW IS HOVERED: the menu or panel is on top
    // and takes the pointer, as a client's own popup already does (below).
    if (!wm_overlay_under(mx, my)) {
        // While a client has a popup up, motion reaches only ITS
        // surfaces (the grab, abi/win_proto.h): sliding along its menu
        // bar still switches menus, but nothing else lights up.
        int owner = wm_client_popup_owner();
        for (int i = window_count - 1; i >= 0; i--) {
            struct window *w = &windows[i];
            if (w->state == WIN_MINIMIZED) continue;
            if (!uui_hit(w->x, w->y, w->w, w->h, mx, my)) continue;
            if (owner && w->client_pid != owner) break;
            // Over this window, but the title bar isn't app content.
            if (w->popup || my >= w->y + WM_TITLEBAR_H) now = i;
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
    // **A BLOCKED WINDOW GETS NO MOTION EITHER**, and that is not
    // tidiness: hover is the only thing that tells a person a control is
    // live, so an owner whose menu bar still highlighted under the
    // cursor while refusing every click would be advertising controls
    // that do nothing. Win32 gets this free by disabling the owner HWND.
    if (wm_dialog_blocker(now) >= 0) return;
    wm_client_send_mouse(w, WIN_EV_MOUSE_MOVE, mx, my, 0); // no-op for an app window
    if (!w->app || !w->app->on_hover) return;
    int ccx = mx - window_content_x(w);
    int ccy = my - window_content_y(w);
    if (w->app->on_hover(w, ccx, ccy)) redraw_pending = 1;
}

// Recomputes title_hover_win/kind from the live mouse position -- see
// wm_internal.h's comment on title_hover_win. A no-op while a button's
// armed (the press visual owns the drawing then, not hover).
// A TITLE-BUTTON STATE CHANGE MUST DAMAGE ITS RECT, not only ask for a
// frame. `redraw_pending` alone repaints everything ONLY when nothing
// else declared damage that iteration; a client presenting a frame
// (Shapes, every tick) declares its own rect, the render is clipped to
// it, and a hover or press on that window's title bar was never
// painted -- or a highlight that had been painted never left. Seen on
// every flipping display and under KVM, where client presents are
// frequent enough to collide every time.
static void damage_title_buttons(int win) {
    if (win < 0 || win >= window_count) return;
    struct btn_rects r = title_buttons(&windows[win]);
    wm_damage_rect(r.min_x, r.y, (r.close_x + r.size) - r.min_x, r.size);
}

void wm_update_title_hover(int mx, int my) {
    if (title_btn_armed_win >= 0) return;
    int kind = -1;
    int win = title_btn_hit_test(mx, my, &kind);
    if (win != title_hover_win || kind != title_hover_kind) {
        damage_title_buttons(title_hover_win);   // the one losing its highlight
        damage_title_buttons(win);               // the one gaining it
        title_hover_win = win;
        title_hover_kind = kind;
        redraw_pending = 1;
    }
}
