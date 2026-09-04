// Notepad, as a RING-3 PROCESS.
//
// The kernel-space original is apps/notepad.c. This is the same editor
// -- wrapped scrollable text, a cursor, click-to-position and
// drag-select, open and save -- running as an ordinary ring-3 program
// over the windowing protocol.
//
// WHAT THE MIGRATION MADE SIMPLER, WHICH IS THE INTERESTING PART
// ---------------------------------------------------------------
// The kernel version cannot block. A GUI app there runs inside
// wm_run()'s loop, so a blocking disk read would freeze the whole
// desktop -- which is why apps/notepad.c carries `window_start_read()`/
// `window_start_write()`, why the WM has pending_read/pending_write
// slots polled once per frame, why loading a file is a state machine
// with an on_read_complete callback, and why close_window() has to
// refuse while I/O is in flight.
//
// None of that is needed here. This is a separate process: it calls
// sys_read() and blocks, and the desktop keeps running because the
// kernel context is a scheduler participant now (the GUI-in-ring-3
// milestone's first stage). So the whole stepped-I/O apparatus
// collapses into an ordinary read-the-file-into-a-buffer loop. That is
// a real architectural dividend of moving apps out of the kernel, not a
// shortcut.
//
// THE FILE DIALOG IS THE TOOLKIT'S, DRAWN BY THIS PROCESS. A display
// server has no business owning file dialogs -- GTK and Qt each draw
// their own -- so it is a `uui_dialog` with a body of `uui_fileview`
// (the listing, navigation, sorting) and `uui_textbox` (the name), in
// this window. It used to be ~230 lines of hand-drawn rows and a caret
// here, the third copy of a directory listing in the tree.
//
// THE CHROME IS ROUTED. The menu bar, the status bar and the dialog are
// declared in `desc.widgets`, so the router owns their input and the
// layout log; what this file still draws and hit-tests by hand is the
// document and its scrollbar, which have no widget yet.
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "rt/sys.h"
#include "kpath.h"   // k_path_join/_dirname/_basename -- the kernel's, linked into ring 3
#include "ui/ugfx.h"
#include "ui/uui.h"
#include "ui/uui_fileview.h"
#include "ui/uapp.h"
#include "ui/ulog.h"
#include "ui/utext.h"
#include "ui/utheme.h"
#include "keyboard.h" // KEY_* codes, the same ones the WM delivers

#define WIN_W 560
#define WIN_H 380

// A small INTERNAL inset so glyphs and the frame don't touch -- not an
// outer moat. The edit surface fills the content area (see
// text_rect_for): flush under the menu bar, flush to the status bar,
// with the scrollbar flush to the right edge. That is how a real
// editor's edit control owns the whole client rect (Windows Notepad,
// gedit/GtkTextView); an 8px margin on every side left the field
// visibly floating inside its own window.
#define TEXT_PAD 3

#define PATH_MAX_LEN 64 // FS_PATH_MAX
#define DIALOG_MAX_FILES 64

// --- widget ids -------------------------------------------------------

#define ID_MENU     1
#define ID_STATUS   2
#define ID_DIALOG   3
#define ID_DLG_LIST 4
#define ID_DLG_NAME 5

// --- menu command codes -----------------------------------------------
//
// One numbering for the menu and for the keyboard accelerators, so
// Ctrl-S and File > Save cannot drift apart: both call do_command().
#define CMD_NEW        1
#define CMD_OPEN       2
#define CMD_SAVE       3
#define CMD_SAVE_AS    4
#define CMD_EXIT       5
#define CMD_RECENT     6  // the submenu itself -- queried, never committed
#define CMD_RECENT_0   7
#define CMD_RECENT_1   8
#define CMD_RECENT_2   9
#define CMD_SELECT_ALL 10
#define CMD_DELETE     11
#define CMD_GOTO       12 // submenu
#define CMD_GOTO_TOP   13
#define CMD_GOTO_END   14
#define CMD_STATUSBAR  15

// The dialog's own codes -- a different widget, so a different space.
#define CMD_DLG_OK     1
#define CMD_DLG_CANCEL 2

#define RECENT_MAX 3

static struct uapp *g_app;   // for the fileview's callbacks, which carry no uapp
static struct utext g_text;

static char g_path[PATH_MAX_LEN];   // "" until saved/opened
static char g_status[96];
static int g_dirty;
static int g_show_status = 1;

// The Recent list. The menu items below point straight at these buffers,
// so "update the menu" is "write the string" -- there is no menu state
// to keep in sync. See ui/uui_menubar.h on why the tree is const and the
// per-item state is asked for instead.
static char g_recent[RECENT_MAX][PATH_MAX_LEN];
static int g_recent_count;

static struct uui_menubar g_menu;
static struct uui_statusbar g_statusbar;
static char g_lncol[20];
static char g_modflag[8];

// --- the menu tree ----------------------------------------------------

