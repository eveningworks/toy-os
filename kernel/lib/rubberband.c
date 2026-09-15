// Rubber-band selection -- see api/rubberband.h for the design and for
// why this is shared source compiled twice.
//
// Freestanding on purpose: <stdint.h> only. Nothing here allocates,
// draws, or knows what an item is.
#include "rubberband.h"

// --- bitset -----------------------------------------------------------
//
// Hand-rolled rather than reaching for kernel/lib/string.h's memset: this
// file is compiled into ring-3 binaries too, and staying dependency-free
// is what keeps it on the shared-source path at all.

static void bits_clear(uint32_t *w) {
    for (int i = 0; i < RB_WORDS; i++) w[i] = 0;
}

static void bits_copy(uint32_t *dst, const uint32_t *src) {
    for (int i = 0; i < RB_WORDS; i++) dst[i] = src[i];
}

static int bit_get(const uint32_t *w, int i) {
    if (i < 0 || i >= RB_MAX_ITEMS) return 0;
    return (w[i / 32] >> (i % 32)) & 1u;
}

static void bit_set(uint32_t *w, int i, int on) {
    if (i < 0 || i >= RB_MAX_ITEMS) return;
    uint32_t mask = 1u << (i % 32);
    if (on) w[i / 32] |= mask;
    else    w[i / 32] &= ~mask;
}

// --- geometry ---------------------------------------------------------

int rb_overlaps(int ax, int ay, int aw, int ah,
                 int bx, int by, int bw, int bh) {
    // Empty rectangles touch nothing. Stated rather than left to the
    // comparisons: a zero-width band is the normal state of a plain
    // click, and "a click selects everything" is the shape of bug this
    // prevents.
    if (aw <= 0 || ah <= 0 || bw <= 0 || bh <= 0) return 0;
    return ax < bx + bw && bx < ax + aw &&
           ay < by + bh && by < ay + ah;
}

int rb_rect(const struct rubberband *rb, int *x, int *y, int *w, int *h) {
    if (!rb || !rb->active) return 0;

    // Normalised, because dragging up-and-left is as ordinary as any
    // other direction and a negative width silently draws nothing.
    int x0 = rb->ax < rb->cx ? rb->ax : rb->cx;
    int y0 = rb->ay < rb->cy ? rb->ay : rb->cy;
    int x1 = rb->ax > rb->cx ? rb->ax : rb->cx;
    int y1 = rb->ay > rb->cy ? rb->ay : rb->cy;

    if (x) *x = x0;
    if (y) *y = y0;
    if (w) *w = x1 - x0;
    if (h) *h = y1 - y0;
    return 1;
}

// --- selection --------------------------------------------------------

void rb_clear(struct rubberband *rb) {
    if (!rb) return;
    rb->armed = rb->active = 0;
    rb->ax = rb->ay = rb->cx = rb->cy = 0;
    rb->mode = RB_REPLACE;
    bits_clear(rb->sel);
    bits_clear(rb->base);
}

int rb_is_selected(const struct rubberband *rb, int index) {
    return rb ? bit_get(rb->sel, index) : 0;
}

void rb_select(struct rubberband *rb, int index, int on) {
    if (rb) bit_set(rb->sel, index, on);
}

int rb_selected_count(const struct rubberband *rb) {
    if (!rb) return 0;
    int n = 0;
    for (int i = 0; i < RB_MAX_ITEMS; i++) n += bit_get(rb->sel, i);
    return n;
}

int rb_first_selected(const struct rubberband *rb) {
    if (!rb) return -1;
    for (int i = 0; i < RB_MAX_ITEMS; i++) if (bit_get(rb->sel, i)) return i;
    return -1;
}

// --- the drag ---------------------------------------------------------

void rb_begin(struct rubberband *rb, int x, int y, enum rb_mode mode) {
    if (!rb) return;
    rb->armed = 1;
    rb->active = 0;
    rb->ax = rb->cx = x;
    rb->ay = rb->cy = y;
    rb->mode = mode;
    // The selection as it stands is the BASE this drag composes with.
    // Kept for the whole drag, not just its first frame, so RB_ADD and
    // RB_TOGGLE stay right while the band shrinks as well as grows.
    bits_copy(rb->base, rb->sel);
}

static int past_threshold(const struct rubberband *rb, int x, int y) {
    int dx = x - rb->ax, dy = y - rb->ay;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    // Manhattan distance: no multiply, and the difference from a real
    // radius is imperceptible at four pixels.
    return dx + dy >= RB_DRAG_THRESHOLD;
}

void rb_motion(struct rubberband *rb, int x, int y,
                const struct rb_ops *ops, void *ctx) {
    // NO OPS IS LEGAL and means a band that selects nothing -- the
    // Screenshot app drags one over a frozen picture and wants only the
    // rectangle. Refusing it made the band never go active, so rb_rect()
    // answered 0 and the drag silently did nothing.
    if (!rb || !rb->armed) return;
    int have_items = ops && ops->count && ops->rect;

    if (!rb->active) {
        if (!past_threshold(rb, x, y)) return;
        rb->active = 1;
    }
    rb->cx = x;
    rb->cy = y;

    int bx, by, bw, bh;
    if (!rb_rect(rb, &bx, &by, &bw, &bh)) return;

    int n = have_items ? ops->count(ctx) : 0;
    if (n > RB_MAX_ITEMS) n = RB_MAX_ITEMS; // never index past the bitset
    if (n < 0) n = 0;

    // Recomputed from `base` on EVERY motion rather than accumulated.
    // An accumulating version looks identical while the band grows and
    // is wrong the moment it shrinks -- items stay selected after the
    // band has been pulled back off them, which no real one does.
    for (int i = 0; i < n; i++) {
        int ix, iy, iw, ih;
        ops->rect(ctx, i, &ix, &iy, &iw, &ih);
        int inside = rb_overlaps(bx, by, bw, bh, ix, iy, iw, ih);
        int was = bit_get(rb->base, i);

        int now;
        switch (rb->mode) {
        case RB_ADD:    now = was || inside; break;
        case RB_TOGGLE: now = inside ? !was : was; break;
        case RB_REPLACE:
        default:        now = inside; break;
        }
        bit_set(rb->sel, i, now);
    }
}

int rb_end(struct rubberband *rb) {
    if (!rb) return 0;
    int was_band = rb->active;

    // A press that never crossed the threshold is a CLICK. In REPLACE
    // mode that clears the selection -- "click empty space to deselect"
    // -- which is the same rule as the band case (an empty band selects
    // nothing) rather than a special case bolted on beside it. A caller
    // that clicked ON an item selects it afterwards; it has the click
    // coordinates and this module does not.
    if (!was_band && rb->armed && rb->mode == RB_REPLACE) bits_clear(rb->sel);

    rb->armed = 0;
    rb->active = 0;
    return was_band;
}
