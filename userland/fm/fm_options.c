// Options: the File Manager's preferences and the window that edits them
// (See more > Options) -- a sidebar of pages, Dolphin's Configure
// dialog's shape. Stored in FILES_CONF beside the rest of the app's
// state; OK writes and applies, Cancel forgets.
//
// One of the File Manager's units -- see fm_internal.h for what is
// where and why these share their state directly.
#include "fm_internal.h"
#include "ui/uui_sidebar.h"
#include "ui/uui_segmented.h"
#include "ui/uui_checkbox.h"
#include "ui/uui_label.h"
#include "ui/uui_layout.h"
#include "ui/uui_focus.h"
#include "ui/uui_route.h"
#include "ui/utheme.h"
#include "lib/uconf.h"
#include <string.h>

// The defaults: what Explorer and Dolphin ship.
static const struct fm_options DEFAULTS = {
    .start = FM_START_LAST, .single_click = 0, .view = FM_VIEW_ICONS,
    .thumbs = 1, .hidden = 0, .extensions = 1, .rename_dialog = 0,
    .confirm_delete = 1,
};
struct fm_options g_opt;

// --- the file -------------------------------------------------------------

static int get(const char *key, char *out, int cap) {
    return uconf_get(FILES_CONF, key, out, (size_t)cap);
}
static int yes(const char *key, int dflt) {
    char v[8];
    return get(key, v, sizeof v) ? v[0] == '1' : dflt;
}

void options_load(void) {
    char v[16];
    g_opt = DEFAULTS;
    if (get("start", v, sizeof v))
        g_opt.start = !strcmp(v, "home") ? FM_START_HOME : !strcmp(v, "root") ? FM_START_ROOT
                                                                                 : FM_START_LAST;
    if (get("click", v, sizeof v)) g_opt.single_click = !strcmp(v, "single");
    if (get("view", v, sizeof v))
        g_opt.view = !strcmp(v, "large") ? FM_VIEW_LARGE : !strcmp(v, "details") ? FM_VIEW_DETAILS
                                                                                   : FM_VIEW_ICONS;
    g_opt.thumbs = yes("thumbs", DEFAULTS.thumbs);
    g_opt.hidden = yes("hidden", DEFAULTS.hidden);
    g_opt.extensions = yes("extensions", DEFAULTS.extensions);
    if (get("rename", v, sizeof v)) g_opt.rename_dialog = !strcmp(v, "dialog");
    g_opt.confirm_delete = yes("confirm_delete", DEFAULTS.confirm_delete);
}

