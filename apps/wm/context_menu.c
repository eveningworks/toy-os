// See context_menu.h for the design writeup.
#include "context_menu.h"
#include "wm_internal.h"
#include "ui/ui.h"
#include "theme.h"
#include "kapi.h"

int context_menu_open = 0;

static const struct context_menu_item *g_items;
static int g_count;
static int g_x, g_y, g_w, g_item_h;

static int item_h(void) { return gfx_char_h() + 6; }

static int menu_w(const struct context_menu_item *items, int count) {
    int max_chars = 0;
    for (int i = 0; i < count; i++) {
        int n = (int)k_strlen(items[i].label);
        if (n > max_chars) max_chars = n;
    }
    return max_chars * gfx_char_w() + 20;
}

void context_menu_open_at(int x, int y, const struct context_menu_item *items, int count) {
    g_items = items;
    g_count = count;
    g_item_h = item_h();
    g_w = menu_w(items, count);
    int h = g_item_h * count;

    // Clamp so the whole menu stays on screen -- a right-click near the
    // taskbar or the right/bottom edge would otherwise draw partly off
    // it. taskbar_h/screen_w/screen_h come from wm_internal.h.
    g_x = x;
    g_y = y;
    if (g_x + g_w > screen_w) g_x = screen_w - g_w;
    if (g_x < 0) g_x = 0;
    if (g_y + h > screen_h - taskbar_h) g_y = screen_h - taskbar_h - h;
    if (g_y < 0) g_y = 0;

    context_menu_open = 1;
    redraw_pending = 1;
}

void context_menu_close(void) {
    context_menu_open = 0;
    redraw_pending = 1;
}

// Which row the cursor is over, or -1. Recomputed from the live cursor
// on every draw rather than stored, exactly as start_menu.c does it --
// one geometry, used by drawing and hit-testing alike, so the two cannot
// disagree about which row is which (docs/gui-guidelines.md).
static int hot_row(int mx, int my) {
    if (!widget_hit(g_x, g_y, g_w, g_item_h * g_count, mx, my)) return -1;
    int idx = (my - g_y) / g_item_h;
    return (idx >= 0 && idx < g_count) ? idx : -1;
}

void context_menu_draw(int mx, int my) {
    if (!context_menu_open) return;
    int h = g_item_h * g_count;

    uint32_t bg = THEME_PANEL_BG, border = THEME_BORDER, fg = THEME_TEXT;
    gfx_fill_rect(g_x, g_y, g_w, h, bg);

    // Hover had to be added: this menu's rows never highlighted at all,
    // because draw() was never given the cursor. A menu whose rows don't
    // react reads as inert -- "any control that can be pressed shows
    // hover", docs/gui-guidelines.md. The wash comes from ui_state_bg()
    // rather than a hand-picked tint, so it darkens on this near-white
    // theme instead of lightening into invisibility.
    int hot = hot_row(mx, my);
    for (int i = 0; i < g_count; i++) {
        int y = g_y + i * g_item_h;
        uint32_t row_bg = (i == hot) ? ui_state_bg(bg, UI_STATE_HOVER) : bg;
        if (i == hot) gfx_fill_rect(g_x, y, g_w, g_item_h, row_bg);
        // Clipped: a label longer than the menu is wide would otherwise
        // be drawn straight through the border.
        gfx_draw_string_clipped(g_x + 8, y + 3, g_w - 16, g_items[i].label, fg, row_bg);
    }
    gfx_draw_rect(g_x, g_y, g_w, h, border);
}

int context_menu_handle_click(int mx, int my) {
    if (!context_menu_open) return 0;

    int h = g_item_h * g_count;
    if (widget_hit(g_x, g_y, g_w, h, mx, my)) {
        int idx = (my - g_y) / g_item_h;
        if (idx >= 0 && idx < g_count) g_items[idx].on_select(g_items[idx].ctx);
    }
    // Unlike start_menu.c's brief post-click flash, this closes
    // immediately whether or not a row was hit -- a right-click menu is
    // a short-lived popup by convention (real desktops don't flash a
    // context-menu selection either), and there's no taskbar button
    // still showing "open" the way the Start button's highlight does.
    context_menu_open = 0;
    redraw_pending = 1;
    return 1;
}
