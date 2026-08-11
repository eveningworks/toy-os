// A basic 4-function GUI calculator: digits, +, -, *, /, %, sign
// toggle, decimal point, clear (C), clear-entry (CE), and backspace --
// works from both the on-screen buttons and the keyboard. All the
// actual arithmetic lives in calc_engine.c/.h, kept deliberately free
// of any gfx.h/wm.h dependency; this file is just the adapter that maps
// button clicks and keypresses onto calc_input() calls and draws
// whatever calc_input() leaves in st->display.
//
// Adding a new button (say, a future square-root key): add one entry to
// BUTTONS[] below with a label and a code calc_input() understands (see
// calc_engine.h's comment on calc_input() for the full code list, and on
// apply_op() for how to teach the engine a brand new operator), and bump
// GRID_ROWS/GRID_COLS if it doesn't fit the existing grid. Nothing else
// in this file needs to change -- layout, drawing, and click hit-testing
// are all generic over BUTTONS[].
#include "calculator.h"
#include "calc_engine.h"
#include "wm/wm.h"
#include "widgets.h"
#include "theme.h"
#include "kapi.h"

// Single static instance -- same reasoning as notepad.c's g_notepad:
// the window manager only allows one open Calculator window at a time.
static struct calc_state g_calc;

// Which BUTTONS[] index is currently held down, or -1 -- purely a GUI
// concern (which button visibly looks pressed right now), deliberately
// NOT part of struct calc_state (calc_engine.h), which stays free of
// any gfx.h/wm.h dependency per this file's own top comment. Driven by
// calculator_press()/calculator_release() below (gui_apps.h's on_press/
// on_release), read by calculator_draw() to pass widget_button() its
// `pressed` flag.
static int g_pressed_index = -1;

#define GRID_COLS 4
#define GRID_ROWS 5

struct calc_button {
    const char *label; // what's drawn on the button
    char code;          // what calc_input() receives on click
};

// Row-major, top-left to bottom-right -- this table IS the layout, both
// for drawing (calculator_draw walks it in order) and for the keyboard
// (calculator_key looks a pressed key up in it via code_for_key()), so
// the two can never drift out of sync with each other.
static const struct calc_button BUTTONS[GRID_ROWS * GRID_COLS] = {
    { "C",   'C' }, { "CE",  'E' }, { "%",   '%' }, { "/",   '/' },
    { "7",   '7' }, { "8",   '8' }, { "9",   '9' }, { "*",   '*' },
    { "4",   '4' }, { "5",   '5' }, { "6",   '6' }, { "-",   '-' },
    { "1",   '1' }, { "2",   '2' }, { "3",   '3' }, { "+",   '+' },
    { "+/-", 's' }, { "0",   '0' }, { ".",   '.' }, { "=",   '=' },
};

// Sized off the largest baked font (20x40px, see font_ttf.c) so buttons
// never clip at any font size -- same reasoning as notepad.c's BTN_W,
// recomputed live rather than cached so `fontsize` still works on an
// already-open Calculator window.
#define BTN_W (4 * gfx_char_w() + 16) // fits "+/-"/"CE" (widest labels) at any size
#define BTN_H (gfx_char_h() + 16)
#define BTN_GAP 6
#define MARGIN 8
#define DISPLAY_H (gfx_char_h() + 16)
#define DISPLAY_GAP 8

// Content-area size for the current font -- see gui_apps.h's
// default_size. Mirrors button_rect()'s layout exactly (same macros),
// so the window is always sized to fit the button grid with no leftover
// margin at small fonts and no clipping at large ones.
void calculator_default_size(int *w, int *h) {
    *w = 2 * MARGIN + GRID_COLS * BTN_W + (GRID_COLS - 1) * BTN_GAP;
    *h = MARGIN + DISPLAY_H + DISPLAY_GAP + GRID_ROWS * BTN_H + (GRID_ROWS - 1) * BTN_GAP + MARGIN;
}

void calculator_open(struct window *win) {
    calc_reset(&g_calc);
    window_set_state(win, &g_calc);
}

static void button_rect(int index, int *bx, int *by, int *bw, int *bh) {
    int row = index / GRID_COLS;
    int col = index % GRID_COLS;
    *bw = BTN_W;
    *bh = BTN_H;
    *bx = MARGIN + col * (BTN_W + BTN_GAP);
    *by = MARGIN + DISPLAY_H + DISPLAY_GAP + row * (BTN_H + BTN_GAP);
}

