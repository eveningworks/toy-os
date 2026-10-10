// optlist. See ui/uui_optlist.h.
#include "ui/uui_optlist.h"
#include "ui/uui_widget.h"
#include "keyboard.h"
#include <string.h>

// --- geometry, ONE derivation for draw, hit and the field ---------------

#define OL_INSET   4    // the rounded row's inset from the box edges
#define OL_RADIUS  5

static int pad_x(void) { return utheme_pad(); }
static int gap(void)   { return utheme_gap() + 2; }
static int check_sz(void) { return utheme_indicator(); }
static int field_h(void) {
    int tb;
    uui_textbox_natural_size(0, 0, &tb);
    return tb;
}

static int bar_w(const struct uui_optlist *ol) {
    (void)ol;
    return uui_sbar_width();
}

int uui_optlist_row_h(const struct uui_optlist *ol) {
    if (ol->row_h > 0) return ol->row_h;
    int rh = 2 * ugfx_char_h();
    return rh > field_h() + 6 ? rh : field_h() + 6;
}

int uui_optlist_visible_rows(const struct uui_optlist *ol) {
    int rh = uui_optlist_row_h(ol);
    int n = rh > 0 ? ol->h / rh : 0;
    return n > 0 ? n : 1;
}

int uui_optlist_scrollbar_visible(const struct uui_optlist *ol) {
    return ol->count > uui_optlist_visible_rows(ol);
}

static int any_value(const struct uui_optlist *ol) {
    for (int i = 0; i < ol->count; i++) if (ol->items[i].has_value) return 1;
    return 0;
}

static int name_col_w(const struct uui_optlist *ol) {
    int widest = 0;
    for (int i = 0; i < ol->count; i++) {
        int tw = ol->items[i].name ? ugfx_text_width(ol->items[i].name) : 0;
        if (tw > widest) widest = tw;
    }
    return widest;
}

static int value_col_w(const struct uui_optlist *ol) {
    if (!any_value(ol)) return 0;
    return (ol->value_chars > 0 ? ol->value_chars : 16) * ugfx_char_w();
}

// Column x offsets from the widget's left edge. The value and the
// description columns line up across rows whether a row has a field or not.
struct ol_cols { int check, name, value, desc; };

static struct ol_cols columns(const struct uui_optlist *ol) {
    struct ol_cols c;
    c.check = OL_INSET + pad_x();
    c.name = c.check + check_sz() + gap();
    c.value = c.name + name_col_w(ol) + 2 * gap();
    int vw = value_col_w(ol);
    c.desc = vw ? c.value + vw + 2 * gap() : c.value;
    return c;
}

static int text_right(const struct uui_optlist *ol) {
    int bar = uui_optlist_scrollbar_visible(ol) ? bar_w(ol) : 0;
    return ol->x + ol->w - bar - OL_INSET - pad_x();
}

// A row's top on screen now, glide included; 0 when it is out of sight.
static int row_y(const struct uui_optlist *ol, int row, int *out_y) {
    int rh = uui_optlist_row_h(ol);
    int ry = ol->y + (row - ol->top) * rh + ol->anim.disp;
    if (row < 0 || row >= ol->count || ry + rh <= ol->y || ry >= ol->y + ol->h) return 0;
    *out_y = ry;
    return 1;
}

int uui_optlist_check_rect(const struct uui_optlist *ol, int row,
                           int *x, int *y, int *w, int *h) {
    int ry;
    if (!row_y(ol, row, &ry)) return 0;
    int cs = check_sz();
    *x = ol->x + columns(ol).check;
    *y = ry + (uui_optlist_row_h(ol) - cs) / 2;
    *w = *h = cs;
    return 1;
}

int uui_optlist_value_rect(const struct uui_optlist *ol, int row,
                           int *x, int *y, int *w, int *h) {
    int ry;
    if (!row_y(ol, row, &ry) || !ol->items[row].has_value) return 0;
    int fh = field_h();
    *x = ol->x + columns(ol).value;
    *y = ry + (uui_optlist_row_h(ol) - fh) / 2;
    *w = value_col_w(ol);
    *h = fh;
    return 1;
}

// The embedded field sits 1px inside the rounded frame the widget draws,
// with its own square border painted in its background -- so the frame
// is the only edge, and its corners survive the field's square fill.
static int edit_place(struct uui_optlist *ol) {
    int x, y, w, h;
    if (ol->editing < 0 || !uui_optlist_value_rect(ol, ol->editing, &x, &y, &w, &h)) return 0;
    uui_textbox_set_geometry(&ol->edit, x + 2, y + 1, w - 4, h - 2);
    return 1;
}

