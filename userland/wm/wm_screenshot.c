// Copying the composited frame out to a client -- WIN_REQ_SCREENSHOT.
//
// The whole capture is a memcpy out of the compositor's back buffer,
// because that buffer already holds the finished frame in ordinary
// cached memory (ui/ugfx.h's screen section). The framebuffer itself is
// write-combining and is never read; a capture that went to the glass
// would be a slow uncached round trip AND would race the scanout.
//
// THE POINTER IS THE ONLY SUBTLE PART, and it is subtle in both
// directions: with a software cursor the sprite is already in the back
// buffer, so "no pointer" means undoing it; on the hardware cursor plane
// it is not there at all, so "with pointer" means drawing it. Neither is
// the caller's to get right, which is why the flag is on the request.
#include "wm.h"
#include "wm_internal.h"
#include "wm_screenshot.h"
#include "ui/ugfx.h"
#include "wm_rawin.h"
#include "rt/sys.h"
#include <kerrno.h>
#include <stdio.h>

// The topmost toplevel that is not the caller's own. DELIBERATELY not
// "the focused window": the program asking for a screenshot is the one
// with focus, so a focus-based answer captures the screenshot tool.
// Spectacle and GNOME's shell both hide their own window and shoot what
// is under it; this is the same rule expressed once, in the compositor,
// where the z-order actually lives.
static int topmost_other(int from) {
    for (int i = window_count - 1; i >= 0; i--) {
        const struct window *w = &windows[i];
        if (w->popup || w->state == WIN_MINIMIZED) continue;
        if (wm_client_is_client_window(w) && w->client_pid == from) continue;
        return i;
    }
    return -1;
}

// The topmost window under a point, skipping the caller's own -- the
// same rule as topmost_other(), because a picker's full-screen overlay
// is over everything and would otherwise be the answer every time.
static int topmost_at(int from, int px, int py) {
    for (int i = window_count - 1; i >= 0; i--) {
        const struct window *w = &windows[i];
        if (w->popup || w->state == WIN_MINIMIZED) continue;
        if (wm_client_is_client_window(w) && w->client_pid == from) continue;
        if (px < w->x || py < w->y || px >= w->x + w->w || py >= w->y + w->h)
            continue;
        return i;
    }
    return -1;
}

static void clamp_rect(struct win_shot *r) {
    if (r->x < 0) { r->w += r->x; r->x = 0; }
    if (r->y < 0) { r->h += r->y; r->y = 0; }
    if (r->x + r->w > screen_w) r->w = screen_w - r->x;
    if (r->y + r->h > screen_h) r->h = screen_h - r->y;
    if (r->w < 0) r->w = 0;
    if (r->h < 0) r->h = 0;
}

static void unmap(void *p, uint64_t bytes) {
    // An unmap is the whole page-rounded region or nothing, and it fails
    // SILENTLY otherwise -- see uchan.c, which paid for that.
    if (p) sys_munmap(p, (bytes + 4095) & ~4095ull);
}

