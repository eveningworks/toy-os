// AVI: Microsoft's RIFF container, for uvid (lib/uvid.h). Read for one
// video stream (Motion JPEG) and one sound stream (PCM, MPEG audio).
//
// THE SHAPE: RIFF 'AVI ' holds LIST 'hdrl' (a main header, then one LIST
// 'strl' per stream: 'strh' says what and how fast, 'strf' the format),
// LIST 'movi' (the packets, as chunks named "00dc", "01wb" -- the stream
// number then the kind) and usually 'idx1', an index of every chunk.
//
// **OPEN BUILDS AN INDEX OF EVERY CHUNK, and nothing after it scans.**
// From 'idx1' when there is one; by walking 'movi' when there is not, or
// when the file grew past its first RIFF (OpenDML's 'AVIX' pieces, which
// 'idx1' never covers). A seek is then an array lookup, which is all the
// player needs from AVI -- every Motion JPEG frame is a keyframe.
//
// Chunk payloads are word-aligned: an odd size is followed by a pad byte
// the size does not count.
#include "lib/uvid_internal.h"
#include "lib/ubytes.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct avi_chunk {
    uint64_t off;           // of the payload
    uint32_t size;
};

struct avi_list {
    struct avi_chunk *c;
    uint32_t n, cap;
};

struct avi {
    int vs, as;             // stream numbers, -1 for none
    uint32_t vscale, vrate, vstart;
    uint32_t arate, ablock; // samples a second, bytes a sample frame
    struct avi_list v, a;
    uint64_t *a_bytes;      // sound bytes before a.c[i], a.n + 1 entries
    uint32_t vpos, apos;    // the next chunk each reader takes
};

static int list_add(struct avi_list *l, uint64_t off, uint32_t size) {
    if (l->n == l->cap) {
        uint32_t cap = l->cap ? l->cap * 2 : 256;
        struct avi_chunk *c = realloc(l->c, cap * sizeof *c);
        if (!c) return -ENOMEM;
        l->c = c;
        l->cap = cap;
    }
    l->c[l->n].off = off;
    l->c[l->n].size = size;
    l->n++;
    return 0;
}

static int avi_probe(const uint8_t *d, size_t n) {
    return n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "AVI ", 4);
}

// "00dc" -> stream 0; -1 for a name that does not start with two digits.
static int stream_of(const uint8_t *id) {
    if (id[0] < '0' || id[0] > '9' || id[1] < '0' || id[1] > '9') return -1;
    return (id[0] - '0') * 10 + (id[1] - '0');
}

static void fourcc_str(char *out, const uint8_t *f) {
    int n = 0;
    for (int i = 0; i < 4; i++)
        if (f[i] > ' ' && f[i] < 0x7f) out[n++] = (char)f[i];
    out[n] = '\0';
}

static int is_mjpeg(const uint8_t *f) {
    static const char *const names[] = { "MJPG", "mjpg", "AVRn", "dmb1", "jpeg", "JPEG" };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!memcmp(f, names[i], 4)) return 1;
    return 0;
}

