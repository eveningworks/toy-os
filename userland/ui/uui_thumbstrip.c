// uui_thumbstrip -- a filmstrip. See uui_thumbstrip.h.
#include "ui/uui_thumbstrip.h"
#include "ui/uui_primitives.h"
#include "ui/utheme.h"

#define RING 3   // the selection ring's width, in pixels

// THE CELL IS FONT-DERIVED, 16:9 -- the shape of a screen and of most
// photographs, so a wallpaper fills it and a portrait is letterboxed.
static int cell_h(void) { int h = ugfx_char_h() * 4; return h < 32 ? 32 : h; }
static int cell_w(void) { return cell_h() * 16 / 9; }
static int gap(void)    { return utheme_pad() + RING; }

void uui_thumbstrip_init(struct uui_thumbstrip *t) {
    *t = (struct uui_thumbstrip){0};
    t->selected = t->hot = t->armed = t->committed = -1;
    t->bg    = ugfx_rgb(38, 38, 43);
    t->sel   = ugfx_rgb(240, 182, 94);
    t->hover = ugfx_rgb(150, 150, 160);
    t->empty = ugfx_rgb(58, 58, 64);
}

void uui_thumbstrip_set(struct uui_thumbstrip *t, int count,
                        const struct uimg *(*thumb)(void *ctx, int index, int px),
                        void *ctx) {
    t->count = count < 0 ? 0 : count;
    t->thumb = thumb;
    t->ctx = ctx;
    t->scroll = 0;
    t->hot = t->armed = t->committed = -1;
    if (t->selected >= t->count) t->selected = -1;
}

static int row_w(const struct uui_thumbstrip *t) {
    return t->count ? t->count * cell_w() + (t->count - 1) * gap() : 0;
}

static int avail_w(const struct uui_thumbstrip *t) { return t->w - 2 * gap(); }

static int max_scroll(const struct uui_thumbstrip *t) {
    int m = row_w(t) - avail_w(t);
    return m > 0 ? m : 0;
}

// Where the row starts: centred when it fits, scrolled when it does not.
static int row_x(const struct uui_thumbstrip *t) {
    int rw = row_w(t), aw = avail_w(t);
    if (rw <= aw) return t->x + (t->w - rw) / 2;
    return t->x + gap() - t->scroll;
}

int uui_thumbstrip_cell_rect(const struct uui_thumbstrip *t, int i,
                             int *x, int *y, int *w, int *h) {
    if (i < 0 || i >= t->count) return 0;
    int cx = row_x(t) + i * (cell_w() + gap());
    if (cx + cell_w() <= t->x || cx >= t->x + t->w) return 0;
    if (x) *x = cx;
    if (y) *y = t->y + (t->h - cell_h()) / 2;
    if (w) *w = cell_w();
    if (h) *h = cell_h();
    return 1;
}

static int cell_at(const struct uui_thumbstrip *t, int px, int py) {
    for (int i = 0; i < t->count; i++) {
        int x, y, w, h;
        if (uui_thumbstrip_cell_rect(t, i, &x, &y, &w, &h) &&
            uui_hit(x, y, w, h, px, py))
            return i;
    }
    return -1;
}

void uui_thumbstrip_select(struct uui_thumbstrip *t, int index) {
    t->selected = (index >= 0 && index < t->count) ? index : -1;
    if (t->selected < 0 || row_w(t) <= avail_w(t)) return;
    int left = t->selected * (cell_w() + gap());
    if (left < t->scroll) t->scroll = left;
    else if (left + cell_w() > t->scroll + avail_w(t))
        t->scroll = left + cell_w() - avail_w(t);
    if (t->scroll > max_scroll(t)) t->scroll = max_scroll(t);
}

int uui_thumbstrip_take(struct uui_thumbstrip *t) {
    int c = t->committed;
    t->committed = -1;
    return c;
}

// --- drawing ----------------------------------------------------------

static void ring(struct ugfx_surface *s, int x, int y, int w, int h, int n, uint32_t c) {
    ugfx_fill_rect(s, x - n, y - n, w + 2 * n, n, c);
    ugfx_fill_rect(s, x - n, y + h, w + 2 * n, n, c);
    ugfx_fill_rect(s, x - n, y, n, h, c);
    ugfx_fill_rect(s, x + w, y, n, h, c);
}

// A dim of `bg` over a cell, row by row -- the unselected cells sit back
// so the selected one reads at a glance (the mockup's 70% opacity).
static void dim(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t bg) {
    for (int r = 0; r < h; r++) ugfx_blend_hspan(s, x, y + r, w, bg, 0, 80);
}

