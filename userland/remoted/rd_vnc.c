// One VNC viewer: RFB 3.8 (RFC 6143) on fd 0 and fd 1. See rd.h.
//
// THE SCREEN IS PULLED, NOT PUSHED. RFB lets the server sit on an
// incremental update request until something changes, so the loop reads
// the viewer with a short deadline and, while a request is pending,
// captures the screen through the compositor (lib/ushot.h -- the same
// request `screenshot` uses) and compares it with the last frame sent in
// 64x64 tiles. Only changed tiles go out. TigerVNC's server does this
// comparison too ("comparing update tracker"); a compositor damage hint
// would save the compare and is the obvious next step.
//
// INPUT GOES IN THROUGH THE KERNEL (SYS_INPUT_INJECT), where a keyboard's
// and a mouse's do, so a viewer's keys meet the same layout, shortcuts
// and focus as the machine's own.
//
// EVERYTHING READ HERE IS A STRANGER'S. Every length is bounded before
// it is used, an unknown message ends the session (RFB has no way to
// skip one), and the password check runs in constant time.
#include "remoted/rd.h"
#include "lib/udes.h"
#include "lib/ukeysym.h"
#include "lib/ushot.h"
#include "rt/sys.h"
#include "net_abi.h"
#include "syscall_abi.h"   // struct input_inject
#include <kerrno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define POLL_MS 33              // the read deadline: ~30 checks a second
#define CUT_TEXT_MAX (1 << 20)  // a clipboard the viewer sends; skipped
#define ENCODINGS_MAX 64

struct vnc {
    const struct rd_conf *conf;
    uint32_t peer;
    struct rd_enc enc;
    struct ushot shot;
    uint32_t *prev;           // the frame the viewer has
    int w, h;
    int want_update, want_full;
    int ux, uy, uw, uh;       // the requested region
    int desktop_size;         // the viewer understands DesktopSize
    uint8_t buttons;
    int view_only;
    struct rd_buf out;
};

// --- the socket -----------------------------------------------------------

// Reads exactly `n` bytes, waiting as long as it takes. 0 when the
// viewer went away.
static int read_full(void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        int64_t r = read(0, p, n);
        if (r <= 0) return 0;
        p += r;
        n -= (size_t)r;
    }
    return 1;
}

// THE FIRST BYTE OF A MESSAGE, with a deadline: 1 read, 0 timed out, -1
// the viewer closed. A timed receive returns 0 for both a timeout and a
// close; the clock tells them apart, because a timeout comes back only
// once the deadline has passed and a close comes back at once.
static int read_first(uint8_t *b) {
    unsigned long long t0 = sys_monotonic_ns();
    uint32_t src;
    uint16_t port;
    int64_t r = sys_recvfrom(0, b, 1, &src, &port, POLL_MS);
    if (r == 1) return 1;
    if (r < 0) return -1;
    return (sys_monotonic_ns() - t0) / 1000000ull + 2 >= POLL_MS ? 0 : -1;
}

static int write_full(const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        int64_t r = write(1, p, n);
        if (r <= 0) return 0;
        p += r;
        n -= (size_t)r;
    }
    return 1;
}

static int flush(struct vnc *v) {
    if (v->out.fail) return 0;
    int ok = write_full(v->out.p, v->out.len);
    v->out.len = 0;
    return ok;
}

