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
#include "ui/uui_keymap.h"
#include "lib/ukeymap.h"
#include "lib/usetting.h"
#include "lib/uconf.h"
#include "keyboard_layout.h"
#include "wm_conf.h"
#include "wm_shadow.h"

int osk_open = 0;

static int g_tray_id = -1;

// The armed sticky modifiers (KEY_MOD_*), and the keycap the pointer is
// holding down. A modifier is armed by a click and consumed by the next
// ordinary key, which is what makes a one-pointer device able to type
// Ctrl-C at all.
static unsigned g_mods = 0;
static int g_pressed = -1;

// --- the layout -------------------------------------------------------
//
// THE CONFIGURED LAYOUT, typed with the kernel's own translator compiled
// into this process (api/keyboard_layout.h, lib/ukeymap.h): loading the
// snapshot the board draws from also loads the tables typing reads, so
// a cap cannot show one character and type another. Reloaded when the
// settings generation moves -- Super+Space, Settings, `config set`.
#define OSK_ROWS 5
#define SET_ACTIVE "system.keyboard_layout"
#define SET_DEAD   "system.keyboard_dead_keys"
#define NAMES_FILE "/etc/settings.d/system.keyboard_layout"

static struct ukeymap g_map;
static struct uui_keymap g_board;
static char g_layout_name[48] = "English (US)";
static uint32_t g_layout_gen = 0xFFFFFFFFu;
static int g_pending_kc;    // the dead key waiting for its letter, or 0

static void load_layout(void) {
    uint32_t gen = wm_setting_generation();
    if (gen == g_layout_gen) return;
    g_layout_gen = gen;
    char code[32], dead[8];
    if (!usetting_get(SET_ACTIVE, code, sizeof code) || !code[0]) k_strlcpy(code, "us", sizeof code);
    if (!ukeymap_load(&g_map, code) && !ukeymap_load(&g_map, "us")) {
        // No file at all: the kernel's compiled-in table, so the panel
        // still types rather than showing blank caps.
        keyboard_layout_use_fallback();
        ukeymap_snapshot(&g_map);
    }
    keyboard_layout_set_dead_keys(!(usetting_get(SET_DEAD, dead, sizeof dead) && !k_strcmp(dead, "off")));
    g_pending_kc = 0;
    char key[48];
    k_snprintf(key, sizeof key, "Choice.%s", code);
    if (!uconf_get(NAMES_FILE, key, g_layout_name, sizeof g_layout_name))
        k_strlcpy(g_layout_name, code, sizeof g_layout_name);
}

// --- geometry ---------------------------------------------------------
//
// Font-derived, never fixed pixels (docs/gui-guidelines.md). FLOATING by
// default -- Windows 11's touch keyboard: a card above the taskbar with a
// bar to drag it by, Dock and close -- or DOCKED, full width as it was.
// The bar is part of both, so Dock is always one click back.

struct osk_geom {
    int x, y, w, h;
    int pad, row_h;
    int kx, ky, kw, kh;               // the keys' rect
    int bar_h;
    int dock_x, dock_w, close_x, close_w, btn_y, btn_h;
    int docked;
};

static int g_docked;
static int g_float_x, g_float_y;
static int g_placed;   // 0: centred above the taskbar, until the bar is dragged
static int g_drag, g_drag_dx, g_drag_dy;

#define OSK_DOCK_LABEL  (g_docked ? "Float" : "Dock")

static void osk_geometry(struct osk_geom *g) {
    int ch = ugfx_char_h();
    g->docked = g_docked;
    g->pad = ch / 2 + 2;
    g->row_h = ch * 2 + 6;
    g->bar_h = ch + 14;
    g->w = g_docked ? screen_w : (ch * 56 < screen_w - 24 ? ch * 56 : screen_w - 24);
    g->h = g->bar_h + OSK_ROWS * g->row_h + g->pad;
    if (g_docked) {
        g->x = 0;
        g->y = screen_h - taskbar_h - g->h;
    } else {
        int x = g_float_x, y = g_float_y;
        if (!g_placed) { x = (screen_w - g->w) / 2; y = screen_h - taskbar_h - 12 - g->h; }
        if (x > screen_w - g->w) x = screen_w - g->w;
        if (y > screen_h - taskbar_h - g->h) y = screen_h - taskbar_h - g->h;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        g->x = x; g->y = y;
    }
    g->kx = g->x + g->pad;
    g->ky = g->y + g->bar_h;
    g->kw = g->w - 2 * g->pad;
    g->kh = OSK_ROWS * g->row_h;
    g->btn_h = g->bar_h - 8;
    g->btn_y = g->y + 4;
    g->close_w = g->btn_h + 6;
    g->close_x = g->x + g->w - g->pad / 2 - g->close_w;
    g->dock_w = ugfx_text_width(OSK_DOCK_LABEL) + ch;
    g->dock_x = g->close_x - 4 - g->dock_w;
}

