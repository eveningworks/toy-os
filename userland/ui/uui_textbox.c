// field. Split out of uwidgets.c -- see ui/uui_textbox.h.
#include "ui/uui_textbox.h"
#include "ui/uui_caret.h"
#include "ui/uui_widget.h"  // the ops tables at the bottom of this file
#include "keyboard.h" // KEY_* codes, as delivered by WIN_EV_KEY

// Caret width in pixels -- a bar, not a block, so it sits between
// characters rather than covering one.
#define CARET_W 2

// Inset around the text inside the box. Shared by the draw and by
// uui_textbox_natural_size(), so the height reported and the height drawn
// cannot disagree.
#define UUI_TEXTBOX_PAD 4

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------

// --- the edit core's view of this field -------------------------------
//
// Four small functions are all a 48-byte line needs to become editable
// text with a full keymap (ui/uui_edit.h). The line ops stay NULL: a
// single-line field genuinely has no line above, so Up and Down are
// declined rather than silently swallowed.
static int tb_len(void *t) { return ((struct uui_textbox *)t)->len; }

static char tb_at(void *t, int i) {
    struct uui_textbox *f = (struct uui_textbox *)t;
    return (i >= 0 && i < f->len) ? f->buf[i] : 0;
}

static int tb_insert(void *t, int i, char c) {
    struct uui_textbox *f = (struct uui_textbox *)t;
    if (f->len >= UUI_TEXTBOX_MAX - 1) return 0; // full: a normal answer
    if (i < 0 || i > f->len) return 0;
    for (int k = f->len; k > i; k--) f->buf[k] = f->buf[k - 1];
    f->buf[i] = c;
    f->len++;
    f->buf[f->len] = '\0';
    return 1;
}

static void tb_erase(void *t, int start, int end) {
    struct uui_textbox *f = (struct uui_textbox *)t;
    if (start < 0) start = 0;
    if (end > f->len) end = f->len;
    if (start >= end) return;
    int n = end - start;
    for (int k = start; k + n <= f->len; k++) f->buf[k] = f->buf[k + n];
    f->len -= n;
    f->buf[f->len] = '\0';
}

static const struct uui_edit_ops TB_EDIT_OPS = {
    .len = tb_len,
    .at = tb_at,
    .insert = tb_insert,
    .erase = tb_erase,
};

void uui_textbox_init(struct uui_textbox *f, const char *initial) {
    int i = 0;
    if (initial) {
        while (initial[i] && i < UUI_TEXTBOX_MAX - 1) { f->buf[i] = initial[i]; i++; }
    }
    f->buf[i] = '\0';
    f->len = i;
    uui_edit_init(&f->ed);
    f->ed.cursor = i;
    f->active = 0;
    f->disabled = 0;
    f->placeholder = 0;
    // Left UNSET so the theme answers at DRAW time -- see utheme.h.
    // These were four literals that happened to equal the default
    // palette, which meant a theme change reached everything except the
    // widgets nobody had overridden.
    f->bg = UUI_COLOR_UNSET;
    f->fg = UUI_COLOR_UNSET;
    f->border = UUI_COLOR_UNSET;
    f->sel_bg = UUI_COLOR_UNSET;
}

// THE TEXT, WITHOUT THE REST OF init(). A caller that re-inits a field
// only to change what it says also clears `active` -- and a field the
// focus ring still points AT cannot be revived by clicking it, because
// uui_focus_click() returns early on the index it already holds. That
// left the Save As name field dead whenever the pointer crossed the
// chooser's Places strip. Geometry, colours and `disabled` are the
// caller's and are left alone; the caret goes to the end, as after a
// programmatic fill.
void uui_textbox_select(struct uui_textbox *f, int start, int end) {
    if (start < 0) start = 0;
    if (end > f->len) end = f->len;
    if (start > end) start = end;
    f->ed.sel_anchor = start;
    f->ed.cursor = end;
    f->ed.sel_active = start != end;
}

void uui_textbox_set_text(struct uui_textbox *f, const char *text) {
    int i = 0;
    if (text) {
        while (text[i] && i < UUI_TEXTBOX_MAX - 1) { f->buf[i] = text[i]; i++; }
    }
    f->buf[i] = '\0';
    f->len = i;
    uui_edit_init(&f->ed);
    f->ed.cursor = i;
}

