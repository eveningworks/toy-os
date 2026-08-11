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

void context_menu_draw(void) {
    if (!context_menu_open) return;
    int h = g_item_h * g_count;

    uint32_t bg = THEME_PANEL_BG, border = THEME_BORDER, fg = THEME_TEXT;
    gfx_fill_rect(g_x, g_y, g_w, h, bg);
    for (int i = 0; i < g_count; i++) {
        int y = g_y + i * g_item_h;
        gfx_draw_string(g_x + 8, y + 3, g_items[i].label, fg, bg);
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
