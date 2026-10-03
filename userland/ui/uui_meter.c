// meter -- see ui/uui_meter.h for why this exists at all.
#include "ui/uui_meter.h"
#include "ui/uui_describe.h"
#include "ui/uui_widget.h"
#include "ui/utheme.h"
#include "ui/uui_chart.h"
#include "ui/uui_primitives.h"
#include "fixed.h"
#include <stddef.h>

// Selects the value's font for one measurement or one paint, restoring
// whatever was current. BOTH halves need it and both must agree, or the
// number is measured in one font and drawn in another -- the trap
// uui_label's `font` field documents.
static const struct ugfx_font *push_value_font(const struct uui_meter *m) {
    return ugfx_set_font(m->value_font ? m->value_font
                                       : ugfx_font_session(UGFX_FONT_BOLD));
}

void uui_meter_init(struct uui_meter *m) {
    m->x = m->y = m->w = m->h = 0;
    m->caption = m->value = m->unit = m->detail = NULL;
    m->fill = -1;                    // no bar, which is not an empty bar
    m->fg = UTHEME_TEXT;
    m->bg = UTHEME_WHITE;
    m->accent = UTHEME_ACCENT;
    m->value_font = NULL;
    m->style = UUI_METER_BAR;
    m->spark = NULL;
    m->value_fg = 0;
    m->active = 0;
}

void uui_meter_set(struct uui_meter *m, const char *caption,
                   const char *value, const char *unit, const char *detail) {
    m->caption = caption;
    m->value = value;
    m->unit = unit;
    m->detail = detail;
}

void uui_meter_set_fill(struct uui_meter *m, int per_mille) {
    m->fill = per_mille;
}

void uui_meter_set_spark(struct uui_meter *m, const struct uui_chart *spark) {
    m->spark = spark;
}

void uui_meter_set_style(struct uui_meter *m, enum uui_meter_style style) {
    m->style = style;
}

// --- ring geometry, all of it derived from the font ------------------
//
// Not a pixel constant anywhere: the ring is sized in text rows, so the
// whole gauge grows with the session font exactly as the chrome around
// it does (docs/gui-guidelines.md's "size everything from the font").
#define RING_ROWS      9   // the ring's diameter, in text rows
// THE HOLE HOLDS TWO SHORT ROWS, NOT THREE. `detail` goes BELOW the
// ring on the tile's full width, because a hole is only as wide as the
// ring's inside and the strings a caller wants there are not: "7.8 GiB
// free" was clipped to "7.8 GiB fre", which reads as a rendering bug
// rather than as a long label. Clipping is the backstop, not the plan.
#define RING_TEXT_ROWS 2   // value and unit, stacked inside the hole

// The ring's thickness. A fraction of the row pitch rather than a
// constant, and floored so it never vanishes on a tiny font.
// The trend's own rows, reserved only when there IS one: unlike the
// text rows above, a spark is a property of the CALLER's tile rather
// than of its content, so it cannot appear later and overflow the box.
#define SPARK_ROWS 3

static int ring_thickness(void) {
    int t = ugfx_char_h() / 2;
    return t < 4 ? 4 : t;
}

// EVERY ROW IS RESERVED WHETHER OR NOT IT IS USED, and that is the
// whole point rather than an oversight. The first version counted the
// strings that were non-NULL, which is a height that DEPENDS ON THE
// CONTENT -- and a meter's content is a value that changes. A tile laid
// out while its unit and detail were still NULL got a two-row box, and
// the moment a result arrived it drew four rows out through the bottom
// of its own border and over the widget below.
//
// So the box is the same size empty as full: caption, value, unit,
// detail, bar. That is uui_label's `rows` rule arriving from the other
// direction -- the caller reserves, the content fills -- and it is what
// CLAUDE.md's natural_size rule requires.
#define METER_TEXT_ROWS 2   // caption, detail; the value (unit beside it) is measured apart

