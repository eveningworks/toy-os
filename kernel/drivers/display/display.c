// The display-driver registry and probe. See display.h for the design.
#include "display.h"
#include "klog.h"
#include "kfmt.h"
#include "paging.h"
#include "multiboot.h" // multiboot_cmdline() -- the video= flag
#include "string.h"    // k_strstr, k_isdigit

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

// --- the mode a modesetting driver should aim for ---------------------

// What the multiboot2 header asks GRUB for (boot.asm). Kept in step by
// hand: the header is assembled, so it cannot share a constant with C.
#define DISPLAY_DEFAULT_W 1280
#define DISPLAY_DEFAULT_H 720

// gfx.c's back buffer is a fixed array, so no mode above it can ever be
// used however capable the adapter is. Stated here as well so the
// ladder never offers a candidate gfx_init() would refuse -- a driver
// that programmed one would come up with a live display the rasteriser
// declines to draw into, which is a black screen with no error.
#define DISPLAY_MAX_W 1920
#define DISPLAY_MAX_H 1080

// The standard sizes, largest first. Deliberately common VESA/panel
// geometries rather than a computed sequence: a mode a real BIOS or a
// virtual adapter actually offers is what makes a fallback useful.
static const struct { int w, h; } DISPLAY_LADDER[] = {
    { 1920, 1080 },
    { 1600, 900 },
    { 1366, 768 },
    { 1280, 1024 },
    { 1280, 720 },
    { 1024, 768 },
    { 800, 600 },
    { 640, 480 },
};
#define DISPLAY_LADDER_COUNT ((int)(sizeof DISPLAY_LADDER / sizeof DISPLAY_LADDER[0]))

void display_preferred_mode(int *out_w, int *out_h) {
    int w = DISPLAY_DEFAULT_W, h = DISPLAY_DEFAULT_H;

    // `video=<W>x<H>` on the GRUB line. Parsed rather than matched by
    // substring like the other boot flags, because it carries values --
    // and a value that does not parse is IGNORED, leaving the default,
    // rather than being guessed at (the toolkit's rule: a parser
    // rejects rather than guesses).
    const char *cmdline = multiboot_cmdline();
    const char *p = cmdline ? k_strstr(cmdline, "video=") : 0;
    if (p) {
        p += 6;
        uint32_t vw = 0, vh = 0;
        const char *x = p;
        while (k_isdigit(*x)) { vw = vw * 10 + (uint32_t)(*x - '0'); x++; }
        if (*x == 'x' || *x == 'X') {
            x++;
            while (k_isdigit(*x)) { vh = vh * 10 + (uint32_t)(*x - '0'); x++; }
        }
        if (vw >= 640 && vh >= 480 && vw <= DISPLAY_MAX_W && vh <= DISPLAY_MAX_H) {
            w = (int)vw;
            h = (int)vh;
        } else if (vw || vh) {
            klog_printf("display: ignoring video=%ux%u -- outside 640x480..%dx%d\n",
                         vw, vh, DISPLAY_MAX_W, DISPLAY_MAX_H);
        }
    }

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

int display_mode_candidate(int index, int *out_w, int *out_h) {
    int pw, ph;
    display_preferred_mode(&pw, &ph);
    if (index < 0) return 0;
    if (index == 0) {
        if (out_w) *out_w = pw;
        if (out_h) *out_h = ph;
        return 1;
    }

    // Walk the ladder, skipping anything at or above the preferred
    // size: index 0 already offered that, and a fallback that went
    // BIGGER would be ignoring what was asked for.
    int seen = 0;
    for (int i = 0; i < DISPLAY_LADDER_COUNT; i++) {
        if (DISPLAY_LADDER[i].w >= pw && DISPLAY_LADDER[i].h >= ph) continue;
        seen++;
        if (seen == index) {
            if (out_w) *out_w = DISPLAY_LADDER[i].w;
            if (out_h) *out_h = DISPLAY_LADDER[i].h;
            return 1;
        }
    }
    return 0;
}

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