static void save(void) {
    static const char *const START[] = { "last", "home", "root" };
    static const char *const VIEW[] = { "icons", "large", "details" };
    uconf_set(FILES_CONF, "start", START[g_opt.start]);
    uconf_set(FILES_CONF, "click", g_opt.single_click ? "single" : "double");
    uconf_set(FILES_CONF, "view", VIEW[g_opt.view]);
    // "New windows use" is what the panes open in, so it is their view too.
    uconf_set(FILES_CONF, "left_view", VIEW[g_opt.view]);
    uconf_set(FILES_CONF, "right_view", VIEW[g_opt.view]);
    uconf_set(FILES_CONF, "thumbs", g_opt.thumbs ? "1" : "0");
    uconf_set(FILES_CONF, "hidden", g_opt.hidden ? "1" : "0");
    uconf_set(FILES_CONF, "extensions", g_opt.extensions ? "1" : "0");
    uconf_set(FILES_CONF, "rename", g_opt.rename_dialog ? "dialog" : "inplace");
    uconf_set(FILES_CONF, "confirm_delete", g_opt.confirm_delete ? "1" : "0");
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
enum {
    OPT_PAGES = 1, OPT_START, OPT_CLICK, OPT_VIEWSEL, OPT_THUMBS, OPT_HIDDEN, OPT_EXT,
    OPT_RENAME, OPT_CONFIRM, OPT_DEFAULTS, OPT_OK, OPT_CANCEL,
};

static const struct uui_sidebar_row PAGE_ROWS[] = {
    { "General",           UUI_SIDEBAR_TOP, 0, PAGE_GENERAL },
    { "View",              UUI_SIDEBAR_TOP, 0, PAGE_VIEW },
    { "Rename and delete", UUI_SIDEBAR_TOP, 0, PAGE_FILES },
};
static const char *const START_OPTS[]  = { "Last folder", "Home", "System" };
static const char *const CLICK_OPTS[]  = { "Double click", "Single click" };
// Shown in the order a person reads sizes; FM_VIEW_* is not that order.
static const char *const VIEW_OPTS[]   = { "Large icons", "Icons", "Details" };
static const int VIEW_OF_SEG[] = { FM_VIEW_LARGE, FM_VIEW_ICONS, FM_VIEW_DETAILS };
static const int SEG_OF_VIEW[] = { 1, 0, 2 };
static const char *const RENAME_OPTS[] = { "In place", "In a dialog" };

// One row per setting: its page, a caption and a control.
#define ROWS 8
static struct {
    int page;
    struct uui_label caption;
    struct uui_item items[2];
    struct uui_layout row;
} g_row[ROWS];

static struct uapp_window *g_win;
static struct fm_options g_edit;   // what the window shows, until OK
static struct uui_sidebar g_pages;
static struct uui_label g_heading;
static char g_heading_text[32];
static struct uui_segmented g_start, g_click, g_view, g_rename;
static struct uui_checkbox g_thumbs, g_hidden, g_ext, g_confirm;
static struct uui_button g_defaults, g_ok, g_cancel;
static struct uui_label g_spacer;

static struct uui_item g_page_items[1 + ROWS], g_body_items[2], g_button_items[4], g_root_items[2];
static struct uui_layout g_page_l, g_body_l, g_button_l, g_root_l;
static struct uui_focusable g_focusables[3 + ROWS];
static struct uui_focus g_focus;

static void show_page(int page) {
    strlcpy(g_heading_text, PAGE_ROWS[page].label, sizeof g_heading_text);
    for (int i = 0; i < ROWS; i++) g_page_items[1 + i].hidden = g_row[i].page != page;
}

// The controls from `g_edit`, and back.
static void to_controls(void) {
    g_start.selected = g_edit.start;
    g_click.selected = g_edit.single_click;
    g_view.selected = SEG_OF_VIEW[g_edit.view];
    g_thumbs.checked = g_edit.thumbs;
    g_hidden.checked = g_edit.hidden;
    g_ext.checked = g_edit.extensions;
    g_rename.selected = g_edit.rename_dialog;
    g_confirm.checked = g_edit.confirm_delete;
}
static void from_controls(void) {
    g_edit.start = g_start.selected < 0 ? 0 : g_start.selected;
    g_edit.single_click = g_click.selected == 1;
    g_edit.view = VIEW_OF_SEG[g_view.selected < 0 ? 1 : g_view.selected];
    g_edit.thumbs = g_thumbs.checked;
    g_edit.hidden = g_hidden.checked;
    g_edit.extensions = g_ext.checked;
    g_edit.rename_dialog = g_rename.selected == 1;
    g_edit.confirm_delete = g_confirm.checked;
}

static void close_window(void) {
    if (g_win) uapp_window_close(g_win);
    g_win = 0;
}

static void on_widget(struct uapp_window *w, int id, int reason) {
    (void)reason;
    if (id == OPT_PAGES) {
        int page = uui_sidebar_selected_id(&g_pages);
        if (page >= 0 && page < PAGE_COUNT) show_page(page);
    }
    uapp_window_redraw(w);
}

// Defaults, OK, Cancel.
static void on_action(struct uapp_window *w, int code) {
    if (code == OPT_DEFAULTS) {
        g_edit = DEFAULTS;
        to_controls();
    } else if (code == OPT_OK) {
        from_controls();
        int view_changed = g_edit.view != g_opt.view;
        g_opt = g_edit;
        save();
        if (view_changed)
            for (int i = 0; i < 2; i++) {
                g_pane[i].icon_px = g_opt.view == FM_VIEW_LARGE ? ugfx_char_h() * 6 : 0;
                uui_fileview_set_mode(&g_pane[i], g_opt.view == FM_VIEW_DETAILS
                                                      ? UUI_FILEVIEW_DETAILS : UUI_FILEVIEW_ICONS);
            }
        options_apply();
        refresh_status();
        uapp_redraw(g_app);
        close_window();
        return;
    } else if (code == OPT_CANCEL) {
        close_window();
        return;
    }
    uapp_window_redraw(w);
}

static void on_key(struct uapp_window *w, int key, unsigned mods) {
    (void)mods;
    // Esc cancels and Return keeps, as in every dialog.
    if (key == 0x1B) { close_window(); return; }
    if (key == '\n' || key == '\r') { on_action(w, OPT_OK); return; }
    uapp_window_redraw(w);
}

static void on_close(struct uapp_window *w) {
    (void)w;
    close_window();
}

static void add_row(int i, int page, const char *caption, const struct uui_widget_ops *ops,
                    void *widget, int id, const char *name) {
    int cw = ugfx_char_advance('n') * 16;
    g_row[i].page = page;
    uui_label_init(&g_row[i].caption, caption);
    g_row[i].items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_row[i].caption,
                                           .flags = UUI_FILL_H, .main_size = cw };
    g_row[i].items[1] = (struct uui_item){ .ops = ops, .widget = widget, .id = id, .name = name };
    g_row[i].row = (struct uui_layout){ .dir = UUI_ROW, .items = g_row[i].items, .count = 2,
                                        .margin = 1 };
    g_page_items[1 + i] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_row[i].row,
                                             .flags = UUI_FILL_W };
    g_focusables[3 + i] = (struct uui_focusable){ widget, ops };
}

