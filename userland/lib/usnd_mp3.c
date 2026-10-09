// The MP3 codec: MPEG-1 Audio Layer III.
//
// Written here for the same reason userland/lib/uimg_jpeg.c was written
// here rather than linked: a decoder is ring-3 code parsing a hostile
// file, and the kernel never sees a byte of it. What it refuses is as
// much a part of the contract as what it plays -- -ENOTSUP for a good
// file this build will not play (Layer I/II, MPEG-2/2.5, free-format,
// intensity stereo), -EINVAL for a broken one.
//
// **THE HARD PART IS THE BIT RESERVOIR, NOT THE MATHS.** A Layer III
// frame's main data does not start in that frame: `main_data_begin`
// points BACKWARDS by up to 511 bytes into data already consumed, so a
// frame cannot be decoded in isolation and seeking cannot simply jump.
// That is why this holds a reservoir and why seek() throws the first
// frames after a jump away rather than pretending they are exact.
//
// The twelve stages, in the order the standard applies them, are the
// section headings below. Nine are arithmetic with no decisions in them;
// the two worth reading before editing are the reservoir (above) and
// the region split in read_huffman(), which is where a wrong boundary
// decodes plausibly and sounds wrong.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lib/usnd.h"
#include "lib/usnd_internal.h"
#include "lib/usnd_mp3_tables.h"
#include "rt/sys.h"
#include "errno.h"

#define GRANULE      576            // spectral lines per granule
#define SBLIMIT      32             // subbands
#define SSLIMIT      18             // samples per subband per granule
#define MAX_FRAME    2048           // 1441 is the real maximum; round up
#define RESERVOIR    512            // main_data_begin is 9 bits

// --- 1. the frame header ----------------------------------------------

struct mp3_hdr {
    int version;        // 3 = MPEG-1; anything else is refused
    int layer;          // 1 = Layer III (the header's inverted numbering)
    int bitrate;        // bits per second
    int rate;           // Hz
    int sr_index;       // 0..2, indexes the scalefactor band tables
    int padding;
    int protect;        // 0 = a 16-bit CRC follows the header
    int mode;           // 3 = mono
    int mode_ext;
    int channels;
    int frame_bytes;
};

static const uint16_t BITRATE_V1L3[15] = {
    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320
};
static const uint32_t RATE_V1[3] = { 44100, 48000, 32000 };

// A header, or 0. Every field is checked here so that nothing below has
// to: a reserved bitrate index or sample rate reaching the decoder is a
// division by zero waiting to be found by a fuzzer, not a stream to
// guess at.
static int parse_header(const uint8_t *h, struct mp3_hdr *o) {
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return 0;
    o->version  = (h[1] >> 3) & 3;
    o->layer    = (h[1] >> 1) & 3;
    o->protect  = h[1] & 1;
    int br      = (h[2] >> 4) & 0x0F;
    o->sr_index = (h[2] >> 2) & 3;
    o->padding  = (h[2] >> 1) & 1;
    o->mode     = (h[3] >> 6) & 3;
    o->mode_ext = (h[3] >> 4) & 3;

    if (o->version != 3 || o->layer != 1) return 0;   // MPEG-1 Layer III only
    if (br == 0 || br == 15 || o->sr_index == 3) return 0;

    o->bitrate  = BITRATE_V1L3[br] * 1000;
    o->rate     = (int)RATE_V1[o->sr_index];
    o->channels = (o->mode == 3) ? 1 : 2;
    // 1152 samples per frame, so the byte count is 144 * rate / sr.
    o->frame_bytes = 144 * o->bitrate / o->rate + o->padding;
    return o->frame_bytes >= 24 && o->frame_bytes <= MAX_FRAME;
}

// --- 2. reading bits --------------------------------------------------

struct bits {
    const uint8_t *buf;
    int len;        // bytes
    int pos;        // bits
};

static uint32_t bget(struct bits *b, int n) {
    uint32_t v = 0;
    while (n-- > 0) {
        int byte = b->pos >> 3;
        int bit = 7 - (b->pos & 7);
        v = (v << 1) | (byte < b->len ? (uint32_t)((b->buf[byte] >> bit) & 1) : 0);
        b->pos++;
    }
    return v;
}

// --- 3. Huffman, as one shared tree pool ------------------------------
//
// Built once from the (length, codeword) tables, because a table walked
// by linear search costs 256 comparisons a symbol and this runs 44 000
// symbols a second. Node 0 is every table's spare root; a leaf is stored
// as -(index + 1) so that 0 can mean "no child".

#define TREE_NODES 4096
static int16_t g_node[TREE_NODES][2];
static int g_nodes_used;
static int16_t g_root[32];
static int16_t g_q1a_root;
static int g_tables_ready;

