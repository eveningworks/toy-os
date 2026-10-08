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
#include "wm_shadow.h"
#include "rt/sys.h"
#include "string.h"   // k_strlcpy, k_memset, k_memcpy
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

// --- screen sharing: each caster's damage (WIN_SHOT_DAMAGE) ------------
//
// A remote desktop session is a CASTER from its first damage capture on.
// Its damage is kept apart from the frame loop's, which is consumed
// every frame, because a caster captures at its own pace -- a slow link
// may skip many frames, and what changed in all of them is owed.
#define CASTERS 4

static struct caster {
    int pid;
    int pointer;                 // it asked for the pointer in the copy
    int full;                    // owes the whole screen (its first capture)
    // WIN_CAST_* bits sent and not yet answered by a capture -- PER BIT.
    // One flag for both was cleared only by a DAMAGE capture, so a
    // pointer-shape nudge answered by a cursor capture left it set, and
    // every later change went unannounced: a VNC picture that froze the
    // moment the pointer changed shape (dragging a window on the ASUS).
    int notified;
    int cursor_dirty;
    struct win_damage d;
} g_cast[CASTERS];

static struct caster *caster_of(int pid) {
    for (int i = 0; i < CASTERS; i++) if (g_cast[i].pid == pid) return &g_cast[i];
    return 0;
}

static void notify(struct caster *c, int bits) {
    if (!(bits & ~c->notified)) return;   // each kind is announced once
    struct win_event ev;
    k_memset(&ev, 0, sizeof ev);
    ev.type = WIN_EV_CAST;
    ev.a = bits;
    // NO RING MEANS NO CLIENT: a session that died is forgotten here
    // rather than through a hook of its own.
    if (!wm_client_push_event(c->pid, &ev)) { k_memset(c, 0, sizeof *c); return; }
    c->notified |= bits;
}

static long area(int w, int h) { return (long)w * h; }

// Adds a rect to a caster's list, merging so the list never exceeds
// WIN_DAMAGE_MAX: into a rect it touches, else -- when full -- into the
// one whose union grows least. Over-covering is safe (the client
// compares); under-covering would lose a change.
static void add_rect(struct win_damage *d, int x, int y, int w, int h) {
    int best = -1;
    long best_grow = 0;
    for (int i = 0; i < d->n; i++) {
        int x0 = d->r[i].x < x ? d->r[i].x : x;
        int y0 = d->r[i].y < y ? d->r[i].y : y;
        int x1 = d->r[i].x + d->r[i].w > x + w ? d->r[i].x + d->r[i].w : x + w;
        int y1 = d->r[i].y + d->r[i].h > y + h ? d->r[i].y + d->r[i].h : y + h;
        long grow = area(x1 - x0, y1 - y0) - area(d->r[i].w, d->r[i].h) - area(w, h);
        if (best < 0 || grow < best_grow) { best = i; best_grow = grow; }
    }
    if (best >= 0 && (best_grow <= 0 || d->n == WIN_DAMAGE_MAX)) {
        int x0 = d->r[best].x < x ? d->r[best].x : x;
        int y0 = d->r[best].y < y ? d->r[best].y : y;
        int x1 = d->r[best].x + d->r[best].w > x + w ? d->r[best].x + d->r[best].w : x + w;
        int y1 = d->r[best].y + d->r[best].h > y + h ? d->r[best].y + d->r[best].h : y + h;
        d->r[best].x = (uint16_t)x0; d->r[best].y = (uint16_t)y0;
        d->r[best].w = (uint16_t)(x1 - x0); d->r[best].h = (uint16_t)(y1 - y0);
        return;
    }
    d->r[d->n].x = (uint16_t)x; d->r[d->n].y = (uint16_t)y;
    d->r[d->n].w = (uint16_t)w; d->r[d->n].h = (uint16_t)h;
    d->n++;
}

static void cast_rect(int pointer_only, int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > screen_w) w = screen_w - x;
    if (y + h > screen_h) h = screen_h - y;
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < CASTERS; i++) {
        struct caster *c = &g_cast[i];
        if (!c->pid || (pointer_only && !c->pointer)) continue;
        if (!c->full) add_rect(&c->d, x, y, w, h);
        notify(c, WIN_CAST_DAMAGE);
    }
}

// A DEAD CASTER IS FORGOTTEN WHEN ITS CLIENT GOES, not when a nudge to
// it next fails: a caster that died with a nudge already out is never
// nudged again, so the four slots filled with the dead and every later
// viewer was refused (-EBUSY) -- the pointer moved, clicks landed, and
// the picture never came. Found on the ASUS after a day of connections.
void wm_screenshot_client_gone(int pid) {
    struct caster *c = caster_of(pid);
    if (c && pid) k_memset(c, 0, sizeof *c);
}

void wm_screenshot_frame_damage(int x, int y, int w, int h) { cast_rect(0, x, y, w, h); }

int wm_screenshot_casting_pointer(void) {
    for (int i = 0; i < CASTERS; i++) if (g_cast[i].pid && g_cast[i].pointer) return 1;
    return 0;
}
void wm_screenshot_pointer_damage(int x, int y, int w, int h) { cast_rect(1, x, y, w, h); }

void wm_screenshot_cursor_changed(void) {
    for (int i = 0; i < CASTERS; i++) {
        if (!g_cast[i].pid) continue;
        g_cast[i].cursor_dirty = 1;
        notify(&g_cast[i], WIN_CAST_CURSOR);
    }
}

