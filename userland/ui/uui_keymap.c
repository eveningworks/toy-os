// keymap -- see ui/uui_keymap.h.
#include "ui/uui_keymap.h"
#include "ui/utheme.h"
#include "ui/uui_primitives.h"
#include "keyboard.h"   // KEY_* codes and KEY_MOD_* bits
#include <string.h>

// --- the boards ----------------------------------------------------------
//
// Widths in 38ths of a cap, the ISO board's own proportions (Backspace
// 2u, Tab and Enter 1.5u, Caps 1.75u, right Shift 2.75u): with a gap of
// four 38ths between caps every row adds up to the same 622, so the
// right edge is straight. A key with a label is a function key.

struct key { int kc; int w; const char *label; int action; unsigned mod; };

#define C(kc)               { kc, 38, 0, 0, 0 }
#define FN(w, l, act)       { 0, w, l, act, 0 }
#define MOD(w, l, bit)      { 0, w, l, 0, bit }

static const struct key P_ROW0[] = {
    C(41),C(2),C(3),C(4),C(5),C(6),C(7),C(8),C(9),C(10),C(11),C(12),C(13), FN(76, "Bksp", 0) };
static const struct key P_ROW1[] = {
    FN(57, "Tab", 0),C(16),C(17),C(18),C(19),C(20),C(21),C(22),C(23),C(24),C(25),C(26),C(27),
    FN(57, "Enter", 0) };
static const struct key P_ROW2[] = {
    FN(66, "Caps", 0),C(30),C(31),C(32),C(33),C(34),C(35),C(36),C(37),C(38),C(39),C(40),C(43),
    FN(48, "", 0) };
static const struct key P_ROW3[] = {
    FN(48, "Shift", 0),C(86),C(44),C(45),C(46),C(47),C(48),C(49),C(50),C(51),C(52),C(53),
    FN(108, "Shift", 0) };

// The on-screen keyboard's board: the picture's typing rows, with the
// ISO Enter's two halves traded for a plain Enter and Del, Caps for Esc
// (a touch keyboard has no Caps Lock to latch), and a bottom row. The
// labels double as the names `gui osk key <cap>` looks a key up by, so
// they are words: " " or "<" cannot be a debug-console token.
static const struct key T_ROW0[] = {
    C(41),C(2),C(3),C(4),C(5),C(6),C(7),C(8),C(9),C(10),C(11),C(12),C(13), FN(76, "Bksp", '\b') };
static const struct key T_ROW1[] = {
    FN(57, "Tab", '\t'),C(16),C(17),C(18),C(19),C(20),C(21),C(22),C(23),C(24),C(25),C(26),C(27),
    FN(57, "Del", KEY_DELETE) };
static const struct key T_ROW2[] = {
    FN(57, "Esc", 27),C(30),C(31),C(32),C(33),C(34),C(35),C(36),C(37),C(38),C(39),C(40),C(43),
    FN(57, "Enter", '\n') };
static const struct key T_ROW3[] = {
    MOD(48, "Shift", KEY_MOD_SHIFT),C(86),C(44),C(45),C(46),C(47),C(48),C(49),C(50),C(51),C(52),
    C(53), MOD(108, "Shift", KEY_MOD_SHIFT) };
static const struct key T_ROW4[] = {
    MOD(57, "Ctrl", KEY_MOD_CTRL), MOD(57, "Alt", KEY_MOD_ALT), FN(271, "Space", ' '),
    MOD(57, "AltGr", KEY_MOD_ALTGR),
    FN(38, "Left", KEY_ARROW_LEFT), FN(38, "Down", KEY_ARROW_DOWN),
    FN(38, "Up", KEY_ARROW_UP), FN(38, "Right", KEY_ARROW_RIGHT) };

struct row { const struct key *keys; int n; };
#define ROW(r) { r, (int)(sizeof r / sizeof r[0]) }
static const struct row PICTURE[] = { ROW(P_ROW0), ROW(P_ROW1), ROW(P_ROW2), ROW(P_ROW3) };
static const struct row TYPING[] = { ROW(T_ROW0), ROW(T_ROW1), ROW(T_ROW2), ROW(T_ROW3), ROW(T_ROW4) };
#define BOARD_38THS 622
#define GAP_38THS   4