// --- state ---------------------------------------------------------------

void uui_optlist_init(struct uui_optlist *ol, struct uui_optlist_item *items, int count) {
    memset(ol, 0, sizeof *ol);
    ol->selected = -1;
    ol->hovered = -1;
    uui_sbar_init(&ol->sb);
    ol->editing = -1;
    ol->bg = UUI_COLOR_UNSET;
    ol->fg = UUI_COLOR_UNSET;
    uui_scrollanim_init(&ol->anim);
    uui_textbox_init(&ol->edit, "");
    uui_optlist_set_items(ol, items, count);
    if (count > 0) ol->selected = 0;
}

static void clamp_top(struct uui_optlist *ol) {
    int vis = uui_optlist_visible_rows(ol);
    int max_top = ol->count > vis ? ol->count - vis : 0;
    if (ol->top > max_top) ol->top = max_top;
    if (ol->top < 0) ol->top = 0;
}

void uui_optlist_set_items(struct uui_optlist *ol, struct uui_optlist_item *items, int count) {
    ol->items = items;
    ol->count = count > 0 ? count : 0;
    if (ol->selected >= ol->count) ol->selected = ol->count - 1;
    ol->hovered = -1;
    ol->editing = -1;
    ol->edit_drag = 0;
    clamp_top(ol);
}

static void reveal(struct uui_optlist *ol, int row) {
    int vis = uui_optlist_visible_rows(ol);
    if (row < ol->top) ol->top = row;
    else if (row >= ol->top + vis) ol->top = row - vis + 1;
    clamp_top(ol);
}

int uui_optlist_select(struct uui_optlist *ol, int row) {
    if (row < 0 || row >= ol->count || row == ol->selected) return 0;
    ol->selected = row;
    ol->items[row].changed |= UUI_OPTLIST_CH_SELECT;
    return 1;
}

int uui_optlist_toggle(struct uui_optlist *ol, int row) {
    if (row < 0 || row >= ol->count) return 0;
    ol->items[row].on = !ol->items[row].on;
    ol->items[row].changed |= UUI_OPTLIST_CH_ON;
    return 1;
}

int uui_optlist_begin_edit(struct uui_optlist *ol, int row) {
    if (row < 0 || row >= ol->count || !ol->items[row].has_value) return 0;
    if (ol->editing >= 0 && ol->editing != row) uui_optlist_commit_edit(ol);
    ol->editing = row;
    ol->edit_drag = 0;
    uui_textbox_set_text(&ol->edit, ol->items[row].value);
    uui_textbox_set_active(&ol->edit, 1);
    ol->edit.bare = 1;
    ol->edit.placeholder = ol->items[row].hint;
    uui_textbox_select(&ol->edit, 0, ol->edit.len);
    reveal(ol, row);
    return 1;
}

void uui_optlist_cancel_edit(struct uui_optlist *ol) {
    ol->editing = -1;
    ol->edit_drag = 0;
    uui_textbox_set_active(&ol->edit, 0);
}

void uui_optlist_commit_edit(struct uui_optlist *ol) {
    int row = ol->editing;
    if (row < 0 || row >= ol->count) { uui_optlist_cancel_edit(ol); return; }
    struct uui_optlist_item *it = &ol->items[row];
    const char *v = uui_textbox_text(&ol->edit);
    if (strcmp(v, it->value) != 0) {
        strlcpy(it->value, v, sizeof it->value);
        it->changed |= UUI_OPTLIST_CH_VALUE;
    }
    if (v[0] && !it->on) {
        it->on = 1;
        it->changed |= UUI_OPTLIST_CH_ON;
    }
    uui_optlist_cancel_edit(ol);
}

int uui_optlist_take_change(struct uui_optlist *ol, int *out_row) {
    for (int i = 0; i < ol->count; i++) {
        unsigned c = ol->items[i].changed;
        if (!c) continue;
        ol->items[i].changed = 0;
        if (out_row) *out_row = i;
        return (int)c;
    }
    return 0;
}

// --- drawing -------------------------------------------------------------

