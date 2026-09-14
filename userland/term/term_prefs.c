// The GUI Terminal's Preferences dialog: the controls, and turning them
// back into a struct term_conf. The file it edits is term_conf.c.
//
// **A MODAL IN THE TERMINAL'S OWN WINDOW, not a window of its own.** A
// TWP client cannot open a second window at all (ui/uui_dialog.h says
// why), and this is where the control belongs anyway: Konsole's "Edit
// Current Profile" and GNOME Terminal's Preferences are both attached
// to the terminal being configured, because the thing you are looking
// at while you pick a palette is the text it will be drawn in.
//
// TWO COLUMNS, which is Konsole's Appearance/Scrolling split: the
// scheme list is tall and the toggles are short, so one column of
// eleven rows would not fit a terminal window that has been made small.
#include "term.h"

#include <stdio.h>
#include <string.h>

#include "keyboard.h"
#include "lib/usetting.h"
#include "ui/ugfx.h"
#include "ui/ulog.h"
#include "ui/utheme.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_route.h"   // UUI_REASON_PRESS
#include "ui/uui_dialog.h"
#include "ui/uui_label.h"
#include "ui/uui_layout.h"
#include "ui/uui_listbox.h"
#include "ui/uui_radio_list.h"
#include "ui/uui_spinbox.h"

#define CMD_OK     1
#define CMD_CANCEL 2

// Ids the router reports back, so a press can move the dialog's key
// focus onto the control that was clicked.
enum {
    ID_SCHEME = 100, ID_CURSOR, ID_USE_SYS_FONT, ID_FONT, ID_SB, ID_MARGIN,
    ID_BLINK, ID_MENUBAR, ID_COPYSEL, ID_SCROLLOUT, ID_CONFIRM, ID_PREFS,
};

static struct uui_dialog g_dlg;

// The scheme list. NAMES are what goes in the file, LABELS are what the
// list shows -- the two differ (`solarized-dark` vs "Solarized Dark"),
// and showing the file name would be showing an implementation detail.
static char g_names[TERM_SCHEME_MAX][TERM_NAME_MAX];
static char g_labels[TERM_SCHEME_MAX][TERM_LABEL_MAX];
static const char *g_label_ptr[TERM_SCHEME_MAX];
static int g_scheme_count;

static struct uui_listbox    g_schemes;
static struct uui_radio_list g_cursor;
static struct uui_checkbox   g_use_sys_font;
static struct uui_spinbox    g_font, g_sb, g_margin;
static struct uui_checkbox   g_blink, g_menubar, g_copysel, g_scrollout, g_confirm;

static struct uui_label g_l_scheme, g_l_cursor, g_l_font, g_l_sb, g_l_margin;

static const char *const CURSOR_LABELS[] = { "Block", "Underline", "Bar" };

// One row of "caption, control". A UUI_ROW rather than a grid, whose
// cells are uniform and would size the spinboxes to the widest caption
// -- but the captions themselves are PINNED to one width through
// uui_item.main_size, or three rows of "caption spinbox" step raggedly
// rightwards with nothing lining up.
static struct uui_layout g_row_font, g_row_sb, g_row_margin;
static struct uui_item g_row_font_items[] = {
    { .ops = &uui_label_ops,   .widget = &g_l_font, .name = "font-label" },
    { .ops = &uui_spinbox_ops, .widget = &g_font, .id = ID_FONT, .name = "font" },
};
static struct uui_item g_row_sb_items[] = {
    { .ops = &uui_label_ops,   .widget = &g_l_sb, .name = "sb-label" },
    { .ops = &uui_spinbox_ops, .widget = &g_sb, .id = ID_SB, .name = "scrollback" },
};
static struct uui_item g_row_margin_items[] = {
    { .ops = &uui_label_ops,   .widget = &g_l_margin, .name = "margin-label" },
    { .ops = &uui_spinbox_ops, .widget = &g_margin, .id = ID_MARGIN, .name = "margin" },
};

static struct uui_layout g_left, g_right, g_body_layout;

