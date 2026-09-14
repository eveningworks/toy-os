// See ui/uui_filedialog.h for the design writeup (what this is, why it
// is a window, and why Places rather than a tree).
#include "ui/uui_filedialog.h"

#include <string.h>
#include <stdio.h>
#include "rt/sys.h"
#include "kpath.h"
#include "ui/ugfx.h"
#include "ui/utheme.h"
#include "ui/uui_widget.h"
#include "ui/uui_route.h"   // UUI_REASON_*

// Widget ids, which are also the toolbar/button codes. One space: the
// window has one on_widget and one switch.
enum {
    ID_BAR = 1, ID_PLACES, ID_VIEW, ID_NAME, ID_TYPE, ID_OK, ID_CANCEL,
    CMD_UP = 10, CMD_REFRESH, CMD_DETAILS, CMD_ICONS,
};

// Where each widget sits in the focus ring. NAMED, because a focus ring
// is addressed by index and a bare 2 in a press handler is the "named by
// its position in the array" bug this tree keeps finding.
enum { FOCUS_BAR, FOCUS_PLACES, FOCUS_VIEW, FOCUS_NAME, FOCUS_TYPE, FOCUS_COUNT };

static const struct uui_toolbar_item g_tools[] = {
    { "tb-up",      "Up",      CMD_UP },
    { "tb-refresh", "Refresh", CMD_REFRESH },
    UUI_TOOLBAR_SEP,
    { "tb-details", "Details", CMD_DETAILS },
    { "tb-icons",   "Icons",   CMD_ICONS },
};

// THE ONE INSTANCE THE TOOLBAR'S FLAGS CALLBACK CAN SEE. `item_flags`
// takes a code and nothing else (ui/uui_toolbar.h), so the latched view
// button has to be reachable from a file-scope pointer. One chooser at a
// time per process is the rule anyway -- it is modal.
static struct uui_filedialog *g_open;

static unsigned tool_flags(int code) {
    if (!g_open) return 0;
    if (code == CMD_DETAILS) return g_open->view.mode == UUI_FILEVIEW_DETAILS ? UUI_MI_CHECKED : 0;
    if (code == CMD_ICONS)   return g_open->view.mode == UUI_FILEVIEW_ICONS   ? UUI_MI_CHECKED : 0;
    return 0;
}

// --- places ------------------------------------------------------------
//
// Only what is REALLY THERE. A destination that does not resolve is left
// out rather than listed and refused: a chooser whose sidebar offers a
// folder that cannot be opened teaches the user to distrust the rest.
static void add_place(struct uui_filedialog *fd, const char *label, const char *path) {
    if (fd->place_count >= UUI_FILEDIALOG_PLACES) return;
    struct sys_stat st;
    if (sys_stat(path, &st) != 0 || !st.is_dir) return;
    int i = fd->place_count++;
    fd->place_paths[i] = path;
    fd->place_rows[i].label = label;
    fd->place_rows[i].kind = UUI_SIDEBAR_TOP;
    fd->place_rows[i].icon = 0;
    fd->place_rows[i].id = i;
}

static void build_places(struct uui_filedialog *fd) {
    fd->place_count = 0;
    // Windows' Quick access and KDE's Places, in the order a person
    // reaches for them: where you are likely to keep things first, the
    // filesystem root last.
    add_place(fd, "Home",      "/home");
    add_place(fd, "Desktop",   "/home/desktop");
    add_place(fd, "Documents", "/usr/share/doc");
    add_place(fd, "Pictures",  "/usr/share/wallpapers");
    add_place(fd, "Music",     "/usr/share/music");
    add_place(fd, "Scratch",   "/var/tmp");
    add_place(fd, "Root",      "/");
}

// The sidebar's selection follows the LISTING, so browsing away from a
// place clears its highlight rather than leaving it claiming to be
// where you are.
static void sync_places(struct uui_filedialog *fd) {
    const char *dir = uui_fileview_dir(&fd->view);
    for (int i = 0; i < fd->place_count; i++)
        if (strcmp(fd->place_paths[i], dir) == 0) {
            uui_sidebar_select_id(&fd->places, i);
            return;
        }
    fd->places.selected = -1;
}

