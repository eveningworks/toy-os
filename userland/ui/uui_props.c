// See uui_props.h.
#include "ui/uui_props.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "ui/utheme.h"
#include "ui/uui_widget.h"
#include "ui/uui_primitives.h"
#include "lib/icon_cache.h"

#define MAX_LINES 4   // a value wraps this far, then elides

static int pad(void)   { return utheme_pad(); }
static int lh(void)    { return ugfx_char_h() + utheme_gap(); }
static int hdr_h(void) { return lh() + pad(); }

// --- building ----------------------------------------------------------------

void uui_props_init(struct uui_props *w) {
    memset(w, 0, sizeof *w);
    w->laid_w = -1;
    w->bg = UTHEME_PANEL_BG;
    w->hot = w->armed = w->action = -1;
}

void uui_props_begin(struct uui_props *w, int keep_scroll) {
    w->nsec = w->nrow = 0;
    w->laid_w = -1;
    w->hot = w->armed = -1;
    if (!keep_scroll) w->scroll = 0;
}

int uui_props_section(struct uui_props *w, const char *title, int kind) {
    if (w->nsec >= UUI_PROPS_SECTIONS) return -1;
    struct uui_props_section *s = &w->sec[w->nsec];
    memset(s, 0, sizeof *s);
    snprintf(s->title, sizeof s->title, "%s", title);
    s->kind = kind;
    s->first = w->nrow;
    w->laid_w = -1;
    return w->nsec++;
}

void uui_props_set_action(struct uui_props *w, int section, const char *link) {
    if (section < 0 || section >= w->nsec) return;
    snprintf(w->sec[section].action, sizeof w->sec[section].action, "%s", link ? link : "");
}

void uui_props_row(struct uui_props *w, const char *key, const char *fmt, ...) {
    if (!w->nsec || w->nrow >= UUI_PROPS_ROWS) return;
    struct uui_props_row *r = &w->row[w->nrow++];
    snprintf(r->key, sizeof r->key, "%s", key ? key : "");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->val, sizeof r->val, fmt, ap);
    va_end(ap);
    w->sec[w->nsec - 1].nrows++;
    w->laid_w = -1;
}

void uui_props_slot(struct uui_props *w, int section, int id, int h) {
    if (section < 0 || section >= w->nsec) return;
    w->sec[section].slot_id = id;
    w->sec[section].slot_h = h > 0 ? h : 0;
    w->laid_w = -1;
}

int uui_props_take_action(struct uui_props *w) {
    int a = w->action;
    w->action = -1;
    return a;
}

// --- measuring ---------------------------------------------------------------

// The key column: as wide as the widest key, between 8 and 16 characters
// and never past two fifths of the page.
static int key_w(const struct uui_props *w, int width) {
    int per = ugfx_char_advance('n');
    if (per <= 0) per = 8;
    int k = per * 8;
    for (int i = 0; i < w->nrow; i++) {
        int kw = ugfx_text_width(w->row[i].key) + per * 2;
        if (kw > k) k = kw;
    }
    if (k > per * 16) k = per * 16;
    if (k > width * 2 / 5) k = width * 2 / 5;
    return k;
}

// Where each wrapped line of `text` starts in a column `width` wide:
// broken at the last space that fits, or mid-word when one word alone
// does not. Measured word by word, so a line costs a width per word.
// Returns the line count (at most MAX_LINES).
static int prefix_w(const char *text, int from, int to) {
    char buf[UUI_PROPS_VALUE];
    int n = to - from;
    if (n <= 0) return 0;
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    memcpy(buf, text + from, (size_t)n);
    buf[n] = '\0';
    return ugfx_text_width(buf);
}

static int wrap(const char *text, int width, int starts[MAX_LINES]) {
    int n = 0, at = 0, len = (int)strlen(text);
    while (n < MAX_LINES) {
        starts[n++] = at;
        if (width <= 0 || prefix_w(text, at, len) <= width) break;
        int brk = -1;
        for (int k = at + 1; k < len; k++) {
            if (text[k] != ' ') continue;
            if (prefix_w(text, at, k) > width) break;
            brk = k;
        }
        if (brk > at) { at = brk + 1; continue; }
        // One word wider than the column: as much of it as fits.
        int fit = at + 1;
        while (fit < len && prefix_w(text, at, fit + 1) <= width) fit++;
        at = fit;
    }
    return n;
}

