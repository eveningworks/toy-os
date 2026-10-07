// Notepad's Options window: three pages over the shared dialog
// (ui/uui_prefs.h), turning a struct np_conf into controls and back.
#include "notepad.h"

#include <stddef.h>
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

// The rows the window cannot bind: a font size where 0 means "the
// desktop's", shown as a number and a checkbox, and a command button.
static struct uui_checkbox g_sysfont;
static struct uui_spinbox g_font;
static struct uui_button g_clear;

static const int TABW_VALUES[] = { 2, 4, 8 };
static const int ON_OFF[] = { 1, 0 };

static void font_to_controls(void) {
    int sys_px = 14;
    usetting_get_int("system.font_size", &sys_px);
    g_sysfont.checked = g_edit.font_size == 0;
    uui_spinbox_set_value(&g_font, g_edit.font_size ? g_edit.font_size : sys_px);
    g_font.disabled = g_sysfont.checked;
}

static void edit_defaults(void *edit) { np_conf_defaults(edit); }
static void on_defaults(void) { font_to_controls(); }

static void on_ok(void) {
    uui_spinbox_commit(&g_font);
    g_edit.font_size = g_sysfont.checked ? 0 : uui_spinbox_value(&g_font);
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

    uui_checkbox_init(&g_sysfont, 0, 0, 0, "Use the desktop's size", UTHEME_WINDOW_BG, UTHEME_TEXT);
    uui_spinbox_init(&g_font, 14, NP_FONT_MIN, NP_FONT_MAX, 1, "px");
    uui_button_init(&g_clear, 0, 0, 0, 0, "Clear list", UTHEME_BUTTON_BG, UTHEME_TEXT, OPT_CLEAR);
    font_to_controls();

    uui_prefs_begin(&(struct uui_prefs_desc){
        .title = "Notepad Options",
        .pages = PAGE_ROWS,
        .page_count = PAGE_COUNT,
        .log_prefix = "options",
        .edit = &g_edit,
        .edit_defaults = edit_defaults,
        .on_defaults = on_defaults,
        .on_ok = on_ok,
        .on_widget = on_widget,
        .on_action = on_action,
    });
#define F(field) offsetof(struct np_conf, field)
    int r;
    r = uui_prefs_row(PAGE_EDITOR, "Font size:", &uui_spinbox_ops, &g_font, OPT_FONT, "font", 0);
    uui_prefs_also(r, &uui_checkbox_ops, &g_sysfont, OPT_SYSFONT, "sysfont");
    // CHECKBOXES STACK under their caption, as the mockup has them: two
    // side by side ran off the window's right edge.
    uui_prefs_choice(PAGE_EDITOR, "Tab width:", TABW_OPTS, TABW_VALUES, 3, F(tab_width), "tabwidth");
    uui_prefs_check(PAGE_EDITOR, "", "Insert spaces when Tab is pressed", F(tab_spaces), "tabspaces");
    uui_prefs_check(PAGE_EDITOR, "Typing:", "Keep the indent of the line above", F(auto_indent), "indent");
    uui_prefs_check(PAGE_EDITOR, "", "Show spaces and tabs", F(show_ws), "whitespace");
    uui_prefs_choice(PAGE_EDITOR, "Word wrap:", ONOFF_OPTS, ON_OFF, 2, F(wrap), "wrap");

    uui_prefs_check(PAGE_VIEW, "Show:", "Line numbers", F(linenums), "linenums");
    uui_prefs_check(PAGE_VIEW, "", "Highlight the current line", F(line_highlight), "linehighlight");
    uui_prefs_check(PAGE_VIEW, "", "Command bar", F(cmdbar), "cmdbar");
    uui_prefs_check(PAGE_VIEW, "", "Status bar", F(statusbar), "statusbar");
    uui_prefs_check(PAGE_VIEW, "", "Menu bar (F10 shows it)", F(menubar), "menubar");
    uui_prefs_choice(PAGE_VIEW, "Markdown preview:", PREVIEW_OPTS, 0, 3, F(preview), "preview");
    uui_prefs_choice(PAGE_VIEW, "Preview position:", PREVPOS_OPTS, 0, 2, F(preview_below), "previewpos");
    uui_prefs_check(PAGE_VIEW, "Scrolling:", "Keep 3 lines visible above and below the cursor",
                    F(scroll_margin), "margin");

    uui_prefs_choice(PAGE_FILES, "When Notepad starts:", START_OPTS, 0, 2, F(reopen), "startup");
    uui_prefs_choice(PAGE_FILES, "Open files in:", OPENIN_OPTS, 0, 2, F(open_window), "openin");
    uui_prefs_choice(PAGE_FILES, "New files end lines:", EOL_OPTS, 0, 2, F(crlf_new), "eol");
    r = uui_prefs_number(PAGE_FILES, "Recent files:", 0, NP_RECENT_MAX, 1, "files", F(recent_max), "recent");
    uui_prefs_also(r, &uui_button_ops, &g_clear, OPT_CLEAR, "clear");
    uui_prefs_check(PAGE_FILES, "Saving:", "Remove spaces at the ends of lines", F(trim_trailing), "trim");
    uui_prefs_check(PAGE_FILES, "", "End the file with a newline", F(final_newline), "newline");
    uui_prefs_check(PAGE_FILES, "Closing:", "Ask before closing several tabs", F(confirm_close), "confirm");
#undef F
    uui_prefs_open(a);
    ulog("notepad: options open\n");
}
