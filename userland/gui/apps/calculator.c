// Calculator, as a RING-3 PROCESS.
//
// The same application as apps/calculator.c, moved out of the kernel:
// same button grid, same keyboard handling, same commit-on-release
// behaviour, same look -- but running as an ordinary ring-3 program
// that talks to the window server over the windowing protocol
// (abi/win_proto.h) instead of being compiled into kernel.bin and
// called through gui_apps.h callbacks.
//
// WHAT IS ACTUALLY SHARED, AND WHAT IS A PORT
// -------------------------------------------
// The arithmetic is not reimplemented or copied: apps/calc_engine.c is
// compiled a SECOND time with USERLAND_CFLAGS and linked in here (see
// the Makefile's shared-source rule). It only ever needed string.h and
// knum.h -- both freestanding -- so it was already portable; nothing
// about it had to change. That is the strongest form this migration
// could take: a bug fixed in the engine fixes both copies of the app,
// because there is only one engine.
//
// What IS ported is the presentation layer: `ui_button_group` becomes
// `uui_button_group` (userland/uui.c), `gfx_*` becomes `ugfx_*`, and
// the THEME_* colours become UTHEME_*. Behaviour is deliberately
// carried over unchanged -- see uui.h.
//
// The three real differences from the kernel version, all forced by
// being a separate process rather than chosen:
//
//   1. No window_content_x/y(). A client draws into its own buffer at
//      (0,0) and the server places the window, so every coordinate here
//      is content-relative with no origin to add. This is simpler than
//      the kernel version, not harder.
//   2. No kzalloc/kfree per window. One process is one window, so the
//      instance is a plain static -- the multi-instance machinery
//      apps/calculator.c needs exists because many windows share one
//      address space, which stops being true here. Multiple calculators
//      are multiple processes.
//   3. Font size is read once at startup rather than live. The kernel
//      version recomputes BTN_W/BTN_H from gfx_char_w() on every draw
//      so `fontsize` affects an open window; a client would need a
//      "font changed" event to do the same, and there isn't one yet
//      (see docs/roadmap.md's Milestone 41).
#include <stdint.h>
#include "rt/sys.h"
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uapp.h"
#include "ui/utheme.h"
#include "calc_engine.h"
#include "string.h"








#define GRID_COLS 4
#define GRID_ROWS 5
#define BUTTON_COUNT (GRID_ROWS * GRID_COLS)

struct calc_button {
    const char *label;
    char code;
};

// Byte-for-byte the same table as apps/calculator.c's, and for the same
// reason: it IS the layout, walked in order to place the buttons and
// searched by the keyboard handler, so the two can't drift apart.
static const struct calc_button BUTTONS[BUTTON_COUNT] = {
    { "C",   'C' }, { "CE",  'E' }, { "%",   '%' }, { "/",   '/' },
    { "7",   '7' }, { "8",   '8' }, { "9",   '9' }, { "*",   '*' },
    { "4",   '4' }, { "5",   '5' }, { "6",   '6' }, { "-",   '-' },
    { "1",   '1' }, { "2",   '2' }, { "3",   '3' }, { "+",   '+' },
    { "+/-", 's' }, { "0",   '0' }, { ".",   '.' }, { "=",   '=' },
};

// Layout. The display area's height is the only metric this app still
// computes -- everything else (window size, button sizes, positions,
// margins, gaps) comes from uui_layout and the buttons' own natural
// sizes. `button_rect()`, `metrics_init()`, `content_w()`,
// `content_h()` and the four spacing constants they needed are gone;
// see ui/uui_layout.h.
#define MARGIN 8   // still used by the display's own drawing, below

static int display_h(void) { return ugfx_char_h() + 16; }
static int expr_h(void)    { return ugfx_char_h() + 4; }

static struct calc_state g_calc;
static struct uui_button g_buttons[BUTTON_COUNT];
static struct uui_button_group g_group;

