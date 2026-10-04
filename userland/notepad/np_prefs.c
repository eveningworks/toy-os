// Notepad's Options window: three pages over the shared dialog
// (ui/uui_prefs.h), turning a struct np_conf into controls and back.
#include "notepad.h"

#include <string.h>

#include "lib/usetting.h"
#include "ui/uapp.h"
#include "ui/ulog.h"
#include "ui/utheme.h"
#include "ui/uui_button.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_prefs.h"
#include "ui/uui_route.h"
#include "ui/uui_segmented.h"
#include "ui/uui_sidebar.h"
#include "ui/uui_spinbox.h"

enum { PAGE_EDITOR, PAGE_VIEW, PAGE_FILES, PAGE_COUNT };
enum {
    OPT_SYSFONT = 1, OPT_FONT, OPT_TABW, OPT_TABSP, OPT_INDENT, OPT_WS, OPT_WRAP,
    OPT_LINENUMS, OPT_LINEHL, OPT_CMDBAR, OPT_STATUS, OPT_MENUBAR, OPT_PREVIEW, OPT_PREVPOS,
    OPT_MARGIN, OPT_REOPEN, OPT_OPENIN, OPT_EOL, OPT_RECENT, OPT_CLEAR, OPT_TRIM, OPT_NEWLINE,
    OPT_CONFIRM,
};

static const struct uui_sidebar_row PAGE_ROWS[] = {
    { "Editor",            UUI_SIDEBAR_TOP, 0, PAGE_EDITOR },
    { "View",              UUI_SIDEBAR_TOP, 0, PAGE_VIEW },
    { "Files and startup", UUI_SIDEBAR_TOP, 0, PAGE_FILES },
};
static const char *const TABW_OPTS[] = { "2", "4", "8" };
static const char *const ONOFF_OPTS[] = { "On", "Off" };
static const char *const PREVIEW_OPTS[] = { "For .md files", "Always", "Never" };
static const char *const PREVPOS_OPTS[] = { "Right", "Below" };
static const char *const START_OPTS[] = { "Open a new tab", "Reopen the last tabs" };
static const char *const OPENIN_OPTS[] = { "A new tab", "A new window" };
static const char *const EOL_OPTS[] = { "LF", "CRLF" };

static struct np_conf g_edit;
static void (*g_on_commit)(const struct np_conf *next);

static struct uui_checkbox g_sysfont, g_tabsp, g_indent, g_ws, g_linenums, g_linehl, g_cmdbar,
                           g_status, g_menubar, g_margin, g_trim, g_newline, g_confirm;
static struct uui_spinbox g_font, g_recent;
static struct uui_segmented g_tabw, g_wrap, g_preview, g_prevpos, g_reopen, g_openin, g_eol;
static struct uui_button g_clear;

static int tabw_index(int w) { return w == 2 ? 0 : w == 8 ? 2 : 1; }

static void to_controls(void) {
    int sys_px = 14;
    usetting_get_int("system.font_size", &sys_px);
    g_sysfont.checked = g_edit.font_size == 0;
    uui_spinbox_set_value(&g_font, g_edit.font_size ? g_edit.font_size : sys_px);
    g_font.disabled = g_sysfont.checked;
    g_tabw.selected = tabw_index(g_edit.tab_width);
    g_tabsp.checked = g_edit.tab_spaces;
    g_indent.checked = g_edit.auto_indent;
    g_ws.checked = g_edit.show_ws;
    g_wrap.selected = g_edit.wrap ? 0 : 1;
    g_linenums.checked = g_edit.linenums;
    g_linehl.checked = g_edit.line_highlight;
    g_cmdbar.checked = g_edit.cmdbar;
    g_status.checked = g_edit.statusbar;
    g_menubar.checked = g_edit.menubar;
    g_preview.selected = g_edit.preview;
    g_prevpos.selected = g_edit.preview_below ? 1 : 0;
    g_margin.checked = g_edit.scroll_margin;
    g_reopen.selected = g_edit.reopen ? 1 : 0;
    g_openin.selected = g_edit.open_window ? 1 : 0;
    g_eol.selected = g_edit.crlf_new ? 1 : 0;
    uui_spinbox_set_value(&g_recent, g_edit.recent_max);
    g_trim.checked = g_edit.trim_trailing;
    g_newline.checked = g_edit.final_newline;
    g_confirm.checked = g_edit.confirm_close;
}

