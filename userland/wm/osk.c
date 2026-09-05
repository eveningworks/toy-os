// The on-screen keyboard. See osk.h for what it is and why it is an
// overlay rather than an app.
#include "wm_internal.h"
#include "wm_overlay.h"
#include "wm_tray.h"
#include "osk.h"
#include "kapi.h"
#include "keyboard.h"
#include "ui/utheme.h"
#include "ui/uui_primitives.h"

int osk_open = 0;

static int g_tray_id = -1;

// The armed sticky modifiers (KEY_MOD_*), and the keycap the pointer is
// holding down. A modifier is armed by a click and consumed by the next
// ordinary key, which is what makes a one-pointer device able to type
// Ctrl-C at all.
static unsigned g_mods = 0;
static int g_pressed_row = -1, g_pressed_col = -1;

// --- the layout -------------------------------------------------------
//
// US QWERTY, matching kernel/lib/keyboard_layout.c's FALLBACK_US tables
// character for character. THE TWO MUST AGREE: this is a second copy of
// a layout, so a key that disagrees types something the physical
// keyboard would not, and only on this machine.
//
// `span` is in HALF key widths, so a plain key is 2 and every row sums
// to OSK_ROW_SPAN. A row that does not sum to it is drawn short rather
// than misaligned, which is a visible mistake instead of a silent one.
#define OSK_ROWS      5
#define OSK_ROW_SPAN  30
#define OSK_MAX_COLS  15

struct osk_key {
    const char *cap;      // label; the shifted label is derived for a letter
    const char *cap_sh;   // label when shift is armed, or 0 for `cap`
    short code;           // an ASCII character, or a KEY_* code
    short code_sh;        // shifted, or 0 for `code`
    unsigned char mod;    // nonzero: a sticky modifier, and `code` is unused
    unsigned char span;
};

#define K(c, s)     { c, s, c[0], s[0], 0, 2 }
#define LTR(l, u)   { l, u, l[0], u[0], 0, 2 }
#define WIDE(cap, code, span) { cap, 0, code, 0, 0, span }
#define MOD(cap, bit, span)   { cap, 0, 0, 0, bit, span }

static const struct osk_key g_row0[] = {
    K("`","~"), K("1","!"), K("2","@"), K("3","#"), K("4","$"), K("5","%"),
    K("6","^"), K("7","&"), K("8","*"), K("9","("), K("0",")"),
    K("-","_"), K("=","+"), WIDE("Bksp", '\b', 4),
};
static const struct osk_key g_row1[] = {
    WIDE("Tab", '\t', 4),
    LTR("q","Q"), LTR("w","W"), LTR("e","E"), LTR("r","R"), LTR("t","T"),
    LTR("y","Y"), LTR("u","U"), LTR("i","I"), LTR("o","O"), LTR("p","P"),
    K("[","{"), K("]","}"),
};
static const struct osk_key g_row2[] = {
    WIDE("Esc", 27, 4),
    LTR("a","A"), LTR("s","S"), LTR("d","D"), LTR("f","F"), LTR("g","G"),
    LTR("h","H"), LTR("j","J"), LTR("k","K"), LTR("l","L"),
    K(";",":"), K("'","\""), WIDE("Enter", '\n', 4),
};
static const struct osk_key g_row3[] = {
    MOD("Shift", KEY_MOD_SHIFT, 5),
    LTR("z","Z"), LTR("x","X"), LTR("c","C"), LTR("v","V"), LTR("b","B"),
    LTR("n","N"), LTR("m","M"), K(",","<"), K(".",">"), K("/","?"),
    K("\\","|"), { "Del", 0, KEY_DELETE, 0, 0, 3 },
};
static const struct osk_key g_row4[] = {
    MOD("Ctrl", KEY_MOD_CTRL, 4), MOD("Alt", KEY_MOD_ALT, 4),
    // Named rather than drawn as glyphs: the caps double as the labels
    // `gui osk key <cap>` looks a key up by, and a cap of " " or "<"
    // cannot be passed as a debug-console token at all.
    WIDE("Space", ' ', 10),
    { "Left", 0, KEY_ARROW_LEFT, 0, 0, 3 }, { "Down", 0, KEY_ARROW_DOWN, 0, 0, 3 },
    { "Up", 0, KEY_ARROW_UP, 0, 0, 3 },     { "Right", 0, KEY_ARROW_RIGHT, 0, 0, 3 },
};

static const struct osk_key *const g_rows[OSK_ROWS] = {
    g_row0, g_row1, g_row2, g_row3, g_row4,
};
static const int g_row_len[OSK_ROWS] = {
    (int)(sizeof g_row0 / sizeof g_row0[0]), (int)(sizeof g_row1 / sizeof g_row1[0]),
    (int)(sizeof g_row2 / sizeof g_row2[0]), (int)(sizeof g_row3 / sizeof g_row3[0]),
    (int)(sizeof g_row4 / sizeof g_row4[0]),
};