void uui_meter_natural_size(const struct uui_meter *m, int *out_w, int *out_h) {
    int pad = utheme_pad();
    int line = ugfx_char_h();

    if (out_w) {
        // The widest of the four strings, each measured in ITS OWN font
        // -- the value is the only one in the big one.
        int widest = 0;
        const struct ugfx_font *was = push_value_font(m);
        if (m->value) widest = ugfx_text_width(m->value);
        ugfx_set_font(was);

        const char *rest[3] = { m->caption, m->unit, m->detail };
        for (int i = 0; i < 3; i++) {
            if (!rest[i]) continue;
            int wpx = ugfx_text_width(rest[i]);
            if (wpx > widest) widest = wpx;
        }
        *out_w = widest + pad * 2;
    }

    if (out_h) {
        int value_h = line;
        if (m->value_font) {
            const struct ugfx_font *was = push_value_font(m);
            value_h = ugfx_char_h();
            ugfx_set_font(was);
        }
        // Every reserved row at the session pitch, the value at its own,
        // plus the bar and the padding. Nothing here reads a string.
        *out_h = METER_TEXT_ROWS * line + value_h + line + pad * 2;
    }

    // A RING IS SQUARE-ISH, and its width is a floor rather than a
    // replacement: a long caption still has to fit, so the wider of the
    // two wins. Measuring the ring as an EXTENT and not from wherever
    // the widget currently sits is CLAUDE.md's natural_size rule.
    if (m->style == UUI_METER_RING) {
        int d = RING_ROWS * line;
        if (out_w && d + pad * 2 > *out_w) *out_w = d + pad * 2;
        // Caption row, the ring, then the detail row under it -- all
        // three reserved whether or not they carry anything, the same
        // rule the bar style follows.
        if (out_h) *out_h = line + d + line + pad * 2;
    }

    if (m->spark && out_h) *out_h += SPARK_ROWS * line;
}

// The ring presentation: a caption row, then a track with the filled
// sweep over it, and the value stacked inside the hole.
// The trend across the tile's foot, in the band natural_size() reserved
// for it. Drawn through the chart's OWN draw so the two cannot disagree
// about what a trace looks like -- the widget is told where to sit and
// nothing here reaches into its samples.
static void draw_spark(struct ugfx_surface *s, const struct uui_meter *m) {
    if (!m->spark) return;
    int pad = utheme_pad();
    int h = SPARK_ROWS * ugfx_char_h();
    struct uui_chart c = *m->spark;        // a COPY: placing it must not
    c.label = NULL;                        // move the caller's widget, and
    c.value = NULL;                        // the tile already says both
    c.sample_ms = 0;                       // and already carries the span
    c.compact = 1;                         // a trace, not a boxed grid
    uui_chart_set_geometry(&c, m->x + pad, m->y + m->h - pad - h,
                           m->w - pad * 2, h);
    uui_chart_draw(s, &c);
}