static const struct uui_menu_item recent_items[] = {
    UUI_MENU(g_recent[0], CMD_RECENT_0, 0),
    UUI_MENU(g_recent[1], CMD_RECENT_1, 0),
    UUI_MENU(g_recent[2], CMD_RECENT_2, 0),
};

static const struct uui_menu_item file_items[] = {
    UUI_MENU("New",          CMD_NEW,     "Ctrl-N"),
    UUI_MENU("Open...",      CMD_OPEN,    "Ctrl-O"),
    UUI_SUBMENU_CODE("Recent files", recent_items, CMD_RECENT),
    UUI_MENU_SEP,
    UUI_MENU("Save",         CMD_SAVE,    "Ctrl-S"),
    UUI_MENU("Save As...",   CMD_SAVE_AS, 0),
    UUI_MENU_SEP,
    UUI_MENU("Exit",         CMD_EXIT,    "Alt+F4"),
};

static const struct uui_menu_item edit_items[] = {
    UUI_MENU("Select All",       CMD_SELECT_ALL, "Ctrl-A"),
    UUI_MENU("Delete Selection", CMD_DELETE,     "Del"),
};

static const struct uui_menu_item goto_items[] = {
    UUI_MENU("Top of file",    CMD_GOTO_TOP, 0),
    UUI_MENU("Bottom of file", CMD_GOTO_END, 0),
};

static const struct uui_menu_item view_items[] = {
    UUI_SUBMENU("Go to", goto_items),
    UUI_MENU_SEP,
    UUI_MENU("Status bar", CMD_STATUSBAR, 0),
};

static const struct uui_menu_item menu_bar[] = {
    UUI_SUBMENU("File", file_items),
    UUI_SUBMENU("Edit", edit_items),
    UUI_SUBMENU("View", view_items),
};

// --- the file dialog --------------------------------------------------
//
// One dialog, two flavours: Open lists and takes a selection, Save As
// lists and takes a typed name. Both have the field -- typing a path
// into an Open dialog is what Win32's chooser allows too.
static struct uui_dialog g_dialog;
static int g_dlg_saving;

// The listing's storage: uui_fileview does not own it (ui/uui_fileview.h),
// and 64 entries is 5 KB, which is why this is file-scope and not a
// local (the ring-3 frame budget is 2 KiB).
static struct sys_dirent g_dlg_entries[DIALOG_MAX_FILES];
static struct uui_fileview g_dlg_list;
static struct uui_textbox g_dlg_name;

static struct uui_item g_dlg_items[] = {
    { .ops = &uui_fileview_ops, .widget = &g_dlg_list, .flags = UUI_FILL_W | UUI_FILL_H,
      .id = ID_DLG_LIST, .name = "flist" },
    { .ops = &uui_textbox_ops,  .widget = &g_dlg_name, .flags = UUI_FILL_W,
      .id = ID_DLG_NAME, .name = "fname" },
};
static struct uui_layout g_dlg_layout;
static struct uui_item g_dlg_body = { .ops = &uui_layout_ops, .widget = &g_dlg_layout };

// The one text row: which directory the listing shows. Rewritten by
// the fileview's own navigation callback, so it cannot lag the list.
static char g_dlg_row[PATH_MAX_LEN + 16];
static const char *const g_dlg_rows[1] = { g_dlg_row };

// --- the routed widgets ---------------------------------------------
//
// `.name` is what the layout log reports each one as (ui/uui_describe.h).
// The dialog is LAST: with a body it is drawn in the items pass, and
// last is what keeps it over the bars (ui/uui_dialog.h).
static struct uui_item g_widgets[] = {
    { .ops = &uui_menubar_ops,   .widget = &g_menu,      .id = ID_MENU,   .name = "menu" },
    { .ops = &uui_statusbar_ops, .widget = &g_statusbar, .id = ID_STATUS, .name = "status" },
    { .ops = &uui_dialog_ops,    .widget = &g_dialog,    .id = ID_DIALOG, .name = "dialog" },
};

// By ID, never by index (docs/conventions/gui.md).
static struct uui_item *item_by_id(struct uui_item *items, int n, int id) {
    for (int i = 0; i < n; i++) if (items[i].id == id) return &items[i];
    return NULL;
}
#define WIDGET(id)   item_by_id(g_widgets, (int)(sizeof g_widgets / sizeof g_widgets[0]), (id))
#define DLG_ITEM(id) item_by_id(g_dlg_items, (int)(sizeof g_dlg_items / sizeof g_dlg_items[0]), (id))

static void set_status(const char *s) { strlcpy(g_status, s, sizeof g_status); }

// The title carries the filename and a dirty marker, so it changes as
// the document does. Sent only when it ACTUALLY changed: this used to
// be called before every repaint, which meant a TWP message per
// keystroke to say the same thing.
static char g_shown_title[WIN_TITLE_LEN];

