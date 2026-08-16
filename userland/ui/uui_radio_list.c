// radio list. Split out of uwidgets.c -- see ui/uui_radio_list.h.
#include "ui/uui_radio_list.h"
#include "ui/uui_widget.h"  // the ops table at the bottom of this file

// ---------------------------------------------------------------------
// radio list
// ---------------------------------------------------------------------

static int radio_rows(const struct uui_radio_list *l) {
    int cols = l->cols > 0 ? l->cols : 1;
    return (l->count + cols - 1) / cols;
}

void uui_radio_list_natural_size(const struct uui_radio_list *l, int *out_w, int *out_h) {
    int cols = l->cols > 0 ? l->cols : 1;
    if (out_w) *out_w = cols * l->col_w;
    if (out_h) *out_h = radio_rows(l) * l->row_h;
}

static void radio_cell(const struct uui_radio_list *l, int i, int x, int y, int *cx, int *cy) {
    int cols = l->cols > 0 ? l->cols : 1;
    *cx = x + (i % cols) * l->col_w;
    *cy = y + (i / cols) * l->row_h;
}

void uui_radio_list_set_geometry(struct uui_radio_list *l, int x, int y) {
    l->x = x;
    l->y = y;
    uui_radio_list_natural_size(l, &l->w, &l->h);
}

void uui_radio_list_draw(struct ugfx_surface *s, const struct uui_radio_list *l,
                          int selected, int hovered,
                          uint32_t bg, uint32_t fg) {
    int x = l->x, y = l->y;
    for (int i = 0; i < l->count; i++) {
        int cx, cy;
        radio_cell(l, i, x, y, &cx, &cy);

        uint32_t row_bg = bg;
        if (i == hovered) {
            row_bg = uui_state_bg(bg, UUI_STATE_HOVER);
            ugfx_fill_rect(s, cx, cy, l->col_w, l->row_h, row_bg);
        }

        int m = l->marker_size;
        int my = cy + (l->row_h - m) / 2;
        ugfx_draw_rect(s, cx, my, m, m, fg);
        if (i == selected) {
            int inset = m / 4 > 0 ? m / 4 : 1;
            ugfx_fill_rect(s, cx + inset, my + inset, m - 2 * inset, m - 2 * inset, fg);
        }
        ugfx_draw_string_clipped(s, cx + m + 6, cy + (l->row_h - ugfx_char_h()) / 2,
                                  l->col_w - m - 8, l->options[i], fg, row_bg);
    }
}

int uui_radio_list_hit(const struct uui_radio_list *l, int cx, int cy) {
    int x = l->x, y = l->y, px = cx, py = cy;
    for (int i = 0; i < l->count; i++) {
        int cx, cy;
        radio_cell(l, i, x, y, &cx, &cy);
        if (uui_hit(cx, cy, l->col_w, l->row_h, px, py)) return i;
    }
    return -1;
}

// --- routed pointer input (ui/uui_route.h) ----------------------------
//
// The widget owns `selected` now. It used to be the APP's -- draw()
// takes the value as an argument -- which meant a routed press had
// nowhere to record a choice, and two apps could disagree about which
// option was current. draw() still accepts an argument (callers pass
// `l->selected`), so nothing that already worked changed.
static int rl_ops_hit(const void *w, int cx, int cy) {
    return uui_radio_list_hit((const struct uui_radio_list *)w, cx, cy) >= 0;
}

static int rl_ops_press(void *w, int cx, int cy) {
    struct uui_radio_list *l = (struct uui_radio_list *)w;
    int idx = uui_radio_list_hit(l, cx, cy);
    if (idx < 0 || idx == l->selected) return 0;
    l->selected = idx;
    return 1;
}

static int rl_ops_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_radio_list *l = (struct uui_radio_list *)w;
    int hot = uui_radio_list_hit(l, cx, cy);
    if (hot == l->hovered) return 0;
    l->hovered = hot;
    return 1;
}

const struct uui_widget_ops uui_radio_list_ops = {
    .hit    = rl_ops_hit,
    .press  = rl_ops_press,
    .motion = rl_ops_motion,
};
