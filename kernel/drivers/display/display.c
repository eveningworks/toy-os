// The display-driver registry and probe. See display.h for the design.
#include "display.h"
#include "klog.h"
#include "kfmt.h"
#include "paging.h"

// Small and fixed: there are two drivers today and a handful is the
// realistic ceiling. A linked list would need each driver to carry a
// mutable `next`, which would stop them being `const`.
#define MAX_DISPLAY_DRIVERS 8

static const struct display_driver *g_drivers[MAX_DISPLAY_DRIVERS];
static int g_count;
static const struct display_driver *g_active;

// Which mechanism (if any) made the framebuffer write-combining. Kept
// because the three outcomes differ by orders of magnitude in speed and
// nothing on screen distinguishes them -- `gfxbench` reports it beside
// the timings so a slow result can be read rather than guessed at.
static int g_wc = PAGING_WC_NONE;

void display_register(const struct display_driver *drv) {
    if (!drv || g_count >= MAX_DISPLAY_DRIVERS) return;
    g_drivers[g_count++] = drv;
}

// A driver that advertises a capability without providing the function
// is refused outright rather than half-used. The failure it prevents is
// specifically nasty: a card needing NEEDS_FLUSH but with no flush()
// renders perfectly into memory and shows a frozen screen, which reads
// as a rendering bug anywhere but here (it did -- see CHANGELOG.md).
static int caps_are_honest(const struct display_driver *d) {
    if (!d->get_surface || !d->probe || !d->name) return 0;
    if ((d->caps & DISPLAY_CAP_NEEDS_FLUSH) && !d->flush) return 0;
    if ((d->caps & DISPLAY_CAP_CURSOR) &&
        (!d->cursor_define || !d->cursor_move || !d->cursor_show)) return 0;
    if ((d->caps & DISPLAY_CAP_ACCEL_FILL) && !d->fill_rect) return 0;
    if ((d->caps & DISPLAY_CAP_ACCEL_COPY) && !d->copy_rect) return 0;
    if ((d->caps & DISPLAY_CAP_MODESET) &&
        (!d->mode_count || !d->mode_at || !d->set_mode)) return 0;
    return 1;
}

int display_probe(void) {
    for (int i = 0; i < g_count; i++) {
        const struct display_driver *d = g_drivers[i];
        if (!caps_are_honest(d)) {
            klog_printf("display: driver \"%s\" advertises capabilities it lacks -- skipped\n",
                         d && d->name ? d->name : "(unnamed)");
            continue;
        }
        if (!d->probe()) continue;

        g_active = d;
        struct display_surface s;
        d->get_surface(&s);

        // Ask for the framebuffer to be write-combining before anything
        // draws. On real hardware it is uncached MMIO otherwise, and the
        // cost is not subtle -- see paging_set_write_combining(). This is
        // the right place for it precisely because it is the first point
        // where the surface's address and extent are both known, and it
        // has to hold for whichever driver claimed, not just vesafb.
        g_wc = paging_set_write_combining(s.addr, (uint64_t)s.pitch * s.height);

        klog_printf("display: using \"%s\" -- %ux%u x%u pitch %u, caps:%s%s%s%s%s\n",
                     d->name, s.width, s.height, (unsigned)s.bpp, s.pitch,
                     (d->caps & DISPLAY_CAP_NEEDS_FLUSH) ? " flush" : "",
                     (d->caps & DISPLAY_CAP_CURSOR)      ? " cursor" : "",
                     (d->caps & DISPLAY_CAP_ACCEL_FILL)  ? " fill" : "",
                     (d->caps & DISPLAY_CAP_ACCEL_COPY)  ? " copy" : "",
                     (d->caps & DISPLAY_CAP_MODESET)     ? " modeset" : "");
        klog_printf("display: framebuffer write-combining: %s\n", paging_wc_name(g_wc));
        return 1;
    }
    klog_write("display: no driver claimed the hardware\n");
    return 0;
}

const struct display_driver *display_active(void) { return g_active; }

int display_write_combining(void) { return g_wc; }

int display_has(uint32_t cap) {
    return g_active && (g_active->caps & cap) != 0;
}

void display_get_surface(struct display_surface *out) {
    if (!out) return;
    if (!g_active) {
        out->addr = 0; out->pitch = 0; out->width = 0; out->height = 0; out->bpp = 0;
        return;
    }
    g_active->get_surface(out);
}

void display_flush(int x, int y, int w, int h) {
    if (!display_has(DISPLAY_CAP_NEEDS_FLUSH)) return; // scanned continuously
    if (w <= 0 || h <= 0) return;
    g_active->flush(x, y, w, h);
}

int display_cursor_define(const uint32_t *argb, int w, int h, int hot_x, int hot_y) {
    if (!display_has(DISPLAY_CAP_CURSOR)) return 0;
    return g_active->cursor_define(argb, w, h, hot_x, hot_y);
}

void display_cursor_move(int x, int y) {
    if (!display_has(DISPLAY_CAP_CURSOR)) return;
    g_active->cursor_move(x, y);
}

void display_cursor_show(int on) {
    if (!display_has(DISPLAY_CAP_CURSOR)) return;
    g_active->cursor_show(on);
}

int display_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!display_has(DISPLAY_CAP_ACCEL_FILL)) return 0;
    g_active->fill_rect(x, y, w, h, color);
    return 1;
}

int display_copy_rect(int sx, int sy, int dx, int dy, int w, int h) {
    if (!display_has(DISPLAY_CAP_ACCEL_COPY)) return 0;
    g_active->copy_rect(sx, sy, dx, dy, w, h);
    return 1;
}

int display_mode_count(void) {
    return display_has(DISPLAY_CAP_MODESET) ? g_active->mode_count() : 0;
}

void display_mode_at(int index, struct display_mode *out) {
    if (!out) return;
    if (!display_has(DISPLAY_CAP_MODESET)) {
        // Not a failure: a fixed-mode driver has exactly one, its own.
        struct display_surface s;
        display_get_surface(&s);
        out->width = s.width; out->height = s.height; out->bpp = s.bpp;
        return;
    }
    g_active->mode_at(index, out);
}

int display_set_mode(const struct display_mode *mode) {
    if (!display_has(DISPLAY_CAP_MODESET) || !mode) return 0;
    return g_active->set_mode(mode);
}
