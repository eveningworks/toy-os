// GIF -- 87a and 89a, every frame, read.
//
// uimg_decode() is frame 0; uimg.h's animation iterator steps the rest
// over ONE canvas, which is the only state that grows with the picture.
// Per frame: the graphic control extension's disposal, delay and
// transparent index, the local colour table, interlacing, and a frame
// rectangle anywhere on the logical screen (clipped to it, as browsers
// clip). The NETSCAPE2.0 block gives the loop count.
//
// **THE COMPOSITING RULES ARE THE BROWSERS'**, where the 89a spec says
// less than it seems to: the canvas starts TRANSPARENT and disposal 2
// clears to transparent -- not to the logical screen's background
// colour, which no browser has painted for decades and which would put
// an opaque box behind every animated sticker. Pillow agrees whenever a
// file has a transparent index, so tools/uimg_codec_hostcheck.py
// compares against it there.
//
// **A FILE IS READ UP TO ITS LAST WHOLE FRAME.** A missing trailer, or a
// truncated or unknown block after at least one complete frame, ends
// the animation there; the same damage before any frame is -EINVAL. A
// frame whose LZW data runs out before its pixels do is -EINVAL too:
// the missing rows are not guessed at.
#include "lib/uimg.h"
#include <kerrno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern void uimg_set_error(const char *msg);
#define GFAIL(code, msg) do { uimg_set_error(msg); return (code); } while (0)

#define GIF_MAX_DIM    16384
#define GIF_MAX_PIXELS (16 * 1024 * 1024)
#define LZW_CODES      4096

struct gif {
    const uint8_t *d;
    size_t n;
    int w, h, version;      // version: 87 or 89
    uint32_t gct[256];
    int gct_n;              // 0: no global table
    size_t first;           // the first block after the header
    int any_transparency;

    size_t pos;             // the next block to read
    uint8_t *idx;           // one frame's indices
    size_t idx_cap;
    uint32_t *saved;        // the canvas before a disposal-3 frame
    int last_disp, lx, ly, lw, lh;  // the shown frame's disposal and rectangle

    uint16_t prefix[LZW_CODES];
    uint8_t suffix[LZW_CODES];
    uint8_t stack[LZW_CODES + 1];
};

static uint32_t le16(const uint8_t *d) { return (uint32_t)d[0] | ((uint32_t)d[1] << 8); }

static int gif_probe(const uint8_t *d, size_t n) {
    return n >= 6 && memcmp(d, "GIF8", 4) == 0 && (d[4] == '7' || d[4] == '9') && d[5] == 'a';
}

static void read_table(const uint8_t *d, int count, uint32_t *out) {
    for (int i = 0; i < count; i++)
        out[i] = 0xFF000000u | ((uint32_t)d[3 * i] << 16) | ((uint32_t)d[3 * i + 1] << 8) | d[3 * i + 2];
    for (int i = count; i < 256; i++) out[i] = 0xFF000000u;
}

// Steps over a chain of sub-blocks. 0, or -1 if the data ran out first.
static int skip_blocks(const uint8_t *d, size_t n, size_t *p) {
    for (;;) {
        if (*p >= n) return -1;
        size_t len = d[(*p)++];
        if (!len) return 0;
        if (*p + len > n) return -1;
        *p += len;
    }
}

static int gif_header(const uint8_t *d, size_t n, struct gif *g) {
    if (n < 13) GFAIL(-EINVAL, "truncated GIF file (no room for a header)");
    if (!gif_probe(d, n)) GFAIL(-EINVAL, "not a GIF file (no GIF87a/GIF89a signature)");
    g->d = d;
    g->n = n;
    g->version = d[4] == '9' ? 89 : 87;
    g->w = (int)le16(d + 6);
    g->h = (int)le16(d + 8);
    if (!g->w || !g->h) GFAIL(-EINVAL, "GIF declares a zero-sized screen");
    if ((uint64_t)g->w * g->h > GIF_MAX_PIXELS)
        GFAIL(-EINVAL, "GIF is larger than this decoder will allocate");
    g->gct_n = 0;
    g->first = 13;
    if (d[10] & 0x80) {
        g->gct_n = 2 << (d[10] & 7);
        if (13 + 3 * (size_t)g->gct_n > n) GFAIL(-EINVAL, "truncated GIF colour table");
        read_table(d + 13, g->gct_n, g->gct);
        g->first += 3 * (size_t)g->gct_n;
    } else {
        read_table(d, 0, g->gct);
    }
    return 0;
}

