// chart -- see ui/uui_chart.h for why this exists beside uui_meter.
#include "ui/uui_chart.h"
#include "ui/utheme.h"
#include "geom.h"   // enum geom_aa -- a line is drawn ALIASED here
#include <stddef.h>

void uui_chart_init(struct uui_chart *c, const char *label) {
    c->x = c->y = c->w = c->h = 0;
    c->label = label;
    c->count = 0;
    c->head = 0;
    for (int i = 0; i < UUI_CHART_MAX; i++) c->samples[i] = 0;
    c->bg = c->grid = c->line = c->fill = UUI_COLOR_UNSET;
}

void uui_chart_push(struct uui_chart *c, int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    c->samples[c->head] = (uint8_t)percent;
    c->head = (c->head + 1) % UUI_CHART_MAX;
    if (c->count < UUI_CHART_MAX) c->count++;
}

int uui_chart_last(const struct uui_chart *c) {
    if (!c->count) return 0;
    return c->samples[(c->head + UUI_CHART_MAX - 1) % UUI_CHART_MAX];
}

// The i'th sample counting back from the newest, oldest-first order.
static int sample_at(const struct uui_chart *c, int i) {
    int first = (c->head + UUI_CHART_MAX - c->count) % UUI_CHART_MAX;
    return c->samples[(first + i) % UUI_CHART_MAX];
}

void uui_chart_natural_size(const struct uui_chart *c, int *out_w, int *out_h) {
    (void)c;
    // FONT-DERIVED, never pixels (docs/gui-guidelines.md): wide enough
    // that the trace is a shape rather than a spike, tall enough that a
    // percentage has somewhere to move.
    if (out_w) *out_w = ugfx_char_advance('n') * 34;
    if (out_h) *out_h = ugfx_char_h() * 7;
}

void uui_chart_set_geometry(struct uui_chart *c, int x, int y, int w, int h) {
    c->x = x; c->y = y; c->w = w; c->h = h;
}

void uui_chart_draw(struct ugfx_surface *s, const struct uui_chart *c) {
    if (c->w <= 0 || c->h <= 0) return;
    uint32_t bg   = UUI_COLOR(c->bg, UTHEME_WHITE);
    uint32_t grid = UUI_COLOR(c->grid, UTHEME_SEPARATOR);
    uint32_t line = UUI_COLOR(c->line, UTHEME_ACCENT);
    uint32_t edge = UUI_COLOR(c->grid, UTHEME_OUTLINE);

    ugfx_fill_rect(s, c->x, c->y, c->w, c->h, bg);

    // Quarters, so the eye has something to read a height against. The
    // 0 and 100 lines are the border itself and are not drawn twice.
    for (int q = 1; q < 4; q++) {
        int gy = c->y + c->h * q / 4;
        ugfx_draw_line(s, c->x, gy, c->x + c->w - 1, gy, grid, GEOM_ALIASED);
    }

    // **ONE COLUMN PER SAMPLE, RIGHT-ALIGNED.** Scaling the series to
    // the width would make the same history a different shape in a
    // resized window; instead the newest UUI_CHART_MAX samples that fit
    // are drawn and the rest is empty, which is what every system
    // monitor does while its history fills.
    int n = c->count < c->w ? c->count : c->w;
    for (int i = 0; i < n; i++) {
        int v = sample_at(c, c->count - n + i);
        int x = c->x + c->w - n + i;
        int top = c->y + c->h - 1 - (c->h - 1) * v / 100;
        // Filled to the baseline rather than a bare line: an area reads
        // as a quantity at a glance, and a one-pixel trace on a busy
        // background does not.
        ugfx_draw_line(s, x, top, x, c->y + c->h - 1, line, GEOM_ALIASED);
    }

    // The frame last, so the area cannot paint over it.
    ugfx_draw_line(s, c->x, c->y, c->x + c->w - 1, c->y, edge, GEOM_ALIASED);
    ugfx_draw_line(s, c->x, c->y + c->h - 1, c->x + c->w - 1, c->y + c->h - 1, edge, GEOM_ALIASED);
    ugfx_draw_line(s, c->x, c->y, c->x, c->y + c->h - 1, edge, GEOM_ALIASED);
    ugfx_draw_line(s, c->x + c->w - 1, c->y, c->x + c->w - 1, c->y + c->h - 1, edge, GEOM_ALIASED);

    if (c->label) {
        // CLIPPED, because a label is caller text in a fixed box --
        // gfx_draw_string()'s unclipped twin has shipped this bug twice.
        ugfx_draw_string_clipped(s, c->x + 4, c->y + 3, c->w - 8, c->label,
                                 UUI_COLOR(c->fill, UTHEME_TEXT), bg);
    }
}

// --- the ops table ----------------------------------------------------

static void chart_natural_op(const void *w, int *out_w, int *out_h) {
    uui_chart_natural_size((const struct uui_chart *)w, out_w, out_h);
}
static void chart_geometry_op(void *w, int x, int y, int width, int height) {
    uui_chart_set_geometry((struct uui_chart *)w, x, y, width, height);
}
static void chart_draw_op(struct ugfx_surface *s, const void *w) {
    uui_chart_draw(s, (const struct uui_chart *)w);
}
static void chart_bounds_op(const void *w, int *x, int *y, int *width, int *height) {
    const struct uui_chart *c = (const struct uui_chart *)w;
    if (x) *x = c->x;
    if (y) *y = c->y;
    if (width) *width = c->w;
    if (height) *height = c->h;
}

// No press, key or focus: a chart is a READING, like uui_meter beside
// it. check_widget_ops.py's rule is that a table with `draw` owes
// natural_size and set_geometry, and one with set_geometry owes bounds.
const struct uui_widget_ops uui_chart_ops = {
    .bounds = chart_bounds_op,
    .draw = chart_draw_op,
    .natural_size = chart_natural_op,
    .set_geometry = chart_geometry_op,
};