// --- geometry ---------------------------------------------------------
//
// Font-derived, never fixed pixels (docs/gui-guidelines.md): the panel
// is as wide as the screen and each half-span is a fifteenth of it, so
// a bigger default font makes taller keys rather than a clipped grid.

struct osk_geom {
    int x, y, w, h;
    int pad, row_h, half_w;
};

static void osk_geometry(struct osk_geom *g) {
    g->pad = ugfx_char_h() / 2;
    g->row_h = ugfx_char_h() * 2;
    g->w = screen_w;
    g->half_w = (g->w - 2 * g->pad) / OSK_ROW_SPAN;
    g->h = OSK_ROWS * g->row_h + 2 * g->pad;
    g->x = 0;
    g->y = screen_h - taskbar_h - g->h;
}

// The keycap's box. Walks the row's spans, which is the SAME walk that
// draws it -- so a click cannot be told a different rect from the one
// the cap was painted at, the rule wm_tray.c's tray_item_rect() follows.
static void key_rect(const struct osk_geom *g, int row, int col,
                     int *x, int *y, int *w, int *h) {
    int span = 0;
    for (int i = 0; i < col; i++) span += g_rows[row][i].span;
    *x = g->x + g->pad + span * g->half_w;
    *y = g->y + g->pad + row * g->row_h;
    *w = g_rows[row][col].span * g->half_w;
    *h = g->row_h;
}

static int key_at(const struct osk_geom *g, int mx, int my, int *out_row, int *out_col) {
    for (int r = 0; r < OSK_ROWS; r++) {
        for (int c = 0; c < g_row_len[r]; c++) {
            int x, y, w, h;
            key_rect(g, r, c, &x, &y, &w, &h);
            if (!uui_hit(x, y, w - 1, h - 1, mx, my)) continue;
            *out_row = r; *out_col = c;
            return 1;
        }
    }
    return 0;
}

// --- typing -----------------------------------------------------------

// The focused window, by the SAME rule wm.c routes real keystrokes with
// -- topmost that is not minimized. A second copy of the rule here
// would be a keyboard that types into a different window from the one
// the title bar says is active.
static struct window *focused_client(void) {
    for (int i = window_count - 1; i >= 0; i--) {
        if (windows[i].state == WIN_MINIMIZED) continue;
        return wm_client_is_client_window(&windows[i]) ? &windows[i] : 0;
    }
    return 0;
}

static void send_one(struct window *win, int code, unsigned mods) {
    wm_client_send_key(win, code, mods);
    wm_client_send_key_up(win, code, mods);
}

// Emits one keycap, encoded the way keyboard.c encodes the physical
// key. The three branches ARE api/keyboard.h's contract, and getting
// any of them wrong is silent: Ctrl-C as 'c' + KEY_MOD_CTRL reaches an
// app that (correctly) never tests that bit for a letter.
static void type_key(const struct osk_key *k) {
    struct window *win = focused_client();
    if (!win) return;

    int shifted = (g_mods & KEY_MOD_SHIFT) != 0;
    int code = shifted && k->code_sh ? k->code_sh : k->code;
    if (!code) return;

    if (g_mods & KEY_MOD_CTRL) {
        // Ctrl folds a LETTER to its control code and drops anything
        // else, rather than inventing an encoding for it.
        int lower = (code >= 'A' && code <= 'Z') ? code - 'A' + 'a' : code;
        if (lower < 'a' || lower > 'z') return;
        send_one(win, lower - 'a' + 1, g_mods);
        return;
    }
    if (g_mods & KEY_MOD_ALT) {
        // Meta prefix: two keystrokes, ESC then the key, which is what
        // readline and this kernel's line editor both expect.
        send_one(win, 27, g_mods);
        send_one(win, code, g_mods);
        return;
    }
    send_one(win, code, g_mods);
}

// --- what the debug console reports ------------------------------------

void osk_report(struct osk_report *r) {
    struct osk_geom g;
    osk_geometry(&g);
    r->x = g.x; r->y = g.y; r->w = g.w; r->h = g.h;
    r->mods = g_mods;
    r->tray_x = r->tray_y = r->tray_w = r->tray_h = 0;
    if (g_tray_id >= 0)
        tray_item_rect(g_tray_id, &r->tray_x, &r->tray_y, &r->tray_w, &r->tray_h);
}

int osk_key_box(const char *cap, int *x, int *y, int *w, int *h) {
    if (!cap) return 0;
    struct osk_geom g;
    osk_geometry(&g);
    for (int r = 0; r < OSK_ROWS; r++)
        for (int c = 0; c < g_row_len[r]; c++)
            if (k_strcmp(g_rows[r][c].cap, cap) == 0) {
                key_rect(&g, r, c, x, y, w, h);
                return 1;
            }
    return 0;
}

// --- overlay ops ------------------------------------------------------

void osk_damage(void) {
    struct osk_geom g;
    osk_geometry(&g);
    wm_damage_rect(g.x, g.y, g.w, g.h);
    redraw_pending = 1;
}