static const struct row *rows_of(const struct uui_keymap *k, int *n) {
    if (k->board == UUI_KEYMAP_TYPING) { *n = (int)(sizeof TYPING / sizeof TYPING[0]); return TYPING; }
    *n = (int)(sizeof PICTURE / sizeof PICTURE[0]);
    return PICTURE;
}

// A cap is two text lines and a margin tall; the gap is a ninth of it.
static int natural_cap(void) { return ugfx_char_h() * 2 + 6; }
static int gap_of(int cap)   { int g = cap / 9; return g < 2 ? 2 : g; }

// The picture's cap: its natural size, or smaller to fit.
static int cap_for(const struct uui_keymap *k) {
    int cap = natural_cap();
    if (k->w > 0 && k->w * 38 / BOARD_38THS < cap) cap = k->w * 38 / BOARD_38THS;
    return cap < ugfx_char_h() + 4 ? ugfx_char_h() + 4 : cap;
}

void uui_keymap_init(struct uui_keymap *k, const struct ukeymap *map) {
    memset(k, 0, sizeof *k);
    k->map = map;
    k->bg = UUI_COLOR_UNSET;
    k->hover = k->pressed = -1;
}

void uui_keymap_natural_size(const struct uui_keymap *k, int *out_w, int *out_h) {
    int n;
    rows_of(k, &n);
    int cap = natural_cap();
    if (out_w) *out_w = BOARD_38THS * cap / 38;
    if (out_h) *out_h = n * cap + (n - 1) * gap_of(cap);
}

void uui_keymap_set_geometry(struct uui_keymap *k, int x, int y, int w, int h) {
    k->x = x; k->y = y; k->w = w; k->h = h;
}

int uui_keymap_key_count(const struct uui_keymap *k) {
    int n, total = 0;
    const struct row *rows = rows_of(k, &n);
    for (int r = 0; r < n; r++) total += rows[r].n;
    return total;
}

static const struct key *key_n(const struct uui_keymap *k, int i, int *row, int *col) {
    int n;
    const struct row *rows = rows_of(k, &n);
    if (i < 0) return 0;
    for (int r = 0; r < n; r++) {
        if (i < rows[r].n) {
            if (row) *row = r;
            if (col) *col = i;
            return &rows[r].keys[i];
        }
        i -= rows[r].n;
    }
    return 0;
}

int uui_keymap_key(const struct uui_keymap *k, int i, struct uui_keymap_key *out) {
    const struct key *key = key_n(k, i, 0, 0);
    if (!key) return 0;
    out->kc = key->kc;
    out->action = key->action;
    out->mod = key->mod;
    out->label = key->label;
    return 1;
}

// THE ONE WALK: drawing and hit-testing both place a key here, so a
// click cannot be told a different box from the one the cap was painted
// at (the rule wm_tray.c's tray_item_rect() follows).
int uui_keymap_key_rect(const struct uui_keymap *k, int i, int *x, int *y, int *w, int *h) {
    int row, col, n;
    const struct row *rows = rows_of(k, &n);
    if (!key_n(k, i, &row, &col)) return 0;
    const struct row *rw = &rows[row];
    int at = 0;
    for (int c = 0; c < col; c++) at += rw->keys[c].w + GAP_38THS;
    if (k->board == UUI_KEYMAP_TYPING) {
        // Fills the rect: 38ths across the width, rows down the height.
        int vgap = k->h / 60 < 2 ? 2 : k->h / 60;
        int rh = (k->h - (n - 1) * vgap) / n;
        *x = k->x + at * k->w / BOARD_38THS;
        *w = rw->keys[col].w * k->w / BOARD_38THS;
        *y = k->y + row * (rh + vgap);
        *h = rh;
        if (col == rw->n - 1) *w = k->x + k->w - *x;   // every row ends level
        return 1;
    }
    int cap = cap_for(k), gap = gap_of(cap);
    // Gaps in pixels here, as the picture always drew them.
    int px = k->x;
    for (int c = 0; c < col; c++) px += rw->keys[c].w * cap / 38 + gap;
    *x = px;
    *w = rw->keys[col].w * cap / 38;
    if (col == rw->n - 1) *w = k->x + BOARD_38THS * cap / 38 - px;
    *y = k->y + row * (cap + gap);
    *h = cap;
    return 1;
}

