#ifndef REMOTED_RD_H
#define REMOTED_RD_H

// /bin/remoted's parts, shared through one header (userland/wm/'s
// shape): rd_vnc.c speaks RFB to one viewer, rd_enc.c turns pixels into
// RFB rectangles. userland/bin/remoted.c is the listener and the session
// entry point; /etc/remote.conf is lib/uremote.h's, shared with System
// Settings and the tray.

#include <stdint.h>
#include <stddef.h>
#include "lib/uremote.h"   // the config, who may connect

// VeNCrypt's identity: made by the listener on first need, kept, and
// trusted by viewers by its fingerprint (utls.h).
#define RD_TLS_KEY "/etc/remote.key"
#define RD_TLS_CRT "/etc/remote.crt"

// Logs to fd 2 -- the kernel log. NEVER stdout: in a session fd 1 is
// the viewer's socket.
void rd_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// --- one VNC viewer, on fd 0/1 (rd_vnc.c) ---------------------------
int rd_vnc_session(const struct uremote_conf *c, uint32_t peer);

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

#define RD_ENC_RAW   0
#define RD_ENC_TIGHT 7
#define RD_ENC_ZRLE  16
#define RD_ENC_DESKTOP_SIZE (-223)   // pseudo-encodings: what a viewer can be told
#define RD_ENC_CURSOR       (-239)
#define RD_ENC_POINTER_POS  (-232)
#define RD_ENC_VMWARE_POS   0x574D5666   // VMware's PointerPos, what TigerVNC reads
#define RD_ENC_QUALITY_0    (-32)        // ...to -23: Tight's JPEG quality level 0-9

// The encoder's state for one connection: the zlib streams ZRLE and
// Tight keep for the whole session, as the viewer keeps its inflaters.
struct rd_enc {
    struct rd_pixfmt pf;
    int encoding;              // RD_ENC_RAW, RD_ENC_ZRLE or RD_ENC_TIGHT
    int fallback;              // the viewer's next choice, for a format Tight cannot send
    int jpeg_level;            // Tight's quality level 0-9, or -1: never lossy
    int zrle_started;
    int tight_started[4];
    struct rd_buf scratch;     // ZRLE's uncompressed tile data
};

// Appends `px` (a `stride`-wide 0x??RRGGBB image) at (x, y, w, h) to
// `out`, header and data. Returns how many rectangles that took: Tight
// cuts a large one up.
int rd_enc_rect(struct rd_enc *e, const uint32_t *px, int stride,
                int x, int y, int w, int h, struct rd_buf *out);

void rd_rect_header(struct rd_buf *out, int x, int y, int w, int h, int32_t enc);

// --- Tight (rd_tight.c) ------------------------------------------------
int rd_tight_ok(const struct rd_pixfmt *pf);   // a format Tight's 3-byte pixels fit
int rd_enc_tight(struct rd_enc *e, const uint32_t *px, int stride, int x, int y,
                 int w, int h, struct rd_buf *out);

// RFB's Cursor pseudo-encoding: the shape (0xAARRGGBB, `w` x `h`) and
// its hotspot as one rectangle -- pixels in the viewer's format, then a
// 1-bit mask of what is drawn.
void rd_enc_cursor(struct rd_enc *e, const uint32_t *argb, int w, int h,
                   int hot_x, int hot_y, struct rd_buf *out);

#endif
