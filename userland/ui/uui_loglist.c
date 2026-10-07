// See uui_loglist.h.
#include "ui/uui_loglist.h"
#include "ui/uui_primitives.h"
#include "ui/uui_scrollbar.h"
#include "ui/utheme.h"
#include "ui/ugfx.h"
#include "keyboard.h"
#include <stdio.h>
#include <string.h>

// --- metrics, all font-derived --------------------------------------------

static int line_h(void) { return ugfx_char_h(); }
static int row_h(void)  { return ugfx_char_h() + 4; }   // uui_table's pitch

static int mono_w(void) {
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_mono(UGFX_FONT_REGULAR));
    int w = ugfx_char_advance('0');
    ugfx_set_font(was);
    return w > 0 ? w : 8;
}
static int ui_w(void) { int w = ugfx_char_advance('n'); return w > 0 ? w : 8; }

int uui_loglist_timeline_h(const struct uui_loglist *l) {
    return l->timeline ? 4 * line_h() + 6 : 0;
}

struct cols { int time_x, time_w, lvl_x, lvl_w, src_x, src_w, msg_x, msg_w, bar_x, bar_w; };

static void columns(const struct uui_loglist *l, struct cols *c) {
    int pad = 6;
    c->bar_w = uui_scrollbar_overlay_width();
    c->bar_x = l->x + l->w - c->bar_w;
    c->time_x = l->x + pad;
    c->time_w = mono_w() * 8;
    c->lvl_x = c->time_x + c->time_w + 2 * pad;
    c->lvl_w = line_h() + ui_w() * 5;
    c->src_x = c->lvl_x + c->lvl_w + pad;
    c->src_w = ui_w() * 9;
    c->msg_x = c->src_x + c->src_w + pad;
    c->msg_w = c->bar_x - c->msg_x - pad;
    if (c->msg_w < 0) c->msg_w = 0;
}

static int header_y(const struct uui_loglist *l) { return l->y + uui_loglist_timeline_h(l); }
static int rows_y(const struct uui_loglist *l)   { return header_y(l) + row_h(); }
static int rows_h(const struct uui_loglist *l)   { int h = l->y + l->h - rows_y(l); return h > 0 ? h : 0; }
static int visible(const struct uui_loglist *l)  { return rows_h(l) / row_h(); }
static int nview(const struct uui_loglist *l)    { return l->set ? l->set->view_count : 0; }

static void clamp(struct uui_loglist *l) {
    int max = nview(l) - visible(l);
    if (max < 0) max = 0;
    if (l->top > max) l->top = max;
    if (l->top < 0) l->top = 0;
    if (l->selected >= nview(l)) l->selected = nview(l) - 1;
}

static void reveal(struct uui_loglist *l) {
    if (l->selected < 0) return;
    if (l->selected < l->top) l->top = l->selected;
    if (l->selected >= l->top + visible(l)) l->top = l->selected - visible(l) + 1;
    clamp(l);
}

static void to_end(struct uui_loglist *l) {
    l->top = nview(l);
    clamp(l);
}

// Stops following once the reader moves off the end, and says so.
static void unfollow_if_off_end(struct uui_loglist *l) {
    if (l->follow && l->top + visible(l) < nview(l)) {
        l->follow = 0;
        l->changes |= UUI_LOGLIST_FOLLOW;
    }
}

// --- the public calls ----------------------------------------------------

void uui_loglist_init(struct uui_loglist *l, const struct ulogset *set) {
    memset(l, 0, sizeof *l);
    l->set = set;
    l->selected = -1;
    l->hovered = -1;
    l->thumb_grab = -1;
    l->brush_anchor = -1;
    l->follow = 1;
}

void uui_loglist_refresh(struct uui_loglist *l) {
    clamp(l);
    if (l->follow) to_end(l);
}

int uui_loglist_selected_line(const struct uui_loglist *l) {
    if (!l->set || l->selected < 0 || l->selected >= l->set->view_count) return -1;
    return l->set->view[l->selected];
}

int uui_loglist_select_line(struct uui_loglist *l, int line, int show) {
    if (line < 0) { l->selected = -1; return 1; }
    for (int r = 0; r < nview(l); r++) {
        if (l->set->view[r] != line) continue;
        l->selected = r;
        if (show) { reveal(l); unfollow_if_off_end(l); }
        return 1;
    }
    l->selected = -1;
    return 0;
}