int wm_screenshot_capture(int from, int mode, unsigned flags,
                          int capacity_px, struct win_shot *rect) {
    // While a lease stands this compositor presents nothing, so its back
    // buffer holds a frame from before the lease began -- a picture of
    // the desktop under a fullscreen game. Refusing is the only honest
    // answer: there is no path from here to what is actually on screen.
    if (wm_scanout_active()) return -EBUSY;

    struct ugfx_surface *back = &g_wm_screen.back;
    if (!back->pixels || back->w <= 0 || back->h <= 0) return -EBUSY;

    switch (mode) {
    case WIN_SHOT_SCREEN:
        rect->x = rect->y = 0;
        rect->w = screen_w;
        rect->h = screen_h;
        break;
    case WIN_SHOT_WINDOW: {
        int i = topmost_other(from);
        if (i < 0) return -EINVAL;
        rect->x = windows[i].x;
        rect->y = windows[i].y;
        rect->w = windows[i].w;
        rect->h = windows[i].h;
        break;
    }
    case WIN_SHOT_WINDOW_AT: {
        int i = topmost_at(from, rect->x, rect->y);
        if (i < 0) return -EINVAL;
        rect->x = windows[i].x;
        rect->y = windows[i].y;
        rect->w = windows[i].w;
        rect->h = windows[i].h;
        break;
    }
    case WIN_SHOT_REGION:
        break;
    default:
        return -EINVAL;
    }
    clamp_rect(rect);
    if (rect->w <= 0 || rect->h <= 0) return -EINVAL;

    // A PROBE STOPS HERE, before the render and before the buffer. It
    // runs once per pointer move while somebody is choosing a window,
    // and rendering a frame for each of those would be a full repaint
    // per mouse motion.
    if (flags & WIN_SHOT_PROBE) return 0;

    if (capacity_px <= 0 || (long)rect->w * rect->h > (long)capacity_px)
        return -ENOSPC;

    // RENDER FIRST. A request arrives in the middle of the frame loop's
    // message pump, so anything that changed this iteration -- a
    // client's new frame, and in particular the requester having asked
    // to be left out -- is not in the back buffer yet. A fixed sleep in
    // the client would be guessing at a frame rate this loop does not
    // have.
    {
        int mx, my;
        uint8_t buttons;
        wm_rawin_mouse(&mx, &my, &buttons);
        if (flags & WIN_SHOT_NO_SELF) wm_render_hide_pid(from);
        wm_render_frame(mx, my);
        wm_render_hide_pid(0);
    }
    // The hidden window has to come BACK, and a bare redraw_pending is
    // not enough: it repaints everything only in a quiet frame, so
    // beside an animating client the render is clipped to that client's
    // rect and the tool's window stays gone (docs/conventions/gui.md).
    // Asked HERE, before any path below can return: the back buffer is
    // only read below, never redrawn, so asking early changes nothing
    // about the copy.
    if (flags & WIN_SHOT_NO_SELF) {
        wm_damage_rect(0, 0, screen_w, screen_h);
        redraw_pending = 1;
    }

    char name[WIN_SHOT_NAME_MAX];
    snprintf(name, sizeof name, WIN_SHOT_NAME_FMT, from);
    uint64_t bytes = (uint64_t)rect->w * rect->h * 4;

    int fd = sys_shm_open(name, 0, 0);
    if (fd < 0) return -ENOENT;
    void *p = sys_mmap(0, bytes, SYS_PROT_READ | SYS_PROT_WRITE,
                       SYS_MAP_SHARED, fd, 0);
    sys_close(fd);
    if (p == (void *)-1) return -ENOMEM;

    // OPAQUE ALPHA, not whatever the back buffer's top byte happens to
    // hold. A surface is 0x00RRGGBB here because the framebuffer has no
    // alpha to composite against, while `struct uimg` means 0xAARRGGBB
    // with 0xFF for opaque -- so a copy that carried the zero byte
    // through is not an image, it is a fully TRANSPARENT one. Nothing
    // says so: the encoders ignore the byte, and uimg_scale() weights
    // colour by alpha and resolves the lot to black. The preview in the
    // Screenshot app was a black rectangle until this line.
    uint32_t *dst = p;
    for (int y = 0; y < rect->h; y++) {
        const uint32_t *src = back->pixels + (size_t)(rect->y + y) * back->w
                              + rect->x;
        for (int x = 0; x < rect->w; x++)
            dst[(size_t)y * rect->w + x] = src[x] | 0xFF000000u;
    }

    // The copy is a surface so the cursor helpers can draw into it with
    // the ordinary clipped primitives -- a sprite half off the captured
    // rect must not run past the buffer.
    struct ugfx_surface shot = ugfx_surface_for_pixels(dst, rect->w, rect->h);
    if (flags & WIN_SHOT_POINTER) wm_render_cursor_into(&shot, rect->x, rect->y);
    else                          wm_render_cursor_erase(&shot, rect->x, rect->y);

    unmap(p, bytes);
    return 0;
}
