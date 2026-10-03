// chart -- see ui/uui_chart.h for why this exists beside uui_meter.
#include "ui/uui_chart.h"
#include "ui/utheme.h"
#include "ui/uui_primitives.h"   // uui_hit -- the pointer is inside the plot
#include "geom.h"   // enum geom_aa -- a line is drawn ALIASED here
#include <stdio.h>
#include <stddef.h>

void uui_chart_init(struct uui_chart *c, const char *label) {
    c->x = c->y = c->w = c->h = 0;
    c->label = label;
    c->value = NULL;
    c->count = 0;
    c->head = 0;
    c->has_parts = 0;
    c->scale_max = 100;     // a percentage unless the caller says otherwise
    c->sample_ms = 0;
    c->hover = -1;
    c->compact = 0;
    for (int i = 0; i < UUI_CHART_MAX; i++) c->samples[i] = c->parts[i] = 0;
    c->bg = c->grid = c->line = c->fill = c->part = UUI_COLOR_UNSET;
    c->fit = 0;
    for (int i = 0; i < UUI_CHART_SERIES; i++) c->series_col[i] = UUI_COLOR_UNSET;
    uui_chart_clear(c);
}

void uui_chart_clear(struct uui_chart *c) {
    c->count = c->head = 0;
    c->has_parts = 0;
    c->stride = 1;
    c->pend_n = 0;
    c->pend_sum = 0;
    c->cur_series = 0;
    c->mark_n = 0;
    c->hover = -1;
}

void uui_chart_set_fit(struct uui_chart *c, int on) {
    c->fit = on ? 1 : 0;
    uui_chart_clear(c);
}

// Fit mode is LINEAR (head == count): oldest at 0.
static void fit_commit(struct uui_chart *c) {
    if (c->pend_n == 0) return;
    if (c->count == UUI_CHART_MAX) {
        // Full: every two samples become one, and the marks follow.
        int n = 0;
        for (int i = 0; i + 1 < c->count; i += 2) {
            c->samples[n] = (uint32_t)(((uint64_t)c->samples[i] + c->samples[i + 1]) / 2);
            c->series[n] = c->series[i + 1];
            n++;
        }
        c->count = n;
        for (int k = 0; k < c->mark_n; k++) c->marks[k].at /= 2;
        c->stride *= 2;
    }
    c->samples[c->count] = (uint32_t)(c->pend_sum / (uint64_t)c->pend_n);
    c->series[c->count] = (uint8_t)c->cur_series;
    c->count++;
    c->head = c->count % UUI_CHART_MAX;
    c->pend_n = 0;
    c->pend_sum = 0;
}

void uui_chart_set_series(struct uui_chart *c, int series) {
    if (series < 0 || series >= UUI_CHART_SERIES || series == c->cur_series) return;
    if (c->fit) fit_commit(c);   // never average across a change of series
    c->cur_series = series;
}

void uui_chart_add_mark(struct uui_chart *c, const char *label) {
    if (c->fit) fit_commit(c);
    if (c->mark_n >= UUI_CHART_MARKS) return;
    c->marks[c->mark_n].at = c->count;
    c->marks[c->mark_n].label = label;
    c->mark_n++;
}

void uui_chart_set_scale(struct uui_chart *c, uint32_t max) { c->scale_max = max; }
void uui_chart_set_interval(struct uui_chart *c, int ms)    { c->sample_ms = ms; }
void uui_chart_set_value(struct uui_chart *c, const char *value) { c->value = value; }

void uui_chart_push_split(struct uui_chart *c, uint32_t total, uint32_t part) {
    if (c->fit) {
        c->pend_sum += total;
        if (++c->pend_n >= c->stride) fit_commit(c);
        return;
    }
    if (part > total) part = total;
    if (part) c->has_parts = 1;   // a component cannot exceed its whole
    c->samples[c->head] = total;
    c->parts[c->head] = part;
    c->head = (c->head + 1) % UUI_CHART_MAX;
    if (c->count < UUI_CHART_MAX) c->count++;
}

