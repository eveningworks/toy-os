// See screen.h. THE INVARIANT: this runs inside one syscall (or at
// boot, before the desktop), so nothing in ring 3 runs between the
// driver freeing the old framebuffer and win_surface_remode() mapping
// the new one -- and win_surface keeps every address the holder ever
// had mapped, so a blit the compositor was preempted in the middle of
// lands on a page either way.
#include "screen.h"
#include "display.h"
#include "gfx.h"
#include "vga.h"
#include "mouse.h"
#include "win_surface.h"
#include "win_server.h"
#include "klog.h"
#include "kfmt.h"

int screen_mode_listed(uint32_t w, uint32_t h) {
    int n = display_mode_count();
    for (int i = 0; i < n; i++) {
        struct display_mode m;
        display_mode_at(i, &m);
        if (m.width == w && m.height == h) return 1;
    }
    return 0;
}

int screen_set_mode(uint32_t w, uint32_t h) {
    if ((uint32_t)gfx_width() == w && (uint32_t)gfx_height() == h) return 1;
    if (!display_has(DISPLAY_CAP_MODESET) || !screen_mode_listed(w, h)) return 0;

    struct display_mode m = { .width = w, .height = h, .bpp = 32 };
    if (!display_set_mode(&m)) {
        klog_printf("screen: the driver refused %ux%u -- keeping %dx%d\n",
                    w, h, gfx_width(), gfx_height());
        return 0;
    }
    // Bottom-up from here: the surface moved, so everything that cached
    // it is re-read in the order it was first built.
    display_refresh_write_combining();
    if (!gfx_remode()) {
        klog_write("screen: gfx could not adopt the new surface\n");
        return 0;
    }
    vga_reflow();
    mouse_set_bounds(gfx_width(), gfx_height());
    if (!win_surface_remode())
        klog_write("screen: the compositor's grant could not be re-mapped\n");
    win_server_screen_changed(gfx_width(), gfx_height());
    klog_printf("screen: mode %dx%d, %d scanout(s)\n", gfx_width(), gfx_height(),
                display_scanout_count());
    return 1;
}