// A leaf is marked in its PARENT's child slot, never in the node itself.
// Storing it in the node's own children would make huff_get() descend one
// level past the codeword -- every symbol would silently eat one extra
// bit, which decodes to plausible small values rather than to an error.
static int tree_insert(int16_t root, uint16_t code, uint8_t len, int symbol) {
    if (len == 0) return -1;
    int16_t n = root;
    for (int i = len - 1; i > 0; i--) {
        int bit = (code >> i) & 1;
        if (g_node[n][bit] == 0) {
            if (g_nodes_used >= TREE_NODES) return -1;
            g_node[n][bit] = (int16_t)g_nodes_used++;
        } else if (g_node[n][bit] < 0) {
            return -1;                  // a leaf on the path: not prefix-free
        }
        n = g_node[n][bit];
    }
    int last = code & 1;
    if (g_node[n][last] != 0) return -1;        // two symbols, one codeword
    g_node[n][last] = (int16_t)(-(symbol + 1));
    return 0;
}

static int build_tables(void) {
    if (g_tables_ready) return 0;
    g_nodes_used = 1;                   // node 0 is never a root
    memset(g_node, 0, sizeof g_node);

    for (int t = 0; t < 32; t++) {
        g_root[t] = 0;
        const struct mp3_htable *h = &mp3_htables[t];
        if (!h->len) continue;
        if (t >= 17 && t <= 23) { g_root[t] = g_root[16]; continue; }
        if (t >= 25) { g_root[t] = g_root[24]; continue; }
        if (g_nodes_used >= TREE_NODES) return -1;
        g_root[t] = (int16_t)g_nodes_used++;
        for (int i = 0; i < h->dim * h->dim; i++)
            if (tree_insert(g_root[t], h->code[i], h->len[i], i) != 0) return -1;
    }
#ifdef USND_MP3_POISON
    // The positive control for tools/usnd_hostcheck.py, and it is shaped to
    // defeat the STRUCTURAL check on purpose: swapping two codewords of
    // equal length leaves the code complete and prefix-free, so
    // usnd_mp3_selftest() still passes and only the comparison against
    // ffmpeg can see it. A control the cheap check catches would prove
    // nothing about the expensive one.
    {
        int16_t save = g_root[13];
        g_root[13] = g_root[15];
        g_root[15] = save;
    }
#endif
    g_q1a_root = (int16_t)g_nodes_used++;
    for (int i = 0; i < 16; i++)
        if (tree_insert(g_q1a_root, mp3_q1a_code[i], mp3_q1a_len[i], i) != 0) return -1;

    g_tables_ready = 1;
    return 0;
}

// Every table must be a COMPLETE PREFIX CODE over exactly dim*dim pairs.
// Kraft's equality is checked in integer arithmetic against 2^20 so that
// no rounding can make a broken table look whole. Exposed because it
// needs no audio to run: a build with a mistyped table fails here rather
// than sounding subtly wrong.
int usnd_mp3_selftest(void) {
    if (build_tables() != 0) return -1;
    for (int t = 0; t < 32; t++) {
        const struct mp3_htable *h = &mp3_htables[t];
        if (!h->len) continue;
        uint32_t kraft = 0;
        for (int i = 0; i < h->dim * h->dim; i++) {
            if (h->len[i] == 0 || h->len[i] > 19) return -(t * 100 + 1);
            kraft += 1u << (20 - h->len[i]);
        }
        if (kraft != (1u << 20)) return -(t * 100 + 2);
    }
    uint32_t k = 0;
    for (int i = 0; i < 16; i++) k += 1u << (20 - mp3_q1a_len[i]);
    if (k != (1u << 20)) return -3;
    return 0;
}

static int huff_get(struct bits *b, int16_t root) {
    int16_t n = root;
    for (int guard = 0; guard < 24; guard++) {
        int byte = b->pos >> 3;
        int bit = (byte < b->len) ? ((b->buf[byte] >> (7 - (b->pos & 7))) & 1) : 0;
        b->pos++;
        n = g_node[n][bit];
        if (n < 0) return -n - 1;
        if (n == 0) return -1;
    }
    return -1;
}

// --- 4. side information ----------------------------------------------

struct granule {
    uint16_t part2_3_length;
    uint16_t big_values;
    uint8_t global_gain;
    uint8_t scalefac_compress;
    uint8_t window_switching;
    uint8_t block_type;
    uint8_t mixed_block;
    uint8_t table_select[3];
    uint8_t subblock_gain[3];
    uint8_t region0_count, region1_count;
    uint8_t preflag, scalefac_scale, count1_table;
    int scalefac_l[23];
    int scalefac_s[13][3];
};

struct mp3 {
    struct mp3_hdr hdr;
    long first_frame_off;       // past any ID3v2 tag
    uint64_t frames;            // audio frames, from Xing or estimated
    int have_xing;

    uint8_t res[RESERVOIR + MAX_FRAME];
    int res_len;
    int primed;                 // a granule has been decoded since the last seek

    uint8_t frame[MAX_FRAME];

    struct granule gr[2][2];
    uint8_t scfsi[2][4];

    float xr[2][GRANULE];
    float overlap[2][GRANULE];
    struct usnd_mpsynth synth[2];   // the filterbank's history, per channel

