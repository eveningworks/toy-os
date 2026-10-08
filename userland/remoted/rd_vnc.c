// One VNC viewer: RFB 3.8 (RFC 6143) on fd 0 and fd 1. See rd.h.
//
// THE SCREEN IS WHAT THE COMPOSITOR SAYS CHANGED. A session is a
// CASTER (lib/ushot.h's ushot_damage()): toywm keeps the rectangles it
// repainted since the last capture, sends WIN_EV_CAST when there are new
// ones, and copies only those into this process's mirror of the screen.
// The rectangles are merged, so each is still compared with the frame
// the viewer has in 64x64 tiles, and only tiles that really differ go
// out. Grabbing and comparing the whole screen 30 times a second was the
// first version; it made a viewer feel late (62 ms per pointer move on
// the ASUS, a full repaint per grab in the compositor).
//
// THE VIEWER DRAWS THE POINTER when it offers RFB's Cursor
// pseudo-encoding (every current viewer does): its shape is sent when it
// changes, and moving the mouse sends no pixels at all. A viewer without
// it gets the pointer drawn into the picture, and a move is damage.
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
#include <signal.h>
#include <stdio.h>
#include "lib/uwmchan.h"
#include "utls.h"
#include "query_abi.h"   // QUERY_REMOTE_STATUS
#include "lib/uclip.h"
#include "lib/ucharset.h"

#define POLL_MS 10              // the read deadline: how often the compositor's
                                // nudge is looked for while the viewer is quiet
#define CUT_TEXT_MAX (1 << 20)  // a viewer's cut text past this ends the session
#define ENCODINGS_MAX 64

#define CMP_TILE 64
#define TILES_X_MAX (8192 / CMP_TILE)
#define TILES_Y_MAX (8192 / CMP_TILE)
#define CURSOR_MAX 64
#define STATS_NS 10000000000ull    // a summary line this often, while busy

struct vnc {
    const struct uremote_conf *conf;
    uint32_t peer;
    struct rd_enc enc;
    struct ushot shot;
    uint32_t *prev;           // the frame the viewer has
    int w, h;
    int want_update, want_full;
    int ux, uy, uw, uh;       // the requested region
    int desktop_size;         // the viewer understands DesktopSize
    int cursor_enc;           // ...and draws the pointer itself
    int damage_pending;       // the compositor says pixels changed
    int cursor_dirty;         // ...or the pointer's shape did
    int tx, ty;               // the tile grid's size
    uint8_t dirty[TILES_Y_MAX][TILES_X_MAX];   // differs from what the viewer has
    uint8_t buttons;
    int view_only;
    unsigned clip_seen;       // the clipboard serial the viewer has, or set itself
    int tls_ready;            // this machine has a key to offer VeNCrypt with
    int encrypted;            // ...and this viewer took it
    struct rd_buf out;
    // What the summary line reports.
    unsigned long long st_t0, st_cap, st_enc, st_send, st_bytes;
    int st_updates;
};

static unsigned long long now_ns(void) { return sys_monotonic_ns(); }

// --- the socket -----------------------------------------------------------
//
// ONE PAIR OF FUNCTIONS FOR EVERY BYTE: the socket until VeNCrypt's
// handshake, the TLS session after it. Nothing above this section knows
// which, so the protocol is written once.
static struct utls *g_tls;

static long io_read(void *buf, size_t n) {
    return g_tls ? utls_read(g_tls, buf, n) : read(0, buf, n);
}

static long io_write(const void *buf, size_t n) {
    return g_tls ? utls_write(g_tls, buf, n) : write(1, buf, n);
}

// THE HANDSHAKE'S DEADLINE (OpenSSH's LoginGraceTime): while it is set,
// a read that would outlast it fails. Without one a viewer that connects
// and says nothing holds one of SESSIONS_MAX slots for ever, and two of
// them lock everybody out. Not set across the question at the screen --
// that wait is the person's, not the viewer's.
#define HANDSHAKE_S 30
static unsigned long long g_deadline_ns;

