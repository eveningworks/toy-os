// A fullscreen, write-only client -- the proof for the fullscreen state
// and the display lease (docs/scanout-design.md, tools/fullscreen_test.py).
//
// It asks for fullscreen the moment its window opens, paints a solid
// colour no wallpaper or chrome uses with a white counter bar that
// grows a pixel per frame (so two frames are never the same picture),
// and toggles on F11. With UAPP_SCANOUT the compositor may lend it the
// display's buffers; the app cannot tell, which is the point.
//
// UAPP_POLL: it draws CONTINUOUSLY, as a game does, which is what
// reproduces the lease-end crash -- a client mid-frame into a buffer
// the kernel had just unmapped (DOOM, on the laptop, 2026-09-11).
#include <stdint.h>
#include "rt/sys.h"
#include "keyboard.h"
#include "ui/uapp.h"
#include "ui/ugfx.h"

#define FILL 0xFF3060C0u   // a blue nothing else on the desktop is

static unsigned g_frames;

static void on_open(struct uapp *a) {
    uapp_set_fullscreen(a, 1);
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    ugfx_fill(d->surface, FILL);
    // A SLOW frame on purpose -- a per-pixel pass over the lower half,
    // tens of milliseconds like a game's scaler -- so that when the
    // lease ends this client is almost certainly mid-frame into the
    // leased buffer, which is the crash the tool has to be able to see.
    int w = uapp_width(a), h = uapp_height(a);
    for (int y = h / 2; y < h; y++)
        for (int x = 0; x < w; x++)
            ugfx_put_pixel(d->surface, x, y, FILL + (uint32_t)((x + y + (int)g_frames) & 7));
    int bar = (int)(g_frames % 200u) + 8;
    if (bar > uapp_width(a) - 8) bar = uapp_width(a) - 8;
    ugfx_fill_rect(d->surface, 8, 8, bar, 6, 0xFFFFFFFFu);
    g_frames++;
}

static int on_tick(struct uapp *a) {
    (void)a;
    return 1;   // repaint: the counter bar moves a pixel
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    if (key == KEY_F11) uapp_set_fullscreen(a, !uapp_fullscreen(a));
}

int main(void) {
    struct uapp_desc desc = {
        .title   = "Fullscreen Client",
        .app_id  = "fsclient",
        .flags   = UAPP_RESIZABLE | UAPP_SCANOUT | UAPP_POLL,
        .w       = 320,
        .h       = 200,
        .on_open = on_open,
        .on_draw = on_draw,
        .on_tick = on_tick,
        .on_key  = on_key,
    };
    return uapp_run(&desc);
}
