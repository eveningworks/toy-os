// AN ANIMATED PICTURE AS THE DESKTOP BACKGROUND. The compositor runs this
// as its background client when the chosen live wallpaper is a GIF in
// LIVEWALL_ANIMATED_DIR (wm_background.c), its name as argv[1]. Scaled
// to cover the screen, as "Fill the screen" places a still.
//
// The decoder runs HERE, in a process that can die, rather than in the
// compositor: it is a parser over a file anyone can drop into
// /usr/share/wallpapers/animated, stepped for as long as the desktop is up.
//
// Scaled NEAREST-neighbour, every frame. An animated picture is small and
// usually pixel art, and a smooth rescale of the screen per frame is the
// cost this is avoiding.
#include "lib/uimg.h"
#include "lib/ulivewall.h"
#include "ui/uapp.h"

#include <stdio.h>
#include <string.h>

#define DESKTOP_BG 0x183c5a   // desktop.c's plain colour, under a transparent pixel

static struct uimg_anim g_anim;

static int on_tick(struct uapp *a) {
    int delay = 100;
    if (uimg_anim_next(&g_anim, &delay) < 0) return 0;
    // A GIF's delay of 0 or 1 centisecond means "as fast as you like",
    // which every browser reads as 100 ms; so does this.
    uapp_set_tick(a, delay < 20 ? 100 : (unsigned)delay);
    return 1;
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    struct ugfx_surface *s = d->surface;
    const struct uimg *f = &g_anim.frame;
    if (!f->px || f->w <= 0 || f->h <= 0) { ugfx_fill(s, DESKTOP_BG); return; }
    // The scale as a 16.16 ratio of screen to picture, the larger of the
    // two for cover and the smaller for fit.
    int64_t sx = ((int64_t)s->w << 16) / f->w, sy = ((int64_t)s->h << 16) / f->h;
    int64_t k = sx > sy ? sx : sy;
    int dw = (int)((f->w * k) >> 16), dh = (int)((f->h * k) >> 16);
    int ox = (s->w - dw) / 2, oy = (s->h - dh) / 2;
    for (int y = 0; y < s->h; y++) {
        int py = (int)(((int64_t)(y - oy) << 16) / k);
        if (py < 0 || py >= f->h) continue;
        uint32_t *row = s->pixels + (size_t)y * (size_t)s->w;
        const uint32_t *src = f->px + (size_t)py * (size_t)f->w;
        for (int x = 0; x < s->w; x++) {
            int px = (int)(((int64_t)(x - ox) << 16) / k);
            if (px < 0 || px >= f->w) continue;
            uint32_t c = src[px];
            row[x] = (c >> 24) ? (c & 0xffffff) : DESKTOP_BG;   // a transparent pixel shows the desktop's colour
        }
    }
}

int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : "";
    char path[128];
    snprintf(path, sizeof path, "%s/%s.gif", LIVEWALL_ANIMATED_DIR, name);
    // A NAME, as the compositor passes it: never a path out of the directory.
    if (!name[0] || strchr(name, '/') || uimg_anim_load(path, &g_anim) < 0) {
        fprintf(stderr, "gif: cannot play %s\n", path);
        return 1;
    }
    int delay = 100;
    uimg_anim_next(&g_anim, &delay);   // frame 0 onto the canvas
    struct uapp_desc desc = {
        .title = "Animated wallpaper",
        .app_id = "wallpaper",
        .flags = UAPP_RESIZABLE,
        .w = 320, .h = 180,
        .tick_ms = delay < 20 ? 100 : (unsigned)delay,
        .on_tick = on_tick,
        .on_draw = on_draw,
    };
    int rc = uapp_run(&desc);
    uimg_anim_close(&g_anim);
    return rc;
}
