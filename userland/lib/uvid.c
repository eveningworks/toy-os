// uvid's core: the container and codec tables, and the loop that turns
// one stream's packets into frames in display order (lib/uvid.h).
#include "lib/uvid.h"
#include "lib/uvid_internal.h"
#include "lib/uimg.h"
#include "rt/sys.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// PROBE order: both magics are strict, four bytes at offset 0.
static const struct uvid_container *const g_containers[] = {
    &uvid_container_avi,
    &uvid_container_ps,
};
#define CONTAINER_COUNT ((int)(sizeof g_containers / sizeof g_containers[0]))

static const struct uvid_codec *const g_codecs[] = {
    &uvid_codec_mjpeg,
    &uvid_codec_mpeg1,
};
#define CODEC_COUNT ((int)(sizeof g_codecs / sizeof g_codecs[0]))

static const char *g_err = "";

const char *uvid_last_error(void) { return g_err; }
void uvid_fail(const char *msg) { g_err = msg; }

int uvid_pkt_reserve(struct uvid_pkt *p, size_t n) {
    if (n <= p->cap) return 0;
    size_t cap = p->cap ? p->cap : 4096;
    while (cap < n) cap *= 2;
    uint8_t *d = realloc(p->data, cap);
    if (!d) { uvid_fail("out of memory for a video packet"); return -ENOMEM; }
    p->data = d;
    p->cap = cap;
    return 0;
}

void uvid_pkt_free(struct uvid_pkt *p) {
    free(p->data);
    memset(p, 0, sizeof *p);
}

static const struct uvid_container *container_for(const uint8_t *d, size_t n) {
    for (int i = 0; i < CONTAINER_COUNT; i++)
        if (g_containers[i]->probe(d, n)) return g_containers[i];
    return 0;
}

int uvid_probe(const void *data, size_t n) {
    return container_for((const uint8_t *)data, n) != 0;
}

int uvid_src_open(int fd, struct uvid_src *c) {
    memset(c, 0, sizeof *c);
    c->fd = fd;
    uint8_t head[16];
    if (sys_lseek(fd, 0, SYS_SEEK_SET) != 0 || sys_read(fd, head, sizeof head) != (long)sizeof head) {
        uvid_fail("file is too short to be a video");
        return -EINVAL;
    }
    c->ops = container_for(head, sizeof head);
    if (!c->ops) { uvid_fail("not a video file this build knows"); return -EINVAL; }
    struct sys_stat st;
    if (sys_fstat(fd, &st) == 0) c->size = st.size;
    int rc = c->ops->open(c);
    if (rc != 0) {
        c->ops->close(c);
        c->ops = 0;
        return rc;
    }
    return 0;
}

void uvid_src_close(struct uvid_src *c) {
    if (c->ops) c->ops->close(c);
    c->ops = 0;
}

static void rate_name(char *out, size_t cap, uint32_t hz) {
    if (hz % 1000) snprintf(out, cap, "%u.%02u kHz", hz / 1000, hz % 1000 / 10);
    else snprintf(out, cap, "%u kHz", hz / 1000);
    // "22.05 kHz", "44.10 kHz" -> "44.1 kHz"
    size_t n = strlen(out);
    if (n > 5 && out[n - 5] == '0' && out[n - 6] != '.') memmove(out + n - 5, out + n - 4, 5);
}

static void describe(const struct uvid_src *c, struct uvid_info *out) {
    memset(out, 0, sizeof *out);
    out->w = c->w;
    out->h = c->h;
    out->fps_num = c->fps_num;
    out->fps_den = c->fps_den;
    out->ms = c->ms;
    out->frames = c->frames;
    out->container = c->ops->name;
    if (c->ms) out->kbps = (uint32_t)(c->size * 8 / c->ms);
    switch (c->vcodec) {
    case UVID_VC_MJPEG: out->codec = "mjpeg"; strlcpy(out->detail, "Motion JPEG", sizeof out->detail); break;
    case UVID_VC_MPEG1: out->codec = "mpeg1"; strlcpy(out->detail, "MPEG-1", sizeof out->detail); break;
    case UVID_VC_UNKNOWN:
        out->codec = "";
        snprintf(out->detail, sizeof out->detail, "%s (not supported)", c->vfourcc[0] ? c->vfourcc : "unknown");
        break;
    default: out->codec = ""; break;
    }
    if (c->acodec == UVID_AC_NONE) return;
    out->has_audio = c->acodec != UVID_AC_UNKNOWN;
    char hz[16];
    rate_name(hz, sizeof hz, c->a_rate);
    const char *ch = c->a_channels == 1 ? "mono" : c->a_channels == 2 ? "stereo" : "multichannel";
    if (c->acodec == UVID_AC_PCM)
        snprintf(out->audio, sizeof out->audio, "PCM %u-bit, %s %s", c->a_bits, hz, ch);
    else if (c->acodec == UVID_AC_MP2)
        snprintf(out->audio, sizeof out->audio, "MP2, %s %s", hz, ch);
    else
        snprintf(out->audio, sizeof out->audio, "%s (not supported)", c->afourcc);
}