void uui_loglist_set_follow(struct uui_loglist *l, int on) {
    l->follow = on;
    if (on) to_end(l);
}

int uui_loglist_take_changes(struct uui_loglist *l) {
    int c = l->changes;
    l->changes = 0;
    return c;
}

// --- drawing -------------------------------------------------------------

// A SHAPE as well as a colour: a cross in a disc for an error, a bar in a
// triangle for a warning, so the two differ without colour vision.
static void draw_severity(struct ugfx_surface *s, int x, int y, int sz, int sev, uint32_t bg) {
    uint32_t c = utheme_severity(sev), w = UTHEME_WHITE;
    (void)bg;
    if (sev == UTHEME_SEV_ERROR) {
        int r = sz / 2, cx = x + r, cy = y + r, k = r / 2;
        ugfx_fill_circle(s, cx, cy, r, c);
        ugfx_draw_line(s, cx - k, cy - k, cx + k, cy + k, w, GEOM_ALIASED);
        ugfx_draw_line(s, cx - k, cy + k, cx + k, cy - k, w, GEOM_ALIASED);
        ugfx_draw_line(s, cx - k + 1, cy - k, cx + k + 1, cy + k, w, GEOM_ALIASED);
        ugfx_draw_line(s, cx - k + 1, cy + k, cx + k + 1, cy - k, w, GEOM_ALIASED);
    } else if (sev == UTHEME_SEV_WARNING) {
        int xs[3] = { x + sz / 2, x + sz, x }, ys[3] = { y, y + sz, y + sz };
        ugfx_fill_polygon(s, xs, ys, 3, c);
        ugfx_fill_rect(s, x + sz / 2 - 1, y + sz / 3, 2, sz / 3, w);
        ugfx_fill_rect(s, x + sz / 2 - 1, y + sz - sz / 5 - 1, 2, 2, w);
    }
}

static void fmt_time(const struct ulog_line *ln, char *out, int cap) {
    if (ln->stamped) snprintf(out, (size_t)cap, "%u.%02u", ln->cs / 100, ln->cs % 100);
    else if (ln->clock[0]) snprintf(out, (size_t)cap, "%s", ln->clock);
    else out[0] = 0;
}

static int draw_right(struct ugfx_surface *s, int x, int y, int w, const char *t, uint32_t fg, uint32_t bg) {
    int tw = ugfx_text_width(t);
    return ugfx_draw_string_clipped(s, x + (tw < w ? w - tw : 0), y, w, t, fg, bg);
}

static void draw_row(struct ugfx_surface *s, const struct uui_loglist *l, const struct cols *c,
                     int r, int ry, uint32_t bg) {
    const struct ulog_line *ln = &l->set->lines[l->set->view[r]];
    int ty = ry + (row_h() - line_h()) / 2;
    uint32_t fg = UTHEME_TEXT, muted = ugfx_blend(UTHEME_TEXT, bg, 140);
    char t[16];
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_mono(UGFX_FONT_REGULAR));
    fmt_time(ln, t, sizeof t);
    draw_right(s, c->time_x, ty, c->time_w, t, muted, bg);
    ugfx_set_font(was);

    int sev = ulog_severity(ln->level);
    if (sev) {
        int sz = line_h() * 3 / 4;
        draw_severity(s, c->lvl_x, ry + (row_h() - sz) / 2, sz, sev, bg);
        was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        ugfx_draw_string_clipped(s, c->lvl_x + sz + 4, ty, c->lvl_w - sz - 4,
                                 ulog_level_name(ln->level), utheme_severity(sev), bg);
        ugfx_set_font(was);
    } else {
        ugfx_draw_string_clipped(s, c->lvl_x, ty, c->lvl_w, ulog_level_name(ln->level), muted, bg);
    }
    ugfx_draw_string_clipped(s, c->src_x, ty, c->src_w, ln->source, muted, bg);

    // The subsystem in bold, then the rest -- the prefix is what a reader
    // scans down the column for.
    int x = c->msg_x, room = c->msg_w;
    const char *rest = ln->text;
    if (ln->subsys[0]) {
        int n = (int)strlen(ln->subsys) + 1;              // with its colon
        char sub[ULOG_SUB_MAX + 2];
        snprintf(sub, sizeof sub, "%.*s", n, ln->text);
        was = ugfx_set_font(ugfx_font_mono(UGFX_FONT_BOLD));
        int used = ugfx_text_width(sub);
        ugfx_draw_string_clipped(s, x, ty, room, sub, fg, bg);
        ugfx_set_font(was);
        x += used;
        room -= used;
        rest = ln->text + n;
    }
    if (room > 0) {
        was = ugfx_set_font(ugfx_font_mono(UGFX_FONT_REGULAR));
        ugfx_draw_string_elided(s, x, ty, room, rest, sev == UTHEME_SEV_ERROR ? utheme_severity(sev) : fg, bg);
        ugfx_set_font(was);
    }
}