void uui_chart_push(struct uui_chart *c, uint32_t value) {
    uui_chart_push_split(c, value, 0);
}

uint32_t uui_chart_last(const struct uui_chart *c) {
    if (!c->count) return 0;
    if (c->fit) return c->samples[c->count - 1];
    return c->samples[(c->head + UUI_CHART_MAX - 1) % UUI_CHART_MAX];
}

int uui_chart_drawn(const struct uui_chart *c) {
    if (c->fit) return c->count;
    int n = c->count < c->w ? c->count : c->w;
    return n < 0 ? 0 : n;
}

// The i'th DRAWN sample, oldest first.
static int drawn_slot(const struct uui_chart *c, int i) {
    if (c->fit) return i;
    int n = uui_chart_drawn(c);
    int first = (c->head + UUI_CHART_MAX - n) % UUI_CHART_MAX;
    return (first + i) % UUI_CHART_MAX;
}

uint32_t uui_chart_sample(const struct uui_chart *c, int i) {
    if (i < 0 || i >= uui_chart_drawn(c)) return 0;
    return c->samples[drawn_slot(c, i)];
}

uint32_t uui_chart_recent(const struct uui_chart *c, int back) {
    if (back < 0 || back >= c->count || back >= UUI_CHART_MAX) return 0;
    return c->samples[(c->head + UUI_CHART_MAX - 1 - back) % UUI_CHART_MAX];
}

int uui_chart_hover_index(const struct uui_chart *c) { return c->hover; }

// The top of the scale: the caller's, or the largest sample in view.
// Never zero, so the division below is safe and an all-zero series
// draws a flat baseline rather than a full block.
static uint32_t scale_of(const struct uui_chart *c) {
    if (c->scale_max) return c->scale_max;
    uint32_t peak = 1;
    int n = uui_chart_drawn(c);
    for (int i = 0; i < n; i++) {
        uint32_t v = c->samples[drawn_slot(c, i)];
        if (v > peak) peak = v;
    }
    return peak;
}

void uui_chart_natural_size(const struct uui_chart *c, int *out_w, int *out_h) {
    (void)c;
    // FONT-DERIVED, never pixels (docs/gui-guidelines.md): wide enough
    // that the trace is a shape rather than a spike, tall enough for a
    // caption above it and an axis below.
    if (out_w) *out_w = ugfx_char_advance('n') * 34;
    if (out_h) *out_h = ugfx_char_h() * 8;
}

void uui_chart_set_geometry(struct uui_chart *c, int x, int y, int w, int h) {
    c->x = x; c->y = y; c->w = w; c->h = h;
}

// "3 min", "45 s" -- the span the DRAWN samples cover, which is what the
// width actually shows rather than what the ring could hold.
static void span_text(const struct uui_chart *c, char *out, int cap) {
    long long ms = (long long)uui_chart_drawn(c) * c->sample_ms;
    long long sec = ms / 1000;
    if (sec >= 120) snprintf(out, (unsigned)cap, "%lld min", sec / 60);
    else            snprintf(out, (unsigned)cap, "%lld s", sec);
}

// The x of sample `i` in fit mode: the history spread over the plot.
static int fit_x(const struct uui_chart *c, int i) {
    int span = c->count > 1 ? c->count - 1 : 1;
    return c->x + 2 + (int)((int64_t)(c->w - 5) * i / span);
}

