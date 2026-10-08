// See ushot.h. The asking side of WIN_REQ_SCREENSHOT.
#include "lib/ushot.h"
#include "lib/uchan.h"
#include "lib/uchan_page.h"
#include "lib/uwmchan.h"
#include "lib/uimg.h"
#include "rt/sys.h"
#include <kerrno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USHOT_TIMEOUT_MS 2000

static int alloc_buffer(struct ushot *s) {
    struct query_display d;
    if (sys_query_record(QUERY_DISPLAY, 0, &d, sizeof d) < (int)sizeof d)
        return -ENODEV;
    s->screen_w = (int)d.width;
    s->screen_h = (int)d.height;
    if (s->screen_w <= 0 || s->screen_h <= 0) return -ENODEV;

    s->cap_px = s->screen_w * s->screen_h;
    s->map_bytes = (uint64_t)s->cap_px * 4;

    char name[WIN_SHOT_NAME_MAX];
    snprintf(name, sizeof name, WIN_SHOT_NAME_FMT, sys_getpid());
    // A stale object from a previous run of the same pid would be the
    // wrong SIZE and could not be resized (there is no ftruncate here),
    // so it is unlinked rather than reused.
    sys_shm_unlink(name);

    int fd = sys_shm_open(name, s->map_bytes, SHM_CREATE);
    if (fd < 0) return -ENOMEM;
    void *p = sys_mmap(0, s->map_bytes, SYS_PROT_READ | SYS_PROT_WRITE,
                       SYS_MAP_SHARED, fd, 0);
    sys_close(fd);
    if (p == (void *)-1) { sys_shm_unlink(name); return -ENOMEM; }

    s->map = p;
    s->px = p;
    return 0;
}

static int open_common(struct ushot *s, struct uchan_client *borrow) {
    memset(s, 0, sizeof *s);

    if (borrow) {
        s->chan = borrow;
        s->chan_owned = 0;
    } else {
        if (uchan_client_open(&s->own, WMCHAN_SERVICE) != 0) return -ENOENT;
        s->chan = &s->own;
        s->chan_owned = 1;
    }

    int rc = alloc_buffer(s);
    if (rc < 0) {
        if (s->chan_owned) uchan_client_close(&s->own);
        s->chan = NULL;
        return rc;
    }

    // The compositor cannot open the object until it is named as the
    // one process that may (SYS_SHM_GRANT is a capability, not a mode).
    char name[WIN_SHOT_NAME_MAX];
    snprintf(name, sizeof name, WIN_SHOT_NAME_FMT, sys_getpid());
    sys_shm_grant(name, s->chan->beacon->server_pid);
    return 0;
}

int ushot_open(struct ushot *s) { return open_common(s, NULL); }

int ushot_open_on(struct ushot *s, struct uchan_client *chan) {
    return open_common(s, chan);
}

int ushot_take(struct ushot *s, int mode, unsigned flags,
               int x, int y, int w, int h) {
    if (!s->chan || !s->map) return -EINVAL;

    struct wmchan_msg m;
    memset(&m, 0, sizeof m);
    m.type = WIN_REQ_SCREENSHOT;
    m.a = mode;
    m.b = (int32_t)flags;
    m.c = s->cap_px;
    m.shot.x = x;
    m.shot.y = y;
    m.shot.w = w;
    m.shot.h = h;

    struct wmchan_msg r;
    memset(&r, 0, sizeof r);
    if (uchan_call(s->chan, &m, sizeof m, &r, sizeof r, USHOT_TIMEOUT_MS) != 0)
        return -EAGAIN;   // no ETIMEDOUT in abi/errno.h
    if (r.a < 0) return r.a;

    s->x = r.shot.x;
    s->y = r.shot.y;
    s->w = r.shot.w;
    s->h = r.shot.h;
    memcpy(s->app, r.shot.app, sizeof s->app);
    s->app[sizeof s->app - 1] = '\0';
    return 0;
}

int ushot_damage(struct ushot *s, unsigned flags, struct win_damage *out) {
    if (!s->chan || !s->map) return -EINVAL;
    struct wmchan_msg m;
    memset(&m, 0, sizeof m);
    m.type = WIN_REQ_SCREENSHOT;
    m.a = WIN_SHOT_SCREEN;
    m.b = (int32_t)(flags | WIN_SHOT_DAMAGE);
    m.c = s->cap_px;
    struct wmchan_msg r;
    memset(&r, 0, sizeof r);
    if (uchan_call(s->chan, &m, sizeof m, &r, sizeof r, USHOT_TIMEOUT_MS) != 0) return -EAGAIN;
    if (r.a < 0) return r.a;
    *out = r.damage;
    if (out->n > WIN_DAMAGE_MAX) out->n = WIN_DAMAGE_MAX;
    s->x = s->y = 0;
    s->w = s->screen_w;
    s->h = s->screen_h;
    return 0;
}