static void draw_display(struct ugfx_surface *s, const struct uui_custom *c) {
    uint32_t bg = UTHEME_PANEL_BG;
    uint32_t fg = UTHEME_TEXT;
    uint32_t display_bg = UTHEME_WHITE;

    // Expression-so-far ("12 +") while an operator is pending, right
    // aligned and muted -- secondary to the main display, exactly as in
    // the kernel version.
    if (g_calc.pending_op != 0) {
        uint32_t expr_fg = ugfx_rgb(90, 100, 115);
        char num[CALC_DISPLAY_MAX];
        calc_format_scaled(g_calc.accumulator, num);
        char expr[CALC_DISPLAY_MAX + 2];
        int pos = 0;
        for (const char *p = num; *p; p++) expr[pos++] = *p;
        expr[pos++] = ' ';
        expr[pos++] = g_calc.pending_op;
        expr[pos] = '\0';

        int expr_x = c->x + c->w - 6 - ugfx_text_width(expr);
        if (expr_x < c->x + 4) expr_x = c->x + 4;
        ugfx_draw_string(s, expr_x, c->y, expr, expr_fg, bg);
    }

    // Display: right-aligned, like a real calculator. Positioned
    // against the rect the layout handed this item rather than against
    // the window -- which is what lets it sit anywhere the layout puts
    // it without this code knowing where that is.
    int display_y = c->y + expr_h() + 2;
    ugfx_fill_rect(s, c->x, display_y, c->w, display_h(), display_bg);
    int text_x = c->x + c->w - 6 - ugfx_text_width(g_calc.display);
    if (text_x < c->x + 4) text_x = c->x + 4;
    int text_y = display_y + (display_h() - ugfx_char_h()) / 2;
    ugfx_draw_string(s, text_x, text_y, g_calc.display, fg, display_bg);
}

// Same passthrough the kernel version uses: the keyboard driver already
// delivers shifted symbols as the right ASCII character, so only the
// few keys that spell differently from their button code need mapping.
static char code_for_key(int key) {
    if (key >= '0' && key <= '9') return (char)key;
    if (key == '.' || key == '+' || key == '-' || key == '*' ||
        key == '/' || key == '%' || key == '=') return (char)key;
    if (key == '\r' || key == '\n') return '=';
    if (key == '\b') return 'B';
    if (key == 'c' || key == 'C') return 'C';
    return 0;
}

// --- self-reported layout --------------------------------------------
//
// Calculator reports where its buttons actually are, rather than
// letting tools/calculator_client_test.py re-derive them from the
// window size. That test used to invert this app's old sizing formula
// in Python (`char_h = (ch - 150) // 7`), which is exactly the trap
// uidemo_test.py and gfxdemo_test.py document: the Python copy drifts
// silently the moment the layout changes. It did drift here -- after
// the move to uui_layout it computed a char_h of 18 against a real 17,
// and its clicks landed several pixels off centre. They still landed
// INSIDE the buttons, so the suite stayed green while measuring
// something it no longer understood. That is a worse failure than a
// red test, and this is the fix.
//
// Grammar matches the other two: one line per widget, content-relative,
// on stderr (which reaches dmesg -- a client's stdout goes to its
// owning Terminal's scrollback, where no test can read it).
static void log_line(const char *s) { sys_eprint(s); }

static int append_int(char *buf, int n, int v) {
    if (v < 0) { buf[n++] = '-'; v = -v; }
    char d[12];
    int c = 0;
    if (v == 0) d[c++] = '0';
    while (v > 0) { d[c++] = (char)('0' + v % 10); v /= 10; }
    while (c > 0) buf[n++] = d[--c];
    return n;
}