static void *map_shot(int from, uint64_t bytes) {
    char name[WIN_SHOT_NAME_MAX];
    snprintf(name, sizeof name, WIN_SHOT_NAME_FMT, from);
    int fd = sys_shm_open(name, 0, 0);
    if (fd < 0) return 0;
    void *p = sys_mmap(0, bytes, SYS_PROT_READ | SYS_PROT_WRITE, SYS_MAP_SHARED, fd, 0);
    sys_close(fd);
    return p == (void *)-1 ? 0 : p;
}

// WIN_SHOT_DAMAGE: copy what the caster is owed into its mirror.
static int capture_damage(int from, unsigned flags, int capacity_px, struct win_damage *out) {
    struct caster *c = caster_of(from);
    if (!c) {
        c = caster_of(0);
        if (!c) return -EBUSY;   // four screens shared already
        k_memset(c, 0, sizeof *c);
        c->pid = from;
        c->full = 1;
        c->cursor_dirty = 1;
    }
    c->pointer = (flags & WIN_SHOT_POINTER) != 0;
    if (capacity_px < screen_w * screen_h) return -ENOSPC;

    struct ugfx_surface *back = &g_wm_screen.back;
    // **NO RENDER HERE**, unlike a screenshot. A frame with no reported
    // damage is a FULL repaint (wm_render_frame()), so rendering per
    // capture repainted the whole screen at the viewer's rate. Nothing is
    // lost by not: a caster is told from inside the frame that drew the
    // change, so the back buffer already holds it.
    struct win_damage d = c->d;
    if (c->full) {
        d.n = 1;
        d.r[0].x = d.r[0].y = 0;
        d.r[0].w = (uint16_t)screen_w;
        d.r[0].h = (uint16_t)screen_h;
    }
    k_memset(&c->d, 0, sizeof c->d);
    c->full = 0;
    c->notified &= ~WIN_CAST_DAMAGE;
    d.flags = WIN_DAMAGE_LIST;
    *out = d;
    if (!d.n) return 0;

    uint64_t bytes = (uint64_t)screen_w * screen_h * 4;
    uint32_t *dst = map_shot(from, bytes);
    if (!dst) return -ENOENT;
    for (int k = 0; k < d.n; k++)
        for (int y = d.r[k].y; y < d.r[k].y + d.r[k].h; y++) {
            const uint32_t *src = back->pixels + (size_t)y * back->w + d.r[k].x;
            uint32_t *row = dst + (size_t)y * screen_w + d.r[k].x;
            for (int x = 0; x < d.r[k].w; x++) row[x] = src[x] | 0xFF000000u;
        }
    // The pointer, drawn or undone -- over the whole mirror, which is
    // right either way: erasing writes the true pixels under the sprite.
    struct ugfx_surface shot = ugfx_surface_for_pixels(dst, screen_w, screen_h);
    if (c->pointer) wm_render_cursor_into(&shot, 0, 0);
    else            wm_render_cursor_erase(&shot, 0, 0);
    unmap(dst, bytes);
    return 0;
}

// WIN_SHOT_CURSOR: the shape alone, for a client that draws the pointer.
static int capture_cursor(int from, int capacity_px, struct win_shot *rect) {
    static uint32_t img[64 * 64];
    int w, h, hx, hy;
    if (!wm_render_cursor_image(img, (int)(sizeof img / sizeof img[0]), &w, &h, &hx, &hy))
        return -EINVAL;
    if (w * h > capacity_px) return -ENOSPC;
    struct caster *c = caster_of(from);
    if (c) {
        c->cursor_dirty = 0;
        c->notified &= ~WIN_CAST_CURSOR;
    }
    uint64_t bytes = (uint64_t)w * h * 4;
    uint32_t *dst = map_shot(from, bytes);
    if (!dst) return -ENOENT;
    k_memcpy(dst, img, (size_t)bytes);
    unmap(dst, bytes);
    rect->x = hx; rect->y = hy; rect->w = w; rect->h = h;
    rect->app[0] = '\0';
    return 0;
}

int wm_screenshot_damage(int from, unsigned flags, int capacity_px, struct win_damage *out) {
    if (wm_scanout_active()) return -EBUSY;
    return capture_damage(from, flags, capacity_px, out);
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

    if (mode == WIN_SHOT_CURSOR) return capture_cursor(from, capacity_px, rect);
    rect->app[0] = '\0';
    switch (mode) {
    case WIN_SHOT_SCREEN:
        rect->x = rect->y = 0;
        rect->w = screen_w;
        rect->h = screen_h;
        break;
    case WIN_SHOT_WINDOW: {
        int i = topmost_other(from);
        if (i < 0) return -EINVAL;
        k_strlcpy(rect->app, windows[i].app_id, sizeof rect->app);
        rect->x = windows[i].x;
        rect->y = windows[i].y;
        rect->w = windows[i].w;
        rect->h = windows[i].h;
        break;
    }
    case WIN_SHOT_WINDOW_AT: {
        int i = topmost_at(from, rect->x, rect->y);
        if (i < 0) return -EINVAL;
        k_strlcpy(rect->app, windows[i].app_id, sizeof rect->app);
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
    if ((flags & WIN_SHOT_SHADOW) && (mode == WIN_SHOT_WINDOW || mode == WIN_SHOT_WINDOW_AT) &&
        wm_shadow_enabled()) {
        int m = wm_shadow_margin();
        rect->x -= m; rect->y -= m;
        rect->w += 2 * m; rect->h += 2 * m;
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
        // THE WHOLE SCREEN, when hiding: the caller's taskbar button and
        // windows are in the back buffer from earlier frames, and a frame
        // clipped to this iteration's damage left them in the picture.
        if (flags & WIN_SHOT_NO_SELF) {
            wm_damage_rect(0, 0, screen_w, screen_h);
            wm_render_hide_pid(from);
        }
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
