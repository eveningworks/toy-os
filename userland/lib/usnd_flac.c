// The FLAC codec: the Free Lossless Audio Codec, native .flac files.
//
// LOSSLESS, SO EXACT: what this hands out is the encoder's input, bit for
// bit, in s32 with full scale in the top bits -- a 24-bit file reaches a
// 24-bit card whole (lib/usnd.h). tools/usnd_hostcheck.py holds it to
// that against `flac -d`, sample for sample, with no tolerance.
//
// Every depth the format allows (4 to 32 bits), any rate, mono or stereo
// (3-8 channels are -ENOTSUP, as for every codec here); CONSTANT,
// VERBATIM, FIXED and LPC subframes, wasted bits, the three stereo
// decorrelations, both Rice codings and their escape. Both CRCs are
// checked: a frame whose CRC-16 fails is -EINVAL, never played as noise.
// Ogg-wrapped FLAC (.oga) is not this codec -- no row claims Ogg.
//
// **SEEKING IS EXACT**, because FLAC frames are independent (MP3's bit
// reservoir has no equivalent here): the SEEKTABLE narrows the search
// when the file has one, a bisection over frame headers finishes it,
// then the target frame is decoded and the samples before the target
// dropped.
//
// Samples are decoded in 64 bits: a 32-bit stream's SIDE channel has 33,
// and an LPC sum of 32 coefficients needs the rest.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lib/usnd.h"
#include "lib/usnd_internal.h"
#include "rt/sys.h"
#include "errno.h"

#define INBUF      65536
#define MAX_BLOCK  65535
#define MAX_LPC    32

struct seekpt { uint64_t sample, offset; };

struct flac {
    // STREAMINFO
    uint32_t rate;
    int channels, bps;
    uint32_t min_block, max_block;
    uint64_t total;                  // 0 when the encoder did not know

    uint64_t first_frame;            // file offset of the first frame
    uint64_t file_len;
    struct seekpt *seek;
    int nseek;

    // The input: a window of the file, read forward.
    int fd;
    uint8_t buf[INBUF];
    long len, pos;
    uint64_t buf_at;                 // file offset of buf[0]
    uint64_t cache;                  // bits not yet consumed, low `nc` of them
    int nc;
    uint8_t crc8;
    uint16_t crc16;

    // One decoded frame.
    int64_t *dec[2];
    int64_t *res;                    // residual scratch, block-sized
    uint32_t block, at;              // its size, and how far it has been handed out
    uint64_t frame_sample;           // stream sample number of dec[*][0]
    uint64_t skip;                   // after a seek: samples still to drop
    int done;
};

// --- CRCs --------------------------------------------------------------------
//
// Both the format's own, polynomial for polynomial: CRC-8 x^8+x^2+x+1
// over the frame header, CRC-16 x^16+x^15+x^2+1 over the whole frame.

static uint8_t g_crc8[256];
static uint16_t g_crc16[256];

static void crc_tables(void) {
    if (g_crc16[1]) return;
    for (int i = 0; i < 256; i++) {
        uint8_t c = (uint8_t)i;
        for (int k = 0; k < 8; k++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : c << 1);
        g_crc8[i] = c;
        uint16_t w = (uint16_t)(i << 8);
        for (int k = 0; k < 8; k++) w = (uint16_t)((w & 0x8000) ? (w << 1) ^ 0x8005 : w << 1);
        g_crc16[i] = w;
    }
}

// --- reading bytes and bits ---------------------------------------------------

static void seek_byte(struct flac *f, uint64_t off) {
    sys_lseek(f->fd, (long long)off, SYS_SEEK_SET);
    f->buf_at = off;
    f->len = f->pos = 0;
    f->nc = 0;
}

static uint64_t tell(const struct flac *f) { return f->buf_at + (uint64_t)f->pos; }

// Back to `off` -- inside the window already read when it is there, which
// is what keeps a sync search from re-reading the file at every 0xFF.
static void rewind_to(struct flac *f, uint64_t off) {
    if (off >= f->buf_at && off <= f->buf_at + (uint64_t)f->len) {
        f->pos = (long)(off - f->buf_at);
        f->nc = 0;
    } else {
        seek_byte(f, off);
    }
}

