// dialog -- see ui/uui_dialog.h for why a client draws its own.
#include "ui/uui_dialog.h"
#include "ui/utheme.h"
#include "keyboard.h"
#include <stddef.h>

// Everything here is FONT-DERIVED (docs/gui-guidelines.md), so the box
// grows with the session font instead of clipping its own text at a
// larger one.
static int line_h(void) { return ugfx_char_h() + utheme_gap(); }
static int btn_h(void) { return utheme_control_h(); }

static int btn_w(const struct uui_dialog *d) {
    // ONE WIDTH FOR ALL OF THEM, sized to the widest label: buttons of
    // differing widths in one row read as an accident, and every real
    // toolkit equalises them.
    int widest = ugfx_char_w() * 6;
    for (int i = 0; i < d->button_count; i++) {
        int w = ugfx_text_width(d->buttons[i].label);
        if (w > widest) widest = w;
    }
    int want = widest + utheme_pad() * 2;

    // ...BUT THE ROW MUST FIT THE BOX, and the box must fit the window.
    // Six buttons at their natural width overflowed a 720px window, and
    // the row is right-aligned -- so the FIRST button went off the left
    // edge and drew as "write". A dialog that clips its own first button
    // is worse than one with narrow buttons.
    if (d->button_count > 0 && d->bw > 0) {
        int gap = utheme_gap(), pad = utheme_pad();
        int room = d->bw - gap * 2 - pad * 2 - gap * (d->button_count - 1);
        int fits = room / d->button_count;
        if (fits > 0 && want > fits) want = fits;
    }
    return want;
}

static void layout(struct uui_dialog *d) {
    int pad = utheme_pad(), gap = utheme_gap();
    int bw = btn_w(d);

    int widest = ugfx_text_width(d->title ? d->title : "");
    for (int i = 0; i < d->row_count; i++) {
        int w = ugfx_text_width(d->rows[i] ? d->rows[i] : "");
        if (w > widest) widest = w;
    }
    int row_w = d->button_count * bw + (d->button_count - 1) * gap;
    if (row_w > widest) widest = row_w;

    d->w = widest + pad * 2;
    if (d->w > d->bw - gap * 2) d->w = d->bw - gap * 2;
    d->h = pad * 2 + line_h() * (d->row_count + 1) + gap + btn_h();

    d->x = d->bx + (d->bw - d->w) / 2;
    d->y = d->by + (d->bh - d->h) / 2;
    if (d->y < d->by) d->y = d->by;
}

// Button `i`'s rect. ONE calculation, shared by the draw and the hit
// test -- two copies of this arithmetic drift, and the symptom is a
// click landing on the button beside the one under the cursor.
static void button_rect(const struct uui_dialog *d, int i,
                         int *x, int *y, int *w, int *h) {
    int pad = utheme_pad(), gap = utheme_gap();
    int bw = btn_w(d), bh = btn_h();
    int total = d->button_count * bw + (d->button_count - 1) * gap;
    int x0 = d->x + d->w - pad - total;   // right-aligned, as everywhere
    *x = x0 + i * (bw + gap);
    *y = d->y + d->h - pad - bh;
    *w = bw;
    *h = bh;
}

void uui_dialog_init(struct uui_dialog *d) {
    for (unsigned i = 0; i < sizeof *d; i++) ((unsigned char *)d)[i] = 0;
    d->committed = -1;
    d->hot = -1;
    d->pressed = -1;
    d->default_button = -1;
    d->cancel_code = -1;
}

void uui_dialog_set_bounds(struct uui_dialog *d, int x, int y, int w, int h) {
    d->bx = x; d->by = y; d->bw = w; d->bh = h;
    if (d->open) layout(d);
}

