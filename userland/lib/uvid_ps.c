// The MPEG program stream (ISO/IEC 11172-1, and 13818-1's PS) for uvid:
// a `.mpg` file -- packs of PES packets, each carrying a slice of one
// elementary stream and, now and then, the time its first unit is shown.
//
// THE SHAPE: 00 00 01 BA starts a pack (a clock reference), BB a system
// header, and C0-DF / E0-EF a packet of audio / video, each with a
// 16-bit length -- so a reader skips what it does not want without
// parsing it. MPEG-1's PES header is stuffing, an optional buffer size
// and a PTS; MPEG-2's has flags and a header length. Both are read.
//
// **TIMES ARE 90 kHz TICKS FROM AN ARBITRARY ORIGIN.** Open finds the
// first PTS of the chosen streams and every time handed out is in ms
// since that origin, so a file cut from the middle of a broadcast still
// starts at 0:00. The duration is the LAST PTS, found by reading the
// file's tail -- a program stream has no index.
//
// **A SEEK IS A BISECTION ON THE FILE OFFSET**, reading the PTS of the
// first packet after each probe. For video it aims TWO GOPs EARLIER than
// asked, because a decoder can only start at a GOP and a picture's time
// says nothing about where its GOP began; uvid.c's exact seek decodes
// forward the rest of the way. The GOP's length is MEASURED at open --
// the span of video scanned over the GOP headers in it: a fixed margin
// would be either too short for a long-GOP file or seconds of wasted
// decoding (and a keyframe seconds early) for a short one.
#include "lib/uvid_internal.h"
#include "lib/ubytes.h"
#include "rt/sys.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUF        (72 * 1024)   // > the largest PES packet, 6 + 65535 bytes
#define SCAN_LIMIT (1024 * 1024)  // how far open looks for the streams' headers
#define TAIL       (512 * 1024)   // how much of the end holds the last PTS
#define SEEK_MARGIN_MS 1500       // when the GOP length could not be measured

struct ps {
    int vid, aid;               // the stream ids read, 0 for none
    int64_t margin_ms;          // how far before a video target a seek aims
    int64_t origin;             // 90 kHz ticks: the first PTS of either stream
    // The buffered reader. `base` is the file offset of buf[0].
    uint8_t buf[BUF];
    uint64_t base;
    int len, pos;
    int eof;
};

static void at(struct uvid_src *c, uint64_t off) {
    struct ps *p = c->priv;
    p->base = off;
    p->len = p->pos = 0;
    p->eof = 0;
}

// At least `n` bytes from pos on, refilling as needed; 0 when the file
// ends first.
static int need(struct uvid_src *c, int n) {
    struct ps *p = c->priv;
    if (p->len - p->pos >= n) return 1;
    if (p->eof) return 0;
    memmove(p->buf, p->buf + p->pos, (size_t)(p->len - p->pos));
    p->base += (uint64_t)p->pos;
    p->len -= p->pos;
    p->pos = 0;
    if (sys_lseek(c->fd, (long long)(p->base + (uint64_t)p->len), SYS_SEEK_SET) < 0) { p->eof = 1; return 0; }
    while (p->len < n) {
        int64_t r = sys_read(c->fd, p->buf + p->len, (size_t)(BUF - p->len));
        if (r <= 0) { p->eof = 1; break; }
        p->len += (int)r;
    }
    return p->len - p->pos >= n;
}

static uint64_t tell(const struct uvid_src *c) {
    const struct ps *p = c->priv;
    return p->base + (uint64_t)p->pos;
}

static int64_t read_ts(const uint8_t *b) {
    return (int64_t)((b[0] >> 1) & 7) << 30 | (int64_t)b[1] << 22 | (int64_t)(b[2] >> 1) << 15 |
           (int64_t)b[3] << 7 | (int64_t)(b[4] >> 1);
}

