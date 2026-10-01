// See uui_transport.h.
#include "ui/uui_transport.h"
#include "ui/uui_primitives.h"
#include "ui/utheme.h"

// FONT-DERIVED: the play disc is two lines of text tall, a side button
// three quarters of it, and they stand a character apart.
static int disc(void) { return ugfx_char_h() * 2 + 8; }
static int side(void) { return disc() * 3 / 4; }
static int gap(void)  { return ugfx_char_w(); }

void uui_transport_init(struct uui_transport *t) {
    *t = (struct uui_transport){ 0 };
}

int uui_transport_take(struct uui_transport *t) {
    int p = t->committed;
    t->committed = UUI_TRANSPORT_NONE;
    return p;
}

void uui_transport_part(const struct uui_transport *t, int part, int *x, int *y, int *w, int *h) {
    int d = disc(), s = side(), g = gap();
    int total = 2 * s + d + 2 * g;
    int x0 = t->x + (t->w - total) / 2, cy = t->y + t->h / 2;
    switch (part) {
    case UUI_TRANSPORT_PREV: *x = x0;                 *y = cy - s / 2; *w = s; *h = s; return;
    case UUI_TRANSPORT_PLAY: *x = x0 + s + g;         *y = cy - d / 2; *w = d; *h = d; return;
    case UUI_TRANSPORT_NEXT: *x = x0 + s + g + d + g; *y = cy - s / 2; *w = s; *h = s; return;
    default: *x = *y = *w = *h = 0;
    }
}

static int part_at(const struct uui_transport *t, int cx, int cy) {
    for (int p = UUI_TRANSPORT_PREV; p <= UUI_TRANSPORT_NEXT; p++) {
        int x, y, w, h;
        uui_transport_part(t, p, &x, &y, &w, &h);
        // A DISC, not its box: the corners of a round button are not it.
        long dx = 2L * (cx - x) - w, dy = 2L * (cy - y) - h;
        if (dx * dx + dy * dy <= (long)w * w) return p;
    }
    return UUI_TRANSPORT_NONE;
}

static int live(const struct uui_transport *t, int p) {
    if (t->disabled || p == UUI_TRANSPORT_NONE) return 0;
    if (p == UUI_TRANSPORT_PREV) return !t->no_prev;
    if (p == UUI_TRANSPORT_NEXT) return !t->no_next;
    return 1;
}

// How much of pixel (x, y) a disc of radius r about (cx, cy) covers,
// 0..16, from a 4x4 grid of sub-samples -- the edge a smooth disc needs.
static int coverage(int x, int y, int cx, int cy, int r) {
    int n = 0;
    long rr = (long)r * r * 64;                 // in 1/8ths of a pixel, squared
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
            long dx = (long)(x - cx) * 8 + sx * 2 - 3, dy = (long)(y - cy) * 8 + sy * 2 - 3;
            if (dx * dx + dy * dy <= rr) n++;
        }
    return n;
}

// The ground under a disc, washed toward `c` -- whatever it is, a theme
// grey or a picture's ambient stage -- with an anti-aliased rim.
static void wash(struct ugfx_surface *s, int cx, int cy, int r, uint32_t c, uint8_t a) {
    int x0 = s->clip_active ? s->clip_x0 : 0, x1 = s->clip_active ? s->clip_x1 : s->w;
    int y0 = s->clip_active ? s->clip_y0 : 0, y1 = s->clip_active ? s->clip_y1 : s->h;
    for (int y = cy - r - 1; y <= cy + r + 1; y++) {
        if (y < y0 || y >= y1) continue;
        for (int x = cx - r - 1; x <= cx + r + 1; x++) {
            if (x < x0 || x >= x1) continue;
            int cov = coverage(x, y, cx, cy, r);
            if (!cov) continue;
            uint32_t *p = &s->pixels[(long)y * s->w + x];
            *p = ugfx_blend(*p, c, (uint8_t)(a * cov / 16));
        }
    }
}

// Smooth edges come from ugfx itself: its fills are anti-aliased.
static void triangle(struct ugfx_surface *s, int x0, int cy, int w, int hh, int dir, uint32_t c) {
    int xs[3], ys[3] = { cy - hh, cy, cy + hh };
    xs[0] = xs[2] = dir > 0 ? x0 : x0 + w;
    xs[1] = dir > 0 ? x0 + w : x0;
    ugfx_fill_polygon(s, xs, ys, 3, c);
}

