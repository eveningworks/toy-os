// See ui/uui_gallery.h.
#include "ui/uui_gallery.h"
#include "ui/uui_primitives.h"
#include "ui/utheme.h"
#include "keyboard.h"

// Font-derived, like everything drawn here: at 14 px a card is at least
// 169 px wide with a 56 px tile, so a Settings page holds three across.
static int gap(void)      { return ugfx_char_h() * 3 / 4; }
static int pad(void)      { return ugfx_char_h() / 3 + 1; }
static int min_card(void) { return ugfx_char_h() * 13; }
static int tile_h(void)   { return ugfx_char_h() * 4 + 4; }
static int label_h(void)  { return ugfx_char_h() + 8; }
static int card_h(void)   { return 2 * pad() + tile_h() + label_h(); }
static int radius(void)   { return ugfx_char_h() * 5 / 8; }

void uui_gallery_init(struct uui_gallery *g, const char *const *labels, int count,
                      int selected) {
    *g = (struct uui_gallery){ .labels = labels, .count = count, .selected = selected,
                               .hovered = -1, .armed_prev = -1 };
}

static int card_min(const struct uui_gallery *g) { return g->min_w > 0 ? g->min_w : min_card(); }

int uui_gallery_cols(const struct uui_gallery *g) {
    int c = g->w > 0 ? (g->w + gap()) / (card_min(g) + gap()) : 3;
    if (c < 1) c = 1;
    if (g->count > 0 && c > g->count) c = g->count;
    return c;
}

static int rows(const struct uui_gallery *g) {
    int c = uui_gallery_cols(g);
    return g->count > 0 ? (g->count + c - 1) / c : 0;
}

int uui_gallery_card_rect(const struct uui_gallery *g, int i, int *x, int *y, int *w, int *h) {
    if (i < 0 || i >= g->count) return 0;
    int c = uui_gallery_cols(g);
    int cw = g->w > 0 ? (g->w - (c - 1) * gap()) / c : card_min(g);
    *x = g->x + (i % c) * (cw + gap());
    *y = g->y + (i / c) * (card_h() + gap());
    *w = cw;
    *h = card_h();
    return 1;
}

int uui_gallery_tile_rect(const struct uui_gallery *g, int i, int *x, int *y, int *w, int *h) {
    int cx, cy, cw, ch;
    if (!uui_gallery_card_rect(g, i, &cx, &cy, &cw, &ch)) return 0;
    *x = cx + pad(); *y = cy + pad(); *w = cw - 2 * pad(); *h = tile_h();
    return 1;
}

int uui_gallery_hit(const struct uui_gallery *g, int cx, int cy) {
    for (int i = 0; i < g->count; i++) {
        int x, y, w, h;
        uui_gallery_card_rect(g, i, &x, &y, &w, &h);
        if (uui_hit(x, y, w, h, cx, cy)) return i;
    }
    return -1;
}

int uui_gallery_key(struct uui_gallery *g, int key) {
    if (g->count <= 0) return 0;
    int c = uui_gallery_cols(g), next = g->selected < 0 ? 0 : g->selected;
    switch (key) {
    case KEY_ARROW_LEFT:  next -= 1; break;
    case KEY_ARROW_RIGHT: next += 1; break;
    case KEY_ARROW_UP:    next -= c; break;
    case KEY_ARROW_DOWN:  next += c; break;
    case KEY_HOME:        next = 0; break;
    case KEY_END:         next = g->count - 1; break;
    default: return 0;
    }
    // At an end the key moves nothing and is not consumed -- the radio
    // list's rule, so Up from the first row can leave the control.
    if (next < 0 || next >= g->count || next == g->selected) return 0;
    g->selected = next;
    return 1;
}

