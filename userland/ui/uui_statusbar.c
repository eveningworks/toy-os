// status bar. See ui/uui_statusbar.h for the design.
#include "ui/uui_statusbar.h"
#include "ui/uui_primitives.h"
#include "ui/uui_widget.h"  // the ops table at the bottom of this file

static int pad(void) { return ugfx_char_w() / 2; }

// A fixed pane's width. One derivation, used by drawing and by
// pane_rect() alike -- the same rule the scrollbar's sb_geometry()
// follows, for the same reason.
//
// **`chars` IS RESERVED IN DIGITS, NOT IN THE WIDEST GLYPH.** It used
// to be `chars * ugfx_char_w()`, which is the same number on a
// monospace face and far too generous on a proportional one -- the
// fixed panes then ate the width the STRETCHING pane needed and its
// text was clipped ("Ctrl-S sa"). A pane that declares a character
// count is almost always reserving room for a number, and digits are
// the same width in any sane face, so that is what it reserves.
//
// Never narrower than the text actually in it, because a pane holding
// a WORD would otherwise be cut by a reservation meant for digits --
// and a pane that clips its own content is worse than one a few pixels
// wider than its neighbour.
static int fixed_w(const struct uui_status_pane *p) {
    int per = ugfx_char_advance('0');
    if (per <= 0) per = ugfx_char_w();
    int w = p->chars * per;
    int textw = p->text ? ugfx_text_width(p->text) : 0;
    if (textw > w) w = textw;
    return w + 2 * pad();
}

void uui_statusbar_init(struct uui_statusbar *sb) {
    sb->x = sb->y = sb->w = sb->h = 0;
    sb->panes[0].text = 0;
    sb->panes[0].chars = 0;
    sb->count = 1;
    sb->bg     = UUI_COLOR_UNSET; // UTHEME_BAR_BG, resolved at draw
    sb->fg     = ugfx_rgb(90, 100, 115);
    sb->border = ugfx_rgb(200, 205, 215);
}

void uui_statusbar_set_geometry(struct uui_statusbar *sb, int x, int y, int w, int h) {
    sb->x = x; sb->y = y; sb->w = w; sb->h = h;
}

void uui_statusbar_natural_size(const struct uui_statusbar *sb, int *out_w, int *out_h) {
    (void)sb;
    if (out_w) *out_w = 0; // no preference -- see the header
    if (out_h) *out_h = ugfx_char_h() + 5; // +1 for the hairline on top
}

int uui_statusbar_pane_rect(const struct uui_statusbar *sb, int index,
                             int *x, int *y, int *w, int *h) {
    if (index < 0 || index >= sb->count) return 0;

    int fixed = 0, stretchers = 0;
    for (int i = 0; i < sb->count; i++) {
        if (sb->panes[i].chars > 0) fixed += fixed_w(&sb->panes[i]);
        else stretchers++;
    }

    // Whatever the fixed panes leave, split evenly. A stretcher is
    // allowed to end up at zero width on a very narrow window; it
    // clips its own text rather than pushing the indicators off.
    int spare = sb->w - fixed;
    if (spare < 0) spare = 0;
    int each = stretchers > 0 ? spare / stretchers : 0;

    int px = sb->x;
    for (int i = 0; i < index; i++)
        px += sb->panes[i].chars > 0 ? fixed_w(&sb->panes[i]) : each;

    int pw = sb->panes[index].chars > 0 ? fixed_w(&sb->panes[index]) : each;

    // The last pane absorbs the rounding, so the strip has no seam at
    // its right edge.
    if (index == sb->count - 1) pw = sb->x + sb->w - px;

    if (x) *x = px;
    if (y) *y = sb->y + 1; // below the hairline
    if (w) *w = pw;
    if (h) *h = sb->h - 1;
    return 1;
}

void uui_statusbar_draw(struct ugfx_surface *s, const struct uui_statusbar *sb) {
    uint32_t bg = UUI_COLOR(sb->bg, UTHEME_BAR_BG);
    ugfx_fill_rect(s, sb->x, sb->y, sb->w, sb->h, bg);
    ugfx_fill_rect(s, sb->x, sb->y, sb->w, 1, sb->border);

    for (int i = 0; i < sb->count; i++) {
        int x, y, w, h;
        if (!uui_statusbar_pane_rect(sb, i, &x, &y, &w, &h)) continue;

        // A hairline BEFORE each pane but the first: separators between
        // parts, not a box around each, which is what keeps a flat strip
        // from reading as a row of buttons.
        if (i > 0) ugfx_fill_rect(s, x, y + 2, 1, h - 4, sb->border);

        if (!sb->panes[i].text) continue;
        int tx = x + pad();
        int avail = w - 2 * pad();
        if (avail <= 0) continue;
        ugfx_draw_string_clipped(s, tx, y + (h - ugfx_char_h()) / 2, avail,
                                  sb->panes[i].text, sb->fg, bg);
    }
}

// --- as a LAYOUT widget (ui/uui_widget.h) ----------------------------
//
// So an app can declare a status bar in uapp_desc.widgets and have the
// layout reserve its height, instead of positioning it by hand against
// the content rect -- which is what the ring-3 Notepad does, and which
// means every app doing it repeats the same "content height minus the
// bar" arithmetic. Getting that arithmetic slightly wrong does not
// fail loudly: the bar simply overlaps whatever is above it.
//
// No input slots on purpose. A status bar reports; it is not a control.
// Adding `hit` would make it swallow clicks meant for the window
// beneath, which is the opposite of what it is for.
static void sb_ops_natural_size(const void *w, int *out_w, int *out_h) {
    uui_statusbar_natural_size((const struct uui_statusbar *)w, out_w, out_h);
}

static void sb_ops_set_geometry(void *w, int x, int y, int width, int height) {
    uui_statusbar_set_geometry((struct uui_statusbar *)w, x, y, width, height);
}

static void sb_ops_draw(struct ugfx_surface *s, const void *w) {
    uui_statusbar_draw(s, (const struct uui_statusbar *)w);
}

static void st_ops_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_statusbar *s = (const struct uui_statusbar *)w;
    *x = s->x; *y = s->y; *ow = s->w; *oh = s->h;
}

// pane i -- the rects a test samples (ui/uui_describe.h).
static void sb_ops_describe(const void *w, const struct uui_describe *d) {
    const struct uui_statusbar *sb = w;
    for (int i = 0; i < sb->count; i++) {
        int x, y, pw, ph;
        if (uui_statusbar_pane_rect(sb, i, &x, &y, &pw, &ph))
            uui_describe_rect_i(d, "pane", i, x, y, pw, ph);
    }
}

const struct uui_widget_ops uui_statusbar_ops = {
    .natural_size = sb_ops_natural_size,
    .set_geometry = sb_ops_set_geometry,
    .draw         = sb_ops_draw,
    .bounds = st_ops_bounds,
    .describe     = sb_ops_describe,
};