static int value_w(const struct uui_props *w, int width, const struct uui_props_row *r) {
    int inner = width - 4 * pad();
    return r->key[0] ? inner - w->key_w : inner;
}

static int notice_text_w(int width) { return width - 6 * pad() - ugfx_char_h(); }

// Measures every row and section at `width`, once: the builders and a
// new width invalidate it. Called from draw through a cast, as the
// cached stage in uui_fileinfo is -- the layout is the widget's own.
static void layout(const struct uui_props *cw, int width) {
    struct uui_props *w = (struct uui_props *)cw;
    if (w->laid_w == width) return;
    w->laid_w = width;
    w->key_w = key_w(w, width);
    for (int i = 0; i < w->nsec; i++) {
        struct uui_props_section *s = &w->sec[i];
        int notice = s->kind == UUI_PROPS_NOTICE;
        int h = notice ? 3 * pad() : hdr_h() + s->slot_h + pad() / 2;
        for (int k = 0; k < s->nrows; k++) {
            struct uui_props_row *r = &w->row[s->first + k];
            int st[MAX_LINES];
            int n = wrap(r->val, notice ? notice_text_w(width) : value_w(w, width, r), st);
            w->lines[s->first + k] = (unsigned char)n;
            h += n * lh();
        }
        w->sec_h[i] = h;
    }
}

static int section_h(const struct uui_props *w, int i) {
    layout(w, w->w);
    return w->sec_h[i];
}

int uui_props_height(const struct uui_props *w, int width) {
    layout(w, width);
    int h = pad();
    for (int i = 0; i < w->nsec; i++) h += w->sec_h[i];
    if (width != w->w) ((struct uui_props *)w)->laid_w = -1;   // measured for a caller's width
    return h + pad();
}

static int section_y(const struct uui_props *w, int i) {
    int y = w->y + pad() - w->scroll;
    for (int k = 0; k < i; k++) y += section_h(w, k);
    return y;
}

static int max_scroll(const struct uui_props *w) {
    int m = uui_props_height(w, w->w) - w->h;
    return m > 0 ? m : 0;
}

static void clamp(struct uui_props *w) {
    if (w->scroll > max_scroll(w)) w->scroll = max_scroll(w);
    if (w->scroll < 0) w->scroll = 0;
}

int uui_props_slot_rect(const struct uui_props *w, int id, int *x, int *y, int *ww, int *hh) {
    for (int i = 0; i < w->nsec; i++) {
        const struct uui_props_section *s = &w->sec[i];
        if (s->slot_id != id || !s->slot_h || s->kind == UUI_PROPS_NOTICE) continue;
        int sy = section_y(w, i) + hdr_h();
        if (sy < w->y || sy + s->slot_h > w->y + w->h) return 0;
        *x = w->x + 2 * pad();
        *y = sy;
        *ww = w->w - 4 * pad();
        *hh = s->slot_h;
        return 1;
    }
    return 0;
}

// The action link's rect in section `i` (0 when it has none).
static int link_rect(const struct uui_props *w, int i, int *x, int *y, int *ww, int *hh) {
    const struct uui_props_section *s = &w->sec[i];
    if (!s->action[0] || s->kind == UUI_PROPS_NOTICE) return 0;
    int lw = ugfx_text_width(s->action);
    *x = w->x + w->w - 2 * pad() - lw;
    *y = section_y(w, i) + (hdr_h() - ugfx_char_h()) / 2;
    *ww = lw;
    *hh = ugfx_char_h();
    return 1;
}

int uui_props_action_rect(const struct uui_props *w, const char *title,
                          int *x, int *y, int *ww, int *hh) {
    for (int i = 0; i < w->nsec; i++)
        if (!strcmp(w->sec[i].title, title) && link_rect(w, i, x, y, ww, hh))
            return *y >= w->y && *y + *hh <= w->y + w->h;
    return 0;
}

void uui_props_natural_size(const struct uui_props *w, int *out_w, int *out_h) {
    int per = ugfx_char_advance('n');
    int width = (per > 0 ? per : 8) * 48;
    if (out_w) *out_w = width;
    if (out_h) *out_h = uui_props_height(w, width);
}

