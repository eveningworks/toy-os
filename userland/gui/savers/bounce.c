// THE LOGO THAT BOUNCES, and the corner hit everybody waits for.
//
// Cheap on purpose: one icon blitted per frame over a black screen, no
// per-pixel work at all, so this is the saver that costs least on a
// machine where that matters. It exercises the icon cache and the
// SYMBOLIC recolouring a panel icon uses -- the logo takes the colour
// this saver gives it rather than its own, which is what lets the
// colour change on every bounce.
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "lib/icon_cache.h"
#include "lib/uimg.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define ICON_PX 64

static int g_x, g_y, g_dx = 3, g_dy = 2, g_seeded;
static int g_w, g_h;           // the surface, as of the last paint
static uint32_t g_tint = 0x66CCFF;
static int g_corners;          // how many times it has hit one exactly

// The palette a bounce cycles through. Named colours rather than a
// random RGB: a random one lands on something muddy about a third of
// the time, and this is a thing being LOOKED at.
static const uint32_t TINT[] = {
    0x66CCFF, 0xFF6B6B, 0xFFD166, 0x8AE68A, 0xC792EA, 0xFFFFFF,
};

static void seed(int w, int h) {
    srand((unsigned)time(0));
    g_x = rand() % (w > ICON_PX ? w - ICON_PX : 1);
    g_y = rand() % (h > ICON_PX ? h - ICON_PX : 1);
    g_dx = (rand() & 1) ? 3 : -3;
    g_dy = (rand() & 1) ? 2 : -2;
    g_seeded = 1;
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    struct ugfx_surface *s = d->surface;
    if (!g_seeded) seed(s->w, s->h);
    g_w = s->w; g_h = s->h;
    ugfx_fill_rect(s, 0, 0, s->w, s->h, 0x000000);
    const struct uimg *ico = icon_get("about", ICON_PX);
    // SYMBOLIC, so the logo takes the colour this saver gives it rather
    // than its own -- which is what lets the bounce change it, and the
    // same call the tray uses for its indicators.
    if (ico)
        ugfx_blit_tinted(s, g_x, g_y, ico->w, ico->h, ico->px, ico->w, g_tint);
    // The corner count, small and dim in a corner of its own. It is the
    // only reason to keep watching, so not saying it would be perverse.
    if (g_corners > 0) {
        char buf[32];
        snprintf(buf, sizeof buf, "corners: %d", g_corners);
        ugfx_draw_string(s, 8, s->h - ugfx_char_h() - 8, buf, 0x303030, 0x000000);
    }
}

static int on_tick(struct uapp *a) {
    (void)a;
    // The surface is not handed to on_tick, so the bounds come from
    // what the last on_draw saw. They are the window's, and the window
    // is the screen.
    if (!g_seeded) return 1;
    int maxx = g_w - ICON_PX, maxy = g_h - ICON_PX;
    if (maxx < 1) maxx = 1;
    if (maxy < 1) maxy = 1;

    g_x += g_dx;
    g_y += g_dy;
    int hit_x = 0, hit_y = 0;
    if (g_x <= 0)    { g_x = 0;    g_dx = -g_dx; hit_x = 1; }
    if (g_x >= maxx) { g_x = maxx; g_dx = -g_dx; hit_x = 1; }
    if (g_y <= 0)    { g_y = 0;    g_dy = -g_dy; hit_y = 1; }
    if (g_y >= maxy) { g_y = maxy; g_dy = -g_dy; hit_y = 1; }
    if (hit_x || hit_y) {
        static int n;
        g_tint = TINT[++n % (int)(sizeof TINT / sizeof TINT[0])];
    }
    // BOTH EDGES IN ONE STEP is the corner, and it is rare rather than
    // impossible: the step is 3 by 2, so it happens when the two
    // distances share a factor at the right moment.
    if (hit_x && hit_y) g_corners++;
    return 1;
}

static void on_open(struct uapp *a) { uapp_set_fullscreen(a, 1); }

int main(void) {
    struct uapp_desc desc = {
        .title = "Bounce",
        .app_id = "saver-bounce",
        .flags = UAPP_RESIZABLE,
        .w = 640, .h = 480,
        .tick_ms = 16,
        .on_open = on_open,
        .on_tick = on_tick,
        .on_draw = on_draw,
    };
    return uapp_run(&desc);
}
