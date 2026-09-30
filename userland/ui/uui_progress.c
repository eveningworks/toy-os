// See uui_progress.h.
#include "ui/uui_progress.h"
#include "ui/uui_describe.h"
#include "ui/uui_widget.h"
#include "ui/utheme.h"

#define BUSY_STEPS 64   // one sweep of the busy block, in ticks

void uui_progress_init(struct uui_progress *p) {
    p->x = p->y = p->w = p->h = 0;
    p->value = 0;
    p->phase = 0;
    p->track = UTHEME_WHITE;
    p->fill = UTHEME_ACCENT;
    p->border = UTHEME_OUTLINE;
}

void uui_progress_set(struct uui_progress *p, int per_mille) {
    p->value = per_mille < 0 ? 0 : per_mille > 1000 ? 1000 : per_mille;
}

void uui_progress_set_busy(struct uui_progress *p) {
    if (p->value >= 0) p->phase = 0;
    p->value = -1;
}

int uui_progress_tick(struct uui_progress *p) {
    if (p->value >= 0) return 0;
    p->phase = (p->phase + 1) % BUSY_STEPS;
    return 1;
}

// Two thirds of a line: tall enough to read as a bar across a whole
// window, short enough not to pass for a text field.
void uui_progress_natural_size(const struct uui_progress *p, int *out_w, int *out_h) {
    (void)p;
    int h = ugfx_char_h() * 2 / 3;
    if (h < 6) h = 6;
    *out_w = ugfx_char_w() * 16;
    *out_h = h;
}

void uui_progress_draw(struct ugfx_surface *s, const struct uui_progress *p) {
    if (p->w <= 2 || p->h <= 2) return;
    int ix = p->x + 1, iy = p->y + 1, iw = p->w - 2, ih = p->h - 2;
    ugfx_fill_rect(s, ix, iy, iw, ih, p->track);
    if (p->value >= 0) {
        int on = iw * p->value / 1000;
        if (on > 0) ugfx_fill_rect(s, ix, iy, on, ih, p->fill);
    } else {
        // A quarter-width block entering at the left and leaving at the
        // right, clipped to the track at both ends.
        int blk = iw / 4 ? iw / 4 : 1;
        int left = (iw + blk) * p->phase / BUSY_STEPS - blk;
        int x0 = left < 0 ? 0 : left;
        int x1 = left + blk > iw ? iw : left + blk;
        if (x1 > x0) ugfx_fill_rect(s, ix + x0, iy, x1 - x0, ih, p->fill);
    }
    ugfx_draw_rect(s, p->x, p->y, p->w, p->h, p->border);
}

static void natural_op(const void *w, int *ow, int *oh) {
    uui_progress_natural_size((const struct uui_progress *)w, ow, oh);
}
static void geometry_op(void *w, int x, int y, int width, int height) {
    struct uui_progress *p = w;
    p->x = x; p->y = y; p->w = width; p->h = height;
}
static void bounds_op(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_progress *p = w;
    *x = p->x; *y = p->y; *ow = p->w; *oh = p->h;
}
static void draw_op(struct ugfx_surface *s, const void *w) {
    uui_progress_draw(s, (const struct uui_progress *)w);
}
// The VALUE, so a test reads "how far" from the widget rather than by
// counting accent pixels -- and can check the pixels against it.
static void describe_op(const void *w, const struct uui_describe *d) {
    uui_describe_int(d, "value", ((const struct uui_progress *)w)->value);
}

// No `hit`: a progress bar is not a control. widget-ops-ok: display-only, like uui_meter.
const struct uui_widget_ops uui_progress_ops = {
    .bounds = bounds_op,
    .draw = draw_op,
    .natural_size = natural_op,
    .set_geometry = geometry_op,
    .describe = describe_op,
};
