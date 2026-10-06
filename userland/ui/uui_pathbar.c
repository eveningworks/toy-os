// pathbar -- a breadcrumb path. See ui/uui_pathbar.h.
#include "ui/uui_pathbar.h"
#include <stdio.h>
#include "ui/uui_widget.h"
#include "ui/uui_primitives.h"
#include "ui/uui_describe.h"
#include "ui/utheme.h"
#include "lib/icon_cache.h"
#include "keyboard.h"
#include "win_proto.h"   // WIN_CURSOR_*
#include <string.h>

#define PAD      10  // inside a segment, each side -- clear of the chip's round ends
#define SEP_W    12  // the chevron between segments
#define EDIT_GAP 28  // blank space kept at the right: a click there edits

// --- the path, as segments ----------------------------------------------

// How long a "scheme:/" prefix the path opens with -- the root of a
// virtual folder, which is not a component -- or 0.
static int scheme_len(const char *path) {
    int i = 0;
    while ((path[i] >= 'a' && path[i] <= 'z') || (path[i] >= '0' && path[i] <= '9')) i++;
    return (i > 0 && path[i] == ':' && path[i + 1] == '/') ? i + 2 : 0;
}

// Component `k` (1-based) of the path: where it starts and its length.
// 0 when there is no such component.
static int component(const char *path, int k, int *start, int *len) {
    int i = scheme_len(path), n = 0;
    while (path[i]) {
        while (path[i] == '/') i++;
        if (!path[i]) break;
        int s = i;
        while (path[i] && path[i] != '/') i++;
        if (++n == k) { *start = s; *len = i - s; return 1; }
    }
    return 0;
}

static int seg_count(const char *path) {
    int n = 1, s, l;
    while (component(path, n, &s, &l)) n++;
    return n;   // the root plus each component
}

// Segment `k`'s path: "/" for the root, else the path up to its end.
static void seg_path(const char *path, int k, char *out, int cap) {
    int s, l;
    if (k <= 0 || !component(path, k, &s, &l)) {
        int sl = scheme_len(path);
        if (sl) snprintf(out, (size_t)cap, "%.*s", sl, path);
        else strlcpy(out, "/", (size_t)cap);
        return;
    }
    int n = s + l < cap - 1 ? s + l : cap - 1;
    memcpy(out, path, (size_t)n);
    out[n] = '\0';
}

// The root chip's label and icon: a registered scheme's, else the
// filesystem root's.
static int root_scheme(const struct uui_pathbar *p) {
    int sl = scheme_len(p->path);
    for (int i = 0; sl && i < p->nscheme; i++)
        if (!strncmp(p->path, p->scheme[i].prefix, (size_t)sl) && !p->scheme[i].prefix[sl])
            return i;
    return -1;
}

static const char *root_icon(const struct uui_pathbar *p) {
    int i = root_scheme(p);
    return i >= 0 ? p->scheme[i].icon : p->root_icon;
}

static void seg_label(const struct uui_pathbar *p, int k, char *out, int cap) {
    int s, l;
    if (k == 0) {
        int i = root_scheme(p), sl = scheme_len(p->path);
        if (i >= 0) strlcpy(out, p->scheme[i].label, (size_t)cap);
        else if (sl) snprintf(out, (size_t)cap, "%.*s", sl, p->path);
        else strlcpy(out, p->root_label ? p->root_label : "/", (size_t)cap);
        return;
    }
    if (k == UUI_PATHBAR_ELIDED) { strlcpy(out, "...", (size_t)cap); return; }
    if (!component(p->path, k, &s, &l)) { out[0] = '\0'; return; }
    if (l > cap - 1) l = cap - 1;
    memcpy(out, p->path + s, (size_t)l);
    out[l] = '\0';
}

// --- layout -------------------------------------------------------------

struct lay {
    int n;
    int seg[UUI_PATHBAR_SEGS];  // a segment index, or UUI_PATHBAR_ELIDED
    int x[UUI_PATHBAR_SEGS], w[UUI_PATHBAR_SEGS];
};

static int icon_px(void) { return ugfx_char_h(); }

static int text_w(const char *s, int bold) {
    const struct ugfx_font *was = bold ? ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD)) : 0;
    int w = ugfx_text_width(s);
    if (bold) ugfx_set_font(was);
    return w;
}