static void set_title(struct uapp *a) {
    char t[WIN_TITLE_LEN];
    snprintf(t, sizeof t, "%s%s", g_dirty ? "*" : "", g_path[0] ? g_path : "untitled");
    if (strcmp(t, g_shown_title) == 0) return;
    strlcpy(g_shown_title, t, sizeof g_shown_title);
    uapp_set_title(a, t);
}

// Most-recent-first, deduplicated, capped. Called on every successful
// open and save, which is the only place a path becomes "a file this
// session has actually touched".
static void recent_push(const char *path) {
    int at = RECENT_MAX - 1;
    for (int i = 0; i < g_recent_count; i++)
        if (strcmp(g_recent[i], path) == 0) { at = i; break; }

    for (int i = at; i > 0; i--) strlcpy(g_recent[i], g_recent[i - 1], PATH_MAX_LEN);
    strlcpy(g_recent[0], path, PATH_MAX_LEN);
    if (g_recent_count < RECENT_MAX) g_recent_count++;
}

// --- file I/O ---------------------------------------------------------
//
// Plain blocking calls. See the file header for why that is allowed
// here and is not in the kernel-space version.

static int load_file(struct uapp *a, const char *path) {
    int fd = sys_open(path, 0);
    if (fd < 0) { set_status("open failed"); return 0; }

    // A whole file, a chunk at a time, with the event loop stopped:
    // say so before going quiet (ui/uapp.h).
    uapp_busy_begin(a);

    utext_clear(&g_text);
    char chunk[512];
    for (;;) {
        int64_t n = sys_read(fd, chunk, sizeof chunk);
        if (n <= 0) break;
        for (int64_t i = 0; i < n; i++) utext_putc(&g_text, chunk[i]);
    }
    sys_close(fd);
    uapp_busy_end(a);

    g_text.ed.cursor = 0;
    g_text.scroll_offset = 0;
    utext_sel_clear(&g_text);
    strlcpy(g_path, path, PATH_MAX_LEN);
    g_dirty = 0;
    recent_push(path);
    set_status("opened");
    return 1;
}

static int save_file(struct uapp *a, const char *path) {
    int fd = sys_open(path, SYS_O_WRITE | SYS_O_CREAT | SYS_O_TRUNC);
    if (fd < 0) { set_status("save failed"); return 0; }
    uapp_busy_begin(a);

    // Write in chunks rather than a character at a time: one syscall
    // per character would be thousands of kernel entries for a modest
    // file, and SYS_WRITE is capped per call anyway (SYS_WRITE_MAX).
    char chunk[512];
    int n = 0;
    for (int i = 0; i < g_text.count; i++) {
        chunk[n++] = utext_at(&g_text, i);
        if (n == (int)sizeof chunk) {
            if (sys_write(fd, chunk, (size_t)n) < 0) { sys_close(fd); uapp_busy_end(a); set_status("write failed"); return 0; }
            n = 0;
        }
    }
    if (n > 0 && sys_write(fd, chunk, (size_t)n) < 0) {
        sys_close(fd);
        uapp_busy_end(a);
        set_status("write failed");
        return 0;
    }
    sys_close(fd);
    uapp_busy_end(a);

    strlcpy(g_path, path, PATH_MAX_LEN);
    g_dirty = 0;
    recent_push(path);
    set_status("saved");
    return 1;
}

// --- layout -----------------------------------------------------------

static int menubar_h(void) { return uui_menubar_height(&g_menu); }
static int statusbar_h(void) { return g_show_status ? uui_statusbar_height(&g_statusbar) : 0; }

// The scrollbar's width comes from the WIDGET, not from a number here.
// It used to be `#define SCROLLBAR_W 8` with a matching hardcoded "10px
// gutter" in the text rect below -- two constants that had to agree,
// both narrower than the toolkit's own bars, and the reason this one was
// hard to grab with a mouse. See ui/uui_scrollbar.h.
static int scrollbar_w(void) {
    int w;
    uui_scrollbar_natural_size(&w, 0);
    return w;
}

// Derived from the content SIZE rather than from a surface, because the
// event callbacks need it too and they never hold one. That it derives
// at all is what makes this app resizable with no resize code: a bigger
// window is simply a bigger page.
static void text_rect_for(int cw, int ch, int *x, int *y, int *w, int *h) {
    *x = TEXT_PAD;
    *y = menubar_h();
    *w = cw - TEXT_PAD - scrollbar_w(); // scrollbar sits flush at the right edge
    *h = ch - *y - statusbar_h();
}

// Placed against the CONTENT rect, which is also the popup bounds handed
// to the menu bar and the box the dialog centres in. Run every draw:
// no `.layout` places these, and the app is resizable.
static void layout_chrome(int cw, int ch) {
    uui_menubar_set_geometry(&g_menu, 0, 0, cw, menubar_h());
    uui_menubar_set_bounds(&g_menu, 0, 0, cw, ch);
    uui_statusbar_set_geometry(&g_statusbar, 0, ch - statusbar_h(), cw, statusbar_h());
    WIDGET(ID_STATUS)->hidden = !g_show_status;
    uui_dialog_set_bounds(&g_dialog, 0, 0, cw, ch);
}