// Counts the whole frames and reads the loop count, without decoding a
// pixel. *loops: 0 forever, else how many times to play.
static int gif_scan(struct gif *g, int *frames, int *loops) {
    const uint8_t *d = g->d;
    size_t n = g->n, p = g->first;
    *frames = 0;
    *loops = 1;    // no NETSCAPE block: once, as browsers play it
    while (p < n) {
        int b = d[p++];
        if (b == 0x3B) break;
        if (b == 0x21) {
            if (p >= n) goto damaged;
            int label = d[p++];
            if (label == 0xF9 && p + 6 <= n && d[p] == 4 && (d[p + 1] & 1))
                g->any_transparency = 1;
            // NETSCAPE2.0's count is REPETITIONS: N plays N+1 times, as
            // Chromium and Firefox read it.
            if (label == 0xFF && p + 16 <= n && d[p] == 11 &&
                (!memcmp(d + p + 1, "NETSCAPE2.0", 11) || !memcmp(d + p + 1, "ANIMEXTS1.0", 11)) &&
                d[p + 12] == 3 && d[p + 13] == 1) {
                uint32_t c = le16(d + p + 14);
                *loops = c ? (int)c + 1 : 0;
            }
            if (skip_blocks(d, n, &p) < 0) goto damaged;
        } else if (b == 0x2C) {
            if (p + 9 > n) goto damaged;
            int packed = d[p + 8];
            p += 9;
            if (packed & 0x80) p += 3 * ((size_t)2 << (packed & 7));
            p++;                                    // the LZW minimum code size
            if (p > n || skip_blocks(d, n, &p) < 0) goto damaged;
            ++*frames;
        } else {
            goto damaged;
        }
        continue;
    damaged:
        if (*frames == 0) GFAIL(-EINVAL, "GIF ends or breaks before its first whole frame");
        break;
    }
    if (*frames == 0) GFAIL(-EINVAL, "GIF has no frames");
    return 0;
}

// --- LZW ---------------------------------------------------------------

struct bits {
    const uint8_t *d;
    size_t n, *p, left;     // left: bytes still in the current sub-block
    uint32_t acc;
    int have, ended;        // ended: the zero-length terminator was read
};

static int next_byte(struct bits *b) {
    if (!b->left) {
        if (*b->p >= b->n) return -1;
        b->left = b->d[(*b->p)++];
        if (!b->left) { b->ended = 1; return -1; }
    }
    if (*b->p >= b->n) return -1;
    b->left--;
    return b->d[(*b->p)++];
}

// Decodes one frame's indices into g->idx; `want` of them, exactly.
static int lzw_frame(struct gif *g, size_t *p, int min, size_t want) {
    if (min < 1 || min > 8) GFAIL(-EINVAL, "GIF declares an impossible LZW code size");
    const int clear = 1 << min, eoi = clear + 1;
    int size = min + 1, next = clear + 2, old = -1, firstc = 0;
    for (int i = 0; i < clear; i++) { g->prefix[i] = 0; g->suffix[i] = (uint8_t)i; }

    struct bits b = { g->d, g->n, p, 0, 0, 0, 0 };
    size_t got = 0;
    for (;;) {
        while (b.have < size) {
            int c = next_byte(&b);
            if (c < 0) goto out;
            b.acc |= (uint32_t)c << b.have;
            b.have += 8;
        }
        int code = (int)(b.acc & ((1u << size) - 1));
        b.acc >>= size;
        b.have -= size;

        if (code == clear) { size = min + 1; next = clear + 2; old = -1; continue; }
        if (code == eoi) break;
        if (old < 0) {
            if (code >= clear) GFAIL(-EINVAL, "GIF frame's LZW data starts with a code it never defined");
            if (got < want) g->idx[got++] = (uint8_t)code;
            old = firstc = code;
            continue;
        }
        if (code > next) GFAIL(-EINVAL, "GIF frame's LZW data uses a code it never defined");
        int sp = 0, c = code;
        if (code == next) { g->stack[sp++] = (uint8_t)firstc; c = old; }   // KwKwK
        while (c >= clear) {
            if (sp >= LZW_CODES) GFAIL(-EINVAL, "GIF frame's LZW table loops");
            g->stack[sp++] = g->suffix[c];
            c = g->prefix[c];
        }
        g->stack[sp++] = (uint8_t)c;
        firstc = c;
        while (sp && got < want) g->idx[got++] = g->stack[--sp];
        // A full table stops growing until the encoder sends a clear
        // ("deferred clear"), which is legal and common.
        if (next < LZW_CODES) {
            g->prefix[next] = (uint16_t)old;
            g->suffix[next] = (uint8_t)firstc;
            next++;
            if (next == (1 << size) && size < 12) size++;
        }
        old = code;
    }
out:
    // Whatever follows the end code in this frame's sub-blocks is padding.
    if (!b.ended) {
        *p += b.left;
        if (*p > g->n || skip_blocks(g->d, g->n, p) < 0) {
            if (got < want) GFAIL(-EINVAL, "GIF frame's data ends early");
        }
    }
    if (got < want) GFAIL(-EINVAL, "GIF frame's data ends before its last pixel");
    return 0;
}