static void draw_ring(struct ugfx_surface *s, const struct uui_meter *m) {
    int pad = utheme_pad();
    int line = ugfx_char_h();
    int x = m->x + pad;
    int inner = m->w - pad * 2;
    if (inner <= 0) return;

    int top = m->y + pad;
    if (m->caption) {
        // Centred over the ring, which is what makes the tiles read as a
        // row of gauges rather than as left-aligned boxes.
        int cw = ugfx_text_width(m->caption);
        int cx = x + (inner - cw) / 2;
        if (cx < x) cx = x;
        ugfx_draw_string_clipped(s, cx, top, inner, m->caption, m->fg, m->bg);
    }
    top += line;

    // The largest ring that fits what is left, so a tile stretched by a
    // layout grows its gauge instead of stranding it in a corner. One
    // row is held back for the detail line under it.
    int spark_h = m->spark ? SPARK_ROWS * ugfx_char_h() : 0;
    int avail_h = m->y + m->h - pad - top - line - spark_h;
    int d = inner < avail_h ? inner : avail_h;
    // CAPPED. A gauge is a gauge at any window size -- letting it grow
    // to fill a maximised tile makes a dinner plate, and the arc's cost
    // is proportional to its radius times its thickness, so an unbounded
    // ring is also an unbounded amount of work every frame.
    int dmax = RING_ROWS * line * 2;
    if (d > dmax) d = dmax;
    if (d < 8) return;                       // too small to be a gauge at all
    int r_outer = d / 2 - 1;
    int thick = ring_thickness();
    if (thick > r_outer - 2) thick = r_outer - 2;
    if (thick < 1) return;
    int r_inner = r_outer - thick;
    int cx = x + inner / 2;
    int cy = top + d / 2;

    // TURN 0 IS 3 O'CLOCK and y grows downward, so a gauge that starts
    // at the top begins a quarter turn earlier (geom.h).
    fx_t start = -FX_ONE / 4;
    // THE TRACK NEEDS REAL CONTRAST. The bar style's trough is
    // UTHEME_PANEL_BG, which works there because it is a thin strip
    // held by a border -- as a wide arc on this near-white theme it was
    // invisible, and an empty gauge read as a broken one. Shifted by
    // uui_state_bg() rather than hand-picked, which is what makes the
    // direction follow the theme's luminance instead of assuming light.
    ugfx_fill_ring(s, cx, cy, r_outer, r_inner, start, start + FX_ONE,
                   uui_state_bg(UTHEME_PANEL_BG, UUI_STATE_PRESSED));
    if (m->fill >= 0) {
        int fill = m->fill > 1000 ? 1000 : m->fill;
        // A sweep rounded to zero would draw nothing, which reads as a
        // broken gauge rather than as an empty one; the track behind it
        // is what says "empty", so zero really is nothing here.
        fx_t sweep = (fx_t)(((int64_t)FX_ONE * fill) / 1000);
        if (sweep > 0) ugfx_fill_ring(s, cx, cy, r_outer, r_inner,
                                      start, start + sweep, m->accent);
    }
    ugfx_draw_circle(s, cx, cy, r_outer, UTHEME_BORDER, GEOM_AA);
    ugfx_draw_circle(s, cx, cy, r_inner, UTHEME_BORDER, GEOM_AA);

    // The value, unit and detail stacked and centred in the HOLE. The
    // hole is 2*r_inner across, and text wider than that is clipped to
    // it rather than drawn over the ring.
    int hole = r_inner * 2 - 2;
    if (hole <= 0) return;
    const char *rows[RING_TEXT_ROWS] = { m->value, m->unit };
    int used = 0;
    for (int i = 0; i < RING_TEXT_ROWS; i++) if (rows[i]) used += line;
    if (rows[0] && m->value_font) {
        const struct ugfx_font *was = push_value_font(m);
        used += ugfx_char_h() - line;
        ugfx_set_font(was);
    }
    int ty = cy - used / 2;
    for (int i = 0; i < RING_TEXT_ROWS; i++) {
        if (!rows[i]) continue;
        const struct ugfx_font *was = (i == 0) ? push_value_font(m) : NULL;
        int tw = ugfx_text_width(rows[i]);
        int tx = cx - tw / 2;
        if (tx < cx - hole / 2) tx = cx - hole / 2;
        ugfx_draw_string_clipped(s, tx, ty, hole, rows[i], m->fg, m->bg);
        ty += ugfx_char_h();
        if (was) ugfx_set_font(was);
    }

    // The detail, centred UNDER the ring on the tile's full width --
    // where a long string fits.
    if (m->detail) {
        int dw = ugfx_text_width(m->detail);
        int dx = x + (inner - dw) / 2;
        if (dx < x) dx = x;
        ugfx_draw_string_clipped(s, dx, top + d, inner, m->detail, m->fg, m->bg);
    }
}

#define CARD_R 8