// LOGICAL lines, not wrapped rows: "Ln 12" in every editor's status bar
// counts newlines, and a reader comparing it against a shell `sed -n
// 12p` expects the same answer.
static void update_indicators(void) {
    int line = 1, col = 1;
    for (int i = 0; i < g_text.ed.cursor && i < g_text.count; i++) {
        if (utext_at(&g_text, i) == '\n') { line++; col = 1; }
        else col++;
    }
    snprintf(g_lncol, sizeof g_lncol, "Ln %d, Col %d", line, col);
    strlcpy(g_modflag, g_dirty ? "MOD" : "--", sizeof g_modflag);
}

// --- item state -------------------------------------------------------
//
// Asked for by the menu bar, per item, every draw and every hit test. So
// there is no "refresh the menu" step anywhere in this app: greying Save
// out when the document is clean is this function and nothing else.
static unsigned menu_item_flags(int code) {
    switch (code) {
    case CMD_SAVE:
        return g_dirty ? 0 : UUI_MI_DISABLED;
    case CMD_DELETE:
        return utext_sel_present(&g_text) ? 0 : UUI_MI_DISABLED;
    case CMD_RECENT:
        return g_recent_count > 0 ? 0 : UUI_MI_DISABLED;
    case CMD_RECENT_0: return g_recent_count > 0 ? 0 : UUI_MI_DISABLED;
    case CMD_RECENT_1: return g_recent_count > 1 ? 0 : UUI_MI_DISABLED;
    case CMD_RECENT_2: return g_recent_count > 2 ? 0 : UUI_MI_DISABLED;
    case CMD_STATUSBAR:
        return g_show_status ? UUI_MI_CHECKED : 0;
    default:
        return 0;
    }
}

// --- drawing ----------------------------------------------------------

// Where the scrollbar strip is, given the text rect. One derivation,
// used by the draw and by the hit test below -- two copies of this is
// the classic way a scrollbar ends up drawing in one place and
// responding in another.
static void scrollbar_rect(int tx, int ty, int tw, int th,
                            int *bx, int *by, int *bw, int *bh) {
    *bx = tx + tw; // flush against the text field, which ends at the scrollbar's left edge
    *by = ty;
    *bw = scrollbar_w();
    *bh = th;
}

// Arrows are ON here (UUI_SCROLLBAR_ARROWS): an editor is where a
// stepper is actually wanted, and this is the flag's first caller.
#define NP_SCROLLBAR_FLAGS UUI_SCROLLBAR_ARROWS

static void draw_scrollbar(struct ugfx_surface *s, int tx, int ty, int tw, int th) {
    int total, visible;
    utext_metrics(&g_text, tw, th, &total, &visible);

    int bx, by, bw, bh;
    scrollbar_rect(tx, ty, tw, th, &bx, &by, &bw, &bh);
    uui_scrollbar_draw(s, bx, by, bw, bh, total, visible, g_text.scroll_offset,
                        ugfx_rgb(225, 225, 230), ugfx_rgb(150, 155, 165),
                        NP_SCROLLBAR_FLAGS);
}

// The document. The toolkit has already cleared the window and paints
// the declared widgets -- bars, dialog -- on top of this afterwards.
static void draw_document(struct ugfx_surface *s, int focused) {
    int tx, ty, tw, th;
    text_rect_for(s->w, s->h, &tx, &ty, &tw, &th);
    ugfx_draw_rect(s, tx - 1, ty - 1, tw + 2, th + 2, ugfx_rgb(200, 205, 215));
    utext_draw(&g_text, s, tx, ty, tw, th,
                // The caret shows only when this window has keyboard
                // focus AND no modal dialog or open menu is over the
                // text. An unfocused window drawing one claims to be
                // taking input that is going somewhere else -- see TWP's
                // WIN_EV_FOCUS.
                UTHEME_TEXT, UTHEME_WHITE, ugfx_rgb(205, 220, 240),
                focused && !uui_dialog_is_open(&g_dialog) && !uui_menubar_is_open(&g_menu));
    draw_scrollbar(s, tx, ty, tw, th);
}

// --- the dialog's behaviour -------------------------------------------

// The field's text, keeping whether it currently draws a caret:
// uui_textbox_init() resets `active`, and the focus is the dialog's.
static void dlg_name_set(const char *s) {
    uui_textbox_init(&g_dlg_name, s);
    uui_textbox_set_active(&g_dlg_name, g_dialog.focus == DLG_ITEM(ID_DLG_NAME));
}

static void dlg_on_dir_changed(void *ctx, const char *dir) {
    (void)ctx;
    snprintf(g_dlg_row, sizeof g_dlg_row, "Folder: %s", dir);
}

// A file ACTIVATED in the listing (Enter, double-click). Directories
// never reach here -- the fileview descends into those itself.
static void dlg_on_open(void *ctx, const char *path) {
    (void)ctx;
    if (g_dlg_saving) { dlg_name_set(k_path_basename(path)); return; }
    uui_dialog_close(&g_dialog);
    load_file(g_app, path);
}

