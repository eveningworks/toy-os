// THE SCREENSAVER THAT DRAWS NOTHING, which is the one a laptop wants.
//
// A saver is an ordinary fullscreen client (userland/wm/wm_idle.h), so
// the simplest useful one is a black rectangle that never changes. It
// is here rather than as a special case in the compositor for the same
// reason the others are: "no saver" and "the blank saver" are then one
// mechanism, and the timeout of zero is what means OFF.
//
// IT IS NOT THE BACKLIGHT. On a panel with a backlight this darkens the
// pixels and the lamp stays on, which is most of the power and all of
// the wear. Turning the lamp off needs a transient display-power
// control the kernel does not expose -- `system.brightness` PERSISTS,
// so using it would overwrite the user's setting and leave a dark
// screen behind a crash. See docs/roadmap.md.
#include "ui/uapp.h"
#include "ui/ugfx.h"

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    ugfx_fill_rect(d->surface, 0, 0, d->surface->w, d->surface->h, 0x000000);
}

static void on_open(struct uapp *a) { uapp_set_fullscreen(a, 1); }

int main(void) {
    struct uapp_desc desc = {
        .title = "Blank",
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
        .app_id = "saver-blank",
        .on_open = on_open,
        .on_draw = on_draw,
    };
    return uapp_run(&desc);
}
