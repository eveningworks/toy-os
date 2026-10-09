// MPEG-1 Audio Layer II (ISO/IEC 11172-3) -- the sound of an MPEG-1
// video, of DVB and DAB broadcasts, and of a `.mp2` file. A row in usnd's
// codec table for the file; the frame decoder is also usnd_vid.c's, for
// the sound inside a .mpg (usnd_internal.h).
//
// Layer II is Layer III's simpler elder: no Huffman coding, no MDCT, no
// bit reservoir. Each frame is self-contained -- a bit allocation per
// subband, scalefactors, then 36 quantised samples per subband -- and
// ends in the SAME synthesis filterbank (usnd_mpsynth.c). So a frame
// decodes from its own bytes alone, which is what makes a seek simple.
//
// The tables below are the standard's (Annex B, Tables 3-B.2a-d and
// 3-B.4), in libmad's compact arrangement: per allocation table, how
// many subbands carry audio and which bit-allocation class each one
// uses; per class, how many allocation bits and which row of
// quantisation classes they index. MPEG-2's half-rate extension (16,
// 22.05, 24 kHz) is refused, as Layer III's is.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lib/usnd.h"
#include "lib/usnd_internal.h"
#include "rt/sys.h"
#include "errno.h"

#define SLOTS 36                // 12 granules of 3 samples per subband

static const uint16_t BITRATE_L2[15] = { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 };
static const uint16_t BITRATE_L1[15] = { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 };
static const uint16_t BITRATE_L3[15] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };
static const uint32_t RATE[3] = { 44100, 48000, 32000 };

int usnd_mpa_header(const uint8_t *h, struct usnd_mpa_hdr *o) {
    if (h[0] != 0xFF || (h[1] & 0xF0) != 0xF0) return 0;  // 12-bit sync, MPEG-1 (ID=1)
    if (!(h[1] & 0x08)) return 0;
    int layer = 4 - ((h[1] >> 1) & 3);
    if (layer == 4) return 0;
    int br = (h[2] >> 4) & 15, sr = (h[2] >> 2) & 3;
    if (br == 0 || br == 15 || sr == 3) return 0;
    o->layer = layer;
    o->protect = !(h[1] & 1);
    o->rate = (int)RATE[sr];
    o->padding = (h[2] >> 1) & 1;
    o->mode = (h[3] >> 6) & 3;
    o->mode_ext = (h[3] >> 4) & 3;
    o->channels = o->mode == 3 ? 1 : 2;
    const uint16_t *t = layer == 1 ? BITRATE_L1 : layer == 2 ? BITRATE_L2 : BITRATE_L3;
    o->bitrate = t[br];
    if (layer == 1) o->frame_bytes = (12000 * o->bitrate / o->rate + o->padding) * 4;
    else o->frame_bytes = 144000 * o->bitrate / o->rate + o->padding;
    return 1;
}

// --- the tables ---------------------------------------------------------

static const struct { uint8_t sblimit; uint8_t cls[30]; } SBQUANT[4] = {
    { 27, { 7, 7, 7, 6, 6, 6, 6, 6, 6, 6, 6, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 0, 0 } },  // B.2a
    { 30, { 7, 7, 7, 6, 6, 6, 6, 6, 6, 6, 6, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 0, 0, 0, 0, 0 } },  // B.2b
    {  8, { 5, 5, 2, 2, 2, 2, 2, 2 } },                                     // B.2c
    { 12, { 5, 5, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 } },                         // B.2d
};

// Per allocation class: bits of allocation, and the row of OFFSETS that
// turns an allocation into a quantisation class.
static const struct { uint8_t nbal, row; } BITALLOC[8] = {
    { 2, 0 }, { 2, 3 }, { 3, 3 }, { 3, 1 }, { 4, 2 }, { 4, 3 }, { 4, 4 }, { 4, 5 },
};

static const uint8_t OFFSETS[6][15] = {
    { 0, 1, 16 },
    { 0, 1, 2, 3, 4, 5, 16 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 },
    { 0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16 },
    { 0, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 },
};

