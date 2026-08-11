// See confirm_dialog.h for the design writeup.
#include "confirm_dialog.h"
#include "wm_internal.h"
#include "ui/ui.h"
#include "theme.h"
#include "kapi.h"

int confirm_dialog_open = 0;

static const char *g_message;
static void (*g_on_yes)(void);
static void (*g_on_no)(void);

// Geometry -- computed once at open time from the message's own length
// (same "size the popup to its content" approach context_menu.c's
// menu_w() uses), not recomputed every frame, since neither the
// message nor the screen size can change while a modal dialog is open.
static int g_x, g_y, g_w, g_h;
static int g_btn_y, g_btn_h;
static int g_yes_x, g_yes_w, g_no_x, g_no_w;

#define DIALOG_PAD 16
#define BTN_GAP 12
#define BTN_H_EXTRA 10 // button height beyond one text row

void confirm_dialog_open_with(const char *message, void (*on_yes)(void), void (*on_no)(void)) {
    g_message = message;
    g_on_yes = on_yes;
    g_on_no = on_no;

    int cw = gfx_char_w(), ch = gfx_char_h();
    int msg_w = (int)k_strlen(message) * cw;

    const char *yes_label = "Yes", *no_label = "No";
    g_yes_w = (int)k_strlen(yes_label) * cw + 24;
    g_no_w = (int)k_strlen(no_label) * cw + 24;
    int buttons_w = g_yes_w + BTN_GAP + g_no_w;

    g_w = (msg_w > buttons_w ? msg_w : buttons_w) + DIALOG_PAD * 2;
    g_btn_h = ch + BTN_H_EXTRA;
    g_h = DIALOG_PAD + ch + DIALOG_PAD + g_btn_h + DIALOG_PAD;

    g_x = (screen_w - g_w) / 2;
    g_y = (screen_h - g_h) / 2;

    g_btn_y = g_y + DIALOG_PAD + ch + DIALOG_PAD;
    int total_btn_w = g_yes_w + BTN_GAP + g_no_w;
    int btn_x0 = g_x + (g_w - total_btn_w) / 2;
    g_yes_x = btn_x0;
    g_no_x = btn_x0 + g_yes_w + BTN_GAP;

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

    int msg_w = (int)k_strlen(g_message) * gfx_char_w();
    gfx_draw_string(g_x + (g_w - msg_w) / 2, g_y + DIALOG_PAD, g_message, fg, bg);

    uint32_t btn_bg = THEME_BUTTON_BG;
    gfx_fill_rect(g_yes_x, g_btn_y, g_yes_w, g_btn_h, btn_bg);
    gfx_draw_rect(g_yes_x, g_btn_y, g_yes_w, g_btn_h, border);
    gfx_draw_string(g_yes_x + (g_yes_w - 3 * gfx_char_w()) / 2, g_btn_y + (g_btn_h - gfx_char_h()) / 2,
                     "Yes", fg, btn_bg);

    gfx_fill_rect(g_no_x, g_btn_y, g_no_w, g_btn_h, btn_bg);
    gfx_draw_rect(g_no_x, g_btn_y, g_no_w, g_btn_h, border);
    gfx_draw_string(g_no_x + (g_no_w - 2 * gfx_char_w()) / 2, g_btn_y + (g_btn_h - gfx_char_h()) / 2,
                     "No", fg, btn_bg);
}

int confirm_dialog_handle_click(int mx, int my) {
    if (!confirm_dialog_open) return 0;

    if (widget_hit(g_yes_x, g_btn_y, g_yes_w, g_btn_h, mx, my)) {
        close_dialog();
        if (g_on_yes) g_on_yes();
        return 1;
    }
    if (widget_hit(g_no_x, g_btn_y, g_no_w, g_btn_h, mx, my)) {
        close_dialog();
        if (g_on_no) g_on_no();
        return 1;
    }
    // Modal -- a click anywhere else (including outside the dialog box
    // entirely) is swallowed, not passed through, and does NOT close
    // it. See this file's top comment for why "click elsewhere
    // dismisses" (context_menu.c's behavior) is wrong for a
    // confirmation specifically.
    return 1;
}
