// dropdown. Split out of uwidgets.c -- see ui/uui_dropdown.h.
#include "ui/uui_dropdown.h"
#include "ui/uui_widget.h"  // the ops table the focus ring takes
#include "ui/uui_popup.h"
#include "keyboard.h" // KEY_* codes, as delivered by WIN_EV_KEY

// ---------------------------------------------------------------------
// dropdown -- composes the listbox as its popup
// ---------------------------------------------------------------------

void uui_dropdown_init(struct uui_dropdown *d, int x, int y, int w, int h,
                        const char *const *items, int count) {
    d->x = x; d->y = y; d->w = w; d->h = h;
    d->open = 0;
    d->max_rows = 6;
    d->focused = 0;
    d->bg = ugfx_rgb(255, 255, 255);
    d->fg = ugfx_rgb(20, 20, 20);
    d->border = ugfx_rgb(150, 155, 165);

    int rh = ugfx_char_h() + 4;
    int rows = count < d->max_rows ? count : d->max_rows;
    uui_listbox_init(&d->list, x, y + h, w, rows > 0 ? rows * rh : rh, items, count);
    d->disabled = 0;
}

int uui_dropdown_selected(const struct uui_dropdown *d) { return d->list.selected; }

// THE POPUP IS A SURFACE WHEN THE COMPOSITOR GRANTS ONE, and the list
// keeps its rect in the PARENT's content coordinates either way -- the
// seam hands the placed position back in that space and translates
// input on the surface back into it (ui/uui_popup.h), so every hit test
// below is unchanged by the move. Only the draw differs.
//
// Opened WITH a grab: an open dropdown owns the next click, which is
// what dd_ops_overlay used to buy in-window.
static void dd_done(void *owner) {
    struct uui_dropdown *d = (struct uui_dropdown *)owner;
    d->popup = 0;
    d->open = 0;
}

static void dd_place_list(struct uui_dropdown *d) {
    int rh = uui_listbox_row_h(&d->list);
    int rows = d->list.count < d->max_rows ? d->list.count : d->max_rows;
    if (rows < 1) rows = 1;
    d->list.x = d->x;
    d->list.y = d->y + d->h;
    d->list.w = d->w;
    d->list.h = rows * rh;
}

static void dd_open(struct uui_dropdown *d) {
    if (d->open) return;
    d->open = 1;
    dd_place_list(d);
    int px, py;
    d->popup = uui_popup_open(d->x, d->y, d->w, d->h,
                              d->list.w, d->list.h, UUI_POPUP_BELOW,
                              UUI_POPUP_GRAB, dd_done, d, &px, &py);
    // Where the COMPOSITOR put it -- it may have flipped the list above
    // the box near the screen's bottom edge, which the in-window
    // placement above could never do.
    if (d->popup) { d->list.x = px; d->list.y = py; }
}

static void dd_close(struct uui_dropdown *d) {
    if (d->popup) {
        uui_popup_close(d->popup);
        d->popup = 0;
    }
    d->open = 0;
}

// Moves the closed box AND re-places the popup under it.
//
// The popup's geometry is the WIDGET's, not the app's: an app that set
// d->x/y/w/h directly (the only way to move one before this existed)
// left the popup wherever init() had put it, so it drew and hit-tested
// at a stale position -- every row committing whatever init happened to
// select. Behaviour belongs to the component (docs/gui-guidelines.md).
void uui_dropdown_set_geometry(struct uui_dropdown *d, int x, int y, int w, int h) {
    d->x = x; d->y = y; d->w = w; d->h = h;
    dd_place_list(d);
}

void uui_dropdown_draw(struct ugfx_surface *s, const struct uui_dropdown *d) {
    // Dimmed text and caret; the face and border stay, so the control
    // still occupies its place rather than looking absent.
    uint32_t fg = d->disabled ? uui_state_bg(d->fg, UUI_STATE_DISABLED) : d->fg;
    ugfx_fill_rect(s, d->x, d->y, d->w, d->h, d->bg);
    ugfx_draw_rect(s, d->x, d->y, d->w, d->h, d->border);

    const char *label = (d->list.selected >= 0 && d->list.selected < d->list.count)
                            ? d->list.items[d->list.selected] : "";
    ugfx_draw_string_clipped(s, d->x + 6, d->y + (d->h - ugfx_char_h()) / 2,
                              d->w - 24, label, fg, d->bg);

    // A caret so it reads as a dropdown rather than a text field.
    int cx = d->x + d->w - 14, cy = d->y + d->h / 2 - 2;
    for (int i = 0; i < 5; i++) ugfx_fill_rect(s, cx + i, cy + i, 5 - i * 2 + 4, 1, fg);

    // FOCUS IS DRAWN, because a focused dropdown accepts typed letters
    // and a control that silently answers the keyboard is a control the
    // user cannot find. Inset by one so it reads as a ring inside the
    // border rather than a thicker border.
    if (d->focused && !d->open) {
        uui_focus_ring(s, d->x + 1, d->y + 1, d->w - 2, d->h - 2);
    }
}

