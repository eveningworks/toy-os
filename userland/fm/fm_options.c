// Options: the File Manager's preferences and the window that edits them
// (See more > Options) -- the shared Options window (ui/uui_prefs.h) over
// one lib/uprefs.h table, Dolphin's Configure dialog's shape. Stored in
// FILES_CONF beside the rest of the app's state; OK writes and applies,
// Cancel forgets.
//
// One of the File Manager's units -- see fm_internal.h for what is
// where and why these share their state directly.
#include "fm_internal.h"
#include "ui/uui_prefs.h"
#include "ui/uui_segmented.h"
#include "ui/uui_sidebar.h"
#include "lib/uconf.h"
#include "lib/ulaunch.h"
#include "lib/uprefs.h"
#include "ui/ulog.h"
#include <stddef.h>
#include <string.h>

struct fm_options g_opt;

// --- the file -------------------------------------------------------------

static const char *const START_WORDS[]  = { "last", "home", "root" };          // FM_START_*
static const char *const CLICK_WORDS[]  = { "double", "single" };
static const char *const VIEW_WORDS[]   = { "icons", "large", "details" };     // FM_VIEW_*
static const char *const RENAME_WORDS[] = { "inplace", "dialog" };

// The defaults: what Explorer and Dolphin ship.
#define C struct fm_options
static const struct upref KEYS[] = {
    UPREF_WORD_KEY("start",          C, start,          START_WORDS, 3, FM_START_LAST),
    UPREF_WORD_KEY("click",          C, single_click,   CLICK_WORDS, 2, 0),
    UPREF_WORD_KEY("view",           C, view,           VIEW_WORDS, 3, FM_VIEW_ICONS),
    UPREF_BOOL_KEY("thumbs",         C, thumbs,         1),
    UPREF_BOOL_KEY("hidden",         C, hidden,         0),
    UPREF_BOOL_KEY("extensions",     C, extensions,     1),
    UPREF_WORD_KEY("rename",         C, rename_dialog,  RENAME_WORDS, 2, 0),
    UPREF_BOOL_KEY("confirm_delete", C, confirm_delete, 1),
};
#undef C
static const struct uprefs PREFS = { FILES_CONF, KEYS, (int)(sizeof KEYS / sizeof KEYS[0]) };

void options_load(void) {
    memset(&g_opt, 0, sizeof g_opt);
    uprefs_load(&PREFS, &g_opt);
}

static void save(void) {
    uprefs_save(&PREFS, &g_opt, 0);
    // "New windows use" is what the panes open in, so it is their view too.
    uconf_set(FILES_CONF, "left_view", VIEW_WORDS[g_opt.view]);
    uconf_set(FILES_CONF, "right_view", VIEW_WORDS[g_opt.view]);
}

void options_apply(void) {
    for (int i = 0; i < 2; i++) {
        struct uui_fileview *fv = &g_pane[i];
        fv->single_click = g_opt.single_click;
        fv->hide_ext = !g_opt.extensions;
        uui_fileview_set_thumb(fv, g_opt.thumbs ? pane_thumb : 0, 0);
    }
    reload_panes();   // hidden files come and go with the filter
}

// --- the window -----------------------------------------------------------

enum { PAGE_GENERAL, PAGE_VIEW, PAGE_FILES, PAGE_COUNT };
enum { OPT_SCRIPTS = 1, OPT_PROGRAMS };

static const struct uui_sidebar_row PAGE_ROWS[] = {
    { "General",           UUI_SIDEBAR_TOP, 0, PAGE_GENERAL },
    { "View",              UUI_SIDEBAR_TOP, 0, PAGE_VIEW },
    { "Rename and delete", UUI_SIDEBAR_TOP, 0, PAGE_FILES },
};
static const char *const START_OPTS[]  = { "Last folder", "Home", "System" };
static const char *const CLICK_OPTS[]  = { "Double click", "Single click" };
// Shown in the order a person reads sizes; FM_VIEW_* is not that order.
static const char *const VIEW_OPTS[]   = { "Large icons", "Icons", "Details" };
static const int VIEW_VALUES[] = { FM_VIEW_LARGE, FM_VIEW_ICONS, FM_VIEW_DETAILS };
static const char *const RENAME_OPTS[] = { "In place", "In a dialog" };
// INDEXED BY ULAUNCH_* ACT. Not the File Manager's own setting: the
// desktop and `open` read it too (lib/ulaunch.h), so OK writes it to
// /etc/mimeapps.conf rather than FILES_CONF -- which is why these two
// rows are the app's and not bound.
static const char *const SCRIPT_OPTS[]  = { "Ask", "Run in Terminal", "Run", "Edit" };
static const char *const PROGRAM_OPTS[] = { "Ask", "Run in Terminal", "Run" };

