// Tight (RFB 7.7.6, TightVNC's): what a VNC viewer on a slow link asks
// for. Each rectangle is the cheapest of a fill (one colour), a palette
// (up to 16, one bit a pixel for two), JPEG (many colours, and only when
// the viewer sent a quality level -- TightVNC's rule, so a viewer that
// never asked for loss gets none) or the pixels through zlib.
//
// THREE-BYTE PIXELS ("TPIXEL", R G B) need a 24-bit true-colour format;
// rd_enc_rect() sends any other format as the viewer's next choice.
#include "remoted/rd.h"
#include "lib/uinflate.h"
#include "lib/uimg.h"
#include <stdlib.h>
#include <string.h>

// libvncclient decodes a rectangle into fixed buffers: wider than 2048
// or more than 64K pixels and it gives up, so larger ones are cut.
#define TIGHT_MAX_W    2048
#define TIGHT_MAX_PX   65536
#define TIGHT_MIN_ZLIB 12      // less data than this is sent as it is (the spec)
#define TIGHT_PAL_MAX  16
#define TIGHT_JPEG_MIN 1024    // pixels: a smaller rect saves nothing as JPEG
#define TIGHT_TILE     64      // what is classed: one colour, a palette, many

// The viewer's quality level (0-9) as a JPEG quality: TigerVNC's table,
// so the same setting looks the same against either server.
static const uint8_t JPEG_QUALITY[10] = { 15, 29, 41, 42, 62, 77, 79, 86, 92, 100 };

int rd_tight_ok(const struct rd_pixfmt *pf) {
    return pf->true_colour && pf->bpp == 32 && pf->depth == 24 &&
           pf->rmax == 255 && pf->gmax == 255 && pf->bmax == 255;
}

static void compact_len(struct rd_buf *b, size_t n) {
    rd_buf_u8(b, (uint8_t)((n & 0x7F) | (n > 0x7F ? 0x80 : 0)));
    if (n > 0x7F) {
        rd_buf_u8(b, (uint8_t)(((n >> 7) & 0x7F) | (n > 0x3FFF ? 0x80 : 0)));
        if (n > 0x3FFF) rd_buf_u8(b, (uint8_t)(n >> 14));
    }
}

static void tpixel(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)v;
}

// The data of a basic rectangle: as it is when short, else through the
// stream's zlib (one per stream id, kept for the session, as the viewer
// keeps its four inflaters).
static void put_data(struct rd_enc *e, int stream, const uint8_t *d, size_t n,
                     struct rd_buf *out) {
    if (n < TIGHT_MIN_ZLIB) { rd_buf_put(out, d, n); return; }
    size_t cap = udeflate_bound(n) + 16, zlen = 0;
    uint8_t *z = malloc(cap);
    if (!z) { out->fail = 1; return; }
    struct udeflate_stream st = { e->tight_started[stream] };
    if (udeflate_sync(&st, d, n, z, cap, &zlen) != 0) {
        free(z);
        out->fail = 1;
        return;
    }
    e->tight_started[stream] = st.started;
    compact_len(out, zlen);
    rd_buf_put(out, z, zlen);
    free(z);
}

static int pal_find(const uint32_t *pal, int n, uint32_t v) {
    for (int i = 0; i < n; i++) if (pal[i] == v) return i;
    return -1;
}