int uui_keymap_key_at(const struct uui_keymap *k, int mx, int my) {
    int total = uui_keymap_key_count(k);
    for (int i = 0; i < total; i++) {
        int x, y, w, h;
        uui_keymap_key_rect(k, i, &x, &y, &w, &h);
        if (uui_hit(x, y, w - 1, h - 1, mx, my)) return i;
    }
    return -1;
}

int uui_keymap_find(const struct uui_keymap *k, const char *name) {
    if (!name || !name[0]) return -1;
    int want_kc = 0;
    if (name[0] == 'k' && name[1] == 'c' && name[2] >= '0' && name[2] <= '9') {
        for (const char *p = name + 2; *p >= '0' && *p <= '9'; p++) want_kc = want_kc * 10 + (*p - '0');
    }
    int total = uui_keymap_key_count(k);
    for (int i = 0; i < total; i++) {
        const struct key *key = key_n(k, i, 0, 0);
        if (key->label) {
            if (key->label[0] && !strcmp(key->label, name)) return i;
            continue;
        }
        if (want_kc && key->kc == want_kc) return i;
        if (!want_kc && k->map && !name[1] &&
            ukeymap_char(k->map, key->kc, UKEYMAP_BASE, 0) == (unsigned char)name[0])
            return i;
    }
    return -1;
}

// --- drawing ---------------------------------------------------------------

static void text1(struct ugfx_surface *s, int x, int y, int c, uint32_t fg, uint32_t bg) {
    char t[2] = { (char)c, 0 };
    ugfx_draw_string_clipped(s, x, y, ugfx_text_width(t) + 1, t, fg, bg);
}

// A dashed outline: the cap's four edges in short runs.
static void dashed(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t c) {
    int d = 3;
    for (int i = 0; i < w; i += 2 * d) {
        int n = i + d > w ? w - i : d;
        ugfx_fill_rect(s, x + i, y, n, 1, c);
        ugfx_fill_rect(s, x + i, y + h - 1, n, 1, c);
    }
    for (int j = 0; j < h; j += 2 * d) {
        int n = j + d > h ? h - j : d;
        ugfx_fill_rect(s, x, y + j, 1, n, c);
        ugfx_fill_rect(s, x + w - 1, y + j, 1, n, c);
    }
}

// A letter shown once, as its capital: a cap reads "A", not "a" and "A".
static int is_case_pair(int lo, int up) {
    if (lo >= 'a' && lo <= 'z') return up == lo - 32;
    if (lo >= 0xE0 && lo <= 0xFE && lo != 0xF7) return up == lo - 32;
    return 0;
}

static int any_dead(const struct uui_keymap *k, int kc) {
    if (!k->map || !kc) return 0;
    int d = 0;
    for (int l = 0; l < 4; l++) {
        int dl = 0;
        ukeymap_char(k->map, kc, (enum ukeymap_level)l, &dl);
        d |= dl;
    }
    return d;
}