// The LAST segment is bold -- where you are -- so it is measured bold.
static int seg_w(const struct uui_pathbar *p, int k, int last) {
    char label[UUI_PATHBAR_MAX];
    seg_label(p, k, label, sizeof label);
    int w = 2 * PAD + text_w(label, last);
    if (k == 0 && root_icon(p)) w += icon_px() + 4;
    return w;
}

static void layout(const struct uui_pathbar *p, struct lay *l) {
    int n = seg_count(p->path);
    if (n > UUI_PATHBAR_SEGS - 1) n = UUI_PATHBAR_SEGS - 1;
    int avail = p->w - 4 - EDIT_GAP;
    int total = 0;
    for (int k = 0; k < n; k++) total += seg_w(p, k, k == n - 1) + (k ? SEP_W : 0);

    // Every segment, or the root, "...", and as many of the last as fit
    // -- always at least the last one, which is where you are.
    int first = 1;
    if (total > avail && n > 2) {
        int used = seg_w(p, 0, 0) + SEP_W + seg_w(p, UUI_PATHBAR_ELIDED, 0);
        first = n - 1;
        used += SEP_W + seg_w(p, first, 1);
        while (first > 1 && used + SEP_W + seg_w(p, first - 1, 0) <= avail) {
            first--;
            used += SEP_W + seg_w(p, first, 0);
        }
    }
    l->n = 0;
    int x = p->x + 2;
    for (int k = 0; k < n; k++) {
        if (k > 0 && k < first) {
            if (k != 1) continue;
            l->seg[l->n] = UUI_PATHBAR_ELIDED;
        } else {
            l->seg[l->n] = k;
        }
        if (l->n) x += SEP_W;
        l->x[l->n] = x;
        l->w[l->n] = seg_w(p, l->seg[l->n], k == n - 1);
        x += l->w[l->n];
        l->n++;
    }
}

static int hit_seg(const struct uui_pathbar *p, int cx, int cy) {
    if (!uui_hit(p->x, p->y, p->w, p->h, cx, cy)) return -1;
    struct lay l;
    layout(p, &l);
    for (int i = 0; i < l.n; i++)
        if (cx >= l.x[i] && cx < l.x[i] + l.w[i]) return l.seg[i];
    return -1;
}

// Where a segment (or the "...") takes you.
static void target_of(const struct uui_pathbar *p, int seg, char *out, int cap) {
    if (seg == UUI_PATHBAR_ELIDED) {
        struct lay l;
        layout(p, &l);
        // The parent of the first segment shown after the "...".
        int after = l.n > 2 ? l.seg[2] : 1;
        seg_path(p->path, after - 1, out, cap);
        return;
    }
    seg_path(p->path, seg, out, cap);
}

// --- the API ------------------------------------------------------------

void uui_pathbar_init(struct uui_pathbar *p, const char *root_label, const char *root_icon) {
    memset(p, 0, sizeof *p);
    p->root_label = root_label;
    p->root_icon = root_icon;
    p->hot = p->armed = -1;
    strlcpy(p->path, "/", sizeof p->path);
    uui_textbox_init(&p->edit, "");
}

void uui_pathbar_set_scheme(struct uui_pathbar *p, const char *prefix,
                            const char *label, const char *icon) {
    if (p->nscheme >= (int)(sizeof p->scheme / sizeof p->scheme[0])) return;
    p->scheme[p->nscheme].prefix = prefix;
    p->scheme[p->nscheme].label = label;
    p->scheme[p->nscheme].icon = icon;
    p->nscheme++;
}

void uui_pathbar_set_path(struct uui_pathbar *p, const char *path) {
    strlcpy(p->path, path && path[0] ? path : "/", sizeof p->path);
    p->editing = 0;
    uui_textbox_set_active(&p->edit, 0);
    p->hot = p->armed = -1;
}

void uui_pathbar_begin_edit(struct uui_pathbar *p) {
    uui_textbox_init(&p->edit, p->path);
    uui_textbox_set_geometry(&p->edit, p->x, p->y, p->w, p->h);
    uui_textbox_set_active(&p->edit, 1);
    uui_textbox_key(&p->edit, 0x01);   // Ctrl-A: typing replaces the path
    p->editing = 1;
    p->hot = p->armed = -1;
}

