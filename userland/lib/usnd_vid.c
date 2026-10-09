// A VIDEO FILE'S SOUND, as a row in usnd's codec table: `usnd_play()`
// on an .avi or .mpg plays its audio track, and the Video Player keeps
// its pictures in step with usnd_position() (lib/uvid.h says why the
// sound is the clock).
//
// The file is opened by uvid's container code a second time, for this
// reader alone: it walks the sound packets and skips the pictures,
// while the picture side does the opposite on its own descriptor.
//
// PCM (AVI) is the packets' bytes. MP2 (an MPEG program stream) is a
// byte stream the packets slice arbitrarily: frames are found in it by
// their headers and decoded by usnd_mp2.c's frame decoder.
#include "lib/usnd.h"
#include "lib/usnd_internal.h"
#include "lib/uvid_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MP2_SAMPLES 1152

struct vaud {
    struct uvid_src src;
    struct uvid_pkt pkt;
    size_t used;            // bytes of `pkt` already handed out (PCM)
    uint64_t skip;          // sample frames to drop after a seek lands early

    // MP2: the gathered stream, and one decoded frame.
    struct usnd_mp2 *mp2;
    uint8_t *es;
    size_t len, cap, pos;
    int32_t out[MP2_SAMPLES * 2];
    int out_n, out_pos;
};

static int vid_probe(const uint8_t *d, size_t n) { return uvid_probe(d, n); }

static int vid_open(struct usnd_stream *s) {
    struct vaud *a = calloc(1, sizeof *a);
    if (!a) { usnd_fail("out of memory"); return -ENOMEM; }
    s->priv = a;
    int rc = uvid_src_open(s->fd, &a->src);
    if (rc != 0) { usnd_fail(uvid_last_error()); free(a); s->priv = 0; return rc; }
    const struct uvid_src *c = &a->src;
    int ac = c->acodec;
    if (ac == UVID_AC_NONE || ac == UVID_AC_UNKNOWN) {
        usnd_fail(ac == UVID_AC_NONE ? "the video has no sound"
                                     : "this build cannot play the video's sound");
        uvid_src_close(&a->src);
        free(a);
        s->priv = 0;
        return ac == UVID_AC_NONE ? -EINVAL : -ENOTSUP;
    }
    if (ac == UVID_AC_MP2 && !(a->mp2 = usnd_mp2_new())) {
        uvid_src_close(&a->src);
        free(a);
        s->priv = 0;
        usnd_fail("out of memory");
        return -ENOMEM;
    }
    s->fmt.rate = c->a_rate;
    s->fmt.channels = c->a_channels;
    s->fmt.bits = c->a_bits;
    s->frames = c->a_frames;
    if (ac == UVID_AC_MP2)
        snprintf(s->detail, sizeof s->detail, "MP2 %u kbit/s in %s", c->a_kbps, c->ops->name);
    else
        snprintf(s->detail, sizeof s->detail, "PCM %u-bit in %s", c->a_bits, c->ops->name);
    return 0;
}

static long read_pcm(struct usnd_stream *s, struct vaud *a, int32_t *dst, long frames) {
    int ch = s->fmt.channels, bytes = s->fmt.bits / 8;
    size_t fb = (size_t)ch * (size_t)bytes;
    long got = 0;
    while (got < frames) {
        if (a->used + fb > a->pkt.len) {
            int rc = a->src.ops->read(&a->src, UVID_AUDIO, &a->pkt);
            if (rc < 0) return got ? got : rc;
            if (rc == 0) break;
            a->used = 0;
            continue;
        }
        const uint8_t *p = a->pkt.data + a->used;
        a->used += fb;
        if (a->skip) { a->skip--; continue; }
        for (int c = 0; c < ch; c++) {
            if (bytes == 2)
                dst[got * ch + c] = (int32_t)(int16_t)(p[2 * c] | p[2 * c + 1] << 8) * 65536;
            else
                dst[got * ch + c] = (int32_t)(p[c] - 128) * (1 << 24);
        }
        got++;
    }
    return got;
}