static void fmt_span(unsigned cs, char *out, int cap) {
    unsigned s = cs / 100;
    if (s >= 120) snprintf(out, (size_t)cap, "%u min", s / 60);
    else snprintf(out, (size_t)cap, "%u s", s);
}

static void plot_rect(const struct uui_loglist *l, int *px, int *py, int *pw, int *ph) {
    int lh = line_h();
    *px = l->x + ui_w() * 2;
    *pw = l->w - ui_w() * 4;
    *py = l->y + lh / 2 + 2;
    *ph = 2 * lh;
}

static unsigned span_of(const struct uui_loglist *l) { return l->set->max_cs + 1; }

// THE TIMELINE: lines per bucket over the whole span, square-root scaled
// so a quiet stretch still shows against the boot's burst; errors as ticks
// under the axis; the chosen range as a band.
static void draw_timeline(struct ugfx_surface *s, const struct uui_loglist *l) {
    const struct utheme *t = utheme_current();
    int th = uui_loglist_timeline_h(l);
    ugfx_fill_rect(s, l->x, l->y, l->w, th, t->panel_bg);
    ugfx_fill_rect(s, l->x, l->y + th - 1, l->w, 1, t->separator);
    int px, py, pw, ph;
    plot_rect(l, &px, &py, &pw, &ph);
    if (pw <= 0) return;
    int bw = ui_w();
    int nb = pw / bw;
    if (nb < 1) return;
    if (nb > 512) nb = 512;
    static int count[512];
    memset(count, 0, sizeof count);
    unsigned span = span_of(l);
    int most = 1;
    for (int i = 0; i < l->set->count; i++) {
        const struct ulog_line *ln = &l->set->lines[i];
        if (!ln->stamped) continue;
        int b = (int)((unsigned long long)ln->cs * (unsigned)nb / span);
        if (b >= nb) b = nb - 1;
        if (++count[b] > most) most = count[b];
    }
    if (l->range_lo || l->range_hi) {
        int a = px + (int)((unsigned long long)l->range_lo * (unsigned)pw / span);
        int b = px + (int)((unsigned long long)l->range_hi * (unsigned)pw / span);
        if (b < a + 2) b = a + 2;
        ugfx_fill_rect(s, a, py - 2, b - a, ph + 8, ugfx_blend(t->panel_bg, t->accent, 40));
        ugfx_fill_rect(s, a, py - 2, 2, ph + 8, t->accent);
        ugfx_fill_rect(s, b - 2, py - 2, 2, ph + 8, t->accent);
    }
    uint32_t bar = ugfx_blend(t->panel_bg, t->accent, 110);
    for (int b = 0; b < nb; b++) {
        if (!count[b]) continue;
        // sqrt by integer search: small counts, no float.
        int lo = 0;
        while ((lo + 1) * (lo + 1) * most <= count[b] * 1024) lo++;   // sqrt(count/most)*32
        int h = lo * ph / 32;
        if (h < 2) h = 2;
        ugfx_fill_rect(s, px + b * bw + 1, py + ph - h, bw - 2, h, bar);
    }
    ugfx_fill_rect(s, px, py + ph, pw, 1, t->outline);
    uint32_t red = utheme_severity(UTHEME_SEV_ERROR);
    for (int i = 0; i < l->set->count; i++) {
        const struct ulog_line *ln = &l->set->lines[i];
        if (!ln->stamped || ulog_severity(ln->level) != UTHEME_SEV_ERROR) continue;
        int x = px + (int)((unsigned long long)ln->cs * (unsigned)pw / span);
        ugfx_fill_rect(s, x, py + ph + 2, 2, 5, red);
    }
    char lbl[24];
    uint32_t muted = ugfx_blend(t->text, t->panel_bg, 140);
    ugfx_draw_string_clipped(s, px, py + ph + 8, pw / 2, "0", muted, t->panel_bg);
    fmt_span(span, lbl, sizeof lbl);
    draw_right(s, px + pw / 2, py + ph + 8, pw / 2, lbl, muted, t->panel_bg);
    if (l->range_lo || l->range_hi) {
        char a[16], b[16];
        snprintf(a, sizeof a, "%u.%02u", l->range_lo / 100, l->range_lo % 100);
        snprintf(b, sizeof b, "%u.%02u", l->range_hi / 100, l->range_hi % 100);
        snprintf(lbl, sizeof lbl, "%s - %s s", a, b);
        int w = ugfx_text_width(lbl);
        ugfx_draw_string_clipped(s, px + (pw - w) / 2, py + ph + 8, pw, lbl, t->accent, t->panel_bg);
    }
}