// **A DIRECTORY ALWAYS PASSES, WHATEVER THE ROW SAYS.** The contract is
// in uui_filedialog.h and it has to be ENFORCED here rather than asked
// of every caller: each of the three apps' filters answers "is this a
// file I can open", and every one of them says no to a directory --
// which in a SIDEBAR pinned to one folder is right, and in a chooser
// leaves a root listing with nothing in it and no way to go anywhere.
// Found by looking at the screen; 22 green checks had not noticed,
// because each of them reached its directory by a Places row or a typed
// path rather than by walking one.
static const struct uui_filedialog_filter *chosen_filter(const struct uui_filedialog *fd);

static int filter_gate(void *ctx, const char *dir, const struct sys_dirent *e) {
    struct uui_filedialog *fd = (struct uui_filedialog *)ctx;
    if (e->is_dir) return 1;
    const struct uui_filedialog_filter *f = chosen_filter(fd);
    return (f && f->fn) ? f->fn(f->ctx, dir, e) : 1;
}

// The row the combo has selected, or NULL when there are none.
static const struct uui_filedialog_filter *chosen_filter(const struct uui_filedialog *fd) {
    int i = fd->filter_count ? uui_dropdown_selected(&fd->type) : -1;
    if (i < 0 || i >= fd->filter_count) return 0;
    return &fd->filters[i];
}

// The combo changed: the view's filter is always the gate, so only the
// listing has to be redone.
static void apply_filter(struct uui_filedialog *fd) {
    uui_fileview_reload(&fd->view);
}

// --- committing --------------------------------------------------------

static void finish(struct uui_filedialog *fd, const char *path) {
    void (*done)(void *, const char *) = fd->on_done;
    void *ctx = fd->ctx;
    char copy[UUI_FILEDIALOG_PATH_MAX];
    copy[0] = '\0';
    if (path) strlcpy(copy, path, sizeof copy);
    // CLOSED FIRST, so the callback may open another chooser -- and so
    // the owner is unblocked before it is asked to do anything.
    uui_filedialog_close_window(fd);
    if (done) done(ctx, path ? copy : 0);
}

// What the OK button means, which is different in all three modes.
// Returns 1 if the dialog is finished; 0 keeps it up (a folder was
// entered instead, or the answer is not usable yet).
static int commit(struct uui_filedialog *fd) {
    const char *dir = uui_fileview_dir(&fd->view);

    if (fd->mode == UUI_FILEDIALOG_FOLDER) {
        // A directory ROW is the answer if one is selected; otherwise
        // the directory being shown is, which is GTK's SELECT_FOLDER
        // behaviour and what makes "choose this folder" reachable
        // without selecting anything.
        char sel[UUI_FILEDIALOG_PATH_MAX];
        if (uui_fileview_selected_is_dir(&fd->view) &&
            uui_fileview_selected_path(&fd->view, sel, sizeof sel)) {
            finish(fd, sel);
            return 1;
        }
        finish(fd, dir);
        return 1;
    }

    const char *typed = uui_textbox_text(&fd->name);
    char full[UUI_FILEDIALOG_PATH_MAX];

    if (typed && typed[0]) {
        // AN ABSOLUTE PATH TYPED INTO THE FIELD IS TAKEN AS ONE, which
        // every real chooser allows and is the only way to reach a
        // directory the Places list does not name.
        if (typed[0] == '/') strlcpy(full, typed, sizeof full);
        else if (!k_path_join(dir, typed, full, sizeof full)) return 0;
    } else if (!uui_fileview_selected_path(&fd->view, full, sizeof full)) {
        return 0;   // nothing named and nothing selected
    }

    struct sys_stat st;
    int exists = sys_stat(full, &st) == 0;
    if (exists && st.is_dir) {
        // OK ON A FOLDER ENTERS IT and keeps the dialog up, as in
        // Explorer and every GTK chooser.
        uui_fileview_set_dir(&fd->view, full);
        uui_textbox_init(&fd->name, "");
        sync_places(fd);
        return 0;
    }
    if (fd->mode == UUI_FILEDIALOG_OPEN && !exists) return 0;
    finish(fd, full);
    return 1;
}