static void tight_one(struct rd_enc *e, const uint32_t *px, int stride, int x, int y,
                      int w, int h, struct rd_buf *out) {
    static uint32_t rgb[TIGHT_MAX_PX];
    static uint8_t data[TIGHT_MAX_PX * 3];
    uint32_t pal[TIGHT_PAL_MAX];
    int np = 0;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint32_t v = px[(size_t)(y + j) * stride + x + i] & 0xFFFFFF;
            rgb[j * w + i] = v;
            if (np <= TIGHT_PAL_MAX && pal_find(pal, np < TIGHT_PAL_MAX ? np : TIGHT_PAL_MAX, v) < 0) {
                if (np < TIGHT_PAL_MAX) pal[np] = v;
                np++;
            }
        }
    rd_rect_header(out, x, y, w, h, RD_ENC_TIGHT);

    if (np == 1) {                                       // fill
        rd_buf_u8(out, 0x80);
        tpixel(data, pal[0]);
        rd_buf_put(out, data, 3);
        return;
    }
    if (np <= TIGHT_PAL_MAX) {                           // palette
        int mono = np == 2, stream = mono ? 1 : 2;
        rd_buf_u8(out, (uint8_t)((stream | 4) << 4));   // the filter byte follows
        rd_buf_u8(out, 1);
        rd_buf_u8(out, (uint8_t)(np - 1));
        for (int i = 0; i < np; i++) {
            tpixel(data, pal[i]);
            rd_buf_put(out, data, 3);
        }
        size_t n = 0;
        if (mono) {
            for (int j = 0; j < h; j++) {
                uint8_t acc = 0;
                int nb = 0;
                for (int i = 0; i < w; i++) {
                    acc = (uint8_t)(acc << 1 | (rgb[j * w + i] == pal[1]));
                    if (++nb == 8) { data[n++] = acc; acc = 0; nb = 0; }
                }
                if (nb) data[n++] = (uint8_t)(acc << (8 - nb));   // rows are byte-padded
            }
        } else {
            for (int i = 0; i < w * h; i++) data[n++] = (uint8_t)pal_find(pal, np, rgb[i]);
        }
        put_data(e, stream, data, n, out);
        return;
    }
    if (e->jpeg_level >= 0 && w * h >= TIGHT_JPEG_MIN) {  // JPEG
        struct uimg im = { w, h, rgb, 0 };
        uint8_t *j = NULL;
        size_t jl = 0;
        if (uimg_encode_jpeg(&im, JPEG_QUALITY[e->jpeg_level], &j, &jl) == 0) {
            rd_buf_u8(out, 0x90);
            compact_len(out, jl);
            rd_buf_put(out, j, jl);
            free(j);
            return;
        }
        free(j);   // and sent losslessly instead
    }
    rd_buf_u8(out, 0x00);                                // copy, stream 0
    for (int i = 0; i < w * h; i++) tpixel(data + i * 3, rgb[i]);
    put_data(e, 0, data, (size_t)w * h * 3, out);
}

// A rectangle within Tight's limits, cut into as many as it needs.
static int tight_cut(struct rd_enc *e, const uint32_t *px, int stride, int x, int y,
                     int w, int h, struct rd_buf *out) {
    int n = 0;
    for (int cx = 0; cx < w; cx += TIGHT_MAX_W) {
        int cw = w - cx < TIGHT_MAX_W ? w - cx : TIGHT_MAX_W;
        int rows = TIGHT_MAX_PX / cw;
        for (int cy = 0; cy < h; cy += rows) {
            int ch = h - cy < rows ? h - cy : rows;
            tight_one(e, px, stride, x + cx, y + cy, cw, ch, out);
            n++;
        }
    }
    return n;
}

enum { T_SOLID, T_PALETTE, T_MANY };

static int classify(const uint32_t *px, int stride, int x, int y, int w, int h, uint32_t *solid) {
    uint32_t pal[TIGHT_PAL_MAX];
    int np = 0;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint32_t v = px[(size_t)(y + j) * stride + x + i] & 0xFFFFFF;
            if (pal_find(pal, np, v) >= 0) continue;
            if (np == TIGHT_PAL_MAX) return T_MANY;
            pal[np++] = v;
        }
    *solid = pal[0];
    return np == 1 ? T_SOLID : T_PALETTE;
}

// BY TILE, NOT BY THE RECTANGLE ASKED FOR: the session hands over whole
// rows of changed tiles, and a row is never one colour and nearly always
// more than 16 -- so it went out as zlib, or as JPEG with its text
// blurred. Each 64-pixel tile is classed instead, and neighbours of a
// class go together: a solid run is one fill, a many-coloured run one
// JPEG (whose tables cost ~600 bytes, too much to pay per tile), and a
// palette tile goes alone, since its palette is its own.
int rd_enc_tight(struct rd_enc *e, const uint32_t *px, int stride, int x, int y,
                 int w, int h, struct rd_buf *out) {
    int n = 0;
    for (int ty = 0; ty < h; ty += TIGHT_TILE) {
        int th = h - ty < TIGHT_TILE ? h - ty : TIGHT_TILE;
        for (int tx = 0; tx < w;) {
            int tw = w - tx < TIGHT_TILE ? w - tx : TIGHT_TILE;
            uint32_t c, c2;
            int kind = classify(px, stride, x + tx, y + ty, tw, th, &c);
            int end = tx + tw;
            while (kind != T_PALETTE && end < w && (end - tx + TIGHT_TILE) * th <= TIGHT_MAX_PX) {
                int nw = w - end < TIGHT_TILE ? w - end : TIGHT_TILE;
                if (classify(px, stride, x + end, y + ty, nw, th, &c2) != kind ||
                    (kind == T_SOLID && c2 != c))
                    break;
                end += nw;
            }
            n += tight_cut(e, px, stride, x + tx, y + ty, end - tx, th, out);
            tx = end;
        }
    }
    return n;
}