// The next byte, folded into both CRCs, or -1 at the end of the file.
static int rbyte(struct flac *f) {
    if (f->pos >= f->len) {
        f->buf_at += (uint64_t)f->len;
        long long n = sys_read(f->fd, f->buf, INBUF);
        f->len = n > 0 ? (long)n : 0;
        f->pos = 0;
        if (f->len == 0) return -1;
    }
    uint8_t b = f->buf[f->pos++];
    f->crc8 = g_crc8[f->crc8 ^ b];
    f->crc16 = (uint16_t)((f->crc16 << 8) ^ g_crc16[(f->crc16 >> 8) ^ b]);
    return b;
}

// `n` bits, 0..32, most significant first. Bytes are fetched one at a
// time as they are needed, so fewer than 8 are ever left over -- which
// is what makes the byte alignment at a frame's end just `nc = 0`.
static int bits(struct flac *f, int n, uint32_t *out) {
    while (f->nc < n) {
        int b = rbyte(f);
        if (b < 0) return -1;
        f->cache = (f->cache << 8) | (uint32_t)b;
        f->nc += 8;
    }
    f->nc -= n;
    *out = (uint32_t)((f->cache >> f->nc) & ((n == 32) ? 0xFFFFFFFFull : ((1ull << n) - 1)));
    return 0;
}

static int sbits(struct flac *f, int n, int64_t *out) {
    uint32_t u;
    if (n == 0) { *out = 0; return 0; }
    if (bits(f, n, &u) < 0) return -1;
    int64_t v = (int64_t)u;
    if (n < 64 && (v & ((int64_t)1 << (n - 1)))) v -= (int64_t)1 << n;
    *out = v;
    return 0;
}

// A wider signed value than 32 bits: a 32-bit stream's side channel, 33.
static int sbits_wide(struct flac *f, int n, int64_t *out) {
    if (n <= 32) return sbits(f, n, out);
    uint32_t hi, lo;
    if (bits(f, n - 32, &hi) < 0 || bits(f, 32, &lo) < 0) return -1;
    int64_t v = ((int64_t)hi << 32) | lo;
    if (v & ((int64_t)1 << (n - 1))) v -= (int64_t)1 << n;
    *out = v;
    return 0;
}

// Zeros before a one. Bounded: a run longer than any real quotient is a
// broken stream, not a reason to read to the end of the file.
static int unary(struct flac *f, uint32_t *q) {
    uint32_t n = 0;
    for (;;) {
        if (f->nc == 0) {
            int b = rbyte(f);
            if (b < 0) return -1;
            f->cache = (uint32_t)b;
            f->nc = 8;
        }
        while (f->nc) {
            f->nc--;
            if ((f->cache >> f->nc) & 1) { *q = n; return 0; }
            if (++n > (1u << 24)) return -1;
        }
    }
}

// --- the frame header ----------------------------------------------------------

struct fhdr {
    uint32_t block;
    int ch_mode;          // 0-7 independent, 8 left/side, 9 right/side, 10 mid/side
    int bps;
    uint64_t number;      // a frame number, or a sample number when `variable`
    int variable;
};

// At a sync code. 0, or -1 for anything that is not a valid header --
// which is how a search tells a real frame from a 0xFFF8 in the middle
// of some other frame's data.
static int frame_header(struct flac *f, struct fhdr *h) {
    f->crc8 = 0;
    f->crc16 = 0;
    f->nc = 0;
    int b0 = rbyte(f), b1 = rbyte(f), b2 = rbyte(f), b3 = rbyte(f);
    if (b3 < 0 || b0 != 0xFF || (b1 & 0xFE) != 0xF8) return -1;
    h->variable = b1 & 1;
    int bs = b2 >> 4, sr = b2 & 15;
    h->ch_mode = b3 >> 4;
    int ss = (b3 >> 1) & 7;
    if ((b3 & 1) || bs == 0 || sr == 15 || h->ch_mode > 10 || ss == 3) return -1;

    // The "UTF-8" coded number: 1 to 7 bytes, the first's leading ones
    // saying how many follow.
    int c = rbyte(f);
    if (c < 0 || c == 0xFF) return -1;
    int more = 0;
    uint64_t v;
    if (!(c & 0x80)) v = (uint64_t)c;
    else {
        int mask = 0x40;
        while (c & mask) { more++; mask >>= 1; }
        if (more == 0 || more > 6) return -1;
        v = (uint64_t)(c & (mask - 1));
    }
    for (int i = 0; i < more; i++) {
        int x = rbyte(f);
        if (x < 0 || (x & 0xC0) != 0x80) return -1;
        v = (v << 6) | (uint64_t)(x & 0x3F);
    }
    h->number = v;

    if (bs == 1) h->block = 192;
    else if (bs <= 5) h->block = 576u << (bs - 2);
    else if (bs == 6) { int x = rbyte(f); if (x < 0) return -1; h->block = (uint32_t)x + 1; }
    else if (bs == 7) {
        int x = rbyte(f), y = rbyte(f);
        if (y < 0) return -1;
        h->block = (uint32_t)((x << 8) | y) + 1;
    } else h->block = 256u << (bs - 8);

    if (sr == 12) { if (rbyte(f) < 0) return -1; }
    else if (sr == 13 || sr == 14) { if (rbyte(f) < 0 || rbyte(f) < 0) return -1; }

    static const int depth[8] = { 0, 8, 12, -1, 16, 20, 24, 32 };
    h->bps = ss ? depth[ss] : f->bps;

    uint8_t want = f->crc8;
    int crc = rbyte(f);
    if (crc < 0 || (uint8_t)crc != want) return -1;
    if (h->block > f->max_block && f->max_block) return -1;
    return 0;
}