// Table 3-B.4: the quantisation classes. GROUPED ones pack three samples
// into one codeword (3, 5 and 9 levels); `nb` is then the width each
// sample would have had, which the requantisation below needs.
static const struct { uint16_t levels; uint8_t grouped, bits, nb; } QCLASS[17] = {
    { 3, 1, 5, 2 }, { 5, 1, 7, 3 }, { 7, 0, 3, 3 }, { 9, 1, 10, 4 },
    { 15, 0, 4, 4 }, { 31, 0, 5, 5 }, { 63, 0, 6, 6 }, { 127, 0, 7, 7 },
    { 255, 0, 8, 8 }, { 511, 0, 9, 9 }, { 1023, 0, 10, 10 }, { 2047, 0, 11, 11 },
    { 4095, 0, 12, 12 }, { 8191, 0, 13, 13 }, { 16383, 0, 14, 14 }, { 32767, 0, 15, 15 },
    { 65535, 0, 16, 16 },
};

static float g_c[17], g_d[17], g_sf[64];
static int g_ready;

// C and D (s'' = C * (s''' + D)) follow from the class: C is 2^nb over
// the level count, D half a step -- except that the grouped classes'
// D is 1/2 whatever their width (Table 3-B.4). The scalefactors are
// 2^(1 - i/3), index 63 unused. Derived rather than tabled.
static void build(void) {
    if (g_ready) return;
    for (int i = 0; i < 17; i++) {
        int nb = QCLASS[i].nb;
        g_c[i] = (float)(1 << nb) / (float)QCLASS[i].levels;
        g_d[i] = QCLASS[i].grouped ? 0.5f : 1.0f / (float)(1 << (nb - 1));
    }
    for (int i = 0; i < 63; i++) g_sf[i] = (float)(2.0 * pow(2.0, -(double)i / 3.0));
    g_sf[63] = 0.0f;
    g_ready = 1;
}

struct usnd_mp2 {
    struct usnd_mpsynth synth[2];
    float sb[2][SLOTS * 32];
    uint8_t alloc[2][32], scfsi[2][32], scf[2][32][3];
};

struct bits { const uint8_t *p; int len, pos; };

static uint32_t bget(struct bits *b, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        int byte = b->pos >> 3;
        int bit = byte < b->len ? (b->p[byte] >> (7 - (b->pos & 7))) & 1 : 0;
        v = v << 1 | (uint32_t)bit;
        b->pos++;
    }
    return v;
}

struct usnd_mp2 *usnd_mp2_new(void) {
    build();
    return calloc(1, sizeof(struct usnd_mp2));
}

void usnd_mp2_free(struct usnd_mp2 *d) { free(d); }

void usnd_mp2_reset(struct usnd_mp2 *d) {
    usnd_mpsynth_reset(&d->synth[0]);
    usnd_mpsynth_reset(&d->synth[1]);
}

static void samples(struct bits *b, int q, float out[3]) {
    int s[3], nb = QCLASS[q].nb;
    if (QCLASS[q].grouped) {
        uint32_t c = bget(b, QCLASS[q].bits);
        for (int i = 0; i < 3; i++) { s[i] = (int)(c % QCLASS[q].levels); c /= QCLASS[q].levels; }
    } else {
        for (int i = 0; i < 3; i++) s[i] = (int)bget(b, nb);
    }
    for (int i = 0; i < 3; i++) {
        // Invert the top bit and sign-extend: a fraction in [-1, 1).
        int v = s[i] ^ (1 << (nb - 1));
        if (v & (1 << (nb - 1))) v -= 1 << nb;
        float frac = (float)v / (float)(1 << (nb - 1));
        out[i] = g_c[q] * (frac + g_d[q]);
    }
}

