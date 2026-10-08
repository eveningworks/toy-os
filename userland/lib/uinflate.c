// See uinflate.h. DEFLATE both ways, with the zlib and gzip wrappers.
#include "lib/uinflate.h"
#include <kcrc.h>
#include <kerrno.h>
#include <stdlib.h>
#include <string.h>

static const char *g_err = "";
const char *uinflate_error(void) { return g_err; }
#define ZFAIL(code, msg) do { g_err = (msg); return (code); } while (0)

// --- the tables both directions share ---------------------------------
//
// RFC 1951 3.2.5. Length code 285 is the odd one: 258 with no extra
// bits, which is why the table stops rather than continuing the run.
const uint16_t uinflate_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
    59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
const uint8_t uinflate_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
    4, 5, 5, 5, 5, 0
};
const uint16_t uinflate_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
    513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
const uint8_t uinflate_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10,
    10, 11, 11, 12, 12, 13, 13
};

#define WSIZE UINFLATE_WSIZE

// --- decoding ---------------------------------------------------------

struct bitr {
    const uint8_t *d;
    size_t n, pos;
    uint32_t acc;
    int nacc;
    int over;      // ran off the end; every later read reads 0
};

static int br_bit(struct bitr *b) {
    if (b->nacc == 0) {
        if (b->pos >= b->n) { b->over = 1; return 0; }
        b->acc = b->d[b->pos++];
        b->nacc = 8;
    }
    int v = (int)(b->acc & 1);
    b->acc >>= 1;
    b->nacc--;
    return v;
}

static uint32_t br_bits(struct bitr *b, int count) {
    uint32_t v = 0;
    for (int i = 0; i < count; i++) v |= (uint32_t)br_bit(b) << i;
    return v;
}

// A canonical Huffman code, in the shape `puff` uses: how many codes of
// each length, and the symbols in canonical order. Decoding walks one
// bit at a time down the length classes.
//
// A TABLE-DRIVEN DECODER IS FASTER AND THIS IS NOT IT, deliberately. The
// fast version builds a lookup indexed by the next 9 bits and needs a
// second level for longer codes; its bugs are in the second level, which
// only rare inputs reach. This one is a dozen lines and is wrong or
// right for every input alike.
#define MAXBITS 15
struct huff {
    short count[MAXBITS + 1];
    short symbol[288];
};

static int huff_build(struct huff *h, const uint8_t *lengths, int n) {
    for (int i = 0; i <= MAXBITS; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    if (h->count[0] == n) return 0;   // no codes at all: legal for distances

    // INCOMPLETE CODES ARE REFUSED, except the one-symbol case the
    // format genuinely produces. A code with slack decodes some bit
    // patterns to nothing, and a decoder that does not check it walks
    // off the end of its own table on a corrupt file.
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;      // over-subscribed
    }

    short offs[MAXBITS + 1];
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) offs[len + 1] = offs[len] + h->count[len];
    for (int i = 0; i < n; i++)
        if (lengths[i]) h->symbol[offs[lengths[i]]++] = (short)i;
    return left;                      // 0 = complete, >0 = incomplete
}