static struct fm_options g_edit;   // what the window shows, until OK
static struct uui_segmented g_scripts, g_programs;

static void edit_defaults(void *edit) {
    memset(edit, 0, sizeof g_edit);
    uprefs_defaults(&PREFS, edit);
}

static void on_defaults(void) { g_scripts.selected = g_programs.selected = ULAUNCH_ASK; }

static void on_ok(void) {
    int view_changed = g_edit.view != g_opt.view;
    g_opt = g_edit;
    save();
    if (!ulaunch_set_policy(ULAUNCH_SCRIPT, g_scripts.selected) ||
        !ulaunch_set_policy(ULAUNCH_PROGRAM, g_programs.selected))
        ulog("files: could not save what a double-click runs\n");
    if (view_changed)
        for (int i = 0; i < 2; i++) {
            g_pane[i].icon_px = g_opt.view == FM_VIEW_LARGE ? ugfx_char_h() * 6 : 0;
            uui_fileview_set_mode(&g_pane[i], g_opt.view == FM_VIEW_DETAILS
                                                  ? UUI_FILEVIEW_DETAILS : UUI_FILEVIEW_ICONS);
        }
    options_apply();
    refresh_status();
    uapp_redraw(g_app);
}

void options_open(struct uapp *a) {
    if (uui_prefs_is_open()) return;   // one at a time
    g_edit = g_opt;
    uui_segmented_init(&g_scripts, SCRIPT_OPTS, 4, ulaunch_policy(ULAUNCH_SCRIPT));
    uui_segmented_init(&g_programs, PROGRAM_OPTS, 3, ulaunch_policy(ULAUNCH_PROGRAM));

    uui_prefs_begin(&(struct uui_prefs_desc){
        .title = "File Manager Options",
        .pages = PAGE_ROWS,
        .page_count = PAGE_COUNT,
        .w = ugfx_char_advance('n') * 88,
        .h = ugfx_char_h() * 22,
        .log_prefix = "options",
        .edit = &g_edit,
        .edit_defaults = edit_defaults,
        .on_defaults = on_defaults,
        .on_ok = on_ok,
    });
#define F(field) offsetof(struct fm_options, field)
    uui_prefs_choice(PAGE_GENERAL, "Open in:", START_OPTS, 0, 3, F(start), "start");
    uui_prefs_choice(PAGE_GENERAL, "Open items with:", CLICK_OPTS, 0, 2, F(single_click), "click");
    uui_prefs_row(PAGE_GENERAL, "Scripts:", &uui_segmented_ops, &g_scripts, OPT_SCRIPTS, "scripts", 0);
    uui_prefs_row(PAGE_GENERAL, "Programs:", &uui_segmented_ops, &g_programs, OPT_PROGRAMS, "programs", 0);
    uui_prefs_choice(PAGE_VIEW, "New windows use:", VIEW_OPTS, VIEW_VALUES, 3, F(view), "view");
    uui_prefs_check(PAGE_VIEW, "Pictures:", "Show thumbnails", F(thumbs), "thumbs");
    uui_prefs_check(PAGE_VIEW, "Hidden files:", "Show names that start with a dot", F(hidden), "hidden");
    uui_prefs_check(PAGE_VIEW, "Names:", "Show file extensions", F(extensions), "ext");
    uui_prefs_choice(PAGE_FILES, "Rename files:", RENAME_OPTS, 0, 2, F(rename_dialog), "rename");
    uui_prefs_check(PAGE_FILES, "Deleting:", "Ask before moving to the Recycle Bin", F(confirm_delete), "confirm");
#undef F
    if (!uui_prefs_open(a)) ulog("files: could not open Options\n");
}
