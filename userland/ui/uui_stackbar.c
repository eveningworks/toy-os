// See uui_stackbar.h.
#include "ui/uui_stackbar.h"
#include "ui/uui_describe.h"
#include "ui/uui_widget.h"
#include "ui/uui_primitives.h"   // uui_state_bg -- the muted label
#include "ui/utheme.h"
#include <string.h>

void uui_stackbar_init(struct uui_stackbar *b, int legend_cols) {
    memset(b, 0, sizeof *b);
    b->legend_cols = legend_cols < 0 ? 0 : legend_cols;
    b->track = b->border = b->label_fg = b->value_fg = b->bg = UUI_COLOR_UNSET;
}

int uui_stackbar_add(struct uui_stackbar *b, const char *label, uint32_t color) {
    if (b->count >= UUI_STACKBAR_MAX) return -1;
    struct uui_stackbar_seg *s = &b->seg[b->count];
    s->label = label;
    s->value[0] = 0;
    s->amount = 0;
    s->color = color;
    return b->count++;
}

void uui_stackbar_set(struct uui_stackbar *b, int i, uint64_t amount, const char *value) {
    if (i < 0 || i >= b->count) return;
    b->seg[i].amount = amount;
    size_t n = value ? strlen(value) : 0;
    if (n >= sizeof b->seg[i].value) n = sizeof b->seg[i].value - 1;
    if (n) memcpy(b->seg[i].value, value, n);
    b->seg[i].value[n] = 0;
}

static int bar_h(void) { return ugfx_char_h() + 4; }
static int row_h(void) { return ugfx_char_h() + ugfx_char_h() / 3; }
static int gap(void) { return ugfx_char_h() / 2; }
static int legend_rows(const struct uui_stackbar *b) {
    return b->legend_cols ? (b->count + b->legend_cols - 1) / b->legend_cols : 0;
}

void uui_stackbar_natural_size(const struct uui_stackbar *b, int *out_w, int *out_h) {
    int rows = legend_rows(b);
    *out_w = ugfx_char_w() * 24 * (b->legend_cols ? b->legend_cols : 1);
    *out_h = bar_h() + (rows ? gap() + rows * row_h() : 0);
}

void uui_stackbar_draw(struct ugfx_surface *s, const struct uui_stackbar *b) {
    if (b->w <= 2) return;
    int bh = bar_h();
    int ix = b->x + 1, iy = b->y + 1, iw = b->w - 2, ih = bh - 2;
    uint32_t bg = UUI_COLOR(b->bg, UTHEME_PANEL_BG);
    uint32_t label_fg = UUI_COLOR(b->label_fg, uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED));
    uint32_t value_fg = UUI_COLOR(b->value_fg, UTHEME_TEXT);
    ugfx_fill_rect(s, ix, iy, iw, ih, UUI_COLOR(b->track, UTHEME_WHITE));
    uint64_t sum = 0;
    for (int i = 0; i < b->count; i++) sum += b->seg[i].amount;
    uint64_t whole = b->total > sum ? b->total : sum;
    if (whole) {
        // Each edge from the RUNNING total, so rounding never leaves a
        // gap or overruns: a full bar ends exactly at the track's edge.
        uint64_t run = 0;
        int x0 = 0;
        for (int i = 0; i < b->count; i++) {
            run += b->seg[i].amount;
            int x1 = (int)((uint64_t)iw * run / whole);
            if (x1 > x0) ugfx_fill_rect(s, ix + x0, iy, x1 - x0, ih, b->seg[i].color);
            x0 = x1;
        }
    }
    ugfx_draw_rect(s, b->x, b->y, b->w, bh, UUI_COLOR(b->border, UTHEME_OUTLINE));

    if (!b->legend_cols) return;
    int cols = b->legend_cols, ch = ugfx_char_h(), rh = row_h();
    int colgap = ugfx_char_w() * 3;
    int cw = (b->w - colgap * (cols - 1)) / cols;
    int sw = ch * 2 / 3;
    int top = b->y + bh + gap();
    for (int i = 0; i < b->count; i++) {
        const struct uui_stackbar_seg *g = &b->seg[i];
        int cx = b->x + (i % cols) * (cw + colgap), cy = top + (i / cols) * rh;
        ugfx_fill_rect(s, cx, cy + (ch - sw) / 2, sw, sw, g->color);
        int vw = ugfx_text_width(g->value);
        int lx = cx + sw + ch / 3;
        int lw = cw - (lx - cx) - vw - ugfx_char_w();
        if (lw > 0) ugfx_draw_string_clipped(s, lx, cy, lw, g->label ? g->label : "", label_fg, bg);
        if (vw <= cw) ugfx_draw_string_clipped(s, cx + cw - vw, cy, vw, g->value, value_fg, bg);
    }
}

static void natural_op(const void *w, int *ow, int *oh) {
    uui_stackbar_natural_size((const struct uui_stackbar *)w, ow, oh);
}
static void geometry_op(void *w, int x, int y, int width, int height) {
    struct uui_stackbar *b = w;
    b->x = x; b->y = y; b->w = width; b->h = height;
}
static void bounds_op(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_stackbar *b = w;
    *x = b->x; *y = b->y; *ow = b->w; *oh = b->h;
}
static void draw_op(struct ugfx_surface *s, const void *w) {
    uui_stackbar_draw(s, (const struct uui_stackbar *)w);
}
// Each segment's drawn extent, so a test can hold the bar's proportions
// against the numbers it was given (and an empty one reads as width 0).
static void describe_op(const void *w, const struct uui_describe *d) {
    const struct uui_stackbar *b = w;
    uui_describe_int(d, "count", b->count);
    uint64_t sum = 0, run = 0;
    for (int i = 0; i < b->count; i++) sum += b->seg[i].amount;
    uint64_t whole = b->total > sum ? b->total : sum;
    int iw = b->w - 2, x0 = 0;
    for (int i = 0; i < b->count; i++) {
        run += b->seg[i].amount;
        int x1 = whole ? (int)((uint64_t)iw * run / whole) : 0;
        uui_describe_rect_i(d, "seg", i, b->x + 1 + x0, b->y + 1, x1 - x0, bar_h() - 2);
        x0 = x1;
    }
}

// No `hit`: a composition bar is not a control. widget-ops-ok: display-only, like uui_meter.
const struct uui_widget_ops uui_stackbar_ops = {
    .bounds = bounds_op,
    .draw = draw_op,
    .natural_size = natural_op,
    .set_geometry = geometry_op,
    .describe = describe_op,
};