int usnd_mp2_decode(struct usnd_mp2 *d, const uint8_t *frame, int len, int32_t *out) {
    struct usnd_mpa_hdr h;
    if (len < 4 || !usnd_mpa_header(frame, &h) || h.layer != 2 || len < h.frame_bytes) return -EINVAL;
    int nch = h.channels;
    int per_ch = nch == 2 ? h.bitrate / 2 : h.bitrate;
    int table;
    if (per_ch <= 48) table = h.rate == 32000 ? 3 : 2;
    else if (per_ch <= 80) table = 0;
    else table = h.rate == 48000 ? 0 : 1;
    int sblimit = SBQUANT[table].sblimit;
    const uint8_t *cls = SBQUANT[table].cls;
    int bound = h.mode == 1 ? 4 + h.mode_ext * 4 : 32;
    if (bound > sblimit) bound = sblimit;

    struct bits b = { frame, h.frame_bytes, 32 + (h.protect ? 16 : 0) };
    for (int sb = 0; sb < sblimit; sb++) {
        int nbal = BITALLOC[cls[sb]].nbal;
        if (sb < bound) {
            for (int ch = 0; ch < nch; ch++) d->alloc[ch][sb] = (uint8_t)bget(&b, nbal);
        } else {
            d->alloc[0][sb] = d->alloc[1][sb] = (uint8_t)bget(&b, nbal);
        }
    }
    for (int sb = 0; sb < sblimit; sb++)
        for (int ch = 0; ch < nch; ch++)
            if (d->alloc[ch][sb]) d->scfsi[ch][sb] = (uint8_t)bget(&b, 2);
    for (int sb = 0; sb < sblimit; sb++) {
        for (int ch = 0; ch < nch; ch++) {
            if (!d->alloc[ch][sb]) continue;
            uint8_t *f = d->scf[ch][sb];
            f[0] = (uint8_t)bget(&b, 6);
            switch (d->scfsi[ch][sb]) {
            case 0: f[1] = (uint8_t)bget(&b, 6); f[2] = (uint8_t)bget(&b, 6); break;
            case 1: f[1] = f[0]; f[2] = (uint8_t)bget(&b, 6); break;
            case 2: f[1] = f[2] = f[0]; break;
            case 3: f[1] = f[2] = (uint8_t)bget(&b, 6); break;
            }
        }
    }

    memset(d->sb, 0, sizeof d->sb);
    for (int gr = 0; gr < 12; gr++) {
        for (int sb = 0; sb < sblimit; sb++) {
            int row = BITALLOC[cls[sb]].row;
            if (sb < bound) {
                for (int ch = 0; ch < nch; ch++) {
                    int a = d->alloc[ch][sb];
                    if (!a) continue;
                    float s[3];
                    samples(&b, OFFSETS[row][a - 1], s);
                    float k = g_sf[d->scf[ch][sb][gr / 4]];
                    for (int i = 0; i < 3; i++) d->sb[ch][(gr * 3 + i) * 32 + sb] = s[i] * k;
                }
            } else {
                int a = d->alloc[0][sb];
                if (!a) continue;
                float s[3];
                samples(&b, OFFSETS[row][a - 1], s);
                for (int ch = 0; ch < nch; ch++) {
                    float k = g_sf[d->scf[ch][sb][gr / 4]];
                    for (int i = 0; i < 3; i++) d->sb[ch][(gr * 3 + i) * 32 + sb] = s[i] * k;
                }
            }
        }
    }
    for (int ch = 0; ch < nch; ch++)
        usnd_mpsynth_run(&d->synth[ch], d->sb[ch], SLOTS, out + ch, nch);
    return SLOTS * 32;
}

// --- a .mp2 file ----------------------------------------------------------

#define MAX_FRAME 1792              // 384 kbit/s at 32 kHz, padded

struct mp2file {
    struct usnd_mp2 *d;
    long first;                     // the first frame's offset
    uint8_t frame[MAX_FRAME];
    int32_t out[SLOTS * 32 * 2];
    int out_n, out_pos;
};

static int mp2_probe(const uint8_t *d, size_t n) {
    struct usnd_mpa_hdr h;
    return n >= 4 && usnd_mpa_header(d, &h) && h.layer == 2;
}