void uui_props_set_geometry(struct uui_props *w, int x, int y, int width, int height) {
    if (width != w->w) w->laid_w = -1;
    w->x = x; w->y = y; w->w = width; w->h = height;
    clamp(w);
}

// --- as text -----------------------------------------------------------------

int uui_props_text(const struct uui_props *w, char *out, int cap) {
    if (cap <= 0) return -1;
    int len = 0;
    out[0] = '\0';
    for (int i = 0; i < w->nsec; i++) {
        const struct uui_props_section *s = &w->sec[i];
        int n = snprintf(out + len, (size_t)(cap - len), "%s%s\n", len ? "\n" : "", s->title);
        if (n < 0 || n >= cap - len) { out[0] = '\0'; return -1; }
        len += n;
        for (int k = 0; k < s->nrows; k++) {
            const struct uui_props_row *r = &w->row[s->first + k];
            n = r->key[0] ? snprintf(out + len, (size_t)(cap - len), "  %s: %s\n", r->key, r->val)
                          : snprintf(out + len, (size_t)(cap - len), "  %s\n", r->val);
            if (n < 0 || n >= cap - len) { out[0] = '\0'; return -1; }
            len += n;
        }
    }
    return len;
}

// --- drawing -----------------------------------------------------------------

static void draw_wrapped(struct ugfx_surface *s, int x, int y, int width, const char *text,
                         uint32_t fg, uint32_t bg) {
    int st[MAX_LINES];
    int n = wrap(text, width, st);   // only rows on screen get here
    char buf[UUI_PROPS_VALUE];
    for (int l = 0; l < n; l++) {
        int end = l + 1 < n ? st[l + 1] : (int)strlen(text);
        int len = end - st[l];
        memcpy(buf, text + st[l], (size_t)len);
        buf[len] = '\0';
        // The last line keeps whatever is left, elided if it still runs over.
        if (l + 1 == n) ugfx_draw_string_elided(s, x, y + l * lh(), width, text + st[l], fg, bg);
        else ugfx_draw_string_clipped(s, x, y + l * lh(), width, buf, fg, bg);
    }
}

void uui_props_draw(struct ugfx_surface *s, const struct uui_props *w) {
    const struct utheme *t = utheme_current();
    uint32_t bg = w->bg;
    layout(w, w->w);
    int p = pad(), kw = w->key_w;
    ugfx_set_clip_rect(s, w->x, w->y, w->w, w->h);
    ugfx_fill_rect(s, w->x, w->y, w->w, w->h, bg);
    for (int i = 0; i < w->nsec; i++) {
        const struct uui_props_section *sec = &w->sec[i];
        int y = section_y(w, i), sh = section_h(w, i);
        if (y > w->y + w->h || y + sh < w->y) continue;
        if (sec->kind == UUI_PROPS_NOTICE) {
            // A WARNING BOX: amber edge on a pale amber ground, the same
            // "!" the tree's badge draws, then the text wrapped beside it.
            uint32_t edge = ugfx_rgb(227, 196, 103), fill = ugfx_rgb(255, 246, 220);
            int bx = w->x + 2 * p, bw = w->w - 4 * p, bh = sh - p;
            uui_fill_round_rect(s, bx, y, bw, bh, p / 2, edge);
            uui_fill_round_rect(s, bx + 1, y + 1, bw - 2, bh - 2, p / 2, fill);
            int isz = ugfx_char_h();
            const struct uimg *ic = icon_get("badge-warning", isz);
            if (ic) ugfx_blit_alpha(s, bx + p, y + p, ic->w, ic->h, ic->px, ic->w);
            int ty = y + p, tx = bx + 2 * p + isz;
            for (int k = 0; k < sec->nrows; k++) {
                draw_wrapped(s, tx, ty, notice_text_w(w->w), w->row[sec->first + k].val, t->text, fill);
                ty += w->lines[sec->first + k] * lh();
            }
            continue;
        }
        if (i > 0) ugfx_fill_rect(s, w->x + 2 * p, y, w->w - 4 * p, 1, t->separator);
        int cy = y + (hdr_h() - ugfx_char_h()) / 2;
        const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        int lx, ly, lw, lhh;
        int has_link = link_rect(w, i, &lx, &ly, &lw, &lhh);
        int title_w = (has_link ? lx - p : w->x + w->w - 2 * p) - (w->x + 2 * p);
        ugfx_draw_string_clipped(s, w->x + 2 * p, cy, title_w, sec->title, t->accent, bg);
        ugfx_set_font(was);
        if (has_link) {
            uint32_t lc = i == w->armed ? t->text : t->accent;
            ugfx_draw_string_clipped(s, lx, ly, lw, sec->action, lc, bg);
            if (i == w->hot) ugfx_fill_rect(s, lx, ly + lhh - 1, lw, 1, lc);
        }
        y += hdr_h() + sec->slot_h;
        for (int k = 0; k < sec->nrows; k++) {
            const struct uui_props_row *r = &w->row[sec->first + k];
            int vx = r->key[0] ? w->x + 2 * p + kw : w->x + 2 * p;
            if (r->key[0]) ugfx_draw_string_clipped(s, w->x + 2 * p, y, kw - p, r->key, t->outline, bg);
            if (y + lh() > w->y && y < w->y + w->h)
                draw_wrapped(s, vx, y, value_w(w, w->w, r), r->val, t->text, bg);
            y += w->lines[sec->first + k] * lh();
        }
    }
    // Taller than the window: a slim thumb at the right edge says so.
    int total = uui_props_height(w, w->w);
    if (total > w->h && w->h > 0) {
        int th = w->h * w->h / total;
        if (th < lh()) th = lh();
        int ty = w->y + (int)((long long)w->scroll * (w->h - th) / (total - w->h));
        uui_fill_round_rect(s, w->x + w->w - 5, ty, 3, th, 1, uui_state_bg(t->text, UUI_STATE_DISABLED));
    }
    ugfx_clear_clip_rect(s);
}