    int32_t out[2 * GRANULE * 2];   // two granules, interleaved, s32
    int out_frames, out_pos;
    int eof;
};

// --- 5. derived constants, computed rather than tabled -----------------

static float g_imdct_long[36][18];
static float g_imdct_short[12][6];
static float g_win[4][36];
static float g_cs[8], g_ca[8];
static float g_is_ratio[7][2];
static int g_consts_ready;

static void build_consts(void) {
    if (g_consts_ready) return;
    for (int i = 0; i < 36; i++)
        for (int k = 0; k < 18; k++)
            g_imdct_long[i][k] =
                (float)cos(M_PI / 72.0 * (2 * i + 1 + 18) * (2 * k + 1));
    for (int i = 0; i < 12; i++)
        for (int k = 0; k < 6; k++)
            g_imdct_short[i][k] =
                (float)cos(M_PI / 24.0 * (2 * i + 1 + 6) * (2 * k + 1));

    // The four block windows. 0 normal, 1 start, 2 short, 3 stop -- the
    // header's own numbering, so `block_type` indexes this directly.
    for (int i = 0; i < 36; i++) g_win[0][i] = (float)sin(M_PI / 36.0 * (i + 0.5));
    for (int i = 0; i < 18; i++) g_win[1][i] = (float)sin(M_PI / 36.0 * (i + 0.5));
    for (int i = 18; i < 24; i++) g_win[1][i] = 1.0f;
    for (int i = 24; i < 30; i++) g_win[1][i] = (float)sin(M_PI / 12.0 * (i - 18 + 0.5));
    for (int i = 30; i < 36; i++) g_win[1][i] = 0.0f;
    for (int i = 0; i < 12; i++) g_win[2][i] = (float)sin(M_PI / 12.0 * (i + 0.5));
    for (int i = 12; i < 36; i++) g_win[2][i] = 0.0f;
    for (int i = 0; i < 6; i++) g_win[3][i] = 0.0f;
    for (int i = 6; i < 12; i++) g_win[3][i] = (float)sin(M_PI / 12.0 * (i - 6 + 0.5));
    for (int i = 12; i < 18; i++) g_win[3][i] = 1.0f;
    for (int i = 18; i < 36; i++) g_win[3][i] = (float)sin(M_PI / 36.0 * (i + 0.5));

    // Alias reduction, derived from the standard's ci[] rather than
    // carried as a second table -- cs and ca are functions of it.
    static const double ci[8] = { -0.6, -0.535, -0.33, -0.185,
                                  -0.095, -0.041, -0.0142, -0.0037 };
    for (int i = 0; i < 8; i++) {
        double d = sqrt(1.0 + ci[i] * ci[i]);
        g_cs[i] = (float)(1.0 / d);
        g_ca[i] = (float)(ci[i] / d);
    }
    // Intensity positions: tan(i*pi/12), as a (left, right) gain pair.
    for (int i = 0; i < 7; i++) {
        double t = tan(i * M_PI / 12.0);
        g_is_ratio[i][0] = (float)(t / (1.0 + t));
        g_is_ratio[i][1] = (float)(1.0 / (1.0 + t));
    }
    g_consts_ready = 1;
}

// 2^(x/4), for the requantiser's gain terms. Split into an integer
// power and one of four fractional steps so that no pow() call lands in
// the per-sample path.
static float exp2_quarter(int q) {
    static const double frac[4] = { 1.0, 1.1892071150027210, 1.4142135623730951,
                                    1.6817928305074290 };
    int i = q >> 2, f = q & 3;
    if (i < -60) return 0.0f;
    if (i > 60) i = 60;
    return (float)(ldexp(frac[f], i));
}

static float pow43(int v) {
    static float small[16];
    static int ready;
    if (!ready) {
        for (int i = 0; i < 16; i++) small[i] = (float)pow((double)i, 4.0 / 3.0);
        ready = 1;
    }
    if (v < 16) return small[v];
    return (float)(v * cbrt((double)v));
}

// --- 6. scalefactors ---------------------------------------------------

static const uint8_t SLEN[16][2] = {
    {0,0},{0,1},{0,2},{0,3},{3,0},{1,1},{1,2},{1,3},
    {2,1},{2,2},{2,3},{3,1},{3,2},{3,3},{4,2},{4,3}
};