// One unit at the read position: 1 and its id, with a PES packet's
// payload (offset into buf, length) and PTS; 0 at the end. Junk between
// units is skipped a byte at a time -- a cut or damaged file resyncs.
struct unit {
    int id;
    int payload, plen;          // into ps->buf, valid until the next call
    int64_t pts;                // -1 when the packet carries none
};

static int next_unit(struct uvid_src *c, struct unit *u) {
    struct ps *p = c->priv;
    for (;;) {
        if (!need(c, 6)) return 0;
        const uint8_t *b = p->buf + p->pos;
        if (b[0] != 0 || b[1] != 0 || b[2] != 1) { p->pos++; continue; }
        int id = b[3];
        if (id == 0xB9) { p->pos += 4; return 0; }          // program end
        if (id == 0xBA) {                                    // pack header
            if (!need(c, 14)) return 0;
            b = p->buf + p->pos;
            if ((b[4] & 0xC0) == 0x40) p->pos += 14 + (b[13] & 7);   // MPEG-2
            else p->pos += 12;                                       // MPEG-1
            continue;
        }
        if (id < 0xBB) { p->pos++; continue; }              // a video start code in a stray place
        int len = b[4] << 8 | b[5];
        if (!need(c, 6 + len)) return 0;
        b = p->buf + p->pos;
        u->id = id;
        u->pts = -1;
        int h = 6, end = 6 + len;
        if (id != 0xBE && id != 0xBF && id != 0xBB && id != 0xBC) {
            if (end > 8 && (b[6] & 0xC0) == 0x80) {          // MPEG-2 PES header
                int flags = b[7], hl = b[8];
                if ((flags & 0x80) && end >= 14) u->pts = read_ts(b + 9);
                h = 9 + hl;
            } else {                                          // MPEG-1
                while (h < end && b[h] == 0xFF) h++;
                if (h < end && (b[h] & 0xC0) == 0x40) h += 2;
                if (h < end && (b[h] & 0xF0) == 0x20) { if (h + 5 <= end) u->pts = read_ts(b + h); h += 5; }
                else if (h < end && (b[h] & 0xF0) == 0x30) { if (h + 10 <= end) u->pts = read_ts(b + h); h += 10; }
                else h += 1;                                 // 0000 1111: no times
            }
        }
        if (h > end) h = end;
        u->payload = p->pos + h;
        u->plen = end - h;
        p->pos += end;
        return 1;
    }
}

static int is_video(int id) { return id >= 0xE0 && id <= 0xEF; }
static int is_audio(int id) { return id >= 0xC0 && id <= 0xDF; }

static int ps_probe(const uint8_t *d, size_t n) {
    return n >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 1 && d[3] == 0xBA;
}

static const uint32_t FPS[9][2] = {
    { 0, 0 }, { 24000, 1001 }, { 24, 1 }, { 25, 1 }, { 30000, 1001 },
    { 30, 1 }, { 50, 1 }, { 60000, 1001 }, { 60, 1 },
};

// The sequence header, from a video payload that holds one.
static void video_header(struct uvid_src *c, const uint8_t *d, int n) {
    for (int i = 0; i + 11 < n; i++) {
        if (d[i] || d[i + 1] || d[i + 2] != 1 || d[i + 3] != 0xB3) continue;
        const uint8_t *s = d + i + 4;
        c->w = s[0] << 4 | s[1] >> 4;
        c->h = (s[1] & 15) << 8 | s[2];
        int rate = s[3] & 15;
        if (rate >= 1 && rate <= 8) { c->fps_num = FPS[rate][0]; c->fps_den = FPS[rate][1]; }
        c->vcodec = UVID_VC_MPEG1;
        // MPEG-2 is the same header followed by a sequence EXTENSION.
        for (int k = i + 4; k + 4 <= n; k++)
            if (!d[k] && !d[k + 1] && d[k + 2] == 1) {
                if (d[k + 3] == 0xB5) {
                    c->vcodec = UVID_VC_UNKNOWN;
                    strlcpy(c->vfourcc, "MPEG-2", sizeof c->vfourcc);
                }
                if (d[k + 3] != 0xB2) break;     // user data may come between
            }
        return;
    }
}