// --- frames --------------------------------------------------------------

static void fill_rect(struct uimg *c, int x, int y, int w, int h, const uint32_t *from) {
    for (int r = y; r < y + h && r < c->h; r++)
        for (int k = x; k < x + w && k < c->w; k++) {
            size_t i = (size_t)r * c->w + k;
            c->px[i] = from ? from[i] : 0;
        }
}

// The next frame onto the canvas. 0, or a negative errno.
static int gif_step(struct uimg_anim *a, int *delay_ms) {
    struct gif *g = a->priv;
    struct uimg *c = &a->frame;
    const uint8_t *d = g->d;

    if (a->index + 1 >= a->frames) {               // round again: a fresh canvas
        memset(c->px, 0, (size_t)c->w * c->h * sizeof *c->px);
        g->pos = g->first;
        g->last_disp = 0;
        a->index = -1;
    } else if (g->last_disp == 2) {
        fill_rect(c, g->lx, g->ly, g->lw, g->lh, NULL);
    } else if (g->last_disp == 3 && g->saved) {
        fill_rect(c, g->lx, g->ly, g->lw, g->lh, g->saved);
    }

    int disp = 0, delay = 0, trans = -1;
    size_t p = g->pos;
    for (;;) {
        if (p >= g->n) GFAIL(-EINVAL, "GIF ended before a frame it counted");
        int blk = d[p++];
        if (blk == 0x21) {
            if (p >= g->n) GFAIL(-EINVAL, "truncated GIF extension");
            int label = d[p++];
            if (label == 0xF9 && p + 5 <= g->n && d[p] == 4) {
                int packed = d[p + 1];
                disp = (packed >> 2) & 7;
                if (disp > 3) disp = 0;            // 4-7 are undefined
                delay = (int)le16(d + p + 2) * 10;
                trans = (packed & 1) ? d[p + 4] : -1;
            }
            if (skip_blocks(d, g->n, &p) < 0) GFAIL(-EINVAL, "truncated GIF extension");
        } else if (blk == 0x2C) {
            break;
        } else {
            GFAIL(-EINVAL, "GIF has a block this decoder does not know");
        }
    }

    if (p + 10 > g->n) GFAIL(-EINVAL, "truncated GIF image descriptor");
    int fx = (int)le16(d + p), fy = (int)le16(d + p + 2);
    int fw = (int)le16(d + p + 4), fh = (int)le16(d + p + 6);
    int packed = d[p + 8];
    p += 9;
    if (!fw || !fh) GFAIL(-EINVAL, "GIF frame is zero-sized");
    if ((uint64_t)fw * fh > GIF_MAX_PIXELS) GFAIL(-EINVAL, "GIF frame is larger than this decoder will allocate");

    uint32_t lct[256];
    const uint32_t *table = g->gct;
    if (packed & 0x80) {
        int cnt = 2 << (packed & 7);
        if (p + 3 * (size_t)cnt > g->n) GFAIL(-EINVAL, "truncated GIF local colour table");
        read_table(d + p, cnt, lct);
        p += 3 * (size_t)cnt;
        table = lct;
    } else if (!g->gct_n) {
        GFAIL(-EINVAL, "GIF frame has no colour table to use");
    }
    if (p >= g->n) GFAIL(-EINVAL, "truncated GIF frame");
    int min = d[p++];

    size_t want = (size_t)fw * fh;
    if (want > g->idx_cap) {
        uint8_t *nb = realloc(g->idx, want);
        if (!nb) GFAIL(-ENOMEM, "not enough memory for a GIF frame");
        g->idx = nb;
        g->idx_cap = want;
    }
    int rc = lzw_frame(g, &p, min, want);
    if (rc < 0) return rc;

    if (disp == 3) {
        size_t bytes = (size_t)c->w * c->h * sizeof *c->px;
        if (!g->saved && !(g->saved = malloc(bytes)))
            GFAIL(-ENOMEM, "not enough memory to keep a GIF frame");
        memcpy(g->saved, c->px, bytes);
    }

    // Interlaced rows arrive in four passes: every 8th from 0, every
    // 8th from 4, every 4th from 2, every 2nd from 1.
    static const int start[4] = { 0, 4, 2, 1 }, step[4] = { 8, 8, 4, 2 };
    int pass = 0, row = 0;
    for (int r = 0; r < fh; r++) {
        int y = r;
        if (packed & 0x40) {
            while (pass < 4 && start[pass] + row * step[pass] >= fh) { pass++; row = 0; }
            y = start[pass] + row * step[pass];
            row++;
        }
        int cy = fy + y;
        if (cy >= c->h) continue;
        const uint8_t *src = g->idx + (size_t)r * fw;
        uint32_t *dst = c->px + (size_t)cy * c->w;
        for (int x = 0; x < fw && fx + x < c->w; x++)
            if (src[x] != trans) dst[fx + x] = table[src[x]];
    }

    g->pos = p;
    g->last_disp = disp;
    g->lx = fx; g->ly = fy; g->lw = fw; g->lh = fh;
    a->index++;
    *delay_ms = delay;

    int clear_px = 0;
    for (size_t i = 0, t = (size_t)c->w * c->h; i < t && !clear_px; i++)
        clear_px = (c->px[i] >> 24) != 255;
    c->has_alpha = clear_px;
    return 0;
}