// The board, placed where this geometry puts the keys. Every caller
// that hit-tests or draws places it first, so the two cannot disagree.
static struct uui_keymap *board(const struct osk_geom *g) {
    load_layout();
    if (!g_board.map) {
        uui_keymap_init(&g_board, &g_map);
        g_board.board = UUI_KEYMAP_TYPING;
    }
    uui_keymap_set_geometry(&g_board, g->kx, g->ky, g->kw, g->kh);
    return &g_board;
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

// Emits one character, encoded the way keyboard.c encodes the physical
// key -- api/keyboard.h's contract, and getting it wrong is silent:
// Ctrl-C as 'c' + KEY_MOD_CTRL reaches an app that (correctly) never
// tests that bit for a letter.
static void type_code(struct window *win, int code) {
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

// One key of the board, as keyboard.c's key_event_body() handles the
// physical one: a symbol from the layout, then, unless Ctrl or Alt makes
// it a shortcut, through the dead-key composer -- 0, 1 or 2 characters.
static void type_key(const struct uui_keymap_key *k) {
    struct window *win = focused_client();
    if (!win) return;
    int shortcut = (g_mods & (KEY_MOD_CTRL | KEY_MOD_ALT)) != 0;
    int sym = k->kc ? keyboard_layout_translate((uint16_t)k->kc, (g_mods & KEY_MOD_SHIFT) != 0,
                                                (g_mods & KEY_MOD_ALTGR) != 0)
                    : k->action;
    if (!sym) return;
    // A function key composes only where the physical one does: Space
    // types a pending accent, Backspace and Esc take it back. The rest
    // (Enter, Tab, the arrows) leave a pending accent pending.
    int composes = k->kc || sym == ' ' || sym == '\b' || sym == 27;
    if (shortcut || !composes) {
        type_code(win, shortcut && k->kc ? keyboard_layout_spacing(sym) : sym);
        return;
    }
    uint8_t out[2];
    int pending_before = keyboard_layout_dead_pending();
    int n = keyboard_layout_compose(sym, out);
    for (int i = 0; i < n; i++) type_code(win, out[i]);
    // Backspace or Esc with nothing pending is just itself.
    if (!pending_before && n == 0 && !KB_SYM_IS_DEAD(sym)) type_code(win, sym);
    g_pending_kc = keyboard_layout_dead_pending() ? k->kc : 0;
}

// --- what the debug console reports ------------------------------------

void osk_report(struct osk_report *r) {
    struct osk_geom g;
    osk_geometry(&g);
    r->x = g.x; r->y = g.y; r->w = g.w; r->h = g.h;
    r->mods = g_mods;
    load_layout();
    k_strlcpy(r->layout, g_layout_name, sizeof r->layout);
    r->pending_kc = g_pending_kc;
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
    struct uui_keymap *b = board(&g);
    int i = uui_keymap_find(b, cap);
    return i >= 0 && uui_keymap_key_rect(b, i, x, y, w, h);
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
    keyboard_layout_compose_reset();
    g_pending_kc = 0;
    g_drag = 0;
    g_pressed = -1;
}

static void osk_open_panel(void) {
    // Deliberately does NOT call wm_overlay_close_others(): this panel
    // has no `close` op, so it is not dismissed by other popups either,
    // and a keyboard that shut the menu it was typing into would be
    // useless.
    osk_open = 1;
    g_mods = 0;
    g_pressed = -1;
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
    int i = uui_keymap_key_at(board(&g), mx, my);
    return i + 1;   // 0 means none, so bias by one
}

void osk_update_press(int mx, int my, uint8_t buttons) {
    if (!osk_open) return;
    struct osk_geom g;
    osk_geometry(&g);
    if (g_drag) {
        // MOVED BY ITS BAR, the core damaging the rect it left
        // (wm_overlay.h); released, it stays where it was put.
        if (!(buttons & 0x1)) { g_drag = 0; return; }
        // CLAMPED AS IT IS STORED, so the bar stays under the pointer at
        // the edges instead of the stored place running off the screen.
        int x = mx - g_drag_dx, y = my - g_drag_dy;
        if (x > screen_w - g.w) x = screen_w - g.w;
        if (y > screen_h - taskbar_h - g.h) y = screen_h - taskbar_h - g.h;
        g_float_x = x < 0 ? 0 : x;
        g_float_y = y < 0 ? 0 : y;
        g_placed = 1;
        osk_damage();
        return;
    }
    if (!(buttons & 0x1)) { g_pressed = -1; return; }
    int i = uui_keymap_key_at(board(&g), mx, my);
    if (i == g_pressed) return;
    g_pressed = i;
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
    struct uui_keymap_key key;
    int i = uui_keymap_key_at(board(&g), mx, my);
    if (i < 0 || !uui_keymap_key(&g_board, i, &key)) {
        // A click outside the panel is NOT ours: it belongs to whatever
        // is beneath, which is how the caret gets placed in the field
        // being typed into.
        return uui_hit(g.x, g.y, g.w, g.h, mx, my);
    }

    if (key.mod) {
        g_mods ^= key.mod;     // sticky: armed until the next ordinary key
    } else if (key.kc || key.action) {
        type_key(&key);
        g_mods = 0;
    }
    osk_damage();
    return 1;
}

void osk_draw(int mx, int my) {
    if (!osk_open) return;

    struct osk_geom g;
    osk_geometry(&g);
    struct ugfx_surface *s = wm_surface();
    int ch = ugfx_char_h();

    uint32_t ground = ugfx_blend(uui_popup_bg(), UTHEME_CHROME, 64);
    uint32_t edge = uui_popup_border(), fg = UTHEME_TEXT;
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
    ugfx_draw_string_clipped(s, g.x + g.pad + 4, by, g.w / 3, g_layout_name,
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

    struct uui_keymap *b = board(&g);
    b->bg = ground;
    b->levels = g_mods & (KEY_MOD_SHIFT | KEY_MOD_ALTGR);
    b->armed = g_mods;
    b->pending_kc = g_pending_kc;
    b->pressed = g_pressed;
    b->hover = uui_keymap_key_at(b, mx, my);
    uui_keymap_draw(s, b);
}