// UUI_FILL_H on the listbox is what makes the left column give its
// leftover height to the list rather than to the gaps -- and what stops
// a too-tall list evicting the cursor control below it, since a FILL
// child absorbs a shortfall as well as a surplus (ui/uui_layout.h).
static struct uui_item g_left_items[] = {
    { .ops = &uui_label_ops,      .widget = &g_l_scheme, .name = "scheme-label" },
    { .ops = &uui_listbox_ops,    .widget = &g_schemes, .flags = UUI_FILL_W | UUI_FILL_H,
      .id = ID_SCHEME, .name = "schemes" },
    { .ops = &uui_label_ops,      .widget = &g_l_cursor, .name = "cursor-label" },
    { .ops = &uui_radio_list_ops, .widget = &g_cursor, .id = ID_CURSOR, .name = "cursor" },
};

static struct uui_item g_right_items[] = {
    { .ops = &uui_checkbox_ops, .widget = &g_use_sys_font, .id = ID_USE_SYS_FONT, .name = "use-sys-font" },
    { .ops = &uui_layout_ops,   .widget = &g_row_font,   .flags = UUI_FILL_W, .name = "row-font" },
    { .ops = &uui_layout_ops,   .widget = &g_row_sb,     .flags = UUI_FILL_W, .name = "row-sb" },
    { .ops = &uui_layout_ops,   .widget = &g_row_margin, .flags = UUI_FILL_W, .name = "row-margin" },
    { .ops = &uui_checkbox_ops, .widget = &g_blink,     .id = ID_BLINK,     .name = "blink" },
    { .ops = &uui_checkbox_ops, .widget = &g_menubar,   .id = ID_MENUBAR,   .name = "menubar" },
    { .ops = &uui_checkbox_ops, .widget = &g_copysel,   .id = ID_COPYSEL,   .name = "copy-on-select" },
    { .ops = &uui_checkbox_ops, .widget = &g_scrollout, .id = ID_SCROLLOUT, .name = "scroll-on-output" },
    { .ops = &uui_checkbox_ops, .widget = &g_confirm,   .id = ID_CONFIRM,   .name = "confirm-close" },
};

static struct uui_item g_body_items[] = {
    { .ops = &uui_layout_ops, .widget = &g_left,  .flags = UUI_FILL_H, .name = "left" },
    { .ops = &uui_layout_ops, .widget = &g_right, .flags = UUI_FILL_H, .name = "right" },
};

static struct uui_item g_body_item = { .ops = &uui_layout_ops, .widget = &g_body_layout,
                                        .name = "prefs-body" };

static int g_committed;   // CMD_OK once, taken by term_prefs_take()

#define COUNT(a) ((int)(sizeof (a) / sizeof (a)[0]))

// --- building it ------------------------------------------------------

static void init_layouts(void) {
    g_row_font.dir = UUI_ROW;
    g_row_font.margin = 0;
    g_row_font.items = g_row_font_items;
    g_row_font.count = COUNT(g_row_font_items);
    g_row_sb = g_row_font;
    g_row_sb.items = g_row_sb_items;
    g_row_sb.count = COUNT(g_row_sb_items);
    g_row_margin = g_row_font;
    g_row_margin.items = g_row_margin_items;
    g_row_margin.count = COUNT(g_row_margin_items);

    g_left.dir = UUI_COLUMN;
    g_left.margin = 0;
    g_left.items = g_left_items;
    g_left.count = COUNT(g_left_items);

    g_right.dir = UUI_COLUMN;
    g_right.margin = 0;
    g_right.items = g_right_items;
    g_right.count = COUNT(g_right_items);

    g_body_layout.dir = UUI_ROW;
    g_body_layout.margin = 1;   // the dialog's own padding is the moat
    g_body_layout.items = g_body_items;
    g_body_layout.count = COUNT(g_body_items);
}