void uui_loglist_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_loglist *l = w;
    const struct utheme *t = utheme_current();
    struct cols c;
    columns(l, &c);
    struct ugfx_clip saved;
    ugfx_clip_save(s, &saved);
    ugfx_clip_intersect(s, l->x, l->y, l->w, l->h);
    if (l->timeline && l->set) draw_timeline(s, l);

    int hy = header_y(l), rh = row_h(), ty = hy + (rh - line_h()) / 2;
    uint32_t hbg = t->panel_bg, hfg = ugfx_blend(t->text, hbg, 150);
    ugfx_fill_rect(s, l->x, hy, l->w, rh, hbg);
    ugfx_fill_rect(s, l->x, hy + rh - 1, l->w, 1, t->separator);
    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    draw_right(s, c.time_x, ty, c.time_w, "Time", hfg, hbg);
    ugfx_draw_string_clipped(s, c.lvl_x, ty, c.lvl_w, "Level", hfg, hbg);
    ugfx_draw_string_clipped(s, c.src_x, ty, c.src_w, "Source", hfg, hbg);
    ugfx_draw_string_clipped(s, c.msg_x, ty, c.msg_w, "Message", hfg, hbg);
    ugfx_set_font(was);

    int y0 = rows_y(l), vh = rows_h(l);
    uint32_t bg = t->field_bg;
    ugfx_fill_rect(s, l->x, y0, l->w, vh, bg);
    if (!l->set || !l->set->view_count) {
        ugfx_draw_string_clipped(s, c.msg_x, y0 + rh, c.msg_w,
                                 l->set && l->set->count ? "No line matches -- clear a filter to see more"
                                                         : "Nothing logged",
                                 hfg, bg);
        ugfx_clip_restore(s, &saved);
        return;
    }
    ugfx_clip_intersect(s, l->x, y0, l->w, vh);
    for (int i = 0; i <= visible(l); i++) {
        int r = l->top + i;
        if (r >= l->set->view_count) break;
        int ry = y0 + i * rh;
        uint32_t rbg = bg;
        int rx = l->x + 4, rw = c.bar_x - l->x - 6;
        // The design language's selection: a soft fill with a mid-accent
        // edge, the full accent when the list has the keyboard.
        if (r == l->selected) {
            uint32_t edge = l->focused ? t->accent : ugfx_blend(t->field_bg, t->accent, 130);
            uui_fill_round_rect(s, rx, ry + 1, rw, rh - 2, 5, edge);
            uui_fill_round_rect(s, rx + 1, ry + 2, rw - 2, rh - 4, 4, t->selection_bg);
            rbg = t->selection_bg;
        } else if (r == l->hovered) {
            rbg = uui_state_bg(bg, UUI_STATE_HOVER);
            uui_fill_round_rect(s, rx, ry + 1, rw, rh - 2, 5, rbg);
        }
        draw_row(s, l, &c, r, ry, rbg);
    }
    // The overlay bar, with every ERROR's place marked on its edge -- a
    // long log's problems are findable from the scrollbar.
    int n = l->set->view_count;
    uui_scrollbar_draw_overlay(s, c.bar_x, y0, c.bar_w, vh, n, visible(l), l->top,
                               bg, t->text, l->bar_hover || l->thumb_grab >= 0 ? 255 : 0,
                               l->thumb_grab >= 0 ? UUI_SCROLLBAR_HELD : 0);
    if (n > visible(l)) {
        uint32_t red = utheme_severity(UTHEME_SEV_ERROR);
        for (int r = 0; r < n; r++) {
            if (ulog_severity(l->set->lines[l->set->view[r]].level) != UTHEME_SEV_ERROR) continue;
            int y = y0 + (int)((long long)r * vh / n);
            ugfx_fill_rect(s, c.bar_x + c.bar_w - 3, y, 3, 2, red);
        }
    }
    ugfx_clip_restore(s, &saved);
}