int uvid_load_info(const char *path, struct uvid_info *out) {
    int fd = sys_open(path, 0);
    if (fd < 0) { uvid_fail("cannot open file"); return -ENOENT; }
    struct uvid_src c;
    int rc = uvid_src_open(fd, &c);
    if (rc == 0) {
        describe(&c, out);
        uvid_src_close(&c);
    }
    sys_close(fd);
    return rc;
}

int uvid_open(const char *path, struct uvid **out) {
    *out = 0;
    struct uvid *v = calloc(1, sizeof *v);
    if (!v) { uvid_fail("out of memory"); return -ENOMEM; }
    v->fd = sys_open(path, 0);
    if (v->fd < 0) { free(v); uvid_fail("cannot open file"); return -ENOENT; }
    int rc = uvid_src_open(v->fd, &v->src);
    if (rc != 0) { sys_close(v->fd); free(v); return rc; }
    describe(&v->src, &v->info);

    for (int i = 0; i < CODEC_COUNT; i++)
        if (g_codecs[i]->id == v->src.vcodec) v->codec = g_codecs[i];
    if (!v->codec) {
        uvid_fail(v->src.vcodec == UVID_VC_NONE ? "the file has no video"
                                                : "this build cannot decode the file's video");
        uvid_close(v);
        return v->src.vcodec == UVID_VC_NONE ? -EINVAL : -ENOTSUP;
    }
    rc = v->codec->open(v);
    if (rc != 0) { uvid_close(v); return rc; }
    *out = v;
    return 0;
}

const struct uvid_info *uvid_info(const struct uvid *v) { return &v->info; }

void uvid_close(struct uvid *v) {
    if (!v) return;
    if (v->codec && v->cpriv) v->codec->close(v);
    uvid_src_close(&v->src);
    uvid_pkt_free(&v->pkt);
    if (v->fd >= 0) sys_close(v->fd);
    free(v);
}

int uvid_next(struct uvid *v, const struct uvid_frame **out) {
    if (v->held) {
        *out = v->held;
        v->held = 0;
        return 1;
    }
    for (;;) {
        if (v->codec->frame(v, v->at_end, out)) return 1;
        if (v->at_end) return 0;
        int rc = v->src.ops->read(&v->src, UVID_VIDEO, &v->pkt);
        if (rc < 0) return rc;
        if (rc == 0) { v->at_end = 1; continue; }
        rc = v->codec->feed(v, &v->pkt);
        // A broken packet costs its frame, not the file: decoders skip
        // to the next one, as every player does with a damaged stream.
        if (rc == -ENOMEM) return rc;
    }
}

int uvid_seek(struct uvid *v, uint32_t ms, int mode) {
    int64_t at = v->src.ops->seek(&v->src, UVID_VIDEO, ms);
    if (at < 0) { uvid_fail("this file cannot seek"); return (int)at; }
    v->codec->reset(v);
    v->at_end = 0;
    v->held = 0;
    if (mode == UVID_SEEK_KEY) return 0;
    // EXACT: decode forward, throwing frames away, until the next one is
    // the frame showing at `ms`. Peeked by decoding, so the frame that
    // passes is kept and handed out first.
    for (;;) {
        const struct uvid_frame *f;
        // The codec's own queue first, so a frame already decoded is seen.
        if (!v->codec->frame(v, v->at_end, &f)) {
            if (v->at_end) return 0;
            int rc = v->src.ops->read(&v->src, UVID_VIDEO, &v->pkt);
            if (rc < 0) return rc;
            if (rc == 0) { v->at_end = 1; continue; }
            if (v->codec->feed(v, &v->pkt) == -ENOMEM) return -ENOMEM;
            continue;
        }
        // Keep it unless the NEXT frame would still be at or before ms;
        // with no frame duration known, keep the first at or past it.
        uint32_t dur = v->info.fps_num ? (uint32_t)(1000ull * v->info.fps_den / v->info.fps_num) : 0;
        if (f->pts_ms < 0 || f->pts_ms + (int64_t)dur > (int64_t)ms) {
            v->held = f;
            return 0;
        }
    }
}

int uvid_still(const char *path, uint32_t ms, int max_w, int max_h, struct uimg *out) {
    memset(out, 0, sizeof *out);
    struct uvid *v;
    int rc = uvid_open(path, &v);
    if (rc != 0) return rc;
    if (ms && (rc = uvid_seek(v, ms, UVID_SEEK_EXACT)) != 0) { uvid_close(v); return rc; }
    const struct uvid_frame *f;
    rc = uvid_next(v, &f);
    if (rc == 1) rc = uvid_frame_to_uimg(f, max_w, max_h, out);
    else if (rc == 0) { uvid_fail("the file has no frame there"); rc = -EINVAL; }
    uvid_close(v);
    return rc;
}
