// See context_menu.h for the design writeup.
#include "context_menu.h"
#include "wm_internal.h"
#include "wm_overlay.h"
#include "ui/uui.h"
#include "ui/utheme.h"
#include "kapi.h"

int context_menu_open = 0;

// One menu level: the rows, and where they sit. The main menu and the
// open submenu are two of these; a submenu's `parent` is the main row
// that opened it, so hovering another main row closes it.
struct level {
    const struct context_menu_item *items;
    int count;
    int x, y, w, h;
};

static struct level g_main, g_sub;
static int g_sub_parent = -1;   // main row whose submenu is open, or -1
static int g_item_h;

static int item_h(void) { return ugfx_char_h() + 6; }
static int sep_h(void) { return item_h() / 2; }
// The tick gutter, so a checked row's tick never overlaps its label.
static int gutter(void) { return ugfx_char_w() * 2; }

static int row_h(const struct context_menu_item *it) {
    return it->separator ? sep_h() : g_item_h;
}

static int level_h(const struct context_menu_item *items, int count) {
    int h = 0;
    for (int i = 0; i < count; i++) h += row_h(&items[i]);
    return h;
}

static int row_top(const struct level *l, int index) {
    int y = 0;
    for (int i = 0; i < index && i < l->count; i++) y += row_h(&l->items[i]);
    return y;
}

static int menu_w(const struct context_menu_item *items, int count) {
    int max_chars = 0;
    for (int i = 0; i < count; i++) {
        if (items[i].separator) continue;
        int n = (int)k_strlen(items[i].label) + (items[i].sub ? 2 : 0);
        if (n > max_chars) max_chars = n;
    }
    return max_chars * ugfx_char_w() + gutter() + 20;
}

static void place(struct level *l, int x, int y, const struct context_menu_item *items, int count) {
    l->items = items;
    l->count = count;
    l->w = menu_w(items, count);
    l->h = level_h(items, count);
    // At the pointer, kept whole on screen -- a right-click near the
    // taskbar or an edge would otherwise draw partly off it.
    wm_popup_place(x, y, l->w, l->h, &l->x, &l->y);
}

static void damage_level(const struct level *l) {
    wm_damage_rect(l->x, l->y, l->w, l->h);
}

void context_menu_open_at(int x, int y, const struct context_menu_item *items, int count) {
    g_item_h = item_h();
    place(&g_main, x, y, items, count);
    g_sub.count = 0;
    g_sub_parent = -1;

    wm_overlay_close_others("context");
    context_menu_open = 1;
    redraw_pending = 1;
}

int context_menu_geometry(int *x, int *y, int *w, int *item_h_out) {
    if (!context_menu_open) return 0;
    if (x) *x = g_main.x;
    if (y) *y = g_main.y;
    if (w) *w = g_main.w;
    if (item_h_out) *item_h_out = g_item_h;
    return g_main.count;
}

int context_menu_sub_geometry(int *x, int *y, int *w, int *item_h_out) {
    if (!context_menu_open || g_sub_parent < 0) return 0;
    if (x) *x = g_sub.x;
    if (y) *y = g_sub.y;
    if (w) *w = g_sub.w;
    if (item_h_out) *item_h_out = g_item_h;
    return g_sub.count;
}

const char *context_menu_row_label(int index) {
    if (!context_menu_open || index < 0 || index >= g_main.count) return 0;
    return g_main.items[index].separator ? "-" : g_main.items[index].label;
}

const char *context_menu_sub_row_label(int index) {
    if (!context_menu_open || g_sub_parent < 0 || index < 0 || index >= g_sub.count) return 0;
    return g_sub.items[index].separator ? "-" : g_sub.items[index].label;
}

int context_menu_row_top(int index) { return g_main.y + row_top(&g_main, index); }
int context_menu_sub_row_top(int index) { return g_sub.y + row_top(&g_sub, index); }

void context_menu_close(void) {
    context_menu_open = 0;
    g_sub_parent = -1;
    redraw_pending = 1;
}

// Which row of `l` the cursor is over, or -1. Recomputed from the live
// cursor rather than stored, exactly as start_menu.c does it -- one
// geometry, used by drawing and hit-testing alike, so the two cannot
// disagree about which row is which (docs/gui-guidelines.md).
static int hot_row_in(const struct level *l, int mx, int my) {
    if (!l->count || !uui_hit(l->x, l->y, l->w, l->h, mx, my)) return -1;
    int y = l->y;
    for (int i = 0; i < l->count; i++) {
        int h = row_h(&l->items[i]);
        if (my < y + h) return l->items[i].separator ? -1 : i;
        y += h;
    }
    return -1;
}

// Open (or switch) the submenu of main row `i`, to the right of it and
// level with it; a row without one closes whatever is open. Hover does
// this, as every desktop's menu does -- a click on the row does too.
static void open_sub(int i) {
    if (i == g_sub_parent) return;
    if (g_sub_parent >= 0) damage_level(&g_sub);
    g_sub_parent = -1;
    g_sub.count = 0;
    if (i < 0 || !g_main.items[i].sub) return;
    place(&g_sub, g_main.x + g_main.w - 2, g_main.y + row_top(&g_main, i),
          g_main.items[i].sub, g_main.items[i].sub_count);
    g_sub_parent = i;
    damage_level(&g_sub);
    redraw_pending = 1;
}