void uui_dialog_open(struct uui_dialog *d, const char *title,
                      const char *const *rows, int row_count,
                      const struct uui_dialog_button *buttons, int count,
                      int default_button, int cancel_code) {
    d->title = title;
    d->row_count = row_count < UUI_DIALOG_ROWS ? row_count : UUI_DIALOG_ROWS;
    for (int i = 0; i < d->row_count; i++) d->rows[i] = rows[i];
    d->button_count = count < UUI_DIALOG_BUTTONS ? count : UUI_DIALOG_BUTTONS;
    for (int i = 0; i < d->button_count; i++) d->buttons[i] = buttons[i];
    d->default_button = default_button;
    d->cancel_code = cancel_code;
    d->committed = -1;
    d->pressed = -1;
    // The default starts hot, so Return works before the pointer has
    // been anywhere near the box.
    d->hot = default_button;
    d->open = 1;
    layout(d);
}

void uui_dialog_close(struct uui_dialog *d) {
    d->open = 0;
    d->pressed = -1;
}

int uui_dialog_is_open(const struct uui_dialog *d) { return d->open; }

int uui_dialog_take_code(struct uui_dialog *d) {
    int c = d->committed;
    d->committed = -1;
    return c;
}

void uui_dialog_draw(struct ugfx_surface *s, const struct uui_dialog *d) {
    if (!d->open) return;
    int pad = utheme_pad();

    ugfx_fill_rect(s, d->x, d->y, d->w, d->h, UTHEME_WINDOW_BG);
    ugfx_draw_rect(s, d->x, d->y, d->w, d->h, UTHEME_BORDER);

    int y = d->y + pad;
    if (d->title) {
        const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        ugfx_draw_string_clipped(s, d->x + pad, y, d->w - pad * 2, d->title,
                                  UTHEME_TEXT, UTHEME_WINDOW_BG);
        ugfx_set_font(was);
    }
    y += line_h();
    for (int i = 0; i < d->row_count; i++) {
        if (d->rows[i])
            // Clipped, always: these rows carry PATHS, which are longer
            // than any box routinely (docs/gui-guidelines.md's oldest
            // trap).
            ugfx_draw_string_clipped(s, d->x + pad, y, d->w - pad * 2,
                                      d->rows[i], UTHEME_TEXT, UTHEME_WINDOW_BG);
        y += line_h();
    }

    for (int i = 0; i < d->button_count; i++) {
        int bx, by, bw, bh;
        button_rect(d, i, &bx, &by, &bw, &bh);
        enum uui_state st = (d->pressed == i) ? UUI_STATE_PRESSED
                           : (d->hot == i)     ? UUI_STATE_HOVER
                                               : UUI_STATE_REST;
        ugfx_fill_rect(s, bx, by, bw, bh, uui_state_bg(UTHEME_BUTTON_BG, st));
        int tw = ugfx_text_width(d->buttons[i].label);
        ugfx_draw_string_clipped(s, bx + (bw - tw) / 2,
                                  by + (bh - ugfx_char_h()) / 2, bw,
                                  d->buttons[i].label, UTHEME_TEXT,
                                  uui_state_bg(UTHEME_BUTTON_BG, st));
        if (d->hot == i) uui_focus_ring(s, bx, by, bw, bh);
    }
}

// --- input ------------------------------------------------------------

static int button_at(const struct uui_dialog *d, int cx, int cy) {
    for (int i = 0; i < d->button_count; i++) {
        int x, y, w, h;
        button_rect(d, i, &x, &y, &w, &h);
        if (uui_hit(x, y, w, h, cx, cy)) return i;
    }
    return -1;
}

static void commit(struct uui_dialog *d, int index) {
    if (index < 0 || index >= d->button_count) return;
    d->committed = d->buttons[index].code;
    uui_dialog_close(d);
}