static void draw(struct ugfx_surface *s, const void *w) {
    const struct uui_gallery *g = w;
    uint32_t bg = UTHEME_WHITE;
    for (int i = 0; i < g->count; i++) {
        int x, y, cw, ch;
        uui_gallery_card_rect(g, i, &x, &y, &cw, &ch);
        int sel = i == g->selected, hot = i == g->hovered && !g->disabled;
        uint32_t fill = bg;
        if (sel) {
            // The soft accent fill alone: a card is big enough to read as
            // chosen without an edge, and the edge beside the focus ring
            // read as two outlines on one card.
            fill = UTHEME_SELECTION;
            uui_fill_round_rect(s, x, y, cw, ch, radius(), fill);
        } else if (hot) {
            fill = uui_state_bg(bg, UUI_STATE_HOVER);
            uui_fill_round_rect(s, x, y, cw, ch, radius(), fill);
        }
        int tx, ty, tw, th;
        uui_gallery_tile_rect(g, i, &tx, &ty, &tw, &th);
        if (g->draw_tile) {
            struct ugfx_clip was_clip;
            ugfx_clip_save(s, &was_clip);
            ugfx_clip_intersect(s, tx, ty, tw, th);   // a painter cannot spill past its tile
            g->draw_tile(s, i, tx, ty, tw, th, g->ctx);
            ugfx_clip_restore(s, &was_clip);
        }
        const char *label = g->labels ? g->labels[i] : "";
        const struct ugfx_font *was = sel ? ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD)) : 0;
        int lw = ugfx_text_width(label);
        int avail = cw - 2 * pad();
        int lx = x + pad() + (lw < avail ? (avail - lw) / 2 : 0);
        int ly = ty + th + (label_h() - ugfx_char_h()) / 2;
        uint32_t fg = g->disabled ? ugfx_blend(UTHEME_TEXT, fill, 140) : UTHEME_TEXT;
        ugfx_draw_string_elided(s, lx, ly, avail, label, fg, fill);
        if (sel) ugfx_set_font(was);
        if (sel && g->focused && g->ring) uui_focus_ring(s, x - 2, y - 2, cw + 4, ch + 4);
    }
}

static void natural_size(const void *w, int *ow, int *oh) {
    const struct uui_gallery *g = w;
    int c = uui_gallery_cols(g), r = rows(g);
    *ow = c * card_min(g) + (c - 1) * gap();
    *oh = r > 0 ? r * card_h() + (r - 1) * gap() : 0;
}

static void set_geometry(void *w, int x, int y, int width, int height) {
    struct uui_gallery *g = w;
    g->x = x; g->y = y; g->w = width; g->h = height;
}

static void bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_gallery *g = w;
    *x = g->x; *y = g->y; *ow = g->w; *oh = g->h;
}

static int hit(const void *w, int cx, int cy) {
    return uui_gallery_hit((const struct uui_gallery *)w, cx, cy) >= 0;
}

// Arm on press (the selection moves so the user sees it), commit on the
// release over a card, put it back if the press was dragged off.
static int press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_gallery *g = w;
    if (g->disabled) return 0;
    int i = uui_gallery_hit(g, cx, cy);
    if (i < 0) return 0;
    g->armed_prev = g->selected;
    g->selected = i;
    g->ring = 0;
    return 1;   // on ANY card, the current one too: the grab is what brings the release
}

static int release(void *w, int cx, int cy) {
    struct uui_gallery *g = w;
    if (g->disabled) return 0;
    if (uui_gallery_hit(g, cx, cy) < 0 && g->armed_prev >= 0) g->selected = g->armed_prev;
    g->armed_prev = -1;
    return 1;
}

static int motion(void *w, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_gallery *g = w;
    int i = g->disabled ? -1 : uui_gallery_hit(g, cx, cy);
    if (i == g->hovered) return 0;
    g->hovered = i;
    return 1;
}

static int key(void *w, int k, unsigned mods) {
    (void)mods;
    struct uui_gallery *g = w;
    if (g->disabled) return 0;
    g->ring = 1;
    return uui_gallery_key(g, k);
}

// Focus that arrives while a press is armed is a CLICK's; any other
// (Tab) shows the ring at once.
static void set_focused(void *w, int f) {
    struct uui_gallery *g = w;
    g->focused = f;
    if (f && g->armed_prev < 0) g->ring = 1;
}

static int accepts_focus(const void *w) {
    const struct uui_gallery *g = w;
    return !g->disabled && g->count > 0;
}

static void describe(const void *w, const struct uui_describe *d) {
    const struct uui_gallery *g = w;
    for (int i = 0; i < g->count; i++) {
        int x, y, cw, ch;
        if (uui_gallery_card_rect(g, i, &x, &y, &cw, &ch)) uui_describe_rect_i(d, "card", i, x, y, cw, ch);
        if (uui_gallery_tile_rect(g, i, &x, &y, &cw, &ch)) uui_describe_rect_i(d, "tile", i, x, y, cw, ch);
    }
    uui_describe_int(d, "selected", g->selected);
}

const struct uui_widget_ops uui_gallery_ops = {
    .natural_size  = natural_size,
    .set_geometry  = set_geometry,
    .bounds        = bounds,
    .draw          = draw,
    .hit           = hit,
    .press         = press,
    .release       = release,
    .motion        = motion,
    .key           = key,
    .set_focused   = set_focused,
    .accepts_focus = accepts_focus,
    .describe      = describe,
};