static int mp2_open(struct usnd_stream *s) {
    struct mp2file *m = calloc(1, sizeof *m);
    if (!m) { usnd_fail("out of memory"); return -ENOMEM; }
    m->d = usnd_mp2_new();
    if (!m->d) { free(m); usnd_fail("out of memory"); return -ENOMEM; }
    uint8_t hb[4];
    struct usnd_mpa_hdr h;
    if (sys_read(s->fd, hb, 4) != 4 || !usnd_mpa_header(hb, &h) || h.layer != 2) {
        usnd_mp2_free(m->d);
        free(m);
        usnd_fail("not an MPEG-1 Layer II file");
        return -EINVAL;
    }
    s->priv = m;
    s->fmt.rate = (uint32_t)h.rate;
    s->fmt.channels = (uint16_t)h.channels;
    s->fmt.bits = 16;
    struct sys_stat st;
    if (sys_fstat(s->fd, &st) == 0 && h.bitrate)
        s->frames = (uint64_t)st.size * 8 / (uint64_t)h.bitrate * (uint64_t)h.rate / 1000;
    snprintf(s->detail, sizeof s->detail, "MP2 %d kbit/s", h.bitrate);
    sys_lseek(s->fd, 0, SYS_SEEK_SET);
    return 0;
}

// The next frame from the file, resyncing past anything that is not one.
static int next_frame(struct usnd_stream *s, struct mp2file *m) {
    for (int scan = 0; scan < 1 << 16; scan++) {
        uint8_t hb[4];
        long long at = sys_lseek(s->fd, 0, SYS_SEEK_CUR);
        if (sys_read(s->fd, hb, 4) != 4) return 0;
        struct usnd_mpa_hdr h;
        if (usnd_mpa_header(hb, &h) && h.layer == 2 && h.frame_bytes <= MAX_FRAME &&
            h.channels == s->fmt.channels && h.rate == (int)s->fmt.rate) {
            memcpy(m->frame, hb, 4);
            if (sys_read(s->fd, m->frame + 4, (size_t)h.frame_bytes - 4) != h.frame_bytes - 4) return 0;
            int n = usnd_mp2_decode(m->d, m->frame, h.frame_bytes, m->out);
            if (n < 0) return n;
            m->out_n = n;
            m->out_pos = 0;
            return 1;
        }
        sys_lseek(s->fd, at + 1, SYS_SEEK_SET);
    }
    return 0;
}

static long mp2_read(struct usnd_stream *s, int32_t *dst, long frames) {
    struct mp2file *m = s->priv;
    int nch = s->fmt.channels;
    long got = 0;
    while (got < frames) {
        if (m->out_pos >= m->out_n) {
            int rc = next_frame(s, m);
            if (rc <= 0) return got ? got : rc;
        }
        long take = m->out_n - m->out_pos;
        if (take > frames - got) take = frames - got;
        memcpy(dst + got * nch, m->out + m->out_pos * nch, (size_t)take * (size_t)nch * sizeof *dst);
        m->out_pos += (int)take;
        got += take;
    }
    return got;
}

// Frames are independent, so a seek is arithmetic on a constant bitrate
// and a resync -- then the filterbank's history from the frame before.
static int mp2_seek(struct usnd_stream *s, uint64_t frame) {
    struct mp2file *m = s->priv;
    uint8_t hb[4];
    struct usnd_mpa_hdr h;
    if (sys_lseek(s->fd, 0, SYS_SEEK_SET) != 0 || sys_read(s->fd, hb, 4) != 4 || !usnd_mpa_header(hb, &h))
        return -EIO;
    uint64_t index = frame / (SLOTS * 32);
    uint64_t pre = index ? 1 : 0;    // one frame back to fill the filterbank
    long long off = (long long)((index - pre) * (uint64_t)h.bitrate * 144000 / (uint64_t)h.rate);
    sys_lseek(s->fd, off, SYS_SEEK_SET);
    usnd_mp2_reset(m->d);
    m->out_n = m->out_pos = 0;
    if (pre && next_frame(s, m) <= 0) return -EIO;
    m->out_n = m->out_pos = 0;
    if (next_frame(s, m) <= 0) return 0;
    m->out_pos = (int)(frame % (SLOTS * 32));
    return 0;
}

static void mp2_close(struct usnd_stream *s) {
    struct mp2file *m = s->priv;
    if (!m) return;
    usnd_mp2_free(m->d);
    free(m);
    s->priv = 0;
}

const struct usnd_codec usnd_codec_mp2 = {
    .name = "mp2",
    .probe = mp2_probe,
    .open = mp2_open,
    .read = mp2_read,
    .seek = mp2_seek,
    .close = mp2_close,
};
