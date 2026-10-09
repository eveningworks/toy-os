// Segments of a whole, side by side (ui/uui_segbar.h).
#include "ui/uui_segbar.h"
#include "ui/uui_primitives.h"
#include "ui/utheme.h"
#include "keyboard.h"

#include <string.h>

#define GAP 3

void uui_segbar_init(struct uui_segbar *b) {
    memset(b, 0, sizeof *b);
    b->selected = b->hot = b->armed = -1;
}

static int floor_w(const struct uui_segbar *b) { return b->min_w > 0 ? b->min_w : ugfx_char_w() * 10; }

// Every segment its floor, then the rest by size; equal shares when the
// floors alone overflow. The last segment takes the rounding.
static void place(struct uui_segbar *b) {
    int n = b->count, room = b->w - GAP * (n - 1);
    if (n <= 0) return;
    int fl = floor_w(b);
    uint64_t total = 0;
    for (int i = 0; i < n; i++) total += b->segs[i].size;
    int spare = room - fl * n, x = b->x;
    for (int i = 0; i < n; i++) {
        int w;
        if (spare < 0 || !total) w = room / n;
        else w = fl + (int)((uint64_t)spare * b->segs[i].size / total);
        if (i == n - 1) w = b->x + b->w - x;
        b->sx[i] = x;
        b->sw[i] = w > 1 ? w : 1;
        x += w + GAP;
    }
}

void uui_segbar_set(struct uui_segbar *b, const struct uui_segbar_seg *segs, int count) {
    b->segs = segs;
    b->count = count > UUI_SEGBAR_MAX ? UUI_SEGBAR_MAX : count < 0 ? 0 : count;
    if (b->selected >= b->count) b->selected = b->count - 1;
    b->hot = b->armed = -1;
    place(b);
}

int uui_segbar_rect(const struct uui_segbar *b, int i, int *x, int *y, int *w, int *h) {
    if (i < 0 || i >= b->count) return 0;
    *x = b->sx[i];
    *y = b->y;
    *w = b->sw[i];
    *h = b->h;
    return 1;
}

static int seg_at(const struct uui_segbar *b, int cx, int cy) {
    if (cy < b->y || cy >= b->y + b->h) return -1;
    for (int i = 0; i < b->count; i++)
        if (cx >= b->sx[i] && cx < b->sx[i] + b->sw[i]) return i;
    return -1;
}

// --- ops ------------------------------------------------------------------

static void sb_natural(const void *w, int *ow, int *oh) {
    const struct uui_segbar *b = w;
    *ow = floor_w(b) * (b->count > 0 ? b->count : 1);
    *oh = ugfx_char_h() * 3 + ugfx_char_w();
}

static void sb_geometry(void *w, int x, int y, int width, int height) {
    struct uui_segbar *b = w;
    b->x = x; b->y = y; b->w = width; b->h = height;
    place(b);
}

static void sb_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_segbar *b = w;
    *x = b->x; *y = b->y; *ow = b->w; *oh = b->h;
}

static void dashed_box(struct ugfx_surface *s, int x, int y, int w, int h, uint32_t c) {
    for (int i = 0; i < w; i += 6) {
        int l = w - i < 3 ? w - i : 3;
        ugfx_fill_rect(s, x + i, y, l, 1, c);
        ugfx_fill_rect(s, x + i, y + h - 1, l, 1, c);
    }
    for (int i = 0; i < h; i += 6) {
        int l = h - i < 3 ? h - i : 3;
        ugfx_fill_rect(s, x, y + i, 1, l, c);
        ugfx_fill_rect(s, x + w - 1, y + i, 1, l, c);
    }
}