void uui_pathbar_end_edit(struct uui_pathbar *p) {
    p->editing = 0;
    uui_textbox_set_active(&p->edit, 0);
}

int uui_pathbar_is_editing(const struct uui_pathbar *p) { return p->editing; }

int uui_pathbar_take(struct uui_pathbar *p, char *out, int cap) {
    if (!p->has_taken) return 0;
    strlcpy(out, p->taken, (size_t)cap);
    p->has_taken = 0;
    return 1;
}

int uui_pathbar_segment_count(const struct uui_pathbar *p) {
    struct lay l;
    layout(p, &l);
    return l.n;
}

int uui_pathbar_segment_rect(const struct uui_pathbar *p, int i,
                             int *x, int *y, int *w, int *h) {
    struct lay l;
    layout(p, &l);
    for (int j = 0; j < l.n; j++) {
        if (l.seg[j] != i) continue;
        if (x) *x = l.x[j];
        if (y) *y = p->y + 2;
        if (w) *w = l.w[j];
        if (h) *h = p->h - 4;
        return 1;
    }
    return 0;
}

// --- drawing ------------------------------------------------------------

static void draw_chevron(struct ugfx_surface *s, int x, int y, int h, uint32_t c) {
    int cy = y + h / 2;
    for (int r = 0; r < 4; r++) {
        ugfx_fill_rect(s, x + 4 + r, cy - 3 + r, 1, 1, c);
        ugfx_fill_rect(s, x + 4 + r, cy + 3 - r, 1, 1, c);
    }
}

static void op_draw(struct ugfx_surface *s, const void *w) {
    const struct uui_pathbar *p = w;
    if (p->editing) { uui_textbox_draw(s, &p->edit); return; }

    uint32_t bg = UTHEME_WHITE, fg = UTHEME_TEXT;
    ugfx_fill_rect(s, p->x, p->y, p->w, p->h, bg);
    ugfx_draw_rect(s, p->x, p->y, p->w, p->h, UTHEME_BORDER);

    struct lay l;
    layout(p, &l);
    int n = seg_count(p->path);
    uint32_t dim = uui_state_bg(fg, UUI_STATE_DISABLED);
    ugfx_set_clip_rect(s, p->x + 1, p->y + 1, p->w - 2, p->h - 2);
    for (int i = 0; i < l.n; i++) {
        int k = l.seg[i], x = l.x[i], sw = l.w[i];
        int last = (k == n - 1);
        if (i) draw_chevron(s, x - SEP_W, p->y, p->h, dim);
        // EACH SEGMENT A CHIP: an accent wash for the folders above, the
        // folder you are in filled in the accent -- the colour-coded File
        // Manager's breadcrumb (2026-10-01), and where you are reads first.
        uint32_t chip = last ? UTHEME_ACCENT : ugfx_blend(bg, UTHEME_ACCENT, 34);
        uint32_t cfg = last ? UTHEME_ACCENT_TEXT : fg;
        if (k == p->armed) chip = uui_state_bg(chip, UUI_STATE_PRESSED);
        else if (k == p->hot) chip = uui_state_bg(chip, UUI_STATE_HOVER);
        uui_fill_round_rect(s, x, p->y + 3, sw, p->h - 6, UUI_CAPSULE, chip);
        uint32_t sbg = chip;

        int tx = x + PAD;
        if (k == 0 && root_icon(p)) {
            const struct uimg *ico = icon_get(root_icon(p), icon_px());
            if (ico) ugfx_blit_alpha(s, tx, p->y + (p->h - ico->h) / 2,
                                     ico->w, ico->h, ico->px, ico->w);
            tx += icon_px() + 4;
        }
        char label[UUI_PATHBAR_MAX];
        seg_label(p, k, label, sizeof label);
        const struct ugfx_font *was = last ? ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD)) : 0;
        ugfx_draw_string_clipped(s, tx, p->y + (p->h - ugfx_char_h()) / 2,
                                  x + sw - tx, label, cfg, sbg);
        if (last) ugfx_set_font(was);
    }
    ugfx_clear_clip_rect(s);
    if (p->focused) uui_focus_ring(s, p->x, p->y, p->w, p->h);
}