// FIT MODE: a polyline per series run, the marks as dividers with their
// labels at the top of the plot.
static void draw_fit(struct ugfx_surface *s, const struct uui_chart *c,
                     int top, int bot, uint32_t bg, uint32_t line) {
    int ph = bot - top;
    uint32_t top_scale = scale_of(c);
    uint32_t sep = UUI_COLOR(c->grid, UTHEME_SEPARATOR);
    uint32_t dim = ugfx_blend(UTHEME_TEXT, bg, 120);
    for (int k = 0; k < c->mark_n; k++) {
        int mx = c->count > 0 ? fit_x(c, c->marks[k].at) : c->x + 2;
        if (c->marks[k].at >= c->count && c->count > 0) mx = fit_x(c, c->count - 1);
        if (k > 0) ugfx_draw_line(s, mx, top, mx, bot, sep, GEOM_ALIASED);
        int nx = k + 1 < c->mark_n ? fit_x(c, c->marks[k + 1].at) : c->x + c->w - 2;
        if (c->marks[k].label && nx - mx > 8)
            ugfx_draw_string_clipped(s, mx + 4, top + 1, nx - mx - 6, c->marks[k].label,
                                     dim, bg);
    }
    int px = 0, py = 0;
    for (int i = 0; i < c->count; i++) {
        uint32_t v = c->samples[i];
        if (v > top_scale) v = top_scale;
        int x = fit_x(c, i);
        int y = bot - 1 - (int)((uint64_t)(ph - 2) * v / top_scale);
        int sr = c->series[i];
        uint32_t col = UUI_COLOR(c->series_col[sr], line);
        if (i > 0 && c->series[i - 1] == sr) {
            ugfx_draw_line(s, px, py, x, y, col, GEOM_AA);
            ugfx_draw_line(s, px, py - 1, x, y - 1, col, GEOM_AA);
        } else {
            ugfx_fill_rect(s, x - 1, y - 1, 2, 2, col);
        }
        px = x;
        py = y;
    }
}

