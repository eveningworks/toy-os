// A ring-3 window client that draws like an actual application: real
// anti-aliased text, a labelled button, and state that changes in
// response to input -- all rendered by the process itself, in its own
// window, with the kernel only compositing finished pixels.
//
// This is the app-shaped counterpart to winclient.c (which proves the
// protocol with flat colour fills). What it adds is the piece every
// real app needs and no ring-3 program here could do before: TEXT,
// using the desktop's own font mapped read-only via WIN_REQ_FONT. Same
// glyph data the kernel's own UI draws with, so this window's text is
// identical to the desktop's rather than a second, drifting copy.
//
// What it does: shows a counter and a hint line, plus a button. Click
// the button (or press space/enter) to increment; 'r' resets; Esc, 'q',
// or the window's close button exit. Every state change repaints into
// the shared buffer and asks the server to composite it.
//
// Each interaction is also logged as one parseable line
// (`uiclient: count 3`) so tools/uiclient_test.py can assert on
// behaviour rather than on pixels alone -- the same idea as
// apps/uidemo.c's log grammar, which exists for exactly this reason.
#include <stdint.h>
#include "rt/sys.h"
#include "ui/ugfx.h"







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

static int win_request(struct win_request_msg *req) {
    return (int)sys_call(SYS_WIN_REQUEST, (uint64_t)(uintptr_t)req, 0, 0);
}

static void clear_req(struct win_request_msg *req) {
    for (unsigned i = 0; i < sizeof(*req); i++) ((uint8_t *)req)[i] = 0;
}

static int wait_event(struct win_event *ev) {
    int64_t r;
    do { // see syscall_abi.h: 0 means "woken, ask again", and parks again
        r = sys_call(SYS_WAIT_EVENT, (uint64_t)(uintptr_t)ev, 0, 0);
    } while (r == 0);
    return (int)r;
}

#define WIN_W 300
#define WIN_H 160

#define BG      0xF5F6F7
#define INK     0x1C2833
#define MUTED   0x7F8C8D
#define BTN     0x2E86C1
#define BTN_HOT 0x1B4F72

// Button geometry, in window-relative pixels. Exported through the log
// on startup so a test doesn't re-derive it -- the same reason
// apps/uidemo.c reports its own layout instead of letting the Python
// side hardcode offsets that silently drift.
#define BTN_X 20
#define BTN_Y 100
#define BTN_W 120
#define BTN_H 34

static int hit_button(int x, int y) {
    return x >= BTN_X && x < BTN_X + BTN_W && y >= BTN_Y && y < BTN_Y + BTN_H;
}

static void draw(struct ugfx_surface *s, int count, int pressed) {
    ugfx_fill(s, BG);

    char label[40];
    int i = 0;
    const char *pre = "Clicks: ";
    while (pre[i]) { label[i] = pre[i]; i++; }
    char digits[12];
    int d = 0, n = count;
    if (n == 0) digits[d++] = '0';
    while (n > 0) { digits[d++] = (char)('0' + n % 10); n /= 10; }
    while (d > 0) label[i++] = digits[--d];
    label[i] = '\0';

    ugfx_draw_string(s, 20, 24, "Drawing its own text", MUTED, BG);
    ugfx_draw_string(s, 20, 24 + ugfx_char_h() + 14, label, INK, BG);

    ugfx_fill_rect(s, BTN_X, BTN_Y, BTN_W, BTN_H, pressed ? BTN_HOT : BTN);
    const char *btn = "Count";
    int tx = BTN_X + (BTN_W - ugfx_text_width(btn)) / 2;
    int ty = BTN_Y + (BTN_H - ugfx_char_h()) / 2;
    ugfx_draw_string(s, tx, ty, btn, 0xFFFFFF, pressed ? BTN_HOT : BTN);

    ugfx_draw_rect(s, 0, 0, s->w, s->h, 0xD5D8DC);
}

int main(void) {
    struct win_request_msg req;

    clear_req(&req);
    req.type = WIN_REQ_CREATE;
    req.a = WIN_W;
    req.b = WIN_H;
    req.c = 300;
    req.d = 220;
    if (win_request(&req) != 1) { log_line("uiclient: create failed\n"); sys_exit(1); }
    uint32_t id = req.window;

    if (!ugfx_font_init()) { log_line("uiclient: font failed\n"); sys_exit(2); }

    clear_req(&req);
    req.type = WIN_REQ_TITLE;
    req.window = id;
    const char *title = "Counter (ring 3)";
    int t = 0;
    for (; title[t] && t < WIN_TITLE_LEN - 1; t++) req.text[t] = title[t];
    req.text[t] = '\0';
    win_request(&req);

    struct ugfx_surface s = ugfx_surface_for_window(id, WIN_W, WIN_H);
    int count = 0;
    draw(&s, count, 0);

    clear_req(&req);
    req.type = WIN_REQ_PRESENT;
    req.window = id;
    win_request(&req);

    log_line("uiclient: ready\n");
    log_line("uiclient: layout btn 20 100 120 34\n");
    log_count(count);

    for (;;) {
        struct win_event ev;
        if (wait_event(&ev) != 1) break;

        int quit = 0, changed = 0;

        if (ev.type == WIN_EV_CLOSE) {
            quit = 1;
        } else if (ev.type == WIN_EV_KEY) {
            if (ev.a == 0x1B || ev.a == 'q') quit = 1;
            else if (ev.a == ' ' || ev.a == '\n' || ev.a == '\r') { count++; changed = 1; }
            else if (ev.a == 'r') { count = 0; changed = 1; }
        } else if (ev.type == WIN_EV_MOUSE_DOWN) {
            if (hit_button(ev.a, ev.b)) { count++; changed = 1; }
        }

        if (quit) break;
        if (changed) {
            draw(&s, count, 0);
            clear_req(&req);
            req.type = WIN_REQ_PRESENT;
            req.window = id;
            win_request(&req);
            log_count(count);
        }
    }

    clear_req(&req);
    req.type = WIN_REQ_DESTROY;
    req.window = id;
    win_request(&req);

    log_line("uiclient: exiting\n");
    sys_exit(0);
}
