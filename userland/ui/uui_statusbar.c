// status bar. See ui/uui_statusbar.h for the design.
#include "ui/uui_statusbar.h"
#include "ui/uui_primitives.h"

static int pad(void) { return ugfx_char_w() / 2; }

// A fixed pane's width. One derivation, used by drawing and by
// pane_rect() alike -- the same rule the scrollbar's sb_geometry()
// follows, for the same reason.
static int fixed_w(const struct uui_status_pane *p) {
    return p->chars * ugfx_char_w() + 2 * pad();
}

void uui_statusbar_init(struct uui_statusbar *sb) {
    sb->x = sb->y = sb->w = sb->h = 0;
    sb->panes[0].text = 0;
    sb->panes[0].chars = 0;
    sb->count = 1;
    sb->bg     = ugfx_rgb(235, 235, 238);
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
    ugfx_fill_rect(s, sb->x, sb->y, sb->w, sb->h, sb->bg);
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
                                  sb->panes[i].text, sb->fg, sb->bg);
    }
}