// --- input ----------------------------------------------------------------

static int op_press(void *w, int cx, int cy, unsigned mods) {
    struct uui_pathbar *p = w;
    if (p->editing) return uui_textbox_ops.press(&p->edit, cx, cy, mods);
    int k = hit_seg(p, cx, cy);
    if (k != -1) { p->armed = k; return 1; }
    // Past the last segment: the path as text, to type over.
    uui_pathbar_begin_edit(p);
    return 1;
}

static int op_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_pathbar *p = w;
    if (p->editing)
        return uui_textbox_ops.motion ? uui_textbox_ops.motion(&p->edit, cx, cy, buttons) : 0;
    int k = hit_seg(p, cx, cy);
    if (k == p->hot) return 0;
    p->hot = k;
    return 1;
}

// A click is a press and a release on the SAME segment.
static int op_release(void *w, int cx, int cy) {
    struct uui_pathbar *p = w;
    if (p->editing) {
        if (uui_textbox_ops.release) uui_textbox_ops.release(&p->edit, cx, cy);
        return 1;
    }
    int k = p->armed;
    p->armed = -1;
    if (k != -1 && hit_seg(p, cx, cy) == k) {
        target_of(p, k, p->taken, sizeof p->taken);
        p->has_taken = 1;
    }
    return 1;
}

static int op_key(void *w, int key, unsigned mods) {
    struct uui_pathbar *p = w;
    if (!p->editing) return 0;
    if (key == '\n' || key == '\r') {
        strlcpy(p->taken, uui_textbox_text(&p->edit), sizeof p->taken);
        p->has_taken = 1;
        return 1;
    }
    if (key == 0x1B) { uui_pathbar_end_edit(p); return 1; }
    return uui_textbox_key_mods(&p->edit, key, mods);
}

static int op_accepts_focus(const void *w) { return ((const struct uui_pathbar *)w)->editing; }
static void op_set_focused(void *w, int on) {
    struct uui_pathbar *p = w;
    p->focused = on;
    if (!on && p->editing) uui_pathbar_end_edit(p);
}

static int op_cursor(const void *w, int cx, int cy) {
    (void)cx; (void)cy;
    return ((const struct uui_pathbar *)w)->editing ? WIN_CURSOR_TEXT : WIN_CURSOR_DEFAULT;
}

static void op_natural(const void *w, int *out_w, int *out_h) {
    const struct uui_pathbar *p = w;
    int h;
    uui_textbox_natural_size(&p->edit, 0, &h);
    if (h < ugfx_char_h() + 8) h = ugfx_char_h() + 8;
    if (out_w) *out_w = 0;    // no preference: it stretches
    if (out_h) *out_h = h;
}

static void op_geometry(void *w, int x, int y, int width, int height) {
    struct uui_pathbar *p = w;
    p->x = x; p->y = y; p->w = width; p->h = height;
    uui_textbox_set_geometry(&p->edit, x, y, width, height);
}

static void op_bounds(const void *w, int *x, int *y, int *bw, int *bh) {
    const struct uui_pathbar *p = w;
    if (x) *x = p->x;
    if (y) *y = p->y;
    if (bw) *bw = p->w;
    if (bh) *bh = p->h;
}

static int op_hit(const void *w, int cx, int cy) {
    const struct uui_pathbar *p = w;
    return uui_hit(p->x, p->y, p->w, p->h, cx, cy);
}

static void op_describe(const void *w, const struct uui_describe *d) {
    const struct uui_pathbar *p = w;
    struct lay l;
    layout(p, &l);
    for (int i = 0; i < l.n; i++)
        uui_describe_rect_i(d, "seg", l.seg[i], l.x[i], p->y + 2, l.w[i], p->h - 4);
    uui_describe_int(d, "editing", p->editing);
}

const struct uui_widget_ops uui_pathbar_ops = {
    .natural_size  = op_natural,
    .set_geometry  = op_geometry,
    .bounds        = op_bounds,
    .draw          = op_draw,
    .hit           = op_hit,
    .press         = op_press,
    .motion        = op_motion,
    .release       = op_release,
    .key           = op_key,
    .accepts_focus = op_accepts_focus,
    .set_focused   = op_set_focused,
    .cursor        = op_cursor,
    .describe      = op_describe,
};