static void audio_header(struct uvid_src *c, const uint8_t *d, int n) {
    for (int i = 0; i + 4 <= n; i++) {
        if (d[i] != 0xFF || (d[i + 1] & 0xE0) != 0xE0) continue;
        int layer = 4 - ((d[i + 1] >> 1) & 3);
        int mpeg1 = (d[i + 1] & 0x18) == 0x18;
        int sr = (d[i + 2] >> 2) & 3, br = (d[i + 2] >> 4) & 15;
        if (layer == 4 || sr == 3 || br == 15) continue;
        static const uint32_t rates[3] = { 44100, 48000, 32000 };
        static const uint16_t l2[15] = { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 };
        c->a_rate = mpeg1 ? rates[sr] : rates[sr] / 2;
        c->a_channels = ((d[i + 3] >> 6) & 3) == 3 ? 1 : 2;
        c->a_bits = 16;
        if (mpeg1 && layer == 2) {
            c->acodec = UVID_AC_MP2;
            c->a_kbps = l2[br];
        } else {
            c->acodec = UVID_AC_UNKNOWN;
            snprintf(c->afourcc, sizeof c->afourcc, "%sL%d", mpeg1 ? "M1" : "M2", layer);
        }
        return;
    }
}

// The last PTS of stream `id` in the file's tail, -1 for none.
static int64_t last_pts(struct uvid_src *c, int id) {
    uint64_t from = c->size > TAIL ? c->size - TAIL : 0;
    int64_t last = -1;
    struct unit u;
    at(c, from);
    while (next_unit(c, &u))
        if (u.id == id && u.pts >= 0) last = u.pts;
    return last;
}

static int64_t to_ms(const struct ps *p, int64_t pts) {
    if (pts < 0) return -1;
    int64_t d = pts - p->origin;
    if (d < 0) d += (int64_t)1 << 33;          // the 33-bit clock wrapped
    return d / 90;
}

static int ps_open(struct uvid_src *c) {
    struct ps *p = calloc(1, sizeof *p);
    if (!p) return -ENOMEM;
    c->priv = p;
    at(c, 0);
    int64_t vfirst = -1, afirst = -1, vlast = -1;
    int gops = 0;
    struct unit u;
    while (tell(c) < SCAN_LIMIT && next_unit(c, &u)) {
        const uint8_t *d = p->buf + u.payload;
        if (is_video(u.id) && (!p->vid || u.id == p->vid)) {
            if (!p->vid) p->vid = u.id;
            if (!c->vcodec) video_header(c, d, u.plen);
            if (vfirst < 0) vfirst = u.pts;
            if (u.pts > vlast) vlast = u.pts;
            // GOP headers, counted: with the span of video they came in,
            // the GOP's length. (A packet's own PTS is NOT its GOP's time:
            // it belongs to the first picture starting in it, often a B
            // picture of the GOP before.)
            for (int i = 0; i + 3 < u.plen; i++)
                if (!d[i] && !d[i + 1] && d[i + 2] == 1 && d[i + 3] == 0xB8) gops++;
        } else if (is_audio(u.id) && (!p->aid || u.id == p->aid)) {
            if (!p->aid) p->aid = u.id;
            if (!c->acodec) audio_header(c, d, u.plen);
            if (afirst < 0) afirst = u.pts;
        }
        if (c->vcodec && c->acodec && vfirst >= 0 && afirst >= 0 && gops >= 4) break;
    }
    // TWO GOPs: the seek may land just past one GOP's start, and the
    // pictures it lands among may be dated only from the one after.
    p->margin_ms = SEEK_MARGIN_MS;
    if (gops >= 3 && vlast > vfirst) {
        int64_t gop = (vlast - vfirst) / 90 / (gops - 1);
        p->margin_ms = gop * 2 + 100;
        if (p->margin_ms > 10000) p->margin_ms = 10000;
    }
    if (!p->vid && !p->aid) { uvid_fail("MPEG file holds no audio or video"); return -EINVAL; }
    if (p->vid && !c->vcodec) { c->vcodec = UVID_VC_UNKNOWN; strlcpy(c->vfourcc, "video", sizeof c->vfourcc); }
    p->origin = vfirst >= 0 && (afirst < 0 || vfirst < afirst) ? vfirst : afirst;
    if (p->origin < 0) p->origin = 0;

    int64_t vl = p->vid ? to_ms(p, last_pts(c, p->vid)) : -1;
    int64_t al = p->aid ? to_ms(p, last_pts(c, p->aid)) : -1;
    if (vl >= 0 && c->fps_num) vl += (int64_t)1000 * c->fps_den / c->fps_num;
    if (al >= 0 && c->a_rate) al += (int64_t)1152 * 1000 / c->a_rate;
    int64_t ms = vl > al ? vl : al;
    c->ms = ms > 0 ? (uint32_t)ms : 0;
    if (c->fps_num) c->frames = (uint32_t)((uint64_t)c->ms * c->fps_num / c->fps_den / 1000);
    if (c->a_rate) c->a_frames = (uint64_t)c->ms * c->a_rate / 1000;
    at(c, 0);
    return 0;
}

