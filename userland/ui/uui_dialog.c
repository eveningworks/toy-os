// dialog -- see ui/uui_dialog.h for why a client draws its own.
#include "ui/uui_dialog.h"
#include "ui/utheme.h"
#include "keyboard.h"
#include "ui/uui_label.h"   // uui_label_wrap_next -- the note
#include "lib/uimg.h"
#include <stddef.h>

// Everything here is FONT-DERIVED (docs/gui-guidelines.md), so the box
// grows with the session font instead of clipping its own text at a
// larger one.
//
// THE LOOK IS A CARD over a dimmed window -- Windows 11's ContentDialog
// and KDE's: rounded, shadowed, generous padding, the question in bold,
// the buttons right-aligned with the answer that matters coloured.
static int line_h(void) { return ugfx_char_h() + utheme_gap(); }
static int btn_h(void) { return utheme_control_h() + 4; }
static int pad(void)   { return utheme_pad() * 2; }
static int pic_px(void) { return ugfx_char_h() * 4; }   // the picture's box

static int btn_w(const struct uui_dialog *d) {
    // ONE WIDTH FOR ALL OF THEM, sized to the widest label: buttons of
    // differing widths in one row read as an accident, and every real
    // toolkit equalises them.
    int widest = ugfx_char_w() * 6;
    for (int i = 0; i < d->button_count; i++) {
        int w = ugfx_text_width(d->buttons[i].label);
        if (w > widest) widest = w;
    }
    int want = widest + utheme_pad() * 3;

    // ...BUT THE ROW MUST FIT THE BOX, and the box must fit the window.
    // Six buttons at their natural width overflowed a 720px window, and
    // the row is right-aligned -- so the FIRST button went off the left
    // edge and drew as "write". A dialog that clips its own first button
    // is worse than one with narrow buttons.
    if (d->button_count > 0 && d->bw > 0) {
        int gap = utheme_gap();
        int room = d->bw - gap * 2 - pad() * 2 - gap * (d->button_count - 1);
        int fits = room / d->button_count;
        if (fits > 0 && want > fits) want = fits;
    }
    return want;
}

// Where the title and rows start: right of the picture when there is one.
static int text_x(const struct uui_dialog *d) {
    return d->x + pad() + (d->picture ? pic_px() + pad() * 3 / 4 : 0);
}
static int note_lines(const struct uui_dialog *d) { return d->note ? 2 : 0; }
// The text block: the title and the rows, or the picture if taller.
static int head_h(const struct uui_dialog *d) {
    int h = line_h() * (d->row_count + 1);
    if (d->picture && pic_px() > h) h = pic_px();
    return h;
}
static int note_h(const struct uui_dialog *d) {
    return d->note ? utheme_gap() * 2 + note_lines(d) * line_h() : 0;
}

static void layout(struct uui_dialog *d) {
    int gap = utheme_gap();
    int bw = btn_w(d);
    int pic = d->picture ? pic_px() + pad() * 3 / 4 : 0;

    const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
    int widest = ugfx_text_width(d->title ? d->title : "") + pic;
    ugfx_set_font(was);
    for (int i = 0; i < d->row_count; i++) {
        int w = ugfx_text_width(d->rows[i] ? d->rows[i] : "") + pic;
        if (w > widest) widest = w;
    }
    int row_w = d->button_count * bw + (d->button_count - 1) * gap;
    if (row_w > widest) widest = row_w;
    // Never a sliver: a card narrower than this reads as a tooltip.
    if (widest < ugfx_char_w() * 30) widest = ugfx_char_w() * 30;

    int body_h = 0;
    if (d->body) {
        if (d->body_w > widest) widest = d->body_w;
        body_h = d->body_h + gap * 2;
    }

    d->w = widest + pad() * 2;
    if (d->w > d->bw - gap * 2) d->w = d->bw - gap * 2;
    int text_h = pad() + head_h(d) + note_h(d) + gap * 2 + btn_h() + pad();
    // The body gives way before the box leaves the window: a listing
    // with fewer rows beats a Cancel button below the bottom edge.
    if (body_h > 0 && text_h + body_h > d->bh - gap * 2) {
        body_h = d->bh - gap * 2 - text_h;
        if (body_h < line_h() + gap) body_h = line_h() + gap;
    }
    d->h = text_h + body_h;

    d->x = d->bx + (d->bw - d->w) / 2 + d->off_x;
    d->y = d->by + (d->bh - d->h) / 2 + d->off_y;
    // CLAMPED EVERY LAYOUT, not just on the drag: a window that shrinks
    // under a dragged box would otherwise strand it off-screen, where a
    // modal is unreachable and nothing else can be clicked either.
    if (d->x > d->bx + d->bw - d->w) d->x = d->bx + d->bw - d->w;
    if (d->x < d->bx) d->x = d->bx;
    if (d->y > d->by + d->bh - d->h) d->y = d->by + d->bh - d->h;
    if (d->y < d->by) d->y = d->by;

    if (d->body && d->body->ops && d->body->ops->set_geometry) {
        int tx = text_x(d);
        d->body->ops->set_geometry(d->body->widget, tx,
                                   d->y + pad() + head_h(d) + note_h(d) + gap,
                                   d->x + d->w - pad() - tx, body_h - gap * 2);
    }
}

