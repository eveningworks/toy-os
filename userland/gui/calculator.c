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
#include "ui/utheme.h"
#include "calc_engine.h"
#include "string.h"





static int win_request(struct win_request_msg *req) {
    return (int)sys_call(SYS_WIN_REQUEST, (uint64_t)(uintptr_t)req, 0, 0);
}

static void clear_req(struct win_request_msg *req) {
    for (unsigned i = 0; i < sizeof(*req); i++) ((uint8_t *)req)[i] = 0;
}

static int wait_event(struct win_event *ev) {
    int64_t r;
    do { // 0 = "woken, ask again"; parks again rather than spinning
        r = sys_call(SYS_WAIT_EVENT, (uint64_t)(uintptr_t)ev, 0, 0);
    } while (r == 0);
    return (int)r;
}

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

// Layout metrics, resolved once from the font the server handed us.
// Same formulas as apps/calculator.c's macros -- BTN_W fits "+/-"/"CE",
// the widest labels, at any size.
static int g_btn_w, g_btn_h, g_display_h, g_expr_h, g_top_h;
#define BTN_GAP 6
#define MARGIN 8
#define DISPLAY_GAP 8
#define EXPR_GAP 2

static void metrics_init(void) {
    g_btn_w     = 4 * ugfx_char_w() + 16;
    g_btn_h     = ugfx_char_h() + 16;
    g_display_h = ugfx_char_h() + 16;
    g_expr_h    = ugfx_char_h() + 4;
    g_top_h     = g_expr_h + EXPR_GAP + g_display_h + DISPLAY_GAP;
}

static int content_w(void) {
    return 2 * MARGIN + GRID_COLS * g_btn_w + (GRID_COLS - 1) * BTN_GAP;
}
static int content_h(void) {
    return MARGIN + g_top_h + GRID_ROWS * g_btn_h + (GRID_ROWS - 1) * BTN_GAP + MARGIN;
}

static void button_rect(int index, int *bx, int *by, int *bw, int *bh) {
    int row = index / GRID_COLS;
    int col = index % GRID_COLS;
    *bw = g_btn_w;
    *bh = g_btn_h;
    *bx = MARGIN + col * (g_btn_w + BTN_GAP);
    *by = MARGIN + g_top_h + row * (g_btn_h + BTN_GAP);
}

static struct calc_state g_calc;
static struct uui_button g_buttons[BUTTON_COUNT];
static struct uui_button_group g_group;

static void draw(struct ugfx_surface *s) {
    uint32_t bg = UTHEME_PANEL_BG;
    uint32_t fg = UTHEME_TEXT;
    uint32_t display_bg = UTHEME_WHITE;

    ugfx_fill(s, bg);

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

        int expr_x = s->w - MARGIN - 6 - ugfx_text_width(expr);
        if (expr_x < MARGIN + 4) expr_x = MARGIN + 4;
        ugfx_draw_string(s, expr_x, MARGIN, expr, expr_fg, bg);
    }

    // Display: right-aligned, like a real calculator.
    int display_y = MARGIN + g_expr_h + EXPR_GAP;
    ugfx_fill_rect(s, MARGIN, display_y, s->w - 2 * MARGIN, g_display_h, display_bg);
    int text_x = s->w - MARGIN - 6 - ugfx_text_width(g_calc.display);
    if (text_x < MARGIN + 4) text_x = MARGIN + 4;
    int text_y = display_y + (g_display_h - ugfx_char_h()) / 2;
    ugfx_draw_string(s, text_x, text_y, g_calc.display, fg, display_bg);

    uui_button_group_draw(&g_group, s);
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

static void present(uint32_t id) {
    struct win_request_msg req;
    clear_req(&req);
    req.type = WIN_REQ_PRESENT;
    req.window = id;
    win_request(&req);
}

int main(void) {
    struct win_request_msg req;

    // The font has to arrive before the layout can be computed -- every
    // metric is derived from the glyph size. But WIN_REQ_FONT needs a
    // session, and a window is what proves there is one, so the window
    // is created first at a provisional size and resized... except a
    // client cannot resize itself yet (Milestone 41). So instead: ask
    // for the font FIRST via a throwaway create is not possible either.
    //
    // Resolved the simple way: create the window at the size implied by
    // the font, which means asking for the font before the window. The
    // server allows that -- WIN_REQ_FONT needs a registered server, not
    // a window (see win_server.c's map_font()).
    if (!ugfx_font_init()) sys_exit(2);
    metrics_init();

    clear_req(&req);
    req.type = WIN_REQ_CREATE;
    req.a = content_w();
    req.b = content_h();
    req.c = 340;
    req.d = 150;
    if (win_request(&req) != 1) sys_exit(1);
    uint32_t id = req.window;

    clear_req(&req);
    req.type = WIN_REQ_TITLE;
    req.window = id;
    const char *title = "Calculator";
    int t = 0;
    for (; title[t] && t < WIN_TITLE_LEN - 1; t++) req.text[t] = title[t];
    req.text[t] = '\0';
    win_request(&req);

    calc_reset(&g_calc);

    uint32_t fg = UTHEME_TEXT;
    uint32_t btn_bg = UTHEME_BUTTON_BG;
    uint32_t op_btn_bg = ugfx_rgb(210, 218, 235); // operators stand out slightly
    for (int i = 0; i < BUTTON_COUNT; i++) {
        char c = BUTTONS[i].code;
        int is_op = (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '=');
        int bx, by, bw, bh;
        button_rect(i, &bx, &by, &bw, &bh);
        uui_button_init(&g_buttons[i], bx, by, bw, bh, BUTTONS[i].label,
                         is_op ? op_btn_bg : btn_bg, fg, (int)(unsigned char)c);
    }
    uui_button_group_init(&g_group, g_buttons, BUTTON_COUNT);

    struct ugfx_surface s = ugfx_surface_for_window(id, content_w(), content_h());
    draw(&s);
    present(id);

    for (;;) {
        struct win_event ev;
        if (wait_event(&ev) != 1) break;

        int quit = 0, dirty = 0;

        if (ev.type == WIN_EV_CLOSE) {
            quit = 1;
        } else if (ev.type == WIN_EV_KEY) {
            if (ev.a == 0x1B) {
                quit = 1; // Esc closes, same as the desktop's own apps
            } else {
                char code = code_for_key(ev.a);
                if (code) { calc_input(&g_calc, code); dirty = 1; }
            }
        } else if (ev.type == WIN_EV_MOUSE_DOWN) {
            dirty |= uui_button_group_press(&g_group, ev.a, ev.b);
        } else if (ev.type == WIN_EV_MOUSE_MOVE) {
            // With a button held this re-hit-tests the press (so
            // dragging off a button un-presses it); with none held it
            // is just hover tracking. Both return "did anything
            // change", so a mouse moving across the window only causes
            // a repaint when it actually crosses a boundary.
            if (ev.mods) dirty |= uui_button_group_press(&g_group, ev.a, ev.b);
            else         dirty |= uui_button_group_hover(&g_group, ev.a, ev.b);
        } else if (ev.type == WIN_EV_MOUSE_UP) {
            // The commit point. A press dragged off its button was
            // already cleared by the moves above, so this returns -1
            // and correctly does nothing.
            int code = uui_button_group_release(&g_group);
            if (code >= 0) calc_input(&g_calc, (char)code);
            dirty = 1;
        }

        if (quit) break;
        if (dirty) { draw(&s); present(id); }
    }

    clear_req(&req);
    req.type = WIN_REQ_DESTROY;
    req.window = id;
    win_request(&req);
    sys_exit(0);
}
