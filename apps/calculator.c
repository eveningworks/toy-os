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
//
// The button grid itself is now a struct ui_button_group (apps/ui/
// ui_button.h + ui_button_group.h, pulled in together via the "ui/ui.h"
// umbrella header -- see that file's top comment) rather than a
// hand-rolled `int g_pressed_index` + private button_at() hit-test loop
// -- this file used to be exactly the "app invents its own buttons"
// case that motivated pulling that state and hit-testing out into a
// real, reusable object. See docs/decisions.md for the writeup and why
// it's modeled on Brutal OS's libs/brutal-ui/button.c.
#include "calculator.h"
#include "calc_engine.h"
#include "wm/wm.h"
#include "widgets.h"
#include "ui/ui.h"
#include "theme.h"
#include "kapi.h"

// Single static instance -- same reasoning as notepad.c's g_notepad:
// the window manager only allows one open Calculator window at a time.
static struct calc_state g_calc;

#define GRID_COLS 4
#define GRID_ROWS 5
#define BUTTON_COUNT (GRID_ROWS * GRID_COLS)

struct calc_button {
    const char *label; // what's drawn on the button
    char code;          // what calc_input() receives on click
};

// Row-major, top-left to bottom-right -- this table IS the layout, both
// for populating g_buttons[] (calculator_layout() walks it in order) and
// for the keyboard (calculator_key() looks a pressed key up in it via
// code_for_key()), so the two can never drift out of sync with each
// other.
static const struct calc_button BUTTONS[BUTTON_COUNT] = {
    { "C",   'C' }, { "CE",  'E' }, { "%",   '%' }, { "/",   '/' },
    { "7",   '7' }, { "8",   '8' }, { "9",   '9' }, { "*",   '*' },
    { "4",   '4' }, { "5",   '5' }, { "6",   '6' }, { "-",   '-' },
    { "1",   '1' }, { "2",   '2' }, { "3",   '3' }, { "+",   '+' },
    { "+/-", 's' }, { "0",   '0' }, { ".",   '.' }, { "=",   '=' },
};

// The live ui_button objects -- BUTTONS[] above stays the static
// label/code source of truth, these hold the per-instance geometry and
// (new) `pressed` state ui_button_group drives. Populated once (label/
// colors/code) by calculator_open() via ui_button_init(), re-positioned
// every draw/press/click by calculator_layout() via
// ui_button_set_geometry() -- see ui_button.h for why those are two
// different calls: re-running ui_button_init() every frame would zero
// `pressed` right before it could ever be drawn.
static struct ui_button g_buttons[BUTTON_COUNT];
static struct ui_button_group g_group;

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
// A slim status line above the main display showing the expression so
// far (e.g. "12 +") while an operator is pending -- see
// calculator_draw()'s own comment on when it's shown/blank. Not its own
// boxed field like the main display (see EXPR_GAP below), just plain
// text on the panel background, so it reads as a lighter secondary line
// rather than a second value. Adds a little to the window's default
// height (see calculator_default_size()) -- a fixed, non-resizable
// window, so this only ever affects the size a freshly-opened Calculator
// starts at, never a live resize.
#define EXPR_H (gfx_char_h() + 4)
#define EXPR_GAP 2
// Everything above the button grid -- the expression line plus the main
// display, with their own internal gaps -- as one constant so
// button_rect()/calculator_default_size() can't drift apart from where
// calculator_draw() actually paints them.
#define TOP_H (EXPR_H + EXPR_GAP + DISPLAY_H + DISPLAY_GAP)

// Content-area size for the current font -- see gui_apps.h's
// default_size. Mirrors button_rect()'s layout exactly (same macros),
// so the window is always sized to fit the button grid with no leftover
// margin at small fonts and no clipping at large ones.
void calculator_default_size(int *w, int *h) {
    *w = 2 * MARGIN + GRID_COLS * BTN_W + (GRID_COLS - 1) * BTN_GAP;
    *h = MARGIN + TOP_H + GRID_ROWS * BTN_H + (GRID_ROWS - 1) * BTN_GAP + MARGIN;
}

static void button_rect(int index, int *bx, int *by, int *bw, int *bh) {
    int row = index / GRID_COLS;
    int col = index % GRID_COLS;
    *bw = BTN_W;
    *bh = BTN_H;
    *bx = MARGIN + col * (BTN_W + BTN_GAP);
    *by = MARGIN + TOP_H + row * (BTN_H + BTN_GAP);
}

