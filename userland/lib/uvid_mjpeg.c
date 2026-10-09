// Motion JPEG for uvid: every packet is a whole JPEG, decoded by
// lib/uimg.h's decoder -- a frame is a picture, so there is no state
// between frames, no reordering, and every frame is a keyframe.
//
// A packet of ZERO bytes is AVI's "drop frame": show the last picture
// again for this frame's time, which a capture card writes when it fell
// behind.
#include "lib/uvid_internal.h"
#include "lib/uimg.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct mjpeg {
    struct uimg img;
    struct uvid_frame f;
    int ready;              // a frame is waiting for frame()
};

static int mj_open(struct uvid *v) {
    struct mjpeg *m = calloc(1, sizeof *m);
    if (!m) return -ENOMEM;
    v->cpriv = m;
    return 0;
}

static int mj_feed(struct uvid *v, const struct uvid_pkt *p) {
    struct mjpeg *m = v->cpriv;
    if (p->len) {
        struct uimg next = {0};
        int rc = uimg_decode(p->data, p->len, &next);
        if (rc < 0) {
            uvid_fail(uimg_last_error());
            return rc;
        }
        uimg_free(&m->img);
        m->img = next;
    }
    if (!m->img.px) return 0;           // a drop frame before any picture
    m->f.w = m->img.w;
    m->f.h = m->img.h;
    m->f.fmt = UVID_ARGB;
    m->f.argb = m->img.px;
    m->f.argb_stride = m->img.w;
    m->f.pts_ms = p->pts_ms;
    m->f.type = 'I';
    m->ready = 1;
    return 0;
}

static int mj_frame(struct uvid *v, int eof, const struct uvid_frame **out) {
    (void)eof;
    struct mjpeg *m = v->cpriv;
    if (!m->ready) return 0;
    m->ready = 0;
    *out = &m->f;
    return 1;
}

static void mj_reset(struct uvid *v) {
    struct mjpeg *m = v->cpriv;
    m->ready = 0;
}

static void mj_close(struct uvid *v) {
    struct mjpeg *m = v->cpriv;
    if (!m) return;
    uimg_free(&m->img);
    free(m);
    v->cpriv = 0;
}

const struct uvid_codec uvid_codec_mjpeg = {
    .id = UVID_VC_MJPEG,
    .name = "mjpeg",
    .open = mj_open,
    .feed = mj_feed,
    .frame = mj_frame,
    .reset = mj_reset,
    .close = mj_close,
};