static void read_scalefactors(struct mp3 *m, struct bits *b, int gr, int ch) {
    struct granule *g = &m->gr[gr][ch];
    int slen1 = SLEN[g->scalefac_compress][0];
    int slen2 = SLEN[g->scalefac_compress][1];

    if (g->window_switching && g->block_type == 2) {
        if (g->mixed_block) {
            for (int sfb = 0; sfb < 8; sfb++) g->scalefac_l[sfb] = (int)bget(b, slen1);
            for (int sfb = 3; sfb < 6; sfb++)
                for (int w = 0; w < 3; w++) g->scalefac_s[sfb][w] = (int)bget(b, slen1);
            for (int sfb = 6; sfb < 12; sfb++)
                for (int w = 0; w < 3; w++) g->scalefac_s[sfb][w] = (int)bget(b, slen2);
        } else {
            for (int sfb = 0; sfb < 6; sfb++)
                for (int w = 0; w < 3; w++) g->scalefac_s[sfb][w] = (int)bget(b, slen1);
            for (int sfb = 6; sfb < 12; sfb++)
                for (int w = 0; w < 3; w++) g->scalefac_s[sfb][w] = (int)bget(b, slen2);
        }
        for (int w = 0; w < 3; w++) g->scalefac_s[12][w] = 0;
        return;
    }

    // Long blocks, where granule 1 may INHERIT a band group from granule
    // 0 rather than resending it -- that is what scfsi is, and reading it
    // as "always present" desynchronises every later field in the granule.
    static const int SCFSI_LO[4] = { 0, 6, 11, 16 };
    static const int SCFSI_HI[4] = { 6, 11, 16, 21 };
    for (int band = 0; band < 4; band++) {
        int slen = (band < 2) ? slen1 : slen2;
        if (gr == 1 && m->scfsi[ch][band]) {
            for (int sfb = SCFSI_LO[band]; sfb < SCFSI_HI[band]; sfb++)
                g->scalefac_l[sfb] = m->gr[0][ch].scalefac_l[sfb];
        } else {
            for (int sfb = SCFSI_LO[band]; sfb < SCFSI_HI[band]; sfb++)
                g->scalefac_l[sfb] = (int)bget(b, slen);
        }
    }
    g->scalefac_l[21] = g->scalefac_l[22] = 0;
}

// --- 7. the Huffman-coded spectrum ------------------------------------

static int read_huffman(struct mp3 *m, struct bits *b, int gr, int ch,
                        int part2_start, int is[GRANULE]) {
    struct granule *g = &m->gr[gr][ch];
    const uint16_t *sfbl = mp3_sfb_long[m->hdr.sr_index];
    int limit = part2_start + g->part2_3_length;

    memset(is, 0, sizeof(int) * GRANULE);

    // THE REGION SPLIT. With window switching the counts are not sent and
    // the boundaries are fixed by the standard instead; a decoder that
    // uses the (absent, therefore zero) counts here still produces
    // plausible audio, just wrong -- which is why this is spelled out.
    int r1, r2;
    if (g->window_switching) {
        r1 = (g->block_type == 2) ? 36 : sfbl[8];
        r2 = GRANULE;
    } else {
        int a = g->region0_count + 1;
        int c = g->region0_count + g->region1_count + 2;
        if (a > 22) a = 22;
        if (c > 22) c = 22;
        r1 = sfbl[a];
        r2 = sfbl[c];
    }
    int big = g->big_values * 2;
    if (big > GRANULE) big = GRANULE;
    if (r1 > big) r1 = big;
    if (r2 > big) r2 = big;

    int i = 0;
    for (int region = 0; region < 3; region++) {
        int end = (region == 0) ? r1 : (region == 1) ? r2 : big;
        int tsel = g->table_select[region];
        const struct mp3_htable *h = &mp3_htables[tsel];
        while (i < end) {
            if (!h->len) { is[i++] = 0; is[i++] = 0; continue; }
            int sym = huff_get(b, g_root[tsel]);
            if (sym < 0) return -EINVAL;
            int x = sym / h->dim, y = sym % h->dim;
            if (x == 15 && h->linbits) x += (int)bget(b, h->linbits);
            if (x && bget(b, 1)) x = -x;
            if (y == 15 && h->linbits) y += (int)bget(b, h->linbits);
            if (y && bget(b, 1)) y = -y;
            is[i++] = x;
            is[i++] = y;
        }
    }

    // The count1 region runs until the granule's bits are spent, so it is
    // bounded by the BIT COUNT rather than by a length -- and overrunning
    // it by one quadruple is how a decoder eats the next granule's data.
    while (b->pos < limit && i <= GRANULE - 4) {
        int sym;
        if (g->count1_table) {
            sym = (int)bget(b, 4) ^ 15;     // table B: a flat 4-bit code
        } else {
            sym = huff_get(b, g_q1a_root);
            if (sym < 0) break;
        }
        for (int k = 0; k < 4; k++) {
            int v = (sym >> (3 - k)) & 1;
            if (v) v = bget(b, 1) ? -1 : 1;
            is[i++] = v;
        }
    }
    if (b->pos > limit) {
        // Overrun means a table or a length was misread. Rewinding to the
        // granule boundary keeps the RESERVOIR consistent so the next
        // frame still decodes; the damaged granule is what it is.
        b->pos = limit;
        return -EINVAL;
    }
    b->pos = limit;
    return 0;
}

// --- 8. requantisation -------------------------------------------------

