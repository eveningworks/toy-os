// Shapes -- a ring-3 client demonstrating the geometry primitives.
//
// A wireframe triangle and an ellipse, both rotating, drawn with the
// shared geometry module (kernel/lib/geom.c) through the canvas widget
// (userland/uwidgets.c). Every pixel here comes from code the KERNEL
// also uses -- the same Bresenham, the same ellipse rasteriser, the
// same fixed-point trig -- compiled a second time for ring 3.
//
// It is also a demo of the ported widget toolkit: the checkbox, the
// button group and the canvas are all doing real work, and the
// anti-aliasing toggle exists because switching it off mid-spin is the
// clearest possible demonstration of what AA is for. Aliased edges
// crawl as the shape turns; smoothed ones do not.
//
// Every state change is logged as one parseable line (`gfxdemo: aa on`)
// so tools/gfxdemo_test.py can assert on behaviour rather than pixels
// alone -- the grammar apps/uidemo.c established.
#include <stdint.h>
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uapp.h"
#include "ui/utheme.h"
#include "geom.h"
#include "fixed.h"
#include "keyboard.h"

#define WIN_W 520
#define WIN_H 400
#define MARGIN 10

// Controls along the bottom.
#define BTN_SLOWER 1
#define BTN_FASTER 2
#define BTN_RESET  3

static struct uui_canvas g_canvas;
static struct uui_button g_buttons[3];
static struct uui_button_group g_bar;
// The anti-aliasing toggle IS the checkbox now: its `checked` field is
// the single store, so the keyboard shortcut and the click cannot end up
// disagreeing with what is drawn. `g_aa` and `g_checkbox_hover` were two
// separate globals the widget had to be handed on every call.
static struct uui_checkbox g_aa_check;
static fx_t g_angle;            // in turns; wraps naturally
static int g_speed = 3;         // angle steps per frame, in 1/1024 turns
static int g_frames;


// The triangle, as unit-ish points about its own centre. Kept in
// fixed point so geom_transform() can rotate and scale it without the
// app ever touching trigonometry.
static const struct geom_pt TRI[3] = {
    { 0,               -110 * FX_ONE / 1 },
    {  95 * FX_ONE,     55 * FX_ONE },
    { -95 * FX_ONE,     55 * FX_ONE },
};

// Diagnostics go to STDERR, which the kernel routes to the kernel log
// rather than to a terminal. A GUI client has no terminal attached, so
// stdout would land wherever the console's sink happens to point; the
// kernel log reaches the serial console and `dmesg`, which is where
// tools/gfxdemo_test.py reads these lines from.
static void log_line(const char *s) { sys_eprint(s); }

// Appends a decimal integer. There is no printf in ring 3 yet (see
// docs/roadmap.md's C library requirements), and the log grammar is
// worth more than the convenience -- a test that re-derives the canvas
// rect from font metrics in Python drifts silently the first time this
// layout changes, which is the trap tools/uidemo_test.py documents.
static int append_int(char *buf, int n, int v) {
    if (v < 0) { buf[n++] = '-'; v = -v; }
    char d[12];
    int dn = 0;
    if (v == 0) d[dn++] = '0';
    while (v > 0) { d[dn++] = (char)('0' + v % 10); v /= 10; }
    while (dn > 0) buf[n++] = d[--dn];
    return n;
}

static void log_layout(void) {
    // "gfxdemo: layout canvas <x> <y> <w> <h>", content-relative.
    char b[64];
    int n = 0;
    const char *pre = "gfxdemo: layout canvas ";
    while (pre[n]) { b[n] = pre[n]; n++; }
    n = append_int(b, n, g_canvas.x);   b[n++] = ' ';
    n = append_int(b, n, g_canvas.y);   b[n++] = ' ';
    n = append_int(b, n, g_canvas.w);   b[n++] = ' ';
    n = append_int(b, n, g_canvas.h);
    b[n++] = '\n';
    b[n] = '\0';
    log_line(b);
}

static void log_speed(void) {
    char b[48];
    int n = 0;
    const char *pre = "gfxdemo: speed ";
    while (pre[n]) { b[n] = pre[n]; n++; }
    n = append_int(b, n, g_speed);
    b[n++] = '\n';
    b[n] = '\0';
    log_line(b);
}