// One LIST 'strl': stream number `num`.
static void parse_strl(struct uvid_src *c, struct avi *a, const uint8_t *p, uint32_t len, int num) {
    const uint8_t *strh = 0, *strf = 0;
    uint32_t strh_len = 0, strf_len = 0;
    for (uint32_t o = 0; o + 8 <= len;) {
        uint32_t sz = ub_le32(p + o + 4);
        if (sz > len - o - 8) break;
        if (!memcmp(p + o, "strh", 4)) { strh = p + o + 8; strh_len = sz; }
        if (!memcmp(p + o, "strf", 4)) { strf = p + o + 8; strf_len = sz; }
        o += 8 + sz + (sz & 1);
    }
    if (!strh || strh_len < 48 || !strf) return;

    if (!memcmp(strh, "vids", 4) && a->vs < 0 && strf_len >= 40) {
        a->vs = num;
        a->vscale = ub_le32(strh + 20);
        a->vrate = ub_le32(strh + 24);
        a->vstart = ub_le32(strh + 28);
        c->w = (int)ub_le32(strf + 4);
        int32_t h = (int32_t)ub_le32(strf + 8);
        c->h = h < 0 ? -h : h;
        const uint8_t *comp = strf + 16;
        if (is_mjpeg(comp) || is_mjpeg(strh + 4)) {
            c->vcodec = UVID_VC_MJPEG;
        } else {
            c->vcodec = UVID_VC_UNKNOWN;
            fourcc_str(c->vfourcc, comp[0] ? comp : strh + 4);
        }
        if (a->vscale && a->vrate) { c->fps_num = a->vrate; c->fps_den = a->vscale; }
    } else if (!memcmp(strh, "auds", 4) && a->as < 0 && strf_len >= 16) {
        a->as = num;
        uint16_t tag = ub_le16(strf);
        c->a_channels = ub_le16(strf + 2);
        c->a_rate = ub_le32(strf + 4);
        c->a_kbps = ub_le32(strf + 8) * 8 / 1000;
        a->ablock = ub_le16(strf + 12);
        c->a_bits = ub_le16(strf + 14);
        a->arate = c->a_rate;
        if (tag == 1 && (c->a_bits == 8 || c->a_bits == 16) && a->ablock) {
            c->acodec = UVID_AC_PCM;
        } else if (tag == 0x50) {
            c->acodec = UVID_AC_MP2;
        } else {
            c->acodec = UVID_AC_UNKNOWN;
            snprintf(c->afourcc, sizeof c->afourcc, "0x%04x", tag);
        }
    }
}

static int parse_hdrl(struct uvid_src *c, struct avi *a, uint64_t off, uint32_t len) {
    if (len > (1u << 20)) { uvid_fail("AVI header list is implausibly large"); return -EINVAL; }
    uint8_t *p = malloc(len ? len : 1);
    if (!p) return -ENOMEM;
    if (ub_read_at(c->fd, off, p, len) != 0) { free(p); uvid_fail("AVI header is truncated"); return -EINVAL; }
    int num = 0;
    for (uint32_t o = 0; o + 8 <= len;) {
        uint32_t sz = ub_le32(p + o + 4);
        if (sz > len - o - 8) break;
        if (!memcmp(p + o, "avih", 4) && sz >= 40) {
            if (!c->frames) c->frames = ub_le32(p + o + 8 + 16);
        } else if (!memcmp(p + o, "LIST", 4) && sz >= 4 && !memcmp(p + o + 8, "strl", 4)) {
            parse_strl(c, a, p + o + 12, sz - 4, num++);
        }
        o += 8 + sz + (sz & 1);
    }
    free(p);
    return 0;
}

static int add_chunk(struct avi *a, const uint8_t *id, uint64_t off, uint32_t size) {
    int s = stream_of(id);
    if (s < 0) return 0;
    if (s == a->vs && (id[2] == 'd' || id[2] == 'D')) return list_add(&a->v, off, size);
    if (s == a->as && id[2] == 'w' && id[3] == 'b') return list_add(&a->a, off, size);
    return 0;
}

// Walks the chunks from `off` to `end`, descending into LIST 'rec '.
static int walk_movi(struct uvid_src *c, struct avi *a, uint64_t off, uint64_t end) {
    uint8_t h[12];
    while (off + 8 <= end) {
        if (ub_read_at(c->fd, off, h, 12) != 0 && ub_read_at(c->fd, off, h, 8) != 0) break;
        uint32_t sz = ub_le32(h + 4);
        if (!memcmp(h, "LIST", 4)) {
            if (sz >= 4 && !memcmp(h + 8, "rec ", 4)) { off += 12; continue; }
        } else {
            if (off + 8 + sz > end) break;      // a truncated last chunk is dropped
            int rc = add_chunk(a, h, off + 8, sz);
            if (rc) return rc;
        }
        off += 8 + (uint64_t)sz + (sz & 1);
    }
    return 0;
}