// --- the window's callbacks --------------------------------------------

static void on_widget(struct uapp_window *w, int id, int reason) {
    struct uui_filedialog *fd = (struct uui_filedialog *)uapp_window_state(w);
    if (!fd) return;

    switch (id) {
    case ID_BAR: {
        int code = uui_toolbar_take_code(&fd->bar);
        if (code == CMD_UP) { uui_fileview_up(&fd->view); sync_places(fd); }
        else if (code == CMD_REFRESH) uui_fileview_reload(&fd->view);
        else if (code == CMD_DETAILS) uui_fileview_set_mode(&fd->view, UUI_FILEVIEW_DETAILS);
        else if (code == CMD_ICONS)   uui_fileview_set_mode(&fd->view, UUI_FILEVIEW_ICONS);
        break;
    }
    case ID_PLACES: {
        int i = uui_sidebar_selected_id(&fd->places);
        if (i >= 0 && i < fd->place_count) {
            uui_fileview_set_dir(&fd->view, fd->place_paths[i]);
            uui_textbox_init(&fd->name, "");
        }
        break;
    }
    case ID_VIEW:
        // A PRESS MOVES THE FOCUS TO WHAT WAS PRESSED. The router names
        // the child, not the container, so nothing else does this --
        // and typing after clicking the list would otherwise go to the
        // name field (ui/uui_dialog.h's note, reached from the other
        // side).
        if (reason == UUI_REASON_PRESS) {
            uui_focus_set(&fd->focus, FOCUS_VIEW);
            uui_textbox_set_active(&fd->name, 0);
        }
        break;
    case ID_NAME:
        if (reason == UUI_REASON_PRESS) uui_focus_set(&fd->focus, FOCUS_NAME);
        break;
    case ID_TYPE:
        // THE LISTING FOLLOWS THE COMBO. Re-listed on every report the
        // widget makes rather than only on a release: a dropdown commits
        // from the keyboard too, and a chooser showing the wrong set
        // until the next click is worse than one that re-lists twice.
        if (reason == UUI_REASON_PRESS) uui_focus_set(&fd->focus, FOCUS_TYPE);
        apply_filter(fd);
        break;
    case ID_OK:
        if (reason == UUI_REASON_RELEASE) { if (commit(fd)) return; }
        break;
    case ID_CANCEL:
        if (reason == UUI_REASON_RELEASE) { finish(fd, 0); return; }
        break;
    default:
        break;
    }
    uapp_window_redraw(w);
}

static void on_key(struct uapp_window *w, int key, unsigned mods) {
    (void)mods;
    struct uui_filedialog *fd = (struct uui_filedialog *)uapp_window_state(w);
    if (!fd) return;
    // ESCAPE CANCELS AND RETURN COMMITS, in a dialog, on every system
    // there is. Both are read here rather than by a widget because
    // neither belongs to one: they are the dialog's own two answers.
    if (key == 0x1B) { finish(fd, 0); return; }
    if (key == '\n' || key == '\r') { if (commit(fd)) return; }
    uapp_window_redraw(w);
}

// WHAT THE WIDGET WALK CANNOT SAY: which directory is listed and which
// type row is selected. Reported HERE rather than from uui_fileview's
// describe op, where it would cost every fileview in the tree two log
// lines a frame -- the File Manager has two panes, and its own test
// reads the same serial console the log goes out on.
static void log_layout(struct uapp_window *w) {
    struct uui_filedialog *fd = (struct uui_filedialog *)uapp_window_state(w);
    if (!fd) return;
    uapp_logf_layout("filedialog: layout view.dir %s\n", uui_fileview_dir(&fd->view));
    uapp_logf_layout("filedialog: layout view.rows %d\n", uui_fileview_row_count(&fd->view));
    if (fd->filter_count)
        uapp_logf_layout("filedialog: layout type.selected %d\n",
                         uui_dropdown_selected(&fd->type));
}