const char *uui_textbox_text(const struct uui_textbox *f) { return f->buf; }

int uui_textbox_has_selection(const struct uui_textbox *f) {
    return uui_edit_has_selection(&f->ed);
}

void uui_textbox_set_active(struct uui_textbox *f, int active) { f->active = active ? 1 : 0; }

// (field_insert()/field_delete() lived here. They are tb_insert() and
// tb_erase() above now -- the same two operations, expressed as the
// edit core's accessors so the keymap that drives them is shared with
// the multi-line editor rather than written twice.)

void uui_textbox_natural_size(const struct uui_textbox *f, int *out_w, int *out_h) {
    (void)f;
    if (out_w) *out_w = 0;                             // no preference
    if (out_h) *out_h = ugfx_char_h() + 2 * UUI_TEXTBOX_PAD; // and this one is real
}

// The pixels the text gets: the box less its padding, and less the
// caret when there is one -- a caret at the very end of a full value
// has to land inside the field rather than on its border.
static int field_avail(const struct uui_textbox *f) {
    int avail = f->w - 2 * UUI_TEXTBOX_PAD - (f->active ? CARET_W : 0);
    return avail > 0 ? avail : 0;
}

// How far the visible window has slid right, as a character INDEX
// chosen by MEASURING -- a proportional face has no column to count, so
// this walks left from the caret while the text between the two still
// fits. Shared by draw() and by uui_textbox_index_at_x() below, so a
// click lands on the character that is actually drawn there -- two
// copies of this arithmetic is the classic way a caret ends up one
// glyph off.
//
// Re-measuring the slice per step is O(len^2), which is 64 characters
// squared and invisible; measuring forward from a running total would
// be wrong, because the kerning of a slice depends on where it starts.
static int field_window_start(const struct uui_textbox *f, int avail) {
    if (!f->active || ugfx_text_width(f->buf) <= avail) return 0;
    int start = f->ed.cursor;
    while (start > 0
           && ugfx_text_width_n(f->buf + start - 1, f->ed.cursor - start + 1) <= avail)
        start--;
    return start;
}

int uui_textbox_index_at_x(const struct uui_textbox *f, int cx) {
    int start = field_window_start(f, field_avail(f));
    int rel = cx - (f->x + UUI_TEXTBOX_PAD);
    if (rel < 0) rel = 0;
    int idx = start + ugfx_text_index_at_x(f->buf + start, rel);
    if (idx > f->len) idx = f->len;
    return idx;
}

int uui_textbox_key_mods(struct uui_textbox *f, int key, unsigned mods) {
    if (!f->active) return 0;
    // The whole keymap -- Ctrl+A, Shift+arrows, backspace-deletes-the-
    // selection, typing-replaces-it -- lives in the edit core, shared
    // with the multi-line editor. This widget used to carry its own,
    // which had none of that: the caret moved with arrows and nothing
    // else, so you could not select anything in a text field at all.
    return uui_edit_key(&f->ed, &TB_EDIT_OPS, f, key, mods);
}

int uui_textbox_key(struct uui_textbox *f, int key) {
    return uui_textbox_key_mods(f, key, 0);
}

void uui_textbox_set_geometry(struct uui_textbox *f, int x, int y, int w, int h) {
    f->x = x; f->y = y; f->w = w; f->h = h;
}

int uui_textbox_hit(const struct uui_textbox *f, int cx, int cy) {
    return uui_hit(f->x, f->y, f->w, f->h, cx, cy);
}

uint32_t uui_textbox_c_bg(const struct uui_textbox *f)
{ return UUI_COLOR(f->bg, UTHEME_WHITE); }
uint32_t uui_textbox_c_fg(const struct uui_textbox *f)
{ return UUI_COLOR(f->fg, UTHEME_TEXT); }
uint32_t uui_textbox_c_border(const struct uui_textbox *f)
{ return UUI_COLOR(f->border, UTHEME_OUTLINE); }

