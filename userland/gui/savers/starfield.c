// FLYING THROUGH A STARFIELD, the oldest screensaver there is.
//
// An ordinary fullscreen client (userland/wm/wm_idle.h): the compositor
// spawns it when the machine goes quiet and kills it on the first key.
// Nothing here knows it is a screensaver, which is the point -- the
// same program run by hand is a program that draws stars.
//
// INTEGER MATH ONLY. A star is a point in a box that flies toward the
// viewer, and the projection is the textbook x/z -- a DIVIDE per star
// per frame, which is why the count is in the hundreds rather than the
// thousands. rand() is the source, because a starfield wants a
// plausible spread and nothing else; the kernel's krandom is not linked
// into ring-3 programs and would be the wrong ask if it were.
#include "ui/uapp.h"
#include "ui/ugfx.h"
#include <stdlib.h>
#include <time.h>

#define STARS 220
#define DEPTH 1024           // the z a star is born at, and dies past
#define SPEED 14             // z units a frame -- about 1.3 s to cross

static struct { int x, y, z; } g_star[STARS];
static int g_seeded;

// A star's x and y are in a box the width of the screen, so the field
// fills the frame at every depth rather than being a cone in the
// middle. Reborn far away when it passes the viewer.
static void respawn(int i, int w, int h, int z) {
    g_star[i].x = (int)((unsigned)rand() % (unsigned)(w * 2)) - w;
    g_star[i].y = (int)((unsigned)rand() % (unsigned)(h * 2)) - h;
    g_star[i].z = z;
}

static void seed(int w, int h) {
    srand((unsigned)time(0));
    for (int i = 0; i < STARS; i++)
        respawn(i, w, h, 1 + (int)((unsigned)rand() % DEPTH));
    g_seeded = 1;
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    struct ugfx_surface *s = d->surface;
    int w = s->w, h = s->h, cx = w / 2, cy = h / 2;
    if (!g_seeded) seed(w, h);
    ugfx_fill_rect(s, 0, 0, w, h, 0x000000);

    for (int i = 0; i < STARS; i++) {
        int z = g_star[i].z;
        if (z <= 0) continue;
        int px = cx + (g_star[i].x * (DEPTH / 2)) / z;
        int py = cy + (g_star[i].y * (DEPTH / 2)) / z;
        if (px < 0 || py < 0 || px >= w || py >= h) continue;
        // NEARER IS BRIGHTER, which is the whole illusion of depth --
        // a field of identical dots reads as noise however it moves.
        int lum = 255 - (z * 255) / DEPTH;
        if (lum < 24) lum = 24;
        uint32_t c = ((uint32_t)lum << 16) | ((uint32_t)lum << 8) | (uint32_t)lum;
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
    for (int i = 0; i < STARS; i++) {
        g_star[i].z -= SPEED;
        if (g_star[i].z <= 1) g_star[i].z = DEPTH;
    }
    return 1;   // repaint
}

static void on_open(struct uapp *a) { uapp_set_fullscreen(a, 1); }

int main(void) {
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