void calculator_draw(struct window *win) {
    struct calc_state *st = (struct calc_state *)window_get_state(win);
    int cx = window_content_x(win);
    int cy = window_content_y(win);
    int cw = window_content_w(win);
    int ch = window_content_h(win);

    uint32_t bg = THEME_PANEL_BG;
    uint32_t fg = THEME_TEXT;
    uint32_t display_bg = THEME_WHITE;
    uint32_t btn_bg = THEME_BUTTON_BG;
    uint32_t op_btn_bg = gfx_rgb(210, 218, 235); // operators stand out slightly
    gfx_fill_rect(cx, cy, cw, ch, bg);

    // Display: right-aligned, like a real calculator.
    gfx_fill_rect(cx + MARGIN, cy + MARGIN, cw - 2 * MARGIN, DISPLAY_H, display_bg);
    int text_len = (int)k_strlen(st->display);
    int text_w = text_len * gfx_char_w();
    int text_x = cx + cw - MARGIN - 6 - text_w;
    if (text_x < cx + MARGIN + 4) text_x = cx + MARGIN + 4; // don't overflow past the left edge
    int text_y = cy + MARGIN + (DISPLAY_H - gfx_char_h()) / 2;
    gfx_draw_string(text_x, text_y, st->display, fg, display_bg);

    for (int i = 0; i < GRID_ROWS * GRID_COLS; i++) {
        int bx, by, bw, bh;
        button_rect(i, &bx, &by, &bw, &bh);
        char c = BUTTONS[i].code;
        int is_op = (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '=');
        uint32_t this_bg = is_op ? op_btn_bg : btn_bg;
        widget_button(cx + bx, cy + by, bw, bh, BUTTONS[i].label, this_bg, fg, i == g_pressed_index);
    }
}

// Which button (if any) is under (cx, cy) -- shared by calculator_click
// and calculator_press so they can never disagree about hit-testing the
// same grid.
static int button_at(int cx, int cy) {
    for (int i = 0; i < GRID_ROWS * GRID_COLS; i++) {
        int bx, by, bw, bh;
        button_rect(i, &bx, &by, &bw, &bh);
        if (widget_hit(bx, by, bw, bh, cx, cy)) return i;
    }
    return -1;
}

// gui_apps.h's on_press: called every tick the button's held, starting
// with the initial button-down. Returns 1 (redraw needed) only when
// which button is "hot" actually changed -- e.g. moving off every
// button, or sliding onto a different one without releasing, both
// un-press/re-press exactly like a real OS button. Deliberately does
// NOT call calc_input() itself -- that still only happens on the
// button-UP click (calculator_click()), so holding a button down and
// dragging off before releasing doesn't accidentally act on it, same
// as clicking any real button.
int calculator_press(struct window *win, int cx, int cy) {
    (void)win;
    int hit = button_at(cx, cy);
    if (hit == g_pressed_index) return 0;
    g_pressed_index = hit;
    return 1;
}

void calculator_release(struct window *win) {
    (void)win;
    g_pressed_index = -1;
    window_invalidate(win);
}

void calculator_key(struct window *win, int key) {
    // Direct passthrough for anything calc_input() understands -- the
    // keyboard driver already delivers shifted symbols ('*', '%', '+',
    // etc) as the right ASCII char (see keyboard.c), so no translation
    // table is needed here beyond the couple of keys that spell
    // differently than their button code.
    char code = 0;
    if (key >= '0' && key <= '9') code = (char)key;
    else if (key == '.' || key == '+' || key == '-' || key == '*' ||
             key == '/' || key == '%' || key == '=') code = (char)key;
    else if (key == '\r' || key == '\n') code = '=';
    else if (key == '\b') code = 'B';
    else if (key == 'c' || key == 'C') code = 'C';

    if (code == 0) return; // unrecognized key -- ignored, same as an unknown button code

    struct calc_state *st = (struct calc_state *)window_get_state(win);
    calc_input(st, code);
    window_invalidate(win);
}

void calculator_click(struct window *win, int cx, int cy) {
    struct calc_state *st = (struct calc_state *)window_get_state(win);
    int hit = button_at(cx, cy);
    if (hit < 0) return;
    calc_input(st, BUTTONS[hit].code);
    window_invalidate(win);
}