static void deadline(int s) {
    g_deadline_ns = s ? sys_monotonic_ns() + (unsigned long long)s * 1000000000ull : 0;
}

// Reads exactly `n` bytes: as long as it takes, or until the deadline.
// 0 when the viewer went away or ran out of time.
static int read_full(void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        long r;
        if (g_deadline_ns) {
            unsigned long long now = sys_monotonic_ns();
            if (now >= g_deadline_ns) return 0;
            int ms = (int)((g_deadline_ns - now) / 1000000ull) + 1;
            if (g_tls) {
                r = utls_read_timeout(g_tls, p, n, ms);
            } else {
                uint32_t src;
                uint16_t port;
                r = (long)sys_recvfrom(0, p, n, &src, &port, ms);
            }
        } else {
            r = io_read(p, n);
        }
        if (r <= 0) return 0;   // a close, a timeout and an error alike
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
    if (g_tls) {
        long r = utls_read_timeout(g_tls, b, 1, POLL_MS);
        return r == 1 ? 1 : r == UTLS_TIMEOUT ? 0 : -1;
    }
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
        long r = io_write(p, n);
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

#define RFB_SEC_VNC        2
#define RFB_SEC_VENCRYPT  19
#define VENCRYPT_X509VNC  261   // TLS with our certificate, then VNC Authentication
#define VENCRYPT_X509PLAIN 262  // TLS with our certificate, then a user name and password

// VeNCrypt (TigerVNC's, RFB security type 19): agree version 0.2, offer
// the two X509 subtypes, take the viewer's pick, run the TLS handshake,
// then the inner check over TLS. 1 the password was right, 0 wrong, -1
// the negotiation failed. From here every byte is encrypted (g_tls).
//
// X509PLAIN SENDS THE WHOLE PASSWORD, inside TLS, and it is compared
// whole -- unlike VNC Authentication's 8-character DES key. There is no
// user on this machine, so the name is read and ignored.
static int vencrypt(struct vnc *v, const char *pw) {
    char ip[16];
    uremote_fmt_ip(v->peer, ip, sizeof ip);
    uint8_t ver[2] = { 0, 2 }, cver[2], ack = 0;
    if (!write_full(ver, 2) || !read_full(cver, 2)) return -1;
    if (cver[0] != 0 || cver[1] != 2) {
        ack = 0xFF;   // "cannot do that version", VeNCrypt's word for it
        write_full(&ack, 1);
        return -1;
    }
    uint8_t sub[9] = { 2, 0, 0, VENCRYPT_X509VNC >> 8, VENCRYPT_X509VNC & 0xFF,
                       0, 0, VENCRYPT_X509PLAIN >> 8, VENCRYPT_X509PLAIN & 0xFF };
    uint8_t pick[4];
    if (!write_full(&ack, 1) || !write_full(sub, sizeof sub) || !read_full(pick, 4)) return -1;
    uint32_t st = be32(pick);
    uint8_t yes = st == VENCRYPT_X509VNC || st == VENCRYPT_X509PLAIN;
    if (!write_full(&yes, 1) || !yes) return -1;

    char err[160];
    g_tls = utls_accept(0, RD_TLS_KEY, RD_TLS_CRT, err, sizeof err);
    if (!g_tls) {
        rd_log("remoted: vnc: %s: TLS failed: %s\n", ip, err);
        return -1;
    }
    v->encrypted = 1;
    rd_log("remoted: vnc: %s: encrypted (%s, %s)\n", ip, utls_version(g_tls),
           utls_ciphersuite(g_tls));
    if (st == VENCRYPT_X509VNC) return vnc_auth(pw);

    uint8_t len[8];
    if (!read_full(len, 8)) return -1;
    uint32_t ul = be32(len), pl = be32(len + 4);
    if (ul > 255 || pl > 255) return -1;
    char user[256], pass[256];
    if (!read_full(user, ul) || !read_full(pass, pl)) return -1;
    pass[pl] = 0;
    // Constant time over the longer of the two, so the length of the
    // right password is not on the wire's clock either.
    size_t a = strlen(pw), n = a > pl ? a : pl;
    uint8_t diff = (uint8_t)(a != pl);
    for (size_t i = 0; i < n; i++)
        diff |= (uint8_t)((i < a ? pw[i] : 0) ^ (i < pl ? pass[i] : 0));
    memset(pass, 0, sizeof pass);
    return diff == 0;
}

static int admitted(struct vnc *v);

// The version exchange, security and authentication. 1 when the viewer
// is in.
static int handshake(struct vnc *v, int *minor) {
    deadline(HANDSHAKE_S);
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
    // WHICH SECURITY: VeNCrypt (TLS) when this machine can make a key and
    // the setting allows it, VNC Authentication when the setting allows
    // that. RFB 3.3 has no negotiation at all, so it gets the plain kind
    // or nothing.
    int tls_ok = v->tls_ready && v->conf->vnc.encryption != UREMOTE_ENC_OFF;
    int plain_ok = v->conf->vnc.encryption != UREMOTE_ENC_REQUIRE;
    if (!plain_ok && (!tls_ok || *minor == 3)) {
        rd_log("remoted: vnc: refused -- encryption is required and %s\n",
               *minor == 3 ? "this viewer speaks RFB 3.3" : "TLS is not available here");
        if (*minor == 3) { uint8_t z[4] = { 0 }; write_full(z, 4); }
        else             { uint8_t z = 0; write_full(&z, 1); }
        send_reason("This machine requires an encrypted connection (VeNCrypt).");
        return 0;
    }
    int ok;
    if (*minor == 3) {
        uint8_t t[4] = { 0, 0, 0, 2 };
        if (!write_full(t, 4)) return 0;
        ok = vnc_auth(pw);
    } else {
        // THE ORDER IS A DECISION: plain VNC Authentication FIRST when
        // both are offered. libvncclient (Remmina) takes the first type it
        // knows in the SERVER's list, and for VeNCrypt's X509 subtypes it
        // demands a CA file -- with a self-signed certificate and none
        // configured it gives up ("No CA certificate provided") rather
        // than fall back. TigerVNC picks by its OWN preference, VeNCrypt
        // first, so it still encrypts. TigerVNC's server dodges this with
        // anonymous TLS (TLSVnc), which mbedTLS does not do.
        uint8_t t[3], n = 0;
        if (plain_ok) t[1 + n++] = RFB_SEC_VNC;
        if (tls_ok) t[1 + n++] = RFB_SEC_VENCRYPT;
        t[0] = n;
        uint8_t pick;
        if (!write_full(t, 1 + n) || !read_full(&pick, 1)) return 0;
        if (pick == RFB_SEC_VENCRYPT && tls_ok) {
            int r = vencrypt(v, pw);
            if (r < 0) return 0;
            ok = r;
        } else if (pick == RFB_SEC_VNC && plain_ok) {
            ok = vnc_auth(pw);
        } else {
            return 0;
        }
    }
    // THE SECOND QUESTION BEFORE THE ANSWER: the password is right, but
    // the result is not sent until "When someone connects" says yes too
    // -- so a viewer that is turned away hears why, in RFB's own words.
    deadline(0);
    int refused = ok && !admitted(v);
    uint8_t res[4] = { 0, 0, 0, ok && !refused ? 0 : 1 };
    write_full(res, 4);
    if (refused) {
        if (*minor == 8) send_reason("The person at this computer did not allow the connection.");
        return 0;
    }
    if (!ok) {
        if (*minor == 8) send_reason("Authentication failed.");
        char ip[16];
        uremote_fmt_ip(v->peer, ip, sizeof ip);
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
// Asks the person at the screen: the compositor's corner notice
// (WIN_NOTICE_REMOTE), answered as WIN_EV_REMOTE_ANSWER on this
// process's channel. Waits a little past the card's own time, so the
// card's deny is what normally ends it. Returns WIN_REMOTE_*.
static int ask(struct vnc *v, int *always) {
    char ip[16];
    uremote_fmt_ip(v->peer, ip, sizeof ip);
    struct wmchan_msg m, r;
    memset(&m, 0, sizeof m);
    m.type = WIN_REQ_NOTICE;
    m.a = 0 | (1 << 8);
    m.b = WIN_NOTICE_REMOTE;
    // "Ask every time" ignores the trusted list, so a box that would add
    // to it would promise something the next connection does not keep.
    m.c = v->conf->when == UREMOTE_ASK ? WIN_NOTICE_F_NO_ALWAYS : 0;
    snprintf(m.text, sizeof m.text, "%s VNC", ip);
    if (!v->shot.chan || uchan_call(v->shot.chan, &m, sizeof m, &r, sizeof r, 2000) != 0)
        return WIN_REMOTE_DENY;   // no desktop to ask on
    rd_log("remoted: vnc: %s: asking at the screen\n", ip);
    unsigned long long end = now_ns() + (WIN_REMOTE_ASK_S + 3) * 1000000000ull;
    while (now_ns() < end) {
        uchan_client_wait(v->shot.chan, 250);
        struct win_event ev;
        while (uchan_client_recv(v->shot.chan, &ev, sizeof ev) == 1) {
            if (ev.type != WIN_EV_REMOTE_ANSWER) continue;
            *always = ev.b != 0;
            return ev.a;
        }
    }
    return WIN_REMOTE_DENY;
}

// May this viewer in, once its password is right? "When someone
// connects": always, a trusted address, or the person at the screen.
static int admitted(struct vnc *v) {
    const struct uremote_conf *c = v->conf;
    if (c->when == UREMOTE_ALWAYS) return 1;
    if (c->when == UREMOTE_ASK_UNLESS_TRUSTED && uremote_trusted(c, v->peer)) return 1;
    int always = 0;
    int a = ask(v, &always);
    char ip[16];
    uremote_fmt_ip(v->peer, ip, sizeof ip);
    rd_log("remoted: vnc: %s: %s%s\n", ip,
           a == WIN_REMOTE_ALLOW ? "allowed" : a == WIN_REMOTE_VIEW ? "allowed, view only" : "denied",
           always && a != WIN_REMOTE_DENY ? ", and trusted from now on" : "");
    if (a == WIN_REMOTE_DENY) return 0;
    if (a == WIN_REMOTE_VIEW) v->view_only = 1;
    // The person said so at the screen, so it is saved for them -- the
    // address alone, as the trusted list always is.
    if (always && uremote_trust(ip, "allowed at the screen") != 0)
        rd_log("remoted: vnc: could not save %s to the trusted list\n", ip);
    return 1;
}

// What QUERY_REMOTESESS shows for this session -- the tray flyout's and
// System Settings' line.
static void set_status(const struct vnc *v) {
    char s[QUERY_REMOTESESS_STATUS_MAX];
    snprintf(s, sizeof s, "VNC, %s%s", v->view_only ? "view only" : "full control",
             v->encrypted ? ", encrypted" : "");
    sys_remote_log(QUERY_REMOTE_STATUS, 0, s);
}

// SIGUSR1: view only; SIGUSR2: control back (lib/uremote.h). The handler
// only records it; the loop applies it.
static volatile int g_want_view = -1;
static void on_usr(int sig) { g_want_view = sig == SIGUSR1; }

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

static int tile_w(const struct vnc *v, int tx) {
    return v->w - tx * CMP_TILE < CMP_TILE ? v->w - tx * CMP_TILE : CMP_TILE;
}
static int tile_h(const struct vnc *v, int ty) {
    return v->h - ty * CMP_TILE < CMP_TILE ? v->h - ty * CMP_TILE : CMP_TILE;
}

// Asks the compositor for what changed and marks the tiles that differ
// from what the viewer has. A tile stays marked until it is SENT, so a
// change outside the region a viewer asked for is not forgotten.
static int take_damage(struct vnc *v) {
    struct win_damage d;
    int rc = ushot_damage(&v->shot, v->cursor_enc ? 0 : WIN_SHOT_POINTER, &d);
    if (rc < 0) return rc;
    v->damage_pending = 0;
    for (int k = 0; k < d.n; k++) {
        int x0 = d.r[k].x / CMP_TILE, y0 = d.r[k].y / CMP_TILE;
        int x1 = (d.r[k].x + d.r[k].w + CMP_TILE - 1) / CMP_TILE;
        int y1 = (d.r[k].y + d.r[k].h + CMP_TILE - 1) / CMP_TILE;
        if (x1 > v->tx) x1 = v->tx;
        if (y1 > v->ty) y1 = v->ty;
        for (int ty = y0; ty < y1; ty++)
            for (int tx = x0; tx < x1; tx++)
                if (!v->dirty[ty][tx] &&
                    tile_changed(v, tx * CMP_TILE, ty * CMP_TILE, tile_w(v, tx), tile_h(v, ty)))
                    v->dirty[ty][tx] = 1;
    }
    return 0;
}

// RFB's Cursor pseudo-encoding (7.8.1): the shape in the viewer's pixel
// format, then a bitmask, 1 = drawn, rows padded to a byte.
static void cursor_rect(struct vnc *v, int *nrects) {
    static uint32_t img[CURSOR_MAX * CURSOR_MAX];
    int w, h, hx, hy;
    if (ushot_cursor(&v->shot, img, CURSOR_MAX * CURSOR_MAX, &w, &h, &hx, &hy) < 0) return;
    v->cursor_dirty = 0;
    rd_enc_cursor(&v->enc, img, w, h, hx, hy, &v->out);
    (*nrects)++;
}

// What the session's updates cost, as one log line: every STATS_NS
// while busy, and once at the end.
static void stats_line(struct vnc *v, unsigned long long now) {
    int u = v->st_updates;
    if (u)
        rd_log("remoted: vnc: %d updates in %llu s: capture %llu, encode %llu, send %llu us each, "
               "%llu KB\n", u, (now - v->st_t0) / 1000000000ull, v->st_cap / 1000 / u,
               v->st_enc / 1000 / u, v->st_send / 1000 / u, v->st_bytes / 1024);
    v->st_t0 = now;
    v->st_updates = 0;
    v->st_cap = v->st_enc = v->st_send = v->st_bytes = 0;
}

// One FramebufferUpdate: the pointer's shape if it changed, then the
// marked tiles in the requested region, merged along each row of tiles.
// 1 sent, 0 nothing to send yet, -1 the connection failed.
static int send_update(struct vnc *v) {
    unsigned long long t0 = now_ns();
    if (v->damage_pending || v->want_full) {
        int rc = take_damage(v);
        if (rc == -EBUSY) return 0;   // a fullscreen program has the display
        if (rc < 0) return -1;
    }
    unsigned long long t1 = now_ns();
    int x0 = v->ux / CMP_TILE, y0 = v->uy / CMP_TILE;
    int x1 = (v->ux + v->uw + CMP_TILE - 1) / CMP_TILE;
    int y1 = (v->uy + v->uh + CMP_TILE - 1) / CMP_TILE;
    if (x1 > v->tx) x1 = v->tx;
    if (y1 > v->ty) y1 = v->ty;
    if (v->want_full)
        for (int ty = y0; ty < y1; ty++)
            for (int tx = x0; tx < x1; tx++) v->dirty[ty][tx] = 1;

    // The header goes first with a placeholder count, patched below.
    v->out.len = 0;
    rd_buf_u8(&v->out, 0);
    rd_buf_u8(&v->out, 0);
    rd_buf_u16(&v->out, 0);
    int n = 0;
    if (v->cursor_enc && v->cursor_dirty) cursor_rect(v, &n);
    for (int ty = y0; ty < y1; ty++) {
        for (int tx = x0; tx < x1;) {
            if (!v->dirty[ty][tx]) { tx++; continue; }
            int run = tx;
            while (tx < x1 && v->dirty[ty][tx]) v->dirty[ty][tx++] = 0;
            int px = run * CMP_TILE, py = ty * CMP_TILE;
            int pw = (tx == v->tx ? v->w : tx * CMP_TILE) - px, ph = tile_h(v, ty);
            rd_enc_rect(&v->enc, v->shot.px, v->w, px, py, pw, ph, &v->out);
            copy_rect(v, px, py, pw, ph);
            n++;
        }
    }
    if (!n) return 0;
    v->out.p[2] = (uint8_t)(n >> 8);
    v->out.p[3] = (uint8_t)n;
    v->want_update = v->want_full = 0;
    unsigned long long t2 = now_ns();
    size_t bytes = v->out.len;
    int ok = flush(v);
    unsigned long long t3 = now_ns();

    v->st_updates++;
    v->st_cap += t1 - t0;
    v->st_enc += t2 - t1;
    v->st_send += t3 - t2;
    v->st_bytes += bytes;
    if (t3 - v->st_t0 >= STATS_NS) stats_line(v, t3);
    return ok ? 1 : -1;
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
        if (enc == -239) v->cursor_enc = 1;
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

// --- the clipboard ---------------------------------------------------------
//
// RFB's cut text is LATIN-1 (RFB 7.5.6, 7.6.4) and toy-os's is UTF-8, so
// each way is converted; a character past Latin-1 goes to a viewer as '?'.
// The UTF-8 Extended Clipboard is not offered -- Debian's libvncclient,
// Remmina's, does not speak it. Text only, as every VNC server sends.

// ClientCutText: the viewer copied `n` bytes. A view-only viewer's are
// read and dropped, as TigerVNC and x11vnc drop them.
static int cut_text_in(struct vnc *v, uint32_t n) {
    static char raw[UCLIP_TEXT_MAX], u8[UCLIP_TEXT_MAX + 1];
    if (v->view_only || n > sizeof raw) return skip(n);
    if (!read_full(raw, n)) return 0;
    long len = ucharset_latin1_to_utf8(u8, sizeof u8, raw, n);
    if (len < 0 || !uclip_set_text(u8, (int)len)) {
        rd_log("remoted: vnc: the viewer's clipboard did not fit on this one\n");
        return 1;
    }
    v->clip_seen = uclip_peek_serial();   // the viewer has it: no echo
    return 1;
}

// ServerCutText when this machine's clipboard changed. 0 on a write
// failure (the viewer has gone).
static int cut_text_out(struct vnc *v) {
    static struct uclip c;
    static uint8_t msg[8 + UCLIP_TEXT_MAX + 1];
    v->clip_seen = uclip_peek_serial();
    int n;
    const char *t = uclip_load(&c) ? uclip_text(&c, &n) : 0;
    if (!t) return 1;   // files, or empty: nothing a viewer can take
    long len = ucharset_utf8_to_latin1((char *)msg + 8, sizeof msg - 8, t, (size_t)n, '?');
    if (len < 0) return 1;
    msg[0] = 3;                                      // ServerCutText
    msg[1] = msg[2] = msg[3] = 0;
    msg[4] = (uint8_t)(len >> 24); msg[5] = (uint8_t)(len >> 16);
    msg[6] = (uint8_t)(len >> 8);  msg[7] = (uint8_t)len;
    return write_full(msg, 8 + (size_t)len);
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
        v->cursor_dirty = 1;
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
        // Negative is the Extended Clipboard form, never offered: skipped.
        uint32_t n = len < 0 ? (uint32_t)-len : (uint32_t)len;
        if (n > CUT_TEXT_MAX) return 0;
        return len < 0 ? skip(n) : cut_text_in(v, n);
    }
    default:
        rd_log("remoted: vnc: unknown message %u -- closing\n", type);
        return 0;
    }
}

int rd_vnc_session(const struct uremote_conf *c, uint32_t peer) {
    static struct vnc v;
    memset(&v, 0, sizeof v);
    v.conf = c;
    v.peer = peer;
    v.view_only = c->view_only;
    rd_pixfmt_native(&v.enc.pf);
    v.enc.encoding = RD_ENC_RAW;

    char ip[16];
    uremote_fmt_ip(peer, ip, sizeof ip);
    int rc = ushot_open(&v.shot);
    if (rc < 0) {
        rd_log("remoted: vnc: %s: no screen to share (%s)\n", ip, ushot_strerror(rc));
        return 1;
    }
    v.w = v.shot.screen_w;
    v.h = v.shot.screen_h;
    v.prev = calloc((size_t)v.w * v.h, 4);
    if (!v.prev) { ushot_close(&v.shot); return 1; }

    // TLS ONLY WITH A KEY ALREADY MADE: the listener makes it (remoted.c),
    // so a session never stalls a viewer on key generation.
    v.tls_ready = c->vnc.encryption != UREMOTE_ENC_OFF && access(RD_TLS_KEY, R_OK) == 0 &&
                  access(RD_TLS_CRT, R_OK) == 0;
    int minor = 8;
    if (!handshake(&v, &minor)) {   // the password, then "When someone connects"
        ushot_close(&v.shot);
        return 1;
    }
    uint8_t shared;
    deadline(HANDSHAKE_S);
    int in = read_full(&shared, 1) && server_init(&v);
    deadline(0);
    if (!in) {
        ushot_close(&v.shot);
        return 1;
    }
    rd_log("remoted: vnc: %s connected (%dx%d)%s\n", ip, v.w, v.h, v.view_only ? ", view only" : "");
    set_status(&v);
    signal(SIGUSR1, on_usr);
    signal(SIGUSR2, on_usr);

    v.tx = (v.w + CMP_TILE - 1) / CMP_TILE;
    v.ty = (v.h + CMP_TILE - 1) / CMP_TILE;
    v.damage_pending = v.cursor_dirty = 1;
    v.clip_seen = uclip_peek_serial();   // sent when it CHANGES, as every server does
    v.st_t0 = now_ns();
    int ok = 1;
    while (ok) {
        uint8_t type;
        int r = read_first(&type);
        if (r < 0) break;
        if (r == 1) {
            ok = message(&v, type);
            continue;   // drain what the viewer sent before drawing
        }
        if (g_want_view >= 0) {
            if (g_want_view && !v.view_only) {
                // Let go of anything the viewer is holding, or it stays down.
                struct input_inject rel = { INPUT_INJECT_RELEASE, 0, 0, 0 };
                inject(&rel, 1);
            }
            v.view_only = g_want_view;
            g_want_view = -1;
            set_status(&v);
            rd_log("remoted: vnc: %s now %s\n", ip, v.view_only ? "view only" : "in control");
        }
        if (uclip_peek_serial() != v.clip_seen && !cut_text_out(&v)) break;
        int bits = ushot_events(&v.shot);
        if (bits & WIN_CAST_DAMAGE) v.damage_pending = 1;
        if (bits & WIN_CAST_CURSOR) v.cursor_dirty = 1;
        if (v.want_update && (v.damage_pending || v.want_full || v.cursor_dirty) &&
            send_update(&v) < 0)
            break;
    }

    struct input_inject rel = { INPUT_INJECT_RELEASE, 0, 0, 0 };
    if (!v.view_only) inject(&rel, 1);
    stats_line(&v, now_ns());
    rd_log("remoted: vnc: %s disconnected\n", ip);
    rd_buf_free(&v.out);
    rd_buf_free(&v.enc.scratch);
    free(v.prev);
    ushot_close(&v.shot);
    if (g_tls) utls_close(g_tls);
    g_tls = 0;
    return 0;
}