static void gif_release(struct uimg_anim *a) {
    struct gif *g = a->priv;
    if (!g) return;
    free(g->idx);
    free(g->saved);
    free(g);
    a->priv = NULL;
}

static int gif_anim_open(const uint8_t *d, size_t n, struct uimg_anim *a) {
    struct gif *g = calloc(1, sizeof *g);
    if (!g) GFAIL(-ENOMEM, "not enough memory to open a GIF");
    int rc = gif_header(d, n, g);
    if (rc == 0) rc = gif_scan(g, &a->frames, &a->loops);
    if (rc < 0) { free(g); return rc; }
    a->frame.w = g->w;
    a->frame.h = g->h;
    a->frame.px = calloc((size_t)g->w * g->h, sizeof *a->frame.px);
    if (!a->frame.px) { free(g); GFAIL(-ENOMEM, "not enough memory for the decoded image"); }
    g->pos = g->first;
    a->priv = g;
    a->index = -1;
    a->next = gif_step;
    a->release = gif_release;
    return 0;
}

static int gif_decode(const uint8_t *d, size_t n, struct uimg *out) {
    struct uimg_anim a;
    memset(&a, 0, sizeof a);
    int rc = gif_anim_open(d, n, &a);
    int delay;
    if (rc == 0) rc = gif_step(&a, &delay);
    gif_release(&a);
    if (rc < 0) {
        free(a.frame.px);
        return rc;
    }
    *out = a.frame;
    return 0;
}

static int gif_info(const uint8_t *d, size_t n, struct uimg_info *out) {
    struct gif *g = calloc(1, sizeof *g);
    if (!g) GFAIL(-ENOMEM, "not enough memory to open a GIF");
    int frames, loops;
    int rc = gif_header(d, n, g);
    if (rc == 0) rc = gif_scan(g, &frames, &loops);
    if (rc < 0) { free(g); return rc; }
    out->w = g->w;
    out->h = g->h;
    out->components = g->any_transparency ? 4 : 3;
    out->format = "gif";
    out->frames = frames;

    // "89a, 12 frames, loops forever" -- built by hand: the hostcheck
    // compiles this file without the guest's snprintf.
    char num[12];
    int k = 0, v = frames;
    char rev[12];
    do { rev[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int i = 0; i < k; i++) num[i] = rev[k - 1 - i];
    num[k] = '\0';
    const char *parts[5] = {
        g->version == 89 ? "89a" : "87a",
        frames > 1 ? ", " : "", frames > 1 ? num : "", frames > 1 ? " frames" : "",
        frames > 1 && loops == 0 ? ", loops forever" : "",
    };
    size_t o = 0;
    for (int i = 0; i < 5; i++)
        for (const char *s = parts[i]; *s && o + 1 < sizeof out->detail; s++) out->detail[o++] = *s;
    out->detail[o] = '\0';
    free(g);
    return 0;
}

const struct uimg_codec uimg_codec_gif = {
    .name      = "gif",
    .probe     = gif_probe,
    .info      = gif_info,
    .decode    = gif_decode,
    .anim_open = gif_anim_open,
};
