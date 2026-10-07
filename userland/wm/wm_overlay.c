// See wm_overlay.h for what this is and why the table drives three
// verbs rather than one.
#include "wm_internal.h"
#include "wm_peek.h"
#include "wm_overlay.h"
#include "wm_log.h"   // overlay transitions, under damage verification
#include "wm_shadow.h"   // wm_damage_window_rect: the rect PLUS its shadow
#include "kapi.h"
#include "start_menu.h"
#include "context_menu.h"
#include "calendar_popup.h"
#include "volume_popup.h"
#include "brightness_popup.h"
#include "network_popup.h"
#include "remote_popup.h"
#include "layout_popup.h"
#include "confirm_dialog.h"
#include "leave_page.h"
#include "osk.h"
#include "wm_tooltip.h"
#include "crash_notice.h"
#include "wm_flash.h"
#include "ui/uui_primitives.h" // uui_hit

// The two that take no cursor, adapted rather than changed: their
// drawing genuinely does not depend on where the pointer is.
static void draw_confirm(int mx, int my)     { (void)mx; (void)my; confirm_dialog_draw(); }

static int open_start(void)   { return start_menu_open; }
static int open_context(void) { return context_menu_open; }
static int open_calendar(void){ return calendar_open; }
static int open_volume(void)  { return volume_open; }
static int open_brightness(void) { return brightness_open; }
static int open_network(void) { return network_open; }
static int open_remote(void)  { return remote_open; }
static int open_layout(void)  { return layout_open; }
static int open_confirm(void) { return confirm_dialog_open; }
static int open_leave(void) { return leave_page_open; }
static int open_osk(void)     { return osk_open; }
static int open_notice(void)  { return crash_notice_open; }

// MOST MODAL FIRST. This order is the click priority -- a modal dialog
// takes a click before a menu does -- and drawing walks it BACKWARDS,
// so the same row that gets the first click is painted last and lands
// on top. Keeping one table for both is what stops the two orders from
// drifting apart, which they had already started to do.
static int open_tooltip(void) { return wm_tooltip_open; }
static int tooltip_click(int mx, int my) { (void)mx; (void)my; return 0; }

static int open_peek(void) { return wm_peek_open; }
static int confirm_covers(int mx, int my) { (void)mx; (void)my; return confirm_dialog_open; }

