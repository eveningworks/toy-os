// A grid of equal cells (ui/uui_grid.h).
#include "ui/uui_grid.h"
#include "ui/uui_primitives.h"
#include "ui/uui_scrollbar.h"
#include "ui/utheme.h"
#include "rt/sys.h"
#include "keyboard.h"

#include <string.h>

#define DOUBLE_NS 400000000ull

void uui_grid_init(struct uui_grid *g, int cell_w, int cell_h) {
    memset(g, 0, sizeof *g);
    g->cell_w = cell_w > 0 ? cell_w : 32;
    g->cell_h = cell_h > 0 ? cell_h : 32;
    g->selected = g->hot = g->armed = g->activated = g->last_click = -1;
    g->bg = UTHEME_WHITE;
}

static int bar_w(void) { return uui_scrollbar_overlay_width(); }

int uui_grid_columns(const struct uui_grid *g) {
    int c = (g->w - bar_w()) / g->cell_w;
    return c > 0 ? c : 1;
}

// The cell pitch after widening: the leftover shared between columns.
static int pitch_x(const struct uui_grid *g) { return (g->w - bar_w()) / uui_grid_columns(g); }

static int rows(const struct uui_grid *g) {
    int c = uui_grid_columns(g);
    return (g->count + c - 1) / c;
}

static int content_h(const struct uui_grid *g) { return rows(g) * g->cell_h; }

static void clamp_scroll(struct uui_grid *g) {
    int max = content_h(g) - g->h;
    if (g->scroll > max) g->scroll = max;
    if (g->scroll < 0) g->scroll = 0;
}

void uui_grid_set(struct uui_grid *g, int count, uui_grid_cell_fn cell, void *ctx) {
    g->count = count;
    g->cell = cell;
    g->ctx = ctx;
    if (g->selected >= count) g->selected = count - 1;
    g->hot = g->armed = -1;
    clamp_scroll(g);
}

void uui_grid_select(struct uui_grid *g, int index) {
    if (index < -1 || index >= g->count) return;
    g->selected = index;
    if (index < 0) return;
    int top = index / uui_grid_columns(g) * g->cell_h;
    if (top < g->scroll) g->scroll = top;
    else if (top + g->cell_h > g->scroll + g->h) g->scroll = top + g->cell_h - g->h;
    clamp_scroll(g);
}

int uui_grid_take(struct uui_grid *g) {
    int a = g->activated;
    g->activated = -1;
    return a;
}

int uui_grid_cell_rect(const struct uui_grid *g, int index, int *x, int *y, int *w, int *h) {
    if (index < 0 || index >= g->count) return 0;
    int c = uui_grid_columns(g), px = pitch_x(g);
    int cy = g->y + index / c * g->cell_h - g->scroll;
    *x = g->x + index % c * px;
    *y = cy;
    *w = px;
    *h = g->cell_h;
    return cy + g->cell_h > g->y && cy < g->y + g->h;
}

static int cell_at(const struct uui_grid *g, int cx, int cy) {
    if (!uui_hit(g->x, g->y, g->w - bar_w(), g->h, cx, cy)) return -1;
    int col = (cx - g->x) / pitch_x(g), row = (cy - g->y + g->scroll) / g->cell_h;
    if (col >= uui_grid_columns(g)) return -1;
    int i = row * uui_grid_columns(g) + col;
    return i < g->count ? i : -1;
}

static int on_bar(const struct uui_grid *g, int cx, int cy) {
    return content_h(g) > g->h && uui_hit(g->x + g->w - bar_w(), g->y, bar_w(), g->h, cx, cy);
}

// --- ops ------------------------------------------------------------------

static void grid_natural(const void *w, int *ow, int *oh) {
    const struct uui_grid *g = w;
    *ow = g->cell_w * 8 + bar_w();
    *oh = g->cell_h * 4;
}

static void grid_geometry(void *w, int x, int y, int width, int height) {
    struct uui_grid *g = w;
    g->x = x; g->y = y; g->w = width; g->h = height;
    clamp_scroll(g);
}

static void grid_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_grid *g = w;
    *x = g->x; *y = g->y; *ow = g->w; *oh = g->h;
}

static void grid_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_grid *g = w;
    ugfx_fill_rect(s, g->x, g->y, g->w, g->h, g->bg);
    if (!g->cell) return;
    struct ugfx_clip saved;
    ugfx_clip_save(s, &saved);
    ugfx_clip_intersect(s, g->x, g->y, g->w, g->h);
    int c = uui_grid_columns(g), px = pitch_x(g);
    int first = g->scroll / g->cell_h * c;
    for (int i = first; i < g->count; i++) {
        int x = g->x + i % c * px, y = g->y + i / c * g->cell_h - g->scroll;
        if (y >= g->y + g->h) break;
        int st = 0;
        if (i == g->selected) {
            st |= UUI_GRID_SELECTED | (g->focused ? UUI_GRID_FOCUSED : 0);
            uui_fill_round_rect(s, x + 2, y + 2, px - 4, g->cell_h - 4, 6, UTHEME_ACCENT);
        } else if (i == g->hot) {
            st |= UUI_GRID_HOT;
            uui_fill_round_rect(s, x + 2, y + 2, px - 4, g->cell_h - 4, 6, uui_state_bg(g->bg, UUI_STATE_HOVER));
        }
        g->cell(s, g->ctx, i, x, y, px, g->cell_h, st);
    }
    ugfx_clip_restore(s, &saved);
    uui_scrollbar_draw_overlay(s, g->x + g->w - bar_w(), g->y, bar_w(), g->h, content_h(g), g->h,
                               g->scroll, g->bg, UTHEME_TEXT, (g->bar_hot || g->bar_drag) ? 255 : 0,
                               g->bar_drag ? UUI_SCROLLBAR_HELD : 0);
}

