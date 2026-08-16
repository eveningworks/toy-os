// dropdown. Split out of uwidgets.c -- see ui/uui_dropdown.h.
#include "ui/uui_dropdown.h"
#include "ui/uui_widget.h"  // the ops table the focus ring takes
#include "keyboard.h" // KEY_* codes, as delivered by WIN_EV_KEY

// ---------------------------------------------------------------------
// dropdown -- composes the listbox as its popup
// ---------------------------------------------------------------------

void uui_dropdown_init(struct uui_dropdown *d, int x, int y, int w, int h,
                        const char *const *items, int count) {
    d->x = x; d->y = y; d->w = w; d->h = h;
    d->open = 0;
    d->max_rows = 6;
    d->bg = ugfx_rgb(255, 255, 255);
    d->fg = ugfx_rgb(20, 20, 20);
    d->border = ugfx_rgb(150, 155, 165);

    int rh = ugfx_char_h() + 4;
    int rows = count < d->max_rows ? count : d->max_rows;
    uui_listbox_init(&d->list, x, y + h, w, rows > 0 ? rows * rh : rh, items, count);
}

int uui_dropdown_selected(const struct uui_dropdown *d) { return d->list.selected; }

// Moves the closed box AND re-places the popup under it.
//
// The popup's geometry is the WIDGET's, not the app's: an app that set
// d->x/y/w/h directly (the only way to move one before this existed)
// left the popup wherever init() had put it, so it drew and hit-tested
// at a stale position -- every row committing whatever init happened to
// select. Behaviour belongs to the component (docs/gui-guidelines.md).
void uui_dropdown_set_geometry(struct uui_dropdown *d, int x, int y, int w, int h) {
    d->x = x; d->y = y; d->w = w; d->h = h;
    int rh = uui_listbox_row_h(&d->list);
    int rows = d->list.count < d->max_rows ? d->list.count : d->max_rows;
    if (rows < 1) rows = 1;
    d->list.x = x;
    d->list.y = y + h;
    d->list.w = w;
    d->list.h = rows * rh;
}

void uui_dropdown_draw(struct ugfx_surface *s, const struct uui_dropdown *d) {
    ugfx_fill_rect(s, d->x, d->y, d->w, d->h, d->bg);
    ugfx_draw_rect(s, d->x, d->y, d->w, d->h, d->border);

    const char *label = (d->list.selected >= 0 && d->list.selected < d->list.count)
                            ? d->list.items[d->list.selected] : "";
    ugfx_draw_string_clipped(s, d->x + 6, d->y + (d->h - ugfx_char_h()) / 2,
                              d->w - 24, label, d->fg, d->bg);

    // A caret so it reads as a dropdown rather than a text field.
    int cx = d->x + d->w - 14, cy = d->y + d->h / 2 - 2;
    for (int i = 0; i < 5; i++) ugfx_fill_rect(s, cx + i, cy + i, 5 - i * 2 + 4, 1, d->fg);
}

void uui_dropdown_draw_popup(struct ugfx_surface *s, const struct uui_dropdown *d) {
    if (!d->open) return;
    uui_listbox_draw(s, &d->list);
    ugfx_draw_rect(s, d->list.x, d->list.y, d->list.w, d->list.h, d->border);
}

void uui_dropdown_natural_size(const struct uui_dropdown *d, int *out_w, int *out_h) {
    int pad = ugfx_char_w() / 2;
    int arrow_room = ugfx_char_w() * 2;
    if (out_w) {
        int widest = 0;
        for (int i = 0; i < d->list.count; i++) {
            int tw = ugfx_text_width(d->list.items[i]);
            if (tw > widest) widest = tw;
        }
        *out_w = widest + pad * 2 + arrow_room;
    }
    if (out_h) *out_h = ugfx_char_h() + 2 * UUI_PAD_Y;
}

int uui_dropdown_hit(const struct uui_dropdown *d, int cx, int cy) {
    return uui_hit(d->x, d->y, d->w, d->h, cx, cy);
}

int uui_dropdown_click(struct uui_dropdown *d, int cx, int cy) {
    if (uui_dropdown_hit(d, cx, cy)) {
        d->open = !d->open;
        return 1;
    }
    if (d->open) {
        if (uui_hit(d->list.x, d->list.y, d->list.w, d->list.h, cx, cy)) {
            uui_listbox_click(&d->list, cx, cy);
            d->open = 0; // committing closes it
            return 1;
        }
        // A click anywhere else DISMISSES rather than falling through to
        // whatever is underneath -- an open popup owns the next click.
        d->open = 0;
        return 1;
    }
    return 0;
}

int uui_dropdown_key(struct uui_dropdown *d, int key) {
    if (!d->open) {
        if (key == '\n' || key == '\r' || key == ' ' || key == KEY_ARROW_DOWN) {
            d->open = 1;
            return 1;
        }
        return 0;
    }
    if (key == 0x1B) { d->open = 0; return 1; }             // Esc dismisses
    if (key == '\n' || key == '\r') { d->open = 0; return 1; } // Enter commits
    return uui_listbox_key(&d->list, key);
}

// --- focus ------------------------------------------------------------
//
// Focus-only ops (see uui_textbox.c). The hit test covers the CLOSED
// box only: an open popup is hit-tested by the app before focus is
// consulted, the same order the kernel version documents -- input order
// is the reverse of draw order.
static int dd_ops_hit(const void *w, int cx, int cy) {
    return uui_dropdown_hit((const struct uui_dropdown *)w, cx, cy);
}
static int dd_ops_key(void *w, int key, unsigned mods) {
    (void)mods;
    return uui_dropdown_key((struct uui_dropdown *)w, key);
}
static int dd_ops_accepts_focus(const void *w) { (void)w; return 1; }

const struct uui_widget_ops uui_dropdown_focus_ops = {
    .hit = dd_ops_hit,
    .key = dd_ops_key,
    .accepts_focus = dd_ops_accepts_focus,
};