void term_prefs_init(void) {
    uui_dialog_init(&g_dlg);

    uui_label_init(&g_l_scheme, "Colour scheme");
    uui_label_init(&g_l_cursor, "Cursor");
    uui_label_init(&g_l_font,   "Size");
    uui_label_init(&g_l_sb,     "Scrollback");
    uui_label_init(&g_l_margin, "Margin");

    uui_listbox_init(&g_schemes, 0, 0, 0, 0, g_label_ptr, 0);

    g_cursor.options = CURSOR_LABELS;
    g_cursor.count = COUNT(CURSOR_LABELS);
    g_cursor.cols = 1;
    g_cursor.selected = 0;
    g_cursor.hovered = -1;
    g_cursor.bg = UTHEME_PANEL_BG;
    g_cursor.fg = UTHEME_TEXT;

    uui_checkbox_init(&g_use_sys_font, 0, 0, 0, "Use the desktop's size",
                       UTHEME_PANEL_BG, UTHEME_TEXT);
    uui_checkbox_init(&g_blink,     0, 0, 0, "Blink the cursor",        UTHEME_PANEL_BG, UTHEME_TEXT);
    uui_checkbox_init(&g_menubar,   0, 0, 0, "Show the menu bar",       UTHEME_PANEL_BG, UTHEME_TEXT);
    uui_checkbox_init(&g_copysel,   0, 0, 0, "Copy on select",          UTHEME_PANEL_BG, UTHEME_TEXT);
    uui_checkbox_init(&g_scrollout, 0, 0, 0, "Scroll on output",        UTHEME_PANEL_BG, UTHEME_TEXT);
    uui_checkbox_init(&g_confirm,   0, 0, 0, "Ask when closing tabs",   UTHEME_PANEL_BG, UTHEME_TEXT);

    uui_spinbox_init(&g_font,   14, TERM_FONT_MIN, TERM_FONT_MAX, 1, "px");
    uui_spinbox_init(&g_sb,    240, TERM_SB_MIN, TERM_SB_MAX, 100, "lines");
    uui_spinbox_init(&g_margin,  6, TERM_MARGIN_MIN, TERM_MARGIN_MAX, 1, "px");

    init_layouts();
}

static struct uui_item g_dlg_item = { .ops = &uui_dialog_ops, .widget = &g_dlg,
                                       .id = ID_PREFS, .name = "prefs" };

struct uui_item *term_prefs_item(void) { return &g_dlg_item; }

int term_prefs_is_open(void) { return uui_dialog_is_open(&g_dlg); }

void term_prefs_set_bounds(int x, int y, int w, int h) {
    uui_dialog_set_bounds(&g_dlg, x, y, w, h);
}

// --- opening ----------------------------------------------------------

// A DISABLED CONTROL OWES THE USER A REASON, and here the reason is the
// checkbox directly above it -- which is why the two are adjacent and
// the caption says "the desktop's size" rather than "system".
static void sync_font_enable(void) {
    g_font.disabled = g_use_sys_font.checked;
}

static int scheme_index(const char *name) {
    for (int i = 0; i < g_scheme_count; i++)
        if (strcmp(g_names[i], name) == 0) return i;
    return -1;
}