static uint16_t be16(const uint8_t *b) { return (uint16_t)(b[0] << 8 | b[1]); }
static uint32_t be32(const uint8_t *b) {
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

// --- the handshake ----------------------------------------------------------

static void send_reason(const char *why) {
    uint8_t len[4] = { 0, 0, 0, (uint8_t)strlen(why) };
    write_full(len, 4);
    write_full(why, strlen(why));
}

// VNC Authentication (RFB 7.2.2): DES of a 16-byte challenge under the
// password, whose key bytes go in bit-reversed -- a quirk of the
// original implementation that every viewer since has had to copy.
static int vnc_auth(const char *password) {
    uint8_t challenge[16], response[16], expect[16], key[8] = { 0 };
    if (sys_getrandom(challenge, sizeof challenge) != (int)sizeof challenge) return 0;
    for (int i = 0; i < 8 && password[i]; i++) {
        uint8_t c = (uint8_t)password[i], r = 0;
        for (int b = 0; b < 8; b++) if (c & (1 << b)) r |= (uint8_t)(0x80 >> b);
        key[i] = r;
    }
    struct udes_key ks;
    udes_set_key(&ks, key);
    udes_encrypt(&ks, challenge, expect);
    udes_encrypt(&ks, challenge + 8, expect + 8);
    if (!write_full(challenge, 16) || !read_full(response, 16)) return 0;
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(response[i] ^ expect[i]);
    memset(&ks, 0, sizeof ks);
    return diff == 0;
}

// The version exchange, security and authentication. 1 when the viewer
// is in.
static int handshake(struct vnc *v, int *minor) {
    if (!write_full("RFB 003.008\n", 12)) return 0;
    char ver[13] = { 0 };
    if (!read_full(ver, 12)) return 0;
    if (memcmp(ver, "RFB 003.", 8) != 0) return 0;
    *minor = atoi(ver + 8);
    // 3.889 is Apple's Screen Sharing; it speaks 3.8 with VNC auth.
    if (*minor >= 8) *minor = 8;
    else if (*minor != 7) *minor = 3;

    const char *pw = v->conf->vnc.password;
    if (!pw[0]) {
        rd_log("remoted: vnc: refused -- no password is set\n");
        if (*minor == 3) { uint8_t z[4] = { 0 }; write_full(z, 4); }
        else             { uint8_t z = 0; write_full(&z, 1); }
        send_reason("No password is set on this machine.");
        return 0;
    }
    if (*minor == 3) {
        uint8_t t[4] = { 0, 0, 0, 2 };
        if (!write_full(t, 4)) return 0;
    } else {
        uint8_t t[2] = { 1, 2 };   // one type: VNC Authentication
        uint8_t pick;
        if (!write_full(t, 2) || !read_full(&pick, 1) || pick != 2) return 0;
    }
    int ok = vnc_auth(pw);
    uint8_t res[4] = { 0, 0, 0, ok ? 0 : 1 };
    write_full(res, 4);
    if (!ok) {
        if (*minor == 8) send_reason("Authentication failed.");
        char ip[16];
        rd_fmt_ip(v->peer, ip, sizeof ip);
        rd_log("remoted: vnc: %s: wrong password\n", ip);
        // A guess per two seconds per connection: the 8-character DES
        // key is weak enough that pacing is the only defence left.
        sys_sleep_ms(2000);
        return 0;
    }
    return 1;
}

// May this viewer in, once authenticated? The password is already
// checked; this is the second question, "When someone connects".
static int admitted(struct vnc *v) {
    const struct rd_conf *c = v->conf;
    if (c->when == RD_ALWAYS) return 1;
    if (c->when == RD_ASK_UNLESS_TRUSTED && rd_peer_trusted(c, v->peer)) return 1;
    // Asking the person at the screen is the compositor's notice, which
    // this build does not have yet: refuse rather than let in unasked.
    char ip[16];
    rd_fmt_ip(v->peer, ip, sizeof ip);
    rd_log("remoted: vnc: %s: not trusted, and asking is not built yet -- refused\n", ip);
    return 0;
}

static int server_init(struct vnc *v) {
    rd_buf_u16(&v->out, (uint16_t)v->w);
    rd_buf_u16(&v->out, (uint16_t)v->h);
    uint8_t pf[16];
    rd_pixfmt_write(&v->enc.pf, pf);
    rd_buf_put(&v->out, pf, 16);
    const char *name = "toy-os";
    rd_buf_u32(&v->out, (uint32_t)strlen(name));
    rd_buf_put(&v->out, name, strlen(name));
    return flush(v);
}

// --- input ---------------------------------------------------------------

static void inject(struct input_inject *ev, int n) {
    if (n > 0 && sys_input_inject(ev, n) < 0)
        rd_log("remoted: vnc: input refused (errno %d)\n", sys_errno());
}

static void key_event(struct vnc *v, int down, uint32_t keysym) {
    if (v->view_only) return;
    uint16_t code;
    struct input_inject e = { 0, 0, down, 0 };
    switch (ukeysym_lookup(keysym, &code)) {
    case UKEYSYM_CHAR: e.op = INPUT_INJECT_CHAR; break;
    case UKEYSYM_KEY:  e.op = INPUT_INJECT_KEY;  break;
    default: return;   // a keysym toy-os has no key for
    }
    e.code = code;
    inject(&e, 1);
}

// RFB's mask: 1 left, 2 middle, 4 right, 8/16 wheel up/down. toy-os's:
// bit0 left, bit1 right, bit2 middle (kernel/input.h).
static void pointer_event(struct vnc *v, uint8_t mask, int x, int y) {
    if (v->view_only) return;
    if (x >= v->w) x = v->w - 1;
    if (y >= v->h) y = v->h - 1;
    struct input_inject ev[3];
    int n = 0;
    uint16_t btn = (uint16_t)((mask & 1) | ((mask & 4) ? 2 : 0) | ((mask & 2) ? 4 : 0));
    ev[n++] = (struct input_inject){ INPUT_INJECT_POINTER, btn, x, y };
    // A wheel notch is the PRESS of button 4 or 5; the release is nothing.
    uint8_t pressed = (uint8_t)(mask & ~v->buttons);
    if (pressed & 8)  ev[n++] = (struct input_inject){ INPUT_INJECT_WHEEL, 0, 1, 0 };
    if (pressed & 16) ev[n++] = (struct input_inject){ INPUT_INJECT_WHEEL, 0, -1, 0 };
    v->buttons = mask;
    inject(ev, n);
}

// --- the screen ------------------------------------------------------------

#define CMP_TILE 64
// More changed runs than this in one update wait for the next one: they
// are still different from `prev`, so the next compare finds them.
#define RECTS_MAX 512

static int tile_changed(const struct vnc *v, int x, int y, int w, int h) {
    for (int j = 0; j < h; j++) {
        size_t o = (size_t)(y + j) * v->w + x;
        if (memcmp(v->shot.px + o, v->prev + o, (size_t)w * 4)) return 1;
    }
    return 0;
}

static void copy_rect(struct vnc *v, int x, int y, int w, int h) {
    for (int j = 0; j < h; j++) {
        size_t o = (size_t)(y + j) * v->w + x;
        memcpy(v->prev + o, v->shot.px + o, (size_t)w * 4);
    }
}

// One FramebufferUpdate with every changed tile in the requested region,
// tiles merged along a row into one rectangle. 1 sent, 0 nothing to
// send, -1 the connection failed.
static int send_update(struct vnc *v) {
    int rc = ushot_take(&v->shot, WIN_SHOT_SCREEN, WIN_SHOT_POINTER, 0, 0, 0, 0);
    if (rc == -EBUSY) return 0;   // a fullscreen program has the display
    if (rc < 0) return -1;
    if (v->shot.w != v->w || v->shot.h != v->h) return -1;   // the mode changed

    int x0 = v->ux, y0 = v->uy, x1 = v->ux + v->uw, y1 = v->uy + v->uh;
    static struct { uint16_t x, y, w, h; } rects[RECTS_MAX];
    int n = 0;
    for (int ty = y0; ty < y1 && n < RECTS_MAX; ty += CMP_TILE) {
        int th = y1 - ty < CMP_TILE ? y1 - ty : CMP_TILE;
        int run = -1;
        // One step past the last tile, so a run reaching the right edge
        // is closed like any other.
        for (int tx = x0;; tx += CMP_TILE) {
            int end = tx >= x1;
            int tw = x1 - tx < CMP_TILE ? x1 - tx : CMP_TILE;
            int ch = !end && (v->want_full || tile_changed(v, tx, ty, tw, th));
            if (ch && run < 0) run = tx;
            if (!ch && run >= 0 && n < RECTS_MAX) {
                int stop = end ? x1 : tx;
                rects[n].x = (uint16_t)run;
                rects[n].y = (uint16_t)ty;
                rects[n].w = (uint16_t)(stop - run);
                rects[n].h = (uint16_t)th;
                n++;
                run = -1;
            }
            if (end) break;
        }
    }
    if (!n) return 0;

    rd_buf_u8(&v->out, 0);   // FramebufferUpdate
    rd_buf_u8(&v->out, 0);
    rd_buf_u16(&v->out, (uint16_t)n);
    for (int i = 0; i < n; i++) {
        rd_enc_rect(&v->enc, v->shot.px, v->w, rects[i].x, rects[i].y, rects[i].w,
                    rects[i].h, &v->out);
        copy_rect(v, rects[i].x, rects[i].y, rects[i].w, rects[i].h);
    }
    v->want_update = v->want_full = 0;
    return flush(v) ? 1 : -1;
}

// --- messages --------------------------------------------------------------

static int set_encodings(struct vnc *v) {
    uint8_t h[3];
    if (!read_full(h, 3)) return 0;
    int n = be16(h + 1);
    int chosen = -1;
    for (int i = 0; i < n; i++) {
        uint8_t e[4];
        if (!read_full(e, 4)) return 0;
        int32_t enc = (int32_t)be32(e);
        // The viewer lists them by preference; the first one known wins.
        if (chosen < 0 && (enc == RD_ENC_ZRLE || enc == RD_ENC_RAW)) chosen = enc;
        if (enc == -223) v->desktop_size = 1;
    }
    v->enc.encoding = chosen < 0 ? RD_ENC_RAW : chosen;
    return 1;
}

static int skip(uint32_t n) {
    uint8_t junk[512];
    while (n) {
        uint32_t k = n < sizeof junk ? n : sizeof junk;
        if (!read_full(junk, k)) return 0;
        n -= k;
    }
    return 1;
}

static int message(struct vnc *v, uint8_t type) {
    uint8_t b[20];
    switch (type) {
    case 0: {                                        // SetPixelFormat
        if (!read_full(b, 19)) return 0;
        struct rd_pixfmt pf;
        rd_pixfmt_read(&pf, b + 3);
        if (rd_pixfmt_check(&pf) != 0) {
            rd_log("remoted: vnc: a pixel format this server cannot make\n");
            return 0;
        }
        v->enc.pf = pf;
        v->want_full = 1;
        return 1;
    }
    case 2:                                          // SetEncodings
        return set_encodings(v);
    case 3:                                          // FramebufferUpdateRequest
        if (!read_full(b, 9)) return 0;
        v->ux = be16(b + 1); v->uy = be16(b + 3);
        v->uw = be16(b + 5); v->uh = be16(b + 7);
        if (v->ux >= v->w || v->uy >= v->h) return 1;
        if (v->ux + v->uw > v->w) v->uw = v->w - v->ux;
        if (v->uy + v->uh > v->h) v->uh = v->h - v->uy;
        if (!b[0]) v->want_full = 1;
        v->want_update = 1;
        return 1;
    case 4:                                          // KeyEvent
        if (!read_full(b, 7)) return 0;
        key_event(v, b[0] != 0, be32(b + 3));
        return 1;
    case 5:                                          // PointerEvent
        if (!read_full(b, 5)) return 0;
        pointer_event(v, b[0], be16(b + 1), be16(b + 3));
        return 1;
    case 6: {                                        // ClientCutText
        if (!read_full(b, 7)) return 0;
        int32_t len = (int32_t)be32(b + 3);
        // Negative is the Extended Clipboard form; either way, skipped.
        uint32_t n = len < 0 ? (uint32_t)-len : (uint32_t)len;
        if (n > CUT_TEXT_MAX) return 0;
        return skip(n);
    }
    default:
        rd_log("remoted: vnc: unknown message %u -- closing\n", type);
        return 0;
    }
}

int rd_vnc_session(const struct rd_conf *c, uint32_t peer) {
    static struct vnc v;
    memset(&v, 0, sizeof v);
    v.conf = c;
    v.peer = peer;
    v.view_only = c->view_only;
    rd_pixfmt_native(&v.enc.pf);
    v.enc.encoding = RD_ENC_RAW;

    char ip[16];
    rd_fmt_ip(peer, ip, sizeof ip);
    int rc = ushot_open(&v.shot);
    if (rc < 0) {
        rd_log("remoted: vnc: %s: no screen to share (%s)\n", ip, ushot_strerror(rc));
        return 1;
    }
    v.w = v.shot.screen_w;
    v.h = v.shot.screen_h;
    v.prev = calloc((size_t)v.w * v.h, 4);
    if (!v.prev) { ushot_close(&v.shot); return 1; }

    int minor = 8;
    if (!handshake(&v, &minor) || !admitted(&v)) {
        ushot_close(&v.shot);
        return 1;
    }
    uint8_t shared;
    if (!read_full(&shared, 1) || !server_init(&v)) {
        ushot_close(&v.shot);
        return 1;
    }
    rd_log("remoted: vnc: %s connected (%dx%d)\n", ip, v.w, v.h);

    int ok = 1;
    while (ok) {
        uint8_t type;
        int r = read_first(&type);
        if (r < 0) break;
        if (r == 1) {
            ok = message(&v, type);
            continue;   // drain what the viewer sent before drawing
        }
        if (v.want_update && send_update(&v) < 0) break;
    }

    struct input_inject rel = { INPUT_INJECT_RELEASE, 0, 0, 0 };
    if (!v.view_only) inject(&rel, 1);
    rd_log("remoted: vnc: %s disconnected\n", ip);
    rd_buf_free(&v.out);
    rd_buf_free(&v.enc.scratch);
    free(v.prev);
    ushot_close(&v.shot);
    return 0;
}