// A tick as one anti-aliased polygon: a short stroke down, a long one up.
static void draw_tick(struct ugfx_surface *s, int x, int y, int sz, uint32_t c) {
    int t = sz / 7 > 1 ? sz / 7 : 1;   // stroke thickness
    int x0 = x + sz * 22 / 100, y0 = y + sz * 50 / 100;
    int x1 = x + sz * 42 / 100, y1 = y + sz * 70 / 100;
    int x2 = x + sz * 78 / 100, y2 = y + sz * 28 / 100;
    int xs[6] = { x0, x1, x2, x2, x1, x0 };
    int ys[6] = { y0 - t, y1 - t, y2 - t, y2 + t, y1 + t, y0 + t };
    ugfx_fill_polygon(s, xs, ys, 6, c);
}

static void draw_check(struct ugfx_surface *s, int x, int y, int sz, int on, uint32_t row_bg) {
    if (on) {
        uui_fill_round_rect(s, x, y, sz, sz, 3, UTHEME_ACCENT);
        draw_tick(s, x, y, sz, UTHEME_ACCENT_TEXT);
    } else {
        uui_fill_round_rect(s, x, y, sz, sz, 3, ugfx_blend(UTHEME_OUTLINE, UTHEME_TEXT, 60));
        uui_fill_round_rect(s, x + 1, y + 1, sz - 2, sz - 2, 2,
                            ugfx_blend(row_bg, UTHEME_WHITE, 200));
    }
}

static void draw_value(struct ugfx_surface *s, const struct uui_optlist *ol, int row,
                       uint32_t fg) {
    int x, y, w, h;
    if (!uui_optlist_value_rect(ol, row, &x, &y, &w, &h)) return;
    const struct uui_optlist_item *it = &ol->items[row];
    uint32_t field = UTHEME_WHITE;
    int editing = row == ol->editing;
    uui_fill_round_rect(s, x, y, w, h, 4, editing ? UTHEME_ACCENT : UTHEME_OUTLINE);
    uui_fill_round_rect(s, x + 1, y + 1, w - 2, h - 2, 3, field);
    if (editing) {
        struct uui_optlist *mut = (struct uui_optlist *)ol;  // the field's geometry is the draw's
        edit_place(mut);
        mut->edit.bg = field;
        mut->edit.border = field;
        uui_textbox_draw(s, &ol->edit);
        return;
    }
    // Where the field's own text lands: 2px frame inset + its 4px pad.
    int tx = x + 6, ty = y + (h - ugfx_char_h()) / 2, avail = w - 12;
    if (it->value[0])
        ugfx_draw_string_clipped(s, tx, ty, avail, it->value, fg, field);
    else if (it->hint)
        ugfx_draw_string_clipped(s, tx, ty, avail, it->hint,
                                 uui_state_bg(fg, UUI_STATE_DISABLED), field);
}