// `repaint` (the last column): only the context menu and the confirm
// dialog run actions on close that may change the scene undeclared; the
// Leave page IS the screen. Start launches windows, which damage
// themselves, and the tray flyouts act through settings and child
// processes, whose own polls repaint.
static const struct wm_overlay g_overlays[] = {
    // FIRST, so it is PAINTED LAST and lands on top of everything --
    // including the menu whose row it describes. Its click op always
    // returns 0: a tooltip must never consume the click the person was
    // about to make, which is the one thing every toolkit gets wrong
    // about them. No hover op either; it is not a control.
    // The shutter flash (wm_flash.h): over everything, PASSIVE, and the
    // whole screen while it fades.
    { "flash",    wm_flash_open, wm_flash_draw,    tooltip_click,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, WM_OVERLAY_REPAINT_WHILE_OPEN },
    { "tooltip",  open_tooltip,  wm_tooltip_draw,  tooltip_click,
      0, wm_tooltip_damage, wm_tooltip_rect, 0, 0, 0, wm_tooltip_cancel, 0, 0, 0, 0, 0 },
    // The taskbar's window preview (wm_peek.h): above every menu, since
    // it only opens while none is up, and its click falls through when
    // it lands outside the card.
    { "peek",     open_peek,     wm_peek_draw,     wm_peek_click,
      wm_peek_hover_at,          wm_peek_damage,          wm_peek_rect, 0, 0, 0, wm_peek_close, 0, 0, 0, 0, 0 },
    // MODAL, so it covers the whole screen for input (confirm_covers)
    // while it damages only its own rect.
    { "confirm",  open_confirm,  draw_confirm,     confirm_dialog_handle_click,
      confirm_dialog_hover_at,   confirm_dialog_damage,   confirm_dialog_rect, confirm_dialog_update_press, 0, 0, 0, 0,
      confirm_covers, 0, 1, WM_OVERLAY_REPAINT_ON_CLOSE },
    { "context",  open_context,  context_menu_draw, context_menu_handle_click,
      context_menu_hover_at,     context_menu_damage,     0 /* a rect per submenu level */, 0, 0, context_menu_key, context_menu_close, 0,
      context_menu_contains, 0, 0, WM_OVERLAY_REPAINT_ON_CLOSE },
    // The Leave page (leave_page.h): full-screen and modal (no `close`,
    // so a menu opening cannot shut it), BELOW "context" so its "Restart
    // into" menu draws over it and is clicked first, and below "confirm"
    // so a Force Quit raised while apps close lands on top.
    { "leave",    open_leave,    leave_page_draw,  leave_page_handle_click,
      leave_page_hover_at,       leave_page_damage,       0 /* the whole screen */, leave_page_update_press, 0, leave_page_key, 0, 0, 0, 0, 1,
      WM_OVERLAY_REPAINT_WHILE_OPEN },
    { "start",    open_start,    start_menu_draw,  start_menu_handle_click,
      start_menu_hover_at,       start_menu_damage,       start_menu_rect, start_menu_update_press, start_menu_wheel, start_menu_key, start_menu_close, 0, 0, 0, 0, 0 },
    { "calendar", open_calendar, calendar_draw,    calendar_handle_click,
      calendar_hover_at,         calendar_damage,         calendar_rect, 0, 0, 0, calendar_close, 0, 0, 0, 0, 0 },
    { "volume",   open_volume,   volume_draw,      volume_handle_click,
      volume_hover_at,           volume_damage,           volume_rect, volume_update_press, 0, 0, volume_close, volume_opened, 0, 0, 0, 0 },
    { "brightness", open_brightness, brightness_draw, brightness_handle_click,
      brightness_hover_at,       brightness_damage,       brightness_rect, brightness_update_press, 0, 0, brightness_close, 0, 0, 0, 0, 0 },
    { "network",  open_network,  network_draw,     network_handle_click,
      network_hover_at,          network_damage,          network_rect, 0, 0, 0, network_close, 0, 0, 0, 0, 0 },
    { "remote",   open_remote,   remote_draw,      remote_handle_click,
      remote_hover_at,           remote_damage,           remote_rect, 0, 0, 0, remote_close, 0, 0, 0, 0, 0 },
    { "layout",   open_layout,   layout_draw,      layout_handle_click,
      layout_hover_at,           layout_damage,           layout_rect, 0, 0, layout_key, layout_close, 0, 0, 0, 0, 0 },
    // Under every menu (drawn before them), above the windows: a crash
    // notice in the corner. Passive -- see wm_overlay.h.
    { "notice",   open_notice,   crash_notice_draw, crash_notice_handle_click,
      crash_notice_hover_at,     crash_notice_damage,     crash_notice_rect, 0, 0, 0, 0, 0, 0, 1, 0, 0 },
    // LAST, so it is the least modal: a menu overlapping the keyboard
    // takes the click and paints on top. No `close` op -- a keyboard
    // must survive the click that puts the caret where it is typing.
    { "osk",      open_osk,      osk_draw,         osk_handle_click,
      osk_hover_at,              osk_damage,              osk_rect, osk_update_press, 0, 0, 0, 0, 0, 0, 0, 0 },
};
#define OVERLAY_COUNT ((int)(sizeof g_overlays / sizeof g_overlays[0]))

// The hovered token per overlay, parallel to the table. Here rather
// than in each overlay because the compare-and-damage is the part they
// were each getting slightly differently.
static int g_hover[OVERLAY_COUNT];

static const char *g_parent;   // see wm_overlay_set_parent()

void wm_overlay_set_parent(const char *name) { g_parent = name; }

int wm_overlay_modal_open(void) {
    for (int i = 0; i < OVERLAY_COUNT; i++)
        if (g_overlays[i].modal && g_overlays[i].is_open()) return 1;
    return 0;
}

int wm_overlay_any_open(void) {
    for (int i = 0; i < OVERLAY_COUNT; i++)
        if (!g_overlays[i].passive && g_overlays[i].is_open()) return 1;
    return 0;
}
const char *wm_overlay_parent(void) { return g_parent; }

// WHERE EACH OVERLAY WAS LAST DRAWN, so damaging it can cover the rect
// it has since left. Kept HERE rather than in each overlay because the
// record-after-draw is the part they would forget -- the same argument
// the hover compare above is built on. `w` of 0 means "nothing on screen
// to cover": never drawn, or closed and its rect already damaged by
// wm_overlay_frame_begin() before the draw pass cleared it.
static struct { int x, y, w, h; } g_drawn[OVERLAY_COUNT];

// Was it open as of the last draw pass? The core owns this so an
// overlay's on_open can fire from ONE place -- see wm_overlay.h on why
// each overlay's own open path is the wrong place for it.
static int g_was_open[OVERLAY_COUNT];