static int huff_decode(struct bitr *b, const struct huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        code |= br_bit(b);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

// Where decompressed bytes go: a 32 KiB history window (back-references
// need it) plus a chunk handed to the caller's callback.
struct sink {
    uint8_t *win;          // the caller's scratch, circular
    size_t wpos;
    uint8_t *buf;          // the caller's scratch too; see uinflate.h
    size_t nbuf;
    uinflate_out out;
    void *ctx;
    size_t total;
    int stopped;
    uint32_t adler_a, adler_b;
    uint32_t crc;
};

static int sink_flush(struct sink *s) {
    if (!s->nbuf) return 0;
    if (s->out && !s->stopped && s->out(s->ctx, s->buf, s->nbuf) != 0)
        s->stopped = 1;
    s->nbuf = 0;
    return s->stopped ? -1 : 0;
}

static int sink_byte(struct sink *s, uint8_t v) {
    s->win[s->wpos] = v;
    s->wpos = (s->wpos + 1) & (WSIZE - 1);
    s->buf[s->nbuf++] = v;
    s->total++;
    // Both checksums run over the DECOMPRESSED bytes, so they are
    // maintained here rather than over the chunk: a caller-supplied sink
    // may refuse the data, and the stream is still what it is.
    s->adler_a += v;
    s->adler_b += s->adler_a;
    if (s->adler_a >= 65521) s->adler_a -= 65521;
    if (s->adler_b >= 65521) s->adler_b -= 65521;
    s->crc = kcrc32_update(s->crc, &v, 1);
    if (s->nbuf == UINFLATE_CHUNK) return sink_flush(s);
    return 0;
}

_Static_assert(2 * sizeof(struct huff) <= UINFLATE_TABLES,
               "the scratch's table area must hold both Huffman tables");

static int inflate_blocks(struct bitr *b, struct sink *s,
                          struct uinflate_scratch *scratch) {
    // INTO THE CALLER'S SCRATCH, not the frame: two of these plus the
    // code-length table put this function three times over the ring-3
    // frame budget (uinflate.h).
    struct huff *lp = (struct huff *)scratch->tables;
    struct huff *dp = lp + 1;
#define lit  (*lp)
#define dist (*dp)
    static const uint8_t FIXED_ORDER[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    int last = 0;

    while (!last) {
        last = br_bit(b);
        int type = (int)br_bits(b, 2);
        if (b->over) ZFAIL(-EINVAL, "the compressed stream ends mid-block");

        if (type == 0) {
            // Stored: discard the partial byte, then LEN and its
            // one's-complement, which is the only integrity check a
            // stored block has.
            b->acc = 0;
            b->nacc = 0;
            if (b->pos + 4 > b->n) ZFAIL(-EINVAL, "a stored block is truncated");
            unsigned len = (unsigned)b->d[b->pos] | ((unsigned)b->d[b->pos + 1] << 8);
            unsigned nlen = (unsigned)b->d[b->pos + 2] | ((unsigned)b->d[b->pos + 3] << 8);
            b->pos += 4;
            if ((len ^ 0xFFFFu) != nlen)
                ZFAIL(-EINVAL, "a stored block's length does not match its complement");
            if (b->pos + len > b->n) ZFAIL(-EINVAL, "a stored block runs past the input");
            for (unsigned i = 0; i < len; i++)
                if (sink_byte(s, b->d[b->pos++]) < 0) ZFAIL(-EIO, "the reader stopped");
            continue;
        }

        if (type == 1) {
            uint8_t l[288], d[30];
            for (int i = 0; i < 144; i++) l[i] = 8;
            for (int i = 144; i < 256; i++) l[i] = 9;
            for (int i = 256; i < 280; i++) l[i] = 7;
            for (int i = 280; i < 288; i++) l[i] = 8;
            for (int i = 0; i < 30; i++) d[i] = 5;
            huff_build(&lit, l, 288);
            huff_build(&dist, d, 30);
        } else if (type == 2) {
            int hlit = (int)br_bits(b, 5) + 257;
            int hdist = (int)br_bits(b, 5) + 1;
            int hclen = (int)br_bits(b, 4) + 4;
            if (hlit > 286 || hdist > 30)
                ZFAIL(-EINVAL, "a dynamic block declares too many codes");

            uint8_t clen[19];
            memset(clen, 0, sizeof clen);
            for (int i = 0; i < hclen; i++) clen[FIXED_ORDER[i]] = (uint8_t)br_bits(b, 3);
            struct huff cl;
            if (huff_build(&cl, clen, 19) != 0)
                ZFAIL(-EINVAL, "a dynamic block's code-length code is not a complete prefix code");

            // The lengths for the literal and distance alphabets are
            // themselves compressed, with three repeat codes. 16 copies
            // the PREVIOUS length, which is why the very first symbol
            // cannot be a 16 -- a file that starts with one is corrupt
            // and would otherwise read uninitialised memory.
            uint8_t lens[288 + 30];
            int i = 0;
            while (i < hlit + hdist) {
                int sym = huff_decode(b, &cl);
                if (sym < 0 || b->over) ZFAIL(-EINVAL, "a code length is unreadable");
                if (sym < 16) {
                    lens[i++] = (uint8_t)sym;
                } else if (sym == 16) {
                    if (i == 0) ZFAIL(-EINVAL, "a code-length repeat with nothing to repeat");
                    uint8_t prev = lens[i - 1];
                    int rep = 3 + (int)br_bits(b, 2);
                    while (rep-- && i < hlit + hdist) lens[i++] = prev;
                } else if (sym == 17) {
                    int rep = 3 + (int)br_bits(b, 3);
                    while (rep-- && i < hlit + hdist) lens[i++] = 0;
                } else {
                    int rep = 11 + (int)br_bits(b, 7);
                    while (rep-- && i < hlit + hdist) lens[i++] = 0;
                }
            }
            if (lens[256] == 0)
                ZFAIL(-EINVAL, "a dynamic block has no end-of-block code");
            if (huff_build(&lit, lens, hlit) != 0)
                ZFAIL(-EINVAL, "a dynamic block's literal code is not a complete prefix code");
            // An INCOMPLETE distance code is legal when there is at most
            // one distance -- a block of pure literals declares one --
            // so only an over-subscribed one is refused here.
            if (huff_build(&dist, lens + hlit, hdist) < 0)
                ZFAIL(-EINVAL, "a dynamic block's distance code is over-subscribed");
        } else {
            ZFAIL(-EINVAL, "a block claims the reserved type 3");
        }

        for (;;) {
            int sym = huff_decode(b, &lit);
            if (sym < 0 || b->over) ZFAIL(-EINVAL, "the compressed stream ends mid-symbol");
            if (sym < 256) {
                if (sink_byte(s, (uint8_t)sym) < 0) ZFAIL(-EIO, "the reader stopped");
                continue;
            }
            if (sym == 256) break;

            sym -= 257;
            if (sym >= 29) ZFAIL(-EINVAL, "an invalid length code");
            int length = uinflate_len_base[sym] + (int)br_bits(b, uinflate_len_extra[sym]);

            int dsym = huff_decode(b, &dist);
            if (dsym < 0 || dsym >= 30) ZFAIL(-EINVAL, "an invalid distance code");
            size_t distance = uinflate_dist_base[dsym] + (size_t)br_bits(b, uinflate_dist_extra[dsym]);
            if (b->over) ZFAIL(-EINVAL, "the compressed stream ends mid-match");
            // BEFORE THE START OF THE STREAM. The window is circular, so
            // without this a distance larger than what has been produced
            // reads whatever the previous stream left there -- which
            // decodes to a plausible picture made of someone else's data.
            if (distance > s->total)
                ZFAIL(-EINVAL, "a match points before the start of the stream");

            // Byte at a time, because an overlapping copy is how a run
            // is encoded: distance 1 with length 100 means "repeat the
            // last byte 100 times", and a memcpy would read ahead of
            // what it is writing.
            for (int k = 0; k < length; k++) {
                uint8_t v = s->win[(s->wpos - distance) & (WSIZE - 1)];
                if (sink_byte(s, v) < 0) ZFAIL(-EIO, "the reader stopped");
            }
        }
    }
    return 0;
}
#undef lit
#undef dist

static uint32_t be32(const uint8_t *d) {
    return ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) |
           ((uint32_t)d[2] << 8) | (uint32_t)d[3];
}
static uint32_t le32(const uint8_t *d) {
    return (uint32_t)d[0] | ((uint32_t)d[1] << 8) |
           ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}

int uinflate(const void *src, size_t n, enum uinflate_wrap wrap,
             struct uinflate_scratch *scratch,
             uinflate_out out, void *ctx, size_t *out_len) {
    if (!scratch) ZFAIL(-EINVAL, "no scratch buffer");
    const uint8_t *d = src;
    size_t start = 0, trailer = 0;

    if (wrap == UINFLATE_ZLIB) {
        if (n < 6) ZFAIL(-EINVAL, "too short to be a zlib stream");
        // CM must be 8 and the two header bytes are a multiple of 31.
        if ((d[0] & 0x0F) != 8) ZFAIL(-EINVAL, "not deflate-compressed");
        if (((unsigned)d[0] << 8 | d[1]) % 31)
            ZFAIL(-EINVAL, "the zlib header's check bits are wrong");
        if (d[1] & 0x20) ZFAIL(-ENOTSUP, "a preset dictionary, which this build has no way to supply");
        start = 2;
        trailer = 4;
    } else if (wrap == UINFLATE_GZIP) {
        if (n < 18) ZFAIL(-EINVAL, "too short to be a gzip member");
        if (d[0] != 0x1F || d[1] != 0x8B) ZFAIL(-EINVAL, "no gzip magic");
        if (d[2] != 8) ZFAIL(-ENOTSUP, "a gzip member compressed with something other than deflate");
        unsigned flg = d[3];
        if (flg & 0xE0) ZFAIL(-EINVAL, "a gzip member sets reserved flags");
        size_t p = 10;
        if (flg & 0x04) {                     // FEXTRA
            if (p + 2 > n) ZFAIL(-EINVAL, "a truncated gzip extra field");
            size_t xlen = (size_t)d[p] | ((size_t)d[p + 1] << 8);
            p += 2 + xlen;
        }
        if (flg & 0x08) { while (p < n && d[p]) p++; p++; }   // FNAME
        if (flg & 0x10) { while (p < n && d[p]) p++; p++; }   // FCOMMENT
        if (flg & 0x02) p += 2;                                // FHCRC
        if (p + 8 > n) ZFAIL(-EINVAL, "a truncated gzip member");
        start = p;
        trailer = 8;
    }

    if (start + trailer > n) ZFAIL(-EINVAL, "a truncated stream");

    struct sink s;
    memset(&s, 0, sizeof s);
    s.win = scratch->window;
    s.buf = scratch->chunk;
    s.out = out;
    s.ctx = ctx;
    s.adler_a = 1;
    s.crc = KCRC32_INIT;

    struct bitr b = { d + start, n - start - trailer, 0, 0, 0, 0 };
    int rc = inflate_blocks(&b, &s, scratch);
    if (rc == 0 && sink_flush(&s) < 0) { g_err = "the reader stopped"; rc = -EIO; }

    if (rc == 0 && wrap == UINFLATE_ZLIB) {
        uint32_t want = be32(d + n - 4);
        if (want != ((s.adler_b << 16) | s.adler_a)) {
            g_err = "the zlib stream's adler32 does not match";
            rc = -EINVAL;
        }
    } else if (rc == 0 && wrap == UINFLATE_GZIP) {
        if (le32(d + n - 8) != KCRC32_FINAL(s.crc)) {
            g_err = "the gzip member's crc32 does not match";
            rc = -EINVAL;
        } else if (le32(d + n - 4) != (uint32_t)s.total) {
            // ISIZE is the length modulo 2^32, so this compares the low
            // word rather than the total -- a >4 GiB member is not wrong,
            // it is merely bigger than the field.
            g_err = "the gzip member's length does not match";
            rc = -EINVAL;
        }
    }

    if (out_len) *out_len = s.total;
    return rc;
}

// --- the two convenience shapes ---------------------------------------

struct into_ctx { uint8_t *dst; size_t cap, len; };

static int into_out(void *vctx, const uint8_t *data, size_t n) {
    struct into_ctx *c = vctx;
    if (c->len + n > c->cap) return -1;   // refuse, never truncate
    memcpy(c->dst + c->len, data, n);
    c->len += n;
    return 0;
}

int uinflate_into(const void *src, size_t n, enum uinflate_wrap wrap,
                  struct uinflate_scratch *scratch,
                  void *dst, size_t cap, size_t *out_len) {
    struct into_ctx c = { dst, cap, 0 };
    int rc = uinflate(src, n, wrap, scratch, into_out, &c, out_len);
    if (rc == -EIO) ZFAIL(-EINVAL, "the stream decompresses to more than it should");
    return rc;
}

// --- compressing ------------------------------------------------------
//
// One module, both directions, one place to test. See uinflate.h on why
// it stays greedy and fixed-Huffman.

struct bitw {
    uint8_t *buf;
    size_t cap, len;
    uint32_t acc;
    int nacc;
    int full;
};

static void bw_bits(struct bitw *b, uint32_t v, int n) {
    b->acc |= v << b->nacc;
    b->nacc += n;
    while (b->nacc >= 8) {
        if (b->len >= b->cap) { b->full = 1; b->nacc = 0; b->acc = 0; return; }
        b->buf[b->len++] = (uint8_t)b->acc;
        b->acc >>= 8;
        b->nacc -= 8;
    }
}

static void bw_flush(struct bitw *b) {
    if (b->nacc > 0) bw_bits(b, 0, 8 - b->nacc);
}

// A Huffman code is packed MOST significant bit first while everything
// else in deflate is least significant bit first, so a code goes out
// reversed. Getting this backwards writes a file that is structurally
// perfect and decodes to noise.
static uint32_t bit_reverse(uint32_t v, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; i++) { r = (r << 1) | (v & 1); v >>= 1; }
    return r;
}