static void requantize(struct mp3 *m, int gr, int ch, const int is[GRANULE]) {
    struct granule *g = &m->gr[gr][ch];
    const uint16_t *sfbl = mp3_sfb_long[m->hdr.sr_index];
    const uint16_t *sfbs = mp3_sfb_short[m->hdr.sr_index];
    float *xr = m->xr[ch];
    int shift = g->scalefac_scale ? 2 : 1;      // the exponent is in quarters

    int long_end = GRANULE;
    if (g->window_switching && g->block_type == 2)
        long_end = g->mixed_block ? sfbl[8] : 0;

    int sfb = 0;
    for (int i = 0; i < long_end; i++) {
        while (sfb < 21 && (int)sfbl[sfb + 1] <= i) sfb++;
        int sf = g->scalefac_l[sfb] + (g->preflag ? mp3_pretab[sfb] : 0);
        int q = g->global_gain - 210 - shift * 2 * sf;
        int v = is[i];
        float a = pow43(v < 0 ? -v : v) * exp2_quarter(q);
        xr[i] = (v < 0) ? -a : a;
    }

    if (long_end < GRANULE) {
        // Short blocks: three windows share a band, each with its own
        // subblock_gain, and the values are still in bitstream order --
        // reorder() below is what puts them where the IMDCT expects.
        for (int i = long_end; i < GRANULE; i++) xr[i] = 0.0f;
        int start_sfb = g->mixed_block ? 3 : 0;
        for (int s = start_sfb; s < 13; s++) {
            int lo = sfbs[s], hi = sfbs[s + 1], width = hi - lo;
            for (int w = 0; w < 3; w++) {
                int q = g->global_gain - 210 - 8 * g->subblock_gain[w]
                        - shift * 2 * g->scalefac_s[s][w];
                float gain = exp2_quarter(q);
                for (int k = 0; k < width; k++) {
                    int idx = lo * 3 + w * width + k;
                    if (idx >= GRANULE) break;
                    int v = is[idx];
                    float a = pow43(v < 0 ? -v : v) * gain;
                    xr[idx] = (v < 0) ? -a : a;
                }
            }
        }
    }
}

// --- 9. reordering, alias reduction ------------------------------------

static void reorder(struct mp3 *m, int ch, const struct granule *g) {
    if (!(g->window_switching && g->block_type == 2)) return;
    const uint16_t *sfbs = mp3_sfb_short[m->hdr.sr_index];
    static float tmp[GRANULE];
    float *xr = m->xr[ch];
    int start_sfb = g->mixed_block ? 3 : 0;
    int from = g->mixed_block ? sfbs[3] * 3 : 0;

    memcpy(tmp, xr, sizeof tmp);
    for (int s = start_sfb; s < 13; s++) {
        int lo = sfbs[s], hi = sfbs[s + 1], width = hi - lo;
        for (int w = 0; w < 3; w++) {
            for (int k = 0; k < width; k++) {
                int f = lo + k;                         // frequency line
                int src = lo * 3 + w * width + k;       // bitstream order
                int dst = (f / 6) * 18 + w * 6 + (f % 6);
                if (src < GRANULE && dst < GRANULE && dst >= from)
                    xr[dst] = tmp[src];
            }
        }
    }
}

static void antialias(struct mp3 *m, int ch, const struct granule *g) {
    float *xr = m->xr[ch];
    int subbands = SBLIMIT - 1;
    if (g->window_switching && g->block_type == 2)
        subbands = g->mixed_block ? 1 : 0;      // only the long part aliases
    for (int sb = 0; sb < subbands; sb++) {
        float *p = xr + sb * 18 + 18;
        for (int i = 0; i < 8; i++) {
            float a = p[-1 - i], b = p[i];
            p[-1 - i] = a * g_cs[i] - b * g_ca[i];
            p[i] = b * g_cs[i] + a * g_ca[i];
        }
    }
}

// --- 10. stereo --------------------------------------------------------

static void stereo(struct mp3 *m) {
    if (m->hdr.channels != 2 || m->hdr.mode != 1) return;
    if (!(m->hdr.mode_ext & 2)) return;
    // Mid/side: the pair carries sum and difference, scaled so that the
    // transform is its own inverse.
    const float inv = 0.70710678f;
    for (int i = 0; i < GRANULE; i++) {
        float mid = m->xr[0][i], side = m->xr[1][i];
        m->xr[0][i] = (mid + side) * inv;
        m->xr[1][i] = (mid - side) * inv;
    }
}

// --- 11. IMDCT and the overlap ----------------------------------------

