// A playlist (ui/uui_medialist.h).
#include "ui/uui_medialist.h"
#include "ui/uui_primitives.h"
#include "ui/uui_scrollbar.h"
#include "ui/utheme.h"
#include "lib/uimg.h"
#include "keyboard.h"

#include <string.h>

void uui_medialist_init(struct uui_medialist *m) {
    memset(m, 0, sizeof *m);
    m->selected = m->current = m->hot = m->armed = m->committed = -1;
    m->bg = UTHEME_PANEL_BG;
}

// Two lines and a margin: the title, the detail under it.
int uui_medialist_row_h(void) {
    return ugfx_char_h() * 2 + ugfx_char_w() * 2;
}

static int content_h(const struct uui_medialist *m) { return m->count * uui_medialist_row_h(); }

static void clamp_scroll(struct uui_medialist *m) {
    int max = content_h(m) - m->h;
    if (m->scroll > max) m->scroll = max;
    if (m->scroll < 0) m->scroll = 0;
}

void uui_medialist_set(struct uui_medialist *m, int count, uui_medialist_text_fn text,
                       uui_medialist_thumb_fn thumb, void *ctx) {
    m->count = count;
    m->text = text;
    m->thumb = thumb;
    m->ctx = ctx;
    if (m->selected >= count) m->selected = count - 1;
    if (m->current >= count) m->current = -1;
    m->hot = m->armed = -1;
    clamp_scroll(m);
}

void uui_medialist_select(struct uui_medialist *m, int row) {
    if (row < -1 || row >= m->count) return;
    m->selected = row;
    if (row < 0) return;
    int rh = uui_medialist_row_h(), top = row * rh;
    if (top < m->scroll) m->scroll = top;
    else if (top + rh > m->scroll + m->h) m->scroll = top + rh - m->h;
    clamp_scroll(m);
}

int uui_medialist_take(struct uui_medialist *m) {
    int r = m->committed;
    m->committed = -1;
    return r;
}

static int bar_w(void) { return uui_scrollbar_overlay_width(); }

static int row_at(const struct uui_medialist *m, int cx, int cy) {
    if (!uui_hit(m->x, m->y, m->w, m->h, cx, cy)) return -1;
    int r = (cy - m->y + m->scroll) / uui_medialist_row_h();
    return r >= 0 && r < m->count ? r : -1;
}

static int on_bar(const struct uui_medialist *m, int cx, int cy) {
    return content_h(m) > m->h && uui_hit(m->x + m->w - bar_w(), m->y, bar_w(), m->h, cx, cy);
}

// --- ops -----------------------------------------------------------------

static void ml_natural(const void *w, int *ow, int *oh) {
    (void)w;
    *ow = ugfx_char_w() * 30;
    *oh = uui_medialist_row_h() * 3;
}

static void ml_geometry(void *w, int x, int y, int width, int height) {
    struct uui_medialist *m = w;
    m->x = x; m->y = y; m->w = width; m->h = height;
    clamp_scroll(m);
}

static void ml_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_medialist *m = w;
    *x = m->x; *y = m->y; *ow = m->w; *oh = m->h;
}