// A tick, drawn with two strokes: the font has no U+2713.
static void draw_tick(int x, int y, int h, uint32_t fg) {
    int cy = y + h / 2;
    for (int k = 0; k < 3; k++) ugfx_fill_rect(wm_surface(), x + k, cy + k, 2, 2, fg);
    for (int k = 0; k < 6; k++) ugfx_fill_rect(wm_surface(), x + 3 + k, cy + 2 - k, 2, 2, fg);
}

static void draw_level(const struct level *l, int hot) {
    uint32_t bg = UTHEME_PANEL_BG, border = UTHEME_BORDER, fg = UTHEME_TEXT;
    ugfx_fill_rect(wm_surface(), l->x, l->y, l->w, l->h, bg);
    int y = l->y;
    for (int i = 0; i < l->count; i++) {
        const struct context_menu_item *it = &l->items[i];
        int h = row_h(it);
        if (it->separator) {
            ugfx_fill_rect(wm_surface(), l->x + 6, y + h / 2, l->w - 12, 1, border);
            y += h;
            continue;
        }
        // Hover from uui_state_bg() rather than a hand-picked tint, so
        // it darkens on this near-white theme instead of lightening
        // into invisibility (docs/gui-guidelines.md).
        uint32_t row_bg = (i == hot) ? uui_state_bg(bg, UUI_STATE_HOVER) : bg;
        if (i == hot) ugfx_fill_rect(wm_surface(), l->x, y, l->w, h, row_bg);
        if (it->checked) draw_tick(l->x + 6, y, h, fg);
        // Clipped: a label longer than the menu is wide would otherwise
        // be drawn straight through the border.
        int lx = l->x + gutter() + 4;
        int lw = l->w - gutter() - 8 - (it->sub ? 2 * ugfx_char_w() : 0);
        ugfx_draw_string_clipped(wm_surface(), lx, y + 3, lw, it->label, fg, row_bg);
        if (it->sub)
            ugfx_draw_string_clipped(wm_surface(), l->x + l->w - 4 - ugfx_char_w(), y + 3,
                                      ugfx_char_w(), ">", fg, row_bg);
        y += h;
    }
    ugfx_draw_rect(wm_surface(), l->x, l->y, l->w, l->h, border);
}

void context_menu_draw(int mx, int my) {
    if (!context_menu_open) return;
    int sub_hot = g_sub_parent >= 0 ? hot_row_in(&g_sub, mx, my) : -1;
    // The parent row stays lit while its submenu is open, whatever the
    // pointer is over now.
    int main_hot = g_sub_parent >= 0 ? g_sub_parent : hot_row_in(&g_main, mx, my);
    draw_level(&g_main, main_hot);
    if (g_sub_parent >= 0) draw_level(&g_sub, sub_hot);
}

// The registry's hover op (wm_overlay.h). A hovered main row with a
// submenu OPENS it here, which is why this is not a pure query: the
// token changes, the core damages, and the submenu appears on the
// next frame.
int context_menu_hover_at(int mx, int my) {
    if (!context_menu_open) return 0;
    int sub = g_sub_parent >= 0 ? hot_row_in(&g_sub, mx, my) : -1;
    if (sub >= 0) return 1000 + sub;
    int main = hot_row_in(&g_main, mx, my);
    if (main >= 0 && g_main.items[main].sub) open_sub(main);
    else if (main >= 0) open_sub(-1);
    return main + 1;   // -1 becomes 0, "none"
}

void context_menu_damage(void) {
    if (!context_menu_open) return;
    damage_level(&g_main);
    if (g_sub_parent >= 0) damage_level(&g_sub);
    redraw_pending = 1;
}

int context_menu_handle_click(int mx, int my) {
    if (!context_menu_open) return 0;

    int sub = g_sub_parent >= 0 ? hot_row_in(&g_sub, mx, my) : -1;
    if (sub >= 0) {
        const struct context_menu_item *it = &g_sub.items[sub];
        if (it->on_select) it->on_select(it->ctx);
    } else {
        int main = hot_row_in(&g_main, mx, my);
        if (main >= 0 && g_main.items[main].sub) {
            open_sub(main);        // a click on a parent row opens it and stays
            return 1;
        }
        if (main >= 0 && g_main.items[main].on_select)
            g_main.items[main].on_select(g_main.items[main].ctx);
    }
    // Unlike start_menu.c's brief post-click flash, this closes
    // immediately whether or not a row was hit -- a right-click menu is
    // a short-lived popup by convention (real desktops don't flash a
    // context-menu selection either), and there's no taskbar button
    // still showing "open" the way the Start button's highlight does.
    context_menu_open = 0;
    g_sub_parent = -1;
    redraw_pending = 1;
    return 1;
}