static void imdct(struct mp3 *m, int ch, const struct granule *g, float *sb) {
    float *xr = m->xr[ch];
    float *ov = m->overlap[ch];
    int short_start = SBLIMIT;
    if (g->window_switching && g->block_type == 2)
        short_start = g->mixed_block ? 2 : 0;

    for (int s = 0; s < SBLIMIT; s++) {
        float raw[36];
        const float *in = xr + s * 18;
        if (s >= short_start) {
            for (int i = 0; i < 36; i++) raw[i] = 0.0f;
            for (int w = 0; w < 3; w++) {
                float win[12];
                for (int i = 0; i < 12; i++) {
                    float acc = 0.0f;
                    for (int k = 0; k < 6; k++) acc += in[w * 6 + k] * g_imdct_short[i][k];
                    win[i] = acc * g_win[2][i];
                }
                for (int i = 0; i < 12; i++) raw[6 + w * 6 + i] += win[i];
            }
        } else {
            int bt = g->window_switching ? g->block_type : 0;
            for (int i = 0; i < 36; i++) {
                float acc = 0.0f;
                for (int k = 0; k < 18; k++) acc += in[k] * g_imdct_long[i][k];
                raw[i] = acc * g_win[bt][i];
            }
        }
        // Overlap-add, then keep the tail for the next granule. This is
        // the only state the IMDCT carries, and it is why a seek must
        // clear it rather than resume mid-window.
        for (int i = 0; i < 18; i++) {
            float v = raw[i] + ov[s * 18 + i];
            ov[s * 18 + i] = raw[18 + i];
            // Frequency inversion: every other subband comes out of the
            // filterbank mirrored, and odd samples of odd subbands undo it.
            sb[i * SBLIMIT + s] = (s & 1) && (i & 1) ? -v : v;
        }
    }
}

// --- 12. the polyphase synthesis filterbank ----------------------------
//
// Shared with Layer II: usnd_mpsynth.c.

// --- putting a frame through all twelve --------------------------------

static int read_side_info(struct mp3 *m, const uint8_t *p, int nch, int *main_begin) {
    struct bits b = { p, nch == 1 ? 17 : 32, 0 };
    *main_begin = (int)bget(&b, 9);
    bget(&b, nch == 1 ? 5 : 3);
    for (int ch = 0; ch < nch; ch++)
        for (int i = 0; i < 4; i++) m->scfsi[ch][i] = (uint8_t)bget(&b, 1);

    for (int gr = 0; gr < 2; gr++) {
        for (int ch = 0; ch < nch; ch++) {
            struct granule *g = &m->gr[gr][ch];
            memset(g->scalefac_l, 0, sizeof g->scalefac_l);
            memset(g->scalefac_s, 0, sizeof g->scalefac_s);
            g->part2_3_length = (uint16_t)bget(&b, 12);
            g->big_values = (uint16_t)bget(&b, 9);
            g->global_gain = (uint8_t)bget(&b, 8);
            g->scalefac_compress = (uint8_t)bget(&b, 4);
            g->window_switching = (uint8_t)bget(&b, 1);
            g->mixed_block = 0;
            g->subblock_gain[0] = g->subblock_gain[1] = g->subblock_gain[2] = 0;
            if (g->window_switching) {
                g->block_type = (uint8_t)bget(&b, 2);
                g->mixed_block = (uint8_t)bget(&b, 1);
                for (int i = 0; i < 2; i++) g->table_select[i] = (uint8_t)bget(&b, 5);
                g->table_select[2] = 0;
                for (int i = 0; i < 3; i++) g->subblock_gain[i] = (uint8_t)bget(&b, 3);
                g->region0_count = 0;
                g->region1_count = 0;
                if (g->block_type == 0) return -EINVAL;   // reserved
            } else {
                g->block_type = 0;
                for (int i = 0; i < 3; i++) g->table_select[i] = (uint8_t)bget(&b, 5);
                g->region0_count = (uint8_t)bget(&b, 4);
                g->region1_count = (uint8_t)bget(&b, 3);
            }
            g->preflag = (uint8_t)bget(&b, 1);
            g->scalefac_scale = (uint8_t)bget(&b, 1);
            g->count1_table = (uint8_t)bget(&b, 1);
        }
    }
    return 0;
}