// The next MP2 frame out of the gathered stream, reading packets as
// needed: 1 decoded, 0 at the end, negative on an error.
static int next_mp2(struct usnd_stream *s, struct vaud *a) {
    for (;;) {
        // A header at pos whose frame is all here: decode it.
        while (a->pos + 4 <= a->len) {
            struct usnd_mpa_hdr h;
            const uint8_t *p = a->es + a->pos;
            if (!usnd_mpa_header(p, &h) || h.layer != 2 || h.rate != (int)s->fmt.rate ||
                h.channels != s->fmt.channels) {
                a->pos++;
                continue;
            }
            if (a->pos + (size_t)h.frame_bytes > a->len) break;
            int n = usnd_mp2_decode(a->mp2, p, h.frame_bytes, a->out);
            a->pos += (size_t)h.frame_bytes;
            if (n < 0) continue;
            a->out_n = n;
            a->out_pos = 0;
            return 1;
        }
        // Keep what is unparsed, then add a packet.
        memmove(a->es, a->es + a->pos, a->len - a->pos);
        a->len -= a->pos;
        a->pos = 0;
        int rc = a->src.ops->read(&a->src, UVID_AUDIO, &a->pkt);
        if (rc <= 0) return rc;
        if (a->len + a->pkt.len > a->cap) {
            size_t cap = a->cap ? a->cap * 2 : 16384;
            while (cap < a->len + a->pkt.len) cap *= 2;
            uint8_t *es = realloc(a->es, cap);
            if (!es) return -ENOMEM;
            a->es = es;
            a->cap = cap;
        }
        memcpy(a->es + a->len, a->pkt.data, a->pkt.len);
        a->len += a->pkt.len;
    }
}

static long read_mp2(struct usnd_stream *s, struct vaud *a, int32_t *dst, long frames) {
    int ch = s->fmt.channels;
    long got = 0;
    while (got < frames) {
        if (a->out_pos >= a->out_n) {
            int rc = next_mp2(s, a);
            if (rc <= 0) return got ? got : rc;
        }
        long take = a->out_n - a->out_pos;
        if (a->skip) {
            long drop = (long)(a->skip < (uint64_t)take ? a->skip : (uint64_t)take);
            a->out_pos += (int)drop;
            a->skip -= (uint64_t)drop;
            continue;
        }
        if (take > frames - got) take = frames - got;
        memcpy(dst + got * ch, a->out + a->out_pos * ch, (size_t)take * (size_t)ch * sizeof *dst);
        a->out_pos += (int)take;
        got += take;
    }
    return got;
}

static long vid_read(struct usnd_stream *s, int32_t *dst, long frames) {
    struct vaud *a = s->priv;
    return a->mp2 ? read_mp2(s, a, dst, frames) : read_pcm(s, a, dst, frames);
}

static int vid_seek(struct usnd_stream *s, uint64_t frame) {
    struct vaud *a = s->priv;
    int64_t ms = (int64_t)(frame * 1000 / s->fmt.rate);
    a->src.a_next_frame = (uint64_t)-1;
    int64_t at = a->src.ops->seek(&a->src, UVID_AUDIO, ms);
    if (at < 0) return (int)at;
    uint64_t landed = a->src.a_next_frame != (uint64_t)-1 ? a->src.a_next_frame
                                                          : (uint64_t)at * s->fmt.rate / 1000;
    a->skip = frame > landed ? frame - landed : 0;
    a->used = a->pkt.len;
    a->len = a->pos = 0;
    a->out_n = a->out_pos = 0;
    if (a->mp2) usnd_mp2_reset(a->mp2);
    return 0;
}

static void vid_close(struct usnd_stream *s) {
    struct vaud *a = s->priv;
    if (!a) return;
    uvid_src_close(&a->src);
    uvid_pkt_free(&a->pkt);
    usnd_mp2_free(a->mp2);
    free(a->es);
    free(a);
    s->priv = 0;
}

const struct usnd_codec usnd_codec_vid = {
    .name = "video",
    .probe = vid_probe,
    .open = vid_open,
    .read = vid_read,
    .seek = vid_seek,
    .close = vid_close,
};