static void from_controls(void) {
    uui_spinbox_commit(&g_font);
    uui_spinbox_commit(&g_recent);
    g_edit.font_size = g_sysfont.checked ? 0 : uui_spinbox_value(&g_font);
    static const int TABW[] = { 2, 4, 8 };
    g_edit.tab_width = TABW[g_tabw.selected >= 0 && g_tabw.selected < 3 ? g_tabw.selected : 1];
    g_edit.tab_spaces = g_tabsp.checked;
    g_edit.auto_indent = g_indent.checked;
    g_edit.show_ws = g_ws.checked;
    g_edit.wrap = g_wrap.selected == 0;
    g_edit.linenums = g_linenums.checked;
    g_edit.line_highlight = g_linehl.checked;
    g_edit.cmdbar = g_cmdbar.checked;
    g_edit.statusbar = g_status.checked;
    g_edit.menubar = g_menubar.checked;
    g_edit.preview = g_preview.selected >= 0 ? g_preview.selected : NP_PREVIEW_MD;
    g_edit.preview_below = g_prevpos.selected == 1;
    g_edit.scroll_margin = g_margin.checked;
    g_edit.reopen = g_reopen.selected == 1;
    g_edit.open_window = g_openin.selected == 1;
    g_edit.crlf_new = g_eol.selected == 1;
    g_edit.recent_max = uui_spinbox_value(&g_recent);
    g_edit.trim_trailing = g_trim.checked;
    g_edit.final_newline = g_newline.checked;
    g_edit.confirm_close = g_confirm.checked;
}

static void on_defaults(void) {
    np_conf_defaults(&g_edit);
    to_controls();
}

static void on_ok(void) {
    from_controls();
    if (g_on_commit) g_on_commit(&g_edit);
}

static void on_widget(int id, int reason) {
    (void)reason;
    if (id == OPT_SYSFONT) g_font.disabled = g_sysfont.checked;
}

int np_prefs_is_open(void) { return uui_prefs_is_open(); }

// "Clear list" acts at once, as Windows' and GNOME's do -- it is a
// command, not a setting to stage, so Cancel does not bring the list back.
static void (*g_clear_fn)(void);

static void on_action(int code) {
    if (code != OPT_CLEAR) return;
    if (g_clear_fn) g_clear_fn();
    g_clear.disabled = 1;
}