void term_prefs_open(const struct term_conf *c) {
    static const struct uui_dialog_button btns[] = {
        { "OK", CMD_OK }, { "Cancel", CMD_CANCEL },
    };

    // The directory is read EVERY open rather than once at startup: a
    // scheme dropped in while the window was up should be offered, and
    // the read is a dozen small files behind a user gesture.
    g_scheme_count = term_scheme_list(g_names, g_labels, TERM_SCHEME_MAX);
    for (int i = 0; i < g_scheme_count; i++) g_label_ptr[i] = g_labels[i];
    uui_listbox_set_items(&g_schemes, g_label_ptr, g_scheme_count);
    g_schemes.selected = scheme_index(c->scheme);
    g_schemes.top = 0;

    g_cursor.selected = c->cursor;
    g_use_sys_font.checked = c->font_size == 0;
    // While following the desktop, the spinbox shows the size it is
    // following -- the SETTING, not ugfx_char_h(), which is the line
    // height and a few pixels taller than the size that produced it.
    int sys_px = 14;
    usetting_get_int("system.font_size", &sys_px);
    uui_spinbox_set_value(&g_font, c->font_size ? c->font_size : sys_px);
    sync_font_enable();
    uui_spinbox_set_value(&g_sb, c->scrollback);
    uui_spinbox_set_value(&g_margin, c->margin);
    g_blink.checked     = c->cursor_blink;
    g_menubar.checked   = c->menubar;
    g_copysel.checked   = c->copy_on_select;
    g_scrollout.checked = c->scroll_on_output;
    g_confirm.checked   = c->confirm_close;

    // The caption column: the widest of the three, so the spinboxes
    // line up. Here rather than in init() because it is font-derived
    // and the font is not up when main() runs.
    int capw = 0;
    struct uui_label *caps[] = { &g_l_font, &g_l_sb, &g_l_margin };
    for (int i = 0; i < COUNT(caps); i++) {
        int w, h;
        uui_label_natural_size(caps[i], &w, &h);
        if (w > capw) capw = w;
    }
    g_row_font_items[0].main_size = capw;
    g_row_sb_items[0].main_size = capw;
    g_row_margin_items[0].main_size = capw;

    // FONT-DERIVED, never a pixel pair: the dialog clamps this to the
    // window, so asking for what the controls want and letting it
    // shrink is right in both directions.
    int lw, lh, rw, rh;
    uui_layout_natural_size(&g_left, &lw, &lh);
    uui_layout_natural_size(&g_right, &rw, &rh);
    int body_w = lw + rw + uui_layout_gap(&g_body_layout) + 2 * uui_layout_margin(&g_body_layout);
    int body_h = (lh > rh ? lh : rh) + 2 * uui_layout_margin(&g_body_layout);
    // Room for a few more scheme rows than the list's own minimum, so
    // the common case does not open with a scrollbar over four entries.
    body_h += uui_listbox_row_h(&g_schemes) * 3;

    g_committed = 0;
    uui_dialog_set_body(&g_dlg, &g_body_item, body_w, body_h);
    uui_dialog_open(&g_dlg, "Terminal Preferences", 0, 0, btns, 2, 0, CMD_CANCEL);
    uui_dialog_focus(&g_dlg, &g_left_items[1]);   // the scheme list
    ulogf("uterm: prefs open %d scheme(s)\n", g_scheme_count);
}

// --- reading it back --------------------------------------------------

// The router names the widget that changed; a press moves the key focus
// onto it, which is what makes Tab and the arrows continue from where
// the mouse left off.
void term_prefs_widget(int id, int reason) {
    if (id == ID_PREFS) {
        int code = uui_dialog_take_code(&g_dlg);
        if (code == CMD_OK) g_committed = 1;
        return;
    }
    if (id == ID_USE_SYS_FONT) sync_font_enable();
    if (reason != UUI_REASON_PRESS) return;
    for (int i = 0; i < COUNT(g_left_items); i++)
        if (g_left_items[i].id == id) { uui_dialog_focus(&g_dlg, &g_left_items[i]); return; }
    for (int i = 0; i < COUNT(g_right_items); i++)
        if (g_right_items[i].id == id) { uui_dialog_focus(&g_dlg, &g_right_items[i]); return; }
}

int term_prefs_take(struct term_conf *out) {
    if (!g_committed) return 0;
    g_committed = 0;

    if (g_schemes.selected >= 0 && g_schemes.selected < g_scheme_count)
        strlcpy(out->scheme, g_names[g_schemes.selected], sizeof out->scheme);
    out->cursor = g_cursor.selected >= 0 ? g_cursor.selected : TERM_CURSOR_BLOCK;
    // The spinbox commits a half-typed number on its own (uui_spinbox.h);
    // asking it to do so here is what stops "1000" typed and not
    // Entered from being read as the value it had before.
    uui_spinbox_commit(&g_font);
    uui_spinbox_commit(&g_sb);
    uui_spinbox_commit(&g_margin);
    out->font_size = g_use_sys_font.checked ? 0 : uui_spinbox_value(&g_font);
    out->scrollback = uui_spinbox_value(&g_sb);
    out->margin = uui_spinbox_value(&g_margin);
    out->cursor_blink     = g_blink.checked;
    out->menubar          = g_menubar.checked;
    out->copy_on_select   = g_copysel.checked;
    out->scroll_on_output = g_scrollout.checked;
    out->confirm_close    = g_confirm.checked;
    return 1;
}