int ushot_cursor(struct ushot *s, uint32_t *out, int cap, int *w, int *h, int *hot_x, int *hot_y) {
    // THE SHAPE LANDS AT THE START OF THE SHARED BUFFER, which under
    // ushot_damage() is the top rows of the screen mirror -- so those
    // pixels are kept aside and put back, or the next compare would see
    // a cursor where the desktop is.
    static uint32_t keep[64 * 64];
    int n = (int)(sizeof keep / sizeof keep[0]);
    if (n > s->cap_px) n = s->cap_px;
    memcpy(keep, s->px, (size_t)n * 4);
    int rc = ushot_take(s, WIN_SHOT_CURSOR, 0, 0, 0, 0, 0);
    if (rc == 0 && s->w * s->h <= cap && s->w * s->h <= n) {
        memcpy(out, s->px, (size_t)(s->w * s->h) * 4);
        *w = s->w;
        *h = s->h;
        // ushot_take() put the hotspot where a capture's position goes.
        *hot_x = s->x;
        *hot_y = s->y;
    } else if (rc == 0) {
        rc = -ENOSPC;
    }
    memcpy(s->px, keep, (size_t)n * 4);
    s->x = s->y = 0;
    s->w = s->screen_w;
    s->h = s->screen_h;
    return rc;
}

int ushot_events(struct ushot *s) {
    int bits = 0;
    struct win_event ev;
    while (s->chan && uchan_client_recv(s->chan, &ev, sizeof ev) == 1)
        if (ev.type == WIN_EV_CAST) bits |= ev.a;
    return bits;
}

int ushot_probe(struct ushot *s, int mode, unsigned flags, int x, int y, struct win_shot *out) {
    if (!s->chan) return -EINVAL;

    struct wmchan_msg m;
    memset(&m, 0, sizeof m);
    m.type = WIN_REQ_SCREENSHOT;
    m.a = mode;
    m.b = (int32_t)(flags | WIN_SHOT_PROBE);
    m.c = s->cap_px;
    m.shot.x = x;
    m.shot.y = y;

    struct wmchan_msg r;
    memset(&r, 0, sizeof r);
    if (uchan_call(s->chan, &m, sizeof m, &r, sizeof r, USHOT_TIMEOUT_MS) != 0)
        return -EAGAIN;
    if (r.a < 0) return r.a;
    *out = r.shot;
    return 0;
}

int ushot_crop(struct ushot *s, int x, int y, int w, int h) {
    if (!s->px || w <= 0 || h <= 0) return -EINVAL;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0) return -EINVAL;

    // Forwards, row by row, into the front of the same buffer. The
    // destination row always starts at or before the source row, so the
    // copy never overwrites a pixel it has yet to read.
    for (int j = 0; j < h; j++) {
        const uint32_t *src = s->px + (size_t)(y + j) * s->w + x;
        uint32_t *dst = s->px + (size_t)j * w;
        for (int i = 0; i < w; i++) dst[i] = src[i];
    }
    s->x += x;
    s->y += y;
    s->w = w;
    s->h = h;
    return 0;
}

int ushot_save(const struct ushot *s, const char *path, const char *format) {
    if (!s->px || s->w <= 0 || s->h <= 0) return -EINVAL;
    // BORROWED pixels: this struct uimg points into the shared mapping
    // and must never reach uimg_free().
    struct uimg im = { .w = s->w, .h = s->h, .px = s->px, .has_alpha = 0 };
    return uimg_save(path, &im, format);
}

void ushot_close(struct ushot *s) {
    if (s->map) {
        sys_munmap(s->map, (s->map_bytes + 4095) & ~4095ull);
        char name[WIN_SHOT_NAME_MAX];
        snprintf(name, sizeof name, WIN_SHOT_NAME_FMT, sys_getpid());
        sys_shm_unlink(name);
    }
    if (s->chan && s->chan_owned) uchan_client_close(&s->own);
    memset(s, 0, sizeof *s);
}

const char *ushot_strerror(int rc) {
    switch (rc) {
    case 0:          return "ok";
    case -ENOENT:    return "no desktop is running";
    case -EBUSY:     return "a fullscreen program has the display";
    case -ENOSPC:    return "the capture does not fit the buffer";
    case -EINVAL:    return "nothing to capture there";
    case -ENODEV:    return "the kernel reports no display";
    case -EAGAIN:    return "the desktop did not answer";
    case -ENOMEM:    return "out of memory";
    default:         return uimg_last_error();
    }
}
