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
#include "ui/uui_popup.h"
#include "wm_shadow.h"

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
// Font-derived, never fixed pixels (docs/gui-guidelines.md). FLOATING by
// default -- Windows 11's touch keyboard: a card above the taskbar with a
// bar to drag it by, Dock and close -- or DOCKED, full width as it was.
// The bar is part of both, so Dock is always one click back.

struct osk_geom {
    int x, y, w, h;
    int pad, row_h, half_w;
    int kx, ky;                       // the keys' origin
    int bar_h;
    int dock_x, dock_w, close_x, close_w, btn_y, btn_h;
    int docked;
};

static int g_docked;
static int g_float_x = -1, g_float_y = -1;   // -1: centred above the taskbar
static int g_drag, g_drag_dx, g_drag_dy;

#define OSK_DOCK_LABEL  (g_docked ? "Float" : "Dock")

static void osk_geometry(struct osk_geom *g) {
    int ch = ugfx_char_h();
    g->docked = g_docked;
    g->pad = ch / 2 + 2;
    g->row_h = ch * 2 + 6;
    g->bar_h = ch + 14;
    g->w = g_docked ? screen_w : (ch * 56 < screen_w - 24 ? ch * 56 : screen_w - 24);
    g->half_w = (g->w - 2 * g->pad) / OSK_ROW_SPAN;
    g->h = g->bar_h + OSK_ROWS * g->row_h + g->pad;
    if (g_docked) {
        g->x = 0;
        g->y = screen_h - taskbar_h - g->h;
    } else {
        int x = g_float_x, y = g_float_y;
        if (x < 0) { x = (screen_w - g->w) / 2; y = screen_h - taskbar_h - 12 - g->h; }
        if (x > screen_w - g->w) x = screen_w - g->w;
        if (y > screen_h - taskbar_h - g->h) y = screen_h - taskbar_h - g->h;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        g->x = x; g->y = y;
    }
    g->kx = g->x + (g->w - OSK_ROW_SPAN * g->half_w) / 2;   // the rounding, split
    g->ky = g->y + g->bar_h;
    g->btn_h = g->bar_h - 8;
    g->btn_y = g->y + 4;
    g->close_w = g->btn_h + 6;
    g->close_x = g->x + g->w - g->pad / 2 - g->close_w;
    g->dock_w = ugfx_text_width(OSK_DOCK_LABEL) + ch;
    g->dock_x = g->close_x - 4 - g->dock_w;
}