// --- input: an action link is a click (armed on press, taken on release) ------

static int link_at(const struct uui_props *w, int cx, int cy) {
    for (int i = 0; i < w->nsec; i++) {
        int x, y, ww, hh;
        if (!link_rect(w, i, &x, &y, &ww, &hh)) continue;
        if (y < w->y || y + hh > w->y + w->h) continue;
        if (uui_hit(x - 2, y - 2, ww + 4, hh + 4, cx, cy)) return i;
    }
    return -1;
}

static void p_natural(const void *w, int *ow, int *oh) { uui_props_natural_size(w, ow, oh); }
static void p_geom(void *w, int x, int y, int ww, int hh) { uui_props_set_geometry(w, x, y, ww, hh); }
static void p_bounds(const void *v, int *x, int *y, int *ww, int *hh) {
    const struct uui_props *w = v;
    *x = w->x; *y = w->y; *ww = w->w; *hh = w->h;
}
static void p_draw(struct ugfx_surface *s, const void *w) { uui_props_draw(s, w); }
// The whole rect: the wheel scrolls it anywhere, not only over a link.
static int p_hit(const void *v, int cx, int cy) {
    const struct uui_props *w = v;
    return uui_hit(w->x, w->y, w->w, w->h, cx, cy);
}
static int p_press(void *v, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_props *w = v;
    w->armed = link_at(w, cx, cy);
    return w->armed >= 0;
}
static int p_motion(void *v, int cx, int cy, unsigned buttons) {
    (void)buttons;
    struct uui_props *w = v;
    int h = link_at(w, cx, cy);
    if (h == w->hot) return 0;
    w->hot = h;
    return 1;
}
static int p_release(void *v, int cx, int cy) {
    struct uui_props *w = v;
    int was = w->armed;
    w->armed = -1;
    if (was < 0) return 0;
    if (link_at(w, cx, cy) == was) w->action = was;
    return 1;
}
static int p_wheel(void *v, int notches) {
    struct uui_props *w = v;
    int was = w->scroll;
    w->scroll -= notches * 3 * lh();
    clamp(w);
    return w->scroll != was;
}

const struct uui_widget_ops uui_props_ops = {
    .natural_size = p_natural,
    .set_geometry = p_geom,
    .bounds       = p_bounds,
    .draw         = p_draw,
    .hit          = p_hit,
    .press        = p_press,
    .motion       = p_motion,
    .release      = p_release,
    .wheel        = p_wheel,
};
