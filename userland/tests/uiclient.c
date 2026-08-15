// A ring-3 window client that draws like an actual application: real
// anti-aliased text, a labelled button, and state that changes in
// response to input -- all rendered by the process itself, in its own
// window, with TWS only compositing finished pixels.
//
// This is the app-shaped counterpart to winclient.c (which proves TWP
// with flat colour fills). What it adds is the piece every real app
// needs and no ring-3 program here could do before: TEXT, using the
// desktop's own font mapped read-only via WIN_REQ_FONT. Same glyph data
// the kernel's own UI draws with, so this window's text is identical to
// the desktop's rather than a second, drifting copy.
//
// What it does: shows a counter and a hint line, plus a button. Click
// the button (or press space/enter) to increment; 'r' resets; Esc, 'q',
// or the window's close button exit.
//
// Each interaction is logged as one parseable line (`uiclient: count 3`)
// so tools/uiclient_test.py can assert on behaviour rather than on
// pixels alone -- the same idea as apps/uidemo.c's log grammar.
//
// Ported to Toykit: the create/title/present/destroy handshake, the
// blocking loop and its switch, and the private win_request()/
// clear_req()/wait_event() helpers are all gone (see ui/uapp.h). The
// button is still hand-hit-tested rather than routed through
// `desc.buttons`, on purpose -- it is drawn as a flat rectangle rather
// than as a uui_button, and this file's job is to exercise ugfx text
// rendering, not the widget toolkit. That uiclient can still do its own
// hit-testing while using the app framework is itself the point: the
// two compose.
#include <stdint.h>
#include "rt/sys.h"
#include "ui/uapp.h"

static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void log_line(const char *s) {
    sys_call(SYS_WRITE, 1, (uint64_t)(uintptr_t)s, (uint64_t)str_len(s));
}

// Small fixed-buffer int formatter -- there is no libc here.
static void log_count(int n) {
    char buf[32];
    int i = 0;
    const char *pre = "uiclient: count ";
    while (pre[i]) { buf[i] = pre[i]; i++; }

    char digits[12];
    int d = 0;
    if (n == 0) digits[d++] = '0';
    while (n > 0) { digits[d++] = (char)('0' + n % 10); n /= 10; }
    while (d > 0) buf[i++] = digits[--d];
    buf[i++] = '\n';
    buf[i] = '\0';
    log_line(buf);
}

#define WIN_W 300
#define WIN_H 160

#define BG      0xF5F6F7
#define INK     0x1C2833
#define MUTED   0x7F8C8D
#define BTN     0x2E86C1

// Button geometry, in window-relative pixels. Exported through the log
// on startup so a test doesn't re-derive it -- the same reason
// apps/uidemo.c reports its own layout instead of letting the Python
// side hardcode offsets that silently drift.
#define BTN_X 20
#define BTN_Y 100
#define BTN_W 120
#define BTN_H 34

static int g_count;

static int hit_button(int x, int y) {
    return x >= BTN_X && x < BTN_X + BTN_W && y >= BTN_Y && y < BTN_Y + BTN_H;
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    struct ugfx_surface *s = uapp_surface(d);
    ugfx_fill(s, BG);

    char label[40];
    int i = 0;
    const char *pre = "Clicks: ";
    while (pre[i]) { label[i] = pre[i]; i++; }
    char digits[12];
    int dg = 0, n = g_count;
    if (n == 0) digits[dg++] = '0';
    while (n > 0) { digits[dg++] = (char)('0' + n % 10); n /= 10; }
    while (dg > 0) label[i++] = digits[--dg];
    label[i] = '\0';

    ugfx_draw_string(s, 20, 24, "Drawing its own text", MUTED, BG);
    ugfx_draw_string(s, 20, 24 + ugfx_char_h() + 14, label, INK, BG);

    ugfx_fill_rect(s, BTN_X, BTN_Y, BTN_W, BTN_H, BTN);
    const char *btn = "Count";
    int tx = BTN_X + (BTN_W - ugfx_text_width(btn)) / 2;
    int ty = BTN_Y + (BTN_H - ugfx_char_h()) / 2;
    ugfx_draw_string(s, tx, ty, btn, 0xFFFFFF, BTN);

    ugfx_draw_rect(s, 0, 0, s->w, s->h, 0xD5D8DC);
}

static void bump(struct uapp *a, int to) {
    g_count = to;
    uapp_redraw(a);
    log_count(g_count);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    if (key == 'q') uapp_quit(a, 0); // Esc no longer closes -- Alt+F4 does
    else if (key == ' ' || key == '\n' || key == '\r') bump(a, g_count + 1);
    else if (key == 'r') bump(a, 0);
}

static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    (void)buttons;
    if (hit_button(x, y)) bump(a, g_count + 1);
}

static void on_open(struct uapp *a) {
    (void)a;
    log_line("uiclient: ready\n");
    log_line("uiclient: layout btn 20 100 120 34\n");
    log_count(g_count);
}

int main(void) {
    struct uapp_desc desc = {
        .title    = "Counter (ring 3)",
        .w        = WIN_W,
        .h        = WIN_H,
        .x        = 300,
        .y        = 220,
        .on_open  = on_open,
        .on_draw  = on_draw,
        .on_key   = on_key,
        .on_press = on_press,
    };
    int rc = uapp_run(&desc);
    log_line("uiclient: exiting\n");
    return rc;
}