void uui_chart_draw(struct ugfx_surface *s, const struct uui_chart *c) {
    if (c->w <= 0 || c->h <= 0) return;
    uint32_t bg   = UUI_COLOR(c->bg, UTHEME_WHITE);
    uint32_t grid = UUI_COLOR(c->grid, UTHEME_SEPARATOR);
    uint32_t line = UUI_COLOR(c->line, UTHEME_ACCENT);
    uint32_t part = UUI_COLOR(c->part, UTHEME_TEXT);
    uint32_t edge = UUI_COLOR(c->grid, UTHEME_OUTLINE);
    uint32_t ink  = UUI_COLOR(c->fill, UTHEME_TEXT);

    int ch = ugfx_char_h();
    // The plot sits below the caption row and above the axis row, so
    // neither can be painted over by a tall column.
    int top = c->y + (c->compact ? 0 : ch + 2);
    int bot = c->y + c->h - 1 - (!c->compact && c->sample_ms ? ch + 1 : 0);
    int ph = bot - top;
    if (ph < 4) { top = c->y; bot = c->y + c->h - 1; ph = bot - top; }

    ugfx_fill_rect(s, c->x, c->y, c->w, c->h, bg);
    for (int q = 1; !c->compact && q < 4; q++) {
        int gy = top + ph * q / 4;
        ugfx_draw_line(s, c->x, gy, c->x + c->w - 1, gy, grid, GEOM_ALIASED);
    }

    if (c->fit) {
        draw_fit(s, c, top, bot, bg, line);
        goto frame;
    }

    // **ONE COLUMN PER SAMPLE, RIGHT-ALIGNED.** Scaling the series to
    // the width would make the same history a different shape in a
    // resized window; the newest samples that fit are drawn and the
    // rest is empty, which is what every monitor does while filling.
    uint32_t top_scale = scale_of(c);
    int n = uui_chart_drawn(c);
    for (int i = 0; i < n; i++) {
        int slot = drawn_slot(c, i);
        uint32_t v = c->samples[slot];
        if (v > top_scale) v = top_scale;
        int x = c->x + c->w - n + i;
        int y1 = bot - (int)((uint64_t)ph * v / top_scale);
        ugfx_draw_line(s, x, y1, x, bot, line, GEOM_ALIASED);
        if (c->has_parts) {
            uint32_t p = c->parts[slot];
            if (p > top_scale) p = top_scale;
            int y2 = bot - (int)((uint64_t)ph * p / top_scale);
            // OVER the total, from the baseline: the component reads as
            // a share of the column it sits inside.
            if (y2 < bot) ugfx_draw_line(s, x, y2, x, bot, part, GEOM_ALIASED);
        }
    }

    // The hovered column, marked full height so it is findable even
    // where the trace is flat.
    if (c->hover >= 0 && c->hover < n) {
        int hx = c->x + c->w - n + c->hover;
        ugfx_draw_line(s, hx, top, hx, bot, edge, GEOM_ALIASED);
    }

frame:
    if (!c->compact) {
        ugfx_draw_line(s, c->x, c->y, c->x + c->w - 1, c->y, edge, GEOM_ALIASED);
        ugfx_draw_line(s, c->x, c->y + c->h - 1, c->x + c->w - 1, c->y + c->h - 1, edge, GEOM_ALIASED);
        ugfx_draw_line(s, c->x, c->y, c->x, c->y + c->h - 1, edge, GEOM_ALIASED);
        ugfx_draw_line(s, c->x + c->w - 1, c->y, c->x + c->w - 1, c->y + c->h - 1, edge, GEOM_ALIASED);
    }

    // The label left and the reading right, both CLIPPED -- caller text
    // in a fixed box, the bug gfx_draw_string()'s unclipped twin has
    // shipped twice.
    int label_w = c->label ? ugfx_text_width(c->label) : 0;
    if (c->label)
        ugfx_draw_string_clipped(s, c->x + 4, c->y + 2, c->w - 8, c->label, ink, bg);
    if (c->value) {
        // MEASURED, never a character count times a width: the face is
        // proportional (docs/conventions/gui.md).
        int vw = ugfx_text_width(c->value);
        int vx = c->x + c->w - 4 - vw;
        int floor_x = c->x + 4 + label_w + ugfx_char_advance('n');
        int room = c->x + c->w - 4 - floor_x;
        if (vx < floor_x) vx = floor_x;
        // The reading loses a fight for space: a nameless graph is worse
        // than an unlabelled number.
        if (room > 0) ugfx_draw_string_clipped(s, vx, c->y + 2, room, c->value, ink, bg);
    }

    // The axis: how far back the left edge is. "now" is not drawn --
    // the right edge being the present is the one thing every reader of
    // a time series already assumes.
    if (c->sample_ms) {
        char span[16];
        span_text(c, span, sizeof span);
        ugfx_draw_string_clipped(s, c->x + 4, bot + 1, c->w - 8, span, grid, bg);
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
static int chart_hit_op(const void *w, int cx, int cy) {
    const struct uui_chart *c = (const struct uui_chart *)w;
    return uui_hit(c->x, c->y, c->w, c->h, cx, cy);
}
// A MOTION, NOT A PRESS: nothing is chosen by pointing at a graph, so
// this widget takes no press and needs no release (check_widget_ops.py's
// rule). Returns 1 only when the column CHANGED, so a pointer resting
// still does not ask for a repaint every frame.
static int chart_motion_op(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_chart *c = (struct uui_chart *)w;
    int was = c->hover;
    if (!uui_hit(c->x, c->y, c->w, c->h, cx, cy)) {
        c->hover = -1;
    } else if (c->fit) {
        int span = c->count > 1 ? c->count - 1 : 1;
        int i = c->w > 5 ? (int)((int64_t)(cx - c->x - 2) * span / (c->w - 5)) : -1;
        c->hover = (c->count > 0 && i >= 0 && i < c->count) ? i : -1;
    } else {
        int n = uui_chart_drawn(c);
        int i = cx - (c->x + c->w - n);
        c->hover = (n > 0 && i >= 0 && i < n) ? i : -1;
    }
    return c->hover != was;
}

const struct uui_widget_ops uui_chart_ops = {
    .bounds = chart_bounds_op,
    .draw = chart_draw_op,
    .natural_size = chart_natural_op,
    .set_geometry = chart_geometry_op,
    .hit = chart_hit_op,
    .motion = chart_motion_op,
};