void uui_meter_draw(struct ugfx_surface *s, const struct uui_meter *m) {
    if (m->w <= 0 || m->h <= 0) return;

    // A ROUNDED CARD with a hairline, Windows 11's -- and the accent
    // ring, two pixels, when this is the reading being taken.
    uint32_t edge = m->active ? UTHEME_ACCENT : ugfx_blend(UTHEME_OUTLINE, m->bg, 120);
    int ring = m->active ? 2 : 1;
    uui_fill_round_rect(s, m->x, m->y, m->w, m->h, CARD_R, edge);
    uui_fill_round_rect(s, m->x + ring, m->y + ring, m->w - 2 * ring, m->h - 2 * ring,
                        CARD_R - ring, m->bg);

    if (m->style == UUI_METER_RING) {
        draw_ring(s, m);
        draw_spark(s, m);
        return;
    }

    int pad = utheme_pad();
    int line = ugfx_char_h();
    int x = m->x + pad;
    int y = m->y + pad;
    int inner = m->w - pad * 2;
    if (inner <= 0) return;

    // CLIPPED, all of it: ugfx_draw_string does not clip, and a value
    // wider than its tile would run into the one beside it -- the
    // identical overlap bug this codebase has shipped twice
    // (docs/gui-guidelines.md).
    uint32_t dim = ugfx_blend(m->fg, m->bg, 110);
    if (m->caption) {
        const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        ugfx_draw_string_clipped(s, x, y, inner, m->caption, dim, m->bg);
        ugfx_set_font(was);
        y += line;
    }

    // The unit BESIDE the number, on its baseline row: "69.4 MB/s".
    {
        const struct ugfx_font *was = push_value_font(m);
        int vh = ugfx_char_h();
        int vw = m->value ? ugfx_text_width(m->value) : 0;
        if (m->value)
            ugfx_draw_string_clipped(s, x, y, inner, m->value,
                                     m->value_fg ? m->value_fg : m->fg, m->bg);
        ugfx_set_font(was);
        if (m->unit && vw + line / 2 < inner)
            ugfx_draw_string_clipped(s, x + vw + line / 3, y + vh - line - line / 6,
                                     inner - vw - line / 3, m->unit, dim, m->bg);
        y += vh;
    }

    if (m->detail) {
        ugfx_draw_string_clipped(s, x, y, inner, m->detail, dim, m->bg);
        y += line;
    }

    if (m->fill >= 0) {
        int fill = m->fill > 1000 ? 1000 : m->fill;   // clamped, not overflowing
        // THREE QUARTERS OF A ROW, not half: at a tile's full width a
        // half-row strip reads as a border along the bottom edge rather
        // than as a meter, which is exactly how it looked.
        int bar_h = line * 3 / 4;
        if (bar_h < 3) bar_h = 3;
        // On its OWN reserved row at the bottom, not hung off the box's
        // edge: the rows above are reserved whether or not they carry
        // text, so this cannot land on top of one.
        int bar_y = m->y + m->h - pad - bar_h;
        if (bar_y < y) bar_y = y;
        if (bar_h > line / 2) bar_h = line / 2;
        if (bar_h < 4) bar_h = 4;
        bar_y = m->y + m->h - pad - bar_h;
        uui_fill_round_rect(s, x, bar_y, inner, bar_h, UUI_CAPSULE,
                            uui_state_bg(UTHEME_PANEL_BG, UUI_STATE_HOVER));
        int on = (inner * fill) / 1000;
        if (on >= bar_h) uui_fill_round_rect(s, x, bar_y, on, bar_h, UUI_CAPSULE, m->accent);
        else if (on > 0) ugfx_fill_rect(s, x, bar_y, on, bar_h, m->accent);
    }
}

static void meter_draw_op(struct ugfx_surface *s, const void *w) {
    uui_meter_draw(s, (const struct uui_meter *)w);
}
static void meter_natural_op(const void *w, int *out_w, int *out_h) {
    uui_meter_natural_size((const struct uui_meter *)w, out_w, out_h);
}
static void meter_geometry_op(void *w, int x, int y, int width, int height) {
    struct uui_meter *m = w;
    m->x = x; m->y = y; m->w = width; m->h = height;
}
static void meter_bounds_op(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_meter *m = w;
    *x = m->x; *y = m->y; *ow = m->w; *oh = m->h;
}

// WHERE THE TILE LANDED, so a test can ask instead of reading pixels
// (docs/gui-guidelines.md). A meter is laid out in a grid beside others
// and "are these cells uniform" is exactly the question a screenshot
// answers badly -- Task Manager's Overview shipped a last row wider
// than the row above it and every check passed.
static void meter_describe_op(const void *w, const struct uui_describe *d) {
    const struct uui_meter *m = w;
    uui_describe_rect(d, "tile", m->x, m->y, m->w, m->h);
}

// No `hit`: a reading is not a control, so a click passes through to
// whatever is behind. widget-ops-ok: display-only, like uui_label.
const struct uui_widget_ops uui_meter_ops = {
    .bounds = meter_bounds_op,
    .draw = meter_draw_op,
    .natural_size = meter_natural_op,
    .set_geometry = meter_geometry_op,
    .describe = meter_describe_op,
};
