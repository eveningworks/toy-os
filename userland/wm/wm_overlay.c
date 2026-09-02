// See wm_overlay.h for what this is and why the table drives three
// verbs rather than one.
#include "wm_internal.h"
#include "wm_overlay.h"
#include "start_menu.h"
#include "context_menu.h"
#include "calendar_popup.h"
#include "volume_popup.h"
#include "brightness_popup.h"
#include "confirm_dialog.h"
#include "file_picker.h"

// The two that take no cursor, adapted rather than changed: their
// drawing genuinely does not depend on where the pointer is.
static void draw_file_picker(int mx, int my) { (void)mx; (void)my; file_picker_draw(); }
static void draw_confirm(int mx, int my)     { (void)mx; (void)my; confirm_dialog_draw(); }

static int open_start(void)   { return start_menu_open; }
static int open_context(void) { return context_menu_open; }
static int open_calendar(void){ return calendar_open; }
static int open_volume(void)  { return volume_open; }
static int open_brightness(void) { return brightness_open; }
static int open_picker(void)  { return file_picker_open; }
static int open_confirm(void) { return confirm_dialog_open; }

// MOST MODAL FIRST. This order is the click priority -- a modal dialog
// takes a click before a menu does -- and drawing walks it BACKWARDS,
// so the same row that gets the first click is painted last and lands
// on top. Keeping one table for both is what stops the two orders from
// drifting apart, which they had already started to do.
static const struct wm_overlay g_overlays[] = {
    { "confirm",  open_confirm,  draw_confirm,     confirm_dialog_handle_click,
      confirm_dialog_hover_at,   confirm_dialog_damage,   confirm_dialog_update_press },
    { "picker",   open_picker,   draw_file_picker, file_picker_handle_click,
      file_picker_hover_at,      file_picker_damage,      file_picker_update_press },
    { "context",  open_context,  context_menu_draw, context_menu_handle_click,
      context_menu_hover_at,     context_menu_damage,     0 },
    { "start",    open_start,    start_menu_draw,  start_menu_handle_click,
      start_menu_hover_at,       start_menu_damage,       0 },
    { "calendar", open_calendar, calendar_draw,    calendar_handle_click,
      calendar_hover_at,         calendar_damage,         0 },
    { "volume",   open_volume,   volume_draw,      volume_handle_click,
      volume_hover_at,           volume_damage,           volume_update_press },
    { "brightness", open_brightness, brightness_draw, brightness_handle_click,
      brightness_hover_at,       brightness_damage,       brightness_update_press },
};
#define OVERLAY_COUNT ((int)(sizeof g_overlays / sizeof g_overlays[0]))

// The hovered token per overlay, parallel to the table. Here rather
// than in each overlay because the compare-and-damage is the part they
// were each getting slightly differently.
static int g_hover[OVERLAY_COUNT];

void wm_overlay_draw(int mx, int my) {
    for (int i = OVERLAY_COUNT - 1; i >= 0; i--) {
        const struct wm_overlay *o = &g_overlays[i];
        // The is_open() check is the CORE's, but each draw() still
        // checks for itself -- context_menu_draw() always did, and a
        // draw that is safe to call closed is one less thing for a new
        // overlay to get wrong.
        if (o->is_open()) o->draw(mx, my);
    }
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
        int want = o->is_open() ? o->hover_at(mx, my) : 0;
        if (want == g_hover[i]) continue;
        g_hover[i] = want;
        // Damaged only while it is up: a closing overlay damages its
        // own rect on the way out, and doing it again from here would
        // ask for a repaint of a panel nothing is drawing.
        if (o->is_open()) o->damage();
        changed = 1;
    }
    return changed;
}

void wm_overlay_press(int mx, int my, uint8_t buttons) {
    for (int i = 0; i < OVERLAY_COUNT; i++) {
        const struct wm_overlay *o = &g_overlays[i];
        if (o->update_press) o->update_press(mx, my, buttons);
    }
}

const char *wm_overlay_topmost(void) {
    for (int i = 0; i < OVERLAY_COUNT; i++)
        if (g_overlays[i].is_open()) return g_overlays[i].name;
    return 0;
}