static void draw_row(struct ugfx_surface *s, const struct uui_medialist *m, int r, int ry) {
    int rh = uui_medialist_row_h(), pad = ugfx_char_w(), lh = ugfx_char_h();
    int rx = m->x + pad / 2, rw = m->w - pad - bar_w() / 2;
    int sel = r == m->selected, cur = r == m->current;
    uint32_t ground = m->bg;
    if (sel || cur) {
        // The design language's selection: a soft fill, a 1px edge --
        // the full accent where the keyboard is.
        uint32_t edge = sel && m->focused ? UTHEME_ACCENT : ugfx_blend(UTHEME_WHITE, UTHEME_ACCENT, 130);
        uui_fill_round_rect(s, rx, ry + 1, rw, rh - 2, 6, edge);
        uui_fill_round_rect(s, rx + 1, ry + 2, rw - 2, rh - 4, 5, UTHEME_SELECTION);
        ground = UTHEME_SELECTION;
    } else if (r == m->hot) {
        ground = uui_state_bg(m->bg, UUI_STATE_HOVER);
        uui_fill_round_rect(s, rx, ry + 1, rw, rh - 2, 6, ground);
    }

    int th = rh - pad * 2 + 2, tw = th * 16 / 9, tx = rx + pad / 2 + 2, ty = ry + (rh - th) / 2;
    const struct uimg *im = m->thumb ? m->thumb(m->ctx, r, tw) : 0;
    ugfx_fill_rect(s, tx, ty, tw, th, ugfx_rgb(24, 24, 30));
    if (im && im->px) {
        // Centred in the 16:9 box, cut to it.
        int iw = im->w < tw ? im->w : tw, ih = im->h < th ? im->h : th;
        int ox = (im->w - iw) / 2, oy = (im->h - ih) / 2;
        ugfx_blit(s, tx + (tw - iw) / 2, ty + (th - ih) / 2, iw, ih,
                  im->px + (size_t)oy * (size_t)im->w + (size_t)ox, im->w);
    }
    if (cur) {
        // What is playing: a play mark on its picture.
        int r3 = th / 5, cx = tx + tw / 2, cy = ty + th / 2;
        int xs[3] = { cx - r3 / 2, cx - r3 / 2, cx + r3 };
        int ys[3] = { cy - r3, cy + r3, cy };
        ugfx_fill_polygon(s, xs, ys, 3, ugfx_rgb(255, 255, 255));
    }

    char title[128] = "", detail[128] = "", right[32] = "";
    if (m->text) m->text(m->ctx, r, title, sizeof title, detail, sizeof detail, right, sizeof right);
    int x = tx + tw + pad, rwid = ugfx_text_width(right);
    int right_x = rx + rw - pad - rwid;
    int y1 = ry + rh / 2 - lh, y2 = ry + rh / 2;
    const struct ugfx_font *was = cur ? ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD)) : 0;
    ugfx_draw_string_clipped(s, x, y1, right_x - pad - x, title, UTHEME_TEXT, ground);
    if (was) ugfx_set_font(was);
    ugfx_draw_string_clipped(s, x, y2, rx + rw - pad - x, detail,
                             uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED), ground);
    ugfx_draw_string_clipped(s, right_x, y1, rwid, right, UTHEME_TEXT, ground);
}

static void ml_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_medialist *m = w;
    ugfx_fill_rect(s, m->x, m->y, m->w, m->h, m->bg);
    int rh = uui_medialist_row_h();
    int first = m->scroll / rh;
    struct ugfx_clip saved;
    ugfx_clip_save(s, &saved);
    ugfx_clip_intersect(s, m->x, m->y, m->w, m->h);
    for (int r = first; r < m->count; r++) {
        int ry = m->y + r * rh - m->scroll;
        if (ry >= m->y + m->h) break;
        draw_row(s, m, r, ry);
    }
    ugfx_clip_restore(s, &saved);
    uui_scrollbar_draw_overlay(s, m->x + m->w - bar_w(), m->y, bar_w(), m->h, content_h(m), m->h,
                               m->scroll, m->bg, UTHEME_TEXT, (m->bar_hot || m->bar_drag) ? 255 : 0,
                               m->bar_drag ? UUI_SCROLLBAR_HELD : 0);
}

// The whole rect: the overlay bar is inside it and must stay live.
static int ml_hit(const void *w, int cx, int cy) {
    const struct uui_medialist *m = w;
    return uui_hit(m->x, m->y, m->w, m->h, cx, cy);
}