void uui_textbox_draw(struct ugfx_surface *s, const struct uui_textbox *f) {
    uint32_t bg = uui_textbox_c_bg(f);
    uint32_t fg = uui_textbox_c_fg(f);
    uint32_t border = uui_textbox_c_border(f);
    if (f->disabled) {
        // Derived from the field's OWN colours, never hand-picked --
        // uui_primitives.h's rule, and what keeps a greyed field greyed
        // on a dark theme as well as a light one.
        bg = uui_state_bg(bg, UUI_STATE_DISABLED);
        fg = uui_state_bg(fg, UUI_STATE_DISABLED);
        border = uui_state_bg(border, UUI_STATE_DISABLED);
    }
    int x = f->x, y = f->y, w = f->w, h = f->h;
    ugfx_fill_rect(s, x, y, w, h, bg);
    ugfx_draw_rect(s, x, y, w, h, border);

    int pad = UUI_TEXTBOX_PAD;
    int ty = y + (h - ugfx_char_h()) / 2;

    // Horizontal windowing: how much text fits, and how far right the
    // window has to slide to keep the caret inside it. Without this a
    // long value draws straight through the border -- a bug the kernel
    // widget actually had.
    int avail = field_avail(f);
    int start = field_window_start(f, avail);

    char shown[UUI_TEXTBOX_MAX];
    int n = ugfx_text_fit_chars(f->buf + start, avail);
    for (int i = 0; i < n; i++) shown[i] = f->buf[start + i];
    shown[n] = '\0';
    // The SELECTION, behind the glyphs. Drawn before the text and
    // clipped to the visible window, so a selection running off either
    // edge shows as far as the field does rather than painting over the
    // border. A selection nobody can see is not a selection.
    if (f->active && uui_edit_has_selection(&f->ed)) {
        int a, b;
        uui_edit_range(&f->ed, &a, &b);
        a -= start; b -= start;
        if (a < 0) a = 0;
        if (b > n) b = n;
        if (b > a) {
            int ax = ugfx_text_width_n(shown, a);
            ugfx_fill_rect(s, x + pad + ax, ty,
                            ugfx_text_width_n(shown, b) - ax,
                            ugfx_char_h(), UUI_COLOR(f->sel_bg, UTHEME_SELECTION));
        }
    }

    // Clipped as a safety net, not because the slice is expected to be
    // wrong: if the windowing ever miscomputes, text stops at the edge
    // rather than drawing through the border.
    ugfx_draw_string_clipped(s, x + pad, ty, w - 2 * pad, shown, fg, bg);
    if (!f->len && f->placeholder)
        ugfx_draw_string_clipped(s, x + pad + CARET_W + 1, ty, w - 2 * pad, f->placeholder,
                                 uui_state_bg(fg, UUI_STATE_DISABLED), bg);

    if (f->active) {
        if (uui_caret_visible())
            ugfx_fill_rect(s, x + pad + ugfx_text_width_n(shown, f->ed.cursor - start),
                            ty, CARET_W, ugfx_char_h(), fg);
        // The border too, not the caret alone: the caret says WHERE the
        // next character lands, the ring says WHICH control is listening,
        // and a caret 200px away is easy to miss. Over the border rather
        // than inside it, so the field does not appear to shrink.
        uui_focus_ring(s, x, y, w, h);
    }
}

// --- focus ------------------------------------------------------------
//
// A FOCUS-ONLY ops table: hit/key/set_focused/accepts_focus, with the
// drawing and layout slots left NULL. Every slot is optional (see
// uui_widget.h), and a field's draw takes three colours the generic
// signature has nowhere to carry -- so this table is what uui_focus
// needs and nothing more, rather than a half-honest full one.
// EVERY interactive slot checks `disabled`, the same sweep uui_spinbox
// makes: a field is reachable by pointer AND by the focus ring, so
// refusing in one of them leaves the other way in.
static int ops_hit(const void *w, int cx, int cy) {
    if (((const struct uui_textbox *)w)->disabled) return 0;
    return uui_textbox_hit((const struct uui_textbox *)w, cx, cy);
}
static int ops_key(void *w, int key, unsigned mods) {
    if (((struct uui_textbox *)w)->disabled) return 0;
    return uui_textbox_key_mods((struct uui_textbox *)w, key, mods);
}
static void ops_set_focused(void *w, int focused) {
    uui_textbox_set_active((struct uui_textbox *)w, focused);
}
static int ops_accepts_focus(const void *w) {
    return !((const struct uui_textbox *)w)->disabled;
}