static void draw_picture_key(struct ugfx_surface *s, const struct uui_keymap *k,
                             const struct key *key, int x, int y, int w, int cap, uint32_t bg) {
    uint32_t fill = ugfx_blend(bg, UTHEME_TEXT, 14);
    uint32_t edge = ugfx_blend(bg, UTHEME_TEXT, 50);
    uint32_t dim  = ugfx_blend(bg, UTHEME_TEXT, 165);
    int ch = ugfx_char_h(), pad = cap / 7;
    int r = cap / 9;

    int base = 0, shift = 0, altgr = 0, dead_b = 0, dead_s = 0, dead_a = 0;
    if (!key->label && k->map) {
        base  = ukeymap_char(k->map, key->kc, UKEYMAP_BASE, &dead_b);
        shift = ukeymap_char(k->map, key->kc, UKEYMAP_SHIFT, &dead_s);
        altgr = ukeymap_char(k->map, key->kc, UKEYMAP_ALTGR, &dead_a);
    }
    int dead = dead_b || dead_s || dead_a;

    uui_fill_round_rect(s, x, y, w, cap, r, dead ? fill : edge);
    uui_fill_round_rect(s, x + 1, y + 1, w - 2, cap - 2, r > 0 ? r - 1 : 0, fill);
    if (dead) dashed(s, x, y, w, cap, UTHEME_ACCENT);

    if (key->label) {
        if (key->label[0])
            ugfx_draw_string_clipped(s, x + pad, y + (cap - ch) / 2, w - 2 * pad, key->label, dim, fill);
        return;
    }
    if (is_case_pair(base, shift)) { base = shift; shift = 0; }
    int low = y + cap - ch - pad / 2;
    if (base)  text1(s, x + pad, low, base, dead_b ? UTHEME_ACCENT : UTHEME_TEXT, fill);
    if (shift) text1(s, x + pad, y + pad / 2, shift, dead_s ? UTHEME_ACCENT : dim, fill);
    if (altgr) {
        char t[2] = { (char)altgr, 0 };
        text1(s, x + w - pad - ugfx_text_width(t), low, altgr, UTHEME_ACCENT, fill);
    }
}

// What a typing cap shows under `levels`, with translate()'s fallthrough
// (level 4 -> 3 -> Shift/base), and whether that came from AltGr.
static int active_char(const struct uui_keymap *k, int kc, int *from_altgr) {
    int shift = (k->levels & KEY_MOD_SHIFT) != 0, altgr = (k->levels & KEY_MOD_ALTGR) != 0;
    *from_altgr = 0;
    if (!k->map) return 0;
    if (altgr) {
        int c = shift ? ukeymap_char(k->map, kc, UKEYMAP_SHIFT_ALTGR, 0) : 0;
        if (!c) c = ukeymap_char(k->map, kc, UKEYMAP_ALTGR, 0);
        // A level that repeats the base (XKB writes q on AltGr+q) adds nothing.
        if (c && c != ukeymap_char(k->map, kc, UKEYMAP_BASE, 0) &&
            c != ukeymap_char(k->map, kc, UKEYMAP_SHIFT, 0)) {
            *from_altgr = 1;
            return c;
        }
    }
    return ukeymap_char(k->map, kc, shift ? UKEYMAP_SHIFT : UKEYMAP_BASE, 0);
}

// The AltGr character a cap hints at, 0 when it has none of its own.
static int altgr_hint(const struct uui_keymap *k, int kc) {
    if (!k->map) return 0;
    int c = ukeymap_char(k->map, kc, UKEYMAP_ALTGR, 0);
    if (c == ukeymap_char(k->map, kc, UKEYMAP_BASE, 0) ||
        c == ukeymap_char(k->map, kc, UKEYMAP_SHIFT, 0)) return 0;
    return c;
}