static void on_close(struct uapp_window *w) {
    struct uui_filedialog *fd = (struct uui_filedialog *)uapp_window_state(w);
    if (fd) finish(fd, 0);
    else uapp_window_close(w);
}

// --- the fileview's callbacks ------------------------------------------

static void view_select(void *ctx, const char *path, int is_dir) {
    struct uui_filedialog *fd = (struct uui_filedialog *)ctx;
    if (is_dir || fd->mode == UUI_FILEDIALOG_FOLDER) return;
    // THE SELECTED NAME LANDS IN THE FIELD, which is what makes saving
    // over an existing file one click. The BASENAME, because the field
    // is relative to the listing.
    if (path) uui_textbox_init(&fd->name, k_path_basename(path));
}

static void view_open(void *ctx, const char *path) {
    struct uui_filedialog *fd = (struct uui_filedialog *)ctx;
    // A double click (or Enter) on a FILE is the commit. A directory
    // never reaches here -- the widget descends into those itself.
    if (fd->mode == UUI_FILEDIALOG_FOLDER) return;
    finish(fd, path);
}

static void view_dir_changed(void *ctx, const char *dir) {
    struct uui_filedialog *fd = (struct uui_filedialog *)ctx;
    (void)dir;
    sync_places(fd);
}

// --- opening -----------------------------------------------------------

void uui_filedialog_close_window(struct uui_filedialog *fd) {
    if (!fd || !fd->win) return;
    struct uapp_window *w = fd->win;
    fd->win = 0;
    if (g_open == fd) g_open = 0;
    uapp_window_close(w);
}

int uui_filedialog_is_open(const struct uui_filedialog *fd) {
    return fd && fd->win && uapp_window_is_open(fd->win);
}