static void draw(struct ugfx_surface *s, const void *w) {
    const struct uui_thumbstrip *t = w;
    // CLIPPED TO THE STRIP: a scrolled row's edge cells are partly
    // outside it, and a panel beside the strip must not be painted over.
    struct ugfx_clip c;
    ugfx_clip_save(s, &c);
    ugfx_clip_intersect(s, t->x, t->y, t->w, t->h);
    ugfx_fill_rect(s, t->x, t->y, t->w, t->h, t->bg);
    for (int i = 0; i < t->count; i++) {
        int x, y, cw, ch;
        if (!uui_thumbstrip_cell_rect(t, i, &x, &y, &cw, &ch)) continue;
        const struct uimg *im = t->thumb ? t->thumb(t->ctx, i, cw) : 0;
        if (im && im->w > 0 && im->h > 0) {
            int dw, dh;
            uimg_fit_size(im->w, im->h, cw, ch, UIMG_FIT_CONTAIN, &dw, &dh);
            if (dw < cw || dh < ch) ugfx_fill_rect(s, x, y, cw, ch, t->empty);
            ugfx_blit_scaled_alpha(s, x + (cw - dw) / 2, y + (ch - dh) / 2, dw, dh,
                                   im->px, im->w, im->h, im->w, 255);
        } else {
            ugfx_fill_rect(s, x, y, cw, ch, t->empty);
        }
        if (i == t->selected) {
            ring(s, x, y, cw, ch, RING, t->sel);
            // A soft glow outside the ring, as the mockup's: two fading
            // rows of the same colour.
            for (int g = 1; g <= 2; g++) {
                uint32_t c = ugfx_blend(t->bg, t->sel, (uint8_t)(110 / g));
                ring(s, x - RING - g + 1, y - RING - g + 1, cw + 2 * (RING + g - 1),
                     ch + 2 * (RING + g - 1), 1, c);
            }
        } else {
            dim(s, x, y, cw, ch, t->bg);
            if (i == t->hot) ring(s, x, y, cw, ch, 2, t->hover);
        }
    }
    ugfx_clip_restore(s, &c);
}

// --- the ops table -----------------------------------------------------

static void natural_size(const void *w, int *out_w, int *out_h) {
    (void)w;
    *out_w = cell_w() + 2 * gap();
    *out_h = cell_h() + 2 * (gap() + RING);
}

static void set_geometry(void *w, int x, int y, int width, int height) {
    struct uui_thumbstrip *t = w;
    t->x = x; t->y = y; t->w = width; t->h = height;
    if (t->scroll > max_scroll(t)) t->scroll = max_scroll(t);
}

static void bounds(const void *w, int *x, int *y, int *out_w, int *out_h) {
    const struct uui_thumbstrip *t = w;
    *x = t->x; *y = t->y; *out_w = t->w; *out_h = t->h;
}

static int hit(const void *w, int cx, int cy) {
    const struct uui_thumbstrip *t = w;
    return uui_hit(t->x, t->y, t->w, t->h, cx, cy);
}

// TAKES THE PRESS (returns 1) anywhere on the strip: the router routes
// the release only to a widget that took its press.
static int press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_thumbstrip *t = w;
    t->armed = cell_at(t, cx, cy);
    return 1;
}

static int motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_thumbstrip *t = w;
    int hot = uui_hit(t->x, t->y, t->w, t->h, cx, cy) ? cell_at(t, cx, cy) : -1;
    if (hot == t->hot) return 0;
    t->hot = hot;
    return 1;
}

static int release(void *w, int cx, int cy) {
    struct uui_thumbstrip *t = w;
    int c = cell_at(t, cx, cy);
    int was = t->armed;
    t->armed = -1;
    if (c < 0 || c != was) return 0;
    t->committed = c;
    uui_thumbstrip_select(t, c);
    return 1;
}

static int wheel(void *w, int notches) {
    struct uui_thumbstrip *t = w;
    int ms = max_scroll(t);
    if (!ms) return 0;
    int s = t->scroll - notches * (cell_w() + gap());   // up/away moves left
    if (s < 0) s = 0;
    if (s > ms) s = ms;
    if (s == t->scroll) return 0;
    t->scroll = s;
    return 1;
}

static void describe(const void *w, const struct uui_describe *d) {
    const struct uui_thumbstrip *t = w;
    uui_describe_int(d, "count", t->count);
    uui_describe_int(d, "selected", t->selected);
    for (int i = 0; i < t->count; i++) {
        int x, y, cw, ch;
        if (uui_thumbstrip_cell_rect(t, i, &x, &y, &cw, &ch))
            uui_describe_rect_i(d, "cell", i, x, y, cw, ch);
    }
}

const struct uui_widget_ops uui_thumbstrip_ops = {
    .natural_size = natural_size,
    .set_geometry = set_geometry,
    .bounds       = bounds,
    .draw         = draw,
    .hit          = hit,
    .press        = press,
    .motion       = motion,
    .release      = release,
    .wheel        = wheel,
    .describe     = describe,
};