void uui_optlist_draw(struct ugfx_surface *s, const struct uui_optlist *ol) {
    uint32_t bg = UUI_COLOR(ol->bg, UTHEME_WHITE);
    uint32_t fg = UUI_COLOR(ol->fg, UTHEME_TEXT);
    int rh = uui_optlist_row_h(ol);
    int vis = uui_optlist_visible_rows(ol);
    int bar = uui_optlist_scrollbar_visible(ol) ? bar_w(ol) : 0;
    struct ol_cols cols = columns(ol);
    int right = text_right(ol);

    ugfx_fill_rect(s, ol->x, ol->y, ol->w, ol->h, bg);

    // The glide: `top` has jumped, the rows are drawn `disp` px from it.
    int disp = uui_scrollanim_sync((struct uui_scrollanim *)&ol->anim, ol->top * rh);
    int extra = uui_scrollanim_extra_rows(disp, rh);
    int first = disp > 0 ? ol->top - extra : ol->top;
    struct ugfx_clip saved;
    ugfx_clip_save(s, &saved);
    ugfx_clip_intersect(s, ol->x, ol->y, ol->w - bar, ol->h);
    for (int i = 0; i < vis + 1 + extra; i++) {
        int row = first + i;
        if (row < 0) continue;
        if (row >= ol->count) break;
        const struct uui_optlist_item *it = &ol->items[row];
        int ry = ol->y + (row - ol->top) * rh + disp;
        int rx = ol->x + OL_INSET, rw = ol->w - bar - 2 * OL_INSET;

        // Selection outranks hover; the selected edge is the focus ring.
        uint32_t row_bg = bg;
        if (row == ol->selected) {
            row_bg = UTHEME_SELECTION;
            uint32_t edge = ol->focused ? UTHEME_ACCENT
                                        : ugfx_blend(UTHEME_WHITE, UTHEME_ACCENT, 130);
            uui_fill_round_rect(s, rx, ry + 1, rw, rh - 2, OL_RADIUS, edge);
            uui_fill_round_rect(s, rx + 1, ry + 2, rw - 2, rh - 4, OL_RADIUS - 1, row_bg);
        } else if (row == ol->hovered) {
            row_bg = uui_state_bg(bg, UUI_STATE_HOVER);
            uui_fill_round_rect(s, rx, ry + 1, rw, rh - 2, OL_RADIUS, row_bg);
        }

        int cs = check_sz();
        draw_check(s, ol->x + cols.check, ry + (rh - cs) / 2, cs, it->on, row_bg);

        int ty = ry + (rh - ugfx_char_h()) / 2;
        int nx = ol->x + cols.name;
        if (it->name && right > nx)
            ugfx_draw_string_clipped(s, nx, ty, right - nx, it->name, fg, row_bg);
        draw_value(s, ol, row, fg);
        int dx = ol->x + cols.desc;
        if (it->desc && right > dx)
            ugfx_draw_string_clipped(s, dx, ty, right - dx, it->desc,
                                     ugfx_blend(fg, row_bg, 110), row_bg);
    }
    ugfx_clip_restore(s, &saved);

    if (bar) {
        // In pixels, so the thumb glides with the rows.
        struct uui_sbar sb = ol->sb;
        uui_sbar_place(&sb, ol->x + ol->w - bar, ol->y, ol->h);
        uui_sbar_set(&sb, ol->count * rh, vis * rh, ol->top * rh - disp);
        uui_sbar_draw(&sb, s, bg, UTHEME_TEXT);
    }

    // A focused list with its selection out of sight still says so.
    int sy;
    if (ol->focused && !(ol->selected >= 0 && row_y(ol, ol->selected, &sy)))
        uui_focus_ring(s, ol->x, ol->y, ol->w, ol->h);
}

void uui_optlist_natural_size(const struct uui_optlist *ol, int *out_w, int *out_h) {
    if (out_w) {
        int desc = 0;
        for (int i = 0; i < ol->count; i++) {
            int tw = ol->items[i].desc ? ugfx_text_width(ol->items[i].desc) : 0;
            if (tw > desc) desc = tw;
        }
        int cap = 32 * ugfx_char_w();
        if (desc > cap) desc = cap;
        *out_w = columns(ol).desc + desc + pad_x() + OL_INSET + bar_w(ol);
    }
    if (out_h) *out_h = ol->count * uui_optlist_row_h(ol);
}

// --- pointer input ---------------------------------------------------------

int uui_optlist_hit(const struct uui_optlist *ol, int cx, int cy) {
    int bar = uui_optlist_scrollbar_visible(ol) ? bar_w(ol) : 0;
    if (!uui_hit(ol->x, ol->y, ol->w - bar, ol->h, cx, cy)) return -1;
    int rh = uui_optlist_row_h(ol);
    // Floor division: mid-glide a row above `top` shows through.
    int rel = cy - ol->y - ol->anim.disp;
    int idx = ol->top + (rel >= 0 ? rel / rh : -((-rel + rh - 1) / rh));
    return idx >= 0 && idx < ol->count ? idx : -1;
}

// The bar in ROWS, filled in before anything asks it.
static struct uui_sbar *sbar(struct uui_optlist *ol) {
    uui_sbar_place(&ol->sb, ol->x + ol->w - bar_w(ol), ol->y,
                   uui_optlist_scrollbar_visible(ol) ? ol->h : 0);
    uui_sbar_set(&ol->sb, ol->count, uui_optlist_visible_rows(ol), ol->top);
    return &ol->sb;
}

static int bar_press(struct uui_optlist *ol, int cx, int cy) {
    int r = uui_sbar_press(sbar(ol), cx, cy);
    if (ol->sb.grab >= 0) uui_scrollanim_cancel(&ol->anim);
    if (r & UUI_SBAR_MOVED) {
        uui_scrollanim_arm(&ol->anim);
        ol->top = ol->sb.top;
    }
    return (r & UUI_SBAR_TOOK) != 0;   // the strip is the bar's, whatever the zone
}

