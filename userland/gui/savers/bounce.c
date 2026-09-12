// THE LOGO THAT BOUNCES, and the corner hit everybody waits for.
//
// Cheap on purpose: one icon blitted per frame over a black screen, no
// per-pixel work at all, so this is the saver that costs least on a
// machine where that matters.
//
// **IT DRAWS `toyos`, IN ITS OWN COLOURS.** The first version blitted
// the `about` icon SYMBOLIC, which is the PANEL's mode: every opaque
// pixel becomes one flat colour, so the artwork is thrown away and what
// bounced was a coloured rounded square. Symbolic is right for a tray
// indicator taking the panel's ink and wrong for a LOGO, whose whole
// job is to be recognisable. `icon_toyos` is this OS's own mark -- the
// Start button's three stacked bricks, on a plate -- and it has colours
// of its own to show.
//
// The colour cycling went with it, because a logo that changes colour
// is not a logo. What survives is the bouncer's actual appeal, which
// was never the palette: it is waiting for the corner.
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "lib/icon_cache.h"
#include "lib/uimg.h"
#include "lib/usaver.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// 96 by default, not larger: the icons are drawn from a 64px master, so
// anything past about this is upscaled and the plate's rounded corners
// go soft. A logo that is slightly small reads better than a big blurry
// one -- which is why the `size` option can go further and the default
// does not.
static int g_icon_px = 96;

static int g_x, g_y, g_dx = 3, g_dy = 2, g_seeded;
static int g_w, g_h;           // the surface, as of the last paint
static int g_corners;          // how many times it has hit one exactly

static void seed(int w, int h) {
    srand((unsigned)time(0));
    g_x = rand() % (w > g_icon_px ? w - g_icon_px : 1);
    g_y = rand() % (h > g_icon_px ? h - g_icon_px : 1);
    // THE TWO AXES MUST DIFFER, which is the whole reason `speed` is not
    // simply copied into both: with equal steps the logo runs a diagonal
    // and hits a corner within seconds, every time, and the count below
    // stops meaning anything. Two thirds keeps them apart across the
    // declared range, which is why that range starts at 2.
    int sx = g_dx < 0 ? -g_dx : g_dx;
    if (sx < 2) sx = 2;
    int sy = sx * 2 / 3;
    if (sy < 1) sy = 1;
    g_dx = (rand() & 1) ? sx : -sx;
    g_dy = (rand() & 1) ? sy : -sy;
    g_seeded = 1;
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    struct ugfx_surface *s = d->surface;
    if (!g_seeded) seed(s->w, s->h);
    g_w = s->w; g_h = s->h;
    ugfx_fill_rect(s, 0, 0, s->w, s->h, 0x000000);
    const struct uimg *ico = icon_get("toyos", g_icon_px);
    if (ico) ugfx_blit(s, g_x, g_y, ico->w, ico->h, ico->px, ico->w);
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
    int maxx = g_w - g_icon_px, maxy = g_h - g_icon_px;
    if (maxx < 1) maxx = 1;
    if (maxy < 1) maxy = 1;

    g_x += g_dx;
    g_y += g_dy;
    int hit_x = 0, hit_y = 0;
    if (g_x <= 0)    { g_x = 0;    g_dx = -g_dx; hit_x = 1; }
    if (g_x >= maxx) { g_x = maxx; g_dx = -g_dx; hit_x = 1; }
    if (g_y <= 0)    { g_y = 0;    g_dy = -g_dy; hit_y = 1; }
    if (g_y >= maxy) { g_y = maxy; g_dy = -g_dy; hit_y = 1; }
    // BOTH EDGES IN ONE STEP is the corner, and it is rare rather than
    // impossible: the step is 3 by 2, so it happens when the two
    // distances share a factor at the right moment.
    if (hit_x && hit_y) g_corners++;
    return 1;
}

static void on_open(struct uapp *a) { uapp_set_fullscreen(a, 1); }

static void load_options(void) {
    static struct usaver cfg;   // past the 2 KB ring-3 frame cap
    usaver_load("bounce", &cfg);
    g_icon_px = usaver_int(&cfg, "size", g_icon_px);
    g_dx = usaver_int(&cfg, "speed", 3);   // seed() derives the other axis
}

int main(void) {
    load_options();
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