struct uapp_window *uui_filedialog_open(struct uapp *a, struct uui_filedialog *fd,
                                         const struct uui_filedialog_opts *opts,
                                         void (*on_done)(void *ctx, const char *path),
                                         void *ctx) {
    if (!a || !fd || !opts) return 0;
    if (fd->win) return 0;   // modal: one at a time

    fd->on_done = on_done;
    fd->ctx = ctx;
    fd->mode = opts->mode;

    const char *deflt = opts->mode == UUI_FILEDIALOG_SAVE   ? "Save File"
                      : opts->mode == UUI_FILEDIALOG_FOLDER ? "Choose Folder"
                                                            : "Open File";
    strlcpy(fd->title, opts->title && opts->title[0] ? opts->title : deflt, sizeof fd->title);

    // --- the widgets ---------------------------------------------------
    uui_toolbar_init(&fd->bar, g_tools, (int)(sizeof g_tools / sizeof g_tools[0]));
    fd->bar.item_flags = tool_flags;

    build_places(fd);
    uui_sidebar_init(&fd->places, 0, 0, 10, 10, fd->place_rows, fd->place_count);
    fd->places.selected = -1;

    uui_fileview_init(&fd->view, 0, 0, 10, 10, fd->entries, UUI_FILEDIALOG_ENTRIES);
    uui_fileview_set_mode(&fd->view, UUI_FILEVIEW_DETAILS);
    fd->filters = opts->filters;
    fd->filter_count = opts->filter_count > UUI_FILEDIALOG_FILTERS
                           ? UUI_FILEDIALOG_FILTERS : opts->filter_count;
    if (fd->filter_count < 0) fd->filter_count = 0;
    for (int i = 0; i < fd->filter_count; i++)
        fd->type_labels[i] = fd->filters[i].label;
    uui_dropdown_init(&fd->type, 0, 0, 10, 10, fd->type_labels, fd->filter_count);
    if (opts->filter_index > 0 && opts->filter_index < fd->filter_count)
        fd->type.list.selected = opts->filter_index;
    uui_fileview_set_filter(&fd->view, filter_gate, fd);
    fd->view.on_select = view_select;
    fd->view.on_open = view_open;
    fd->view.on_dir_changed = view_dir_changed;
    fd->view.ctx = fd;
    const char *start = (opts->start_dir && opts->start_dir[0]) ? opts->start_dir : "/";
    if (!uui_fileview_set_dir(&fd->view, start)) uui_fileview_set_dir(&fd->view, "/");

    uui_label_init(&fd->name_label, opts->mode == UUI_FILEDIALOG_FOLDER ? "Folder:" : "File name:");
    uui_label_init(&fd->type_label, "Files of type:");
    uui_textbox_init(&fd->name, opts->initial_name ? opts->initial_name : "");
    uui_label_init(&fd->spacer, "");

    const char *ok_label = opts->mode == UUI_FILEDIALOG_SAVE   ? "Save"
                         : opts->mode == UUI_FILEDIALOG_FOLDER ? "Choose"
                                                               : "Open";
    uui_button_init(&fd->ok, 0, 0, 0, 0, ok_label, UTHEME_ACCENT, UTHEME_ACCENT_TEXT, ID_OK);
    uui_button_init(&fd->cancel, 0, 0, 0, 0, "Cancel", UTHEME_BUTTON_BG, UTHEME_TEXT, ID_CANCEL);

    // --- the layout ----------------------------------------------------
    //
    // A COLUMN of four rows. The body is the only thing that grows, so
    // it carries both FILL flags and everything else keeps its natural
    // height -- which is what stops the button row being pushed off the
    // bottom of a short window (docs/conventions/gui.md).
    fd->body_items[0] = (struct uui_item){ .ops = &uui_sidebar_ops, .widget = &fd->places,
                                           .flags = UUI_FILL_H, .id = ID_PLACES,
                                           .main_size = ugfx_char_w() * 13, .name = "places" };
    fd->body_items[1] = (struct uui_item){ .ops = &uui_fileview_ops, .widget = &fd->view,
                                           .flags = UUI_FILL_W | UUI_FILL_H, .id = ID_VIEW,
                                           .name = "view" };
    // **MARGIN 1, NOT 0.** uui_layout_margin() reads anything <= 0 as
    // "the font-derived default" (ui/uui_layout.c), so a nested row
    // asking for no padding gets a character cell of it on every side --
    // three of those stacked took a third of the window's height.
    fd->body = (struct uui_layout){ .dir = UUI_ROW, .items = fd->body_items, .count = 2,
                                    .margin = 1 };

    // **UUI_FILL_H ON THE LABELS, AND IT IS WHAT CENTRES THEM.** A
    // label's natural height is one text row, so without it the box is
    // short and sits at the TOP of a row whose field is half as tall
    // again -- the caption then reads as belonging to whatever is
    // above. uui_label already centres its text inside a taller box;
    // the flag is what gives it one. Both labels are pinned to the same
    // width so the two fields line up, which is what a form layout does
    // (Win32 dialog units, QFormLayout, GtkGrid).
    int label_w = ugfx_text_width("Files of type:") + ugfx_char_w();
    fd->name_items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &fd->name_label,
                                           .flags = UUI_FILL_H,
                                           .main_size = label_w, .name = "namelabel" };
    fd->name_items[1] = (struct uui_item){ .ops = &uui_textbox_ops, .widget = &fd->name,
                                           .flags = UUI_FILL_W, .id = ID_NAME, .name = "name" };
    fd->name_row = (struct uui_layout){ .dir = UUI_ROW, .items = fd->name_items, .count = 2,
                                        .margin = 1 };

    fd->type_items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &fd->type_label,
                                           .flags = UUI_FILL_H,
                                           .main_size = label_w, .name = "typelabel" };
    fd->type_items[1] = (struct uui_item){ .ops = &uui_dropdown_ops, .widget = &fd->type,
                                           .flags = UUI_FILL_W, .id = ID_TYPE, .name = "type" };
    fd->type_row = (struct uui_layout){ .dir = UUI_ROW, .items = fd->type_items, .count = 2,
                                        .margin = 1 };

    // An EMPTY LABEL that takes the leftover width, which is what pushes
    // the buttons to the right edge -- UUI_FILL_W in a row absorbs the
    // slack (ui/uui_layout.h), and a row with nothing stretchable would
    // leave them hard against the left.
    fd->button_items[0] = (struct uui_item){ .ops = &uui_label_ops, .widget = &fd->spacer,
                                             .flags = UUI_FILL_W };
    fd->button_items[1] = (struct uui_item){ .ops = &uui_button_ops, .widget = &fd->ok,
                                             .id = ID_OK, .name = "ok" };
    fd->button_items[2] = (struct uui_item){ .ops = &uui_button_ops, .widget = &fd->cancel,
                                             .id = ID_CANCEL, .name = "cancel" };
    fd->button_row = (struct uui_layout){ .dir = UUI_ROW, .items = fd->button_items, .count = 3,
                                          .margin = 1 };

    fd->root_items[0] = (struct uui_item){ .ops = &uui_toolbar_ops, .widget = &fd->bar,
                                           .flags = UUI_FILL_W, .id = ID_BAR, .name = "bar" };
    fd->root_items[1] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &fd->body,
                                           .flags = UUI_FILL_W | UUI_FILL_H, .name = "body" };
    fd->root_items[2] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &fd->name_row,
                                           .flags = UUI_FILL_W, .name = "namerow" };
    // THE TYPE ROW IS HIDDEN WHEN THERE ARE NO FILTERS rather than
    // absent: a combo with nothing in it is a control that reads as
    // broken, and an app that opens anything has no types to offer.
    fd->root_items[3] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &fd->type_row,
                                           .flags = UUI_FILL_W, .name = "typerow",
                                           .hidden = fd->filter_count == 0 };
    fd->root_items[4] = (struct uui_item){ .ops = &uui_layout_ops, .widget = &fd->button_row,
                                           .flags = UUI_FILL_W, .name = "buttons" };
    // THE FOLDER MODE HAS NO NAME FIELD to speak of -- the answer is the
    // listing's own directory -- but the row stays, showing which folder
    // OK would choose. Hiding it would leave the dialog with no text at
    // all saying what it is about to return.
    fd->root = (struct uui_layout){ .dir = UUI_COLUMN, .items = fd->root_items, .count = 5 };

    // Tab order: the chrome first, then the list, then the field. The
    // list is what opens focused -- it is what the user came to use.
    fd->focusables[FOCUS_BAR]    = (struct uui_focusable){ &fd->bar, &uui_toolbar_ops };
    fd->focusables[FOCUS_PLACES] = (struct uui_focusable){ &fd->places, &uui_sidebar_ops };
    fd->focusables[FOCUS_VIEW]   = (struct uui_focusable){ &fd->view, &uui_fileview_ops };
    fd->focusables[FOCUS_NAME]   = (struct uui_focusable){ &fd->name, &uui_textbox_focus_ops };
    fd->focusables[FOCUS_TYPE]   = (struct uui_focusable){ &fd->type, &uui_dropdown_ops };
    uui_focus_init(&fd->focus, fd->focusables, FOCUS_COUNT);
    uui_focus_set(&fd->focus,
                  opts->mode == UUI_FILEDIALOG_SAVE ? FOCUS_NAME : FOCUS_VIEW);

    // --- the window ----------------------------------------------------
    //
    // FONT-DERIVED, like every size here (docs/gui-guidelines.md). Wide
    // enough for the Places strip plus a details view with room for the
    // Modified column; tall enough for about twenty rows.
    int w = ugfx_char_w() * 60;
    int h = ugfx_char_h() * 30;

    struct uapp_window_desc d = {
        .title = fd->title,
        .w = w, .h = h,
        .flags = UAPP_WIN_MODAL,
        .widgets = fd->root_items,
        .widget_count = 5,
        .layout = &fd->root,
        .focus = &fd->focus,
        .state = fd,
        .on_widget = on_widget,
        .on_key = on_key,
        .on_close = on_close,
        // ONE PREFIX FOR EVERY APP'S CHOOSER, because it is one widget:
        // a test that can drive Notepad's Open can drive the Image
        // Viewer's without learning a second vocabulary.
        .log_prefix = "filedialog",
        .on_log_layout = log_layout,
    };
    fd->win = uapp_window_open(a, &d);
    if (!fd->win) return 0;
    g_open = fd;
    sync_places(fd);
    return fd->win;
}