void uui_dropdown_draw_popup(struct ugfx_surface *s, const struct uui_dropdown *d) {
    if (!d->open) return;
    int ox = 0, oy = 0;
    if (d->popup) {
        struct ugfx_surface *ps = uui_popup_surface(d->popup);
        if (ps) { s = ps; ox = d->list.x; oy = d->list.y; }
    }
    uui_listbox_draw_at(s, &d->list, ox, oy);
    ugfx_draw_rect(s, d->list.x - ox, d->list.y - oy, d->list.w, d->list.h,
                    d->border);
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
        if (d->open) dd_close(d); else dd_open(d);
        return 1;
    }
    if (d->open) {
        // The popup's SCROLLBAR first: a press on it scrolls the list
        // and must not commit or dismiss. Without this the bar was not
        // merely inert -- clicking it CLOSED the popup, which is the
        // most annoying possible answer to "I tried to scroll".
        if (uui_listbox_press(&d->list, cx, cy)) return 1;

        if (uui_hit(d->list.x, d->list.y, d->list.w, d->list.h, cx, cy)) {
            uui_listbox_click(&d->list, cx, cy);
            dd_close(d); // committing closes it
            return 1;
        }
        // A click anywhere else DISMISSES rather than falling through to
        // whatever is underneath -- an open popup owns the next click.
        // On a popup SURFACE this arm is unreachable: the compositor's
        // grab consumes that press and answers with WIN_EV_POPUP_DONE,
        // which lands in dd_done() instead.
        dd_close(d);
        return 1;
    }
    return 0;
}

// Forwarded to the popup's list, so a thumb drag inside an open popup
// works exactly as it does in a standalone listbox. No-ops while closed.
int uui_dropdown_drag(struct uui_dropdown *d, int cx, int cy) {
    if (!d->open) return 0;
    return uui_listbox_drag(&d->list, cx, cy);
}

void uui_dropdown_drag_end(struct uui_dropdown *d) {
    uui_listbox_drag_end(&d->list);
}

int uui_dropdown_key(struct uui_dropdown *d, int key) {
    if (!d->open) {
        if (key == '\n' || key == '\r' || key == ' ' || key == KEY_ARROW_DOWN) {
            dd_open(d);
            return 1;
        }
        // A LETTER SELECTS WITHOUT OPENING, as a Windows or KDE combobox
        // does: the list is one line long while closed, so typing "hel"
        // moves the value to Helsinki in place. This is deliberately
        // NOT the rule the wheel follows -- a wheel notch over a closed
        // dropdown is ignored because the pointer is merely passing
        // over it and the value would change unseen, whereas a typed
        // letter can only arrive at the control that has focus.
        //
        // PRINTABLE KEYS ONLY, and the test is here rather than in the
        // list: a closed dropdown must go on IGNORING the arrows, Home
        // and End, so a key cannot walk a value the user cannot see
        // (uidemo_test asserts it, and forwarding everything broke it).
        // A letter is a deliberate search; an arrow is navigation that
        // belongs to whatever is on screen.
        if (key > ' ' && key < 0x7F) return uui_listbox_key(&d->list, key);
        return 0;
    }
    if (key == 0x1B) { dd_close(d); return 1; }             // Esc dismisses
    if (key == '\n' || key == '\r') { dd_close(d); return 1; } // Enter commits
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
    struct uui_dropdown *d = (struct uui_dropdown *)w;
    if (d->disabled) return 0;
    return uui_dropdown_key(d, key);
}
static int dd_ops_accepts_focus(const void *w) {
    return !((const struct uui_dropdown *)w)->disabled;
}

static void dd_ops_set_focused(void *w, int focused) {
    struct uui_dropdown *d = (struct uui_dropdown *)w;
    d->focused = focused;
    // Focus leaving CLOSES the popup. A popup left open while the keys
    // go somewhere else is a menu nobody is driving, and it would still
    // be drawn over the rest of the window.
    if (!focused) dd_close(d);
}

