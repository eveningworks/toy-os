// meter -- see ui/uui_meter.h for why this exists at all.
#include "ui/uui_meter.h"
#include "ui/uui_widget.h"
#include "ui/utheme.h"
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
#define METER_TEXT_ROWS 3   // caption, unit, detail; the value is measured apart

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
}

void uui_meter_draw(struct ugfx_surface *s, const struct uui_meter *m) {
    if (m->w <= 0 || m->h <= 0) return;

    ugfx_fill_rect(s, m->x, m->y, m->w, m->h, m->bg);
    ugfx_draw_rect(s, m->x, m->y, m->w, m->h, UTHEME_BORDER);

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
    if (m->caption) {
        ugfx_draw_string_clipped(s, x, y, inner, m->caption, m->fg, m->bg);
        y += line;
    }

    if (m->value) {
        const struct ugfx_font *was = push_value_font(m);
        ugfx_draw_string_clipped(s, x, y, inner, m->value, m->fg, m->bg);
        y += ugfx_char_h();
        ugfx_set_font(was);
    }

    if (m->unit) {
        ugfx_draw_string_clipped(s, x, y, inner, m->unit, m->fg, m->bg);
        y += line;
    }

    if (m->detail) {
        ugfx_draw_string_clipped(s, x, y, inner, m->detail, m->fg, m->bg);
        y += line;
    }

    if (m->fill >= 0) {
        int fill = m->fill > 1000 ? 1000 : m->fill;   // clamped, not overflowing
        int bar_h = line / 2;
        if (bar_h < 2) bar_h = 2;
        // On its OWN reserved row at the bottom, not hung off the box's
        // edge: the rows above are reserved whether or not they carry
        // text, so this cannot land on top of one.
        int bar_y = m->y + m->h - pad - bar_h;
        if (bar_y < y) bar_y = y;
        ugfx_fill_rect(s, x, bar_y, inner, bar_h, UTHEME_PANEL_BG);
        int on = (inner * fill) / 1000;
        if (on > 0) ugfx_fill_rect(s, x, bar_y, on, bar_h, m->accent);
        ugfx_draw_rect(s, x, bar_y, inner, bar_h, UTHEME_BORDER);
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

// No `hit`: a reading is not a control, so a click passes through to
// whatever is behind. widget-ops-ok: display-only, like uui_label.
const struct uui_widget_ops uui_meter_ops = {
    .bounds = meter_bounds_op,
    .draw = meter_draw_op,
    .natural_size = meter_natural_op,
    .set_geometry = meter_geometry_op,
};
