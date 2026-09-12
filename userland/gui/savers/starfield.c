// FLYING THROUGH A STARFIELD, the oldest screensaver there is.
//
// An ordinary fullscreen client (userland/wm/wm_idle.h): the compositor
// spawns it when the machine goes quiet and kills it on the first key.
// Nothing here knows it is a screensaver, which is the point -- the
// same program run by hand is a program that draws stars.
//
// INTEGER MATH ONLY. A star is a point in a box that flies toward the
// viewer, and the projection is the textbook x/z -- a DIVIDE per star
// per frame, which is what the `stars` option's ceiling is about.
// rand() is the source, because a starfield wants a
// plausible spread and nothing else; the kernel's krandom is not linked
// into ring-3 programs and would be the wrong ask if it were.
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include "lib/usaver.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

// THE CEILING THE OPTION IS BOUNDED BY, not a count: `stars` comes from
// /usr/wm/savers/starfield.saver and the array has to hold its maximum.
// The two must agree -- a descriptor raised past this would be clamped
// here with nothing saying so.
#define STARS_MAX 2000
#define DEPTH 1024           // the z a star is born at, and dies past

static struct { int x, y, z; } g_star[STARS_MAX];
static int g_seeded;
static int g_stars = 420, g_speed = 14;
// The tint, as a per-channel weight over the depth brightness. White is
// the identity, so the unconfigured saver draws exactly what it drew
// before this file learned about options.
static int g_tint_r = 255, g_tint_g = 255, g_tint_b = 255;
// The surface the field was spread over. A saver opens at its
// descriptor's size and is resized to the screen a frame later, so a
// field seeded once fills a 640x480 box in the middle of a 1280x720
// screen.
static int g_for_w, g_for_h;

// A star is born in a box HALF the screen's size, not the whole of it.
//
// The projection divides by depth, so a star at its birth depth is
// drawn at half its model offset and spreads outward as it approaches.
// Born across the full width, most of them therefore start already off
// screen and are never seen: the first version drew about eighty pixels
// of a four-hundred-star field. Born across half, the field is dense
// in the middle and streams outward, which is what it should look like.
static void respawn(int i, int w, int h, int z) {
    g_star[i].x = (int)((unsigned)rand() % (unsigned)w) - w / 2;
    g_star[i].y = (int)((unsigned)rand() % (unsigned)h) - h / 2;
    g_star[i].z = z;
}

static void seed(int w, int h) {
    if (!g_seeded) srand((unsigned)time(0));
    g_for_w = w; g_for_h = h;
    for (int i = 0; i < g_stars; i++)
        respawn(i, w, h, 1 + (int)((unsigned)rand() % DEPTH));
    g_seeded = 1;
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    struct ugfx_surface *s = d->surface;
    int w = s->w, h = s->h, cx = w / 2, cy = h / 2;
    if (!g_seeded || w != g_for_w || h != g_for_h) seed(w, h);
    ugfx_fill_rect(s, 0, 0, w, h, 0x000000);

    for (int i = 0; i < g_stars; i++) {
        int z = g_star[i].z;
        if (z <= 0) continue;
        int px = cx + (g_star[i].x * (DEPTH / 2)) / z;
        int py = cy + (g_star[i].y * (DEPTH / 2)) / z;
        if (px < 0 || py < 0 || px >= w || py >= h) continue;
        // NEARER IS BRIGHTER, which is the whole illusion of depth --
        // a field of identical dots reads as noise however it moves.
        int lum = 255 - (z * 255) / DEPTH;
        if (lum < 24) lum = 24;
        uint32_t c = (uint32_t)(lum * g_tint_r / 255) << 16 |
                     (uint32_t)(lum * g_tint_g / 255) << 8 |
                     (uint32_t)(lum * g_tint_b / 255);
        ugfx_put_pixel(s, px, py, c);
        // A NEAR star is a 2x2 block, so the field gains weight as it
        // arrives instead of staying one pixel until it vanishes.
        if (z < DEPTH / 3 && px + 1 < w && py + 1 < h) {
            ugfx_put_pixel(s, px + 1, py, c);
            ugfx_put_pixel(s, px, py + 1, c);
            ugfx_put_pixel(s, px + 1, py + 1, c);
        }
    }
}

static int on_tick(struct uapp *a) {
    (void)a;
    for (int i = 0; i < g_stars; i++) {
        g_star[i].z -= g_speed;
        if (g_star[i].z <= 1) g_star[i].z = DEPTH;
    }
    return 1;   // repaint
}

static void on_open(struct uapp *a) { uapp_set_fullscreen(a, 1); }

// READ ONCE, BEFORE THE FIRST FRAME. The options cannot change under a
// running saver: the compositor kills this process on the first
// keypress, so the next run is what picks a new value up.
static void load_options(void) {
    static struct usaver cfg;   // ~1.5 KB, past the 2 KB ring-3 frame cap
    usaver_load("starfield", &cfg);
    g_stars = usaver_int(&cfg, "stars", g_stars);
    if (g_stars > STARS_MAX) g_stars = STARS_MAX;
    g_speed = usaver_int(&cfg, "speed", g_speed);
    const char *tint = usaver_str(&cfg, "colour", "white");
    if (!strcmp(tint, "amber")) { g_tint_r = 255; g_tint_g = 191; g_tint_b = 64; }
    else if (!strcmp(tint, "ice")) { g_tint_r = 160; g_tint_g = 208; g_tint_b = 255; }
    else if (!strcmp(tint, "green")) { g_tint_r = 96; g_tint_g = 255; g_tint_b = 128; }
}

int main(void) {
    load_options();
    struct uapp_desc desc = {
        .title = "Starfield",
        // RESIZABLE, because the compositor REFUSES fullscreen to a
        // window that is not (wm_input.c's wm_set_fullscreen) -- and a
        // saver that quietly stayed a 640x480 box in the corner is what
        // that refusal looks like from out here.
        .flags = UAPP_RESIZABLE,
        // A SIZE NOBODY SEES. uapp_open() refuses a window with no
        // dimensions, and on_open asks for fullscreen on the very first
        // frame -- so this is what the buffer is created at and what the
        // compositor immediately replaces. It has to be non-zero and it
        // does not have to be anything else.
        .w = 640, .h = 480,
        .app_id = "saver-starfield",
        .tick_ms = 33,          // ~30 fps, which is plenty for points
        .on_open = on_open,
        .on_tick = on_tick,
        .on_draw = on_draw,
    };
    return uapp_run(&desc);
}
