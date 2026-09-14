// label -- see ui/uui_label.h for why this exists at all.
#include "ui/uui_label.h"
#include "ui/uui_widget.h"
#include <stddef.h>

void uui_label_init(struct uui_label *l, const char *text) {
    l->x = l->y = l->w = l->h = 0;
    l->text = text;
    l->fg = ugfx_rgb(20, 20, 20);
    l->bg = ugfx_rgb(245, 245, 245);
    l->rows = 1;
    l->wrap = 0;
}

void uui_label_set_wrap(struct uui_label *l, int rows) {
    l->wrap = 1;
    l->rows = rows > 0 ? rows : 1;
}

void uui_label_set_text(struct uui_label *l, const char *text) {
    l->text = text;
}

// Selects the label's font for the duration of one measurement or one
// paint, restoring whatever was current. Both halves need it and both
// must agree -- see the `font` field's comment in uui_label.h.
static void uui_label_draw_body(struct ugfx_surface *s, const struct uui_label *l);

static const struct ugfx_font *push_font(const struct uui_label *l) {
    return ugfx_set_font(l->font); // NULL means the session regular weight
}

void uui_label_natural_size(const struct uui_label *l, int *out_w, int *out_h) {
    // The TEXT's width, but a height that does not depend on it: a
    // caption whose row count varied with its content would reflow the
    // page every time it changed. See CLAUDE.md on natural_size.
    //
    // A WRAPPING LABEL ASKS FOR ALMOST NO WIDTH, deliberately. Its
    // whole point is to fit the width it is given, so reporting the
    // width of its longest line would make a long sentence demand a
    // page wider than the window -- which is the problem wrapping was
    // added to solve, arriving from the other direction. It takes its
    // width from UUI_FILL_W instead; the floor is so that a label
    // declared without it still draws something rather than nothing.
    const struct ugfx_font *was = push_font(l);
    if (out_w) {
        if (l->wrap) *out_w = ugfx_char_w() * 8;
        else         *out_w = l->text ? ugfx_text_width(l->text) : 0;
    }
    // ROWS AT THE LINE PITCH, plus the last row's descender overhang if
    // this label asked for it (`descender_room` -- off by default, and
    // that header comment says what it cost to learn). Only the LAST row
    // could ever need it: the rows above overhang into each other, which
    // is exactly what the pitch already accounts for.
    if (out_h) {
        int rows = l->rows > 0 ? l->rows : 1;
        int overhang = 0;
        if (l->descender_room) {
            overhang = ugfx_glyph_h() - ugfx_char_h();
            if (overhang < 0) overhang = 0;
        }
        *out_h = ugfx_char_h() * rows + overhang;
    }
    ugfx_set_font(was);
}

// See uui_label.h. Not static, so it can be tested directly: the
// "consume something every call" rule below is what stops a word wider
// than the line from looping forever, and that failure would be a hung
// COMPOSITOR rather than a wrong pixel -- worth asserting rather than
// reasoning about.
const char *uui_label_wrap_next(const char *src, int max_w, char *out, int cap) {
    while (*src == ' ') src++;             // no leading space on a fresh line
    int fit = ugfx_text_fit_chars(src, max_w);
    if (fit <= 0) fit = 1;                 // always consume something
    if (fit >= cap) fit = cap - 1;

    int take = fit;
    if (src[fit] != '\0' && src[fit] != ' ') {
        // Mid-word: back up to the last space that is still on this
        // line. If there is none, the word is longer than the line and
        // gets broken at `fit`.
        int b = fit;
        while (b > 0 && src[b - 1] != ' ') b--;
        if (b > 0) take = b;
    }
    for (int i = 0; i < take; i++) out[i] = src[i];
    // Trim the trailing space so it cannot be mistaken for indentation
    // on a right-aligned or bordered draw later.
    while (take > 0 && out[take - 1] == ' ') take--;
    out[take] = '\0';
    return src + (take > 0 ? take : fit);
}

void uui_label_draw(struct ugfx_surface *s, const struct uui_label *l) {
    const struct ugfx_font *was = push_font(l);
    uui_label_draw_body(s, l);
    ugfx_set_font(was);
}

static void uui_label_draw_body(struct ugfx_surface *s, const struct uui_label *l) {
    if (!l->text || !l->text[0]) return;
    if (!l->wrap) {
        // **CENTRED IN WHATEVER HEIGHT IT WAS GIVEN.** A label's box is
        // its own text height unless a layout stretched it, and then
        // the centre IS the top -- so this is a no-op for every label
        // that is not filling. Where it matters is a form caption
        // beside a taller control (a spinbox, a field): drawn at l->y
        // it floats at the top of the row and reads as misaligned,
        // which is what every toolkit centres it to avoid.
        int th = ugfx_char_h();
        int ty = l->h > th ? l->y + (l->h - th) / 2 : l->y;
        ugfx_draw_string_clipped(s, l->x, ty, l->w, l->text, l->fg, l->bg);
        return;
    }

    int rows = l->rows > 0 ? l->rows : 1;
    int line_h = ugfx_char_h();
    const char *p = l->text;
    char line[128];

    for (int r = 0; r < rows && *p; r++) {
        const char *next = uui_label_wrap_next(p, l->w, line, (int)sizeof line);
        // THE LAST LINE SAYS SO WHEN THERE IS MORE. A sentence that
        // simply stops looks like the text ends there; "..." is the
        // difference between a truncated caption and a wrong one. Only
        // on the final row, and only if anything is actually left.
        if (r == rows - 1) {
            const char *rest = next;
            while (*rest == ' ') rest++;
            if (*rest) {
                int n = 0;
                while (line[n]) n++;
                while (n > 0 && ugfx_text_width(line) + ugfx_text_width("...") > l->w) {
                    line[--n] = '\0';
                }
                if (n + 3 < (int)sizeof line) {
                    line[n] = '.'; line[n+1] = '.'; line[n+2] = '.'; line[n+3] = '\0';
                }
            }
        }
        ugfx_draw_string_clipped(s, l->x, l->y + r * line_h, l->w, line, l->fg, l->bg);
        p = next;
    }
}

static void label_draw_op(struct ugfx_surface *s, const void *w) {
    uui_label_draw(s, (const struct uui_label *)w);
}
static void label_natural_op(const void *w, int *out_w, int *out_h) {
    uui_label_natural_size((const struct uui_label *)w, out_w, out_h);
}
static void label_geometry_op(void *w, int x, int y, int width, int height) {
    struct uui_label *l = w;
    l->x = x; l->y = y; l->w = width; l->h = height;
}

// NO `hit`, deliberately -- so the router never offers a label a press
// and a click goes to whatever is behind it. A caption that swallowed
// clicks would be a control that does nothing, which is the shape of
// bug this toolkit keeps a rule about.
static void label_bounds_op(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_label *c = w;
    *x = c->x; *y = c->y; *ow = c->w; *oh = c->h;
}

const struct uui_widget_ops uui_label_ops = {
    .bounds = label_bounds_op,
    .draw = label_draw_op,
    .natural_size = label_natural_op,
    .set_geometry = label_geometry_op,
};
