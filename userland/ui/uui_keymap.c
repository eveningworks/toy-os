// keymap -- see ui/uui_keymap.h.
#include "ui/uui_keymap.h"
#include "ui/utheme.h"
#include "ui/uui_primitives.h"
#include <string.h>

// --- the board ---------------------------------------------------------
//
// Widths in 38ths of a cap, the ISO board's own proportions (Backspace
// 2u, Tab and Enter 1.5u, Caps 1.75u, right Shift 2.75u): every row adds
// up to the same 622 with its gaps, so the right edge is straight.
// A key with a label is a modifier or an editing key and types nothing.

struct key { int kc; int w; const char *label; };

static const struct key ROW0[] = {
    {41,38,0},{2,38,0},{3,38,0},{4,38,0},{5,38,0},{6,38,0},{7,38,0},{8,38,0},
    {9,38,0},{10,38,0},{11,38,0},{12,38,0},{13,38,0},{0,76,"Bksp"} };
static const struct key ROW1[] = {
    {0,57,"Tab"},{16,38,0},{17,38,0},{18,38,0},{19,38,0},{20,38,0},{21,38,0},
    {22,38,0},{23,38,0},{24,38,0},{25,38,0},{26,38,0},{27,38,0},{0,57,"Enter"} };
static const struct key ROW2[] = {
    {0,66,"Caps"},{30,38,0},{31,38,0},{32,38,0},{33,38,0},{34,38,0},{35,38,0},
    {36,38,0},{37,38,0},{38,38,0},{39,38,0},{40,38,0},{43,38,0},{0,48,""} };
static const struct key ROW3[] = {
    {0,48,"Shift"},{86,38,0},{44,38,0},{45,38,0},{46,38,0},{47,38,0},{48,38,0},
    {49,38,0},{50,38,0},{51,38,0},{52,38,0},{53,38,0},{0,108,"Shift"} };

static const struct { const struct key *keys; int n; } ROWS[] = {
    { ROW0, (int)(sizeof ROW0 / sizeof ROW0[0]) },
    { ROW1, (int)(sizeof ROW1 / sizeof ROW1[0]) },
    { ROW2, (int)(sizeof ROW2 / sizeof ROW2[0]) },
    { ROW3, (int)(sizeof ROW3 / sizeof ROW3[0]) },
};
#define NROWS ((int)(sizeof ROWS / sizeof ROWS[0]))
#define BOARD_38THS 622

// A cap is two text lines and a margin tall; the gap is a ninth of it.
static int natural_cap(void) { return ugfx_char_h() * 2 + 6; }
static int gap_of(int cap)   { int g = cap / 9; return g < 2 ? 2 : g; }

// The cap the board is drawn at: its natural size, or smaller to fit.
static int cap_for(const struct uui_keymap *k) {
    int cap = natural_cap();
    if (k->w > 0 && k->w * 38 / BOARD_38THS < cap) cap = k->w * 38 / BOARD_38THS;
    return cap < ugfx_char_h() + 4 ? ugfx_char_h() + 4 : cap;
}

void uui_keymap_init(struct uui_keymap *k, const struct ukeymap *map) {
    memset(k, 0, sizeof *k);
    k->map = map;
    k->bg = UUI_COLOR_UNSET;
}

void uui_keymap_natural_size(const struct uui_keymap *k, int *out_w, int *out_h) {
    (void)k;
    int cap = natural_cap();
    if (out_w) *out_w = BOARD_38THS * cap / 38;
    if (out_h) *out_h = NROWS * cap + (NROWS - 1) * gap_of(cap);
}

void uui_keymap_set_geometry(struct uui_keymap *k, int x, int y, int w, int h) {
    k->x = x; k->y = y; k->w = w; k->h = h;
}

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

static void draw_key(struct ugfx_surface *s, const struct uui_keymap *k, const struct key *key,
                     int x, int y, int w, int cap, uint32_t bg) {
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

void uui_keymap_draw(struct ugfx_surface *s, const struct uui_keymap *k) {
    uint32_t bg = UUI_COLOR(k->bg, UTHEME_PANEL_BG);
    int cap = cap_for(k), gap = gap_of(cap);
    int y = k->y;
    for (int r = 0; r < NROWS; r++) {
        int x = k->x;
        for (int i = 0; i < ROWS[r].n; i++) {
            const struct key *key = &ROWS[r].keys[i];
            int w = key->w * cap / 38;
            // The last key takes up the rounding, so every row ends level.
            if (i == ROWS[r].n - 1) w = k->x + BOARD_38THS * cap / 38 - x;
            draw_key(s, k, key, x, y, w, cap, bg);
            x += w + gap;
        }
        y += cap + gap;
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