// Save As: the selected file's name lands in the field, as in every
// desktop's chooser, so "save over that one" is a click and Return.
static void dlg_on_select(void *ctx, const char *path, int is_dir) {
    (void)ctx;
    if (g_dlg_saving && !is_dir) dlg_name_set(k_path_basename(path));
}

// (Re)opens the widget around the current flavour, directory and name.
static void dialog_show(void) {
    static const struct uui_dialog_button open_btns[] = {
        { "Open", CMD_DLG_OK }, { "Cancel", CMD_DLG_CANCEL },
    };
    static const struct uui_dialog_button save_btns[] = {
        { "Save", CMD_DLG_OK }, { "Cancel", CMD_DLG_CANCEL },
    };
    // Font-derived: the listing's natural height plus a few rows, and
    // the field's. The dialog clamps this to the window.
    int lw, lh, fw, fh;
    uui_fileview_natural_size(&g_dlg_list, &lw, &lh);
    lh += uui_table_row_h(&g_dlg_list.table) * 4;
    uui_textbox_natural_size(&g_dlg_name, &fw, &fh);
    int body_w = ugfx_char_w() * 44;
    int body_h = lh + fh + uui_layout_gap(&g_dlg_layout) + 2 * uui_layout_margin(&g_dlg_layout);

    uui_dialog_set_body(&g_dialog, &g_dlg_body, body_w, body_h);
    uui_dialog_open(&g_dialog, g_dlg_saving ? "Save As" : "Open", g_dlg_rows, 1,
                     g_dlg_saving ? save_btns : open_btns, 2, 0, CMD_DLG_CANCEL);
    uui_dialog_focus(&g_dialog, DLG_ITEM(g_dlg_saving ? ID_DLG_NAME : ID_DLG_LIST));
}

static void file_dialog(int saving) {
    g_dlg_saving = saving;
    uui_menubar_close(&g_menu); // a modal owns the input; the menu steps aside
    uui_dialog_focus(&g_dialog, NULL);

    // The document's directory, or the root -- and for Save As its name,
    // selected so that typing replaces it.
    char dir[PATH_MAX_LEN];
    if (!g_path[0] || !k_path_dirname(g_path, dir, sizeof dir)) strlcpy(dir, "/", sizeof dir);
    uui_fileview_set_dir(&g_dlg_list, dir);
    dlg_name_set(saving && g_path[0] ? k_path_basename(g_path) : "");
    dialog_show();
    if (saving && g_path[0]) uui_textbox_key(&g_dlg_name, 0x01); // Ctrl-A
}

// A button committed (or Return/Escape did). Typed name first, then
// the selection; OK on a folder enters it and keeps the dialog up, as
// every chooser does.
static void dialog_answer(struct uapp *a, int code) {
    if (code != CMD_DLG_OK) { set_status("cancelled"); return; }

    char full[PATH_MAX_LEN];
    const char *typed = uui_textbox_text(&g_dlg_name);
    const char *sel = uui_fileview_selected_name(&g_dlg_list);
    if (typed[0]) {
        // Normalised to an absolute path: a bare name is relative to
        // the listing, never to the kernel's cwd, so the title reads the
        // same after a save as after an open of the same file.
        int ok = typed[0] == '/' ? (int)strlcpy(full, typed, sizeof full) < (int)sizeof full
                                 : k_path_join(uui_fileview_dir(&g_dlg_list), typed, full, sizeof full);
        if (!ok) { set_status("path too long"); return; }
    } else if (uui_fileview_selected_is_dir(&g_dlg_list) || (sel && strcmp(sel, "..") == 0)) {
        uui_fileview_activate(&g_dlg_list);
        dialog_show();
        return;
    } else if (!uui_fileview_selected_path(&g_dlg_list, full, sizeof full)) {
        set_status("no file chosen");
        return;
    }
    if (g_dlg_saving) save_file(a, full);
    else load_file(a, full);
}

// --- commands ---------------------------------------------------------
//
// The ONE place a command happens. The menu bar routes here, and so do
// the Ctrl accelerators -- which is what stops "Ctrl-S" and "File >
// Save" from being two implementations of saving.