static int checkbox_y(void) { return WIN_H - MARGIN - ugfx_char_h() - 8; }

static void layout(void) {
    int bar_h = ugfx_char_h() + 14;
    int cw = WIN_W - 2 * MARGIN;
    int ch = WIN_H - 2 * MARGIN - bar_h - 8 - ugfx_char_h() - 8;
    uui_canvas_init(&g_canvas, MARGIN, MARGIN, cw, ch,
                     ugfx_rgb(16, 18, 24), ugfx_rgb(70, 78, 92));

    int bw = 9 * ugfx_char_w();
    int by = MARGIN + ch + 8;
    for (int i = 0; i < 3; i++) {
        uui_button_set_geometry(&g_buttons[i], MARGIN + i * (bw + 6), by, bw, bar_h);
    }
}

static void draw(struct ugfx_surface *s) {
    ugfx_fill(s, UTHEME_PANEL_BG);
    layout();
    uui_canvas_begin(s, &g_canvas);

    int cx = uui_canvas_cx(&g_canvas);
    int cy = uui_canvas_cy(&g_canvas);
    enum geom_aa aa = g_aa_check.checked ? GEOM_AA : GEOM_ALIASED;

    // A few static rings, so there is something to judge the curve
    // rasteriser against while everything else moves.
    uui_canvas_circle(s, &g_canvas, cx, cy, 150, ugfx_rgb(38, 44, 56), aa);
    uui_canvas_circle(s, &g_canvas, cx, cy, 100, ugfx_rgb(38, 44, 56), aa);

    // The rotating ellipse: the SHAPE spins, which only means anything
    // for an ellipse because its axes differ -- a rotating circle would
    // look identical every frame and prove nothing.
    {
        // Sweep its radii with the angle too, so the curve rasteriser
        // is exercised across sizes rather than at one fixed radius.
        fx_t pulse = fx_sin(g_angle * 2);
        int rx = 130 + fx_round(fx_mul(pulse, fx_from_int(25)));
        int ry = 60 + fx_round(fx_mul(pulse, fx_from_int(-20)));

        // An ellipse rotated about its centre, drawn as a polyline of
        // transformed points -- geom_ellipse() itself is axis-aligned,
        // which is the honest primitive; rotation belongs to the caller.
        struct geom_pt pts[48];
        int xs[48], ys[48];
        for (int i = 0; i < 48; i++) {
            fx_t t = (fx_t)(((int64_t)i << FX_SHIFT) / 48);
            pts[i].x = fx_mul(fx_cos(t), fx_from_int(rx));
            pts[i].y = fx_mul(fx_sin(t), fx_from_int(ry));
        }
        geom_transform(pts, 48, g_angle, FX_ONE, cx, cy, xs, ys);
        uui_canvas_polyline(s, &g_canvas, xs, ys, 48, 1, ugfx_rgb(90, 200, 250), aa);
    }

    // The rotating triangle, spinning the other way so the two are
    // visually distinguishable at a glance.
    {
        int xs[3], ys[3];
        geom_transform(TRI, 3, -g_angle, FX_ONE, cx, cy, xs, ys);
        uui_canvas_polyline(s, &g_canvas, xs, ys, 3, 1, ugfx_rgb(250, 190, 90), aa);

        // A dot at each vertex, so the filled-ellipse path is on screen
        // too rather than only the outlines.
        for (int i = 0; i < 3; i++) {
            uui_canvas_fill_ellipse(s, &g_canvas, xs[i] - g_canvas.x, ys[i] - g_canvas.y,
                                     4, 4, ugfx_rgb(250, 230, 180));
        }
    }

    // Controls.
    uui_button_group_draw(&g_bar, s);

    uui_checkbox_set_geometry(&g_aa_check, MARGIN, checkbox_y());
    uui_checkbox_draw(s, &g_aa_check);

    // Readout, right-aligned so it does not jump around as digits change.
    char info[48];
    int n = 0;
    const char *pre = "speed ";
    while (pre[n]) { info[n] = pre[n]; n++; }
    int v = g_speed;
    char d[8];
    int dn = 0;
    if (v == 0) d[dn++] = '0';
    while (v > 0) { d[dn++] = (char)('0' + v % 10); v /= 10; }
    while (dn > 0) info[n++] = d[--dn];
    info[n] = '\0';
    int ix = WIN_W - MARGIN - ugfx_text_width(info);
    ugfx_draw_string(s, ix, checkbox_y(), info, ugfx_rgb(110, 120, 135), UTHEME_PANEL_BG);
}