// Refreshes every button's position from button_rect() -- geometry is
// font-size-dependent (BTN_W/BTN_H both read gfx_char_w()/gfx_char_h()
// live) so this needs to re-run before every draw/press/click, not just
// once at open. Deliberately doesn't touch label/bg/fg/code/pressed --
// see ui_button_set_geometry()'s own doc comment.
static void calculator_layout(void) {
    for (int i = 0; i < BUTTON_COUNT; i++) {
        int bx, by, bw, bh;
        button_rect(i, &bx, &by, &bw, &bh);
        ui_button_set_geometry(&g_buttons[i], bx, by, bw, bh);
    }
}

void calculator_open(struct window *win) {
    calc_reset(&g_calc);
    window_set_state(win, &g_calc);

    uint32_t fg = THEME_TEXT;
    uint32_t btn_bg = THEME_BUTTON_BG;
    uint32_t op_btn_bg = gfx_rgb(210, 218, 235); // operators stand out slightly
    for (int i = 0; i < BUTTON_COUNT; i++) {
        char c = BUTTONS[i].code;
        int is_op = (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '=');
        ui_button_init(&g_buttons[i], 0, 0, 0, 0, BUTTONS[i].label,
                        is_op ? op_btn_bg : btn_bg, fg, (int)(unsigned char)c);
    }
    ui_button_group_init(&g_group, g_buttons, BUTTON_COUNT);
    calculator_layout();
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
    gfx_fill_rect(cx, cy, cw, ch, bg);

    // Expression-so-far line: "<accumulator> <op>" (e.g. "12 +") while
    // an operator is pending, so it's visible what's already been
    // committed and what the next number typed will be combined with --
    // the same information calc_state has always tracked (accumulator/
    // pending_op), just not shown anywhere before this. Blank the rest
    // of the time: nothing's pending right after calc_reset() or right
    // after '=' (pending_op is cleared there too), and there's nothing
    // useful to show mid-typing the very first operand either.
    if (st->pending_op != 0) {
        uint32_t expr_fg = gfx_rgb(90, 100, 115); // muted -- secondary to the main display
        char num[CALC_DISPLAY_MAX];
        calc_format_scaled(st->accumulator, num);
        char expr[CALC_DISPLAY_MAX + 2]; // + ' ' + op + NUL
        int pos = 0;
        for (const char *p = num; *p; p++) expr[pos++] = *p;
        expr[pos++] = ' ';
        expr[pos++] = st->pending_op;
        expr[pos] = '\0';

        int expr_len = (int)k_strlen(expr);
        int expr_w = expr_len * gfx_char_w();
        int expr_x = cx + cw - MARGIN - 6 - expr_w;
        if (expr_x < cx + MARGIN + 4) expr_x = cx + MARGIN + 4;
        gfx_draw_string(expr_x, cy + MARGIN, expr, expr_fg, bg);
    }

    // Display: right-aligned, like a real calculator.
    int display_y = cy + MARGIN + EXPR_H + EXPR_GAP;
    gfx_fill_rect(cx + MARGIN, display_y, cw - 2 * MARGIN, DISPLAY_H, display_bg);
    int text_len = (int)k_strlen(st->display);
    int text_w = text_len * gfx_char_w();
    int text_x = cx + cw - MARGIN - 6 - text_w;
    if (text_x < cx + MARGIN + 4) text_x = cx + MARGIN + 4; // don't overflow past the left edge
    int text_y = display_y + (DISPLAY_H - gfx_char_h()) / 2;
    gfx_draw_string(text_x, text_y, st->display, fg, display_bg);

    calculator_layout();
    ui_button_group_draw(&g_group, cx, cy);
}

// gui_apps.h's on_press: called every tick the button's held, starting
// with the initial button-down. Returns 1 (redraw needed) only when
// which button is "hot" actually changed -- e.g. moving off every
// button, or sliding onto a different one without releasing, both
// un-press/re-press exactly like a real OS button (ui_button_group_press()
// handles this). Deliberately does NOT call calc_input() itself -- that
// still only happens on the button-UP click (calculator_click()), so
// holding a button down and dragging off before releasing doesn't
// accidentally act on it, same as clicking any real button.
int calculator_press(struct window *win, int cx, int cy) {
    (void)win;
    calculator_layout();
    return ui_button_group_press(&g_group, cx, cy);
}

void calculator_release(struct window *win) {
    ui_button_group_release(&g_group);
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
    calculator_layout();
    int code = ui_button_group_click(&g_group, cx, cy);
    if (code < 0) return;
    calc_input(st, (char)code);
    window_invalidate(win);
}