// --- geometry and input --------------------------------------------------

void uui_loglist_natural_size(const void *w, int *out_w, int *out_h) {
    const struct uui_loglist *l = w;
    *out_w = ui_w() * 60;
    *out_h = row_h() * 8 + uui_loglist_timeline_h(l);
}

void uui_loglist_set_geometry(void *w, int x, int y, int width, int height) {
    struct uui_loglist *l = w;
    l->x = x; l->y = y; l->w = width; l->h = height;
    clamp(l);
    if (l->follow) to_end(l);
}

static void bounds(const void *w, int *x, int *y, int *bw, int *bh) {
    const struct uui_loglist *l = w;
    *x = l->x; *y = l->y; *bw = l->w; *bh = l->h;
}

// The WHOLE rect: the list has a scrollbar (userland/CLAUDE.md's rule).
static int hit(const void *w, int cx, int cy) {
    const struct uui_loglist *l = w;
    return uui_hit(l->x, l->y, l->w, l->h, cx, cy);
}

static int row_at(const struct uui_loglist *l, int cy) {
    int y0 = rows_y(l);
    if (cy < y0 || cy >= y0 + rows_h(l)) return -1;
    int r = l->top + (cy - y0) / row_h();
    return r < nview(l) ? r : -1;
}

static int in_bar(const struct uui_loglist *l, int cx, int cy) {
    struct cols c;
    columns(l, &c);
    return cx >= c.bar_x && cy >= rows_y(l) && nview(l) > visible(l);
}

static unsigned cs_at(const struct uui_loglist *l, int cx) {
    int px, py, pw, ph;
    plot_rect(l, &px, &py, &pw, &ph);
    if (pw <= 0) return 0;
    if (cx < px) cx = px;
    if (cx > px + pw) cx = px + pw;
    return (unsigned)((unsigned long long)(cx - px) * span_of(l) / (unsigned)pw);
}

static int press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_loglist *l = w;
    if (l->timeline && cy < l->y + uui_loglist_timeline_h(l)) {
        l->brush_anchor = (long)cs_at(l, cx);
        return 1;
    }
    if (in_bar(l, cx, cy)) {
        struct cols c;
        columns(l, &c);
        int y0 = rows_y(l), vh = rows_h(l);
        enum uui_scrollbar_zone z = uui_scrollbar_hit(c.bar_x, y0, c.bar_w, vh, nview(l), visible(l),
                                                      l->top, cx, cy, 0);
        if (z == UUI_SB_THUMB) {
            int ty, th;
            uui_scrollbar_thumb_rect(y0, vh, nview(l), visible(l), l->top, &ty, &th, c.bar_w, 0);
            l->thumb_grab = cy - ty;
        } else if (z == UUI_SB_ABOVE) l->top -= visible(l);
        else if (z == UUI_SB_BELOW) l->top += visible(l);
        clamp(l);
        unfollow_if_off_end(l);
        return 1;
    }
    int r = row_at(l, cy);
    if (r < 0) return 1;
    if (r != l->selected) { l->selected = r; l->changes |= UUI_LOGLIST_SEL; }
    // CHOOSING A LINE IS READING IT: following would scroll it away within
    // a second. The newest line is the exception -- that is following.
    if (l->follow && r < nview(l) - 1) { l->follow = 0; l->changes |= UUI_LOGLIST_FOLLOW; }
    return 1;
}

static int motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_loglist *l = w;
    if (l->brush_anchor >= 0 && buttons) {
        unsigned a = (unsigned)l->brush_anchor, b = cs_at(l, cx);
        l->range_lo = a < b ? a : b;
        l->range_hi = a < b ? b : a;
        return 1;
    }
    if (l->thumb_grab >= 0 && buttons) {
        struct cols c;
        columns(l, &c);
        l->top = uui_scrollbar_offset_for_drag(rows_y(l), rows_h(l), nview(l), visible(l),
                                               cy, l->thumb_grab, c.bar_w, 0);
        clamp(l);
        unfollow_if_off_end(l);
        return 1;
    }
    int hb = in_bar(l, cx, cy), hr = hb ? -1 : row_at(l, cy);
    if (hb == l->bar_hover && hr == l->hovered) return 0;
    l->bar_hover = hb;
    l->hovered = hr;
    return 1;
}