static void do_command(struct uapp *a, int code) {
    // An ACTION is an event a test waits for exactly once, so it is
    // never a layout line (docs/conventions/gui.md).
    ulogf("notepad: action %d\n", code);
    switch (code) {
    case CMD_NEW:
        utext_clear(&g_text);
        g_path[0] = '\0';
        g_dirty = 0;
        set_status("new file");
        break;
    case CMD_OPEN:
        file_dialog(0);
        break;
    case CMD_SAVE:
        if (g_path[0]) save_file(a, g_path);
        else file_dialog(1);
        break;
    case CMD_SAVE_AS:
        file_dialog(1);
        break;
    case CMD_EXIT:
        uapp_quit(a, 0);
        break;
    case CMD_RECENT_0:
    case CMD_RECENT_1:
    case CMD_RECENT_2: {
        int i = code - CMD_RECENT_0;
        if (i < g_recent_count) {
            // A copy: load_file() calls recent_push(), which rewrites the
            // very slot the path is being read out of.
            char p[PATH_MAX_LEN];
            strlcpy(p, g_recent[i], sizeof p);
            load_file(a, p);
        }
        break;
    }
    case CMD_SELECT_ALL:
        // Ctrl-A reaches the editor as a control code and is handled by
        // the shared keymap too; this is the MENU path to the same
        // thing, which is why it calls the same core rather than
        // re-implementing it out of cursor moves.
        utext_key(&g_text, 0x01, 0);
        set_status("selected all");
        break;
    case CMD_DELETE:
        if (utext_sel_present(&g_text)) { utext_sel_delete(&g_text); g_dirty = 1; }
        break;
    case CMD_GOTO_TOP:
        g_text.ed.cursor = 0;
        utext_sel_clear(&g_text);
        utext_scroll(&g_text, g_text.count); // clamps to the top
        break;
    case CMD_GOTO_END:
        g_text.ed.cursor = g_text.count;
        utext_sel_clear(&g_text);
        g_text.scroll_offset = 0; // 0 is pinned to the newest text
        break;
    case CMD_STATUSBAR:
        g_show_status = !g_show_status;
        break;
    default:
        break;
    }
}

// --- editor keys -------------------------------------------------------

// Returns 1 if this key was an accelerator, having run its command.
static int accelerator(struct uapp *a, int key) {
    switch (key) {
    case 0x0E: do_command(a, CMD_NEW);        return 1; // Ctrl-N
    case 0x0F: do_command(a, CMD_OPEN);       return 1; // Ctrl-O
    case 0x13: do_command(a, CMD_SAVE);       return 1; // Ctrl-S
    case 0x01: do_command(a, CMD_SELECT_ALL); return 1; // Ctrl-A
    default:   return 0;
    }
}

static void editor_key(int key) {
    // Paging is this app's, because it is about the VIEW rather than
    // the text -- the core moves a caret, and a page here scrolls
    // without moving one.
    if (key == KEY_PAGE_UP)   { utext_scroll(&g_text, 5);  return; }
    if (key == KEY_PAGE_DOWN) { utext_scroll(&g_text, -5); return; }

    // Everything else -- arrows, Shift+arrows, Home/End, Ctrl+A,
    // Backspace and Delete removing a selection, typing replacing one --
    // is the SHARED keymap (ui/uui_edit.h). This function used to be
    // sixty lines of exactly that, written here and nowhere else, so
    // the single-line text field got none of it.
    int before = g_text.count;
    if (utext_key(&g_text, key, 0)) {
        if (g_text.count != before) g_dirty = 1;
        return;
    }

    // Enter is deliberately declined by the core: a field commits, a
    // document inserts a newline. This is a document.
    if (key == '\n' || key == '\r') {
        utext_sel_delete(&g_text);
        utext_insert(&g_text, '\n');
        g_dirty = 1;
    }
}

// --- Toykit callbacks -------------------------------------------------
//
// The bars and the dialog are ROUTED (desc.widgets); the document is
// not a widget, so its clicks, drags and wheel are still handled here.

static int g_dragging;
static int g_scrollbar_drag;
static int g_scrollbar_grab; // how far down the thumb the drag started

// Set when the router named a widget for the press now being delivered
// -- on_widget runs before on_press for the same event -- so a click a
// menu swallowed to dismiss itself never also moves the caret.
static int g_press_taken;

// docs/gui-guidelines.md: a GUI test asks the app where things are. The
// widgets report themselves through the toolkit's walk; the two rects
// only this file knows are added first. `layout scrollbar` LEADS every
// frame -- tests cut the log to the current frame at that line.
static void log_layout(struct uapp *a) {
    int tx, ty, tw, th, x, y, w, h;
    text_rect_for(uapp_width(a), uapp_height(a), &tx, &ty, &tw, &th);
    scrollbar_rect(tx, ty, tw, th, &x, &y, &w, &h);
    uapp_logf_layout("notepad: layout scrollbar %d %d %d %d\n", x, y, w, h);
    uapp_logf_layout("notepad: layout text %d %d %d %d\n", tx, ty, tw, th);
    uapp_log_layout(a, "notepad");
}

static void on_draw(struct uapp *a, struct uapp_draw *d) {
    set_title(a);
    layout_chrome(d->surface->w, d->surface->h);
    if (g_show_status) update_indicators();
    draw_document(uapp_surface(d), uapp_focused(a));
    log_layout(a);
}