int uui_dialog_body_rect(const struct uui_dialog *d, int *x, int *y, int *w, int *h) {
    if (!d->open || !d->body || !d->body->ops || !d->body->ops->bounds) return 0;
    d->body->ops->bounds(d->body->widget, x, y, w, h);
    return 1;
}

// Button `i`'s rect. ONE calculation, shared by the draw and the hit
// test -- two copies of this arithmetic drift, and the symptom is a
// click landing on the button beside the one under the cursor.
static void button_rect(const struct uui_dialog *d, int i,
                         int *x, int *y, int *w, int *h) {
    int gap = utheme_gap();
    int bw = btn_w(d), bh = btn_h();
    int total = d->button_count * bw + (d->button_count - 1) * gap;
    int x0 = d->x + d->w - pad() - total;   // right-aligned, as everywhere
    *x = x0 + i * (bw + gap);
    *y = d->y + d->h - pad() - bh;
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
    d->picture = NULL;
    d->note = NULL;
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

void uui_dialog_set_picture(struct uui_dialog *d, const struct uimg *picture) {
    d->picture = picture;
    if (d->open) layout(d);
}

void uui_dialog_set_note(struct uui_dialog *d, const char *note) {
    d->note = note;
    if (d->open) layout(d);
}

void uui_dialog_set_body(struct uui_dialog *d, struct uui_item *body, int w, int h) {
    d->body = body;
    d->body_w = w;
    d->body_h = h;
    if (!body) d->focus = NULL;
    if (d->open) layout(d);
}

void uui_dialog_focus(struct uui_dialog *d, struct uui_item *it) {
    if (d->focus == it) return;
    if (d->focus && d->focus->ops && d->focus->ops->set_focused)
        d->focus->ops->set_focused(d->focus->widget, 0);
    d->focus = it;
    if (it && it->ops && it->ops->set_focused) it->ops->set_focused(it->widget, 1);
}

int uui_dialog_take_code(struct uui_dialog *d) {
    int c = d->committed;
    d->committed = -1;
    return c;
}

// The scrim, the card, its picture, title, rows and note. Split from the
// buttons because a dialog WITH A BODY draws these around its children
// (children_begin / children_end); one without draws both at once.
static void draw_frame(struct ugfx_surface *s, const struct uui_dialog *d) {
    // THE WINDOW DIMS: what is behind is not answerable until this is.
    for (int y = d->by; y < d->by + d->bh; y++)
        ugfx_blend_hspan(s, d->bx, y, d->bw, ugfx_rgb(16, 20, 28), NULL, 80);
    // A soft shadow, down and to the right, then the card.
    for (int i = 3; i >= 1; i--)
        for (int y = d->y + i * 2; y < d->y + d->h + i * 2; y++)
            ugfx_blend_hspan(s, d->x + i, y, d->w, ugfx_rgb(0, 0, 0), NULL, 26);
    uint32_t bg = UTHEME_WHITE;
    uui_fill_round_rect(s, d->x, d->y, d->w, d->h, 8, UTHEME_BORDER);
    uui_fill_round_rect(s, d->x + 1, d->y + 1, d->w - 2, d->h - 2, 7, bg);

    if (d->picture) {
        const struct uimg *p = d->picture;
        int px = d->x + pad() + (pic_px() - p->w) / 2;
        int py = d->y + pad() + (pic_px() - p->h) / 2;
        if (p->has_alpha) ugfx_blit_alpha(s, px, py, p->w, p->h, p->px, p->w);
        else {
            ugfx_blit(s, px, py, p->w, p->h, p->px, p->w);
            ugfx_draw_rect(s, px - 1, py - 1, p->w + 2, p->h + 2, UTHEME_BORDER);
        }
    }

    int tx = text_x(d), tw = d->x + d->w - pad() - tx;
    int y = d->y + pad();
    if (d->title) {
        const struct ugfx_font *was = ugfx_set_font(ugfx_font_session(UGFX_FONT_BOLD));
        ugfx_draw_string_clipped(s, tx, y, tw, d->title, UTHEME_TEXT, bg);
        ugfx_set_font(was);
    }
    y += line_h();
    uint32_t dim = uui_state_bg(UTHEME_TEXT, UUI_STATE_DISABLED);
    for (int i = 0; i < d->row_count; i++) {
        if (d->rows[i])
            // Clipped, always: these rows carry PATHS, which are longer
            // than any box routinely (docs/gui-guidelines.md's oldest
            // trap).
            ugfx_draw_string_clipped(s, tx, y, tw, d->rows[i], i ? dim : UTHEME_TEXT, bg);
        y += line_h();
    }

    if (d->note) {
        // Tinted by what the dialog does: red when an answer destroys
        // something, the accent otherwise.
        int danger = 0;
        for (int i = 0; i < d->button_count; i++)
            if (d->buttons[i].style == UUI_DLG_DANGER) danger = 1;
        uint32_t tint = ugfx_blend(bg, danger ? ugfx_rgb(178, 58, 36) : UTHEME_ACCENT, 28);
        uint32_t ink = danger ? ugfx_rgb(122, 42, 24) : UTHEME_TEXT;
        int ny = d->y + pad() + head_h(d) + utheme_gap();
        int nx = d->x + pad(), nw = d->w - pad() * 2;
        uui_fill_round_rect(s, nx, ny, nw, note_lines(d) * line_h() + utheme_gap(), 4, tint);
        const char *rest = d->note;
        char line[160];
        for (int n = 0; n < note_lines(d) && *rest; n++) {
            rest = uui_label_wrap_next(rest, nw - 16, line, sizeof line);
            ugfx_draw_string_clipped(s, nx + 8, ny + utheme_gap() / 2 + 2 + n * line_h(),
                                      nw - 16, line, ink, tint);
        }
    }
}

// The button's look: an explicit style wins; with none in the row, the
// default answer is the primary one.
static int button_style(const struct uui_dialog *d, int i) {
    for (int j = 0; j < d->button_count; j++)
        if (d->buttons[j].style) return d->buttons[i].style;
    return i == d->default_button ? UUI_DLG_PRIMARY : UUI_DLG_PLAIN;
}

static void draw_buttons(struct ugfx_surface *s, const struct uui_dialog *d) {
    for (int i = 0; i < d->button_count; i++) {
        int bx, by, bw, bh;
        button_rect(d, i, &bx, &by, &bw, &bh);
        enum uui_state st = (d->pressed == i) ? UUI_STATE_PRESSED
                           : (d->hot == i)     ? UUI_STATE_HOVER
                                               : UUI_STATE_REST;
        int style = button_style(d, i);
        uint32_t face = style == UUI_DLG_PRIMARY ? UTHEME_ACCENT
                      : style == UUI_DLG_DANGER  ? ugfx_rgb(178, 58, 36)
                                                 : UTHEME_BUTTON_BG;
        uint32_t fg = style ? ugfx_rgb(255, 255, 255) : UTHEME_TEXT;
        uint32_t fill = uui_state_bg(face, st);
        uint32_t edge = uui_state_bg(uui_state_bg(face, UUI_STATE_PRESSED), UUI_STATE_PRESSED);
        uui_fill_round_rect(s, bx, by, bw, bh, 4, edge);
        uui_fill_round_rect(s, bx + 1, by + 1, bw - 2, bh - 2, 3, fill);
        uui_button_draw_label(s, bx, by, bw, bh, d->buttons[i].label, fg, fill, st);
        if (d->hot == i) uui_focus_ring(s, bx, by, bw, bh);
    }
}

void uui_dialog_draw(struct ugfx_surface *s, const struct uui_dialog *d) {
    if (!d->open) return;
    draw_frame(s, d);
    draw_buttons(s, d);
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
    // The focused body item first: a fileview's Enter opens, a field's
    // Left moves its caret. What it declines falls to the buttons -- a
    // field never takes Enter, so Return still commits the default.
    if (d->focus && d->focus->ops && d->focus->ops->key &&
        d->focus->ops->key(d->focus->widget, key, 0)) return 1;
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
// With a body the dialog is a container, and the router paints it in
// the items pass: frame, then the body's items, then the buttons. The
// overlay slot above is never reached for one (ui/uui_route.c draws
// a container's children instead), so nothing paints twice.
static struct uui_item *dlg_children(void *w, int *out_count) {
    struct uui_dialog *d = w;
    if (!d->open || !d->body) { *out_count = 0; return NULL; }
    *out_count = 1;
    return d->body;
}
static void dlg_children_begin(struct ugfx_surface *s, void *w) {
    draw_frame(s, (const struct uui_dialog *)w);
}
static void dlg_children_end(struct ugfx_surface *s, void *w) {
    draw_buttons(s, (const struct uui_dialog *)w);
}
static void dlg_describe(const void *w, const struct uui_describe *desc) {
    const struct uui_dialog *d = w;
    if (!d->open) return;
    for (int i = 0; i < d->button_count; i++) {
        int x, y, bw, bh;
        button_rect(d, i, &x, &y, &bw, &bh);
        uui_describe_rect_i(desc, "button", i, x, y, bw, bh);
    }
}
static int dlg_overlay_active(const void *w) {
    return ((const struct uui_dialog *)w)->open;
}
// **THE SCRIM, NOT THE BOX -- THAT IS WHAT MAKES IT MODAL.** The router
// gates press/motion/wheel on this, so answering with the panel let
// every click outside it fall through to whatever was behind: an open
// Preferences dialog and a working menu bar and tab close button under
// it. dlg_press() always meant to consume wherever it landed and was
// simply never asked. bx/by/bw/bh are the whole window; x/y/w/h are the
// centred panel, which is still what `bounds` reports for drawing.
static int dlg_hit(const void *w, int cx, int cy) {
    const struct uui_dialog *d = w;
    return d->open && uui_hit(d->bx, d->by, d->bw, d->bh, cx, cy);
}
// The title strip: the band the title is drawn in, and the only part of
// the box that starts a drag. A body fills the rest, so grabbing
// anywhere else would fight whatever control is under the pointer.
static int on_title(const struct uui_dialog *d, int cx, int cy) {
    if (!d->title) return 0;
    return uui_hit(d->x, d->y, d->w, pad() + line_h(), cx, cy);
}

static int dlg_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_dialog *d = w;
    if (!d->open) return 0;
    d->pressed = button_at(d, cx, cy);
    d->hot = d->pressed >= 0 ? d->pressed : d->hot;
    if (d->pressed < 0 && on_title(d, cx, cy)) {
        d->dragging = 1;
        d->grab_dx = cx - d->x;
        d->grab_dy = cy - d->y;
    }
    // CONSUMED WHEREVER IT LANDED, including outside the box: that is
    // what makes it modal. Clicking outside does NOT dismiss -- these
    // dialogs ask questions whose default answer is not obvious, and
    // a stray click is not an answer.
    return 1;
}
static int dlg_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_dialog *d = w;
    if (!d->open) return 0;
    // A DRAG NEEDS THE BUTTON STILL DOWN (docs/conventions/gui.md): a
    // motion with none held is not a drag, and treating it as one
    // leaves the box following the pointer after the release.
    // **A LEAVE CARRIES NO POSITION** (docs/conventions/gui.md), and a
    // motion with no button held is ignored rather than treated as a
    // release -- the drag ends on the RELEASE and nowhere else. Both
    // matter here: dragging a box towards an edge sends the pointer out
    // of the content area, which arrives as (-1, -1) with no buttons,
    // and cancelling on either of those dropped the box mid-drag.
    if (cx < 0 || cy < 0) return 0;
    if (d->dragging) {
        if (!buttons) return 0;
        int want_x = cx - d->grab_dx, want_y = cy - d->grab_dy;
        d->off_x += want_x - d->x;
        d->off_y += want_y - d->y;
        layout(d);              // re-clamps, and moves the body with it
        return 1;
    }
    int over = button_at(d, cx, cy);
    if (over == d->hot) return 0;
    d->hot = over >= 0 ? over : d->hot;
    return 1;
}
static int dlg_release(void *w, int cx, int cy) {
    struct uui_dialog *d = w;
    if (!d->open) return 1;
    if (d->dragging) { d->dragging = 0; d->pressed = -1; return 1; }
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
    .children        = dlg_children,
    .children_begin  = dlg_children_begin,
    .children_end    = dlg_children_end,
    .describe        = dlg_describe,
};