// 'idx1': 16 bytes an entry -- id, flags, offset, size. The offset is
// from the 'movi' fourcc in most files and from the file's start in
// some; the first entry says which, by pointing at its own name.
static int read_idx1(struct uvid_src *c, struct avi *a, uint64_t off, uint32_t len, uint64_t movi) {
    uint32_t n = len / 16;
    if (!n) return -EINVAL;
    uint8_t *p = malloc((size_t)n * 16);
    if (!p) return -ENOMEM;
    if (ub_read_at(c->fd, off, p, (size_t)n * 16) != 0) { free(p); return -EINVAL; }
    uint64_t base = movi;
    uint8_t name[4];
    uint32_t first = ub_le32(p + 8);
    if (ub_read_at(c->fd, movi + first, name, 4) != 0 || memcmp(name, p, 4) != 0) base = 0;
    if (base == 0 && (ub_read_at(c->fd, first, name, 4) != 0 || memcmp(name, p, 4) != 0)) {
        free(p);
        return -EINVAL;     // neither reading holds: walk instead
    }
    int rc = 0;
    for (uint32_t i = 0; i < n && !rc; i++) {
        const uint8_t *e = p + i * 16;
        uint64_t at = base + ub_le32(e + 8);
        rc = add_chunk(a, e, at + 8, ub_le32(e + 12));
    }
    free(p);
    return rc;
}

static int avi_open(struct uvid_src *c) {
    struct avi *a = calloc(1, sizeof *a);
    if (!a) return -ENOMEM;
    a->vs = a->as = -1;
    c->priv = a;

    uint8_t h[12];
    if (ub_read_at(c->fd, 0, h, 12) != 0) { uvid_fail("AVI file is truncated"); return -EINVAL; }
    uint64_t riff_end = 8 + (uint64_t)ub_le32(h + 4);
    if (riff_end > c->size) riff_end = c->size;

    uint64_t movi = 0, movi_end = 0, idx1 = 0;
    uint32_t idx1_len = 0;
    int have_hdrl = 0;
    for (uint64_t off = 12; off + 8 <= riff_end;) {
        if (ub_read_at(c->fd, off, h, 12) != 0) break;
        uint32_t sz = ub_le32(h + 4);
        if (!memcmp(h, "LIST", 4) && sz >= 4) {
            if (!memcmp(h + 8, "hdrl", 4)) {
                int rc = parse_hdrl(c, a, off + 12, sz - 4);
                if (rc) return rc;
                have_hdrl = 1;
            } else if (!memcmp(h + 8, "movi", 4)) {
                movi = off + 8;
                movi_end = off + 8 + sz;
                if (movi_end > c->size) movi_end = c->size;
            }
        } else if (!memcmp(h, "idx1", 4)) {
            idx1 = off + 8;
            idx1_len = sz;
        }
        off += 8 + (uint64_t)sz + (sz & 1);
    }
    if (!have_hdrl || !movi) { uvid_fail("AVI file has no header or no packets"); return -EINVAL; }
    if (a->vs < 0) c->vcodec = UVID_VC_NONE;

    // OpenDML: further RIFF 'AVIX' pieces, each with its own 'movi'.
    int extended = riff_end + 12 <= c->size;
    if (!idx1 || extended || read_idx1(c, a, idx1, idx1_len, movi) != 0) {
        a->v.n = a->a.n = 0;
        int rc = walk_movi(c, a, movi + 4, movi_end);
        if (rc) return rc;
        uint64_t off = (riff_end + 1) & ~1ull;
        while (off + 24 <= c->size) {
            uint8_t x[24];
            if (ub_read_at(c->fd, off, x, 24) != 0 || memcmp(x, "RIFF", 4) || memcmp(x + 8, "AVIX", 4))
                break;
            uint64_t end = off + 8 + ub_le32(x + 4);
            if (!memcmp(x + 12, "LIST", 4) && !memcmp(x + 20, "movi", 4))
                walk_movi(c, a, off + 24, end < c->size ? end : c->size);
            off = (end + 1) & ~1ull;
        }
    }

    a->a_bytes = malloc(((size_t)a->a.n + 1) * sizeof *a->a_bytes);
    if (!a->a_bytes) return -ENOMEM;
    a->a_bytes[0] = 0;
    for (uint32_t i = 0; i < a->a.n; i++) a->a_bytes[i + 1] = a->a_bytes[i] + a->a.c[i].size;

    if (a->v.n) c->frames = a->v.n;
    uint32_t vms = 0, ams = 0;
    if (a->vrate) vms = (uint32_t)((uint64_t)(a->vstart + a->v.n) * a->vscale * 1000 / a->vrate);
    if (a->ablock && a->arate && c->acodec == UVID_AC_PCM) {
        c->a_frames = a->a_bytes[a->a.n] / a->ablock;
        ams = (uint32_t)(c->a_frames * 1000 / a->arate);
    }
    c->ms = vms > ams ? vms : ams;
    return 0;
}