// --- subframes -------------------------------------------------------------------

static int residual(struct flac *f, int64_t *out, uint32_t block, int order) {
    uint32_t method, porder;
    if (bits(f, 2, &method) < 0 || method > 1 || bits(f, 4, &porder) < 0) return -1;
    int pbits = method ? 5 : 4;
    uint32_t escape = method ? 31 : 15;
    uint32_t parts = 1u << porder;
    if (porder && (block % parts)) return -1;
    uint32_t per = block >> porder;
    if (per < (uint32_t)order) return -1;

    uint32_t i = 0;
    for (uint32_t p = 0; p < parts; p++) {
        uint32_t n = p == 0 ? per - (uint32_t)order : per;
        uint32_t k;
        if (bits(f, pbits, &k) < 0) return -1;
        if (k == escape) {
            uint32_t raw;
            if (bits(f, 5, &raw) < 0) return -1;
            for (uint32_t j = 0; j < n; j++, i++)
                if (sbits(f, (int)raw, &out[i]) < 0) return -1;
            continue;
        }
        for (uint32_t j = 0; j < n; j++, i++) {
            uint32_t q, r = 0;
            if (unary(f, &q) < 0) return -1;
            if (k && bits(f, (int)k, &r) < 0) return -1;
            uint64_t u = ((uint64_t)q << k) | r;
            out[i] = (int64_t)(u >> 1) ^ -(int64_t)(u & 1);   // zig-zag back to signed
        }
    }
    return 0;
}

static int subframe(struct flac *f, int64_t *s, uint32_t block, int depth) {
    uint32_t pad, type, wflag;
    if (bits(f, 1, &pad) < 0 || pad || bits(f, 6, &type) < 0 || bits(f, 1, &wflag) < 0)
        return -1;
    int wasted = 0;
    if (wflag) {
        uint32_t q;
        if (unary(f, &q) < 0) return -1;
        wasted = (int)q + 1;
        depth -= wasted;
        if (depth <= 0) return -1;
    }

    if (type == 0) {                                           // CONSTANT
        int64_t v;
        if (sbits_wide(f, depth, &v) < 0) return -1;
        for (uint32_t i = 0; i < block; i++) s[i] = v;
    } else if (type == 1) {                                    // VERBATIM
        for (uint32_t i = 0; i < block; i++)
            if (sbits_wide(f, depth, &s[i]) < 0) return -1;
    } else if (type >= 8 && type <= 12) {                     // FIXED, order 0-4
        int order = (int)type - 8;
        if ((uint32_t)order > block) return -1;
        for (int i = 0; i < order; i++)
            if (sbits_wide(f, depth, &s[i]) < 0) return -1;
        if (residual(f, f->res, block, order) < 0) return -1;
        const int64_t *r = f->res;
        for (uint32_t i = (uint32_t)order; i < block; i++) {
            int64_t e = r[i - (uint32_t)order];
            switch (order) {  // dispatch-ok: the format's five fixed predictors
            case 0: s[i] = e; break;
            case 1: s[i] = e + s[i - 1]; break;
            case 2: s[i] = e + 2 * s[i - 1] - s[i - 2]; break;
            case 3: s[i] = e + 3 * s[i - 1] - 3 * s[i - 2] + s[i - 3]; break;
            default: s[i] = e + 4 * s[i - 1] - 6 * s[i - 2] + 4 * s[i - 3] - s[i - 4]; break;
            }
        }
    } else if (type >= 32) {                                   // LPC, order 1-32
        int order = (int)(type & 31) + 1;
        if ((uint32_t)order > block) return -1;
        for (int i = 0; i < order; i++)
            if (sbits_wide(f, depth, &s[i]) < 0) return -1;
        uint32_t prec;
        int64_t shift;
        if (bits(f, 4, &prec) < 0 || prec == 15 || sbits(f, 5, &shift) < 0 || shift < 0)
            return -1;
        int64_t coef[MAX_LPC];
        for (int i = 0; i < order; i++)
            if (sbits(f, (int)prec + 1, &coef[i]) < 0) return -1;
        if (residual(f, f->res, block, order) < 0) return -1;
        for (uint32_t i = (uint32_t)order; i < block; i++) {
            int64_t sum = 0;
            for (int j = 0; j < order; j++) sum += coef[j] * s[i - 1 - (uint32_t)j];
            s[i] = f->res[i - (uint32_t)order] + (sum >> shift);
        }
    } else {
        return -1;                                             // reserved
    }
    if (wasted)
        for (uint32_t i = 0; i < block; i++) s[i] = (int64_t)((uint64_t)s[i] << wasted);
    return 0;
}

