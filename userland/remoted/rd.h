#ifndef REMOTED_RD_H
#define REMOTED_RD_H

// /bin/remoted's parts, shared through one header (userland/wm/'s
// shape): rd_conf.c reads /etc/remote.conf and judges a peer, rd_vnc.c
// speaks RFB to one viewer, rd_enc.c turns pixels into RFB rectangles.
// userland/bin/remoted.c is the listener and the session entry point.

#include <stdint.h>
#include <stddef.h>

#define RD_CONF "/etc/remote.conf"
#define RD_PASSWORD_MAX 64
#define RD_TRUSTED_MAX 16

enum rd_when { RD_ASK, RD_ASK_UNLESS_TRUSTED, RD_ALWAYS };
enum rd_from { RD_FROM_NETWORK, RD_FROM_ANYWHERE };

struct rd_net { uint32_t ip, mask; };   // one trusted address or subnet

struct rd_proto {
    int enabled;
    int port;
    char user[32];                 // RDP's; VNC has no user name
    char password[RD_PASSWORD_MAX];
};

struct rd_conf {
    struct rd_proto vnc, rdp;
    enum rd_when when;
    enum rd_from from;
    struct rd_net trusted[RD_TRUSTED_MAX];
    int ntrusted;
    int view_only;
};

// Reads RD_CONF; a missing file or key is its default (everything off).
void rd_conf_load(struct rd_conf *c);

// Is `ip` on a network one of this machine's cards is on? What
// `from = network` admits.
int rd_peer_local(uint32_t ip);
int rd_peer_trusted(const struct rd_conf *c, uint32_t ip);
// "a.b.c.d" or "a.b.c.d/bits"; 1 on success. A parser that refuses.
int rd_parse_net(const char *s, struct rd_net *out);
void rd_fmt_ip(uint32_t ip, char *out, size_t cap);

// Logs to fd 2 -- the kernel log. NEVER stdout: in a session fd 1 is
// the viewer's socket.
void rd_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// --- one VNC viewer, on fd 0/1 (rd_vnc.c) ---------------------------
int rd_vnc_session(const struct rd_conf *c, uint32_t peer);

// --- encoding (rd_enc.c) ----------------------------------------------

// RFB's PIXEL_FORMAT (RFB 7.4), as the viewer asked for it.
struct rd_pixfmt {
    uint8_t bpp, depth, big_endian, true_colour;
    uint16_t rmax, gmax, bmax;
    uint8_t rshift, gshift, bshift;
};

// What toy-os draws: 32 bits, 0x00RRGGBB, little-endian.
void rd_pixfmt_native(struct rd_pixfmt *pf);
void rd_pixfmt_read(struct rd_pixfmt *pf, const uint8_t b[16]);
void rd_pixfmt_write(const struct rd_pixfmt *pf, uint8_t b[16]);
// 0 if the server can produce it: true colour, 8/16/32 bpp.
int rd_pixfmt_check(const struct rd_pixfmt *pf);

// A growable output buffer; `fail` sticks once memory runs out.
struct rd_buf { uint8_t *p; size_t len, cap; int fail; };
void rd_buf_put(struct rd_buf *b, const void *src, size_t n);
void rd_buf_u8(struct rd_buf *b, uint8_t v);
void rd_buf_u16(struct rd_buf *b, uint16_t v);
void rd_buf_u32(struct rd_buf *b, uint32_t v);
void rd_buf_free(struct rd_buf *b);

#define RD_ENC_RAW  0
#define RD_ENC_ZRLE 16

// The encoder's state for one connection: ZRLE's single zlib stream.
struct rd_enc {
    struct rd_pixfmt pf;
    int encoding;              // RD_ENC_RAW or RD_ENC_ZRLE
    int zrle_started;
    struct rd_buf scratch;     // ZRLE's uncompressed tile data
};

// Appends one rectangle -- header and data -- of `px` (a `stride`-wide
// 0x??RRGGBB image) at (x, y, w, h) to `out`.
void rd_enc_rect(struct rd_enc *e, const uint32_t *px, int stride,
                 int x, int y, int w, int h, struct rd_buf *out);

#endif