static int release(void *w, int cx, int cy) {
    (void)cy;
    struct uui_loglist *l = w;
    if (l->brush_anchor >= 0) {
        // A CLICK without a drag clears the range -- the whole span again.
        if (cs_at(l, cx) == (unsigned)l->brush_anchor) l->range_lo = l->range_hi = 0;
        l->brush_anchor = -1;
        l->changes |= UUI_LOGLIST_RANGE;
        return 1;
    }
    if (l->thumb_grab >= 0) { l->thumb_grab = -1; return 1; }
    return 0;
}

static int wheel(void *w, int notches) {
    struct uui_loglist *l = w;
    int before = l->top;
    l->top -= notches * 3;
    clamp(l);
    unfollow_if_off_end(l);
    if (l->top + visible(l) >= nview(l) && notches < 0 && !l->follow) {
        // Scrolling down to the end is how a reader asks to follow again.
        l->follow = 1;
        l->changes |= UUI_LOGLIST_FOLLOW;
    }
    return l->top != before || l->changes;
}

static int key(void *w, int k, unsigned mods) {
    (void)mods;
    struct uui_loglist *l = w;
    int n = nview(l), was = l->selected;
    if (!n) return 0;
    int sel = l->selected < 0 ? (l->top + visible(l) - 1 < n ? l->top + visible(l) - 1 : n - 1) : l->selected;
    switch (k) {
    case KEY_ARROW_UP:   sel = l->selected < 0 ? sel : sel - 1; break;
    case KEY_ARROW_DOWN: sel = l->selected < 0 ? sel : sel + 1; break;
    case KEY_PAGE_UP:    sel -= visible(l) - 1; break;
    case KEY_PAGE_DOWN:  sel += visible(l) - 1; break;
    case KEY_HOME:       sel = 0; break;
    case KEY_END:        sel = n - 1; break;
    default: return 0;
    }
    if (sel < 0) sel = 0;
    if (sel >= n) sel = n - 1;
    l->selected = sel;
    reveal(l);
    if (k == KEY_END) { if (!l->follow) { l->follow = 1; l->changes |= UUI_LOGLIST_FOLLOW; } }
    else if (l->follow && sel < n - 1) { l->follow = 0; l->changes |= UUI_LOGLIST_FOLLOW; }
    if (sel != was) l->changes |= UUI_LOGLIST_SEL;
    return 1;
}

static void set_focused(void *w, int focused) { ((struct uui_loglist *)w)->focused = focused; }
static int accepts_focus(const void *w) { (void)w; return 1; }

static void describe(const void *w, const struct uui_describe *d) {
    // NOTHING THAT MOVES AS THE LOG GROWS (the row count, `top`): a
    // layout block is re-logged when it changes, so a live list would log
    // itself into the log it shows, once per refresh.
    const struct uui_loglist *l = w;
    uui_describe_int(d, "selected", l->selected);
    uui_describe_int(d, "follow", l->follow);
    uui_describe_int(d, "row_h", row_h());
    struct cols c;
    columns(l, &c);
    uui_describe_rect(d, "col_level", c.lvl_x, header_y(l), c.lvl_w, row_h());
    uui_describe_rect(d, "col_msg", c.msg_x, header_y(l), c.msg_w, row_h());
    uui_describe_rect(d, "rows_area", l->x, rows_y(l), l->w, rows_h(l));
    if (l->timeline) {
        int px, py, pw, ph;
        plot_rect(l, &px, &py, &pw, &ph);
        uui_describe_rect(d, "timeline", px, py, pw, ph);
    }
}

const struct uui_widget_ops uui_loglist_ops = {
    .natural_size  = uui_loglist_natural_size,
    .set_geometry  = uui_loglist_set_geometry,
    .bounds        = bounds,
    .draw          = uui_loglist_draw,
    .hit           = hit,
    .key           = key,
    .set_focused   = set_focused,
    .accepts_focus = accepts_focus,
    .press         = press,
    .motion        = motion,
    .release       = release,
    .wheel         = wheel,
    .describe      = describe,
};