static void draw_typing_key(struct ugfx_surface *s, const struct uui_keymap *k, int i,
                            const struct key *key, int x, int y, int w, int h, uint32_t ground) {
    int ch = ugfx_char_h();
    uint32_t face = UTHEME_WHITE, fn_face = ugfx_blend(ground, UTHEME_CHROME, 200);
    uint32_t fg = UTHEME_TEXT;

    // An armed modifier reads as SELECTED, not as hovered -- selection
    // outranks hover, so the accent says "this is held" while the pointer
    // is elsewhere. Enter is the accent too: the key that commits.
    int armed = key->mod && (k->armed & key->mod);
    int pending = !key->label && key->kc && key->kc == k->pending_kc;
    uint32_t cap_bg = key->label ? fn_face : face, cap_fg = fg;
    if (armed || key->action == '\n') {
        cap_bg = UTHEME_ACCENT;
        cap_fg = UTHEME_ACCENT_TEXT;
    } else if (pending) {
        cap_bg = UTHEME_SELECTION;
    }
    if (i == k->pressed) cap_bg = uui_state_bg(cap_bg, UUI_STATE_PRESSED);
    else if (i == k->hover && !armed && !pending) cap_bg = uui_state_bg(cap_bg, UUI_STATE_HOVER);

    int ix = x + 3, iy = y + 3, iw = w - 6, ih = h - 6;
    // A cap is lifted by a hairline under it, not outlined.
    uui_fill_round_rect(s, ix, iy + 1, iw, ih, 6, ugfx_blend(ground, UTHEME_TEXT, 40));
    if (pending) {
        // Latched: an accent ring around the selection fill.
        uui_fill_round_rect(s, ix, iy, iw, ih, 6, UTHEME_ACCENT);
        uui_fill_round_rect(s, ix + 2, iy + 2, iw - 4, ih - 4, 4, cap_bg);
    } else {
        uui_fill_round_rect(s, ix, iy, iw, ih, 6, cap_bg);
        if (any_dead(k, key->kc)) dashed(s, ix, iy, iw, ih, UTHEME_ACCENT);
    }

    if (key->label) {
        int tw = ugfx_text_width(key->label);
        int lx = ix + (iw - tw) / 2;
        if (lx < ix + 2) lx = ix + 2;
        ugfx_draw_string_clipped(s, lx, iy + (ih - ch) / 2, ix + iw - 2 - lx, key->label, cap_fg, cap_bg);
        return;
    }

    int from_altgr = 0;
    int c = active_char(k, key->kc, &from_altgr);
    int altgr_held = (k->levels & KEY_MOD_ALTGR) != 0;
    if (c) {
        // With AltGr held, a key that has an AltGr character shows it in
        // the accent, bold; one that has none types its base, dimmed.
        uint32_t col = cap_fg;
        const struct ugfx_font *was = 0;
        if (altgr_held && from_altgr) {
            col = UTHEME_ACCENT;
            was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        } else if (altgr_held) {
            col = ugfx_blend(cap_bg, UTHEME_TEXT, 120);
        }
        char t[2] = { (char)c, 0 };
        int tw = ugfx_text_width(t);
        text1(s, ix + (iw - tw) / 2, iy + (ih - ugfx_char_h()) / 2, c, col, cap_bg);
        if (was) ugfx_set_font(was);
    }
    // THE HINT: the AltGr character, small, top right -- only while AltGr
    // is not held, since then the cap itself shows it.
    int hint = altgr_held ? 0 : altgr_hint(k, key->kc);
    if (hint) {
        const struct ugfx_font *was = ugfx_set_font(ugfx_font_caption());
        char t[2] = { (char)hint, 0 };
        int tw = ugfx_text_width(t);
        text1(s, ix + iw - tw - 5, iy + 3, hint, UTHEME_ACCENT, cap_bg);
        ugfx_set_font(was);
    }
}

void uui_keymap_draw(struct ugfx_surface *s, const struct uui_keymap *k) {
    uint32_t bg = UUI_COLOR(k->bg, UTHEME_PANEL_BG);
    int total = uui_keymap_key_count(k);
    for (int i = 0; i < total; i++) {
        const struct key *key = key_n(k, i, 0, 0);
        int x, y, w, h;
        uui_keymap_key_rect(k, i, &x, &y, &w, &h);
        if (k->board == UUI_KEYMAP_TYPING) draw_typing_key(s, k, i, key, x, y, w, h, bg);
        else                               draw_picture_key(s, k, key, x, y, w, h, bg);
    }
}

// --- the ops table ------------------------------------------------------

static void km_natural(const void *w, int *ow, int *oh) {
    uui_keymap_natural_size((const struct uui_keymap *)w, ow, oh);
}
static void km_geometry(void *w, int x, int y, int width, int height) {
    uui_keymap_set_geometry((struct uui_keymap *)w, x, y, width, height);
}
static void km_bounds(const void *w, int *x, int *y, int *width, int *height) {
    const struct uui_keymap *k = (const struct uui_keymap *)w;
    if (x) *x = k->x;
    if (y) *y = k->y;
    if (width) *width = k->w;
    if (height) *height = k->h;
}
static void km_draw(struct ugfx_surface *s, const void *w) {
    uui_keymap_draw(s, (const struct uui_keymap *)w);
}

const struct uui_widget_ops uui_keymap_ops = {
    .natural_size = km_natural,
    .set_geometry = km_geometry,
    .bounds = km_bounds,
    .draw = km_draw,
};