void wm_overlay_draw(int mx, int my) {
    for (int i = OVERLAY_COUNT - 1; i >= 0; i--) {
        const struct wm_overlay *o = &g_overlays[i];
        // The is_open() check is the CORE's, but each draw() still
        // checks for itself -- context_menu_draw() always did, and a
        // draw that is safe to call closed is one less thing for a new
        // overlay to get wrong.
        int open = o->is_open();
        // THE TRANSITION, before the first draw of the overlay that
        // just opened -- this pass runs after input, so a click that
        // opened it is handled in the frame that click belongs to.
        if (open && !g_was_open[i] && o->on_open) o->on_open();
        // AN OVERLAY THAT OPENS OR CLOSES BETWEEN RENDER PASSES makes
        // the damage verifier's two renders disagree about a whole
        // panel, which reads as a missed damage declaration. Only while
        // verification is on: a tooltip transitions on every hover.
        if (open != g_was_open[i] && wm_damage_verify_enabled())
            wm_logf("wm: overlay %s %s\n", o->name, open ? "opened" : "closed");
        g_was_open[i] = open;
        if (!open) {
            // wm_overlay_frame_begin() damaged where it was this frame,
            // so the record has done its job; keeping it would damage a
            // dead rect on every later call.
            g_drawn[i].w = 0;
            continue;
        }
        o->draw(mx, my);
        int x, y, w, h;
        if (o->rect && o->rect(&x, &y, &w, &h))
            g_drawn[i] = (typeof(g_drawn[0])){ x, y, w, h };
    }
}

// Drawn by the last pass and shut now: the ONE copy of "this close still
// needs its frame", asked by the render gate and again by the frame.
static int closed_since_drawn(int i) { return g_was_open[i] && !g_overlays[i].is_open(); }

int wm_overlay_close_pending(void) {
    for (int i = 0; i < OVERLAY_COUNT; i++)
        if (closed_since_drawn(i)) return 1;
    return 0;
}

// THE CLOSE IS THE CORE'S TO DAMAGE: an overlay that was drawn last frame
// and is shut now has its last drawn rect, shadow included, damaged here
// -- whoever closed it, by whatever path. Before the frame's damage is
// final, so after input and before wm_overlay_draw() clears the record.
int wm_overlay_frame_begin(void) {
    int full = 0;
    for (int i = 0; i < OVERLAY_COUNT; i++) {
        int now = g_overlays[i].is_open();
        int r = g_overlays[i].repaint;
        if (closed_since_drawn(i)) {
            if (g_drawn[i].w > 0)
                wm_damage_window_rect(g_drawn[i].x, g_drawn[i].y, g_drawn[i].w, g_drawn[i].h);
            if (r) full = 1;
        }
        if (r == WM_OVERLAY_REPAINT_WHILE_OPEN && now) full = 1;
    }
    return full;
}

void wm_overlay_reset(void) {
    for (int i = 0; i < OVERLAY_COUNT; i++) {
        g_was_open[i] = 0;
        g_drawn[i].w = 0;
    }
}

// **AN OVERLAY CAN MOVE WITH NOBODY TOUCHING IT, and the rect it left
// has to be covered.** A tray popup is anchored to its tray ITEM, and
// the tray's layout is not fixed: an icon appearing or going away
// shifts everything left of it by its own width, and the clock's width
// changes with its digits on a proportional font. The panel then paints
// at the new anchor and the pixels at the old one stay on screen --
// measured on the laptop as a 46 px jump (one tray item) and a 1 px one
// (a clock digit), which is the tray panel's leftover band in
// docs/bugs.md.
//
// Called from the poll phase, BEFORE the frame's clip is derived, so
// the repair lands on the same frame rather than the next one.
void wm_overlay_poll_geometry(void) {
    for (int i = 0; i < OVERLAY_COUNT; i++) {
        const struct wm_overlay *o = &g_overlays[i];
        if (!o->is_open() || !o->rect) continue;
        if (g_drawn[i].w <= 0) continue;      // never drawn: nothing to cover
        int x, y, w, h;
        if (!o->rect(&x, &y, &w, &h)) continue;
        if (g_drawn[i].x == x && g_drawn[i].y == y &&
            g_drawn[i].w == w && g_drawn[i].h == h) continue;
        // Both rects: the one being vacated and the one being taken.
        wm_damage_window_rect(g_drawn[i].x, g_drawn[i].y, g_drawn[i].w, g_drawn[i].h);
        wm_damage_window_rect(x, y, w, h);
        redraw_pending = 1;
    }
}

void wm_overlay_damage(const char *name) {
    for (int i = 0; i < OVERLAY_COUNT; i++) {
        if (k_strcmp(g_overlays[i].name, name) != 0) continue;
        int x, y, w, h;
        // WINDOW rect, so the shadow outside it is covered too -- an
        // overlay never has to know whether it draws one.
        if (g_overlays[i].rect && g_overlays[i].rect(&x, &y, &w, &h))
            wm_damage_window_rect(x, y, w, h);
        if (g_drawn[i].w > 0)
            wm_damage_window_rect(g_drawn[i].x, g_drawn[i].y,
                                  g_drawn[i].w, g_drawn[i].h);
        redraw_pending = 1;
        return;
    }
}