// --- one frame -------------------------------------------------------------------

// Decodes the frame at the current position into f->dec. 1 a frame, 0
// the end of the stream, -1 a broken one (with the reason set).
static int decode_frame(struct flac *f) {
    if (f->total && f->frame_sample + f->block >= f->total && f->block) return 0;
    struct fhdr h;
    uint64_t at = tell(f);
    if (frame_header(f, &h) < 0) {
        // The end of the audio is where the bytes stop parsing as
        // frames: the file's end, or a trailing tag (ID3v1 is 128 bytes)
        // after the last frame.
        if (at >= f->file_len || f->file_len - at <= 256) return 0;
        usnd_fail("FLAC frame header is corrupt");
        return -1;
    }
    int nch = h.ch_mode <= 7 ? h.ch_mode + 1 : 2;
    if (nch != f->channels || h.bps != f->bps) {
        usnd_fail("FLAC frame disagrees with STREAMINFO about channels or depth");
        return -1;
    }
    for (int c = 0; c < nch; c++) {
        // The SIDE channel carries one bit more than the samples do.
        int side = (h.ch_mode == 8 && c == 1) || (h.ch_mode == 9 && c == 0) ||
                   (h.ch_mode == 10 && c == 1);
        if (subframe(f, f->dec[c], h.block, h.bps + side) < 0) {
            usnd_fail("FLAC subframe is corrupt");
            return -1;
        }
    }
    f->nc = 0;                                  // zero padding to a byte
    uint16_t want = f->crc16;
    int a = rbyte(f), b = rbyte(f);
    if (b < 0 || (uint16_t)((a << 8) | b) != want) {
        usnd_fail("FLAC frame fails its CRC-16");
        return -1;
    }

    int64_t *l = f->dec[0], *r = f->dec[1];
    if (h.ch_mode == 8) {                       // left, side
        for (uint32_t i = 0; i < h.block; i++) r[i] = l[i] - r[i];
    } else if (h.ch_mode == 9) {                // side, right
        for (uint32_t i = 0; i < h.block; i++) l[i] += r[i];
    } else if (h.ch_mode == 10) {               // mid, side
        for (uint32_t i = 0; i < h.block; i++) {
            int64_t mid = (int64_t)((uint64_t)l[i] << 1) | (r[i] & 1), side = r[i];
            l[i] = (mid + side) >> 1;
            r[i] = (mid - side) >> 1;
        }
    }
    f->frame_sample = h.variable ? h.number : h.number * f->min_block;
    f->block = h.block;
    f->at = 0;
    return 1;
}

// --- the codec -------------------------------------------------------------------

static int flac_probe(const uint8_t *d, size_t n) {
    return n >= 4 && memcmp(d, "fLaC", 4) == 0;
}

static uint32_t be24(const uint8_t *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
static uint64_t be64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | p[i];
    return v;
}