static void on_widget(struct uapp *a, int id, int reason) {
    if (reason == UUI_REASON_PRESS) g_press_taken = 1;
    switch (id) {
    case ID_MENU: {
        // The commit is PARKED in the widget (ui/uui_menubar.h).
        int code = uui_menubar_take_code(&g_menu);
        if (code > 0) do_command(a, code);
        break;
    }
    case ID_DIALOG: {
        int code = uui_dialog_take_code(&g_dialog);
        if (code > 0) dialog_answer(a, code);
        break;
    }
    case ID_DLG_LIST:
    case ID_DLG_NAME:
        // Keys follow the click; the router names the child, so the
        // dialog is told here (ui/uui_dialog.h).
        if (reason == UUI_REASON_PRESS) uui_dialog_focus(&g_dialog, DLG_ITEM(id));
        break;
    default:
        break;
    }
    uapp_redraw(a);
}

static void on_key(struct uapp *a, int key, unsigned mods) {
    (void)mods;
    // An open dialog never reaches here: the router hands it every key
    // first (ui/uui_route.h). An open menu does -- its ops table has
    // no key slot -- and when closed it takes only F10.
    int code = 0;
    if (uui_menubar_key(&g_menu, key, &code)) {
        if (code > 0) do_command(a, code);
        uapp_redraw(a);
        return;
    }

    if (accelerator(a, key)) {
        uapp_redraw(a);
        return;
    }

    // Esc is NOT a quit key. It closes a menu or cancels a dialog --
    // both handled above -- and otherwise does nothing. Closing the
    // window is Alt+F4, which never reaches here: the WM takes it and
    // asks through the same handshake the X button uses, so this app's
    // on_close (uapp's default, accept) still decides.
    editor_key(key);
    uapp_redraw(a);
}

// A click in the trough jumps there; a click on the thumb starts a
// drag. Returns 1 if the scrollbar took the click.
static int scrollbar_press(int px, int py, int tx, int ty, int tw, int th) {
    int bx, by, bw, bh;
    scrollbar_rect(tx, ty, tw, th, &bx, &by, &bw, &bh);

    int total, visible;
    utext_metrics(&g_text, tw, th, &total, &visible);

    // The widget classifies the click, so the arrows, the thumb and the
    // trough cannot disagree with what was drawn.
    enum uui_scrollbar_zone z =
        uui_scrollbar_hit(bx, by, bw, bh, total, visible, g_text.scroll_offset,
                           px, py, NP_SCROLLBAR_FLAGS);
    switch (z) {
    case UUI_SB_NONE:
        return 0;
    case UUI_SB_UP:
        utext_scroll(&g_text, 1);   // one line back
        return 1;
    case UUI_SB_DOWN:
        utext_scroll(&g_text, -1);  // one line forward
        return 1;
    case UUI_SB_ABOVE:
        utext_scroll(&g_text, visible);  // page
        return 1;
    case UUI_SB_BELOW:
        utext_scroll(&g_text, -visible);
        return 1;
    case UUI_SB_THUMB: {
        // WHERE on the thumb the grab happened. Without this the drag
        // maths below is told the cursor is at the thumb's TOP, so the
        // thumb leaps up by however far down it was actually grabbed --
        // which made the bar usable only by catching its top edge
        // exactly. ui/uui_scrollbar.h's `grab_offset_in_thumb` says so.
        int thumb_y, thumb_h;
        uui_scrollbar_thumb_rect(by, bh, total, visible, g_text.scroll_offset,
                                  &thumb_y, &thumb_h, bw, NP_SCROLLBAR_FLAGS);
        g_scrollbar_grab = py - thumb_y;
        g_scrollbar_drag = 1;
        return 1;
    }
    }
    return 1;
}

static void on_wheel(struct uapp *a, int notches) {
    // Only reached when no widget took the wheel -- an open dialog or
    // menu absorbs it (ui/uui_route.h). Three lines a notch, the same
    // step the kernel-space scrollback uses. utext_scroll clamps for us.
    utext_scroll(&g_text, notches * 3);
    uapp_redraw(a);
}

static void on_press(struct uapp *a, int x, int y, unsigned buttons) {
    (void)buttons;
    if (g_press_taken) { g_press_taken = 0; return; }
    if (uui_dialog_is_open(&g_dialog)) return; // a secondary press; the modal keeps it

    int tx, ty, tw, th;
    text_rect_for(uapp_width(a), uapp_height(a), &tx, &ty, &tw, &th);
    if (scrollbar_press(x, y, tx, ty, tw, th)) {
        // handled: jumped to the clicked position
    } else if (x >= tx && x < tx + tw && y >= ty && y < ty + th) {
        g_text.ed.cursor = utext_index_at_point(&g_text, tx, ty, tw, th, x, y);
        utext_sel_start(&g_text);
        g_dragging = 1;
    }
    uapp_redraw(a);
}