static int64_t vpts(const struct avi *a, uint32_t i) {
    if (!a->vrate) return -1;
    return (int64_t)((uint64_t)(a->vstart + i) * a->vscale * 1000 / a->vrate);
}

static int64_t apts(const struct avi *a, uint32_t i) {
    if (!a->ablock || !a->arate) return -1;
    return (int64_t)(a->a_bytes[i] / a->ablock * 1000 / a->arate);
}

static int avi_read(struct uvid_src *c, int kind, struct uvid_pkt *p) {
    struct avi *a = c->priv;
    struct avi_list *l = kind == UVID_VIDEO ? &a->v : &a->a;
    uint32_t *pos = kind == UVID_VIDEO ? &a->vpos : &a->apos;
    if (*pos >= l->n) return 0;
    const struct avi_chunk *ch = &l->c[*pos];
    if (uvid_pkt_reserve(p, ch->size) != 0) return -ENOMEM;
    if (ch->size && ub_read_at(c->fd, ch->off, p->data, ch->size) != 0) {
        uvid_fail("AVI packet is truncated");
        return -EINVAL;
    }
    p->len = ch->size;
    p->key = 1;
    p->pts_ms = kind == UVID_VIDEO ? vpts(a, *pos) : apts(a, *pos);
    (*pos)++;
    return 1;
}

static int64_t avi_seek(struct uvid_src *c, int kind, int64_t ms) {
    struct avi *a = c->priv;
    if (ms < 0) ms = 0;
    if (kind == UVID_VIDEO) {
        if (!a->vrate || !a->vscale) return -ENOTSUP;
        uint64_t f = (uint64_t)ms * a->vrate / ((uint64_t)a->vscale * 1000);
        f = f > a->vstart ? f - a->vstart : 0;
        if (f >= a->v.n) f = a->v.n ? a->v.n - 1 : 0;
        a->vpos = (uint32_t)f;
        return vpts(a, a->vpos);
    }
    if (!a->ablock || !a->arate) return -ENOTSUP;
    uint64_t want = (uint64_t)ms * a->arate / 1000 * a->ablock;
    uint32_t lo = 0, hi = a->a.n;      // the last chunk starting at or before `want`
    while (hi - lo > 1) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (a->a_bytes[mid] <= want) lo = mid; else hi = mid;
    }
    a->apos = lo;
    c->a_next_frame = a->a_bytes[lo] / a->ablock;
    return apts(a, lo);
}

static void avi_close(struct uvid_src *c) {
    struct avi *a = c->priv;
    if (!a) return;
    free(a->v.c);
    free(a->a.c);
    free(a->a_bytes);
    free(a);
    c->priv = 0;
}

const struct uvid_container uvid_container_avi = {
    .name = "avi",
    .probe = avi_probe,
    .open = avi_open,
    .read = avi_read,
    .seek = avi_seek,
    .close = avi_close,
};