void np_prefs_open(struct uapp *a, const struct np_conf *c,
                   void (*on_commit)(const struct np_conf *next),
                   void (*on_clear_recent)(void)) {
    if (uui_prefs_is_open()) return;
    g_edit = *c;
    g_on_commit = on_commit;
    g_clear_fn = on_clear_recent;

    uint32_t bg = UTHEME_WINDOW_BG, fg = UTHEME_TEXT;
    uui_checkbox_init(&g_sysfont,  0, 0, 0, "Use the desktop's size", bg, fg);
    uui_checkbox_init(&g_tabsp,    0, 0, 0, "Insert spaces when Tab is pressed", bg, fg);
    uui_checkbox_init(&g_indent,   0, 0, 0, "Keep the indent of the line above", bg, fg);
    uui_checkbox_init(&g_ws,       0, 0, 0, "Show spaces and tabs", bg, fg);
    uui_checkbox_init(&g_linenums, 0, 0, 0, "Line numbers", bg, fg);
    uui_checkbox_init(&g_linehl,   0, 0, 0, "Highlight the current line", bg, fg);
    uui_checkbox_init(&g_cmdbar,   0, 0, 0, "Command bar", bg, fg);
    uui_checkbox_init(&g_status,   0, 0, 0, "Status bar", bg, fg);
    uui_checkbox_init(&g_menubar,  0, 0, 0, "Menu bar (F10 shows it)", bg, fg);
    uui_checkbox_init(&g_margin,   0, 0, 0, "Keep 3 lines visible above and below the cursor", bg, fg);
    uui_checkbox_init(&g_trim,     0, 0, 0, "Remove spaces at the ends of lines", bg, fg);
    uui_checkbox_init(&g_newline,  0, 0, 0, "End the file with a newline", bg, fg);
    uui_checkbox_init(&g_confirm,  0, 0, 0, "Ask before closing several tabs", bg, fg);
    uui_spinbox_init(&g_font, 14, NP_FONT_MIN, NP_FONT_MAX, 1, "px");
    uui_spinbox_init(&g_recent, 8, 0, NP_RECENT_MAX, 1, "files");
    uui_segmented_init(&g_tabw, TABW_OPTS, 3, 1);
    uui_segmented_init(&g_wrap, ONOFF_OPTS, 2, 0);
    uui_segmented_init(&g_preview, PREVIEW_OPTS, 3, 0);
    uui_segmented_init(&g_prevpos, PREVPOS_OPTS, 2, 0);
    uui_segmented_init(&g_reopen, START_OPTS, 2, 0);
    uui_segmented_init(&g_openin, OPENIN_OPTS, 2, 0);
    uui_segmented_init(&g_eol, EOL_OPTS, 2, 0);
    uui_button_init(&g_clear, 0, 0, 0, 0, "Clear list", UTHEME_BUTTON_BG, UTHEME_TEXT, OPT_CLEAR);
    to_controls();

    uui_prefs_begin(&(struct uui_prefs_desc){
        .title = "Notepad Options",
        .pages = PAGE_ROWS,
        .page_count = PAGE_COUNT,
        .log_prefix = "options",
        .on_defaults = on_defaults,
        .on_ok = on_ok,
        .on_widget = on_widget,
        .on_action = on_action,
    });
    int r;
    r = uui_prefs_row(PAGE_EDITOR, "Font size:", &uui_spinbox_ops, &g_font, OPT_FONT, "font", 0);
    uui_prefs_also(r, &uui_checkbox_ops, &g_sysfont, OPT_SYSFONT, "sysfont");
    // CHECKBOXES STACK under their caption, as the mockup has them: two
    // side by side ran off the window's right edge.
    uui_prefs_row(PAGE_EDITOR, "Tab width:", &uui_segmented_ops, &g_tabw, OPT_TABW, "tabwidth", 0);
    uui_prefs_row(PAGE_EDITOR, "", &uui_checkbox_ops, &g_tabsp, OPT_TABSP, "tabspaces", 0);
    uui_prefs_row(PAGE_EDITOR, "Typing:", &uui_checkbox_ops, &g_indent, OPT_INDENT, "indent", 0);
    uui_prefs_row(PAGE_EDITOR, "", &uui_checkbox_ops, &g_ws, OPT_WS, "whitespace", 0);
    uui_prefs_row(PAGE_EDITOR, "Word wrap:", &uui_segmented_ops, &g_wrap, OPT_WRAP, "wrap", 0);

    uui_prefs_row(PAGE_VIEW, "Show:", &uui_checkbox_ops, &g_linenums, OPT_LINENUMS, "linenums", 0);
    uui_prefs_row(PAGE_VIEW, "", &uui_checkbox_ops, &g_linehl, OPT_LINEHL, "linehighlight", 0);
    uui_prefs_row(PAGE_VIEW, "", &uui_checkbox_ops, &g_cmdbar, OPT_CMDBAR, "cmdbar", 0);
    uui_prefs_row(PAGE_VIEW, "", &uui_checkbox_ops, &g_status, OPT_STATUS, "statusbar", 0);
    uui_prefs_row(PAGE_VIEW, "", &uui_checkbox_ops, &g_menubar, OPT_MENUBAR, "menubar", 0);
    uui_prefs_row(PAGE_VIEW, "Markdown preview:", &uui_segmented_ops, &g_preview, OPT_PREVIEW, "preview", 0);
    uui_prefs_row(PAGE_VIEW, "Preview position:", &uui_segmented_ops, &g_prevpos, OPT_PREVPOS, "previewpos", 0);
    uui_prefs_row(PAGE_VIEW, "Scrolling:", &uui_checkbox_ops, &g_margin, OPT_MARGIN, "margin", 0);

    uui_prefs_row(PAGE_FILES, "When Notepad starts:", &uui_segmented_ops, &g_reopen, OPT_REOPEN, "startup", 0);
    uui_prefs_row(PAGE_FILES, "Open files in:", &uui_segmented_ops, &g_openin, OPT_OPENIN, "openin", 0);
    uui_prefs_row(PAGE_FILES, "New files end lines:", &uui_segmented_ops, &g_eol, OPT_EOL, "eol", 0);
    r = uui_prefs_row(PAGE_FILES, "Recent files:", &uui_spinbox_ops, &g_recent, OPT_RECENT, "recent", 0);
    uui_prefs_also(r, &uui_button_ops, &g_clear, OPT_CLEAR, "clear");
    uui_prefs_row(PAGE_FILES, "Saving:", &uui_checkbox_ops, &g_trim, OPT_TRIM, "trim", 0);
    uui_prefs_row(PAGE_FILES, "", &uui_checkbox_ops, &g_newline, OPT_NEWLINE, "newline", 0);
    uui_prefs_row(PAGE_FILES, "Closing:", &uui_checkbox_ops, &g_confirm, OPT_CONFIRM, "confirm", 0);
    uui_prefs_open(a);
    ulog("notepad: options open\n");
}