int wm_overlay_under(int mx, int my) {
    for (int i = 0; i < OVERLAY_COUNT; i++) {
        const struct wm_overlay *o = &g_overlays[i];
        if (!o->is_open() || o->is_open == open_tooltip) continue;
        if (o->contains) {
            if (o->contains(mx, my)) return 1;
            continue;
        }
        int x, y, w, h;
        if (o->rect) {
            if (o->rect(&x, &y, &w, &h) && uui_hit(x, y, w, h, mx, my)) return 1;
            continue;
        }
        return 1;
    }
    return 0;
}

int wm_overlay_click(int mx, int my) {
    for (int i = 0; i < OVERLAY_COUNT; i++) {
        const struct wm_overlay *o = &g_overlays[i];
        if (o->handle_click(mx, my)) return 1;
    }
    return 0;
}

int wm_overlay_hover(int mx, int my, uint8_t buttons) {
    // See the header: a control being dragged or armed keeps its
    // highlight, so nothing re-hovers under a held button.
    if (buttons & 0x1) return 0;

    int changed = 0;
    for (int i = 0; i < OVERLAY_COUNT; i++) {
        const struct wm_overlay *o = &g_overlays[i];
        if (!o->hover_at) continue;
        // A PARENT DOES NOT HOVER WHILE ITS CHILD IS UP. The popup is
        // drawn over it, so a row lighting up under the menu belongs to
        // neither -- it is the Start menu highlighting whatever the
        // context menu happens to be covering. Hovered at a point that
        // is on nothing, so it also DROPS the row it was holding
        // (every overlay's rect is on screen, so (-1,-1) is off all of
        // them) rather than freezing the last one lit.
        int parent = g_parent && k_strcmp(o->name, g_parent) == 0;
        int want = o->is_open() ? o->hover_at(parent ? -1 : mx,
                                              parent ? -1 : my) : 0;
        if (want == g_hover[i]) continue;
        g_hover[i] = want;
        // Damaged only while it is up: a closed overlay's rect is the
        // core's to damage (wm_overlay_frame_begin()).
        if (o->is_open()) o->damage();
        changed = 1;
    }
    return changed;
}

int wm_overlay_wheel(int mx, int my, int notches) {
    for (int i = 0; i < OVERLAY_COUNT; i++) {
        const struct wm_overlay *o = &g_overlays[i];
        if (!o->wheel || !o->is_open()) continue;
        if (o->wheel(mx, my, notches)) return 1;
    }
    return 0;
}

int wm_overlay_key(int key, uint8_t mods) {
    for (int i = 0; i < OVERLAY_COUNT; i++) {
        const struct wm_overlay *o = &g_overlays[i];
        if (!o->key || !o->is_open()) continue;
        if (o->key(key, mods)) return 1;
    }
    return 0;
}

void wm_overlay_press(int mx, int my, uint8_t buttons) {
    for (int i = 0; i < OVERLAY_COUNT; i++) {
        const struct wm_overlay *o = &g_overlays[i];
        if (o->update_press) o->update_press(mx, my, buttons);
    }
}

// The table, enumerable -- so `gui state` reports every overlay by
// walking it rather than naming each one. The hand-written list had
// already lost `osk` from its JSON when this was added.
int wm_overlay_count(void) { return OVERLAY_COUNT; }

const char *wm_overlay_name(int i) {
    if (i < 0 || i >= OVERLAY_COUNT) return "";
    return g_overlays[i].name;
}

int wm_overlay_is_open(int i) {
    if (i < 0 || i >= OVERLAY_COUNT) return 0;
    return g_overlays[i].is_open && g_overlays[i].is_open();
}

const char *wm_overlay_topmost(void) {
    for (int i = 0; i < OVERLAY_COUNT; i++)
        if (g_overlays[i].is_open()) return g_overlays[i].name;
    return 0;
}

void wm_overlay_close_others(const char *keep) {
    for (int i = 0; i < OVERLAY_COUNT; i++) {
        const struct wm_overlay *o = &g_overlays[i];
        if (!o->close) continue;
        if (keep && k_strcmp(o->name, keep) == 0) continue;
        if (g_parent && k_strcmp(o->name, g_parent) == 0) continue;
        if (o->is_open()) o->close();
    }
}

void wm_popup_place(int want_x, int want_y, int w, int h, int *out_x, int *out_y) {
    int x = want_x, y = want_y;
    if (x + w > screen_w - WM_POPUP_MARGIN) x = screen_w - WM_POPUP_MARGIN - w;
    if (x < WM_POPUP_MARGIN) x = WM_POPUP_MARGIN;
    if (y + h > screen_h - taskbar_h - WM_POPUP_MARGIN)
        y = screen_h - taskbar_h - WM_POPUP_MARGIN - h;
    if (y < 0) y = 0;
    *out_x = x;
    *out_y = y;
}