void options_open(struct uapp *a) {
    if (g_win) return;   // one at a time
    g_edit = g_opt;

    uui_sidebar_init(&g_pages, 0, 0, 0, 0, PAGE_ROWS, PAGE_COUNT);
    uui_label_init(&g_heading, g_heading_text);
    g_heading.font = ugfx_font_session(UGFX_FONT_BOLD);
    // The page you are on, in the accent -- the mockup's, and Dolphin's.
    g_pages.sel_bg = UTHEME_ACCENT;
    g_pages.sel_fg = UTHEME_ACCENT_TEXT;
    uui_segmented_init(&g_start, START_OPTS, 3, 0);
    uui_segmented_init(&g_click, CLICK_OPTS, 2, 0);
    uui_segmented_init(&g_view, VIEW_OPTS, 3, 1);
    uui_segmented_init(&g_rename, RENAME_OPTS, 2, 0);
    uint32_t bg = UTHEME_WINDOW_BG, fg = UTHEME_TEXT;
    uui_checkbox_init(&g_thumbs, 0, 0, 0, "Show thumbnails", bg, fg);
    uui_checkbox_init(&g_hidden, 0, 0, 0, "Show names that start with a dot", bg, fg);
    uui_checkbox_init(&g_ext, 0, 0, 0, "Show file extensions", bg, fg);
    uui_checkbox_init(&g_confirm, 0, 0, 0, "Ask before moving to the Recycle Bin", bg, fg);
    uui_button_init(&g_defaults, 0, 0, 0, 0, "Defaults", UTHEME_BUTTON_BG, UTHEME_TEXT, OPT_DEFAULTS);
    uui_button_init(&g_ok, 0, 0, 0, 0, "OK", UTHEME_ACCENT, UTHEME_ACCENT_TEXT, OPT_OK);
    uui_button_init(&g_cancel, 0, 0, 0, 0, "Cancel", UTHEME_BUTTON_BG, UTHEME_TEXT, OPT_CANCEL);
    uui_label_init(&g_spacer, "");
    to_controls();

    g_focusables[0] = (struct uui_focusable){ &g_pages, &uui_sidebar_ops };
    g_page_items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_heading,
                                         .flags = UUI_FILL_W, .name = "heading" };
    add_row(0, PAGE_GENERAL, "Open in:",         &uui_segmented_ops, &g_start,   OPT_START,   "start");
    add_row(1, PAGE_GENERAL, "Open items with:", &uui_segmented_ops, &g_click,   OPT_CLICK,   "click");
    add_row(2, PAGE_VIEW,    "New windows use:", &uui_segmented_ops, &g_view,    OPT_VIEWSEL, "view");
    add_row(3, PAGE_VIEW,    "Pictures:",        &uui_checkbox_ops,  &g_thumbs,  OPT_THUMBS,  "thumbs");
    add_row(4, PAGE_VIEW,    "Hidden files:",    &uui_checkbox_ops,  &g_hidden,  OPT_HIDDEN,  "hidden");
    add_row(5, PAGE_VIEW,    "Names:",           &uui_checkbox_ops,  &g_ext,     OPT_EXT,     "ext");
    add_row(6, PAGE_FILES,   "Rename files:",    &uui_segmented_ops, &g_rename,  OPT_RENAME,  "rename");
    add_row(7, PAGE_FILES,   "Deleting:",        &uui_checkbox_ops,  &g_confirm, OPT_CONFIRM, "confirm");
    g_focusables[1] = (struct uui_focusable){ &g_ok, &uui_button_ops };
    g_focusables[2] = (struct uui_focusable){ &g_cancel, &uui_button_ops };
    show_page(PAGE_GENERAL);

    g_page_l = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_page_items, .count = 1 + ROWS,
                                    .margin = 12, .gap = 12 };
    g_body_items[0] = (struct uui_item){ .ops = &uui_sidebar_ops, .widget = &g_pages, .id = OPT_PAGES,
                                         .flags = UUI_FILL_H, .name = "pages",
                                         .main_size = ugfx_char_advance('n') * 20 };
    g_body_items[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_page_l,
                                         .flags = UUI_FILL_W | UUI_FILL_H };
    g_body_l = (struct uui_layout){ .dir = UUI_ROW, .items = g_body_items, .count = 2, .margin = 1 };
    g_button_items[0] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_defaults,
                                           .id = OPT_DEFAULTS, .name = "defaults" };
    g_button_items[1] = (struct uui_item){ .ops = &uui_label_ops, .widget = &g_spacer,
                                           .flags = UUI_FILL_W };
    g_button_items[2] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_ok,
                                           .id = OPT_OK, .name = "ok" };
    g_button_items[3] = (struct uui_item){ .ops = &uui_button_ops, .widget = &g_cancel,
                                           .id = OPT_CANCEL, .name = "cancel" };
    g_button_l = (struct uui_layout){ .dir = UUI_ROW, .items = g_button_items, .count = 4,
                                      .margin = 10 };
    g_root_items[0] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_body_l,
                                         .flags = UUI_FILL_W | UUI_FILL_H };
    g_root_items[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &g_button_l,
                                         .flags = UUI_FILL_W };
    g_root_l = (struct uui_layout){ .dir = UUI_COLUMN, .items = g_root_items, .count = 2,
                                    .margin = 1, .gap = 1 };
    uui_focus_init(&g_focus, g_focusables, 3 + ROWS);
    uui_focus_set(&g_focus, 0);

    struct uapp_window_desc d = {
        .title = "File Manager Options",
        .w = ugfx_char_advance('n') * 88,
        .h = ugfx_char_h() * 22,
        .flags = UAPP_WIN_MODAL,
        .widgets = g_root_items,
        .widget_count = 2,
        .layout = &g_root_l,
        .focus = &g_focus,
        .on_widget = on_widget,
        .on_action = on_action,
        .on_key = on_key,
        .on_close = on_close,
        .log_prefix = "options",
    };
    g_win = uapp_window_open(a, &d);
}