const struct uui_widget_ops uui_dropdown_focus_ops = {
    .hit = dd_ops_hit,
    .key = dd_ops_key,
    .set_focused = dd_ops_set_focused,
    .accepts_focus = dd_ops_accepts_focus,
};

// --- routed pointer input (ui/uui_route.h) ----------------------------
//
// **This is the widget `overlay_active` exists for.** An open popup is
// drawn on top of everything and extends well outside the closed box,
// so `hit` -- which covers the box only -- cannot decide who gets a
// press. Declaring the overlay makes the router offer every press here
// FIRST, which is what lets a click inside the popup select a row, a
// click on the popup's scrollbar scroll it, and a click anywhere else
// dismiss the popup AND be swallowed rather than also landing on
// whatever sits underneath.
static int dd_ops_overlay(const void *w) {
    return ((const struct uui_dropdown *)w)->open;
}

static int dd_ops_press(void *w, int cx, int cy, unsigned mods) {
    (void)mods;
    struct uui_dropdown *d = (struct uui_dropdown *)w;
    if (d->disabled) return 0;
    return uui_dropdown_click(d, cx, cy);
}

static int dd_ops_motion(void *w, int cx, int cy, unsigned buttons) {
    struct uui_dropdown *d = (struct uui_dropdown *)w;
    if (d->disabled) return 0;
    if (buttons) return uui_dropdown_drag(d, cx, cy);
    if (!d->open) return 0;
    return uui_listbox_hover(&d->list, cx, cy);
}

static int dd_ops_release(void *w, int cx, int cy) {
    (void)cx; (void)cy;
    if (((struct uui_dropdown *)w)->disabled) return 0;
    uui_dropdown_drag_end((struct uui_dropdown *)w);
    return 0;
}

// Only while OPEN: a closed dropdown deliberately ignores the wheel, so
// the wheel over a form scrolls the form rather than silently changing
// a value the user cannot see. Same rule the keyboard follows.
static int dd_ops_wheel(void *w, int notches) {
    struct uui_dropdown *d = (struct uui_dropdown *)w;
    if (!d->open) return 0;
    return uui_listbox_wheel(&d->list, notches);
}

static void dd_ops_draw(struct ugfx_surface *s, const void *w) {
    uui_dropdown_draw(s, (const struct uui_dropdown *)w);
}

// The popup, drawn in the painter's SECOND pass so it lands on top of
// every other widget without the app having to order anything.
static void dd_ops_draw_overlay(struct ugfx_surface *s, const void *w) {
    uui_dropdown_draw_popup(s, (const struct uui_dropdown *)w);
}

static void dd_ops_natural_size(const void *w, int *out_w, int *out_h) {
    uui_dropdown_natural_size((const struct uui_dropdown *)w, out_w, out_h);
}

static void dd_ops_set_geometry(void *w, int x, int y, int width, int height) {
    uui_dropdown_set_geometry((struct uui_dropdown *)w, x, y, width, height);
}

// NATURAL_SIZE AND SET_GEOMETRY WERE MISSING UNTIL 2026-08-19, so a
// dropdown declared in a uui_layout was never positioned OR measured --
// it stayed at 0x0 at the origin, and the layout, unable to size the
// child, placed nothing after it either. A whole page below the
// dropdown simply did not appear.
//
// It went unnoticed because no app had put one in a layout: UI Demo
// positions its widgets by hand, and System Settings was the first to
// declare one. Both functions already existed -- only the table was
// short. Worth remembering when adding a widget: the ops table is the
// contract, and a missing slot fails SILENTLY and at a distance.
static void dropdown_bounds_op(const void *w, int *x, int *y, int *ow, int *oh) {
    const struct uui_dropdown *c = w;
    *x = c->x; *y = c->y; *ow = c->w; *oh = c->h;
}

const struct uui_widget_ops uui_dropdown_ops = {
    .bounds = dropdown_bounds_op,
    .natural_size   = dd_ops_natural_size,
    .set_geometry   = dd_ops_set_geometry,
    .draw           = dd_ops_draw,
    .draw_overlay   = dd_ops_draw_overlay,
    .hit            = dd_ops_hit,
    .key            = dd_ops_key,
    .set_focused    = dd_ops_set_focused,
    .accepts_focus  = dd_ops_accepts_focus,
    .overlay_active = dd_ops_overlay,
    .press          = dd_ops_press,
    .motion         = dd_ops_motion,
    .release        = dd_ops_release,
    .wheel          = dd_ops_wheel,
};