// Decodes both granules of the frame whose main data now sits at the end
// of the reservoir. Returns frames produced (1152) or a negative errno.
static int decode_frame(struct mp3 *m, const uint8_t *frame, int nbytes) {
    int nch = m->hdr.channels;
    int side = (nch == 1) ? 17 : 32;
    int off = 4 + (m->hdr.protect ? 0 : 2);
    if (nbytes < off + side) return -EINVAL;

    int main_begin = 0;
    if (read_side_info(m, frame + off, nch, &main_begin) != 0) return -EINVAL;

    int main_len = nbytes - off - side;
    if (main_len < 0) return -EINVAL;

    // THE RESERVOIR. main_begin counts backwards from where this frame's
    // own data starts, into bytes already consumed -- so the append has
    // to happen before the decode, and a frame whose pointer reaches
    // further back than we have kept is simply not decodable yet.
    if (main_begin > m->res_len) {
        if (m->res_len + main_len > (int)sizeof m->res) m->res_len = 0;
        memcpy(m->res + m->res_len, frame + off + side, (size_t)main_len);
        m->res_len += main_len;
        if (m->res_len > RESERVOIR) {
            memmove(m->res, m->res + m->res_len - RESERVOIR, RESERVOIR);
            m->res_len = RESERVOIR;
        }
        return 0;               // no output: the history is not there yet
    }

    int start = m->res_len - main_begin;
    if (m->res_len + main_len > (int)sizeof m->res) {
        memmove(m->res, m->res + m->res_len - main_begin, (size_t)main_begin);
        m->res_len = main_begin;
        start = 0;
    }
    memcpy(m->res + m->res_len, frame + off + side, (size_t)main_len);
    m->res_len += main_len;

    struct bits b = { m->res, m->res_len, start * 8 };
    int bad = 0;
    for (int gr = 0; gr < 2; gr++) {
        for (int ch = 0; ch < nch; ch++) {
            static int is[GRANULE];
            int part2_start = b.pos;
            read_scalefactors(m, &b, gr, ch);
            if (read_huffman(m, &b, gr, ch, part2_start, is) != 0) bad = 1;
            requantize(m, gr, ch, is);
        }
        stereo(m);
        for (int ch = 0; ch < nch; ch++) {
            static float sb[GRANULE];
            reorder(m, ch, &m->gr[gr][ch]);
            antialias(m, ch, &m->gr[gr][ch]);
            imdct(m, ch, &m->gr[gr][ch], sb);
            usnd_mpsynth_run(&m->synth[ch], sb, SSLIMIT, m->out + gr * GRANULE * nch + ch, nch);
        }
    }

    // Keep only what a later frame can point back into.
    if (m->res_len > RESERVOIR) {
        memmove(m->res, m->res + m->res_len - RESERVOIR, RESERVOIR);
        m->res_len = RESERVOIR;
    }
    (void)bad;
    return 2 * GRANULE;
}

// --- finding frames in the file ---------------------------------------

static long skip_id3(int fd) {
    uint8_t t[10];
    if (sys_lseek(fd, 0, SYS_SEEK_SET) < 0) return 0;
    if (sys_read(fd, t, 10) != 10) return 0;
    if (memcmp(t, "ID3", 3) != 0) return 0;
    // A syncsafe size: seven bits per byte, so no byte can look like a
    // sync word. Reading it as a plain 32-bit integer is a classic way
    // to land in the middle of the audio.
    long size = ((long)(t[6] & 0x7F) << 21) | ((long)(t[7] & 0x7F) << 14) |
                ((long)(t[8] & 0x7F) << 7) | (long)(t[9] & 0x7F);
    if (t[5] & 0x10) size += 10;        // a footer, if the flags say so
    return 10 + size;
}

// The next frame at or after `off`. Resyncs by scanning, because a tag
// this build does not know is indistinguishable from garbage and both
// are recoverable the same way.
static long find_frame(struct mp3 *m, int fd, long off, struct mp3_hdr *h) {
    uint8_t win[4];
    for (long scan = 0; scan < 1 << 18; scan++) {
        if (sys_lseek(fd, off, SYS_SEEK_SET) < 0) return -1;
        if (sys_read(fd, win, 4) != 4) return -1;
        if (parse_header(win, h)) return off;
        off++;
        (void)m;
    }
    return -1;
}

// Xing/Info: the frame count a VBR file carries, so duration is a fact
// rather than an estimate from the first frame's bitrate.
static void read_xing(struct mp3 *m, int fd, long frame_off, const struct mp3_hdr *h) {
    uint8_t buf[192];
    int side = (h->channels == 1) ? 17 : 32;
    long off = frame_off + 4 + (h->protect ? 0 : 2) + side;
    if (sys_lseek(fd, off, SYS_SEEK_SET) < 0) return;
    if (sys_read(fd, buf, sizeof buf) != (long long)sizeof buf) return;
    if (memcmp(buf, "Xing", 4) != 0 && memcmp(buf, "Info", 4) != 0) return;
    uint32_t flags = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                     ((uint32_t)buf[6] << 8) | buf[7];
    if (!(flags & 1)) return;
    uint32_t nframes = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) |
                       ((uint32_t)buf[10] << 8) | buf[11];
    m->frames = (uint64_t)nframes * 1152;
    m->have_xing = 1;
}

// --- the codec ---------------------------------------------------------

static int mp3_probe(const uint8_t *d, size_t n) {
    if (n >= 3 && memcmp(d, "ID3", 3) == 0) return 1;
    if (n < 4) return 0;
    struct mp3_hdr h;
    return parse_header(d, &h);
}