static int ml_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_medialist *m = w;
    if (on_bar(m, cx, cy)) {
        int zone = uui_scrollbar_hit(m->x + m->w - bar_w(), m->y, bar_w(), m->h, content_h(m), m->h,
                                     m->scroll, cx, cy, 0);
        if (zone == UUI_SB_THUMB) {
            int ty, tth;
            uui_scrollbar_thumb_rect(m->y, m->h, content_h(m), m->h, m->scroll, &ty, &tth, bar_w(), 0);
            m->bar_drag = 1;
            m->bar_grab = cy - ty;
        } else if (zone == UUI_SB_ABOVE) {
            m->scroll -= m->h;
        } else if (zone == UUI_SB_BELOW) {
            m->scroll += m->h;
        }
        clamp_scroll(m);
        return 1;
    }
    m->armed = row_at(m, cx, cy);
    if (m->armed >= 0) m->selected = m->armed;
    return uui_hit(m->x, m->y, m->w, m->h, cx, cy);
}

static int ml_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_medialist *m = w;
    if (m->bar_drag) {
        m->scroll = uui_scrollbar_offset_for_drag(m->y, m->h, content_h(m), m->h, cy, m->bar_grab, bar_w(), 0);
        clamp_scroll(m);
        return 1;
    }
    int hot = buttons ? m->hot : row_at(m, cx, cy);
    int bh = on_bar(m, cx, cy);
    if (bh) hot = -1;
    int changed = hot != m->hot || bh != m->bar_hot;
    m->hot = hot;
    m->bar_hot = bh;
    return changed;
}

static int ml_release(void *w, int cx, int cy) {
    struct uui_medialist *m = w;
    if (m->bar_drag) { m->bar_drag = 0; return 1; }
    int r = row_at(m, cx, cy);
    if (m->armed >= 0 && r == m->armed) m->committed = r;
    m->armed = -1;
    return 1;
}

static int ml_wheel(void *w, int notches) {
    struct uui_medialist *m = w;
    int before = m->scroll;
    m->scroll -= notches * uui_medialist_row_h();
    clamp_scroll(m);
    return m->scroll != before;
}

static int ml_key(void *w, int key, unsigned mods) {
    (void)mods;
    struct uui_medialist *m = w;
    if (!m->count) return 0;
    int s = m->selected, page = m->h / uui_medialist_row_h();
    if (page < 1) page = 1;
    switch (key) {
    case KEY_ARROW_UP:   s = s > 0 ? s - 1 : 0; break;
    case KEY_ARROW_DOWN: s = s < m->count - 1 ? s + 1 : m->count - 1; break;
    case KEY_PAGE_UP:    s = s - page < 0 ? 0 : s - page; break;
    case KEY_PAGE_DOWN:  s = s + page >= m->count ? m->count - 1 : s + page; break;
    case KEY_HOME:       s = 0; break;
    case KEY_END:        s = m->count - 1; break;
    case '\n': case '\r':
        if (m->selected >= 0) m->committed = m->selected;
        return m->selected >= 0;
    default: return 0;
    }
    uui_medialist_select(m, s);
    return 1;
}

static int ml_accepts(const void *w) { return ((const struct uui_medialist *)w)->count > 0; }
static void ml_focused(void *w, int f) { ((struct uui_medialist *)w)->focused = f; }

static void ml_describe(const void *w, const struct uui_describe *d) {
    const struct uui_medialist *m = w;
    int rh = uui_medialist_row_h();
    for (int r = m->scroll / rh; r < m->count; r++) {
        int ry = m->y + r * rh - m->scroll;
        if (ry >= m->y + m->h) break;
        uui_describe_rect_i(d, "row", r, m->x, ry, m->w, rh);
    }
    uui_describe_int(d, "current", m->current);
    uui_describe_int(d, "selected", m->selected);
}

const struct uui_widget_ops uui_medialist_ops = {
    .natural_size = ml_natural,
    .set_geometry = ml_geometry,
    .bounds = ml_bounds,
    .draw = ml_draw,
    .hit = ml_hit,
    .key = ml_key,
    .set_focused = ml_focused,
    .accepts_focus = ml_accepts,
    .press = ml_press,
    .motion = ml_motion,
    .release = ml_release,
    .wheel = ml_wheel,
    .describe = ml_describe,
};