static void bw_fixed_sym(struct bitw *b, int sym) {
    if (sym < 144)      bw_bits(b, bit_reverse(0x30 + sym, 8), 8);
    else if (sym < 256) bw_bits(b, bit_reverse(0x190 + sym - 144, 9), 9);
    else if (sym < 280) bw_bits(b, bit_reverse(sym - 256, 7), 7);
    else                bw_bits(b, bit_reverse(0xC0 + sym - 280, 8), 8);
}

static void bw_match(struct bitw *b, int len, int dist) {
    int lc = 28;
    while (lc > 0 && len < uinflate_len_base[lc]) lc--;
    bw_fixed_sym(b, 257 + lc);
    if (uinflate_len_extra[lc]) bw_bits(b, (uint32_t)(len - uinflate_len_base[lc]), uinflate_len_extra[lc]);

    int dc = 29;
    while (dc > 0 && dist < uinflate_dist_base[dc]) dc--;
    bw_bits(b, bit_reverse((uint32_t)dc, 5), 5);
    if (uinflate_dist_extra[dc])
        bw_bits(b, (uint32_t)(dist - uinflate_dist_base[dc]), uinflate_dist_extra[dc]);
}

#define DEF_HBITS  15
#define DEF_HSIZE  (1 << DEF_HBITS)
#define DEF_MINLEN 3
#define DEF_MAXLEN 258
#define DEF_PROBES 32   // chain depth: the whole speed/ratio dial here
// A LIVE stream's (udeflate_sync()): a frame is sent once, and late is
// worse than large. On a 1080p desktop's ZRLE tiles, 8 cost 1% in size.
#define DEF_PROBES_LIVE 8