static void log_rect(const char *what, const char *name, int x, int y, int w, int h) {
    char b[80];
    int n = 0;
    const char *pre = "calculator: layout ";
    while (pre[n]) { b[n] = pre[n]; n++; }
    for (const char *p = what; *p; p++) b[n++] = *p;
    b[n++] = ' ';
    if (name) { for (const char *p = name; *p; p++) b[n++] = *p; b[n++] = ' '; }
    n = append_int(b, n, x); b[n++] = ' ';
    n = append_int(b, n, y); b[n++] = ' ';
    n = append_int(b, n, w); b[n++] = ' ';
    n = append_int(b, n, h);
    b[n++] = '\n';
    b[n] = '\0';
    log_line(b);
}

// --- the layout ------------------------------------------------------
//
// A column of [display area, 4x5 button grid]. That is the whole of
// this app's geometry now: the window is sized from this column's
// natural size, the grid divides its room between twenty buttons, and
// each button's natural size comes from its own label. Nothing here
// says a coordinate.

static struct uui_custom g_display;
static struct uui_item g_grid_items[BUTTON_COUNT];
static struct uui_layout g_grid;
static struct uui_item g_root_items[2];
static struct uui_layout g_root;

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    // Esc deliberately does NOT close. Closing is Alt+F4 (a WM
    // shortcut) or the title bar's X -- see docs/decisions.md. Esc is
    // the app's own key, for cancelling whatever it has open.
    char code = code_for_key(key);
    if (code) { calc_input(&g_calc, code); uapp_redraw(a); }
}

static void on_action(struct uapp *a, int code) {
    calc_input(&g_calc, (char)code);
    uapp_redraw(a);
}

static void on_open(struct uapp *a) {
    (void)a;
    calc_reset(&g_calc);

    // Geometry is settled by now: uapp runs the layout before calling
    // this, so every rect below is the one that will actually be drawn.
    log_rect("display", 0, g_display.x, g_display.y, g_display.w, g_display.h);
    for (int i = 0; i < BUTTON_COUNT; i++) {
        log_rect("btn", BUTTONS[i].label,
                  g_buttons[i].x, g_buttons[i].y, g_buttons[i].w, g_buttons[i].h);
    }
    log_line("calculator: ready\n");
}

int main(void) {
    if (!ugfx_font_init()) return 2; // metrics are needed to build the layout

    uint32_t fg = UTHEME_TEXT;
    uint32_t btn_bg = UTHEME_BUTTON_BG;
    uint32_t op_btn_bg = ugfx_rgb(210, 218, 235); // operators stand out slightly
    for (int i = 0; i < BUTTON_COUNT; i++) {
        char c = BUTTONS[i].code;
        int is_op = (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '=');
        // Geometry is 0 here on purpose: the layout assigns it, and a
        // button's natural size comes from its own label.
        uui_button_init(&g_buttons[i], 0, 0, 0, 0, BUTTONS[i].label,
                         is_op ? op_btn_bg : btn_bg, fg, (int)(unsigned char)c);
        g_grid_items[i].ops = &uui_button_ops;
        g_grid_items[i].widget = &g_buttons[i];
    }
    uui_button_group_init(&g_group, g_buttons, BUTTON_COUNT);

    g_grid.dir = UUI_GRID;
    g_grid.cols = GRID_COLS;
    g_grid.margin = 0; // the root column already insets everything
    g_grid.items = g_grid_items;
    g_grid.count = BUTTON_COUNT;

    g_display.w = 0; // no width preference -- fill the column
    g_display.h = expr_h() + 2 + display_h();
    g_display.draw = draw_display;

    g_root_items[0].ops = &uui_custom_ops;
    g_root_items[0].widget = &g_display;
    g_root_items[1].ops = &uui_layout_ops;
    g_root_items[1].widget = &g_grid;

    g_root.dir = UUI_COLUMN;
    g_root.items = g_root_items;
    g_root.count = 2;

    struct uapp_desc desc = {
        .title     = "Calculator",
        .app_id    = "calculator",
        .x         = 340,
        .y         = 150,
        .layout    = &g_root,
        .buttons   = &g_group,
        .on_open   = on_open,
        .on_key    = on_key,
        .on_action = on_action,
    };
    return uapp_run(&desc);
}