static int read_exact(int fd, void *dst, size_t n) {
    return (long long)n == sys_read(fd, dst, n) ? 0 : -1;
}

static void flac_close(struct usnd_stream *s) {
    struct flac *f = s->priv;
    if (!f) return;
    free(f->dec[0]);
    free(f->dec[1]);
    free(f->res);
    free(f->seek);
    free(f);
    s->priv = 0;
}

static int flac_open(struct usnd_stream *s) {
    crc_tables();
    uint8_t m[4];
    if (read_exact(s->fd, m, 4) < 0 || memcmp(m, "fLaC", 4) != 0) {
        usnd_fail("not a FLAC file");
        return -EINVAL;
    }
    struct flac *f = calloc(1, sizeof *f);
    if (!f) { usnd_fail("out of memory"); return -ENOMEM; }
    f->fd = s->fd;
    s->priv = f;

    // THE METADATA BLOCKS: STREAMINFO must come first; SEEKTABLE is
    // kept; the rest (tags and pictures are utags.h's) are stepped over.
    uint64_t at = 4;
    int have_info = 0, last = 0;
    while (!last) {
        uint8_t h[4];
        if (read_exact(s->fd, h, 4) < 0) { usnd_fail("FLAC metadata is truncated"); goto bad; }
        last = h[0] >> 7;
        int type = h[0] & 0x7F;
        uint32_t len = be24(h + 1);
        at += 4;
        if (type == 0) {
            uint8_t b[34];
            if (len < 34 || read_exact(s->fd, b, 34) < 0) { usnd_fail("FLAC STREAMINFO is short"); goto bad; }
            f->min_block = (uint32_t)b[0] << 8 | b[1];
            f->max_block = (uint32_t)b[2] << 8 | b[3];
            f->rate = (uint32_t)b[10] << 12 | (uint32_t)b[11] << 4 | (uint32_t)(b[12] >> 4);
            f->channels = ((b[12] >> 1) & 7) + 1;
            f->bps = (((b[12] & 1) << 4) | (b[13] >> 4)) + 1;
            f->total = ((uint64_t)(b[13] & 15) << 32) | (uint64_t)b[14] << 24 |
                       (uint64_t)b[15] << 16 | (uint64_t)b[16] << 8 | b[17];
            have_info = 1;
        } else if (type == 3 && len >= 18 && !f->seek) {
            int n = (int)(len / 18);
            f->seek = malloc((size_t)n * sizeof *f->seek);
            uint8_t *raw = malloc((size_t)n * 18);
            if (!f->seek || !raw || read_exact(s->fd, raw, (size_t)n * 18) < 0) {
                free(raw);
                usnd_fail("FLAC SEEKTABLE is truncated");
                goto bad;
            }
            for (int i = 0; i < n; i++) {
                uint64_t smp = be64(raw + i * 18);
                if (smp == ~0ull) continue;                   // a placeholder
                f->seek[f->nseek].sample = smp;
                f->seek[f->nseek].offset = be64(raw + i * 18 + 8);
                f->nseek++;
            }
            free(raw);
        }
        at += len;
        sys_lseek(s->fd, (long long)at, SYS_SEEK_SET);
    }
    if (!have_info) { usnd_fail("FLAC has no STREAMINFO"); goto bad; }
    if (f->channels > 2) { usnd_fail("FLAC with more than two channels is not supported"); goto notsup; }
    if (f->bps < 4 || f->rate == 0 || f->max_block < 16 || f->max_block > MAX_BLOCK ||
        f->min_block > f->max_block) {
        usnd_fail("FLAC STREAMINFO is invalid");
        goto bad;
    }
    if (f->rate < 4000 || f->rate > 192000) {
        usnd_fail("FLAC sample rate is out of range");
        goto notsup;
    }

    f->first_frame = at;
    long long end = sys_lseek(s->fd, 0, SYS_SEEK_END);
    f->file_len = end > 0 ? (uint64_t)end : 0;
    for (int c = 0; c < 2; c++) {
        f->dec[c] = malloc((size_t)f->max_block * sizeof(int64_t));
        if (!f->dec[c]) { usnd_fail("out of memory"); flac_close(s); return -ENOMEM; }
    }
    f->res = malloc((size_t)f->max_block * sizeof(int64_t));
    if (!f->res) { usnd_fail("out of memory"); flac_close(s); return -ENOMEM; }
    seek_byte(f, f->first_frame);

    s->fmt.rate = f->rate;
    s->fmt.channels = (uint16_t)f->channels;
    s->fmt.bits = (uint16_t)f->bps;
    s->frames = f->total;
    snprintf(s->detail, sizeof s->detail, "FLAC %d-bit %s %u Hz", f->bps,
             f->channels == 1 ? "mono" : "stereo", (unsigned)f->rate);
    return 0;
bad:
    flac_close(s);
    return -EINVAL;
notsup:
    flac_close(s);
    return -ENOTSUP;
}

