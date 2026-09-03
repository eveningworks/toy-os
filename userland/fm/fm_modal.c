// The app's own modal: Rename and New folder -- the two prompts with a
// text field. (The delete confirmation is a uui_dialog, in files.c.)
//
// One of the File Manager's units -- see fm_internal.h for what is
// where and why these share their state directly.
#include "fm_internal.h"
#include "ui/utheme.h"
#include "keyboard.h"
#include <string.h>
#include <stdio.h>

// --- the modal ---------------------------------------------------------
//
// Its own, in this window, exactly as Notepad's dialog is: a client
// cannot open a WM-level dialog (there is no such request). Not a
// uui_dialog because that widget has no text field; the day it grows
// one, this file goes.
enum modal_kind g_modal;
static char g_modal_title[64];
static struct uui_textbox g_modal_field;
static int g_modal_cmd;          // what to do when it commits

// --- commands ---------------------------------------------------------

void open_prompt(int cmd, const char *title, const char *initial) {
    g_modal = MODAL_PROMPT;
    g_modal_cmd = cmd;
    strlcpy(g_modal_title, title, sizeof g_modal_title);
    uui_textbox_init(&g_modal_field, initial ? initial : "");
    uui_textbox_set_active(&g_modal_field, 1);
}

// --- the modal's own input --------------------------------------------

int modal_key(struct uapp *a, int key) {
    if (g_modal == MODAL_NONE) return 0;

    if (key == 0x1B) { g_modal = MODAL_NONE; set_note("cancelled"); uapp_redraw(a); return 1; }

    if (key == '\n' || key == '\r') {
        int cmd = g_modal_cmd;
        char text[UUI_TEXTBOX_MAX];
        strlcpy(text, uui_textbox_text(&g_modal_field), sizeof text);
        // Closed BEFORE the action runs: an action that opens another
        // modal (or logs) must not find this one still up.
        g_modal = MODAL_NONE;
        if (cmd == CMD_MKDIR) commit_mkdir(text);
        else if (cmd == CMD_RENAME) commit_rename(text);
        uapp_redraw(a);
        return 1;
    }

    if (uui_textbox_key(&g_modal_field, key)) uapp_redraw(a);
    return 1; // modal: swallow everything else
}

// The modal's box, centred. Derived, never constant: every size here
// comes from the font (docs/gui-guidelines.md).
static void modal_rect(int cw, int ch, int *x, int *y, int *w, int *h) {
    int pad = utheme_pad();
    int lines = 3;
    *w = cw * 3 / 4;
    *h = pad * 2 + lines * (ugfx_char_h() + utheme_gap()) + utheme_control_h();
    *x = (cw - *w) / 2;
    *y = (ch - *h) / 2;
}

void draw_modal(struct ugfx_surface *s) {
    if (g_modal == MODAL_NONE) return;

    int x, y, w, h;
    modal_rect(s->w, s->h, &x, &y, &w, &h);
    int pad = utheme_pad(), lh = ugfx_char_h() + utheme_gap();

    ugfx_fill_rect(s, x, y, w, h, UTHEME_PANEL_BG);
    ugfx_draw_rect(s, x, y, w, h, UTHEME_BORDER);
    ugfx_draw_string_clipped(s, x + pad, y + pad, w - pad * 2, g_modal_title,
                              UTHEME_TEXT, UTHEME_PANEL_BG);

    uui_textbox_set_geometry(&g_modal_field, x + pad, y + pad + lh,
                              w - pad * 2, utheme_control_h());
    uui_textbox_draw(s, &g_modal_field);
    ugfx_draw_string_clipped(s, x + pad, y + pad + lh + utheme_control_h() + utheme_gap(),
                              w - pad * 2, "Enter = ok, Esc = cancel",
                              UTHEME_TEXT, UTHEME_PANEL_BG);
}