// --- Toykit callbacks -------------------------------------------------
//
// Shapes is the app that forced `on_tick` to exist: there is no timer
// event in TWP, so its animation is driven by the loop's own pace, and
// a client that blocked for input would simply stop moving. With
// on_tick set, uapp polls instead of blocking, calls this once per
// pass, repaints if it returns 1, and yields.
static int on_tick(struct uapp *a) {
    (void)a;
    g_angle += g_speed * (FX_ONE / 1024);
    g_frames++;
    return 1; // always repaint -- at speed 0 that redraws the same
              // pixels, which is exactly what "it stops dead" means
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    (void)a;
    draw(uapp_surface(d));
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    if (key == 0x1B || key == 'q') { uapp_quit(a, 0); return; }
    if (key == 'a' || key == 'A') {
        log_line(uui_checkbox_toggle(&g_aa_check)
                  ? "gfxdemo: aa on\n" : "gfxdemo: aa off\n");
    }
    if (key == '+' || key == '=') { if (g_speed < 40) { g_speed++; log_speed(); } }
    if (key == '-') { if (g_speed > 0) { g_speed--; log_speed(); } }
}

// The button bar is routed by uapp (see desc.buttons below), so this
// only has to handle the control uapp does not know about. The two
// never overlap, so it does not matter that the bar was offered the
// press first -- there is no button under the checkbox to press.
static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    (void)a; (void)buttons;
    if (uui_checkbox_hit(&g_aa_check, x, y)) {
        log_line(uui_checkbox_toggle(&g_aa_check)
                  ? "gfxdemo: aa on\n" : "gfxdemo: aa off\n");
    }
}

static void on_motion(struct uapp *a, int x, int y, unsigned buttons) {
    (void)a;
    if (!buttons) uui_checkbox_hover(&g_aa_check, x, y);
}

// A button COMMITTED -- pressed and released on the same control. The
// press/drag-off/release bookkeeping that decides this lives in uapp
// and uui_button_group now, not here.
static void on_action(struct uapp *a, int code) {
    (void)a;
    if (code == BTN_SLOWER && g_speed > 0) g_speed--;
    else if (code == BTN_FASTER && g_speed < 40) g_speed++;
    else if (code == BTN_RESET) { g_speed = 3; g_angle = 0; }
    log_speed();
}

static void on_open(struct uapp *a) {
    (void)a;
    uint32_t fg = UTHEME_TEXT, bg = UTHEME_BUTTON_BG;
    uui_button_init(&g_buttons[0], 0, 0, 0, 0, "Slower", bg, fg, BTN_SLOWER);
    uui_button_init(&g_buttons[1], 0, 0, 0, 0, "Faster", bg, fg, BTN_FASTER);
    uui_button_init(&g_buttons[2], 0, 0, 0, 0, "Reset",  bg, fg, BTN_RESET);
    uui_button_group_init(&g_bar, g_buttons, 3);

    // Anti-aliasing starts on, and the checkbox holds that fact -- see
    // g_aa_check's declaration.
    uui_checkbox_init(&g_aa_check, MARGIN, checkbox_y(), ugfx_char_h(),
                       "anti-aliased  (A)", UTHEME_PANEL_BG, UTHEME_TEXT);
    g_aa_check.checked = 1;
    layout();

    log_line("gfxdemo: ready\n");
    log_line("gfxdemo: aa on\n");
    log_layout();
    log_speed();
}

int main(void) {
    struct uapp_desc desc = {
        .title     = "Shapes",
        .w         = WIN_W,
        .h         = WIN_H,
        .x         = 220,
        .y         = 110,
        .buttons   = &g_bar,
        .on_open   = on_open,
        .on_draw   = on_draw,
        .on_tick   = on_tick,
        .on_key    = on_key,
        .on_press  = on_press,
        .on_motion = on_motion,
        .on_action = on_action,
    };
    int rc = uapp_run(&desc);
    log_line("gfxdemo: exiting\n");
    return rc;
}
