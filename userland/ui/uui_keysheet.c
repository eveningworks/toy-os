// keysheet -- see ui/uui_keysheet.h.
#include "ui/uui_keysheet.h"
#include "ui/utheme.h"
#include "ui/uui_primitives.h"
#include "geom.h"   // enum geom_aa -- the arrows are antialiased lines
#include <string.h>

// --- measures, all from the font ---------------------------------------

static int ch(void)      { return ugfx_char_h(); }
static int cap_h(void)   { return ch() + 4; }
static int cap_pad(void) { return ch() / 3; }
static int gap(void)     { int g = ch() / 5; return g < 2 ? 2 : g; }
static int row_h(void)   { return cap_h() + 6; }
static int head_h(void)  { return ch() + ch() / 2; }
static int col_gap(void) { return ch() * 2; }

enum arrow { A_NONE, A_UP, A_DOWN, A_LEFT, A_RIGHT };

static enum arrow arrow_of(const char *word, int n) {
    if (n == 2 && !strncmp(word, "Up", 2)) return A_UP;
    if (n == 4 && !strncmp(word, "Down", 4)) return A_DOWN;
    if (n == 4 && !strncmp(word, "Left", 4)) return A_LEFT;
    if (n == 5 && !strncmp(word, "Right", 5)) return A_RIGHT;
    return A_NONE;
}

static void draw_arrow(struct ugfx_surface *s, int cx, int cy, enum arrow a, uint32_t c) {
    int r = ch() / 3;
    int dx = a == A_LEFT ? -1 : a == A_RIGHT ? 1 : 0;
    int dy = a == A_UP ? -1 : a == A_DOWN ? 1 : 0;
    int tx = cx + dx * r, ty = cy + dy * r;   // the tip
    int bx = cx - dx * r, by = cy - dy * r;   // the tail
    // The two barbs, back from the tip and out to either side.
    int h = r * 2 / 3;
    int px = -dy, py = dx;                    // perpendicular
    for (int k = 0; k < 2; k++) {             // two strokes: a 1px arrow is lost in a cap
        int ox = px * k, oy = py * k;
        ugfx_draw_line(s, bx + ox, by + oy, tx + ox, ty + oy, c, GEOM_AA);
        ugfx_draw_line(s, tx + ox, ty + oy, tx - dx * h + px * h + ox, ty - dy * h + py * h + oy, c, GEOM_AA);
        ugfx_draw_line(s, tx + ox, ty + oy, tx - dx * h - px * h + ox, ty - dy * h - py * h + oy, c, GEOM_AA);
    }
}

// ONE WALK FOR MEASURING AND DRAWING, so a row's keys cannot be laid out
// at one width and drawn at another. `s` NULL measures. Returns the width.
static int walk(struct ugfx_surface *s, const char *keys, int x, int y, uint32_t bg) {
    int x0 = x, first = 1;
    const char *p = keys ? keys : "";
    uint32_t dim = ugfx_blend(UTHEME_TEXT, bg, 150);
    while (*p) {
        if (*p == ' ') { p++; continue; }
        if (!first) x += gap();
        first = 0;
        if (*p == '[') {
            const char *e = strchr(p + 1, ']');
            int n = e ? (int)(e - p - 1) : (int)strlen(p + 1);
            char word[24];
            if (n > (int)sizeof word - 1) n = (int)sizeof word - 1;
            memcpy(word, p + 1, (size_t)n);
            word[n] = 0;
            enum arrow a = arrow_of(word, n);
            int inner = a ? ch() * 2 / 3 : ugfx_text_width(word);
            int w = inner + 2 * cap_pad();
            if (w < cap_h()) w = cap_h();
            if (s) {
                int h = cap_h(), r = ch() / 4;
                // An outline with a heavier bottom edge: a cap, not a box.
                uui_fill_round_rect(s, x, y, w, h, r, UTHEME_OUTLINE);
                uui_fill_round_rect(s, x + 1, y + 1, w - 2, h - 3, r - 1, UTHEME_WHITE);
                if (a) draw_arrow(s, x + w / 2, y + (h - 2) / 2, a, UTHEME_TEXT);
                else ugfx_draw_string_clipped(s, x + (w - inner) / 2, y + (h - 2 - ch()) / 2 + 1,
                                              inner + 1, word, UTHEME_TEXT, UTHEME_WHITE);
            }
            x += w;
            p = e ? e + 1 : p + 1 + n;
        } else {
            // Plain text up to the next cap, trailing spaces trimmed.
            const char *e = strchr(p, '[');
            int n = e ? (int)(e - p) : (int)strlen(p);
            while (n > 0 && p[n - 1] == ' ') n--;
            char word[32];
            if (n > (int)sizeof word - 1) n = (int)sizeof word - 1;
            memcpy(word, p, (size_t)n);
            word[n] = 0;
            int w = ugfx_text_width(word);
            if (s) ugfx_draw_string_clipped(s, x, y + (cap_h() - 2 - ch()) / 2 + 1, w + 1, word, dim, bg);
            x += w;
            p = e ? e : p + strlen(p);
        }
    }
    return x - x0;
}