static uint32_t def_hash(const uint8_t *p) {
    return (((uint32_t)p[0] << 10) ^ ((uint32_t)p[1] << 5) ^ (uint32_t)p[2])
           & (DEF_HSIZE - 1);
}

size_t udeflate_bound(size_t n) { return n + n / 8 + 128; }

// One fixed-Huffman block of `src` into `b`: greedy LZ77 over this
// input alone, so a block never reaches back into an earlier one -- which
// is what lets a stream be cut into independently made pieces.
static int deflate_block(const uint8_t *src, size_t n, struct bitw *b, int final, int probes_max) {
    int32_t *head_tbl = malloc((size_t)DEF_HSIZE * sizeof *head_tbl);
    int32_t *prev = malloc((size_t)WSIZE * sizeof *prev);
    if (!head_tbl || !prev) {
        free(head_tbl); free(prev);
        ZFAIL(-ENOMEM, "no memory for the match tables");
    }
    for (int i = 0; i < DEF_HSIZE; i++) head_tbl[i] = -1;
    for (int i = 0; i < WSIZE; i++) prev[i] = -1;

    bw_bits(b, final ? 1 : 0, 1); // BFINAL
    bw_bits(b, 1, 2);             // BTYPE = fixed Huffman

    size_t pos = 0;
    while (pos < n) {
        int best_len = 0, best_dist = 0;
        if (pos + DEF_MINLEN <= n) {
            uint32_t h = def_hash(src + pos);
            int32_t cand = head_tbl[h];
            int probes = probes_max;
            while (cand >= 0 && probes-- > 0) {
                size_t distance = pos - (size_t)cand;
                if (distance == 0 || distance > WSIZE) break;
                size_t maxl = n - pos;
                if (maxl > DEF_MAXLEN) maxl = DEF_MAXLEN;
                size_t l = 0;
                while (l < maxl && src[cand + l] == src[pos + l]) l++;
                if ((int)l > best_len) {
                    best_len = (int)l;
                    best_dist = (int)distance;
                    if (best_len >= DEF_MAXLEN) break;
                }
                cand = prev[(size_t)cand & (WSIZE - 1)];
            }
        }

        if (best_len >= DEF_MINLEN) {
            bw_match(b, best_len, best_dist);
            // Every position inside the match still has to enter the
            // chain, or the next search cannot see back past it.
            for (int i = 0; i < best_len; i++) {
                if (pos + DEF_MINLEN <= n) {
                    uint32_t h = def_hash(src + pos);
                    prev[pos & (WSIZE - 1)] = head_tbl[h];
                    head_tbl[h] = (int32_t)pos;
                }
                pos++;
            }
        } else {
            bw_fixed_sym(b, src[pos]);
            if (pos + DEF_MINLEN <= n) {
                uint32_t h = def_hash(src + pos);
                prev[pos & (WSIZE - 1)] = head_tbl[h];
                head_tbl[h] = (int32_t)pos;
            }
            pos++;
        }
        if (b->full) break;
    }

    bw_fixed_sym(b, 256); // end of block
    free(head_tbl);
    free(prev);
    return 0;
}