static long flac_read(struct usnd_stream *s, int32_t *dst, long frames) {
    struct flac *f = s->priv;
    int shift = 32 - f->bps;
    long done = 0;
    while (done < frames && !f->done) {
        if (f->at >= f->block) {
            int rc = decode_frame(f);
            if (rc == 0) { f->done = 1; break; }
            if (rc < 0) return done ? done : -EINVAL;
            if (f->skip) {
                uint32_t drop = f->skip < f->block ? (uint32_t)f->skip : f->block;
                f->at = drop;
                f->skip -= drop;
                continue;
            }
        }
        uint32_t take = f->block - f->at;
        if ((long)take > frames - done) take = (uint32_t)(frames - done);
        if (f->total && f->frame_sample + f->at + take > f->total)
            take = f->frame_sample + f->at < f->total ? (uint32_t)(f->total - f->frame_sample - f->at) : 0;
        if (!take) { f->done = 1; break; }
        for (uint32_t i = 0; i < take; i++)
            for (int c = 0; c < f->channels; c++)
                dst[(done + (long)i) * f->channels + c] =
                    (int32_t)(uint32_t)((uint64_t)f->dec[c][f->at + i] << shift);
        f->at += take;
        done += (long)take;
    }
    return done;
}

// --- seeking ---------------------------------------------------------------------

// The first frame header at or after `off`: its offset and first sample.
// Found by scanning for the sync code and accepting only a header whose
// CRC-8 checks out. 0, or -1 when there is none before the end.
static int frame_at_or_after(struct flac *f, uint64_t off, uint64_t *where, uint64_t *sample) {
    uint64_t limit = off + 2 * (uint64_t)INBUF;   // a frame turns up well within this
    seek_byte(f, off);
    while (tell(f) < f->file_len && tell(f) < limit) {
        uint64_t p = tell(f);
        int b = rbyte(f);
        if (b < 0) return -1;
        if (b != 0xFF) continue;
        rewind_to(f, p);
        struct fhdr h;
        if (frame_header(f, &h) == 0) {
            *where = p;
            *sample = h.variable ? h.number : h.number * f->min_block;
            return 0;
        }
        rewind_to(f, p + 1);
    }
    return -1;
}

static int flac_seek(struct usnd_stream *s, uint64_t target) {
    struct flac *f = s->priv;
    if (f->total && target >= f->total) target = f->total ? f->total - 1 : 0;

    // The SEEKTABLE's bracket, when there is one; the whole file else.
    uint64_t best = f->first_frame, best_sample = 0, hi = f->file_len;
    for (int i = 0; i < f->nseek; i++) {
        if (f->seek[i].sample <= target) {
            best = f->first_frame + f->seek[i].offset;
            best_sample = f->seek[i].sample;
        } else {
            hi = f->first_frame + f->seek[i].offset;
            break;
        }
    }
    // Bisection over frame headers, down to a window the forward decode
    // crosses cheaply.
    uint64_t lo = best;
    for (int guard = 0; guard < 48 && hi > lo + 2 * (uint64_t)INBUF; guard++) {
        uint64_t mid = lo + (hi - lo) / 2, where, smp;
        if (frame_at_or_after(f, mid, &where, &smp) < 0 || where >= hi) { hi = mid; continue; }
        if (smp <= target) { best = where; best_sample = smp; lo = where + 1; }
        else hi = mid;
    }
    seek_byte(f, best);
    f->block = f->at = 0;
    f->frame_sample = best_sample;
    f->skip = target - best_sample;
    f->done = 0;
    return 0;
}

const struct usnd_codec usnd_codec_flac = {
    .name = "flac",
    .probe = flac_probe,
    .open = flac_open,
    .read = flac_read,
    .seek = flac_seek,
    .close = flac_close,
};