// The keycap's box. Walks the row's spans, which is the SAME walk that
// draws it -- so a click cannot be told a different rect from the one
// the cap was painted at, the rule wm_tray.c's tray_item_rect() follows.
static void key_rect(const struct osk_geom *g, int row, int col,
                     int *x, int *y, int *w, int *h) {
    int span = 0;
    for (int i = 0; i < col; i++) span += g_rows[row][i].span;
    *x = g->kx + span * g->half_w;
    *y = g->ky + row * g->row_h;
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
// key -- api/keyboard.h's contract, and getting it wrong is silent:
// Ctrl-C as 'c' + KEY_MOD_CTRL reaches an app that (correctly) never
// tests that bit for a letter.
static void type_key(const struct osk_key *k) {
    struct window *win = focused_client();
    if (!win) return;

    int shifted = (g_mods & KEY_MOD_SHIFT) != 0;
    int code = shifted && k->code_sh ? k->code_sh : k->code;
    if (!code) return;

    if (g_mods & KEY_MOD_CTRL) {
        // Ctrl folds a LETTER to its control code; anything else is the
        // key itself, with the bit.
        int lower = (code >= 'A' && code <= 'Z') ? code - 'A' + 'a' : code;
        if (lower >= 'a' && lower <= 'z') {
            send_one(win, lower - 'a' + 1, g_mods);
            return;
        }
    }
    // Alt is the key with KEY_MOD_ALT; the ESC prefix a terminal wants
    // is the terminal's to add.
    send_one(win, code, g_mods);
}

// --- what the debug console reports ------------------------------------

void osk_report(struct osk_report *r) {
    struct osk_geom g;
    osk_geometry(&g);
    r->x = g.x; r->y = g.y; r->w = g.w; r->h = g.h;
    r->mods = g_mods;
    r->docked = g.docked;
    r->bar_h = g.bar_h;
    r->dock_cx = g.dock_x + g.dock_w / 2;  r->dock_cy = g.btn_y + g.btn_h / 2;
    r->close_cx = g.close_x + g.close_w / 2; r->close_cy = g.btn_y + g.btn_h / 2;
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

int osk_rect(int *x, int *y, int *w, int *h) {
    struct osk_geom g;
    osk_geometry(&g);
    *x = g.x; *y = g.y; *w = g.w; *h = g.h;
    return 1;
}

void osk_damage(void) { wm_overlay_damage("osk"); }

static void osk_close_panel(void) {
    if (!osk_open) return;
    osk_damage();          // while it is still up, so its rect repaints
    osk_open = 0;
    g_mods = 0;
    g_drag = 0;
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
    if (osk_open && uui_hit(g.dock_x, g.btn_y, g.dock_w, g.btn_h, mx, my)) return 10000;
    if (osk_open && uui_hit(g.close_x, g.btn_y, g.close_w, g.btn_h, mx, my)) return 10001;
    int r, c;
    if (!key_at(&g, mx, my, &r, &c)) return 0;
    return r * OSK_MAX_COLS + c + 1;   // 0 means none, so bias by one
}

void osk_update_press(int mx, int my, uint8_t buttons) {
    if (!osk_open) return;
    struct osk_geom g;
    osk_geometry(&g);
    if (g_drag) {
        // MOVED BY ITS BAR, the core damaging the rect it left
        // (wm_overlay.h); released, it stays where it was put.
        if (!(buttons & 0x1)) { g_drag = 0; return; }
        g_float_x = mx - g_drag_dx;
        g_float_y = my - g_drag_dy;
        osk_damage();
        return;
    }
    if (!(buttons & 0x1)) { g_pressed_row = g_pressed_col = -1; return; }
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
    if (uui_hit(g.close_x, g.btn_y, g.close_w, g.btn_h, mx, my)) {
        osk_close_panel();
        return 1;
    }
    if (uui_hit(g.dock_x, g.btn_y, g.dock_w, g.btn_h, mx, my)) {
        osk_damage();
        g_docked = !g_docked;
        osk_damage();
        return 1;
    }
    if (!g_docked && uui_hit(g.x, g.y, g.w, g.bar_h, mx, my)) {
        g_drag = 1;
        g_drag_dx = mx - g.x;
        g_drag_dy = my - g.y;
        return 1;
    }
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

// A function key: the modifiers, the editing and navigation keys. Drawn
// a step darker than a letter, as every touch keyboard does.
static int is_fn(const struct osk_key *k) {
    if (k->mod) return 1;
    int c = k->code;
    return c == '\b' || c == '\t' || c == 27 || c == KEY_DELETE || c == KEY_ARROW_LEFT ||
           c == KEY_ARROW_RIGHT || c == KEY_ARROW_UP || c == KEY_ARROW_DOWN;
}

void osk_draw(int mx, int my) {
    if (!osk_open) return;

    struct osk_geom g;
    osk_geometry(&g);
    struct ugfx_surface *s = wm_surface();
    int ch = ugfx_char_h();

    uint32_t ground = ugfx_blend(uui_popup_bg(), UTHEME_CHROME, 64);
    uint32_t edge = uui_popup_border(), fg = UTHEME_TEXT;
    uint32_t face = UTHEME_WHITE, fn_face = ugfx_blend(ground, UTHEME_CHROME, 200);
    if (g.docked) {
        ugfx_fill_rect(s, g.x, g.y, g.w, g.h, ground);
        ugfx_fill_rect(s, g.x, g.y, g.w, 1, edge);
    } else {
        int r = uui_popup_radius() + 2;
        wm_shadow_draw(g.x, g.y, g.w, g.h, r, WM_SHADOW_POPUP);
        uui_fill_round_rect(s, g.x, g.y, g.w, g.h, r, edge);
        uui_fill_round_rect(s, g.x + 1, g.y + 1, g.w - 2, g.h - 2, r - 1, ground);
    }

    // THE BAR: what the keys type, a grip to drag by, Dock and close.
    int by = g.y + (g.bar_h - ch) / 2;
    ugfx_draw_string_clipped(s, g.x + g.pad + 4, by, g.w / 3, "English (US)",
                             ugfx_blend(fg, ground, 60), ground);
    if (!g.docked)
        uui_fill_round_rect(s, g.x + (g.w - 44) / 2, g.y + g.bar_h / 2 - 2, 44, 5, UUI_CAPSULE,
                            ugfx_blend(edge, UTHEME_TEXT, 80));
    int hot_dock = uui_hit(g.dock_x, g.btn_y, g.dock_w, g.btn_h, mx, my);
    int hot_close = uui_hit(g.close_x, g.btn_y, g.close_w, g.btn_h, mx, my);
    if (hot_dock) uui_fill_round_rect(s, g.dock_x, g.btn_y, g.dock_w, g.btn_h, 5,
                                      uui_state_bg(ground, UUI_STATE_HOVER));
    if (hot_close) uui_fill_round_rect(s, g.close_x, g.btn_y, g.close_w, g.btn_h, 5,
                                       uui_state_bg(ground, UUI_STATE_HOVER));
    ugfx_draw_string_clipped(s, g.dock_x + ch / 2, by, g.dock_w - ch / 2, OSK_DOCK_LABEL, fg,
                             hot_dock ? uui_state_bg(ground, UUI_STATE_HOVER) : ground);
    int cx = g.close_x + g.close_w / 2, cy = g.btn_y + g.btn_h / 2, k = ch / 3;
    ugfx_draw_line(s, cx - k, cy - k, cx + k, cy + k, fg, GEOM_AA);
    ugfx_draw_line(s, cx - k, cy + k, cx + k, cy - k, fg, GEOM_AA);

    int shifted = (g_mods & KEY_MOD_SHIFT) != 0;
    for (int r = 0; r < OSK_ROWS; r++) {
        for (int c = 0; c < g_row_len[r]; c++) {
            const struct osk_key *key = &g_rows[r][c];
            int x, y, w, h;
            key_rect(&g, r, c, &x, &y, &w, &h);

            // An armed modifier reads as SELECTED, not as hovered --
            // selection outranks hover, so the accent says "this is
            // held" while the pointer is elsewhere. Enter is the accent
            // too: the key that commits, as on every touch keyboard.
            int armed = key->mod && (g_mods & key->mod);
            int down = (r == g_pressed_row && c == g_pressed_col);
            uint32_t cap_bg = is_fn(key) ? fn_face : face, cap_fg = fg;
            if (armed || key->code == '\n') {
                cap_bg = UTHEME_ACCENT;
                cap_fg = UTHEME_ACCENT_TEXT;
                if (down) cap_bg = uui_state_bg(cap_bg, UUI_STATE_PRESSED);
            } else if (down) {
                cap_bg = uui_state_bg(cap_bg, UUI_STATE_PRESSED);
            } else if (uui_hit(x, y, w - 1, h - 1, mx, my)) {
                cap_bg = uui_state_bg(cap_bg, UUI_STATE_HOVER);
            }

            int ix = x + 3, iy = y + 3, iw = w - 6, ih = h - 6;
            // A cap is lifted by a hairline under it, not outlined.
            uui_fill_round_rect(s, ix, iy + 1, iw, ih, 6, ugfx_blend(ground, UTHEME_TEXT, 40));
            uui_fill_round_rect(s, ix, iy, iw, ih, 6, cap_bg);

            const char *cap = (shifted && key->cap_sh) ? key->cap_sh : key->cap;
            int tw = ugfx_text_width(cap);
            int cap_x = ix + (iw - tw) / 2;
            if (cap_x < ix + 2) cap_x = ix + 2;
            ugfx_draw_string_clipped(s, cap_x, iy + (ih - ch) / 2, ix + iw - 2 - cap_x, cap,
                                     cap_fg, cap_bg);
        }
    }
}
