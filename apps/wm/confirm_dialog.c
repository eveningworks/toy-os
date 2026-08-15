// See confirm_dialog.h for the design writeup.
#include "confirm_dialog.h"
#include "wm_internal.h"
#include "ui/ui.h"
#include "theme.h"
#include "kapi.h"

int confirm_dialog_open = 0;

static const char *g_yes_label = "Yes";
static const char *g_no_label = "No";
static const char *g_message;
static void (*g_on_yes)(void);
static void (*g_on_no)(void);

// Geometry -- computed once at open time from the message's own length
// (same "size the popup to its content" approach context_menu.c's
// menu_w() uses), not recomputed every frame, since neither the
// message nor the screen size can change while a modal dialog is open.
static int g_x, g_y, g_w, g_h;

// The two buttons, as a real group: hover, pressed and
// commit-on-release all come from it rather than being reimplemented
// here (see the header for what this replaced). Screen-absolute
// geometry, like the rest of this dialog, so every draw/hit call passes
// origin (0, 0).
#define BTN_YES 0
#define BTN_NO  1
static struct ui_button g_btns[2];
static struct ui_button_group g_group;

#define DIALOG_PAD 16
#define BTN_GAP 12
#define BTN_H_EXTRA 10 // button height beyond one text row


// The real opener. Both entry points below set the labels FIRST,
// because the layout is measured from them -- a "Force Quit" button
// sized for the word "Yes" is exactly the kind of bug this file's own
// comment about hand-drawn geometry warns about.
static void open_common(const char *message, void (*on_yes)(void), void (*on_no)(void)) {
    g_message = message;
    g_on_yes = on_yes;
    g_on_no = on_no;

    int ch = gfx_char_h();
    // gfx_text_width(), not k_strlen() * gfx_char_w() -- that identity
    // holds only while every glyph is one fixed cell wide, which is
    // exactly what Milestone 21's proportional metrics stop being true
    // (see api/gfx.h).
    int msg_w = gfx_text_width(message);

    const char *yes_label = g_yes_label, *no_label = g_no_label;
    int yes_w = gfx_text_width(yes_label) + 24;
    int no_w = gfx_text_width(no_label) + 24;
    int buttons_w = yes_w + BTN_GAP + no_w;

    g_w = (msg_w > buttons_w ? msg_w : buttons_w) + DIALOG_PAD * 2;
    int btn_h = ch + BTN_H_EXTRA;
    g_h = DIALOG_PAD + ch + DIALOG_PAD + btn_h + DIALOG_PAD;

    g_x = (screen_w - g_w) / 2;
    g_y = (screen_h - g_h) / 2;

    int btn_y = g_y + DIALOG_PAD + ch + DIALOG_PAD;
    int btn_x0 = g_x + (g_w - buttons_w) / 2;

    ui_button_init(&g_btns[BTN_YES], btn_x0, btn_y, yes_w, btn_h, yes_label,
                    THEME_BUTTON_BG, THEME_TEXT, BTN_YES);
    ui_button_init(&g_btns[BTN_NO], btn_x0 + yes_w + BTN_GAP, btn_y, no_w, btn_h,
                    no_label, THEME_BUTTON_BG, THEME_TEXT, BTN_NO);
    ui_button_group_init(&g_group, g_btns, 2);

    confirm_dialog_open = 1;
    redraw_pending = 1;
}

static void close_dialog(void) {
    confirm_dialog_open = 0;
    redraw_pending = 1;
}

void confirm_dialog_draw(void) {
    if (!confirm_dialog_open) return;

    uint32_t bg = THEME_PANEL_BG, border = THEME_BORDER, fg = THEME_TEXT;
    gfx_fill_rect(g_x, g_y, g_w, g_h, bg);
    gfx_draw_rect(g_x, g_y, g_w, g_h, border);

    int msg_w = gfx_text_width(g_message);
    // Clipped to the dialog's interior: a message longer than the box
    // would otherwise be drawn straight through the border and out onto
    // the desktop, since gfx_draw_string() bounds nothing at all
    // (docs/gui-guidelines.md).
    gfx_draw_string_clipped(g_x + (g_w - msg_w) / 2, g_y + DIALOG_PAD,
                             g_w - DIALOG_PAD, g_message, fg, bg);

    ui_button_group_draw(&g_group, 0, 0); // screen-absolute, so no origin
}

int confirm_dialog_handle_click(int mx, int my) {
    if (!confirm_dialog_open) return 0;
    (void)mx; (void)my;
    // Deliberately acts on NOTHING. This fires on button-DOWN, and a
    // control that commits here can never be cancelled -- the exact trap
    // gui_apps.h's on_click documents and that this dialog fell into.
    // Arming and committing live in confirm_dialog_update_press() below.
    //
    // Still returns 1: the dialog is modal, so the click is swallowed
    // rather than passed through to whatever is underneath.
    return 1;
}

void confirm_dialog_update_press(int mx, int my, uint8_t buttons) {
    if (!confirm_dialog_open) return;

    if (buttons & 0x1) {
        // Re-hit-tested every tick, so dragging off a button visibly
        // un-presses it and dragging back re-presses.
        if (ui_button_group_press(&g_group, mx, my)) redraw_pending = 1;
        return;
    }

    // Button released. -1 means either nothing was armed or the press
    // had been dragged off -- both mean "do nothing", which is the whole
    // point of committing on release.
    int code = ui_button_group_release(&g_group);
    if (code < 0) return;

    close_dialog();
    if (code == BTN_YES) { if (g_on_yes) g_on_yes(); }
    else                 { if (g_on_no) g_on_no(); }
}

int confirm_dialog_update_hover(int mx, int my) {
    if (!confirm_dialog_open) return 0;
    return ui_button_group_hover(&g_group, mx, my);
}


void confirm_dialog_open_with(const char *message, void (*on_yes)(void), void (*on_no)(void)) {
    // Reset, so a labelled dialog's words cannot leak into an ordinary
    // one. This is the only place that can guarantee it.
    g_yes_label = "Yes";
    g_no_label = "No";
    open_common(message, on_yes, on_no);
}

void confirm_dialog_open_labelled(const char *message, const char *yes, const char *no,
                                   void (*on_yes)(void), void (*on_no)(void)) {
    g_yes_label = yes ? yes : "Yes";
    g_no_label = no ? no : "No";
    open_common(message, on_yes, on_no);
}

// The buttons' own rects, for the debug console (`gui dialog`) and so
// for tests. Same reasoning as context_menu_geometry(): read from the
// SAME ui_button the dialog draws and hit-tests, so a test cannot be
// told a position a click would not land on.
//
// tools/dialog_test.py used to find these by scanning the button row for
// THEME_BUTTON_BG, which works but re-derives geometry in Python -- the
// thing this project's own rules say not to do, and which stops working
// the moment a button's label changes its width. "Force Quit"/"Wait" is
// exactly that change.
int confirm_dialog_button_rect(int index, int *x, int *y, int *w, int *h,
                                const char **label) {
    if (!confirm_dialog_open || index < 0 || index > 1) return 0;
    if (x) *x = g_btns[index].x;
    if (y) *y = g_btns[index].y;
    if (w) *w = g_btns[index].w;
    if (h) *h = g_btns[index].h;
    if (label) *label = g_btns[index].label;
    return 1;
}

const char *confirm_dialog_message(void) {
    return confirm_dialog_open ? g_message : 0;
}