static void on_motion(struct uapp *a, int x, int y, unsigned buttons) {
    int tx, ty, tw, th;
    text_rect_for(uapp_width(a), uapp_height(a), &tx, &ty, &tw, &th);

    // The I-beam over the document and nothing else. Set on EVERY
    // motion, including back to the arrow -- it is a state (ui/uapp.h).
    // With the dialog up the widget tree's answer stands (its field
    // names the I-beam itself), so this says nothing then.
    if (!uui_dialog_is_open(&g_dialog)) {
        int over_text = x >= tx && x < tx + tw && y >= ty && y < ty + th &&
                        !uui_menubar_is_open(&g_menu);
        uapp_set_cursor(a, over_text ? WIN_CURSOR_TEXT : WIN_CURSOR_DEFAULT);
    }

    if (!buttons) return;
    if (g_scrollbar_drag) {
        int total, visible;
        utext_metrics(&g_text, tw, th, &total, &visible);
        if (total > visible) {
            int bx, by, bw, bh;
            scrollbar_rect(tx, ty, tw, th, &bx, &by, &bw, &bh);
            // The widget's own drag mapping -- so the thumb tracks
            // the cursor the same way it is drawn, arrows included.
            g_text.scroll_offset =
                uui_scrollbar_offset_for_drag(by, bh, total, visible, y,
                                               g_scrollbar_grab,
                                               bw, NP_SCROLLBAR_FLAGS);
            uapp_redraw(a);
        }
    } else if (g_dragging) {
        g_text.ed.cursor = utext_index_at_point(&g_text, tx, ty, tw, th, x, y);
        uapp_redraw(a);
    }
}

static void on_release(struct uapp *a, int x, int y, unsigned buttons) {
    (void)x; (void)y; (void)buttons;
    g_dragging = 0;
    g_scrollbar_drag = 0;
    uapp_redraw(a);
}

// A file named on the command line, opened once the window exists.
// Empty when there was no argument -- which is every launch until the
// File Manager started spawning this with a path (a .desktop `Handles=`
// entry claims .txt and friends, see data/wm/desktop/README.md).
static char g_arg_path[PATH_MAX_LEN];

static void on_open_cb(struct uapp *a) {
    g_app = a;
    utext_init(&g_text);
    g_path[0] = '\0';
    set_status("F10 for the menu -- Ctrl-O open, Ctrl-S save, Alt+F4 quit");

    for (int i = 0; i < RECENT_MAX; i++) strlcpy(g_recent[i], "(empty)", PATH_MAX_LEN);

    uui_menubar_init(&g_menu, menu_bar, (int)(sizeof menu_bar / sizeof menu_bar[0]));
    g_menu.item_flags = menu_item_flags;

    uui_statusbar_init(&g_statusbar);
    g_statusbar.count = 3;
    g_statusbar.panes[0].text = g_status;   g_statusbar.panes[0].chars = 0;
    g_statusbar.panes[1].text = g_lncol;    g_statusbar.panes[1].chars = 14;
    g_statusbar.panes[2].text = g_modflag;  g_statusbar.panes[2].chars = 4;
    update_indicators();

    uui_dialog_init(&g_dialog);
    uui_fileview_init(&g_dlg_list, 0, 0, 100, 100, g_dlg_entries, DIALOG_MAX_FILES);
    uui_fileview_set_mode(&g_dlg_list, UUI_FILEVIEW_LIST);
    g_dlg_list.on_open = dlg_on_open;
    g_dlg_list.on_select = dlg_on_select;
    g_dlg_list.on_dir_changed = dlg_on_dir_changed;
    uui_textbox_init(&g_dlg_name, "");
    g_dlg_layout.dir = UUI_COLUMN;
    g_dlg_layout.margin = 1;   // the dialog's own padding is the moat
    g_dlg_layout.items = g_dlg_items;
    g_dlg_layout.count = (int)(sizeof g_dlg_items / sizeof g_dlg_items[0]);

    // AFTER the widgets are set up, not before: load_file() writes the
    // status bar and the title, and both have to exist first.
    if (g_arg_path[0]) {
        load_file(a, g_arg_path);
        set_title(a);
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && argv[1][0]) strlcpy(g_arg_path, argv[1], sizeof g_arg_path);

    struct uapp_desc desc = {
        .title        = "untitled",
        .app_id       = "notepad",
        .w            = WIN_W,
        .h            = WIN_H,
        .x            = 180,
        .y            = 90,
        // Resizable: an editor is the app that most wants it, and it
        // needs no resize code -- text_rect_for() already derives the
        // text area from the content size, so a bigger window is a
        // bigger page. The minimum keeps the menu bar, one text row and
        // the status bar visible.
        .flags        = UAPP_RESIZABLE,
        .min_w        = 240,
        .min_h        = 120,
        .widgets      = g_widgets,
        .widget_count = (int)(sizeof g_widgets / sizeof g_widgets[0]),
        .on_open      = on_open_cb,
        .on_draw      = on_draw,
        .on_widget    = on_widget,
        .on_key       = on_key,
        .on_press     = on_press,
        .on_motion    = on_motion,
        .on_release   = on_release,
        .on_wheel     = on_wheel,
    };
    return uapp_run(&desc);
}