// The whole rect: the overlay bar is inside it and must stay live.
static int grid_hit(const void *w, int cx, int cy) {
    const struct uui_grid *g = w;
    return uui_hit(g->x, g->y, g->w, g->h, cx, cy);
}

static int grid_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_grid *g = w;
    if (on_bar(g, cx, cy)) {
        int zone = uui_scrollbar_hit(g->x + g->w - bar_w(), g->y, bar_w(), g->h, content_h(g), g->h,
                                     g->scroll, cx, cy, 0);
        if (zone == UUI_SB_THUMB) {
            int ty, th;
            uui_scrollbar_thumb_rect(g->y, g->h, content_h(g), g->h, g->scroll, &ty, &th, bar_w(), 0);
            g->bar_drag = 1;
            g->bar_grab = cy - ty;
        } else if (zone == UUI_SB_ABOVE) {
            g->scroll -= g->h;
        } else if (zone == UUI_SB_BELOW) {
            g->scroll += g->h;
        }
        clamp_scroll(g);
        return 1;
    }
    g->armed = cell_at(g, cx, cy);
    if (g->armed >= 0) g->selected = g->armed;
    return uui_hit(g->x, g->y, g->w, g->h, cx, cy);
}

static int grid_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_grid *g = w;
    if (g->bar_drag) {
        g->scroll = uui_scrollbar_offset_for_drag(g->y, g->h, content_h(g), g->h, cy, g->bar_grab, bar_w(), 0);
        clamp_scroll(g);
        return 1;
    }
    int hot = buttons ? g->hot : cell_at(g, cx, cy);
    int bh = on_bar(g, cx, cy);
    if (bh) hot = -1;
    int changed = hot != g->hot || bh != g->bar_hot;
    g->hot = hot;
    g->bar_hot = bh;
    return changed;
}

static int grid_release(void *w, int cx, int cy) {
    struct uui_grid *g = w;
    if (g->bar_drag) { g->bar_drag = 0; return 1; }
    int i = cell_at(g, cx, cy);
    if (g->armed >= 0 && i == g->armed) {
        uint64_t now = sys_monotonic_ns();
        if (i == g->last_click && now - g->last_click_ns < DOUBLE_NS) {
            g->activated = i;
            g->last_click = -1;
        } else {
            g->last_click = i;
            g->last_click_ns = now;
        }
    }
    g->armed = -1;
    return 1;
}

static int grid_wheel(void *w, int notches) {
    struct uui_grid *g = w;
    int before = g->scroll;
    g->scroll -= notches * g->cell_h * 2;
    clamp_scroll(g);
    return g->scroll != before;
}

static int grid_key(void *w, int key, unsigned mods) {
    (void)mods;
    struct uui_grid *g = w;
    if (!g->count) return 0;
    int c = uui_grid_columns(g), s = g->selected < 0 ? 0 : g->selected;
    int page = (g->h / g->cell_h) * c;
    if (page < c) page = c;
    switch (key) {
    case KEY_ARROW_LEFT:  s = s > 0 ? s - 1 : 0; break;
    case KEY_ARROW_RIGHT: s = s < g->count - 1 ? s + 1 : s; break;
    case KEY_ARROW_UP:    s = s - c >= 0 ? s - c : s; break;
    case KEY_ARROW_DOWN:  s = s + c < g->count ? s + c : s; break;
    case KEY_PAGE_UP:     s = s - page >= 0 ? s - page : s % c; break;
    case KEY_PAGE_DOWN:   s = s + page < g->count ? s + page : g->count - 1; break;
    case KEY_HOME:        s = 0; break;
    case KEY_END:         s = g->count - 1; break;
    case '\n': case '\r':
        if (g->selected >= 0) g->activated = g->selected;
        return g->selected >= 0;
    default: return 0;
    }
    uui_grid_select(g, s);
    return 1;
}

static int grid_accepts(const void *w) { return ((const struct uui_grid *)w)->count > 0; }
static void grid_focused(void *w, int f) { ((struct uui_grid *)w)->focused = f; }

static void grid_describe(const void *w, const struct uui_describe *d) {
    const struct uui_grid *g = w;
    int x, y, cw, ch;
    uui_describe_int(d, "columns", uui_grid_columns(g));
    uui_describe_int(d, "selected", g->selected);
    if (uui_grid_cell_rect(g, g->selected, &x, &y, &cw, &ch)) uui_describe_rect(d, "selected_cell", x, y, cw, ch);
    if (uui_grid_cell_rect(g, 0, &x, &y, &cw, &ch)) uui_describe_rect(d, "first_cell", x, y, cw, ch);
}

const struct uui_widget_ops uui_grid_ops = {
    .natural_size = grid_natural,
    .set_geometry = grid_geometry,
    .bounds = grid_bounds,
    .draw = grid_draw,
    .hit = grid_hit,
    .key = grid_key,
    .set_focused = grid_focused,
    .accepts_focus = grid_accepts,
    .press = grid_press,
    .motion = grid_motion,
    .release = grid_release,
    .wheel = grid_wheel,
    .describe = grid_describe,
};