int uui_dialog_key(struct uui_dialog *d, int key) {
    if (!d->open) return 0;
    switch (key) {
    case 0x1B:   // Esc answers the cancel code, whatever it is
        d->committed = d->cancel_code;
        uui_dialog_close(d);
        return 1;
    case '\r':
    case '\n':
        commit(d, d->hot >= 0 ? d->hot : d->default_button);
        return 1;
    case KEY_ARROW_LEFT:
        if (d->hot > 0) d->hot--;
        return 1;
    case KEY_ARROW_RIGHT:
    case '\t':
        if (d->hot < d->button_count - 1) d->hot++;
        else d->hot = 0;
        return 1;
    default:
        // EVERY OTHER KEY IS SWALLOWED. A modal that let a keystroke
        // through to whatever is behind it is not a modal -- and behind
        // this one is a file listing where a letter seeks and Delete
        // deletes.
        return 1;
    }
}

// --- the ops table ----------------------------------------------------

static void dlg_natural(const void *w, int *out_w, int *out_h) {
    const struct uui_dialog *d = w;
    // It places itself; a layout must reserve nothing for it.
    (void)d;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
}
static void dlg_geometry(void *w, int x, int y, int width, int height) {
    uui_dialog_set_bounds((struct uui_dialog *)w, x, y, width, height);
}
static void dlg_bounds(const void *w, int *x, int *y, int *out_w, int *out_h) {
    const struct uui_dialog *d = w;
    if (x) *x = d->x;
    if (y) *y = d->y;
    if (out_w) *out_w = d->open ? d->w : 0;
    if (out_h) *out_h = d->open ? d->h : 0;
}
static void dlg_draw_overlay(struct ugfx_surface *s, const void *w) {
    uui_dialog_draw(s, (const struct uui_dialog *)w);
}
static int dlg_overlay_active(const void *w) {
    return ((const struct uui_dialog *)w)->open;
}
static int dlg_hit(const void *w, int cx, int cy) {
    const struct uui_dialog *d = w;
    return d->open && uui_hit(d->x, d->y, d->w, d->h, cx, cy);
}
static int dlg_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_dialog *d = w;
    if (!d->open) return 0;
    d->pressed = button_at(d, cx, cy);
    d->hot = d->pressed >= 0 ? d->pressed : d->hot;
    // CONSUMED WHEREVER IT LANDED, including outside the box: that is
    // what makes it modal. Clicking outside does NOT dismiss -- these
    // dialogs ask questions whose default answer is not obvious, and
    // a stray click is not an answer.
    return 1;
}
static int dlg_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_dialog *d = w;
    if (!d->open) return 0;
    (void)buttons;
    int over = button_at(d, cx, cy);
    if (over == d->hot) return 0;
    d->hot = over >= 0 ? over : d->hot;
    return 1;
}
static int dlg_release(void *w, int cx, int cy) {
    struct uui_dialog *d = w;
    if (!d->open) return 1;
    // COMMITS ON RELEASE, and only if the release is on the button the
    // press armed -- so a press dragged off commits nothing, which is
    // this GUI's rule for every control.
    int over = button_at(d, cx, cy);
    if (d->pressed >= 0 && over == d->pressed) commit(d, over);
    d->pressed = -1;
    return 1;
}
static int dlg_key(void *w, int key, unsigned mods) {
    (void)mods;
    return uui_dialog_key((struct uui_dialog *)w, key);
}
static int dlg_accepts_focus(const void *w) {
    return ((const struct uui_dialog *)w)->open;
}
static void dlg_set_focused(void *w, int focused) { (void)w; (void)focused; }

// FILLED AGAINST uui_widget.h, not against the widget this was modelled
// on: a copied table inherits its gaps.
const struct uui_widget_ops uui_dialog_ops = {
    .natural_size    = dlg_natural,
    .set_geometry    = dlg_geometry,
    .bounds          = dlg_bounds,
    .draw_overlay    = dlg_draw_overlay,
    .overlay_active  = dlg_overlay_active,
    .hit             = dlg_hit,
    .press           = dlg_press,
    .motion          = dlg_motion,
    .release         = dlg_release,
    .key             = dlg_key,
    .accepts_focus   = dlg_accepts_focus,
    .set_focused     = dlg_set_focused,
};