static void sb_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_segbar *b = w;
    int lh = ugfx_char_h(), pad = ugfx_char_w() / 2 + 2;
    for (int i = 0; i < b->count; i++) {
        const struct uui_segbar_seg *g = &b->segs[i];
        int x = b->sx[i], y = b->y, sw = b->sw[i], h = b->h;
        int sel = i == b->selected;
        uint32_t ground = g->empty ? UTHEME_WHITE : g->fill;
        if (i == b->hot && !sel) ground = uui_state_bg(ground, UUI_STATE_HOVER);
        // SELECTED is a 2px accent border in place of the card's own
        // edge, so its fill -- which says what it holds -- still shows.
        int bw = sel ? 2 : 1;
        if (g->empty) {
            ugfx_fill_rect(s, x, y, sw, h, ground);
            if (sel) {
                uui_fill_round_rect(s, x, y, sw, h, 6, UTHEME_ACCENT);
                uui_fill_round_rect(s, x + bw, y + bw, sw - 2 * bw, h - 2 * bw, 4, ground);
            } else {
                dashed_box(s, x, y, sw, h, UTHEME_OUTLINE);
            }
        } else {
            uui_fill_round_rect(s, x, y, sw, h, 6, sel ? UTHEME_ACCENT : g->edge);
            uui_fill_round_rect(s, x + bw, y + bw, sw - 2 * bw, h - 2 * bw, 6 - bw, ground);
        }
        int tx = x + pad, tw = sw - pad * 2;
        if (tw < 4) continue;
        int cw = g->corner ? ugfx_text_width(g->corner) : 0;
        if (cw > tw / 2) cw = 0;   // no room: the title keeps it
        const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        ugfx_draw_string_clipped(s, tx, y + pad, tw - (cw ? cw + pad : 0), g->title ? g->title : "", UTHEME_TEXT, ground);
        ugfx_set_font(was);
        if (cw) ugfx_draw_string_clipped(s, tx + tw - cw, y + pad, cw, g->corner, UTHEME_TEXT, ground);
        if (g->used_pct >= 0 && h >= lh * 3) {
            int my = y + h / 2 - 2, used = tw * (g->used_pct > 100 ? 100 : g->used_pct) / 100;
            uui_fill_round_rect(s, tx, my, tw, 5, 2, UTHEME_WHITE);
            if (used > 0) uui_fill_round_rect(s, tx, my, used < 5 ? 5 : used, 5, 2, UTHEME_ACCENT);
        }
        if (g->detail && h >= lh * 2 + pad)
            ugfx_draw_string_clipped(s, tx, y + h - pad - lh, tw, g->detail, UTHEME_TEXT, ground);
    }
}

// The whole rect, a boolean: the gaps between cards are still the bar.
static int sb_hit(const void *w, int cx, int cy) {
    const struct uui_segbar *b = w;
    return uui_hit(b->x, b->y, b->w, b->h, cx, cy);
}

static int sb_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_segbar *b = w;
    b->armed = seg_at(b, cx, cy);
    return uui_hit(b->x, b->y, b->w, b->h, cx, cy);
}

static int sb_release(void *w, int cx, int cy) {
    struct uui_segbar *b = w;
    int i = seg_at(b, cx, cy), changed = 0;
    if (b->armed >= 0 && i == b->armed && i != b->selected) {
        b->selected = i;
        changed = 1;
    }
    b->armed = -1;
    return changed;
}

static int sb_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_segbar *b = w;
    int hot = buttons ? b->hot : seg_at(b, cx, cy);
    int changed = hot != b->hot;
    b->hot = hot;
    return changed;
}

static int sb_key(void *w, int key, unsigned mods) {
    (void)mods;
    struct uui_segbar *b = w;
    if (!b->count) return 0;
    int s = b->selected < 0 ? 0 : b->selected;
    switch (key) {
    case KEY_ARROW_LEFT:  s = s > 0 ? s - 1 : 0; break;
    case KEY_ARROW_RIGHT: s = s < b->count - 1 ? s + 1 : s; break;
    case KEY_HOME:        s = 0; break;
    case KEY_END:         s = b->count - 1; break;
    default: return 0;
    }
    b->selected = s;
    return 1;
}

static int sb_accepts(const void *w) { return ((const struct uui_segbar *)w)->count > 0; }
static void sb_focused(void *w, int f) { ((struct uui_segbar *)w)->focused = f; }

static void sb_describe(const void *w, const struct uui_describe *d) {
    const struct uui_segbar *b = w;
    uui_describe_int(d, "count", b->count);
    uui_describe_int(d, "selected", b->selected);
    for (int i = 0; i < b->count; i++) uui_describe_rect_i(d, "seg", i, b->sx[i], b->y, b->sw[i], b->h);
}

const struct uui_widget_ops uui_segbar_ops = {
    .natural_size = sb_natural,
    .set_geometry = sb_geometry,
    .bounds = sb_bounds,
    .draw = sb_draw,
    .hit = sb_hit,
    .key = sb_key,
    .set_focused = sb_focused,
    .accepts_focus = sb_accepts,
    .press = sb_press,
    .motion = sb_motion,
    .release = sb_release,
    .describe = sb_describe,
};