static int ol_press(struct uui_optlist *ol, int cx, int cy, unsigned mods) {
    // In the field being edited a press places the caret; anywhere else
    // it ENDS the edit, keeping what was typed, then does its own thing.
    if (ol->editing >= 0) {
        if (edit_place(ol) && uui_textbox_hit(&ol->edit, cx, cy)) {
            ol->edit_drag = 1;
            return uui_textbox_ops.press(&ol->edit, cx, cy, mods) || 1;
        }
        uui_optlist_commit_edit(ol);
    }
    if (bar_press(ol, cx, cy)) return 1;
    int row = uui_optlist_hit(ol, cx, cy);
    if (row < 0) return uui_hit(ol->x, ol->y, ol->w, ol->h, cx, cy);
    uui_optlist_select(ol, row);
    int x, y, w, h;
    // The checkbox's target runs from the row's edge to the name, full height.
    if (cx < ol->x + columns(ol).name - gap() / 2) {
        uui_optlist_toggle(ol, row);
    } else if (uui_optlist_value_rect(ol, row, &x, &y, &w, &h) &&
               uui_hit(x, y, w, h, cx, cy)) {
        uui_optlist_begin_edit(ol, row);
        edit_place(ol);
        ol->edit_drag = 1;
        uui_textbox_ops.press(&ol->edit, cx, cy, mods);
    }
    return 1;
}

static int ol_motion(struct uui_optlist *ol, int cx, int cy, unsigned buttons) {
    if (buttons) {
        if (ol->edit_drag && ol->editing >= 0 && edit_place(ol))
            return uui_textbox_ops.motion(&ol->edit, cx, cy, buttons);
        if (ol->sb.grab < 0) return 0;
        int r = uui_sbar_motion(sbar(ol), cx, cy, buttons);
        if (r & UUI_SBAR_MOVED) ol->top = ol->sb.top;
        return (r & UUI_SBAR_MOVED) != 0;
    }
    int r = uui_sbar_motion(sbar(ol), cx, cy, 0);   // the bar's hover
    if (r & UUI_SBAR_TOOK) {
        if (ol->hovered != -1) { ol->hovered = -1; r |= UUI_SBAR_REDRAW; }
        return (r & UUI_SBAR_REDRAW) != 0;
    }
    if (r & UUI_SBAR_REDRAW) { ol->hovered = uui_optlist_hit(ol, cx, cy); return 1; }
    int row = uui_optlist_hit(ol, cx, cy);
    if (row == ol->hovered) return 0;
    ol->hovered = row;
    return 1;
}

int uui_optlist_wheel(struct uui_optlist *ol, int notches) {
    int before = ol->top;
    uui_scrollanim_arm(&ol->anim);
    ol->top -= notches * 3;   // navigation, never a selection change
    clamp_top(ol);
    return ol->top != before;
}

// --- keys --------------------------------------------------------------------

int uui_optlist_key(struct uui_optlist *ol, int key, unsigned mods) {
    if (ol->editing >= 0) {
        if (key == '\n' || key == '\r') uui_optlist_commit_edit(ol);
        else if (key == 0x1B) uui_optlist_cancel_edit(ol);
        else uui_textbox_key_mods(&ol->edit, key, mods);
        return 1;   // the field is modal while it is open
    }
    if (ol->count <= 0) return 0;
    int sel = ol->selected < 0 ? 0 : ol->selected;
    int page = uui_optlist_visible_rows(ol);
    if (key == ' ') return uui_optlist_toggle(ol, sel) | uui_optlist_select(ol, sel);
    if (key == '\n' || key == '\r') {
        uui_optlist_select(ol, sel);
        return uui_optlist_begin_edit(ol, sel);
    }
    int to;
    if (key == KEY_ARROW_UP)        to = sel - 1;
    else if (key == KEY_ARROW_DOWN) to = sel + 1;
    else if (key == KEY_HOME)       to = 0;
    else if (key == KEY_END)        to = ol->count - 1;
    else if (key == KEY_PAGE_UP)    to = sel - page;
    else if (key == KEY_PAGE_DOWN)  to = sel + page;
    else return 0;
    if (to < 0) to = 0;
    if (to >= ol->count) to = ol->count - 1;
    uui_scrollanim_arm(&ol->anim);
    int changed = uui_optlist_select(ol, to);
    reveal(ol, to);
    return changed;
}

// --- the ops table ---------------------------------------------------------

static void op_natural_size(const void *w, int *ow, int *oh) {
    uui_optlist_natural_size((const struct uui_optlist *)w, ow, oh);
}