int uui_keysheet_keys_width(const char *keys) { return walk(0, keys, 0, 0, 0); }

// The width one column needs: the widest action beside the widest keys
// of the groups in it -- and a title, if that is wider.
static int column_w(const struct uui_keysheet *k) {
    int act = 0, keys = 0, title = 0;
    for (int g = 0; g < k->group_count; g++) {
        const struct uui_keysheet_group *gr = &k->groups[g];
        int tw = gr->title ? ugfx_text_width(gr->title) : 0;
        if (tw > title) title = tw;
        for (int r = 0; r < gr->count; r++) {
            int aw = ugfx_text_width(gr->rows[r].action);
            int kw = uui_keysheet_keys_width(gr->rows[r].keys);
            if (aw > act) act = aw;
            if (kw > keys) keys = kw;
        }
    }
    int w = act + ch() + keys;
    return w > title ? w : title;
}

static int group_h(const struct uui_keysheet_group *g) {
    return (g->title ? head_h() : 0) + g->count * row_h();
}

static int cols(const struct uui_keysheet *k) { return k->columns < 1 ? 1 : k->columns; }

void uui_keysheet_init(struct uui_keysheet *k, const struct uui_keysheet_group *groups,
                       int group_count, int columns) {
    k->x = k->y = k->w = k->h = 0;
    k->groups = groups;
    k->group_count = group_count;
    k->columns = columns;
    k->bg = UUI_COLOR_UNSET;
}

void uui_keysheet_natural_size(const struct uui_keysheet *k, int *out_w, int *out_h) {
    int c = cols(k), h = 0;
    for (int g = 0; g < k->group_count; g += c) {
        int tall = 0;
        for (int i = g; i < g + c && i < k->group_count; i++)
            if (group_h(&k->groups[i]) > tall) tall = group_h(&k->groups[i]);
        h += tall + (g ? ch() : 0);
    }
    if (out_w) *out_w = c * column_w(k) + (c - 1) * col_gap();
    if (out_h) *out_h = h;
}

void uui_keysheet_set_geometry(struct uui_keysheet *k, int x, int y, int w, int h) {
    k->x = x; k->y = y; k->w = w; k->h = h;
}

void uui_keysheet_draw(struct ugfx_surface *s, const struct uui_keysheet *k) {
    uint32_t bg = UUI_COLOR(k->bg, UTHEME_PANEL_BG);
    int c = cols(k);
    int cw = (k->w - (c - 1) * col_gap()) / c;
    int y = k->y;
    const struct ugfx_font *was = ugfx_font_current();
    for (int g = 0; g < k->group_count; g += c) {
        int tall = 0;
        for (int i = g; i < g + c && i < k->group_count; i++) {
            const struct uui_keysheet_group *gr = &k->groups[i];
            int x = k->x + (i - g) * (cw + col_gap()), gy = y;
            if (gr->title) {
                ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
                ugfx_draw_string_elided(s, x, gy, cw, gr->title, utheme_action(UTHEME_ACT_NAV), bg);
                ugfx_set_font(was);
                gy += head_h();
            }
            for (int r = 0; r < gr->count; r++, gy += row_h()) {
                int kw = uui_keysheet_keys_width(gr->rows[r].keys);
                int cy = gy + (row_h() - cap_h()) / 2;
                walk(s, gr->rows[r].keys, x + cw - kw, cy, bg);
                // The action gives way to the keys, never the reverse:
                // keys elided are keys you cannot press.
                ugfx_draw_string_elided(s, x, cy + (cap_h() - 2 - ch()) / 2 + 1, cw - kw - ch() / 2,
                                        gr->rows[r].action, UTHEME_TEXT, bg);
            }
            if (group_h(gr) > tall) tall = group_h(gr);
        }
        y += tall + ch();
    }
}

// --- the ops table ----------------------------------------------------

static void ks_natural(const void *w, int *ow, int *oh) {
    uui_keysheet_natural_size((const struct uui_keysheet *)w, ow, oh);
}
static void ks_geometry(void *w, int x, int y, int width, int height) {
    uui_keysheet_set_geometry((struct uui_keysheet *)w, x, y, width, height);
}
static void ks_bounds(const void *w, int *x, int *y, int *width, int *height) {
    const struct uui_keysheet *k = (const struct uui_keysheet *)w;
    if (x) *x = k->x;
    if (y) *y = k->y;
    if (width) *width = k->w;
    if (height) *height = k->h;
}
static void ks_draw(struct ugfx_surface *s, const void *w) {
    uui_keysheet_draw(s, (const struct uui_keysheet *)w);
}

const struct uui_widget_ops uui_keysheet_ops = {
    .natural_size = ks_natural,
    .set_geometry = ks_geometry,
    .bounds = ks_bounds,
    .draw = ks_draw,
};
