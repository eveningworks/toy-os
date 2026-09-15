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
    return 0;
}

int ushot_probe(struct ushot *s, int mode, int x, int y, struct win_shot *out) {
    if (!s->chan) return -EINVAL;

    struct wmchan_msg m;
    memset(&m, 0, sizeof m);
    m.type = WIN_REQ_SCREENSHOT;
    m.a = mode;
    m.b = WIN_SHOT_PROBE;
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