static void osk_close_panel(void) {
    if (!osk_open) return;
    osk_damage();          // while it is still up, so its rect repaints
    osk_open = 0;
    g_mods = 0;
    g_pressed_row = g_pressed_col = -1;
}

static void osk_open_panel(void) {
    // Deliberately does NOT call wm_overlay_close_others(): this panel
    // has no `close` op, so it is not dismissed by other popups either,
    // and a keyboard that shut the menu it was typing into would be
    // useless.
    osk_open = 1;
    g_mods = 0;
    g_pressed_row = g_pressed_col = -1;
    osk_damage();
}

void osk_init(void) {
    g_tray_id = tray_register_icon("tray-keyboard");
}

int osk_hover_at(int mx, int my) {
    struct osk_geom g;
    osk_geometry(&g);
    int r, c;
    if (!key_at(&g, mx, my, &r, &c)) return 0;
    return r * OSK_MAX_COLS + c + 1;   // 0 means none, so bias by one
}

void osk_update_press(int mx, int my, uint8_t buttons) {
    if (!osk_open) return;
    if (!(buttons & 0x1)) { g_pressed_row = g_pressed_col = -1; return; }
    struct osk_geom g;
    osk_geometry(&g);
    int r, c;
    if (key_at(&g, mx, my, &r, &c)) {
        if (r == g_pressed_row && c == g_pressed_col) return;
        g_pressed_row = r; g_pressed_col = c;
    } else if (g_pressed_row < 0) {
        return;
    } else {
        g_pressed_row = g_pressed_col = -1;
    }
    osk_damage();
}

int osk_handle_click(int mx, int my) {
    // The tray item toggles it, and is answered whether or not the
    // panel is up -- wm_overlay_click() calls every row's handler, which
    // is what lets a closed overlay open itself.
    int tx, ty, tw, th;
    if (g_tray_id >= 0 && tray_item_rect(g_tray_id, &tx, &ty, &tw, &th) &&
        uui_hit(tx, ty, tw, th, mx, my)) {
        if (osk_open) osk_close_panel();
        else          osk_open_panel();
        return 1;
    }
    if (!osk_open) return 0;

    struct osk_geom g;
    osk_geometry(&g);
    int r, c;
    if (!key_at(&g, mx, my, &r, &c)) {
        // A click outside the panel is NOT ours: it belongs to whatever
        // is beneath, which is how the caret gets placed in the field
        // being typed into.
        return uui_hit(g.x, g.y, g.w, g.h, mx, my);
    }

    const struct osk_key *k = &g_rows[r][c];
    if (k->mod) {
        g_mods ^= k->mod;      // sticky: armed until the next ordinary key
    } else {
        type_key(k);
        g_mods = 0;
    }
    osk_damage();
    return 1;
}

void osk_draw(int mx, int my) {
    if (!osk_open) return;

    struct osk_geom g;
    osk_geometry(&g);

    uint32_t bg = UTHEME_PANEL_BG, border = UTHEME_BORDER, fg = UTHEME_TEXT;
    ugfx_fill_rect(wm_surface(), g.x, g.y, g.w, g.h, bg);
    ugfx_fill_rect(wm_surface(), g.x, g.y, g.w, 1, border);

    int shifted = (g_mods & KEY_MOD_SHIFT) != 0;

    for (int r = 0; r < OSK_ROWS; r++) {
        for (int c = 0; c < g_row_len[r]; c++) {
            const struct osk_key *k = &g_rows[r][c];
            int x, y, w, h;
            key_rect(&g, r, c, &x, &y, &w, &h);

            // An armed modifier reads as SELECTED, not as hovered --
            // selection outranks hover, so the accent says "this is
            // held" while the pointer is elsewhere.
            int armed = k->mod && (g_mods & k->mod);
            int down = (r == g_pressed_row && c == g_pressed_col);
            uint32_t cap_bg = bg, cap_fg = fg;
            if (armed) {
                cap_bg = UTHEME_ACCENT;
                cap_fg = UTHEME_ACCENT_TEXT;
            } else if (down) {
                cap_bg = uui_state_bg(bg, UUI_STATE_PRESSED);
            } else if (uui_hit(x, y, w - 1, h - 1, mx, my)) {
                cap_bg = uui_state_bg(bg, UUI_STATE_HOVER);
            }

            ugfx_fill_rect(wm_surface(), x + 1, y + 1, w - 2, h - 2, cap_bg);
            ugfx_draw_rect(wm_surface(), x + 1, y + 1, w - 2, h - 2, border);

            const char *cap = (shifted && k->cap_sh) ? k->cap_sh : k->cap;
            int tw = ugfx_text_width(cap);
            int cap_x = x + (w - tw) / 2;
            if (cap_x < x + 1) cap_x = x + 1;
            ugfx_draw_string_clipped(wm_surface(), cap_x,
                                     y + (h - ugfx_char_h()) / 2,
                                     x + w - 1 - cap_x, cap, cap_fg, cap_bg);
        }
    }
}