static void draw_op(struct ugfx_surface *s, const void *w) {
    const struct uui_transport *t = w;
    uint32_t ink = t->dark ? ugfx_rgb(240, 242, 248) : utheme_action(UTHEME_ACT_NAV);
    uint32_t off = t->dark ? ugfx_rgb(120, 124, 136) : uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    uint32_t disc_c = t->dark ? ugfx_rgb(246, 247, 250) : UTHEME_ACCENT;
    uint32_t on_disc = t->dark ? ugfx_rgb(30, 32, 44) : UTHEME_ACCENT_TEXT;
    uint32_t wash_c = t->dark ? ugfx_rgb(255, 255, 255) : ugfx_rgb(0, 0, 0);

    for (int p = UUI_TRANSPORT_PREV; p <= UUI_TRANSPORT_NEXT; p++) {
        int x, y, bw, bh;
        uui_transport_part(t, p, &x, &y, &bw, &bh);
        int cx = x + bw / 2, cy = y + bh / 2, r = bw / 2;
        int on = live(t, p);
        if (p == UUI_TRANSPORT_PLAY) {
            uint32_t c = on ? disc_c : off;
            if (on && t->armed == p) c = ugfx_blend(c, wash_c, 40);
            else if (on && t->hot == p) c = ugfx_blend(c, wash_c, 22);
            ugfx_fill_circle(s, cx, cy, r, c);
            int g = r / 2;
            if (t->playing) {
                // CENTRED IN THE DISC'S OWN BOX, which is 2r+1 wide: the
                // gap takes the parity that leaves equal margins.
                int box = 2 * r + 1, bar = g * 2 / 3 + 1, gap = g * 2 / 3;
                if ((box - 2 * bar - gap) & 1) gap++;
                int left = cx - r + (box - 2 * bar - gap) / 2;
                ugfx_fill_rect(s, left, cy - g, bar, 2 * g + 1, on_disc);
                ugfx_fill_rect(s, left + bar + gap, cy - g, bar, 2 * g + 1, on_disc);
            } else {
                // A triangle's mass sits a third of the way from its
                // base, so it is nudged right of centre to LOOK centred.
                triangle(s, cx - g * 2 / 3, cy, g * 3 / 2, g, +1, on_disc);
            }
            continue;
        }
        if (on && (t->hot == p || t->armed == p))
            wash(s, cx, cy, r, wash_c, t->armed == p ? 60 : 30);
        uint32_t c = on ? ink : off;
        // |<  or  >| : a bar and a triangle pointing away from it.
        int g = r / 2, dir = p == UUI_TRANSPORT_NEXT ? +1 : -1;
        int bar = g / 3 + 1;
        int bx = dir > 0 ? cx + g - bar : cx - g;
        ugfx_fill_rect(s, bx, cy - g, bar, 2 * g + 1, c);
        triangle(s, dir > 0 ? cx - g : cx - g + bar + 1, cy, g * 2 - bar - 1, g, dir, c);
    }
}

static void natural_op(const void *w, int *ow, int *oh) {
    (void)w;
    *ow = 2 * side() + disc() + 2 * gap();
    *oh = disc();
}

static void geometry_op(void *w, int x, int y, int ww, int hh) {
    struct uui_transport *t = w;
    t->x = x; t->y = y; t->w = ww; t->h = hh;
}

static void bounds_op(const void *w, int *x, int *y, int *ww, int *hh) {
    const struct uui_transport *t = w;
    *x = t->x; *y = t->y; *ww = t->w; *hh = t->h;
}

static int hit_op(const void *w, int cx, int cy) {
    return part_at(w, cx, cy) != UUI_TRANSPORT_NONE;
}

static int press_op(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_transport *t = w;
    int p = part_at(t, cx, cy);
    t->armed = live(t, p) ? p : UUI_TRANSPORT_NONE;
    return 1;
}

static int motion_op(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_transport *t = w;
    int p = part_at(t, cx, cy);
    if (p == t->hot) return 0;
    t->hot = p;
    return 1;
}

// Committed only over the part that was pressed: sliding off cancels.
static int release_op(void *w, int cx, int cy) {
    struct uui_transport *t = w;
    if (t->armed != UUI_TRANSPORT_NONE && part_at(t, cx, cy) == t->armed)
        t->committed = t->armed;
    t->armed = UUI_TRANSPORT_NONE;
    return 1;
}

static void describe_op(const void *w, const struct uui_describe *d) {
    const struct uui_transport *t = w;
    static const char *const names[] = { 0, "prev", "play", "next" };
    for (int p = UUI_TRANSPORT_PREV; p <= UUI_TRANSPORT_NEXT; p++) {
        int x, y, bw, bh;
        uui_transport_part(t, p, &x, &y, &bw, &bh);
        uui_describe_rect(d, names[p], x, y, bw, bh);
    }
    uui_describe_int(d, "playing", t->playing);
}

const struct uui_widget_ops uui_transport_ops = {
    .natural_size = natural_op,
    .set_geometry = geometry_op,
    .bounds       = bounds_op,
    .draw         = draw_op,
    .hit          = hit_op,
    .press        = press_op,
    .motion       = motion_op,
    .release      = release_op,
    .describe     = describe_op,
};