static int mp3_open(struct usnd_stream *s) {
    build_consts();
    if (build_tables() != 0) { usnd_fail("MP3 tables did not build"); return -ENOMEM; }

    struct mp3 *m = calloc(1, sizeof *m);
    if (!m) { usnd_fail("out of memory for the MP3 decoder"); return -ENOMEM; }

    long off = skip_id3(s->fd);
    struct mp3_hdr h;
    long first = find_frame(m, s->fd, off, &h);
    if (first < 0) {
        free(m);
        // Everything parse_header() rejects lands here, so the message
        // has to cover both "not an MP3" and "an MP3 this build refuses".
        usnd_fail("no MPEG-1 Layer III frame found (Layer I/II and "
                  "MPEG-2/2.5 are not supported)");
        return -ENOTSUP;
    }
    if (h.channels == 2 && h.mode == 1 && (h.mode_ext & 1)) {
        free(m);
        usnd_fail("intensity stereo is not supported");
        return -ENOTSUP;
    }

    m->hdr = h;
    m->first_frame_off = first;
    read_xing(m, s->fd, first, &h);
    if (m->have_xing) {
        // The Xing frame is a real frame carrying no audio. Stepping past
        // it is what stops a click at the start of every VBR file.
        m->first_frame_off = first + h.frame_bytes;
    } else {
        long size = (long)sys_lseek(s->fd, 0, SYS_SEEK_END);
        m->frames = (size > first && h.bitrate)
                    ? (uint64_t)((size - first) * 8 / (h.bitrate / h.rate ? 1 : 1)) : 0;
        m->frames = (size > first)
                    ? (uint64_t)(((double)(size - first) * 8.0 / h.bitrate) * h.rate)
                    : 0;
    }
    sys_lseek(s->fd, m->first_frame_off, SYS_SEEK_SET);

    s->priv = m;
    s->fmt.rate = (uint32_t)h.rate;
    s->fmt.channels = (uint16_t)h.channels;
    s->fmt.bits = 16;
    s->frames = m->frames;
    snprintf(s->detail, sizeof s->detail, "MPEG-1 Layer III %d kbps %s",
             h.bitrate / 1000,
             h.channels == 1 ? "mono" : (h.mode == 1 ? "joint stereo" : "stereo"));
    return 0;
}

// One frame's worth of PCM into m->out. 0 at end of file.
static int fill(struct mp3 *m, int fd) {
    for (;;) {
        long pos = (long)sys_lseek(fd, 0, SYS_SEEK_CUR);
        uint8_t hb[4];
        if (sys_read(fd, hb, 4) != 4) return 0;
        struct mp3_hdr h;
        if (!parse_header(hb, &h)) {
            long next = find_frame(m, fd, pos + 1, &h);
            if (next < 0) return 0;
            sys_lseek(fd, next, SYS_SEEK_SET);
            if (sys_read(fd, hb, 4) != 4) return 0;
        }
        if (h.channels != m->hdr.channels || h.rate != m->hdr.rate) return 0;
        m->hdr = h;

        if (h.frame_bytes > (int)sizeof m->frame) return 0;
        memcpy(m->frame, hb, 4);
        int rest = h.frame_bytes - 4;
        if (sys_read(fd, m->frame + 4, (unsigned)rest) != rest) return 0;

        int n = decode_frame(m, m->frame, h.frame_bytes);
        if (n > 0) {
            m->out_frames = n;
            m->out_pos = 0;
            return n;
        }
        // n == 0 means the reservoir was not primed yet, which is normal
        // for the first frame and after a seek: read on.
    }
}

static long mp3_read(struct usnd_stream *s, int32_t *dst, long frames) {
    struct mp3 *m = s->priv;
    int nch = m->hdr.channels;
    long done = 0;
    while (done < frames) {
        if (m->out_pos >= m->out_frames) {
            if (!fill(m, s->fd)) break;
        }
        long avail = m->out_frames - m->out_pos;
        long take = (frames - done < avail) ? frames - done : avail;
        memcpy(dst + done * nch, m->out + (long)m->out_pos * nch,
               (size_t)take * nch * sizeof(int32_t));
        m->out_pos += (int)take;
        done += take;
    }
    return done;
}

// Seeking an MP3 is APPROXIMATE and says so. Without a frame index the
// only way to land exactly is to decode from the start; what this does
// is jump by the average frame size and let the reservoir refill, which
// is what every player does with a file that has no seek table.
static int mp3_seek(struct usnd_stream *s, uint64_t frame) {
    struct mp3 *m = s->priv;
    long size = (long)sys_lseek(s->fd, 0, SYS_SEEK_END);
    long span = size - m->first_frame_off;
    if (span <= 0 || !m->frames) return -ENOTSUP;

    double f = (double)frame / (double)m->frames;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    long want = m->first_frame_off + (long)(f * span);

    struct mp3_hdr h;
    long at = find_frame(m, s->fd, want, &h);
    if (at < 0) return -EIO;
    sys_lseek(s->fd, at, SYS_SEEK_SET);

    // Everything carried across frames is now wrong for this position.
    m->res_len = 0;
    m->out_frames = m->out_pos = 0;
    memset(m->overlap, 0, sizeof m->overlap);
    usnd_mpsynth_reset(&m->synth[0]);
    usnd_mpsynth_reset(&m->synth[1]);
    return 0;
}

static void mp3_close(struct usnd_stream *s) {
    free(s->priv);
    s->priv = 0;
}

const struct usnd_codec usnd_codec_mp3 = {
    .name  = "mp3",
    .probe = mp3_probe,
    .open  = mp3_open,
    .read  = mp3_read,
    .seek  = mp3_seek,
    .close = mp3_close,
};