static int ps_read(struct uvid_src *c, int kind, struct uvid_pkt *pk) {
    struct ps *p = c->priv;
    int want = kind == UVID_VIDEO ? p->vid : p->aid;
    if (!want) return 0;
    struct unit u;
    while (next_unit(c, &u)) {
        if (u.id != want) continue;
        if (uvid_pkt_reserve(pk, (size_t)u.plen) != 0) return -ENOMEM;
        memcpy(pk->data, p->buf + u.payload, (size_t)u.plen);
        pk->len = (size_t)u.plen;
        pk->pts_ms = to_ms(p, u.pts);
        pk->key = 0;
        return 1;
    }
    return 0;
}

// The first packet of `id` with a PTS at or after `off`: its time in ms
// and the offset of the unit it is in. -1 when there is none.
static int64_t probe_at(struct uvid_src *c, int id, uint64_t off, uint64_t *where) {
    struct ps *p = c->priv;
    struct unit u;
    at(c, off);
    uint64_t start = tell(c);
    while (tell(c) - start < SCAN_LIMIT) {
        uint64_t here = tell(c);
        if (!next_unit(c, &u)) return -1;
        if (u.id == id && u.pts >= 0) {
            // `here` may be inside junk before the unit; the unit starts
            // where the reader is now, less its own length.
            *where = here;
            return to_ms(p, u.pts);
        }
    }
    return -1;
}

static int64_t ps_seek(struct uvid_src *c, int kind, int64_t ms) {
    struct ps *p = c->priv;
    int id = kind == UVID_VIDEO ? p->vid : p->aid;
    if (!id) return -EINVAL;
    int64_t target = kind == UVID_VIDEO ? ms - p->margin_ms : ms;
    uint64_t lo = 0, hi = c->size, best = 0;
    int64_t best_t = 0;
    if (target > 0) {
        while (hi - lo > 4096) {
            uint64_t mid = lo + (hi - lo) / 2, where;
            int64_t t = probe_at(c, id, mid, &where);
            if (t >= 0 && t <= target) { lo = mid; best = where; best_t = t; }
            else hi = mid;
        }
    }
    if (!best) {
        uint64_t where;
        int64_t t = probe_at(c, id, 0, &where);
        best_t = t >= 0 ? t : 0;
        best = 0;
    }
    at(c, best);
    if (kind == UVID_AUDIO) c->a_next_frame = (uint64_t)best_t * c->a_rate / 1000;
    return best_t;
}

static void ps_close(struct uvid_src *c) {
    free(c->priv);
    c->priv = 0;
}

const struct uvid_container uvid_container_ps = {
    .name = "mpeg-ps",
    .probe = ps_probe,
    .open = ps_open,
    .read = ps_read,
    .seek = ps_seek,
    .close = ps_close,
};