const struct uui_widget_ops uui_textbox_focus_ops = {
    .hit = ops_hit,
    .key = ops_key,
    .set_focused = ops_set_focused,
    .accepts_focus = ops_accepts_focus,
};

// --- routed pointer input (ui/uui_route.h) ----------------------------
//
// A press PLACES THE CARET, which is what every text field on every
// desktop does and what this one could not do at all: the caret moved
// only with arrow keys, so clicking into the middle of a value did
// nothing and you had to walk there. The character index comes from the
// same horizontal-window arithmetic draw() uses (see caret_index()), so
// clicking a glyph puts the caret at that glyph rather than near it.
static int tb_ops_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    (void)cy;
    struct uui_textbox *f = (struct uui_textbox *)w;
    if (f->disabled) return 0;
    // Places the caret AND collapses any selection, which is what a
    // plain click does everywhere.
    uui_edit_place(&f->ed, &TB_EDIT_OPS, f, uui_textbox_index_at_x(f, cx), 0);
    return 1;
}

// Dragging from that press EXTENDS the selection -- the pointer grab
// (ui/uui_route.h) is what delivers these once the cursor has left the
// field, so a drag that runs past the end keeps selecting instead of
// stopping at the border.
static int tb_ops_motion(void *w, int cx, int cy, unsigned buttons) {
    (void)cy;
    if (!buttons) return 0;
    struct uui_textbox *f = (struct uui_textbox *)w;
    if (f->disabled) return 0;
    uui_edit_place(&f->ed, &TB_EDIT_OPS, f, uui_textbox_index_at_x(f, cx), 1);
    return 1;
}

static void tb_ops_draw(struct ugfx_surface *s, const void *w) {
    uui_textbox_draw(s, (const struct uui_textbox *)w);
}

static void te_ops_natural_size(const void *w, int *out_w, int *out_h) {
    uui_textbox_natural_size((const struct uui_textbox *)w, out_w, out_h);
}

static void te_ops_set_geometry(void *w, int x, int y, int width, int height) {
    uui_textbox_set_geometry((struct uui_textbox *)w, x, y, width, height);
}

static int te_ops_release(void *w, int cx, int cy) {
    (void)w; (void)cx; (void)cy;
    // Nothing to undo -- a textbox has no drag state of its own to end.
    // It is here purely so the router NAMES this widget to its app: the
    // router reports a widget only when it has a release op, so without
    // this a textbox took clicks and keys while the app was never told
    // anything had happened.
    return 1;
}

// NATURAL_SIZE AND SET_GEOMETRY were missing until 2026-08-19, so a
// uui_textbox declared in a uui_layout was never positioned or measured
// -- it stayed at a zero rect and the layout could not size it. Both
// functions already existed; only the table was short. Three other
// widgets had the same gap the same day; tools/check_widget_ops.py
// exists to stop a fourth. See docs/decisions.md.
static void textbox_bounds_op(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_textbox *c = w;
    *x = c->x; *y = c->y; *ow = c->w; *oh = c->h;
}

// No coordinates consulted: a field is uniformly a place text goes.
static int tb_ops_cursor(const void *w, int cx, int cy) {
    (void)w; (void)cx; (void)cy;
    return WIN_CURSOR_TEXT;
}

const struct uui_widget_ops uui_textbox_ops = {
    .bounds = textbox_bounds_op,
    .natural_size = te_ops_natural_size,
    .set_geometry = te_ops_set_geometry,
    .release = te_ops_release,
    .draw          = tb_ops_draw,
    .hit           = ops_hit,
    .key           = ops_key,
    .set_focused   = ops_set_focused,
    .accepts_focus = ops_accepts_focus,
    .press         = tb_ops_press,
    .motion        = tb_ops_motion,
    .cursor        = tb_ops_cursor,
};