static void op_set_geometry(void *w, int x, int y, int width, int height) {
    struct uui_optlist *ol = (struct uui_optlist *)w;
    ol->x = x; ol->y = y; ol->w = width; ol->h = height;
    clamp_top(ol);
}

static void op_bounds(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_optlist *ol = (const struct uui_optlist *)w;
    *x = ol->x; *y = ol->y; *ow = ol->w; *oh = ol->h;
}

static void op_draw(struct ugfx_surface *s, const void *w) {
    uui_optlist_draw(s, (const struct uui_optlist *)w);
}

// The WHOLE rect, scrollbar included: uui_optlist_hit() answers "which
// row" and excludes the bar, and the router gates press and wheel on this.
static int op_hit(const void *w, int cx, int cy) {
    const struct uui_optlist *ol = (const struct uui_optlist *)w;
    return uui_hit(ol->x, ol->y, ol->w, ol->h, cx, cy);
}

static int op_key(void *w, int key, unsigned mods) {
    return uui_optlist_key((struct uui_optlist *)w, key, mods);
}

static int op_accepts_focus(const void *w) {
    return ((const struct uui_optlist *)w)->count > 0;
}

// Focus leaving commits an edit, as it does a spinbox's.
static void op_set_focused(void *w, int focused) {
    struct uui_optlist *ol = (struct uui_optlist *)w;
    ol->focused = focused;
    if (!focused && ol->editing >= 0) uui_optlist_commit_edit(ol);
}

static int op_press(void *w, int cx, int cy, unsigned mods) {
    return ol_press((struct uui_optlist *)w, cx, cy, mods);
}

static int op_motion(void *w, int cx, int cy, unsigned buttons) {
    return ol_motion((struct uui_optlist *)w, cx, cy, buttons);
}

// Present so the router names this widget to its app (uui_route.c).
static int op_release(void *w, int cx, int cy) {
    (void)cx; (void)cy;
    struct uui_optlist *ol = (struct uui_optlist *)w;
    uui_sbar_release(&ol->sb);
    ol->edit_drag = 0;
    return 1;
}

static int op_wheel(void *w, int notches) {
    return uui_optlist_wheel((struct uui_optlist *)w, notches);
}

static int op_cursor(const void *w, int cx, int cy) {
    const struct uui_optlist *ol = (const struct uui_optlist *)w;
    int row = uui_optlist_hit(ol, cx, cy), x, y, fw, fh;
    if (row >= 0 && uui_optlist_value_rect(ol, row, &x, &y, &fw, &fh) &&
        uui_hit(x, y, fw, fh, cx, cy))
        return WIN_CURSOR_TEXT;
    return WIN_CURSOR_DEFAULT;
}

// Each row on screen reports its checkbox and field, so a test clicks
// them by asking rather than re-deriving the columns.
static void op_describe(const void *w, const struct uui_describe *d) {
    const struct uui_optlist *ol = (const struct uui_optlist *)w;
    uui_describe_int(d, "row_h", uui_optlist_row_h(ol));
    uui_describe_int(d, "selected", ol->selected);
    uui_describe_int(d, "editing", ol->editing);
    uui_describe_int(d, "top", ol->top);
    for (int r = 0; r < ol->count; r++) {
        int x, y, fw, fh;
        if (uui_optlist_check_rect(ol, r, &x, &y, &fw, &fh))
            uui_describe_rect_i(d, "check", r, x, y, fw, fh);
        if (uui_optlist_value_rect(ol, r, &x, &y, &fw, &fh))
            uui_describe_rect_i(d, "value", r, x, y, fw, fh);
    }
}

// The inner field's, for the shared edit menu (ui/uui_widget.h).
static int optlist_edit_target(void *w, int cx, int cy, struct uui_edit_target *out) {
    struct uui_optlist *p = (struct uui_optlist *)w;
    if (!p->editing) return 0;   // the field shows only then
    return uui_textbox_edit_target(&p->edit, cx, cy, out);
}

const struct uui_widget_ops uui_optlist_ops = {
    .natural_size  = op_natural_size,
    .set_geometry  = op_set_geometry,
    .bounds        = op_bounds,
    .draw          = op_draw,
    .hit           = op_hit,
    .key           = op_key,
    .set_focused   = op_set_focused,
    .accepts_focus = op_accepts_focus,
    .press         = op_press,
    .motion        = op_motion,
    .release       = op_release,
    .wheel         = op_wheel,
    .cursor        = op_cursor,
    .describe      = op_describe,
    .edit_target = optlist_edit_target,
};