int udeflate_into(const void *src_v, size_t n, enum uinflate_wrap wrap,
                  void *dst_v, size_t cap, size_t *out_len) {
    const uint8_t *src = src_v;
    uint8_t *dst = dst_v;
    size_t head = 0;

    if (wrap == UINFLATE_ZLIB) {
        if (cap < 6) ZFAIL(-ENOMEM, "no room for a zlib stream");
        dst[0] = 0x78;   // deflate, 32 KiB window
        dst[1] = 0x01;   // and the two bytes are a multiple of 31
        head = 2;
    } else if (wrap == UINFLATE_GZIP) {
        if (cap < 18) ZFAIL(-ENOMEM, "no room for a gzip member");
        static const uint8_t hdr[10] = { 0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 255 };
        memcpy(dst, hdr, sizeof hdr);
        head = 10;
    }

    size_t tail = (wrap == UINFLATE_RAW) ? 0 : (wrap == UINFLATE_ZLIB ? 4 : 8);
    struct bitw b = { dst + head, cap > head + tail ? cap - head - tail : 0, 0, 0, 0, 0 };
    int rc = deflate_block(src, n, &b, 1, DEF_PROBES);
    if (rc) return rc;
    bw_flush(&b);
    if (b.full) ZFAIL(-ENOMEM, "the compressed result did not fit");

    size_t len = head + b.len;
    if (wrap == UINFLATE_ZLIB) {
        uint32_t a = 1, s2 = 0;
        for (size_t i = 0; i < n; i++) { a = (a + src[i]) % 65521; s2 = (s2 + a) % 65521; }
        uint32_t sum = (s2 << 16) | a;
        dst[len++] = (uint8_t)(sum >> 24); dst[len++] = (uint8_t)(sum >> 16);
        dst[len++] = (uint8_t)(sum >> 8);  dst[len++] = (uint8_t)sum;
    } else if (wrap == UINFLATE_GZIP) {
        uint32_t crc = kcrc32(src, n);
        dst[len++] = (uint8_t)crc; dst[len++] = (uint8_t)(crc >> 8);
        dst[len++] = (uint8_t)(crc >> 16); dst[len++] = (uint8_t)(crc >> 24);
        uint32_t isz = (uint32_t)n;
        dst[len++] = (uint8_t)isz; dst[len++] = (uint8_t)(isz >> 8);
        dst[len++] = (uint8_t)(isz >> 16); dst[len++] = (uint8_t)(isz >> 24);
    }
    if (out_len) *out_len = len;
    return 0;
}

int udeflate_sync(struct udeflate_stream *st, const void *src, size_t n,
                  void *dst_v, size_t cap, size_t *out_len) {
    uint8_t *dst = dst_v;
    size_t head = 0;
    if (!st->started) {
        if (cap < 2) ZFAIL(-ENOMEM, "no room for a zlib header");
        dst[0] = 0x78;
        dst[1] = 0x01;
        head = 2;
    }
    struct bitw b = { dst + head, cap > head ? cap - head : 0, 0, 0, 0, 0 };
    int rc = deflate_block(src, n, &b, 0, DEF_PROBES_LIVE);
    if (rc) return rc;
    // THE SYNC FLUSH: an empty stored block, which byte-aligns the stream
    // (zlib's Z_SYNC_FLUSH). The reader can then decode everything sent
    // so far without the stream ever ending.
    bw_bits(&b, 0, 1);
    bw_bits(&b, 0, 2);
    bw_flush(&b);
    bw_bits(&b, 0x0000, 16);
    bw_bits(&b, 0xFFFF, 16);
    if (b.full) ZFAIL(-ENOMEM, "the compressed result did not fit");
    st->started = 1;
    if (out_len) *out_len = head + b.len;
    return 0;
}
